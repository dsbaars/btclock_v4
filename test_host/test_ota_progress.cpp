// Host tests for the pure-logic OTA progress helpers.
//
// ProgressFractionToLedCount pins the 4-LED bar mapping from an upload
// fraction; the callback wiring in init_control_api.cpp translates the
// (written, total) pair into a fraction then into an LED count, so
// both helpers need independent coverage.

#include "doctest.h"

#include "ota_progress.hpp"

TEST_CASE("ProgressFractionToLedCount boundaries") {
  // Zero / negative / NaN → "at least 1 pixel", matches the UX intent:
  // as soon as an upload has been accepted the user sees a lit LED.
  CHECK(btclock::ProgressFractionToLedCount(0.0f) == 1);
  CHECK(btclock::ProgressFractionToLedCount(-0.1f) == 1);
  // Quarter boundaries — inclusive on the lower side of each step.
  CHECK(btclock::ProgressFractionToLedCount(0.24f) == 1);
  CHECK(btclock::ProgressFractionToLedCount(0.25f) == 2);
  CHECK(btclock::ProgressFractionToLedCount(0.49f) == 2);
  CHECK(btclock::ProgressFractionToLedCount(0.50f) == 3);
  CHECK(btclock::ProgressFractionToLedCount(0.74f) == 3);
  CHECK(btclock::ProgressFractionToLedCount(0.75f) == 4);
  CHECK(btclock::ProgressFractionToLedCount(0.99f) == 4);
  CHECK(btclock::ProgressFractionToLedCount(1.0f) == 4);
  // Overshoot clamps to 4 — a misbehaving caller that reports 150 %
  // still lands on a valid strip index.
  CHECK(btclock::ProgressFractionToLedCount(1.5f) == 4);
}

TEST_CASE("ProgressFraction — Content-Length missing fallback") {
  // total == 0 signals a missing Content-Length. The LED path uses
  // this to pick the indeterminate indicator rather than a concrete
  // bar position; we return 0 so callers can test `total==0` directly
  // OR check `fraction == 0`.
  CHECK(btclock::ProgressFraction(0, 0) == 0.0f);
  CHECK(btclock::ProgressFraction(1024, 0) == 0.0f);
  CHECK(btclock::ProgressFraction(99999999, 0) == 0.0f);
}

TEST_CASE("ProgressFraction — happy path with known total") {
  // Quarter steps over a 4 KiB image.
  constexpr std::size_t total = 4096;
  CHECK(btclock::ProgressFraction(0, total) == doctest::Approx(0.0f));
  CHECK(btclock::ProgressFraction(1024, total) == doctest::Approx(0.25f));
  CHECK(btclock::ProgressFraction(2048, total) == doctest::Approx(0.5f));
  CHECK(btclock::ProgressFraction(3072, total) == doctest::Approx(0.75f));
  CHECK(btclock::ProgressFraction(4096, total) == doctest::Approx(1.0f));
}

TEST_CASE("ProgressFraction — clamps overrun") {
  // Some transports (unreliable proxies) can replay bytes past the
  // declared Content-Length. We clamp to 1.0 so the LED bar lands on
  // "all four green" rather than wrapping to a lower count.
  CHECK(btclock::ProgressFraction(5000, 4096) == doctest::Approx(1.0f));
  CHECK(btclock::ProgressFraction(1u << 24, 1024) == doctest::Approx(1.0f));
}

TEST_CASE("End-to-end: bytes → LED count for a 1.5 MiB image") {
  // Cross-check the two helpers on a typical image size (matches the
  // btclock firmware binary roughly). Each checkpoint should land on
  // the documented step of the 4-LED bar.
  constexpr std::size_t total = 1536u * 1024u;
  auto count_at = [](std::size_t written, std::size_t total) {
    return btclock::ProgressFractionToLedCount(
        btclock::ProgressFraction(written, total));
  };
  CHECK(count_at(0, total) == 1);
  CHECK(count_at(total / 4 - 1, total) == 1);
  CHECK(count_at(total / 4, total) == 2);
  CHECK(count_at(total / 2 - 1, total) == 2);
  CHECK(count_at(total / 2, total) == 3);
  CHECK(count_at(3 * total / 4 - 1, total) == 3);
  CHECK(count_at(3 * total / 4, total) == 4);
  CHECK(count_at(total, total) == 4);
}

TEST_CASE("OtaProgress::Phase happy path + failure transitions") {
  // Phase transitions don't have runtime-enforced ordering (the type is
  // a plain enum), so these checks pin the documented lifecycle: the
  // kStarting → kWriting → kVerifying → kRebooting chain for a
  // successful upload and the kStarting → kFailed short-circuit for
  // errors. The host test serves as documentation-by-example so a
  // future refactor that renames or reorders a phase shows up here.
  btclock::OtaProgress p;
  CHECK(p.phase == btclock::OtaProgress::Phase::kStarting);
  p.phase = btclock::OtaProgress::Phase::kWriting;
  CHECK(p.phase == btclock::OtaProgress::Phase::kWriting);
  p.phase = btclock::OtaProgress::Phase::kVerifying;
  CHECK(p.phase == btclock::OtaProgress::Phase::kVerifying);
  p.phase = btclock::OtaProgress::Phase::kRebooting;
  CHECK(p.phase == btclock::OtaProgress::Phase::kRebooting);
  // Error exit from the starting phase (partition lookup fail, oversize,
  // etc.). The progress callback must see kFailed with written==0.
  btclock::OtaProgress err;
  err.phase = btclock::OtaProgress::Phase::kFailed;
  err.written = 0;
  CHECK(err.phase == btclock::OtaProgress::Phase::kFailed);
  CHECK(err.written == 0u);
}
