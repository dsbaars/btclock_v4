// Compatibility shim — the production code still spells
//   #include "epd_ssd1680.hpp"
// in dozens of TUs across main/, components/webserver, tools/wasm.
// The driver split moved everything under epd/, but rather than
// touch every call site this header re-exports the legacy class
// names so the renderers compile without churn.
//
// New code should #include "epd/panel.hpp" + "epd/factory.hpp"
// directly.

#pragma once

#include "epd/bus.hpp"
#include "epd/factory.hpp"
#include "epd/panel.hpp"

namespace btclock {

// `EpdPanel` was the only renderer-facing handle in the pre-split
// driver; alias it onto IEpdPanel so the existing
//   std::array<std::unique_ptr<EpdPanel>, N>
// declarations across main/screens/*.cpp keep working unchanged.
using EpdPanel = epd::IEpdPanel;

// Free functions kept at the legacy namespace level — same signatures
// as the original epd_ssd1680.hpp. These delegate to epd::SetGlobal*.
void EpdSetGlobalInverted(bool inverted);
bool EpdGetGlobalInverted();

}  // namespace btclock
