#include "test/server/config_validation/xds_fuzz.h"

#include "common/common/logger.h"
#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/listener/v3/listener.pb.h"
#include "envoy/config/route/v3/route.pb.h"

#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

namespace Envoy {

// helper functions to build API responses
envoy::config::cluster::v3::Cluster XdsFuzzTest::buildCluster(const std::string& name) {
  return ConfigHelper::buildCluster(name, "ROUND_ROBIN", api_version_);
}

envoy::config::endpoint::v3::ClusterLoadAssignment
XdsFuzzTest::buildClusterLoadAssignment(const std::string& name) {
  return ConfigHelper::buildClusterLoadAssignment(
      name, Network::Test::getLoopbackAddressString(ip_version_),
      fake_upstreams_[0]->localAddress()->ip()->port(), api_version_);
}

envoy::config::listener::v3::Listener XdsFuzzTest::buildListener(uint32_t listener_num,
                                                                 uint32_t route_num) {
  std::string name = fmt::format("{}{}", "listener_", listener_num % NUM_LISTENERS);
  std::string route = fmt::format("{}{}", "route_config_", route_num % NUM_ROUTES);
  return ConfigHelper::buildListener(
      name, route, Network::Test::getLoopbackAddressString(ip_version_), "ads_test", api_version_);
}

envoy::config::route::v3::RouteConfiguration XdsFuzzTest::buildRouteConfig(uint32_t route_num) {
  std::string route = fmt::format("{}{}", "route_config_", route_num % NUM_ROUTES);
  return ConfigHelper::buildRouteConfig(route, "cluster_0", api_version_);
}

// helper functions to send API responses
void XdsFuzzTest::updateListener(
    const std::vector<envoy::config::listener::v3::Listener>& listeners,
    const std::vector<envoy::config::listener::v3::Listener>& added_or_updated,
    const std::vector<std::string>& removed) {
  ENVOY_LOG_MISC(info, "Sending Listener DiscoveryResponse version {}", version_);
  sendDiscoveryResponse<envoy::config::listener::v3::Listener>(Config::TypeUrl::get().Listener,
                                                               listeners, added_or_updated, removed,
                                                               std::to_string(version_));
}

void XdsFuzzTest::updateRoute(
    const std::vector<envoy::config::route::v3::RouteConfiguration> routes,
    const std::vector<envoy::config::route::v3::RouteConfiguration>& added_or_updated,
    const std::vector<std::string>& removed) {
  ENVOY_LOG_MISC(info, "Sending Route DiscoveryResponse version {}", version_);
  sendDiscoveryResponse<envoy::config::route::v3::RouteConfiguration>(
      Config::TypeUrl::get().RouteConfiguration, routes, added_or_updated, removed,
      std::to_string(version_));
}

XdsFuzzTest::XdsFuzzTest(const test::server::config_validation::XdsTestCase& input,
                         envoy::config::core::v3::ApiVersion api_version)
    : HttpIntegrationTest(
          Http::CodecClient::Type::HTTP2,
          input.config().ip_version() == test::server::config_validation::Config::IPv4
              ? Network::Address::IpVersion::v4
              : Network::Address::IpVersion::v6,
          ConfigHelper::adsBootstrap(input.config().sotw_or_delta() ==
                                             test::server::config_validation::Config::SOTW
                                         ? "GRPC"
                                         : "DELTA_GRPC",
                                     api_version)),
      actions_(input.actions()), version_(1), api_version_(api_version) {
  use_lds_ = false;
  create_xds_upstream_ = true;
  tls_xds_upstream_ = false;

  parseConfig(input);
}

void XdsFuzzTest::parseConfig(const test::server::config_validation::XdsTestCase& input) {
  if (input.config().ip_version() == test::server::config_validation::Config::IPv4) {
    ip_version_ = Network::Address::IpVersion::v4;
  } else {
    ip_version_ = Network::Address::IpVersion::v6;
  }

  if (input.config().client_type() == test::server::config_validation::Config::GOOGLE_GRPC) {
    client_type_ = Grpc::ClientType::GoogleGrpc;
  } else {
    client_type_ = Grpc::ClientType::EnvoyGrpc;
  }

  if (input.config().sotw_or_delta() == test::server::config_validation::Config::SOTW) {
    sotw_or_delta_ = Grpc::SotwOrDelta::Sotw;
  } else {
    sotw_or_delta_ = Grpc::SotwOrDelta::Delta;
  }
}

/**
 * initialize an envoy configured with a fully dynamic bootstrap with ADS over gRPC
 */
void XdsFuzzTest::initialize() {
  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* ads_config = bootstrap.mutable_dynamic_resources()->mutable_ads_config();
    auto* grpc_service = ads_config->add_grpc_services();

    std::string cluster_name = "ads_cluster";
    switch (client_type_) {
    case Grpc::ClientType::EnvoyGrpc:
      grpc_service->mutable_envoy_grpc()->set_cluster_name(cluster_name);
      break;
    case Grpc::ClientType::GoogleGrpc: {
      auto* google_grpc = grpc_service->mutable_google_grpc();
      google_grpc->set_target_uri(xds_upstream_->localAddress()->asString());
      google_grpc->set_stat_prefix(cluster_name);
      break;
    }
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }
    auto* ads_cluster = bootstrap.mutable_static_resources()->add_clusters();
    ads_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
    ads_cluster->set_name("ads_cluster");
  });
  setUpstreamProtocol(FakeHttpConnection::Type::HTTP2);
  HttpIntegrationTest::initialize();
  if (xds_stream_ == nullptr) {
    createXdsConnection();
    AssertionResult result = xds_connection_->waitForNewStream(*dispatcher_, xds_stream_);
    RELEASE_ASSERT(result, result.message());
    xds_stream_->startGrpcStream();
  }
}

