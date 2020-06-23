#include "envoy/admin/v3/config_dump.pb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/listener/v3/listener.pb.h"
#include "envoy/config/route/v3/route.pb.h"
#include "envoy/config/filter/network/http_connection_manager/v2/http_connection_manager.pb.h"

#include "envoy/common/exception.h"

namespace Envoy {

class XdsVerifier {
public:
  XdsVerifier() = default;
  void listenerAdded(envoy::config::listener::v3::Listener listener, bool updated=false);
  void listenerRemoved(std::string& name);
  void routeAdded(envoy::config::route::v3::RouteConfiguration route, bool updated=false);
  void routeRemoved(std::string& name);
  void drainedListener(const std::string& name);

  enum ListenerState {
    WARMING,
    ACTIVE,
    DRAINING
  };
  struct ListenerRep {
    envoy::config::listener::v3::Listener listener;
    ListenerState state;
  };

  const std::vector<ListenerRep> &listeners() const { return listeners_; }
  const std::vector<envoy::config::route::v3::RouteConfiguration> &routes() const { return routes_; }

  uint32_t numWarming() { return num_warming; }
  uint32_t numActive() { return num_active; }
  uint32_t numDraining() { return num_draining; }

private:
  std::string getRoute(envoy::config::listener::v3::Listener);
  std::vector<ListenerRep> listeners_;
  std::vector<envoy::config::route::v3::RouteConfiguration> routes_;
  uint32_t num_warming;
  uint32_t num_active;
  uint32_t num_draining;
};

} // namespace Envoy
