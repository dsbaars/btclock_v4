// Drain step for the main event loop's ControlServer command queue.
//
// Extracted from event_loop.cpp so the loop body stays focused on the
// wake-source priority ordering (commands -> buttons -> heartbeat ->
// debug -> zap -> rotate -> new-block -> general render). The drain
// owns the per-command dispatch, the optional re-render that follows a
// state-mutating command, and the post-step status publish.
//
// Ordering invariants the caller depends on:
//   - All queued commands drain in one pass. A single settings PATCH
//     from the WebUI typically posts several commands (kSetFont +
//     kSetTimezone + on_inverted_color_changed's MarkDirty, sometimes
//     more); rendering once per command would mean N sequential full-
//     refreshes for one user save. Coalesces correctly because the
//     state mutations commute (each touches a different field).
//   - State mutations stay on the main task: ScreenManager,
//     BtclockDataSource, AppFonts, libc tz globals.
//
// Returns true if at least one command was handled. Caller should
// `continue` the iteration so unrelated wake sources don't compound
// side-effects in the same pass.

#pragma once

namespace btclock {

struct AppCtx;

bool DrainControlCommands(AppCtx& ctx);

}  // namespace btclock
