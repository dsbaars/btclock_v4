// Host tests for the concurrent-provisioning fallback decision helpers.
// Covers the grace window, the has-connected-once latch (mid-run outages
// are OutageWatchdog's job, not the fallback's), the already-up guard,
// and the teardown-on-reconnect edge.

#include <cstdint>

#include "doctest.h"
#include "io/provisioning_fallback.hpp"

using btclock::ShouldStartFallbackAp;
using btclock::ShouldTeardownAp;

namespace {
constexpr uint32_t kBoot = 1'000u;
constexpr uint32_t kGrace = 20'000u;  // 20 s default grace
}  // namespace

TEST_CASE("ShouldStartFallbackAp: within grace, do not bring up the AP") {
  const uint32_t now = kBoot + kGrace - 1u;
  CHECK_FALSE(ShouldStartFallbackAp(/*connected_once=*/false, /*ap_up=*/false,
                                    /*sta_connected=*/false, now, kBoot,
                                    kGrace));
}

TEST_CASE("ShouldStartFallbackAp: exactly at grace, bring up the AP") {
  const uint32_t now = kBoot + kGrace;  // >= semantics
  CHECK(ShouldStartFallbackAp(false, false, false, now, kBoot, kGrace));
}

TEST_CASE("ShouldStartFallbackAp: past grace, bring up the AP") {
  const uint32_t now = kBoot + kGrace + 60'000u;
  CHECK(ShouldStartFallbackAp(false, false, false, now, kBoot, kGrace));
}

TEST_CASE("ShouldStartFallbackAp: grace 0 brings the AP up immediately") {
  // grace_ms == 0 means "no grace window" — fallback fires on the first
  // tick (now == boot). This is the 'always-APSTA' configuration.
  CHECK(ShouldStartFallbackAp(false, false, false, kBoot, kBoot, 0u));
}

TEST_CASE("ShouldStartFallbackAp: already connected suppresses the AP") {
  const uint32_t now = kBoot + kGrace + 1u;
  CHECK_FALSE(ShouldStartFallbackAp(false, false, /*sta_connected=*/true, now,
                                    kBoot, kGrace));
}

TEST_CASE("ShouldStartFallbackAp: AP already up is not re-triggered") {
  const uint32_t now = kBoot + kGrace + 1u;
  CHECK_FALSE(
      ShouldStartFallbackAp(false, /*ap_up=*/true, false, now, kBoot, kGrace));
}

TEST_CASE(
    "ShouldStartFallbackAp: has_connected_once latches the fallback off") {
  // After a successful first connect, a later disconnect must NOT pop the
  // provisioning AP — that window belongs to OutageWatchdog. Even well
  // past the grace and with STA down, the fallback stays disarmed.
  const uint32_t now = kBoot + 10u * 60u * 1000u;  // 10 min later
  CHECK_FALSE(ShouldStartFallbackAp(/*connected_once=*/true, false,
                                    /*sta_connected=*/false, now, kBoot,
                                    kGrace));
}

TEST_CASE("ShouldTeardownAp: tear down only when AP up AND STA connected") {
  CHECK(ShouldTeardownAp(/*ap_up=*/true, /*sta_connected=*/true));
  CHECK_FALSE(ShouldTeardownAp(/*ap_up=*/true, /*sta_connected=*/false));
  CHECK_FALSE(ShouldTeardownAp(/*ap_up=*/false, /*sta_connected=*/true));
  CHECK_FALSE(ShouldTeardownAp(/*ap_up=*/false, /*sta_connected=*/false));
}
