// Host tests for DataHub::Report dedupe + ForceNotify bypass.
//
// Background — btclock_v4-et8: Rev B was observed stuck on a stale block
// height for ~22 blocks while `connectionStatus.blocks: true`. The IDF
// WS stack had silently dropped + reconnected without firing
// DISCONNECTED, so the source-side `connected_` flag stayed `true`.
// When the server's first frame after re-subscribe carried the same
// height as the cached snapshot (no new block mined during the silent
// gap), DataSnapshot::Merge short-circuited, on_update_ never fired,
// and the renderer stayed on the stale frame. ForceNotify exists so
// every (re-)connect path can explicitly re-engage the renderer.

#include <cstdint>
#include <string>

#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "doctest.h"

using btclock::DataHub;
using btclock::DataSnapshot;

TEST_CASE("Report fires on_update for the first non-empty snapshot") {
  DataHub hub;
  int callbacks = 0;
  uint32_t last_height = 0;
  hub.SetOnUpdate([&](const DataSnapshot& s) {
    ++callbacks;
    if (s.block_height) last_height = *s.block_height;
  });

  DataSnapshot partial;
  partial.block_height = 900'000;
  hub.Report(partial);

  CHECK(callbacks == 1);
  CHECK(last_height == 900'000);
}

TEST_CASE("Report dedupe: same height does NOT fire on_update again") {
  // The dedupe-by-equality in DataSnapshot::Merge is correct in the
  // common case — it prevents spurious renders from heartbeat frames
  // that re-publish the current tip. The bug being defended against
  // is the *reconnect* path, where this dedupe combined with a stale
  // last_rendered_height_ leaves the screen frozen.
  DataHub hub;
  int callbacks = 0;
  hub.SetOnUpdate([&](const DataSnapshot&) { ++callbacks; });

  DataSnapshot partial;
  partial.block_height = 900'000;
  hub.Report(partial);  // first → fires
  hub.Report(partial);  // same height → suppressed
  hub.Report(partial);  // still same → suppressed
  CHECK(callbacks == 1);
}

TEST_CASE("Report fires on_update when height advances") {
  DataHub hub;
  int callbacks = 0;
  hub.SetOnUpdate([&](const DataSnapshot&) { ++callbacks; });

  DataSnapshot a;
  a.block_height = 900'000;
  hub.Report(a);
  DataSnapshot b;
  b.block_height = 900'001;
  hub.Report(b);
  CHECK(callbacks == 2);
}

TEST_CASE("ForceNotify fires on_update with current snapshot, no Merge") {
  DataHub hub;
  int callbacks = 0;
  uint32_t height_seen = 0;
  hub.SetOnUpdate([&](const DataSnapshot& s) {
    ++callbacks;
    if (s.block_height) height_seen = *s.block_height;
  });

  // Seed the snapshot through the normal Report path.
  DataSnapshot partial;
  partial.block_height = 900'000;
  hub.Report(partial);
  CHECK(callbacks == 1);

  // The reconnect-scenario: server re-publishes the same height we
  // already cached. Report's Merge returns false → no notify.
  hub.Report(partial);
  CHECK(callbacks == 1);

  // ForceNotify must fire the callback anyway, with the current
  // (unchanged) snapshot — this is what wakes the renderer on
  // (re-)connect even when no new block arrived.
  hub.ForceNotify();
  CHECK(callbacks == 2);
  CHECK(height_seen == 900'000);
}

TEST_CASE("ForceNotify on an empty hub fires with an empty snapshot") {
  // Hub starts with no fields populated. ForceNotify before any
  // Report still invokes the callback so callers don't need to gate
  // on "have we ever Reported?" before re-engaging the pipeline.
  DataHub hub;
  int callbacks = 0;
  bool saw_height = true;
  hub.SetOnUpdate([&](const DataSnapshot& s) {
    ++callbacks;
    saw_height = s.block_height.has_value();
  });
  hub.ForceNotify();
  CHECK(callbacks == 1);
  CHECK_FALSE(saw_height);
}

TEST_CASE("ForceNotify with no callback registered is a no-op") {
  // No registered callback → no crash, no work. Exercises the
  // `if (cb) cb(copy)` guard in the implementation.
  DataHub hub;
  hub.ForceNotify();
  // If we get here without UB the contract holds.
  CHECK(true);
}

TEST_CASE("ForceNotify after multiple Reports surfaces the latest snapshot") {
  DataHub hub;
  DataSnapshot last_seen;
  hub.SetOnUpdate([&](const DataSnapshot& s) { last_seen = s; });

  DataSnapshot a;
  a.block_height = 900'000;
  a.block_fee = 5;
  hub.Report(a);
  DataSnapshot b;
  b.block_height = 900'005;
  b.block_fee = 9;
  hub.Report(b);
  // Move-overwrite the seen-by-callback to prove ForceNotify
  // produces its own delivery (not relying on prior callbacks).
  last_seen = DataSnapshot{};
  hub.ForceNotify();
  // value_or sidesteps clang-tidy's bugprone-unchecked-optional-access
  // — the checker treats both operator* and .value() as unchecked
  // whenever a has_value() guard isn't on the same control-flow path,
  // and CHECK()/REQUIRE() macro borders are opaque to it. With
  // value_or(0), an unexpectedly empty optional renders the CHECK as
  // `0 == 900'005` which fails as loudly as the bare-deref form would.
  CHECK(last_seen.block_height.value_or(0) == 900'005);
  CHECK(last_seen.block_fee.value_or(0) == 9);
}

TEST_CASE("SetOnUpdate({}) clears the callback") {
  // Defensive — after a teardown the hub must not retain a dangling
  // callback. ForceNotify after clear is a no-op.
  DataHub hub;
  int callbacks = 0;
  hub.SetOnUpdate([&](const DataSnapshot&) { ++callbacks; });
  hub.ForceNotify();
  CHECK(callbacks == 1);

  hub.SetOnUpdate({});
  hub.ForceNotify();
  CHECK(callbacks == 1);  // unchanged
}
