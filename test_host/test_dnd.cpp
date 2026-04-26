// Host tests for the DND window algebra + top-level ComputeDndActive.
// Pure-logic — no NVS, no wall-clock. Mirrors the contract from
// lib/btclock/dnd_window.cpp (old firmware) so a schedule that was
// "active" in production stays "active" after the IDF port.

#include "dnd/dnd_window.hpp"
#include "doctest.h"

using btclock::dnd::ComputeDndActive;
using btclock::dnd::IsTimeInWindow;

// -------------------------------------------------------------------
// Same-day windows
// -------------------------------------------------------------------

TEST_CASE("Same-day window: inside range is active") {
  // 22:00 -> 23:00 window, probe at 22:30.
  CHECK(IsTimeInWindow(22, 30, 22, 0, 23, 0));
}

TEST_CASE("Same-day window: start boundary is inclusive") {
  CHECK(IsTimeInWindow(22, 0, 22, 0, 23, 0));
}

TEST_CASE("Same-day window: end boundary is exclusive") {
  CHECK_FALSE(IsTimeInWindow(23, 0, 22, 0, 23, 0));
}

TEST_CASE("Same-day window: outside range is inactive") {
  CHECK_FALSE(IsTimeInWindow(10, 0, 22, 0, 23, 0));
  CHECK_FALSE(IsTimeInWindow(23, 1, 22, 0, 23, 0));
}

// -------------------------------------------------------------------
// Midnight-wrap windows
// -------------------------------------------------------------------

TEST_CASE("Overnight window: before midnight is active") {
  // 22:30 -> 07:00 window.
  CHECK(IsTimeInWindow(22, 31, 22, 30, 7, 0));
}

TEST_CASE("Overnight window: midnight is active") {
  CHECK(IsTimeInWindow(0, 0, 22, 30, 7, 0));
}

TEST_CASE("Overnight window: after midnight is active") {
  CHECK(IsTimeInWindow(6, 59, 22, 30, 7, 0));
}

TEST_CASE("Overnight window: end boundary remains exclusive") {
  CHECK_FALSE(IsTimeInWindow(7, 0, 22, 30, 7, 0));
}

TEST_CASE("Overnight window: well outside is inactive") {
  CHECK_FALSE(IsTimeInWindow(12, 0, 22, 30, 7, 0));
}

// -------------------------------------------------------------------
// Degenerate cases
// -------------------------------------------------------------------

TEST_CASE("Degenerate: start == end is always empty") {
  // Pinned as "never active" so users can't brick themselves into
  // permanent DND by picking the same start and end.
  CHECK_FALSE(IsTimeInWindow(12, 0, 12, 0, 12, 0));
  CHECK_FALSE(IsTimeInWindow(0, 0, 12, 0, 12, 0));
  CHECK_FALSE(IsTimeInWindow(23, 59, 12, 0, 12, 0));
}

TEST_CASE("Minimal one-minute window") {
  CHECK(IsTimeInWindow(12, 0, 12, 0, 12, 1));
  CHECK_FALSE(IsTimeInWindow(12, 1, 12, 0, 12, 1));
  CHECK_FALSE(IsTimeInWindow(11, 59, 12, 0, 12, 1));
}

TEST_CASE("Full-day-minus-one window") {
  CHECK(IsTimeInWindow(0, 0, 0, 0, 23, 59));
  CHECK(IsTimeInWindow(12, 0, 0, 0, 23, 59));
  CHECK_FALSE(IsTimeInWindow(23, 59, 0, 0, 23, 59));
}

// -------------------------------------------------------------------
// ComputeDndActive — precedence of the master flags
// -------------------------------------------------------------------

TEST_CASE(
    "ComputeDndActive: neither flag set -> inactive regardless of clock") {
  // Inside the schedule window but scheduling is off.
  CHECK_FALSE(ComputeDndActive(22, 30, 22, 0, 23, 0, false, false));
  // Outside the window too.
  CHECK_FALSE(ComputeDndActive(10, 0, 22, 0, 23, 0, false, false));
}

TEST_CASE("ComputeDndActive: manual override wins over an inactive schedule") {
  // Manual flag on, time_enabled off, clock outside any window -> still active.
  CHECK(ComputeDndActive(10, 0, 22, 0, 23, 0, true, false));
}

TEST_CASE(
    "ComputeDndActive: manual override wins even when schedule disagrees") {
  // Manual on, time_enabled on, clock outside window -> still active.
  CHECK(ComputeDndActive(10, 0, 22, 0, 23, 0, true, true));
}

TEST_CASE("ComputeDndActive: time_enabled only consults the schedule") {
  CHECK(ComputeDndActive(22, 30, 22, 0, 23, 0, false, true));
  CHECK_FALSE(ComputeDndActive(10, 0, 22, 0, 23, 0, false, true));
}

TEST_CASE("ComputeDndActive: boundaries reflect IsTimeInWindow") {
  // Start inclusive, end exclusive — carries through the dispatch.
  CHECK(ComputeDndActive(22, 0, 22, 0, 23, 0, false, true));
  CHECK_FALSE(ComputeDndActive(23, 0, 22, 0, 23, 0, false, true));
}

TEST_CASE("ComputeDndActive: overnight schedule stays armed across midnight") {
  CHECK(ComputeDndActive(23, 0, 22, 30, 7, 0, false, true));
  CHECK(ComputeDndActive(3, 15, 22, 30, 7, 0, false, true));
  CHECK_FALSE(ComputeDndActive(7, 0, 22, 30, 7, 0, false, true));
}