void XdsFuzzTest::close() {
  cleanUpXdsConnection();
  test_server_.reset();
  fake_upstreams_.clear();
}

/**
 * remove a listener from the list of listeners if it exists
 * @param the listener number to be removed
 * @return the listener as an optional so that it can be used in a delta request
 */
absl::optional<std::string> XdsFuzzTest::removeListener(uint32_t listener_num) {
  std::string match = fmt::format("{}{}", "listener_", listener_num % NUM_LISTENERS);

  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->name() == match) {
      std::string name = it->name();
      listeners_.erase(it);
      return name;
    }
  }
  return {};
}

/**
 * remove a route from the list of routes if it exists
 * @param the route number to be removed
 * @return the route as an optional so that it can be used in a delta request
 */
absl::optional<std::string> XdsFuzzTest::removeRoute(uint32_t route_num) {
  std::string match = fmt::format("{}{}", "route_config_", route_num % NUM_ROUTES);
  for (auto it = routes_.begin(); it != routes_.end(); ++it) {
    if (it->name() == match) {
      std::string name = it->name();
      routes_.erase(it);
      return name;
    }
  }
  return {};
}

AssertionResult XdsFuzzTest::waitForAck(const std::string& expected_type_url,
                                        const std::string& expected_version) {
  API_NO_BOOST(envoy::api::v2::DiscoveryRequest) discovery_request;
  do {
    VERIFY_ASSERTION(xds_stream_->waitForGrpcMessage(*dispatcher_, discovery_request));
  } while (expected_type_url != discovery_request.type_url() &&
           expected_version != discovery_request.version_info());
  /* ENVOY_LOG_MISC(info, "Successfully received ACK for {} version {}", expected_type_url, expected_version); */
  return AssertionSuccess();
}

/**
 * run the sequence of actions defined in the fuzzed protobuf
 */
