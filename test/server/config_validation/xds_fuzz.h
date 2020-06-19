#pragma once

#include <memory>
#include <string>

#include "envoy/admin/v3/config_dump.pb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/listener/v3/listener.pb.h"
#include "envoy/config/route/v3/route.pb.h"

#include "test/common/grpc/grpc_client_integration.h"
#include "test/config/utility.h"
#include "test/integration/http_integration.h"
#include "test/server/config_validation/xds_fuzz.pb.h"
#include "test/server/config_validation/xds_verifier.h"

#include "absl/types/optional.h"

// TODO(samflattery): add these to fuzz config instead?
#define NUM_LISTENERS 3
#define NUM_ROUTES 5

namespace Envoy {

class XdsFuzzTest : public HttpIntegrationTest {
public:
  XdsFuzzTest(const test::server::config_validation::XdsTestCase& input,
              envoy::config::core::v3::ApiVersion api_version);

  envoy::config::cluster::v3::Cluster buildCluster(const std::string& name);

  envoy::config::endpoint::v3::ClusterLoadAssignment
  buildClusterLoadAssignment(const std::string& name);

  envoy::config::listener::v3::Listener buildListener(uint32_t listener_num, uint32_t route_num);

  envoy::config::route::v3::RouteConfiguration buildRouteConfig(uint32_t route_num);

  void updateListener(const std::vector<envoy::config::listener::v3::Listener>& listeners,
                      const std::vector<envoy::config::listener::v3::Listener>& added_or_updated,
                      const std::vector<std::string>& removed);

  void
  updateRoute(const std::vector<envoy::config::route::v3::RouteConfiguration> routes,
              const std::vector<envoy::config::route::v3::RouteConfiguration>& added_or_updated,
              const std::vector<std::string>& removed);

  void initialize() override;
  void replay();
  void close();

private:
  void parseConfig(const test::server::config_validation::XdsTestCase& input);
  XdsVerifier verifier;

  absl::optional<std::string> removeListener(uint32_t listener_num);
  absl::optional<std::string> removeRoute(uint32_t route_num);

  Protobuf::RepeatedPtrField<test::server::config_validation::Action> actions_;
  std::vector<envoy::config::route::v3::RouteConfiguration> routes_;
  std::vector<envoy::config::listener::v3::Listener> listeners_;

  std::vector<envoy::config::listener::v3::Listener> listener_pool_;
  std::vector<envoy::config::route::v3::RouteConfiguration> route_pool_;

  Network::Address::IpVersion ip_version_;
  Grpc::ClientType client_type_;
  Grpc::SotwOrDelta sotw_or_delta_;

  uint64_t version_;
  envoy::config::core::v3::ApiVersion api_version_;
};

} // namespace Envoy
