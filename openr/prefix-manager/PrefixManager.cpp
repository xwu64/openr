/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"

#include <fb303/ServiceData.h>
#include <folly/futures/Future.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <optional>
#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/kvstore/KvStore.h>

namespace fb303 = facebook::fb303;

namespace openr {

namespace detail {

void
PrefixManagerPendingUpdates::reset() {
  changedPrefixes_.clear();
}

void
PrefixManagerPendingUpdates::applyPrefixChange(
    const folly::small_vector<folly::CIDRNetwork>& change) {
  for (const auto& network : change) {
    changedPrefixes_.insert(network);
  }
}

} // namespace detail

PrefixManager::PrefixManager(
    messaging::ReplicateQueue<DecisionRouteUpdate>& staticRouteUpdatesQueue,
    messaging::RQueue<PrefixEvent> prefixUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> decisionRouteUpdatesQueue,
    std::shared_ptr<const Config> config,
    KvStore* kvStore,
    const std::chrono::seconds& initialDumpTime)
    : nodeId_(config->getNodeName()),
      ttlKeyInKvStore_(std::chrono::milliseconds(
          *config->getKvStoreConfig().key_ttl_ms_ref())),
      staticRouteUpdatesQueue_(staticRouteUpdatesQueue),
      v4OverV6Nexthop_(config->isV4OverV6NexthopEnabled()),
      kvStore_(kvStore),
      preferOpenrOriginatedRoutes_(
          config->getConfig().get_prefer_openr_originated_routes()),
      enableNewPrefixFormat_(
          config->getConfig().get_enable_new_prefix_format()) {
  CHECK(kvStore_);
  CHECK(config);

  if (auto policyConf = config->getAreaPolicies()) {
    policyManager_ = std::make_unique<PolicyManager>(*policyConf);
  }

  for (const auto& [areaId, areaConf] : config->getAreas()) {
    areaToPolicy_.emplace(areaId, areaConf.getImportPolicyName());
  }

  // Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
      this, nodeId_, kvStore_, true /* useThrottle */);

