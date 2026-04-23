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
  kBlockFeeRate,
  kClock,
  kHalving,
  kBitcoinSupply,
  kMarketCap,
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
// Panel 0 = "BTC/<CCY>" label, panels 1..N-1 = price digits with an
// optional currency-symbol glyph placed one slot before the first digit
// (same layout pattern as the sats glyph on Moscow time). Symbol is a
// UTF-8 string; pass "" to skip the symbol panel. Right-justified
// integer part — fractional precision is tracked in beads lx0.12.
template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    const std::string& prev_price = "",
    const char* symbol_utf8 = "");

// --- Block fee rate ---
// Panel 0 = "FEE/RATE" split-text label, panels 1..N-1 = integer
// median-mempool-fee digits (sats/vB), right-justified. Pass
// `prev_fee_sats_vb = -1` to force a full refresh; otherwise pass the
// previously-rendered integer so only the digit panels whose glyph
// changed get a partial refresh. Pass `fee_sats_vb = -1` when no value
// has been received yet — the digit panels paint blank rather than '0'.
// The old firmware also renders a "sat/vB" unit on the last panel; we
// skip that here (no unit glyph yet; see fee_rate_layout.hpp).
template <size_t N>
void RenderFeeRateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    int32_t fee_sats_vb,
    int32_t prev_fee_sats_vb = -1);

// --- Wall-clock HH:MM ---
// Panel 0 = dd/mm date label (split-text). Remaining panels carry
// "HH:MM" right-justified with a ':' separator panel. `valid` is
// false until SNTP produces a reasonable epoch — renderer then
// blanks the time area instead of showing bogus 1970 values.
// `prev_*` arguments drive the partial-refresh diff; pass
// `prev_valid=false` to force a full refresh.
template <size_t N>
void RenderClockScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    bool valid, int hour, int minute, int mday, int month,
    bool prev_valid, int prev_hour, int prev_minute,
    int prev_mday, int prev_month);

// --- Halving countdown (blocks-remaining mode) ---
// Panel 0 = "HAL/VING" label. Remaining panels = blocks remaining
// until the next halving, right-justified. `prev_height == 0` forces
// a full refresh.
template <size_t N>
void RenderHalvingScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    uint32_t block_height,
    uint32_t prev_height = 0);

// --- Bitcoin circulating supply ---
// Panel 0 = "BTC/SUPPLY" label. Remaining panels = integer BTC supply,
// right-justified. Capped at 21,000,000. `prev_height == 0` forces a
// full refresh; the digit diff handles the low-frequency change rate.
template <size_t N>
void RenderBitcoinSupplyScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    uint32_t block_height,
    uint32_t prev_height = 0);

// --- Market cap (price × supply) ---
// Panel 0 = "<CCY>/MCAP" label. Remaining panels = integer market cap
// (no suffix scaling — full digit string, right-justified, truncated
// from the left if it exceeds the panel count). `prev_price.empty()`
// or `prev_height == 0` forces a full refresh.
template <size_t N>
void RenderMarketCapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    uint32_t block_height,
    const std::string& prev_price = "",
    uint32_t prev_height = 0);

}  // namespace btclock
