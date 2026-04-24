// SSD1680 SPI bus + per-panel construction + splash render.
//
// Wires up the EPD SPI bus (SPI2_HOST), constructs kNumPanels panels
// using the board's pin map, and paints the boot-time BTCLOCK splash
// so it's obvious all panels are alive before the main loop spins up.
// The `make_pin` helper closes over ctx.mcp and ctx.mcp2 — both must
// have been populated by InitHardware prior to calling this TU.

#pragma once

namespace btclock {

struct AppCtx;

void InitPanelsAndSplash(AppCtx& ctx);

}  // namespace btclock
