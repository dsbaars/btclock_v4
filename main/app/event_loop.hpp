// Main-task render + event pump.
//
// Three wake sources: data-push notify (from DataHub SetOnUpdate),
// button event (ButtonReader queue), 1 s heartbeat tick. One action
// per pass — command drain, button drain, zap-notify dispatch,
// auto-rotate, data re-render — so the loop never compounds unrelated
// side-effects in a single iteration.
//
// Never returns. Called once from app_main after every init_* TU has
// completed.

#pragma once

namespace btclock {

struct AppCtx;

[[noreturn]] void RunEventLoop(AppCtx& ctx);

}  // namespace btclock
