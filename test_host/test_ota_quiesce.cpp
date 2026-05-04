// Pins the pre-flash quiesce ordering. The original ordering destroyed
// the Nostr data source's SubscriptionManager (via DataHub::StopAll())
// before the zap listener was unsubscribed; in shared-WSS mode the
// listener borrows that manager, so the next subs_.Unsubscribe() UAFed
// on the freed std::mutex and the device panicked with a Cache error /
// MMU entry fault before esp_https_ota_begin even ran (Rev B
// 4.0.0-beta.11, captured serial trace).
//
// Test strategy: stub the three duck-typed types QuiesceOtaPreFlash
// expects (a hub, a relay, a listener) and assert two invariants:
//
//   1. The listener fires Stop() BEFORE the hub fires StopAll().
//      That's the bug-fixing invariant — flip it and shared-WSS mode
//      crashes again.
//   2. After hub->StopAll() pretends to destroy the data source's
//      SubscriptionManager, the listener does NOT touch it. We model
//      this with a "subscription_manager_alive" flag that StopAll
//      flips off; the listener's Stop must run while the flag is
//      still on.
//
// Doctest's strict-equality call-order log is the primary assertion;
// the secondary alive-flag check is belt-and-braces against a future
// edit that swaps the order back without anyone re-reading
// ota_quiesce.hpp's comment.

#include <string>
#include <vector>

#include "app/ota_quiesce.hpp"
#include "doctest.h"

namespace {

// Tracks the call sequence a real teardown would produce. The exact
// strings here are the test's contract; the production code talks to
// real ESP-IDF objects (esp_websocket_client, DataSource::Stop) but
// the ORDER is the same and the host test is what catches a regression.
struct CallLog {
  std::vector<std::string> events;
  bool subscription_manager_alive = true;
};

struct StubHub {
  CallLog* log;
  void StopAll() const {
    log->events.emplace_back("hub.StopAll");
    // Models NostrDataSource::Stop() resetting subs_, which destroys
    // the SubscriptionManager the zap listener may be borrowing.
    log->subscription_manager_alive = false;
  }
};

struct StubRelay {
  CallLog* log;
  void Stop() const { log->events.emplace_back("relay.Stop"); }
};

struct StubListener {
  CallLog* log;
  void Stop() const {
    // Must observe the SubscriptionManager still alive. If StopAll
    // ran first this would be false and the real code's
    // pthread_mutex_lock would dereference freed memory.
    REQUIRE(log->subscription_manager_alive);
    log->events.emplace_back("listener.Stop");
  }
};

}  // namespace

TEST_CASE(
    "QuiesceOtaPreFlash: listener stops before hub (shared-WSS UAF guard)") {
  CallLog log;
  StubListener listener{&log};
  StubRelay relay{&log};
  StubHub hub{&log};

  btclock::QuiesceOtaPreFlash(&listener, &relay, &hub);

  REQUIRE(log.events.size() == 3);
  CHECK(log.events[0] == "listener.Stop");
  CHECK(log.events[1] == "relay.Stop");
  CHECK(log.events[2] == "hub.StopAll");
}

TEST_CASE("QuiesceOtaPreFlash: shared-WSS mode (zap_relay null) still safe") {
  // When the zap listener rides the data source's WSS, ctx.zap_relay
  // is null. The hook must still tear the listener down before the
  // hub destroys the underlying SubscriptionManager.
  CallLog log;
  StubListener listener{&log};
  StubHub hub{&log};

  btclock::QuiesceOtaPreFlash<StubListener, StubRelay>(&listener, nullptr,
                                                       &hub);

  REQUIRE(log.events.size() == 2);
  CHECK(log.events[0] == "listener.Stop");
  CHECK(log.events[1] == "hub.StopAll");
  // Subscription manager was alive when the listener Stop fired.
  // (StubListener::Stop REQUIREs this; the assertion would have
  // exploded above if the order were wrong.)
}

TEST_CASE("QuiesceOtaPreFlash: every pointer null is a no-op, not a crash") {
  // The hook may run before any of these are constructed (e.g. in a
  // future early-boot OTA path). All-null must be tolerated.
  btclock::QuiesceOtaPreFlash<StubListener, StubRelay, StubHub>(
      nullptr, nullptr, nullptr);
}

TEST_CASE("QuiesceOtaPreFlash: dedicated-WSS mode (everything set)") {
  // The non-shared shape — zap_relay is the listener's private
  // RelayClient. Order is the same: stop the listener first (sends
  // CLOSE while the WSS is up), then the dedicated relay, then the
  // hub. The hub doesn't own the relay in this shape, but the order
  // still has to match so a future change that introduces sharing
  // doesn't silently regress.
  CallLog log;
  StubListener listener{&log};
  StubRelay relay{&log};
  StubHub hub{&log};

  btclock::QuiesceOtaPreFlash(&listener, &relay, &hub);

  REQUIRE(log.events.size() == 3);
  CHECK(log.events[0] == "listener.Stop");
  CHECK(log.events[1] == "relay.Stop");
  CHECK(log.events[2] == "hub.StopAll");
}
