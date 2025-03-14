/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>
#include <vector>

#include <folly/IPAddress.h>
#include <openr/common/Util.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RibPolicy.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

/*
 * Generic structure to represent a route update. There are various sources and
 * consumers of route updates,
 * - Decision produces routes updates, consumed by Fib;
 * - Fib produces programmed routes, consumed by PrefixManager/BgpSpeaker;
 * - BgpSpeaker produces static MPLS prepend label routes, consumed by Decision;
 * - PrefixManager produces static unicast routes, consumed by Decision.
 */
struct DecisionRouteUpdate {
  enum Type {
    // Default value.
    // [Not recommended] Producer and consumer have implicit signal indicating
    // route updates are incremental or from full sync.
    DEFAULT,
    // Incremental route updates.
    INCREMENTAL,
    // Route updates from full sync.
    FULL_SYNC
  };

  Type type;
  std::unordered_map<folly::CIDRNetwork /* prefix */, RibUnicastEntry>
      unicastRoutesToUpdate;
  std::vector<folly::CIDRNetwork> unicastRoutesToDelete;
  std::vector<RibMplsEntry> mplsRoutesToUpdate;
  std::vector<int32_t> mplsRoutesToDelete;
  std::optional<thrift::PerfEvents> perfEvents = std::nullopt;

  void
  addRouteToUpdate(RibUnicastEntry const& route) {
    CHECK(!unicastRoutesToUpdate.count(route.prefix));
    unicastRoutesToUpdate.emplace(route.prefix, route);
  }

  void
  addRouteToUpdate(RibUnicastEntry&& route) {
    auto prefix = route.prefix;
    CHECK(!unicastRoutesToUpdate.count(prefix));
    unicastRoutesToUpdate.emplace(std::move(prefix), std::move(route));
  }

  // TODO: rename this func
  thrift::RouteDatabaseDelta
  toThrift() {
    thrift::RouteDatabaseDelta delta;

    // unicast
    for (const auto& [_, route] : unicastRoutesToUpdate) {
      delta.unicastRoutesToUpdate_ref()->emplace_back(route.toThrift());
    }
    for (const auto& route : unicastRoutesToDelete) {
      delta.unicastRoutesToDelete_ref()->emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& route : mplsRoutesToUpdate) {
      delta.mplsRoutesToUpdate_ref()->emplace_back(route.toThrift());
    }
    *delta.mplsRoutesToDelete_ref() = mplsRoutesToDelete;
    delta.perfEvents_ref().from_optional(perfEvents);

    return delta;
  }

  // TODO: rename this func
  thrift::RouteDatabaseDeltaDetail
  toThriftDetail() {
    thrift::RouteDatabaseDeltaDetail deltaDetail;

    // unicast
    for (const auto& [_, route] : unicastRoutesToUpdate) {
      deltaDetail.unicastRoutesToUpdate_ref()->emplace_back(
          route.toThriftDetail());
    }
    for (const auto& route : unicastRoutesToDelete) {
      deltaDetail.unicastRoutesToDelete_ref()->emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& route : mplsRoutesToUpdate) {
      deltaDetail.mplsRoutesToUpdate_ref()->emplace_back(
          route.toThriftDetail());
    }
    *deltaDetail.mplsRoutesToDelete_ref() = mplsRoutesToDelete;

    return deltaDetail;
  }
};

} // namespace openr
