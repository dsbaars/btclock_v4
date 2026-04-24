// ScreenManager construction + NVS-driven per-user tweaks.
//
// Builds ctx.sm from the compile-time currency list, restores the
// sats-glyph variant from NVS, and installs the runtime skip predicate
// that hides the mining-pool earnings slot on solo pools (which have
// no per-user payout to render). Button queue and main-task handle are
// also prepared here because they need to exist before the first
// render and before the data hub sets up its on-update callback.
//
// AppCtx fields populated: sm, currencies, button_q, main_task.

#pragma once

namespace btclock {

struct AppCtx;

void InitScreenManager(AppCtx& ctx);

}  // namespace btclock
