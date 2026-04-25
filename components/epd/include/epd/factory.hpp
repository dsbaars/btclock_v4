// Compile-time driver dispatch. The factory keys off the
// BTCLOCK_PANEL_<X>=1 macro emitted by the top-level CMakeLists.txt;
// only the chosen driver's translation unit is referenced at link
// time (the other .cpps still compile so static analysis catches
// regressions, but their constructors aren't instantiated unless
// some unit-test path opts in).

#pragma once

#include <memory>

#include "epd/panel.hpp"

namespace btclock {
namespace epd {

// Allocate one driver instance for the panel selected at build time.
// Returns nullptr only on configurations that escaped the CMake panel
// validation — Init() on a real instance can still fail on hardware,
// but type selection itself can't.
std::unique_ptr<IEpdPanel> CreatePanel(const PanelConfig& cfg);

}  // namespace epd
}  // namespace btclock
