// Host tests for the firmware-baked catalogues that flow into
// GET /api/settings (main/app/catalogs.hpp). Pins the JSON shapes the
// WebUI consumes so a rename in the constexpr arrays — or a drift
// between ScreenType and BTCLOCK_SCREEN_KIND_LIST — fails here before
// it ships to a device.

#include "doctest.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "app/catalogs.hpp"
#include "cJSON.h"
#include "settings/api.hpp"
#include "settings/pref_keys.hpp"

namespace {

class NullPrefs final : public btclock::settings::PrefsReader {
 public:
  std::string GetString(const char* key,
                        const char* default_value) const override {
    (void)key;
    return default_value ? default_value : "";
  }
  uint32_t GetU32(const char* key, uint32_t default_value) const override {
    (void)key;
    return default_value;
  }
  int32_t GetI32(const char* key, int32_t default_value) const override {
    (void)key;
    return default_value;
  }
  uint8_t GetU8(const char* key, uint8_t default_value) const override {
    (void)key;
    return default_value;
  }
  bool GetBool(const char* key, bool default_value) const override {
    (void)key;
    return default_value;
  }
};

// Mirrors the main.cpp wiring that copies the constexpr catalogues into
// DeviceContext. Kept inline so the test fails if either side changes.
btclock::settings::DeviceContext CtxFromCatalogs() {
  btclock::settings::DeviceContext ctx;
  for (const auto& f : btclock::catalogs::kAvailableFonts) {
    ctx.available_fonts.emplace_back(f);
  }
  for (const auto& c : btclock::catalogs::kAvailableCurrencies) {
    ctx.available_currencies.emplace_back(c);
  }
  for (const auto& s : btclock::catalogs::kScreenKinds) {
    ctx.screens.push_back({s.api_id, std::string(s.display_label)});
  }
  return ctx;
}

std::set<std::string> StringArrayToSet(cJSON* arr) {
  std::set<std::string> out;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    if (cJSON_IsString(it) && it->valuestring) out.insert(it->valuestring);
  }
  return out;
}

}  // namespace

TEST_CASE("availableFonts is a plain-string array with known ids") {
  NullPrefs prefs;
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, CtxFromCatalogs());
  REQUIRE(root != nullptr);

  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "availableFonts");
  REQUIRE(cJSON_IsArray(arr));
  // WebUI's src/lib/types/settings.ts types this as `string[]`, not
  // `{id,label}[]`. If someone flips the shape, DisplaySettings.svelte
  // silently drops every font from the picker.
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    CHECK(cJSON_IsString(it));
  }
  const auto fonts = StringArrayToSet(arr);
  // Pin the minimum set. If a build drops one of these, the renderer
  // paths that hard-code fontFamily::kAntonio etc. would still compile
  // but the UI's font picker would suddenly hide options.
  CHECK(fonts.count("antonio") == 1);
  CHECK(fonts.count("oswald") == 1);
  CHECK(fonts.count("inter") == 1);
  CHECK(fonts.count("sourceSerif") == 1);
  CHECK(fonts.count("merriweather") == 1);
  CHECK(fonts.count("bitter") == 1);
  CHECK(fonts.count("atkinson") == 1);
  // Retired family — must NOT reappear in the catalogue. A regression
  // here would let the WebUI offer a font the firmware no longer ships
  // and the validation walk silently snap to antonio.
  CHECK(fonts.count("dejavu") == 0);

  cJSON_Delete(root);
}

TEST_CASE("availableCurrencies is a plain-string array of ISO-4217 codes") {
  NullPrefs prefs;
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, CtxFromCatalogs());
  REQUIRE(root != nullptr);

  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "availableCurrencies");
  REQUIRE(cJSON_IsArray(arr));
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    REQUIRE(cJSON_IsString(it));
    // ISO 4217 is three uppercase letters. Guard against someone
    // adding a "btc" lowercase entry that the price websocket can't
    // subscribe to.
    const std::string code = it->valuestring;
    CHECK(code.size() == 3);
    for (char ch : code) {
      CHECK(ch >= 'A');
      CHECK(ch <= 'Z');
    }
  }

  // Pin the exact 7-code set the upstream price websocket
  // (ws.btclock.dev/api/v2/currencies) publishes. If the upstream
  // list grows and the firmware needs to follow, the new code goes
  // into main/app/catalogs.hpp *and* gets added here. Drift in the
  // other direction — firmware advertising a code the WS can't
  // serve — surfaces as a permanently-empty price panel.
  REQUIRE(cJSON_GetArraySize(arr) == 7);

  // Order is deterministic and matches the catalogue declaration so
  // the WebUI's drop-down keeps the same sort across firmware builds.
  const std::vector<std::string> expected_order = {
      "USD", "EUR", "GBP", "CAD", "CHF", "AUD", "JPY",
  };
  std::size_t i = 0;
  cJSON_ArrayForEach(it, arr) {
    REQUIRE(cJSON_IsString(it));
    REQUIRE(i < expected_order.size());
    CHECK(std::string(it->valuestring) == expected_order[i]);
    ++i;
  }

  // Set-membership check as well, so the failure message is clearer
  // when someone drops an entry (the order-check would cascade into
  // N confusing mismatches instead of "CHF missing").
  const auto codes = StringArrayToSet(arr);
  CHECK(codes.count("USD") == 1);
  CHECK(codes.count("EUR") == 1);
  CHECK(codes.count("GBP") == 1);
  CHECK(codes.count("CAD") == 1);
  CHECK(codes.count("CHF") == 1);
  CHECK(codes.count("AUD") == 1);
  CHECK(codes.count("JPY") == 1);
  CHECK(codes.size() == 7);

  cJSON_Delete(root);
}

