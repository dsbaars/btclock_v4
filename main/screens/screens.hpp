// STA-mode data screens.
//
// All screen renderers follow the same per-digit diff pattern: pass
// empty `prev_*` state on first paint to force a full refresh; on
// subsequent paints pass the previously-rendered value so only the
// digit panels whose glyph actually changed get a partial refresh
// (~800 ms vs ~2.4 s full, label panel untouched after first paint).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"

namespace btclock {

// Which top-level screen is currently being displayed. Screen rotation
// and button navigation cycle through these.
enum class ScreenType : uint8_t {
  kBlockHeight,
  kMoscowTime,
  kBtcPrice,
};

// --- Block height ---
// Panel 0 = "BLOCK/HEIGHT" split-text label, panels 1..N-1 = one digit.
template <size_t N>
void RenderBlockHeightScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    uint32_t block_height,
    uint32_t prev_height = 0);

// --- Moscow time (sats/USD as HH:MM-style digits) ---
// `price` and `prev_price` are the raw price strings from the data
// source (e.g. "64211.53"). Pass an empty `prev_price` to force full
// refresh. `currency` is the label suffix: "USD" → "MSCW/TIME" panel 0
// label when valid Moscow-time range; other currencies get "SATS/<CCY>"
// (e.g. "SATS/EUR"). `sats_variant` selects one of the 16 glyphs in
// the Satoshi Symbol font (0..15; default 7).
template <size_t N>
void RenderMoscowTimeScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    const std::string& prev_price = "",
    uint8_t sats_variant = kSatsVariantDefault);

// --- BTC price ---
// Panel 0 = "BTC/<CCY>" label, panels 1..N-1 = price digits with a
// decimal point placed to fit. Right-justified integer part.
template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    const std::string& prev_price = "");

}  // namespace btclock
