// STA-mode data screens.
//
// Two independent knobs drive every data renderer:
//
//   prev_value         — previously-rendered value for cell-diff.
//                        Sentinel (empty/0/-1) forces every cell to
//                        repaint this frame (cells don't get the "no
//                        change" short-circuit).
//   full_refresh_mode  — EPD refresh kind for this frame. True →
//                        RefreshKind::kFull (slow, ghost-clear), false
//                        → RefreshKind::kPartial (fast). The user's
//                        `refrScrnChange` + `fullRefreshMin` prefs are
//                        consumed by RefreshPolicy::Decide upstream;
//                        ScreenManager hands the result through here.
//
// Decoupling matters because screen transitions need every cell to
// repaint (cell-diff reset) but may only warrant a partial refresh
// (policy) — the previous coupling (deriving full_refresh from prev==
// sentinel) meant transitions always did a full, even when the policy
// wanted partial. See btclock_v4-jo6.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Under the WASM preview build the real EpdPanel/RefreshKind defs come
// from tools/wasm/wasm_panel.hpp (a shim the build script puts on the
// include path). On device we use the real driver header.
#ifdef BTCLOCK_WASM_BUILD
#include "wasm_panel.hpp"
#else
#include "epd_ssd1680.hpp"
#endif
#include "data_core/snapshot.hpp"
#include "fonts_app.hpp"
#include "data_core/snapshot.hpp"
#include "screens/screen_kind.hpp"

namespace btclock {

// --- Block height ---
// Panel 0 = "BLOCK/HEIGHT" split-text label, panels 1..N-1 = one digit.
// `vertical_desc=true` rotates the label panel 90° CCW so the text reads
// along the longer physical edge of the panel (ports the v3 verticalDesc
// pref — see btclock_v3_fci/src/lib/drivers/epd/epd.cpp splitText).
template <size_t N>
void RenderBlockHeightScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    uint32_t block_height,
    uint32_t prev_height = 0,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

// --- Moscow time (sats/USD as HH:MM-style digits) ---
// `price` and `prev_price` are the raw price strings from the data
// source (e.g. "64211.53"). Pass an empty `prev_price` to force full
// refresh. `currency` is the label suffix: "USD" → "MSCW/TIME" panel 0
// label when valid Moscow-time range; other currencies get "SATS/<CCY>"
// (e.g. "SATS/EUR"). `sats_variant` selects one of the 16 glyphs in
// the Satoshi Symbol font (0..15; default 7).
// `use_sats_symbol=false` suppresses the sats-glyph cell. `use_mscw_time
// =false` forces the label to SATS/<CCY> even for USD in the classic
// Moscow-time range. Defaults keep the documented legacy layout.
template <size_t N>
void RenderMoscowTimeScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    const std::string& prev_price = "",
    uint8_t sats_variant = kSatsVariantDefault,
    bool use_sats_symbol = true,
    bool use_mscw_time = true,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

// --- BTC price ---
// Panel 0 = "BTC/<CCY>" label (or "MOW/UNITS" when `mow_mode` is on and
// the MOW form fits with a label slot; blank on the suffix-overflow
// path where priceString fills all panels). Panels 1..N-1 = price
// digits with an optional currency-symbol glyph placed one slot before
// the first digit. Symbol is a UTF-8 string; pass "" to skip the
// symbol panel.
//
// `suffix_price=true` routes through FormatNumberWithSuffix so huge
// prices compress to "$78.3K" / "$1.02M" — ports v3's parsePriceData
// useSuffixFormat branch. `mow_mode=true` forces the M suffix (MOW
// units): 78280 → "$0.078M", 1_000_000 → "$1.000M". v3 precedence:
// `mow_mode` without `suffix_price` is ignored for short integer prices
// — the suffix branch only fires when `suffix_price=true` or the
// integer price itself is wide enough (digit count >= N).
template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& currency,
    const std::string& price,
    const std::string& prev_price = "",
    const char* symbol_utf8 = "",
    bool suffix_price = false,
    bool mow_mode = false,
    bool share_dot = false,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

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
    double prev_fee_sats_vb = -1.0,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

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
    int prev_mday, int prev_month,
    bool full_refresh_mode = true,
    bool vertical_desc = false,
    bool hide_leading_zero = false);

