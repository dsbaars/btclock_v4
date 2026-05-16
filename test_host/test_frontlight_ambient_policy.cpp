// Host tests for FrontlightAmbientPolicy — the pure-logic state
// machine behind the ambient auto-on / auto-off + flOffWhenDark +
// user-off latch. See the v3 firmware's main.cpp:31-47 for the v3 path
// we mirror (without hysteresis, without a user-off latch).

#include <initializer_list>

#include "doctest.h"
#include "frontlight_logic/frontlight_ambient_policy.hpp"

using btclock::FrontlightAmbientAction;
using btclock::FrontlightAmbientConfig;
using btclock::FrontlightAmbientPolicy;

namespace {

FrontlightAmbientConfig MakeCfg(uint32_t threshold, bool off_when_dark,
                                bool enabled = true) {
  FrontlightAmbientConfig c{};
  c.ambient_auto_enabled = enabled;
  c.lux_threshold = threshold;
  c.off_when_dark = off_when_dark;
  return c;
}

}  // namespace

TEST_CASE("ambient-off feature-disabled returns kNone at every lux") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, false, /*enabled=*/false));

  for (float lux : {0.0f, 1.0f, 50.0f, 200.0f, 10000.0f}) {
    CHECK(p.Evaluate(lux) == FrontlightAmbientAction::kNone);
  }
}

TEST_CASE("lux_threshold == 0 disables the feature (v3 parity)") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(0, false));
  for (float lux : {0.0f, 1.0f, 50.0f, 200.0f}) {
    CHECK(p.Evaluate(lux) == FrontlightAmbientAction::kNone);
  }
}

TEST_CASE("negative lux (sensor error) is ignored") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, true));
  p.NoteOutputOn();
  CHECK(p.Evaluate(-1.0f) == FrontlightAmbientAction::kNone);
  // Dark-mode latch must not have flipped.
  CHECK(p.is_dark() == false);
}

TEST_CASE("normal threshold crossing: below -> on, above -> off") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(100, false));
  // Start dim with light off. Below threshold -> kOn.
  p.NoteOutputOff();
  CHECK(p.Evaluate(10.0f) == FrontlightAmbientAction::kOn);

  // Caller would now turn the light on; reflect that.
  p.NoteOutputOn();

  // Now very bright; should fade out.
  CHECK(p.Evaluate(500.0f) == FrontlightAmbientAction::kOff);
}

TEST_CASE("Bug 2: off_when_dark=true + lux=0 forces off even below threshold") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/true));

  // Light currently on, room just went dark.
  p.NoteOutputOn();
  CHECK(p.Evaluate(0.0f) == FrontlightAmbientAction::kOff);
  CHECK(p.is_dark());

  // Caller turned it off.
  p.NoteOutputOff();

  // Stays off across subsequent dark-room reads.
  CHECK(p.Evaluate(0.0f) == FrontlightAmbientAction::kNone);
  CHECK(p.Evaluate(0.5f) == FrontlightAmbientAction::kNone);
}

TEST_CASE("Bug 2: off_when_dark=true + lux=5 tracks normal auto-curve") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/true));
  p.NoteOutputOff();
  // Dim but not "dark" — below threshold, above dark-enter. Should
  // request kOn like the normal branch does.
  CHECK(p.Evaluate(5.0f) == FrontlightAmbientAction::kOn);
  CHECK(p.is_dark() == false);
}

TEST_CASE("Bug 2: off_when_dark=false + lux=0 follows normal curve") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/false));
  p.NoteOutputOff();
  // Below threshold -> normal kOn.
  CHECK(p.Evaluate(0.0f) == FrontlightAmbientAction::kOn);
  CHECK(p.is_dark() == false);
}