  // Create initial timer to update all prefixes after HoldTime (2 * KA)
  initialSyncKvStoreTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { syncKvStore(); });

  // Create throttled update state
  syncKvStoreThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kKvStoreSyncThrottleTimeout, [this]() noexcept {
        // No write to KvStore before initial KvStore sync
        if (initialSyncKvStoreTimer_->isScheduled()) {
          return;
        }
        syncKvStore();
      });

  // Schedule fiber to read prefix updates messages
  addFiberTask([q = std::move(prefixUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeUpdate = q.get(); // perform read
      if (maybeUpdate.hasError()) {
        VLOG(1) << "Terminating prefix update request processing fiber";
        break;
      }
      auto& update = maybeUpdate.value();

      // if no specified dstination areas, apply to all areas
      std::unordered_set<std::string> dstAreas;
      if (update.dstAreas.empty()) {
        dstAreas = allAreaIds();
      } else {
        for (const auto& area : update.dstAreas) {
          dstAreas.emplace(area);
        }
      }

      switch (update.eventType) {
      case PrefixEventType::ADD_PREFIXES:
        advertisePrefixesImpl(std::move(update.prefixes), dstAreas);
        advertisePrefixesImpl(std::move(update.prefixEntries), dstAreas);
        break;
      case PrefixEventType::WITHDRAW_PREFIXES:
        withdrawPrefixesImpl(update.prefixes);
        withdrawPrefixEntriesImpl(update.prefixEntries);
        break;
      case PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE:
        CHECK(update.type.has_value());
        withdrawPrefixesByTypeImpl(update.type.value());
        break;
      case PrefixEventType::SYNC_PREFIXES_BY_TYPE:
        CHECK(update.type.has_value());
        syncPrefixesByTypeImpl(update.type.value(), update.prefixes, dstAreas);
        break;
      default:
        LOG(ERROR) << "Unknown command received. "
                   << static_cast<int>(update.eventType);
      }
    }
  });

  // Fiber to process route updates from Decision
  addFiberTask([q = std::move(decisionRouteUpdatesQueue),
                this]() mutable noexcept {
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      if (maybeThriftObj.hasError()) {
        VLOG(1) << "Terminating route delta processing fiber";
        break;
      }

      try {
        VLOG(2) << "Received RIB updates from Decision";
        processDecisionRouteUpdates(std::move(maybeThriftObj).value());
      } catch (const std::exception&) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          LOG(ERROR) << exInfo;
        }
#endif
        throw;
      }
    }
  });

  // register kvstore publication callback
  // ATTN: in case of receiving update from `KvStore` for keys we didn't
  // persist, subscribe update to delete this key.
  const auto keyPrefix =
      fmt::format("{}{}:", Constants::kPrefixDbMarker.toString(), nodeId_);
  kvStoreClient_->subscribeKeyFilter(
      // TODO: by default keyMatch option is OR. Change to leverage originatorId
      // for subscription instead of checking nodeId internally
      KvStoreFilters({keyPrefix}, {} /* originatorIds */),
      [this](
          const std::string& prefixStr,
          std::optional<thrift::Value> val) noexcept {
        // Ignore update if:
        //  1) val is std::nullopt;
        //  2) val has no value field inside `thrift::Value`(e.g. ttl update)
        if ((not val.has_value()) or (not val.value().value_ref())) {
          return;
        }

        // TODO: avoid decoding keys
        folly::CIDRNetwork network;
        if (enableNewPrefixFormat_) {
          if (not PrefixKey::isPrefixKeyV2Str(prefixStr)) {
            // ATTN: local prefixMgr will received previously advertised keys
            // with old prefix key format. Ignore them.
            LOG(INFO) << "Skip processing old format of prefix key: "
                      << prefixStr;
            return;
          }
          auto prefixKeyV2 = PrefixKey::fromStrV2(prefixStr);
          network = prefixKeyV2->getCIDRNetwork();
        } else {
          auto prefixKey = PrefixKey::fromStr(prefixStr);
          network = prefixKey->getCIDRNetwork();
        }

        const auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
            *val.value().value_ref(), serializer_);
        if ((not *prefixDb.deletePrefix_ref()) and
            nodeId_ == *prefixDb.thisNodeName_ref()) {
          VLOG(2) << "Learning previously announced prefix: " << prefixStr;

          // populate advertisedKeys_ collection to make sure we can find
          // key when clear key from `KvStore`
          advertisedKeys_[network].keys.emplace(prefixStr);

          // Populate pendingState to check keys
          folly::small_vector<folly::CIDRNetwork> changed{network};
          pendingUpdates_.applyPrefixChange(changed);
          syncKvStoreThrottled_->operator()();
        }
      });

  // get initial dump of keys related to `myNodeId_`.
  // ATTN: when Open/R restarts, newly started prefixManager will need to
  // understand what it has previously advertised.
  for (const auto& [area, _] : areaToPolicy_) {
    auto result = kvStoreClient_->dumpAllWithPrefix(AreaId{area}, keyPrefix);
    if (not result.has_value()) {
      LOG(ERROR) << "Failed dumping prefix " << keyPrefix << " from area "
                 << area;
      continue;
    }

    folly::small_vector<folly::CIDRNetwork> changed;
    for (auto const& [prefixStr, _] : result.value()) {
      // TODO: avoid decoding keys
      folly::CIDRNetwork network;
      if (enableNewPrefixFormat_) {
        if (not PrefixKey::isPrefixKeyV2Str(prefixStr)) {
          // ATTN: local prefixMgr will received previously advertised keys
          // with old prefix key format. Ignore them.
          continue;
        }
        auto prefixKeyV2 = PrefixKey::fromStrV2(prefixStr);
        network = prefixKeyV2->getCIDRNetwork();
      } else {
        auto prefixKey = PrefixKey::fromStr(prefixStr);
        network = prefixKey->getCIDRNetwork();
      }

      // populate advertisedKeys_ collection to make sure we can find
      // key when clear key from `KvStore`
      advertisedKeys_[network].keys.emplace(prefixStr);

      // Populate pendingState to check keys
      changed.emplace_back(network);
    }

    // populate pending update in one shot
    pendingUpdates_.applyPrefixChange(changed);
    syncKvStoreThrottled_->operator()();
  }

  // schedule one-time initial dump
  initialSyncKvStoreTimer_->scheduleTimeout(initialDumpTime);

  // Load openrConfig for local-originated routes
  if (auto prefixes = config->getConfig().originated_prefixes_ref()) {
    // read originated prefixes from OpenrConfig
    buildOriginatedPrefixDb(*prefixes);

    // ATTN: consider min_supporting_route = 0, immediately advertise
    // originated routes to `KvStore`
    processOriginatedPrefixes();
  }
}

PrefixManager::~PrefixManager() {
  // - If EventBase is stopped or it is within the evb thread, run immediately;
  // - Otherwise, will wait the EventBase to run;
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    initialSyncKvStoreTimer_.reset();
    syncKvStoreThrottled_.reset();
  });
  kvStoreClient_.reset();
}