// --- Halving countdown ---
// `as_blocks=true` (default) paints the blocks-remaining form: panel 0 =
// "HAL/VING", remaining panels = blocks right-justified.
// `as_blocks=false` paints the time-breakdown form: "BIT/COIN|HAL/VING"
// header + "N/YRS", "N/DAYS", "N/HRS", "N/MINS" per-panel labels +
// "TO/GO" terminator. Matches old-firmware
// lib/btclock/data_handler.cpp::parseHalvingCountdown(asBlocks).
// `prev_height == 0` forces a full refresh.
template <size_t N>
void RenderHalvingScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    uint32_t block_height,
    uint32_t prev_height = 0,
    bool as_blocks = true,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

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
    bool show_percent = false,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

// --- Mining pool hashrate ---
// Panels 0+1 = pool identity area: a vendored 1-bpp logo bitmap painted
// on panel 0 when pool_logos::Lookup(pool.name) matches, or the pool
// name split across the pair as a text fallback (first word on panel 0,
// second word on panel 1; single-word names leave panel 1 blank).
// Trailing panel = unit label (e.g. "PH/S"). Panels 2..N-2 carry the
// hashrate digits right-justified. Pass an empty `prev_pool` (i.e.
// default-constructed PoolStats) to force a full refresh.
template <size_t N>
void RenderMiningPoolHashrateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const DataSnapshot::PoolStats& pool,
    const DataSnapshot::PoolStats& prev_pool,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

// --- Mining pool earnings (daily sats) ---
// Same pool-identity layout as the hashrate screen (panels 0+1 = logo
// or name-split, panel N-1 = "SATS"/"BTC" unit label). Panels 2..N-2
// carry the formatted sats/day right-justified with the same suffix
// compression the old firmware used (K/M/BTC). Pass an empty
// `prev_pool` to force a full refresh; the renderer short-circuits
// paint when the pool has no daily_sats.
template <size_t N>
void RenderMiningPoolEarningsScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const DataSnapshot::PoolStats& pool,
    const DataSnapshot::PoolStats& prev_pool,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

// --- Runtime-pushed custom content ---
// Renders the string in `cells[i]` centered on panel i for i in [0, N).
// A cell containing a single '/' is drawn via DrawSplitText (top/bottom
// halves) to mirror the old firmware's split-text dispatch for tokens
// like "BLOCK/HEIGHT". Everything else is centered on a single line
// with an auto-fit pixel size — larger fonts for single-character
// cells, smaller fonts for multi-word labels — matching the old
// EPDManager::showChars / showDigit / renderText dispatch in spirit
// without porting the full QR + MDI icon stack (tracked separately;
// see CustomScreen follow-ups below).
//
// Callers pass `cells.size() == N`. Missing cells (empty string) paint
// as a blank panel. A first paint of this screen always does a full
// refresh; subsequent paints diff against `prev_cells` and only repaint
// panels whose string changed.
template <size_t N>
void RenderCustomScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::array<std::string, N>& cells,
    const std::array<std::string, N>& prev_cells,
    bool cell_diff_reset,
    bool full_refresh_mode);

// --- Diagnostic screen ---
// Off-rotation debug screen driven by the on-device button 4. Shows
// IP / SSID / free heap + PSRAM / HW variant / build date / uptime so
// the user can read live network + memory state without the WebUI.
// Always full-refresh — there's no diff state; the caller ensures it
// only paints on entry / value change.
struct DebugScreenInfo {
  std::string ip;            // dotted-quad, or "no link"
  std::string ssid;          // stored STA ssid, or "" if none
  uint32_t free_heap = 0;    // bytes, MALLOC_CAP_INTERNAL
  uint32_t free_psram = 0;   // bytes, MALLOC_CAP_SPIRAM
  const char* hw_name = "";  // board::kHardwareName
  const char* built = "";    // __DATE__ from the build
  uint32_t uptime_s = 0;     // seconds since boot
};
template <size_t N>
void RenderDebugScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const DebugScreenInfo& info);