TEST_CASE("Bug 2: hysteresis around 1 lux doesn't flap") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/true));

  // Light on, room drops to dark.
  p.NoteOutputOn();
  CHECK(p.Evaluate(0.5f) == FrontlightAmbientAction::kOff);
  p.NoteOutputOff();
  REQUIRE(p.is_dark());

  // Noise crossing back to exactly 1.0 must NOT exit dark-mode yet
  // (exit threshold is 2.0).
  CHECK(p.Evaluate(1.0f) == FrontlightAmbientAction::kNone);
  CHECK(p.is_dark());

  // 1.5 is still in the hysteresis band — stays dark.
  CHECK(p.Evaluate(1.5f) == FrontlightAmbientAction::kNone);
  CHECK(p.is_dark());

  // 2.0 crosses the exit band — dark-mode releases. Below threshold
  // so the normal branch returns kOn.
  CHECK(p.Evaluate(2.0f) == FrontlightAmbientAction::kOn);
  CHECK(p.is_dark() == false);
}

TEST_CASE("Bug 2: off_when_dark=true with light currently off returns kNone") {
  // If we're already off, entering dark mode is a no-op (policy
  // doesn't need to enqueue a redundant kOff).
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/true));
  p.NoteOutputOff();
  CHECK(p.Evaluate(0.0f) == FrontlightAmbientAction::kNone);
  CHECK(p.is_dark());
}

TEST_CASE("Bug 1: user-off latch blocks ambient kOn (dark room case)") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/false));

  // Simulate: user hit /api/frontlight/off; controller latched it.
  p.SetUserOff(true);
  p.NoteOutputOff();

  // Room is dim. Without the latch this would return kOn.
  CHECK(p.Evaluate(10.0f) == FrontlightAmbientAction::kNone);
  // Still true across repeated ticks.
  CHECK(p.Evaluate(20.0f) == FrontlightAmbientAction::kNone);
  CHECK(p.Evaluate(50.0f) == FrontlightAmbientAction::kNone);

  // Bright room — ambient-off path is still allowed (idempotent when
  // output is already off, but the policy doesn't need to suppress
  // kOff). Output is off so nothing to do.
  CHECK(p.Evaluate(500.0f) == FrontlightAmbientAction::kNone);
}

TEST_CASE("Bug 1: user-off latch cleared -> ambient resumes") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, false));
  p.SetUserOff(true);
  p.NoteOutputOff();
  CHECK(p.Evaluate(10.0f) == FrontlightAmbientAction::kNone);

  // User hits /api/frontlight/on — controller clears latch.
  p.SetUserOff(false);
  // Same lux now returns kOn.
  CHECK(p.Evaluate(10.0f) == FrontlightAmbientAction::kOn);
}

TEST_CASE("Bug 1 + Bug 2: user-off + dark room stays off with no churn") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/true));
  p.SetUserOff(true);
  p.NoteOutputOff();
  // Dark room, user-off latched, off_when_dark on — every path says
  // stay off, policy emits kNone.
  CHECK(p.Evaluate(0.0f) == FrontlightAmbientAction::kNone);
  CHECK(p.Evaluate(0.5f) == FrontlightAmbientAction::kNone);
  CHECK(p.is_dark());
}

TEST_CASE("threshold-driven kOff leaves is_dark() false (controller routing)") {
  // The controller routes ambient kOff to either kAmbientOff (gated
  // by flAlwaysOn) or kDarkOff (bypasses flAlwaysOn) based on
  // is_dark() after Evaluate(). Pin the contract: a high-lux kOff
  // with off_when_dark enabled still reports is_dark()==false so the
  // controller treats it as the regular threshold path.
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, /*off_when_dark=*/true));
  p.NoteOutputOn();
  CHECK(p.Evaluate(500.0f) == FrontlightAmbientAction::kOff);
  CHECK(p.is_dark() == false);
}

TEST_CASE("Equal-to-threshold is a no-op (no flap at the exact boundary)") {
  FrontlightAmbientPolicy p;
  p.SetConfig(MakeCfg(128, false));
  p.NoteOutputOff();
  CHECK(p.Evaluate(128.0f) == FrontlightAmbientAction::kNone);
  p.NoteOutputOn();
  CHECK(p.Evaluate(128.0f) == FrontlightAmbientAction::kNone);
}