void
PrefixManager::stop() {
  // Stop KvStoreClient first
  kvStoreClient_->stop();
  VLOG(1) << "KvStoreClient successfully stopped.";

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

thrift::PrefixEntry
PrefixManager::toPrefixEntryThrift(
    const thrift::OriginatedPrefix& prefix, const thrift::PrefixType& tType) {
  // Populate PrefixMetric struct
  thrift::PrefixMetrics metrics;
  if (auto pref = prefix.path_preference_ref()) {
    metrics.path_preference_ref() = *pref;
  }
  if (auto pref = prefix.source_preference_ref()) {
    metrics.source_preference_ref() = *pref;
  }

  // Populate PrefixEntry struct
  thrift::PrefixEntry entry;
  entry.prefix_ref() =
      toIpPrefix(folly::IPAddress::createNetwork(*prefix.prefix_ref()));
  entry.metrics_ref() = std::move(metrics);
  // ATTN: local-originated prefix has unique type CONFIG
  //      to be differentiated from others.
  entry.type_ref() = tType;
  // ATTN: `area_stack` will be explicitly set to empty
  //      as there is no "cross-area" behavior for local
  //      originated prefixes.
  CHECK(entry.area_stack_ref()->empty());
  if (auto tags = prefix.tags_ref()) {
    entry.tags_ref() = *tags;
  }
  return entry;
}

void
PrefixManager::buildOriginatedPrefixDb(
    const std::vector<thrift::OriginatedPrefix>& prefixes) {
  for (const auto& prefix : prefixes) {
    auto network = folly::IPAddress::createNetwork(*prefix.prefix_ref());
    auto nh = network.first.isV4() and not v4OverV6Nexthop_
        ? Constants::kLocalRouteNexthopV4.toString()
        : Constants::kLocalRouteNexthopV6.toString();

    auto entry = toPrefixEntryThrift(prefix, thrift::PrefixType::CONFIG);

    // Populate RibUnicastEntry struct
    // ATTN: AREA field is empty for NHs
    RibUnicastEntry unicastEntry(network, {createNextHop(toBinaryAddress(nh))});
    unicastEntry.bestPrefixEntry = std::move(entry);

    // ATTN: upon initialization, no supporting routes
    originatedPrefixDb_.emplace(
        network,
        OriginatedRoute(
            prefix,
            std::move(unicastEntry),
            std::unordered_set<folly::CIDRNetwork>{}));
  }
}

std::unordered_set<std::string>
PrefixManager::updateKvStoreKeyHelper(const PrefixEntry& entry) {
  std::unordered_set<std::string> prefixKeys;
  const auto& tPrefixEntry = entry.tPrefixEntry;
  const auto& type = *tPrefixEntry->type_ref();
  const std::unordered_set<std::string> areaStack{
      tPrefixEntry->area_stack_ref()->begin(),
      tPrefixEntry->area_stack_ref()->end()};

  for (const auto& toArea : entry.dstAreas) {
    // prevent area_stack loop
    // ATTN: for local-originated prefixes, `area_stack` is explicitly
    //       set to empty.
    if (areaStack.count(toArea)) {
      continue;
    }

    // run ingress policy
    std::shared_ptr<thrift::PrefixEntry> postPolicyTPrefixEntry;
    std::string hitPolicyName;

    const auto& policy = areaToPolicy_.at(toArea);
    if (policy) {
      std::tie(postPolicyTPrefixEntry, hitPolicyName) =
          policyManager_->applyPolicy(*policy, tPrefixEntry);

      // policy reject prefix, nothing to do.
      if (not postPolicyTPrefixEntry) {
        VLOG(2) << "[Area Policy] " << *policy << " rejected prefix: "
                << "(Type, PrefixEntry): (" << toString(type) << ", "
                << toString(*tPrefixEntry, true) << "), hit term ("
                << hitPolicyName << ")";
        continue;
      }

      // policy accept prefix, go ahread with prefix announcement.
      VLOG(2) << "[Area Policy] " << *policy << " accepted/modified prefix: "
              << "(Type, PrefixEntry): (" << toString(type) << ", "
              << toString(*tPrefixEntry, true) << "), PostPolicyEntry: ("
              << toString(*postPolicyTPrefixEntry) << "), hit term ("
              << hitPolicyName << ")";
    } else {
      postPolicyTPrefixEntry = tPrefixEntry;
    }

    const auto prefixKey = PrefixKey(nodeId_, entry.network, toArea);
    const auto prefixKeyStr = enableNewPrefixFormat_
        ? prefixKey.getPrefixKeyV2()
        : prefixKey.getPrefixKey();
    auto prefixDb = createPrefixDb(nodeId_, {*postPolicyTPrefixEntry}, toArea);
    auto prefixDbStr = writeThriftObjStr(std::move(prefixDb), serializer_);

    // advertise key to `KvStore`
    bool changed = kvStoreClient_->persistKey(
        AreaId{toArea}, prefixKeyStr, prefixDbStr, ttlKeyInKvStore_);
    fb303::fbData->addStatValue(
        "prefix_manager.route_advertisements", 1, fb303::SUM);
    VLOG_IF(1, changed) << "[Prefix Advertisement] "
                        << "Area: " << toArea << ", "
                        << "Type: " << toString(type) << ", "
                        << toString(*postPolicyTPrefixEntry, VLOG_IS_ON(2));
    prefixKeys.emplace(prefixKeyStr);
  }
  return prefixKeys;
}

void
PrefixManager::deleteKvStoreKeyHelper(
    const std::unordered_set<std::string>& deletedKeys) {
  // Prepare thrift::PrefixDatabase object for deletion
  thrift::PrefixDatabase deletedPrefixDb;
  deletedPrefixDb.thisNodeName_ref() = nodeId_;
  deletedPrefixDb.deletePrefix_ref() = true;

  // TODO: see if we can avoid encoding/decoding of string
  for (const auto& prefixStr : deletedKeys) {
    auto prefixKey = PrefixKey::fromStr(prefixStr);
    CHECK(prefixKey.hasValue());

    thrift::PrefixEntry entry;
    entry.prefix_ref() = toIpPrefix(prefixKey->getCIDRNetwork());
    deletedPrefixDb.prefixEntries_ref() = {entry};
    VLOG(1) << "[Prefix Withdraw] "
            << "Area: " << prefixKey->getPrefixArea() << ", "
            << toString(*entry.prefix_ref());
    fb303::fbData->addStatValue(
        "prefix_manager.route_withdraws", 1, fb303::SUM);

    kvStoreClient_->clearKey(
        AreaId{prefixKey->getPrefixArea()},
        prefixStr,
        writeThriftObjStr(std::move(deletedPrefixDb), serializer_),
        ttlKeyInKvStore_);
  }
}

void
PrefixManager::syncKvStore() {
  VLOG(1)
      << "[KvStore Sync] Syncing "
      << pendingUpdates_.getChangedPrefixes().size()
      << " changed prefixes. Total prefixes advertised: " << prefixMap_.size();
  DecisionRouteUpdate routeUpdatesOut;
  // iterate over `pendingUpdates_` to advertise/withdraw incremental changes
  for (auto const& network : pendingUpdates_.getChangedPrefixes()) {
    auto it = prefixMap_.find(network);
    if (it == prefixMap_.end()) {
      // delete actual keys being advertised in the cache
      //
      // Sample format:
      //  prefix    :    node1    :    0    :    0.0.0.0/32
      //    |              |           |             |
      //  marker        nodeId      areaId        prefixStr
      auto keysIt = advertisedKeys_.find(network);
      if (keysIt != advertisedKeys_.end()) {
        deleteKvStoreKeyHelper(keysIt->second.keys);
        if (keysIt->second.installedToFib) {
          routeUpdatesOut.unicastRoutesToDelete.emplace_back(network);
        }
        advertisedKeys_.erase(keysIt);
      }
    } else {
      // add/update keys in `KvStore`
      const auto& typeToPrefixes = it->second;

      // select the best entry/entries by comparing metric_ref() field
      const auto bestTypes = selectBestPrefixMetrics(typeToPrefixes);
      auto bestType = *bestTypes.begin();
      // if best route is BGP, and an equivalent CONFIG route exists,
      // then prefer config route if knob prefer_openr_originated_config_=true
      if (bestType == thrift::PrefixType::BGP and
          preferOpenrOriginatedRoutes_ and
          bestTypes.count(thrift::PrefixType::CONFIG)) {
        bestType = thrift::PrefixType::CONFIG;
      }
      auto& bestEntry = typeToPrefixes.at(bestType);

      // advertise best-entry for this prefix to `KvStore`
      auto newKeys = updateKvStoreKeyHelper(bestEntry);

      auto keysIt = advertisedKeys_.find(network);
      if (keysIt != advertisedKeys_.end()) {
        // ATTN: This collection holds "advertised" prefixes in previous
        // round of syncing. By removing prefixes in current run,
        // whatever left in `advertisedKeys_` will be the delta to
        // be removed.
        for (const auto& key : newKeys) {
          keysIt->second.keys.erase(key);
        }

        // remove keys which are no longer advertised
        // e.g.
        // t0: prefix_1 => {area_1, area_2}
        // t1: prefix_1 => {area_1, area_3}
        //     (prefix_1, area_2) will be removed
        deleteKvStoreKeyHelper(keysIt->second.keys);
      }

      // override `advertisedKeys_` for next-round syncing
      advertisedKeys_[network].keys = std::move(newKeys);
      // propogate route update to `KvStore` and `Decision`(if necessary)
      if (bestEntry.shouldInstall()) {
        advertisedKeys_[network].installedToFib = true;
        // Populate RibUnicastEntry struct
        // ATTN: AREA field is empty for NHs
        // if shouldInstall() is true, nexthops is guaranteed to have value.
        RibUnicastEntry unicastEntry(network, bestEntry.nexthops.value());
        unicastEntry.bestPrefixEntry = *bestEntry.tPrefixEntry;
        routeUpdatesOut.addRouteToUpdate(std::move(unicastEntry));
      } else {
        // if was installed to fib, but now lose in tie break, withdraw from
        // fib.
        if (advertisedKeys_[network].installedToFib) {
          routeUpdatesOut.unicastRoutesToDelete.emplace_back(network);
          advertisedKeys_[network].installedToFib = false;
        }
      }
    }
  }
  // push originatedRoutes update via replicate queue
  if (routeUpdatesOut.unicastRoutesToUpdate.size() or
      routeUpdatesOut.unicastRoutesToDelete.size()) {
    CHECK(routeUpdatesOut.mplsRoutesToUpdate.empty());
    CHECK(routeUpdatesOut.mplsRoutesToDelete.empty());
    staticRouteUpdatesQueue_.push(std::move(routeUpdatesOut));
  }

  VLOG(1) << "[KvStore Sync] Done syncing: "
          << pendingUpdates_.getChangedPrefixes().size()
          << " changed prefixes.";

  // clean up
  pendingUpdates_.reset();

  // Update flat counters
  size_t num_prefixes = 0;
  for (auto const& [_, typeToPrefixes] : prefixMap_) {
    num_prefixes += typeToPrefixes.size();
  }
  fb303::fbData->setCounter("prefix_manager.received_prefixes", num_prefixes);
  fb303::fbData->setCounter(
      "prefix_manager.advertised_prefixes", prefixMap_.size());
}

folly::SemiFuture<bool>
PrefixManager::advertisePrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixes = std::move(prefixes)]() mutable noexcept {
    auto dstAreas = allAreaIds();
    p.setValue(advertisePrefixesImpl(std::move(prefixes), dstAreas));
  });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::withdrawPrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixes = std::move(prefixes)]() mutable noexcept {
    p.setValue(withdrawPrefixesImpl(prefixes));
  });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::withdrawPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixType = std::move(prefixType)]() mutable noexcept {
    p.setValue(withdrawPrefixesByTypeImpl(prefixType));
  });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::syncPrefixesByType(
    thrift::PrefixType prefixType, std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixType = std::move(prefixType),
                        prefixes = std::move(prefixes)]() mutable noexcept {
    auto dstAreas = allAreaIds();
    p.setValue(syncPrefixesByTypeImpl(prefixType, prefixes, dstAreas));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
PrefixManager::getPrefixes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable noexcept {
    std::vector<thrift::PrefixEntry> prefixes;
    for (const auto& [_, typeToInfo] : prefixMap_) {
      for (const auto& [_, entry] : typeToInfo) {
        prefixes.emplace_back(*entry.tPrefixEntry);
      }
    }
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(prefixes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
PrefixManager::getPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixType = std::move(prefixType)]() mutable noexcept {
    std::vector<thrift::PrefixEntry> prefixes;
    for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
      auto it = typeToPrefixes.find(prefixType);
      if (it != typeToPrefixes.end()) {
        prefixes.emplace_back(*it->second.tPrefixEntry);
      }
    }
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(prefixes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
PrefixManager::getAdvertisedRoutesFiltered(
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable noexcept {
        auto routes =
            std::make_unique<std::vector<thrift::AdvertisedRouteDetail>>();
        if (filter.prefixes_ref()) {
          // Explicitly lookup the requested prefixes
          for (auto& prefix : filter.prefixes_ref().value()) {
            auto it = prefixMap_.find(toIPNetwork(prefix));
            if (it == prefixMap_.end()) {
              continue;
            }
            filterAndAddAdvertisedRoute(
                *routes, filter.prefixType_ref(), it->first, it->second);
          }
        } else {
          // Iterate over all prefixes
          for (auto& [prefix, prefixEntries] : prefixMap_) {
            filterAndAddAdvertisedRoute(
                *routes, filter.prefixType_ref(), prefix, prefixEntries);
          }
        }
        p.setValue(std::move(routes));
      });
  return std::move(sf);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>
PrefixManager::getAreaAdvertisedRoutes(
    std::string areaName,
    thrift::RouteFilterType routeFilterType,
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>();
  runInEventBaseThread([this,
                        p = std::move(p),
                        filter = std::move(filter),
                        areaName = std::move(areaName),
                        routeFilterType = routeFilterType]() mutable noexcept {
    auto routes = std::make_unique<std::vector<thrift::AdvertisedRoute>>();
    if (filter.prefixes_ref()) {
      // Explicitly lookup the requested prefixes
      for (auto& prefix : filter.prefixes_ref().value()) {
        auto it = prefixMap_.find(toIPNetwork(prefix));
        if (it == prefixMap_.end()) {
          continue;
        }
        filterAndAddAreaRoute(
            *routes,
            areaName,
            routeFilterType,
            it->second,
            filter.prefixType_ref());
      }
    } else {
      for (auto& [prefix, prefixEntries] : prefixMap_) {
        filterAndAddAreaRoute(
            *routes,
            areaName,
            routeFilterType,
            prefixEntries,
            filter.prefixType_ref());
      }
    }
    p.setValue(std::move(routes));
  });
  return std::move(sf);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::OriginatedPrefixEntry>>>
PrefixManager::getOriginatedPrefixes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::OriginatedPrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable noexcept {
    // convert content inside originatedPrefixDb_ into thrift struct
    auto prefixes =
        std::make_unique<std::vector<thrift::OriginatedPrefixEntry>>();
    for (auto const& [_, route] : originatedPrefixDb_) {
      auto const& prefix = route.originatedPrefix;
      auto supportingRoutes =
          folly::gen::from(route.supportingRoutes) |
          folly::gen::mapped([](const folly::CIDRNetwork& network) {
            return folly::IPAddress::networkToString(network);
          }) |
          folly::gen::as<std::vector<std::string>>();

      auto entry = createOriginatedPrefixEntry(
          prefix,
          supportingRoutes,
          supportingRoutes.size() >= *prefix.minimum_supporting_routes_ref());
      prefixes->emplace_back(std::move(entry));
    }
    p.setValue(std::move(prefixes));
  });
  return sf;
}

void
PrefixManager::filterAndAddAdvertisedRoute(
    std::vector<thrift::AdvertisedRouteDetail>& routes,
    apache::thrift::optional_field_ref<thrift::PrefixType&> const& typeFilter,
    folly::CIDRNetwork const& prefix,
    std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::AdvertisedRouteDetail routeDetail;
  routeDetail.prefix_ref() = toIpPrefix(prefix);

  // Add best route selection data
  for (auto& prefixType : selectBestPrefixMetrics(prefixEntries)) {
    routeDetail.bestKeys_ref()->emplace_back(prefixType);
  }
  routeDetail.bestKey_ref() = routeDetail.bestKeys_ref()->at(0);

  // Add prefix entries and honor the filter
  for (auto& [prefixType, prefixEntry] : prefixEntries) {
    if (typeFilter && *typeFilter != prefixType) {
      continue;
    }
    routeDetail.routes_ref()->emplace_back();
    auto& route = routeDetail.routes_ref()->back();
    route.key_ref() = prefixType;
    route.route_ref() = *prefixEntry.tPrefixEntry;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes_ref()->size()) {
    routes.emplace_back(std::move(routeDetail));
  }
}

void
PrefixManager::filterAndAddAreaRoute(
    std::vector<thrift::AdvertisedRoute>& routes,
    const std::string& area,
    const thrift::RouteFilterType& routeFilterType,
    std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries,
    apache::thrift::optional_field_ref<thrift::PrefixType&> const& typeFilter) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  auto bestPrefixType = *selectBestPrefixMetrics(prefixEntries).begin();
  const auto& bestPrefixEntry = prefixEntries.at(bestPrefixType);
  // The prefix will not be advertised to user provided area
  if (not bestPrefixEntry.dstAreas.count(area)) {
    return;
  }
  // return if type does not match
  if (typeFilter && *typeFilter != bestPrefixType) {
    return;
  }

  const auto& prePolicyTPrefixEntry = bestPrefixEntry.tPrefixEntry;

  // prefilter advertised route
  if (routeFilterType == thrift::RouteFilterType::PREFILTER_ADVERTISED) {
    thrift::AdvertisedRoute route;
    route.set_key(bestPrefixType);
    route.set_route(*prePolicyTPrefixEntry);
    routes.emplace_back(std::move(route));
    return;
  }

  // run policy
  std::shared_ptr<thrift::PrefixEntry> postPolicyTPrefixEntry;
  std::string hitPolicyName{};

  const auto& policy = areaToPolicy_.at(area);
  if (policy) {
    std::tie(postPolicyTPrefixEntry, hitPolicyName) =
        policyManager_->applyPolicy(*policy, prePolicyTPrefixEntry);
  } else {
    postPolicyTPrefixEntry = prePolicyTPrefixEntry;
  }

  if (routeFilterType == thrift::RouteFilterType::POSTFILTER_ADVERTISED and
      postPolicyTPrefixEntry) {
    // add post filter advertised route
    thrift::AdvertisedRoute route;
    route.set_key(bestPrefixType);
    route.set_route(*postPolicyTPrefixEntry);
    if (not hitPolicyName.empty()) {
      route.set_hitPolicy(hitPolicyName);
    }
    routes.emplace_back(std::move(route));
    return;
  }

  if (routeFilterType == thrift::RouteFilterType::REJECTED_ON_ADVERTISE and
      not postPolicyTPrefixEntry) {
    // add post filter rejected route
    thrift::AdvertisedRoute route;
    route.set_key(bestPrefixType);
    route.set_route(*prePolicyTPrefixEntry);
    route.set_hitPolicy(hitPolicyName);
    routes.emplace_back(std::move(route));
  }
}

bool
PrefixManager::advertisePrefixesImpl(
    std::vector<thrift::PrefixEntry>&& tPrefixEntries,
    const std::unordered_set<std::string>& dstAreas) {
  if (tPrefixEntries.empty()) {
    return false;
  }
  std::vector<PrefixEntry> toAddOrUpdate;
  for (auto& tPrefixEntry : tPrefixEntries) {
    auto dstAreasCp = dstAreas;
    toAddOrUpdate.emplace_back(
        std::make_shared<thrift::PrefixEntry>(std::move(tPrefixEntry)),
        std::move(dstAreasCp));
  }
  return advertisePrefixesImpl(toAddOrUpdate);
}

bool
PrefixManager::advertisePrefixesImpl(
    std::vector<PrefixEntry>&& prefixEntries,
    const std::unordered_set<std::string>& dstAreas) {
  if (prefixEntries.empty()) {
    return false;
  }

  std::vector<PrefixEntry> toAddOrUpdate;
  for (auto& prefixEntry : prefixEntries) {
    auto dstAreasCp = dstAreas;
    prefixEntry.dstAreas = std::move(dstAreasCp);

    // Create PrefixEntry and set unicastRotues
    toAddOrUpdate.push_back(std::move(prefixEntry));
  }
  return advertisePrefixesImpl(toAddOrUpdate);
}

bool
PrefixManager::advertisePrefixesImpl(
    const std::vector<PrefixEntry>& prefixEntries) {
  folly::small_vector<folly::CIDRNetwork> changed{};

  for (const auto& entry : prefixEntries) {
    const auto& type = *entry.tPrefixEntry->type_ref();
    const auto& prefixCidr = entry.network;

    // ATTN: create new folly::CIDRNetwork -> typeToPrefixes
    //       mapping if it is new prefix. `[]` operator is
    //       used intentionally.
    auto [it, inserted] = prefixMap_[prefixCidr].emplace(type, entry);

    if (not inserted) {
      if (it->second == entry) {
        // Case 1: ignore SAME `PrefixEntry`
        continue;
      }
      // Case 2: update existing `PrefixEntry`
      it->second = entry;
    }
    // Case 3: create new `PrefixEntry`
    changed.emplace_back(prefixCidr);
  }

  bool updated = (not changed.empty()) ? true : false;
  if (updated) {
    // store pendingUpdate for batch processing
    pendingUpdates_.applyPrefixChange(changed);

    // schedule `syncKvStore` after throttled timeout
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::withdrawPrefixesImpl(
    const std::vector<thrift::PrefixEntry>& tPrefixEntries) {
  if (tPrefixEntries.empty()) {
    return false;
  }
  folly::small_vector<folly::CIDRNetwork> changed{};

  for (const auto& prefixEntry : tPrefixEntries) {
    const auto& type = *prefixEntry.type_ref();
    const auto& prefixCidr = toIPNetwork(*prefixEntry.prefix_ref());

    // iterator usage to avoid multiple times of map access
    auto typeIt = prefixMap_.find(prefixCidr);

    // ONLY populate changed collection when successfully erased key
    if (typeIt != prefixMap_.end() and typeIt->second.erase(type)) {
      changed.emplace_back(prefixCidr);

      // clean up data structure
      if (typeIt->second.empty()) {
        prefixMap_.erase(prefixCidr);
      }
    }
  }

  bool updated = (not changed.empty()) ? true : false;
  if (updated) {
    // store pendingUpdate for batch processing
    pendingUpdates_.applyPrefixChange(changed);

    // schedule `syncKvStore` after throttled timeout
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::withdrawPrefixEntriesImpl(
    const std::vector<PrefixEntry>& prefixEntries) {
  if (prefixEntries.empty()) {
    return false;
  }
  folly::small_vector<folly::CIDRNetwork> changed{};

  for (const auto& prefixEntry : prefixEntries) {
    const auto& type = *prefixEntry.tPrefixEntry->type_ref();

    // iterator usage to avoid multiple times of map access
    auto typeIt = prefixMap_.find(prefixEntry.network);

    // ONLY populate changed collection when successfully erased key
    if (typeIt != prefixMap_.end() and typeIt->second.erase(type)) {
      changed.emplace_back(prefixEntry.network);
      // clean up data structure
      if (typeIt->second.empty()) {
        prefixMap_.erase(prefixEntry.network);
      }
    }
  }

  bool updated = (not changed.empty()) ? true : false;
  if (updated) {
    // store pendingUpdate for batch processing
    pendingUpdates_.applyPrefixChange(changed);

    // schedule `syncKvStore` after throttled timeout
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::syncPrefixesByTypeImpl(
    thrift::PrefixType type,
    const std::vector<thrift::PrefixEntry>& tPrefixEntries,
    const std::unordered_set<std::string>& dstAreas) {
  VLOG(1) << "Syncing prefixes of type " << toString(type);
  // building these lists so we can call add and remove and get detailed
  // logging
  std::vector<thrift::PrefixEntry> toAddOrUpdate, toRemove;
  std::unordered_set<folly::CIDRNetwork> toRemoveSet;
  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    if (typeToPrefixes.count(type)) {
      toRemoveSet.emplace(prefix);
    }
  }
  for (auto const& entry : tPrefixEntries) {
    CHECK(type == *entry.type_ref());
    toRemoveSet.erase(toIPNetwork(*entry.prefix_ref()));
    toAddOrUpdate.emplace_back(entry);
  }
  for (auto const& prefix : toRemoveSet) {
    toRemove.emplace_back(*prefixMap_.at(prefix).at(type).tPrefixEntry);
  }
  bool updated = false;
  updated |=
      ((advertisePrefixesImpl(std::move(toAddOrUpdate), dstAreas)) ? 1 : 0);
  updated |= ((withdrawPrefixesImpl(toRemove)) ? 1 : 0);
  return updated;
}

bool
PrefixManager::withdrawPrefixesByTypeImpl(thrift::PrefixType type) {
  std::vector<thrift::PrefixEntry> toRemove;
  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    auto it = typeToPrefixes.find(type);
    if (it != typeToPrefixes.end()) {
      toRemove.emplace_back(*it->second.tPrefixEntry);
    }
  }

  return withdrawPrefixesImpl(toRemove);
}

void
PrefixManager::aggregatesToAdvertise(const folly::CIDRNetwork& prefix) {
  // ATTN: ignore attribute-ONLY update for existing RIB entries
  //       as it won't affect `supporting_route_cnt`
  auto [ribPrefixIt, inserted] =
      ribPrefixDb_.emplace(prefix, std::vector<folly::CIDRNetwork>());
  if (not inserted) {
    return;
  }

  for (auto& [network, route] : originatedPrefixDb_) {
    // folly::CIDRNetwork.first -> IPAddress
    // folly::CIDRNetwork.second -> cidr length
    if (not prefix.first.inSubnet(network.first, network.second)) {
      continue;
    }

    VLOG(1) << "[Route Origination] Adding supporting route "
            << folly::IPAddress::networkToString(prefix)
            << " for originated route "
            << folly::IPAddress::networkToString(network);

    // reverse mapping: RIB prefixEntry -> OriginatedPrefixes
    ribPrefixIt->second.emplace_back(network);

    // mapping: OriginatedPrefix -> RIB prefixEntries
    route.supportingRoutes.emplace(prefix);
  }
}

void
PrefixManager::aggregatesToWithdraw(const folly::CIDRNetwork& prefix) {
  // ignore invalid RIB entry
  auto ribPrefixIt = ribPrefixDb_.find(prefix);
  if (ribPrefixIt == ribPrefixDb_.end()) {
    return;
  }

  // clean mapping
  for (auto& network : ribPrefixIt->second) {
    auto originatedPrefixIt = originatedPrefixDb_.find(network);
    CHECK(originatedPrefixIt != originatedPrefixDb_.end());
    auto& route = originatedPrefixIt->second;

    VLOG(1) << "[Route Origination] Removing supporting route "
            << folly::IPAddress::networkToString(prefix)
            << " for originated route "
            << folly::IPAddress::networkToString(network);

    route.supportingRoutes.erase(prefix);
  }

  // clean local caching
  ribPrefixDb_.erase(prefix);
}

void
PrefixManager::processOriginatedPrefixes() {
  std::vector<PrefixEntry> advertisedPrefixes{};
  std::vector<thrift::PrefixEntry> withdrawnPrefixes{};

  for (auto& [network, route] : originatedPrefixDb_) {
    if (route.shouldAdvertise()) {
      route.isAdvertised = true; // mark as advertised
      advertisedPrefixes.emplace_back(
          std::make_shared<thrift::PrefixEntry>(
              route.unicastEntry.bestPrefixEntry),
          allAreaIds());
      if (route.originatedPrefix.install_to_fib_ref().has_value() &&
          *route.originatedPrefix.install_to_fib_ref()) {
        advertisedPrefixes.back().nexthops = route.unicastEntry.nexthops;
      }
      LOG(INFO) << "[Route Origination] Advertising originated route "
                << folly::IPAddress::networkToString(network);
    }

    if (route.shouldWithdraw()) {
      route.isAdvertised = false; // mark as withdrawn
      withdrawnPrefixes.emplace_back(
          createPrefixEntry(toIpPrefix(network), thrift::PrefixType::CONFIG));

      VLOG(1) << "[Route Origination] Withdrawing originated route "
              << folly::IPAddress::networkToString(network);
    }
  }
  // advertise originated config routes to KvStore
  advertisePrefixesImpl(advertisedPrefixes);
  withdrawPrefixesImpl(withdrawnPrefixes);
}

void
PrefixManager::processDecisionRouteUpdates(
    DecisionRouteUpdate&& decisionRouteUpdate) {
  std::vector<PrefixEntry> advertisedPrefixes{};
  std::vector<thrift::PrefixEntry> withdrawnPrefixes{};

  // ATTN: Routes imported from local BGP won't show up inside
  // `decisionRouteUpdate`. However, local-originated static route
  // (e.g. from route-aggregation) can come along.

  // Add/Update unicast routes
  for (auto& [prefix, route] : decisionRouteUpdate.unicastRoutesToUpdate) {
    // NOTE: future expansion - run egress policy here

    //
    // cross area, modify attributes
    //
    auto& prefixEntry = route.bestPrefixEntry;

    if (*prefixEntry.type_ref() == thrift::PrefixType::CONFIG) {
      // skip local-originated prefix as it won't be considered as
      // part of its own supporting routes.
      auto originatedPrefixIt = originatedPrefixDb_.find(prefix);
      if (originatedPrefixIt != originatedPrefixDb_.end()) {
        continue;
      }
    }

    // 1. append area stack
    prefixEntry.area_stack_ref()->emplace_back(route.bestArea);
    // 2. increase distance by 1
    ++(*prefixEntry.metrics_ref()->distance_ref());
    // 3. normalize to RIB routes
    prefixEntry.type_ref() = thrift::PrefixType::RIB;
    // 4. Avoid leaking prepend labels into other areas.
    // Today prefixes with prepend label are produced in BgpSpeaker and arrive
    // at prefixUpdatesQueue. Prefixes extracted from decisionRouteUpdate are
    // for the purposes of redistribution from one area to another.
    prefixEntry.prependLabel_ref().reset();

    // populate routes to be advertised to KvStore
    auto dstAreas = allAreaIds();
    for (const auto& nh : route.nexthops) {
      if (nh.area_ref().has_value()) {
        dstAreas.erase(*nh.area_ref());
      }
    }
    advertisedPrefixes.emplace_back(
        std::make_shared<thrift::PrefixEntry>(std::move(prefixEntry)),
        std::move(dstAreas));

    // adjust supporting route count due to prefix advertisement
    aggregatesToAdvertise(prefix);
  }

  // Delete unicast routes
  for (const auto& prefix : decisionRouteUpdate.unicastRoutesToDelete) {
    // TODO: remove this when advertise RibUnicastEntry for routes to delete
    if (originatedPrefixDb_.count(prefix)) {
      // skip local-originated prefix as it won't be considered as
      // part of its own supporting routes.
      continue;
    }

    // Routes to be withdrawn via KvStore
    withdrawnPrefixes.emplace_back(
        createPrefixEntry(toIpPrefix(prefix), thrift::PrefixType::RIB));

    // adjust supporting route count due to prefix withdrawn
    aggregatesToWithdraw(prefix);
  }

  // Maybe advertise/withdrawn for local originated routes
  processOriginatedPrefixes();

  // Redisrtibute RIB route ONLY when there are multiple `areaId` configured .
  // We want to keep processDecisionRouteUpdates() running as dynamic
  // configuration could add/remove areas.
  if (areaToPolicy_.size() > 1) {
    advertisePrefixesImpl(advertisedPrefixes);
    withdrawPrefixesImpl(withdrawnPrefixes);
  }

  // ignore mpls updates
}

std::unordered_set<std::string>
PrefixManager::allAreaIds() {
  std::unordered_set<std::string> allAreaIds;
  for (const auto& [area, _] : areaToPolicy_) {
    allAreaIds.emplace(area);
  }
  return allAreaIds;
}

} // namespace openr