// --- Bitaxe hashrate ---
// Panel 0 = pickaxe MDI icon, panels 1..N-1 = hashrate value string
// one codepoint per cell, right-aligned. Empty `hostname` OR
// `!hashrate_ghs` paints "OFFLINE" across the tail so the user can tell
// an unconfigured screen from a momentarily-zero sample.
// `force_full=true` or empty `prev_value` triggers a full EPD refresh
// (digit diff is all-or-nothing for the tail — bitaxe poll cadence is
// slow enough that a per-cell diff wouldn't buy visible latency).
template <size_t N>
void RenderBitaxeHashrateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& hostname,
    const std::optional<double>& hashrate_ghs,
    bool full_refresh_mode,
    const std::string& prev_value,
    bool vertical_desc = false);

// --- Bitaxe best-share difficulty ---
// Panel 0 = rocket-launch MDI icon, panels 1..N-1 = best-diff string
// (as returned by the AxeOS API, e.g. "15.6M") one codepoint per cell.
// Same OFFLINE and refresh semantics as the hashrate screen above.
template <size_t N>
void RenderBitaxeBestDiffScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& hostname,
    const std::optional<std::string>& best_diff,
    bool full_refresh_mode,
    const std::string& prev_value,
    bool vertical_desc = false);

// --- Nostr zap notification ---
// Transient overlay painted when a NIP-57 zap receipt arrives. Layout:
// [ZAP][bolt][...blanks...][sats glyph][amount...] on every variant —
// ZAP and the bolt anchor at the leftmost two cells; V8's extra panel
// widens the blank gap between bolt and sats glyph. The bolt cell paints the
// mdi-lightning-bolt MDI glyph (icon font role, 130 px). The amount
// is scaled via FormatZapAmount ("21k", "1.2M", "100") and
// right-justified, anchored to panel N-1; longer amounts spill
// leftward through the blank middle cells. When `use_sats_symbol=true`
// (default) the sats glyph cell sits one slot before the
// most-significant amount digit (kSatsGlyph + sats_glyph font role);
// when off the cell stays blank and the amount may use one extra
// tail cell. `sats_variant` selects one of the 16 Satoshi-symbol
// glyphs (0..15; default 7). Always full-refresh — the screen is only
// up for a few seconds before ScreenManager restores the prior slot,
// so diff bookkeeping would add no value.
template <size_t N>
void RenderNostrZapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const DataSnapshot::LatestZap& zap,
    bool use_sats_symbol = false,
    uint8_t sats_variant = kSatsVariantDefault,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

// Format a zap amount (sats) into the string painted on the trailing
// panels. `max_int_cells` is the panel-tail budget for an integer
// rendering — when the raw integer fits, it's preferred over the
// suffix form so a 1000-sat zap on a 7-panel board reads "1000" not
// "1.0k". When it doesn't fit, falls back to k / M / B suffix:
// 1000..999_999 → "Nk" / "N.Nk", >= 1_000_000 → "NM" / "N.NM",
// >= 1_000_000_000 → "NB". Negative / missing → "?". Pure-logic
// helper so host tests can pin the rules without pulling the EPD
// driver.
std::string FormatZapAmount(const std::optional<int64_t>& amount_sats,
                            std::size_t max_int_cells);

// --- Firmware OTA overlay ---
// Painted once by the /upload/firmware handler before esp_ota_begin
// erases the target partition. Panel 0 shows an "UP/DATE" split-text
// label; remaining panels carry the same "UPDATE!" label so the user
// can tell from any viewing angle that a flash is in progress. Always
// full-refresh — only painted once per OTA, and the content never
// changes during the write (the LED strip carries the progress
// indication). After the render returns the HTTP worker blocks in the
// flash-write loop for ~15 s; the main render loop's data-screen path
// short-circuits via ScreenManager::IsOtaActive() so a concurrent
// data push can't stomp the overlay.
template <size_t N>
void RenderOtaUpdateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts);

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
    bool big_chars = true,
    bool full_refresh_mode = true,
    bool vertical_desc = false);

}  // namespace btclock
