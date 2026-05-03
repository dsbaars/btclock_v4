// Runtime catalogues the WebUI reads from GET /api/settings so its
// drop-downs can list every option this firmware build supports.
//
// All three arrays here are `constexpr` so adding or removing an entry
// is a rebuild — there is no NVS state that could go stale. The
// settings handler already has `std::vector<std::string>` slots for
// each (ControlServer::Config), so we copy the catalogue into those at
// startup rather than changing the pure-logic API's types.
//
// Source-of-truth rules:
//   - `kAvailableFonts`: stays in lock-step with AppFonts in
//     main/fonts_app.hpp. Adding a font means bundling the TTF, plumbing
//     it through AppFonts, *and* appending its id here so the WebUI's
//     font picker can select it. The ids are intentionally lowercase and
//     stable — the WebUI keys its i18n lookup off `fonts.<id>`.
//   - `kAvailableCurrencies`: codes the BTC-price / Moscow-time data
//     source can render. Keep in sync with the currency set the
//     DataHub / price websocket actually populates (main.cpp passes
//     the active subset into ScreenManager).
//   - `kScreenKinds`: derived at compile time from BTCLOCK_SCREEN_KIND_LIST
//     in main/screens/screen_kind.hpp so the enum and the API catalogue
//     can't drift. Only user-rotatable screens live in the list; boot
//     and provisioning UIs are intentionally excluded.

#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "screens/screen_kind.hpp"

namespace btclock {
namespace catalogs {

// Font ids the WebUI shows in its font picker. Plain string array —
// WebUI expects `availableFonts: string[]` and does its own label
// lookup via the `m['fonts.<id>']` translation table. Order matches
// FontFamily's numeric values so WASM/IDF dropdowns and the WebUI line
// up by index without a separate mapping table.
inline constexpr std::array<std::string_view, 9> kAvailableFonts = {
    "antonio",      "oswald",          "inter",       "sourceSerif",
    "merriweather", "bitter",          "atkinson",    "antonioSemiBold",
    "antonioBold",
};

// ISO 4217 codes the price / moscow-time / market-cap screens can
// render. Must stay in lock-step with the upstream price websocket
// (ws.btclock.dev/api/v2/currencies) — advertising codes the backend
// doesn't actually publish leaves the WebUI's picker showing options
// that never produce a price tick. settings_api's GET drops any legacy
// `actCurrencies` entries that fall outside this set, and PATCH silently
// skips unknown codes, so shrinking this list is safe for upgrades.
inline constexpr std::array<std::string_view, 7> kAvailableCurrencies = {
    "USD", "EUR", "GBP", "CAD", "CHF", "AUD", "JPY",
};

// One entry per rotatable screen. `api_id` is what gets serialised into
// `screens[].id` and what NVS keys like `screen<id>Visible` use.
struct ScreenKind {
  int api_id;
  std::string_view short_name;     // log tag (KindName())
  std::string_view display_label;  // WebUI-facing human name
};

// Expand the X-macro into a std::array. Using string_view so the array
// is header-only constexpr without forcing a .cpp translation unit.
inline constexpr auto kScreenKinds = [] {
#define BTCLOCK_SCREEN_KIND_COUNT_ENTRY(enum_name, api_id, short_name, label) +1
  constexpr std::size_t kCount =
      0 BTCLOCK_SCREEN_KIND_LIST(BTCLOCK_SCREEN_KIND_COUNT_ENTRY);
#undef BTCLOCK_SCREEN_KIND_COUNT_ENTRY
  std::array<ScreenKind, kCount> arr{};
  std::size_t i = 0;
#define BTCLOCK_SCREEN_KIND_EMIT_ENTRY(enum_name, api_id, short_name, label) \
  arr[i++] = ScreenKind{(api_id), (short_name), (label)};
  BTCLOCK_SCREEN_KIND_LIST(BTCLOCK_SCREEN_KIND_EMIT_ENTRY)
#undef BTCLOCK_SCREEN_KIND_EMIT_ENTRY
  return arr;
}();

}  // namespace catalogs
}  // namespace btclock
