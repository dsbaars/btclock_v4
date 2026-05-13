// Boot-time loading spinner: a rotating MDI loading glyph painted on
// the middle panel between the splash and the first data render. Pre-
// renders the glyph alpha bitmap on the calling (main) task — font.cpp's
// glyph scratch buffer is single-threaded by contract, so the worker
// task only does framebuffer blits + DrawFramebufferStart on a panel
// it exclusively owns for the spinner's lifetime.

#pragma once

#include "app/app_ctx.hpp"

namespace btclock {

// Spawn the spinner task. Idempotent — calling twice is a no-op (the
// existing task keeps running). Safe to call from the main task right
// after the splash render; the spinner owns exactly one panel
// (kNumPanels/2) and no main-task code paints during its window.
void StartBootSpinner(AppCtx& ctx);

// Stop the spinner and block until the task has released its panel.
// Idempotent — safe to call when the spinner was never started. Both
// callers (DispatchBootPath's provisioning render and FinishWiringData
// Sources' first sm->Render) hit this before painting the middle
// panel themselves.
void StopBootSpinner();

}  // namespace btclock
