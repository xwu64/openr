/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <common/fb303/cpp/FacebookBase2.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>
#include <openr/common/OpenrEventLoop.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/OpenrCtrl.h>

namespace openr {
class OpenrCtrlHandler final : public thrift::OpenrCtrlSvIf,
                               public facebook::fb303::FacebookBase2 {
 public:
  /**
   * NOTE: If acceptablePeerCommonNames is empty then check for peerName is
   *       skipped
   */
  OpenrCtrlHandler(
      const std::string& nodeName,
      const std::unordered_set<std::string>& acceptablePeerCommonNames,
      std::unordered_map<
          thrift::OpenrModuleType,
          std::shared_ptr<OpenrEventLoop>>& moduleTypeToEvl,
      MonitorSubmitUrl const& monitorSubmitUrl,
      fbzmq::Context& context);

  ~OpenrCtrlHandler() override;

  //
  // Raw APIs to directly interact with Open/R modules
  //

  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_command(
      thrift::OpenrModuleType type,
      std::unique_ptr<std::string> request) override;

  folly::SemiFuture<bool> semifuture_hasModule(
      thrift::OpenrModuleType type) override;

  //
  // fb303 service APIs
  //

  facebook::fb303::cpp2::fb_status getStatus() override;

  void getCounters(std::map<std::string, int64_t>& _return) override;
  void getRegexCounters(
      std::map<std::string, int64_t>& _return,
      std::unique_ptr<std::string> regex) override;
  void getSelectedCounters(
      std::map<std::string, int64_t>& _return,
      std::unique_ptr<std::vector<std::string>> keys) override;
  int64_t getCounter(std::unique_ptr<std::string> key) override;

  //
  // Route APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
  semifuture_getRouteDb() override;

  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
  semifuture_getRouteDbComputed(std::unique_ptr<std::string> nodeName) override;

  //
  // Performance stats APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>>
  semifuture_getPerfDb() override;

  //
  // Decision APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>>
  semifuture_getDecisionAdjacencyDbs() override;

  folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>>
  semifuture_getDecisionPrefixDbs() override;

  //
  // HealthChecker APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::HealthCheckerInfo>>
  semifuture_getHealthCheckerInfo() override;

  //
  // KvStore APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyVals(
      std::unique_ptr<std::vector<std::string>> filterKeys) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsFiltered(
      std::unique_ptr<thrift::KeyDumpParams> filter) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreHashFiltered(
      std::unique_ptr<thrift::KeyDumpParams> filter) override;

  folly::SemiFuture<folly::Unit> semifuture_setKvStoreKeyVals(
      std::unique_ptr<thrift::KeySetParams> setParams) override;
  folly::SemiFuture<folly::Unit> semifuture_setKvStoreKeyValsOneWay(
      std::unique_ptr<thrift::KeySetParams> setParams) override;

  folly::SemiFuture<folly::Unit> semifuture_processKvStoreDualMessage(
      std::unique_ptr<thrift::DualMessages> messages) override;

  folly::SemiFuture<folly::Unit> semifuture_updateFloodTopologyChild(
      std::unique_ptr<thrift::FloodTopoSetParams> params) override;

  folly::SemiFuture<std::unique_ptr<thrift::SptInfo>>
  semifuture_getSpanningTreeInfo() override;

  folly::SemiFuture<folly::Unit> semifuture_addUpdateKvStorePeers(
      std::unique_ptr<thrift::PeersMap> peers) override;

  folly::SemiFuture<folly::Unit> semifuture_deleteKvStorePeers(
      std::unique_ptr<std::vector<std::string>> peerNames) override;

  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeers() override;

  //
  // LinkMonitor APIs
  //

  folly::SemiFuture<folly::Unit> semifuture_setNodeOverload() override;
  folly::SemiFuture<folly::Unit> semifuture_unsetNodeOverload() override;

  folly::SemiFuture<folly::Unit> semifuture_setInterfaceOverload(
      std::unique_ptr<std::string> interfaceName) override;
  folly::SemiFuture<folly::Unit> semifuture_unsetInterfaceOverload(
      std::unique_ptr<std::string> interfaceName) override;

  folly::SemiFuture<folly::Unit> semifuture_setInterfaceMetric(
      std::unique_ptr<std::string> interfaceName,
      int32_t overrideMetric) override;
  folly::SemiFuture<folly::Unit> semifuture_unsetInterfaceMetric(
      std::unique_ptr<std::string> interfaceName) override;

  folly::SemiFuture<folly::Unit> semifuture_setAdjacencyMetric(
      std::unique_ptr<std::string> interfaceName,
      std::unique_ptr<std::string> adjNodeName,
      int32_t overrideMetric) override;
  folly::SemiFuture<folly::Unit> semifuture_unsetAdjacencyMetric(
      std::unique_ptr<std::string> interfaceName,
      std::unique_ptr<std::string> adjNodeName) override;

  folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
  semifuture_getInterfaces() override;

  folly::SemiFuture<std::unique_ptr<thrift::OpenrVersions>>
  semifuture_getOpenrVersion() override;

  folly::SemiFuture<std::unique_ptr<thrift::BuildInfo>>
  semifuture_getBuildInfo() override;

 private:
  // For oneway requests, empty message will be returned immediately
  folly::Expected<fbzmq::Message, fbzmq::Error> requestReplyMessage(
      thrift::OpenrModuleType module, fbzmq::Message&& request, bool oneway);

  template <typename ReturnType, typename InputType>
  folly::Expected<ReturnType, fbzmq::Error> requestReplyThrift(
      thrift::OpenrModuleType module, InputType&& input);

  template <typename InputType>
  folly::SemiFuture<folly::Unit> processThriftRequest(
      thrift::OpenrModuleType module, InputType&& request, bool oneway);

  void authorizeConnection();
  const std::string nodeName_;
  const std::unordered_set<std::string> acceptablePeerCommonNames_;
  std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
      moduleTypeToEvl_;
  std::unordered_map<
      thrift::OpenrModuleType,
      fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>>
      moduleSockets_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

}; // class OpenrCtrlHandler
} // namespace openr
