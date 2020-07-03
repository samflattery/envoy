#include "test/server/config_validation/xds_verifier.h"

#include "common/common/logger.h"

namespace Envoy {

XdsVerifier::XdsVerifier() : num_warming_(0), num_active_(0), num_draining_(0) {}

/**
 * get the route referenced by a listener
 */
std::string XdsVerifier::getRoute(envoy::config::listener::v3::Listener listener) {
  envoy::config::listener::v3::Filter filter0 = listener.filter_chains()[0].filters()[0];
  envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager conn_man;
  filter0.typed_config().UnpackTo(&conn_man);
  return conn_man.rds().route_config_name();
}

void XdsVerifier::listenerAdded(envoy::config::listener::v3::Listener listener, bool updated) {
  if (updated) {
    // a listener in listeners_ needs to drain
    auto it = listeners_.begin();
    while (it != listeners_.end()) {
      if (it->listener.name() == listener.name()) {
        if (it->state == ACTIVE) {
          ENVOY_LOG_MISC(info, "Moving {} to DRAINING state", listener.name());
          num_active_--;
          num_draining_++;
          it->state = ListenerState::DRAINING;
          ++it;
        } else if (it->state == WARMING) {
          ENVOY_LOG_MISC(info, "Removed warming listener {}", listener.name());
          num_warming_--;
          it = listeners_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  bool found_route = false;
  for (auto& route : routes_) {
    if (getRoute(listener) == route.name()) {
      ENVOY_LOG_MISC(info, "Adding {} to listeners_ as ACTIVE", listener.name());
      // will need to change if there are multiple routes that the listener can reference
      listeners_.push_back({listener, ACTIVE});
      num_active_++;
      found_route = true;
    }
  }

  if (!found_route) {
    num_warming_++;
    ENVOY_LOG_MISC(info, "Adding {} to listeners_ as WARMING", listener.name());
    listeners_.push_back({listener, ListenerState::WARMING});
  }
}

void XdsVerifier::listenerRemoved(std::string& name) {
  auto it = listeners_.begin();
  while (it != listeners_.end()) {
    if (it->listener.name() == name) {
      if (it->state == ACTIVE) {
        ENVOY_LOG_MISC(info, "Changing {} to DRAINING", name);
        num_active_--;
        num_draining_++;
        it->state = ListenerState::DRAINING;
        ++it;
      } else if (it->state == WARMING) {
        ENVOY_LOG_MISC(info, "Removed warming listener {}", name);
        num_warming_--;
        it = listeners_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void XdsVerifier::routeAdded(envoy::config::route::v3::RouteConfiguration route, bool updated) {
  if (updated) {
    // do routes do anything when they're updated?
  }

  routes_.push_back(route);

  for (auto& listener_rep : listeners_) {
    if (getRoute(listener_rep.listener) == route.name()) {
      if (listener_rep.state == WARMING) {
        // it should successfully warm now
        ENVOY_LOG_MISC(info, "Moving {} to ACTIVE state", listener_rep.listener.name());
        num_warming_--;
        num_active_++;
        listener_rep.state = ListenerState::ACTIVE;
      }
    }
  }
}

void XdsVerifier::routeRemoved(std::string& name) {
  // it might not be possible to remove a route when it references listeners, check this again
  for (auto& route : routes_) {
    if (route.name() == name) {
      ;
    }
  }
}

void XdsVerifier::drainedListener(const std::string& name) {
  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->listener.name() == name && it->state == DRAINING) {
      ENVOY_LOG_MISC(info, "Drained and removed {}", name);
      listeners_.erase(it);
      return;
    }
  }
  throw EnvoyException("not draining");
}

} // namespace Envoy