void XdsFuzzTest::replay() {
  initialize();

  // set up cluster
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "", {}, {}, {}, true));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {buildCluster("cluster_0")},
                                                             {buildCluster("cluster_0")}, {}, "1");
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().ClusterLoadAssignment, "",
                                      {"cluster_0"}, {"cluster_0"}, {}));
  sendDiscoveryResponse<envoy::config::endpoint::v3::ClusterLoadAssignment>(
      Config::TypeUrl::get().ClusterLoadAssignment, {buildClusterLoadAssignment("cluster_0")},
      {buildClusterLoadAssignment("cluster_0")}, {}, "1");

  ENVOY_LOG_MISC(info, "added: {}, modified {}, removed {}",
                 test_server_->counter("listener_manager.listener_added")->value(),
                 test_server_->counter("listener_manager.listener_modified")->value(),
                 test_server_->counter("listener_manager.listener_removed")->value());

  version_++;

  uint32_t added = 0;
  uint32_t modified = 0;
  uint32_t removed_ = 0;

  for (const auto& action : actions_) {
    switch (action.action_selector_case()) {
    case test::server::config_validation::Action::kAddListener: {
      ENVOY_LOG_MISC(info, "Adding listener_{} with reference to route_{}",
                     action.add_listener().listener_num(), action.add_listener().route_num());
      auto removed = removeListener(action.add_listener().listener_num());
      auto listener =
          buildListener(action.add_listener().listener_num(), action.add_listener().route_num());
      listeners_.push_back(listener);

      updateListener(listeners_, {listener}, {});

      EXPECT_TRUE(waitForAck(Config::TypeUrl::get().Listener, std::to_string(version_)));

      if (removed) {
        modified++;
        verifier_.listenerAdded(listener, true);
        test_server_->waitForCounterGe("listener_manager.listener_modified", modified);
      } else {
        added++;
        verifier_.listenerAdded(listener, false);
        test_server_->waitForCounterGe("listener_manager.listener_added", added);
      }
      ENVOY_LOG_MISC(info, "added: {}, modified {}, removed {}",
                     test_server_->counter("listener_manager.listener_added")->value(),
                     test_server_->counter("listener_manager.listener_modified")->value(),
                     test_server_->counter("listener_manager.listener_removed")->value());
      break;
    }
    case test::server::config_validation::Action::kRemoveListener: {
      ENVOY_LOG_MISC(info, "Removing listener_{}", action.remove_listener().listener_num());
      auto removed = removeListener(action.remove_listener().listener_num());

      if (removed) {
        removed_++;
        updateListener(listeners_, {}, {*removed});
        verifier_.listenerRemoved(*removed);
        test_server_->waitForCounterGe("listener_manager.listener_removed", removed_);
      } else {
        updateListener(listeners_, {}, {});
      }
      EXPECT_TRUE(waitForAck(Config::TypeUrl::get().RouteConfiguration, std::to_string(version_)));
      ENVOY_LOG_MISC(info, "added: {}, modified {}, removed {}",
                     test_server_->counter("listener_manager.listener_added")->value(),
                     test_server_->counter("listener_manager.listener_modified")->value(),
                     test_server_->counter("listener_manager.listener_removed")->value());

      break;
    }
    case test::server::config_validation::Action::kAddRoute: {
      ENVOY_LOG_MISC(info, "Adding route_{}", action.add_route().route_num());
      auto removed = removeRoute(action.add_route().route_num());
      auto route = buildRouteConfig(action.add_route().route_num());
      routes_.push_back(route);
      updateRoute(routes_, {route}, {});
      EXPECT_TRUE(waitForAck(Config::TypeUrl::get().RouteConfiguration, std::to_string(version_)));

      if (removed) {
        verifier_.routeAdded(route, true);
      } else {
        verifier_.routeAdded(route, false);
      }

      break;
    }
    case test::server::config_validation::Action::kRemoveRoute: {
      ENVOY_LOG_MISC(info, "Removing route_{}", action.remove_route().route_num());
      if (sotw_or_delta_ == Grpc::SotwOrDelta::Sotw) {
        // routes cannot be removed in SOTW updates
        break;
      }

      auto removed = removeRoute(action.remove_route().route_num());

      if (removed) {
        updateRoute(routes_, {}, {*removed});
        verifier_.routeRemoved(*removed);
      } else {
        updateRoute(routes_, {}, {});
      }
      break;
    }
    default:
      break;
    }

    // TODO(samflattery): makeSingleRequest here?
    version_++;
  }

  verifyState();
  close();
}

