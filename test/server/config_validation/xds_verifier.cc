#include "test/server/config_validation/xds_verifier.h"

std::string XdsVerifier::getRoute(envoy::config::listener::v3::Listener listener) {
  envoy::config::listener::v3::Filter filter0 = listener.filter_chains()[0].filters()[0];
  envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager conn_man;
  filter0.typed_config().UnpackTo(&conn_man);
  return conn_man.rds().route_config_name();
}

void XdsVerifier::listenerAdded(envoy::config::listener::v3::Listener listener, bool updated) {
  if (updated) {
    // a listener in listeners_ needs to drain
    for (auto& listener_rep : listeners_) {
      if (listener_rep.listener.name() == listener.name()) {
        listener_rep.state = ListenerState::DRAINING;
        // drain it with simulated time?
      }
    }
  }

  bool found = false;
  for (auto& route : routes_) {
    if (getRoute(listener) == route.name()) {
      // will need to change if there are multiple routes that the listener can
      // reference
      listeners_.push_back({listener, ACTIVE});
      found = true;
    }
  }

  if (!found) {
    listeners_.push_back({listener, ListenerState::WARMING});
  }

  // check some stats here
}

void XdsVerifier::listenerRemoved(std::string& name) {
  for (auto& listener_rep : listeners_) {
    if (listener_rep.listener.name() == name) {
      listener_rep.state = ListenerState::DRAINING;
      // wait for it to drain?
    }
  }
  // check some stats here
  // TestUtility::findCounter(statStore(), name);
  // listener_manager.listener_create_success
  // check in server.listenerManager().listeners() that it is removed
}

void XdsVerifier::routeAdded(envoy::config::route::v3::RouteConfiguration route, bool updated) {
  if (updated) {
    // do routes do anything when they're updated?
  }

  for (auto& listener_rep : listeners_) {
    if (getRoute(listener_rep.listener) == route.name()) {
      // it should successfully warm now
      listener_rep.state = ListenerState::ACTIVE;
    }
  }
  // check that the listener became active in the server stats
}

void XdsVerifier::routeRemoved(std::string& name) {
  // it might not be possible to remove a route when it references listeners,
  // check this again
  for (auto& route : routes_) {
    if (route.name() == name) {
      ;
    }
  }
}