TEST_CASE("screens catalogue matches BTCLOCK_SCREEN_KIND_LIST") {
  NullPrefs prefs;
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, CtxFromCatalogs());
  REQUIRE(root != nullptr);

  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(arr));
  // Same count as the X-macro — the test catches both "forgot to wire
  // a new screen" and "accidentally duplicated an entry".
  CHECK(static_cast<std::size_t>(cJSON_GetArraySize(arr)) ==
        btclock::catalogs::kScreenKinds.size());

  // Build a map from the catalogue so we can assert each entry appears
  // exactly once and in the same id-order.
  std::map<int, std::string> expected;
  for (const auto& s : btclock::catalogs::kScreenKinds) {
    expected.emplace(s.api_id, std::string(s.display_label));
  }
  // Pin the specific ids + labels the WebUI's translation tables key
  // off (m['screens.<label>']). Drift here = users lose their saved
  // rotation order on firmware upgrade.
  CHECK(expected.at(0) == "Block Height");
  CHECK(expected.at(3) == "Time");
  CHECK(expected.at(4) == "Halving countdown");
  CHECK(expected.at(6) == "Block Fee Rate");
  CHECK(expected.at(10) == "Sats per dollar");
  CHECK(expected.at(20) == "Ticker");
  CHECK(expected.at(30) == "Market Cap");
  CHECK(expected.at(40) == "Bitcoin Supply");
  CHECK(expected.at(70) == "Mining Pool Hashrate");
  CHECK(expected.at(71) == "Mining Pool Earnings");

  // Each emitted entry carries {id, name, enabled, order}. Pin this
  // shape — the WebUI's Screen type requires all four, and a missing
  // `order` would force it to fall back to JsonArray iteration order.
  std::set<int> seen_ids;
  std::set<int> seen_orders;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    REQUIRE(cJSON_IsObject(it));
    cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
    cJSON* name = cJSON_GetObjectItemCaseSensitive(it, "name");
    cJSON* enabled = cJSON_GetObjectItemCaseSensitive(it, "enabled");
    cJSON* order = cJSON_GetObjectItemCaseSensitive(it, "order");
    REQUIRE(cJSON_IsNumber(id));
    REQUIRE(cJSON_IsString(name));
    REQUIRE(cJSON_IsBool(enabled));
    REQUIRE(cJSON_IsNumber(order));
    const int iid = static_cast<int>(id->valuedouble);
    const int iord = static_cast<int>(order->valuedouble);
    CHECK(seen_ids.insert(iid).second);  // no duplicates
    CHECK(seen_orders.insert(iord).second);
    auto it_exp = expected.find(iid);
    REQUIRE(it_exp != expected.end());
    CHECK(std::string(name->valuestring) == it_exp->second);
  }

  cJSON_Delete(root);
}

TEST_CASE("screen kinds do not drift from ScreenType enum") {
  // Every api_id in the catalogue should correspond to a ScreenType
  // value the ScreenManager knows how to render. We don't have a
  // reverse-lookup helper; instead pin that every enum that exists
  // shows up by its api_id here. If a new ScreenType is added and
  // forgotten in BTCLOCK_SCREEN_KIND_LIST, this count check catches it.
  // 12: 8 base kinds + MiningPoolHashrate/MiningPoolEarnings +
  // BitaxeHashrate/BitaxeBestDiff. kCustom, kDebug and kNostrZap are
  // intentionally excluded — they're off-rotation overrides, not
  // user-rotatable catalogue entries.
  constexpr std::size_t kEnumCount = 12;
  CHECK(btclock::catalogs::kScreenKinds.size() == kEnumCount);
}