void XdsFuzzTest::drainListener(const std::string& name) {
  // ensure that the listener drains correctly by checking that it is still draining halfway through
  ENVOY_LOG_MISC(info, "Draining {}", name);
  EXPECT_EQ(test_server_->gauge("listener_manager.total_listeners_draining")->value(),
            verifier_.numDraining());
  timeSystem().advanceTimeWait(std::chrono::milliseconds(300000));
  EXPECT_EQ(test_server_->gauge("listener_manager.total_listeners_draining")->value(),
            verifier_.numDraining());
  timeSystem().advanceTimeWait(std::chrono::milliseconds(300001));
  EXPECT_EQ(test_server_->gauge("listener_manager.total_listeners_draining")->value(),
            verifier_.numDraining() - 1);
  verifier_.drainedListener(name);
}

void XdsFuzzTest::verifyListeners() {
  ENVOY_LOG_MISC(info, "Verifying listeners");
  const auto& listeners = verifier_.listeners();
  ENVOY_LOG_MISC(info, "There are {} listeners in listeners_", listeners.size());
  envoy::admin::v3::ListenersConfigDump listener_dump = getListenersConfigDump();
  /* ENVOY_LOG_MISC(info, "{}", listener_dump.DebugString()); */
  for (auto& listener_rep : listeners) {
    ENVOY_LOG_MISC(info, "Verifying {} with state {}", listener_rep.listener.name(), listener_rep.state);
    bool found = false;
    for (auto& dump_listener : listener_dump.dynamic_listeners()) {
      if (dump_listener.name() == listener_rep.listener.name()) {
        ENVOY_LOG_MISC(info, "Found a matching listener in config dump");
        ENVOY_LOG_MISC(info, "drain: {}, warm {}, active {}", dump_listener.has_draining_state(), dump_listener.has_warming_state(), dump_listener.has_active_state());
        switch (listener_rep.state) {
        case XdsVerifier::DRAINING:
          if (dump_listener.has_draining_state()) {
            ENVOY_LOG_MISC(info, "{} is draining", listener_rep.listener.name());
            drainListener(listener_rep.listener.name());
            found = true;
          }
          break;
        case XdsVerifier::WARMING:
          if (dump_listener.has_warming_state()) {
            ENVOY_LOG_MISC(info, "{} is warming", listener_rep.listener.name());
            found = true;
          }
          break;
        case XdsVerifier::ACTIVE:
          if (dump_listener.has_active_state()) {
            ENVOY_LOG_MISC(info, "{} is active", listener_rep.listener.name());
            found = true;
          }
          break;
        default:
          NOT_REACHED_GCOVR_EXCL_LINE;
        }
      }
    }
    if (!found) {
      ENVOY_LOG_MISC(info, "Expected to find listener {} in config dump", listener_rep.listener.name());
      throw EnvoyException(
          fmt::format("Expected to find listener {} in config dump", listener_rep.listener.name()));
    }
  }
}

void XdsFuzzTest::verifyState() {
  verifyListeners();
  ENVOY_LOG_MISC(info, "Verified listeners");

  EXPECT_EQ(test_server_->gauge("listener_manager.total_listeners_warming")->value(),
            verifier_.numWarming());
  EXPECT_EQ(test_server_->gauge("listener_manager.total_listeners_active")->value(),
            verifier_.numActive());
  ENVOY_LOG_MISC(info, "Verified stats");
}

envoy::admin::v3::ClustersConfigDump XdsFuzzTest::getClustersConfigDump() {
  auto message_ptr =
      test_server_->server().admin().getConfigTracker().getCallbacksMap().at("clusters")();
  return dynamic_cast<const envoy::admin::v3::ClustersConfigDump&>(*message_ptr);
}

envoy::admin::v3::ListenersConfigDump XdsFuzzTest::getListenersConfigDump() {
  auto message_ptr =
      test_server_->server().admin().getConfigTracker().getCallbacksMap().at("listeners")();
  return dynamic_cast<const envoy::admin::v3::ListenersConfigDump&>(*message_ptr);
}

envoy::admin::v3::RoutesConfigDump XdsFuzzTest::getRoutesConfigDump() {
  auto message_ptr =
      test_server_->server().admin().getConfigTracker().getCallbacksMap().at("routes")();
  return dynamic_cast<const envoy::admin::v3::RoutesConfigDump&>(*message_ptr);
}

} // namespace Envoy
