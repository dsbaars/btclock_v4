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

// Under the WASM preview build the real EpdPanel/RefreshKind defs come
// from tools/wasm/wasm_panel.hpp (a shim the build script puts on the
// include path). On device we use the real driver header.
#ifdef BTCLOCK_WASM_BUILD
#include "wasm_panel.hpp"
#else
#include "epd_ssd1680.hpp"
#endif
#include "fonts_app.hpp"
#include "screens/screen_kind.hpp"

namespace btclock {

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
// Panel 0 = "FEE/RATE" split-text label, panels 1..N-2 = median-mempool-
// fee digits (sats/vB, right-justified; "X.YY" when fractional fits,
// integer otherwise — see fee_rate_layout.hpp for the full rule). Panel
// N-1 = "sat/vB" unit text. Pass `prev_fee_sats_vb < 0` to force a full
// refresh; otherwise pass the previously-rendered value so only digit
// panels whose glyph changed repaint. Pass `fee_sats_vb < 0` when no
// value has been received yet — the digit panels paint blank rather
// than '0'.
template <size_t N>
void RenderFeeRateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    double fee_sats_vb,
    double prev_fee_sats_vb = -1.0);

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
// Panel 0 = "BTC/SUPPLY" label. Mode selection matches the old-firmware
// `parseBitcoinSupply(bigChars, showPercentage)` branches, one char per
// panel (see test_datahandler_parity.cpp / old data_handler.cpp):
//   show_percent=true  → "93.48" spread over 5 panels + " % " trailing
//   show_percent=false, big_chars=true  → "19.9M" one char per panel
//   show_percent=false, big_chars=false → plain integer digits (legacy,
//     silently truncates on real mainnet — see btclock_v3_fci-33e for
//     the small-char 3-digit-group port).
// `prev_height == 0` forces a full refresh; the digit diff handles the
// low-frequency change rate.
template <size_t N>
void RenderBitcoinSupplyScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    uint32_t block_height,
    uint32_t prev_height = 0,
    bool big_chars = true,
    bool show_percent = false);

// --- Market cap (price × supply) ---
// Panel 0 = "<CCY>/MCAP" label. Mode selection mirrors old-firmware
// `parseMarketCap(..., bigChars)`:
//   big_chars=true  → "<sym><N.NN>T/B/M" one char per panel (e.g. "$1.02T")
//   big_chars=false → plain integer digits (legacy, silently truncates
//     for real mainnet; small-char 3-digit-group deferred via
//     btclock_v3_fci-33e).
// `prev_price.empty()` or `prev_height == 0` forces a full refresh.
template <size_t N>
void RenderMarketCapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    uint32_t block_height,
    const std::string& prev_price = "",
    uint32_t prev_height = 0,
    bool big_chars = true);

}  // namespace btclock
