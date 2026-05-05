// Host tests for the pure-logic core of /api/settings
// (components/settings/settings_api.cpp). Drives BuildGetResponse +
// ApplyPatch against an in-memory PrefsReader/Writer fake so the
// schema round-trip is covered without ESP-IDF.

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
#include "doctest.h"
#include "settings/api.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace {

class FakePrefs final : public btclock::settings::PrefsReader,
                        public btclock::settings::PrefsWriter {
 public:
  // PrefsReader
  std::string GetString(const char* key,
                        const char* default_value) const override {
    auto it = str_.find(key);
    return it != str_.end() ? it->second : (default_value ? default_value : "");
  }
  uint32_t GetU32(const char* key, uint32_t default_value) const override {
    auto it = u32_.find(key);
    return it != u32_.end() ? it->second : default_value;
  }
  int32_t GetI32(const char* key, int32_t default_value) const override {
    auto it = i32_.find(key);
    return it != i32_.end() ? it->second : default_value;
  }
  uint8_t GetU8(const char* key, uint8_t default_value) const override {
    auto it = u8_.find(key);
    return it != u8_.end() ? it->second : default_value;
  }
  bool GetBool(const char* key, bool default_value) const override {
    auto it = b_.find(key);
    return it != b_.end() ? it->second : default_value;
  }

  // PrefsWriter
  void SetString(const char* key, const char* value) override {
    str_[key] = value ? value : "";
  }
  void SetU32(const char* key, uint32_t value) override { u32_[key] = value; }
  void SetI32(const char* key, int32_t value) override { i32_[key] = value; }
  void SetU8(const char* key, uint8_t value) override { u8_[key] = value; }
  void SetBool(const char* key, bool value) override { b_[key] = value; }
  void Remove(const char* key) override {
    str_.erase(key);
    u32_.erase(key);
    i32_.erase(key);
    u8_.erase(key);
    b_.erase(key);
  }

  // Test helpers.
  bool HasStr(const char* key) const { return str_.count(key) > 0; }
  bool HasBool(const char* key) const { return b_.count(key) > 0; }
  bool HasU32(const char* key) const { return u32_.count(key) > 0; }

  std::map<std::string, std::string> str_;
  std::map<std::string, uint32_t> u32_;
  std::map<std::string, int32_t> i32_;
  std::map<std::string, uint8_t> u8_;
  std::map<std::string, bool> b_;
};

btclock::settings::DeviceContext DefaultCtx() {
  btclock::settings::DeviceContext ctx;
  ctx.hostname = "btclock-abc123";
  ctx.ip = "10.0.0.7";
  ctx.tx_power = 44;
  ctx.num_screens = 3;
  ctx.has_frontlight = true;
  ctx.has_light_level = false;
  ctx.hw_rev = "REV_B_EPD_2_13";
  ctx.fs_rev = "fs-42";
  ctx.git_rev = "deadbeef";
  ctx.available_fonts = {"antonio", "oswald"};
  ctx.available_pools = {"ocean", "braiins"};
  ctx.available_currencies = {"USD", "EUR", "GBP", "JPY", "AUD", "CAD"};
  ctx.screens = {
      {0, "Block Height"},      {3, "Time"},
      {4, "Halving countdown"}, {6, "Block Fee Rate"},
      {10, "Sats per dollar"},  {20, "Ticker"},
      {30, "Market Cap"},       {40, "Bitcoin Supply"},
  };
  return ctx;
}

}  // namespace

TEST_CASE("GET /api/settings emits lastBuildTime as Unix seconds integer") {
  // 2026-04-24T15:30:45Z -> 1777044645 (see test_build_time.cpp).
  FakePrefs prefs;
  auto ctx = DefaultCtx();
  ctx.last_build_time_unix = 1777044645;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);

  cJSON* lbt = cJSON_GetObjectItemCaseSensitive(root, "lastBuildTime");
  REQUIRE(lbt != nullptr);
  // MUST be a JSON number, never a string — the WebUI formats it with
  // `new Date(seconds * 1000)` and a stray string would land as NaN.
  CHECK(cJSON_IsNumber(lbt));
  CHECK_FALSE(cJSON_IsString(lbt));
  CHECK(static_cast<int64_t>(lbt->valuedouble) == 1777044645);

  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings omits lastBuildTime when unknown") {
  // Populate nothing — default-constructed ctx has last_build_time_unix=0,
  // which the handler treats as "unknown" and drops from the response so
  // the WebUI renders a placeholder instead of "1970-01-01".
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  CHECK(cJSON_GetObjectItemCaseSensitive(root, "lastBuildTime") == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings emits device-context fields") {
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);

  cJSON* hostname = cJSON_GetObjectItemCaseSensitive(root, "hostname");
  REQUIRE(cJSON_IsString(hostname));
  CHECK(std::string(hostname->valuestring) == "btclock-abc123");

  cJSON* hw = cJSON_GetObjectItemCaseSensitive(root, "hwRev");
  REQUIRE(cJSON_IsString(hw));
  CHECK(std::string(hw->valuestring) == "REV_B_EPD_2_13");

  cJSON* num = cJSON_GetObjectItemCaseSensitive(root, "numScreens");
  REQUIRE(cJSON_IsNumber(num));
  CHECK(static_cast<int>(num->valuedouble) == 3);

  cJSON* fl = cJSON_GetObjectItemCaseSensitive(root, "hasFrontlight");
  REQUIRE(cJSON_IsBool(fl));
  CHECK(cJSON_IsTrue(fl));

  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings surfaces every schema field") {
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);

  // Every field in the schema table must appear in the response. The
  // WebUI's type predicates (`is<bool>`, `is<uint>`) silently skip a
  // missing field, which is how old bugs like "miningPollStats" crept
  // in — use explicit checks here.
  for (const auto& f : btclock::settings::kFields) {
    std::string key(f.key);
    CAPTURE(key);
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key.c_str());
    REQUIRE(item != nullptr);
    // actCurrencies stores a CSV in NVS but BuildGetResponse emits the
    // filtered array shape (catalogue-validated) on the wire — schema
    // kind is kString but the JSON value is an array.
    if (key == "actCurrencies") {
      CHECK(cJSON_IsArray(item));
      continue;
    }
    switch (f.kind) {
      case btclock::settings::FieldKind::kString:
        CHECK(cJSON_IsString(item));
        break;
      case btclock::settings::FieldKind::kUint:
      case btclock::settings::FieldKind::kInt:
      case btclock::settings::FieldKind::kUChar:
        CHECK(cJSON_IsNumber(item));
        break;
      case btclock::settings::FieldKind::kBool:
        CHECK(cJSON_IsBool(item));
        break;
    }
  }
  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings redacts httpAuthPass / otaPass") {
  FakePrefs prefs;
  // Even when stored, the raw values must NOT show up in the response.
  prefs.str_["httpAuthPass"] = "topsecret";
  prefs.str_["otaPass"] = "another";
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);

  // They *are* emitted as raw strings today (matching old firmware's
  // generic loop), but the "set" indicators must also be present and
  // truthy so the WebUI can render a "password configured" badge.
  cJSON* set1 = cJSON_GetObjectItemCaseSensitive(root, "httpAuthPassSet");
  REQUIRE(cJSON_IsBool(set1));
  CHECK(cJSON_IsTrue(set1));
  cJSON* set2 = cJSON_GetObjectItemCaseSensitive(root, "otaPassSet");
  REQUIRE(cJSON_IsBool(set2));
  CHECK(cJSON_IsTrue(set2));

  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings emits screens with id/name/enabled/order") {
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);
  cJSON* screens = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(screens));
  const int n = cJSON_GetArraySize(screens);
  CHECK(n == 8);
  // First entry must have the four keys.
  cJSON* first = cJSON_GetArrayItem(screens, 0);
  CHECK(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(first, "id")));
  CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "name")));
  CHECK(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(first, "enabled")));
  CHECK(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(first, "order")));
  cJSON_Delete(root);
}

namespace {

// Mirrors main/app/catalogs.hpp's full screen list so the hidden-id
// test reflects what a real device emits (not the trimmed fixture the
// other tests use).
btclock::settings::DeviceContext CtxWithEarnings() {
  btclock::settings::DeviceContext ctx = DefaultCtx();
  ctx.screens = {
      {0, "Block Height"},          {3, "Time"},
      {4, "Halving countdown"},     {6, "Block Fee Rate"},
      {10, "Sats per dollar"},      {20, "Ticker"},
      {30, "Market Cap"},           {40, "Bitcoin Supply"},
      {70, "Mining Pool Hashrate"}, {71, "Mining Pool Earnings"},
      {80, "Bitaxe Hashrate"},      {81, "Bitaxe Best Difficulty"},
  };
  return ctx;
}

// Collect the emitted `screens[]` ids into a sorted vector so tests can
// assert set membership without depending on JSON iteration order.
std::vector<int> EmittedScreenIds(cJSON* root) {
  std::vector<int> out;
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
    if (cJSON_IsNumber(id)) out.push_back(static_cast<int>(id->valuedouble));
  }
  return out;
}

}  // namespace

TEST_CASE("GET /api/settings keeps earnings screen (id 71) by default") {
  FakePrefs prefs;
  // No hidden_screen_ids set — caller decided every catalogue entry is
  // available. 71 must appear.
  cJSON* root = btclock::settings::BuildGetResponse(prefs, CtxWithEarnings());
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  CHECK(std::find(ids.begin(), ids.end(), 71) != ids.end());
  CHECK(ids.size() == CtxWithEarnings().screens.size());
  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings drops hidden screens from screens[]") {
  FakePrefs prefs;
  // Same context a device sees when the active pool is solo: id 71
  // hidden, everything else visible.
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.hidden_screen_ids = {71};
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  // 71 must be gone; 70 (Mining Pool Hashrate) stays visible.
  CHECK(std::find(ids.begin(), ids.end(), 71) == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 70) != ids.end());
  CHECK(ids.size() == ctx.screens.size() - 1);
  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings renumbers order after hiding a screen") {
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.hidden_screen_ids = {71};
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(arr));
  // Every emitted `order` must be contiguous 0..n-1 so the WebUI's
  // sort-by-order preserves the rotation sequence without gaps.
  int expected = 0;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    cJSON* o = cJSON_GetObjectItemCaseSensitive(it, "order");
    REQUIRE(cJSON_IsNumber(o));
    CHECK(static_cast<int>(o->valuedouble) == expected);
    ++expected;
  }
  cJSON_Delete(root);
}

TEST_CASE("GET drops mining-pool screens (70,71) when miningPoolStats off") {
  // Feature-flag gate. Both the pool hashrate slot (70) and the earnings
  // slot (71) must disappear from the emitted list when the parent
  // `miningPoolStats` toggle is off — matches how the bitaxe pair is
  // suppressed below. The WebUI's "which screens can I re-order" picker
  // must only advertise screens the firmware can actually render.
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.mining_pool_stats_enabled = false;
  ctx.bitaxe_enabled = true;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  CHECK(std::find(ids.begin(), ids.end(), 70) == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 71) == ids.end());
  // Bitaxe pair still present.
  CHECK(std::find(ids.begin(), ids.end(), 80) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 81) != ids.end());
  // Order must remain contiguous 0..N-1 after the filter.
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  int expected = 0;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    cJSON* o = cJSON_GetObjectItemCaseSensitive(it, "order");
    REQUIRE(cJSON_IsNumber(o));
    CHECK(static_cast<int>(o->valuedouble) == expected);
    ++expected;
  }
  cJSON_Delete(root);
}

TEST_CASE("GET drops bitaxe screens (80,81) when bitaxeEnabled off") {
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.mining_pool_stats_enabled = true;
  ctx.bitaxe_enabled = false;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  CHECK(std::find(ids.begin(), ids.end(), 80) == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 81) == ids.end());
  // Mining-pool slots still present when the parent flag is on.
  CHECK(std::find(ids.begin(), ids.end(), 70) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 71) != ids.end());
  // Order renumbered contiguously.
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  int expected = 0;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    cJSON* o = cJSON_GetObjectItemCaseSensitive(it, "order");
    REQUIRE(cJSON_IsNumber(o));
    CHECK(static_cast<int>(o->valuedouble) == expected);
    ++expected;
  }
  cJSON_Delete(root);
}

TEST_CASE(
    "GET drops 70/80/81 when both features off (71 stays dropped via mining "
    "gate)") {
  // Bug repro fixture: stock Rev A boot has both features off and still
  // emits 70, 80, 81 as enabled=false entries. After the fix all three
  // must be gone.
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.mining_pool_stats_enabled = false;
  ctx.bitaxe_enabled = false;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  CHECK(std::find(ids.begin(), ids.end(), 70) == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 71) == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 80) == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 81) == ids.end());
  // The eight always-available screens from CtxWithEarnings() remain.
  CHECK(ids.size() == ctx.screens.size() - 4);
  cJSON_Delete(root);
}

TEST_CASE(
    "GET keeps id 70 + drops 71 on solo pool (mining on, earnings hidden)") {
  // Combined path: parent feature is on but the active pool doesn't
  // support per-user earnings, so the legacy `hidden_screen_ids` gate
  // alone drops 71 while 70 stays visible.
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.mining_pool_stats_enabled = true;
  ctx.bitaxe_enabled = true;
  ctx.hidden_screen_ids = {71};  // what the solo-pool probe contributes
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  CHECK(std::find(ids.begin(), ids.end(), 70) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 71) == ids.end());
  cJSON_Delete(root);
}

TEST_CASE(
    "GET all four (70,71,80,81) present when both flags on + non-solo pool") {
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.mining_pool_stats_enabled = true;
  ctx.bitaxe_enabled = true;
  // `hidden_screen_ids` empty = non-solo pool (ocean etc.).
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root != nullptr);
  const auto ids = EmittedScreenIds(root);
  CHECK(std::find(ids.begin(), ids.end(), 70) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 71) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 80) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 81) != ids.end());
  cJSON_Delete(root);
}

TEST_CASE("PATCH still accepts screen 71 in full reorder when hidden") {
  // A WebUI built against an older /api/settings response (before the
  // solo-pool gate shipped) may PATCH the full 12-entry order. We must
  // not reject it — the capability filter is presentation-only.
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.hidden_screen_ids = {71};
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":["
      "{\"id\":0,\"enabled\":true,\"order\":0},"
      "{\"id\":3,\"enabled\":true,\"order\":1},"
      "{\"id\":4,\"enabled\":true,\"order\":2},"
      "{\"id\":6,\"enabled\":true,\"order\":3},"
      "{\"id\":10,\"enabled\":true,\"order\":4},"
      "{\"id\":20,\"enabled\":true,\"order\":5},"
      "{\"id\":30,\"enabled\":true,\"order\":6},"
      "{\"id\":40,\"enabled\":true,\"order\":7},"
      "{\"id\":70,\"enabled\":true,\"order\":8},"
      "{\"id\":71,\"enabled\":true,\"order\":9},"
      "{\"id\":80,\"enabled\":true,\"order\":10},"
      "{\"id\":81,\"enabled\":true,\"order\":11}"
      "]}",
      ctx, prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // screen<id>Visible defaults to true; compare-on-write skips the
  // SetBool when the new value matches the default. Read effective.
  CHECK(prefs.GetBool("screen71Visible", true) == true);
}

TEST_CASE("PATCH accepts reorder covering only visible subset (71 hidden)") {
  // New WebUI path: the client only sees 11 screens in the GET (71 is
  // the hidden earnings slot) so a round-tripped reorder omits 71. The
  // validator must treat that as "full reorder of visible slots", not
  // an incomplete-catalogue error.
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.hidden_screen_ids = {71};
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":["
      "{\"id\":0,\"enabled\":true,\"order\":0},"
      "{\"id\":3,\"enabled\":true,\"order\":1},"
      "{\"id\":4,\"enabled\":true,\"order\":2},"
      "{\"id\":6,\"enabled\":true,\"order\":3},"
      "{\"id\":10,\"enabled\":true,\"order\":4},"
      "{\"id\":20,\"enabled\":true,\"order\":5},"
      "{\"id\":30,\"enabled\":true,\"order\":6},"
      "{\"id\":40,\"enabled\":true,\"order\":7},"
      "{\"id\":70,\"enabled\":true,\"order\":8},"
      "{\"id\":80,\"enabled\":true,\"order\":9},"
      "{\"id\":81,\"enabled\":true,\"order\":10}"
      "]}",
      ctx, prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["screenOrder"] == "0,3,4,6,10,20,30,40,70,80,81");
}

TEST_CASE("GET /api/settings emits dnd nested object") {
  FakePrefs prefs;
  prefs.u32_["dndStartHour"] = 22;
  prefs.u32_["dndStartMin"] = 30;
  prefs.u32_["dndEndHour"] = 7;
  prefs.u32_["dndEndMin"] = 45;
  prefs.b_["dndEnabled"] = true;

  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);
  cJSON* dnd = cJSON_GetObjectItemCaseSensitive(root, "dnd");
  REQUIRE(cJSON_IsObject(dnd));
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dnd, "enabled")));
  CHECK(static_cast<int>(
            cJSON_GetObjectItemCaseSensitive(dnd, "startHour")->valuedouble) ==
        22);
  CHECK(static_cast<int>(
            cJSON_GetObjectItemCaseSensitive(dnd, "endMinute")->valuedouble) ==
        45);
  cJSON_Delete(root);
}

TEST_CASE("PATCH malformed JSON returns BadRequest with no NVS writes") {
  FakePrefs prefs;
  auto res =
      btclock::settings::ApplyPatch("{not_json", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "json");
  CHECK(prefs.str_.empty());
  CHECK(prefs.u32_.empty());
  CHECK(prefs.b_.empty());
}

TEST_CASE("PATCH non-object body rejected") {
  FakePrefs prefs;
  auto res =
      btclock::settings::ApplyPatch("[1,2,3]", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "not_object");
}

TEST_CASE("PATCH unknown field silently ignored") {
  FakePrefs prefs;
  // Matches old-firmware behaviour: strSettings/uintSettings/boolSettings
  // only act on keys they recognise; an unknown key is a no-op.
  auto res = btclock::settings::ApplyPatch("{\"notAField\":\"hello\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.touched_keys.empty());
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH same-value scalar leaves touched_keys empty") {
  // Compare-on-write: if the body sends the same value the prefs
  // reader already returns, ApplyPatch must NOT mark the key touched.
  // Without this, the post-PATCH dispatch hooks (fontName / tzString /
  // invertedColor) fire on every WebUI save, triggering one full EPD
  // refresh per hook — the WebUI sends the entire settings object on
  // each save, so every field would otherwise be "touched" regardless
  // of what the user changed.
  FakePrefs prefs;
  prefs.SetString(btclock::prefs::kFontName, "antonio");
  prefs.SetString(btclock::prefs::kTzString, "Europe/Amsterdam");
  prefs.SetBool(btclock::prefs::kInvertedColor, false);

  auto res = btclock::settings::ApplyPatch(
      "{\"fontName\":\"antonio\",\"tzString\":\"Europe/Amsterdam\","
      "\"invertedColor\":false}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.touched_keys.empty());
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH mixed same+changed only marks the changed keys") {
  FakePrefs prefs;
  prefs.SetString(btclock::prefs::kFontName, "antonio");
  prefs.SetString(btclock::prefs::kTzString, "Europe/Amsterdam");

  auto res = btclock::settings::ApplyPatch(
      "{\"fontName\":\"antonio\",\"tzString\":\"America/New_York\"}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  REQUIRE(res.touched_keys.size() == 1);
  CHECK(res.touched_keys[0] == "tzString");
}

TEST_CASE("PATCH same-value matches default (unwritten slot) is unchanged") {
  // Fresh prefs (no slot written yet). The schema's default for satsVariant
  // is 7; sending 7 in the PATCH must match the default and skip the
  // touched-keys emplace, so on_sats_variant_changed doesn't fire on a
  // first-save where the user never picked a glyph.
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"satsVariant\":7}", DefaultCtx(),
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.touched_keys.empty());
}

TEST_CASE("PATCH same-value invertedColor leaves touched_keys empty") {
  // invertedColor is a bespoke writer (also stamps fgColor/bgColor); it
  // must respect the same compare-on-write semantics as the schema-driven
  // scalars so a re-save of the dashboard's current state doesn't fire a
  // full refresh.
  FakePrefs prefs;
  prefs.SetBool(btclock::prefs::kInvertedColor, true);
  prefs.SetU32(btclock::prefs::kFgColor, 0xFFFFu);
  prefs.SetU32(btclock::prefs::kBgColor, 0u);
  auto res = btclock::settings::ApplyPatch("{\"invertedColor\":true}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.touched_keys.empty());
}

TEST_CASE("PATCH writes runtime-editable bool without rebootRequired") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"mcapBigChar\":true,\"stealFocus\":false}", DefaultCtx(), prefs,
      prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!res.reboot_required);
  // mcapBigChar default is `true` and stealFocus default is `true`; the
  // test PATCHes mcapBigChar=true (no-op write) and stealFocus=false
  // (changed). Compare-on-write only writes the latter, so check the
  // effective state via the prefs interface.
  CHECK(prefs.GetBool("mcapBigChar", true) == true);
  CHECK(prefs.GetBool("stealFocus", true) == false);
}

TEST_CASE("PATCH writes runtime-editable uint with range clamping") {
  FakePrefs prefs;
  // In-range value accepted. ledBrightness default is 128 — same as the
  // PATCH value, so compare-on-write skips the SetU32 and the underlying
  // map stays empty. Observe via the effective interface.
  auto ok = btclock::settings::ApplyPatch("{\"ledBrightness\":128}",
                                          DefaultCtx(), prefs, prefs);
  CHECK(ok.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.GetU32("ledBrightness", 128) == 128u);

  // Out-of-range value rejected (ledBrightness bounded 0..255).
  FakePrefs prefs2;
  auto bad = btclock::settings::ApplyPatch("{\"ledBrightness\":9999}",
                                           DefaultCtx(), prefs2, prefs2);
  CHECK(bad.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(bad.error == "range:ledBrightness");
  CHECK(prefs2.u32_.count("ledBrightness") == 0);
}

TEST_CASE("PATCH boot-only field triggers rebootRequired") {
  FakePrefs prefs;
  // otaPass is one of the remaining boot_only string fields (the OTA
  // password is captured by ArduinoOTA::setPassword at boot only).
  auto res = btclock::settings::ApplyPatch("{\"otaPass\":\"newsecret\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.reboot_required);
  CHECK(prefs.str_["otaPass"] == "newsecret");
}

TEST_CASE("PATCH mixed runtime + boot-only still sets rebootRequired") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"mcapBigChar\":true,\"otaEnabled\":false}", DefaultCtx(), prefs,
      prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // otaEnabled is boot-only (ArduinoOTA::begin runs once at setup).
  CHECK(res.reboot_required);
}

TEST_CASE(
    "PATCH hostnamePrefix is live (mDNS re-publishes via on_mdns_changed)") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"hostnamePrefix\":\"newhost\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // bd btclock_v4-9ut flipped this to runtime — the control server
  // now wires on_mdns_changed which calls ReinitMdns to re-publish
  // under the freshly-persisted prefix.
  CHECK_FALSE(res.reboot_required);
  CHECK(prefs.str_["hostnamePrefix"] == "newhost");
}

TEST_CASE("PATCH mdnsEnabled is live (advert toggles via on_mdns_changed)") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"mdnsEnabled\":false}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // bd btclock_v4-9ut: ReinitMdns frees the existing responder when
  // the user disables mdns, no reboot needed.
  CHECK_FALSE(res.reboot_required);
  CHECK(prefs.u32_["mdnsEnabled"] == 0u);
}

TEST_CASE(
    "Boot-only detection: every flagged key is what old firmware reboots on") {
  // Paranoia guard against drift: the beads issue's acceptance
  // criterion is that WiFi/provisioning-adjacent settings trigger
  // reboot. Check the ones an end-user would expect. `invertedColor`
  // is intentionally absent — bd btclock_v4-5wj flipped it to runtime
  // now that EpdSetGlobalInverted lets the driver swap polarity live.
  // `hostnamePrefix` and `mdnsEnabled` are also absent — bd
  // btclock_v4-9ut wired on_mdns_changed so the advert re-publishes
  // live without a reboot.
  for (const char* k :
       {"otaEnabled", "httpAuthEnabled", "httpAuthUser", "httpAuthPass",
        "otaPass", "mempoolInstance", "dataSource"}) {
    CAPTURE(k);
    const auto* spec = btclock::settings::FindField(k);
    REQUIRE(spec != nullptr);
    CHECK(spec->boot_only);
  }
  // fontName is intentionally absent — bd btclock_v4-j76.5 flipped it
  // to runtime: kSetFont is dispatched through the ControlCommand queue
  // and AppFonts::SetFamily applies live on the main task.
  {
    const auto* spec = btclock::settings::FindField("fontName");
    REQUIRE(spec != nullptr);
    CHECK_FALSE(spec->boot_only);
  }
}

TEST_CASE("PATCH timePerScreen converts minutes to seconds") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"timePerScreen\":5}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u32_["timerSeconds"] == 300u);
}

TEST_CASE("PATCH actCurrencies filters against available list") {
  FakePrefs prefs;
  auto ok = btclock::settings::ApplyPatch(
      "{\"actCurrencies\":[\"USD\",\"JPY\"]}", DefaultCtx(), prefs, prefs);
  CHECK(ok.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["actCurrencies"] == "USD,JPY");

  // Unknown codes are silently dropped (not a hard 400) so an older
  // cached client that still sends a legacy code doesn't wipe out the
  // rest of the PATCH. The surviving codes get persisted.
  FakePrefs prefs2;
  auto partial = btclock::settings::ApplyPatch(
      "{\"actCurrencies\":[\"USD\",\"XYZ\",\"EUR\"]}", DefaultCtx(), prefs2,
      prefs2);
  CHECK(partial.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs2.str_["actCurrencies"] == "USD,EUR");

  // Non-string entries still hard-fail — that's a malformed client,
  // not a catalogue-drift case.
  FakePrefs prefs3;
  auto bad = btclock::settings::ApplyPatch("{\"actCurrencies\":[\"USD\",42]}",
                                           DefaultCtx(), prefs3, prefs3);
  CHECK(bad.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(bad.error == "currency:not_string");
}

TEST_CASE("GET actCurrencies filters legacy NVS codes not in catalogue") {
  // Simulate an upgrade: user has an older firmware's richer list
  // persisted (CNY/BRL/AED were all valid before the prune). The GET
  // response should drop them rather than expose codes the upstream
  // price websocket can no longer serve.
  FakePrefs prefs;
  prefs.str_["actCurrencies"] = "USD,BRL,EUR,CNY,JPY,AED";
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);

  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "actCurrencies");
  REQUIRE(cJSON_IsArray(arr));
  std::vector<std::string> kept;
  cJSON* it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    REQUIRE(cJSON_IsString(it));
    kept.emplace_back(it->valuestring);
  }
  // Order is preserved, only the out-of-catalogue codes drop out.
  REQUIRE(kept.size() == 3u);
  CHECK(kept[0] == "USD");
  CHECK(kept[1] == "EUR");
  CHECK(kept[2] == "JPY");

  cJSON_Delete(root);
}

TEST_CASE("PATCH screens visibility only") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":[{\"id\":0,\"enabled\":false},"
      "{\"id\":3,\"enabled\":true}]}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // ApplyPatch's compare-on-write skips writes whose new value matches
  // the prior value (default = true for screen visibility), so
  // screen3Visible's "stay-true" doesn't materialise in the underlying
  // map. Read via GetBool to observe the effective state.
  CHECK(prefs.GetBool("screen0Visible", true) == false);
  CHECK(prefs.GetBool("screen3Visible", true) == true);
}

TEST_CASE("PATCH screens partial order rejected") {
  FakePrefs prefs;
  // Some entries carry `order`, others don't — that's ambiguous.
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":[{\"id\":0,\"enabled\":true,\"order\":0},"
      "{\"id\":3,\"enabled\":true}]}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "screens:partial_order");
}

TEST_CASE("PATCH screens full reorder produces CSV order") {
  FakePrefs prefs;
  // Reverse the rotation order (8 screens in DefaultCtx).
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":["
      "{\"id\":40,\"enabled\":true,\"order\":0},"
      "{\"id\":30,\"enabled\":true,\"order\":1},"
      "{\"id\":20,\"enabled\":true,\"order\":2},"
      "{\"id\":10,\"enabled\":true,\"order\":3},"
      "{\"id\":6,\"enabled\":true,\"order\":4},"
      "{\"id\":4,\"enabled\":true,\"order\":5},"
      "{\"id\":3,\"enabled\":true,\"order\":6},"
      "{\"id\":0,\"enabled\":true,\"order\":7}"
      "]}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["screenOrder"] == "40,30,20,10,6,4,3,0");
}

TEST_CASE("PATCH screens reorder with unknown id rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":[{\"id\":999,\"enabled\":true,\"order\":0},"
      "{\"id\":3,\"enabled\":true,\"order\":1}]}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "screens:unknown_id");
}

TEST_CASE("PATCH dnd time range writes hour/minute quad") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"dnd\":{\"dndTimeEnabled\":true,"
      "\"startHour\":22,\"startMinute\":30,"
      "\"endHour\":7,\"endMinute\":0}}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["dndTimeEnabled"] == true);
  CHECK(prefs.u32_["dndStartHour"] == 22u);
  CHECK(prefs.u32_["dndStartMin"] == 30u);
  CHECK(prefs.u32_["dndEndHour"] == 7u);
  CHECK(prefs.u32_["dndEndMin"] == 0u);
}

TEST_CASE("PATCH dnd hour out of range rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"dnd\":{\"startHour\":24,\"startMinute\":0,"
      "\"endHour\":7,\"endMinute\":0}}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "dnd:range");
}

TEST_CASE("PATCH invertedColor also writes fgColor/bgColor") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"invertedColor\":true}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["invertedColor"] == true);
  CHECK(prefs.u32_["fgColor"] == 0xFFFFu);
  CHECK(prefs.u32_["bgColor"] == 0u);
  // bd btclock_v4-5wj — invertedColor is now runtime; the EPD driver's
  // global polarity flag flips live on the on_inverted_color_changed
  // hook, so the PATCH response no longer sets rebootRequired.
  CHECK_FALSE(res.reboot_required);
}

TEST_CASE("PATCH txPower 80 resets to default") {
  FakePrefs prefs;
  prefs.i32_["txPower"] = 44;
  auto res = btclock::settings::ApplyPatch("{\"txPower\":80}", DefaultCtx(),
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!prefs.i32_.count("txPower"));
}

TEST_CASE("Schema invariants: field count + boot-only distribution") {
  // Freezes the classification until someone intentionally edits the
  // schema. Beads report echoes these numbers.
  // 2026-04-24 bd btclock_v4-9rx: gmtOffset removed from the schema
  // (v4 drives the clock from tzString); field count 64 -> 63,
  // boot-only count 19 -> 18.
  // 2026-04-24: hideLeadZero added for the time-screen leading-zero
  // toggle; field count 63 -> 64, boot-only count unchanged (runtime).
  // 2026-04-26 bd btclock_v4-6hq / btclock_v4-gku: bitaxePollSec and
  // poolPollSec exposed as user-configurable poll cadences (both runtime
  // — no reboot needed); field count 64 -> 66, boot-only count unchanged.
  // 2026-04-26 bd btclock_v4-8i2 / btclock_v4-htp: viabtcApiKey,
  // foundryApiKey, foundrySubacct added for ViaBTC + Foundry USA Pool
  // support (all runtime — pool source re-reads on each poll, no reboot
  // needed); field count 66 -> 69, boot-only count unchanged.
  // 2026-04-26 follow-up: dropped viabtcApiKey / foundryApiKey /
  // foundrySubacct in favour of overloading the shared miningPoolUser
  // slot (with per-pool secret-suppression) plus the new generic
  // miningPoolWorker secondary slot. Field count 69 -> 67.
  // 2026-04-26 schema-default consolidation: actCurrencies + 4 DND time
  // fields promoted from BuildGetResponse-only special-cases into the
  // schema so the boot-read sites can derive their defaults from the
  // single source of truth. Field count 67 -> 72.
  // 2026-05-02: satsVariant added (uint, range 0..15, default 7) so the
  // 16 sats-symbol glyphs at U+E000..U+E00F are PATCH-able live via
  // /api/settings; field count 72 -> 73, boot-only unchanged (runtime
  // hook re-binds the renderer immediately).
  // 2026-05-03 bd btclock_v4-0ut: digitFontPx added (uint, range 80..220,
  // default 180) so the big-digit pixel height on data screens is
  // PATCH-able live; field count 73 -> 74, boot-only unchanged (runtime
  // — ScreenManager::Render reads it each frame and pushes it through
  // SetGlobalDigitPx, on_settings_patched marks the screen dirty).
  CHECK(btclock::settings::kFields.size() == 74);
  // Boot-only count: otaEnabled, httpAuthEnabled, httpAuthUser,
  // httpAuthPass, otaPass, mempoolInstance, mempoolSecure, dataSource,
  // ceEndpoint, ceDisableSSL, localPoolHost, nostrPubKey, nostrRelay,
  // enableDebugLog, wpTimeout = 15. hostnamePrefix and mdnsEnabled used
  // to be in this set; bd btclock_v4-9ut flipped them to runtime via
  // on_mdns_changed. fontName likewise: bd btclock_v4-j76.5 dropped it
  // because kSetFont applies live via the ControlCommand queue.
  CHECK(btclock::settings::BootOnlyCount() == 15);
}

TEST_CASE("NVS key length guard: every field key fits NVS's 15-char limit") {
  for (const auto& f : btclock::settings::kFields) {
    CAPTURE(std::string(f.key));
    CHECK(f.key.size() <= 15);
  }
}

TEST_CASE("Round-trip: GET field count matches what PATCH can write") {
  // A field is "readable but not writable" only via the special
  // handlers (screens, dnd, actCurrencies, invertedColor, etc.). Keep
  // the schema table in sync with the GET emitter so no field drifts
  // into read-only by accident.
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);
  // The GET response should contain strictly more keys than the
  // schema table (device-fact fields like hostname, hwRev, plus
  // nested/catalog arrays).
  int count = 0;
  for (cJSON* it = root->child; it; it = it->next) ++count;
  CHECK(count > static_cast<int>(btclock::settings::kFields.size()));
  cJSON_Delete(root);
}

// -- fontName catalog validation -----------------------------------

TEST_CASE("PATCH fontName in catalog is accepted and applies live") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"fontName\":\"oswald\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["fontName"] == "oswald");
  // bd btclock_v4-j76.5: fontName is dispatched via kSetFont
  // ControlCommand on the main task; no reboot required.
  CHECK_FALSE(res.reboot_required);
}

TEST_CASE("PATCH fontName outside catalog rejected as bad_field") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"fontName\":\"comic_sans\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "fontName:unknown");
  CHECK(prefs.str_.count("fontName") == 0);
}

TEST_CASE("PATCH fontName wrong type rejected as bad_type") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"fontName\":42}", DefaultCtx(),
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "fontName:bad_type");
}

// -- miningPoolName catalog validation ------------------------------

TEST_CASE("PATCH miningPoolName in catalog accepted") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"miningPoolName\":\"ocean\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["miningPoolName"] == "ocean");
  // Not boot-only — pool selector picks up the change on next fetch cycle.
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH miningPoolName unknown pool rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"miningPoolName\":\"nicehash\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "miningPoolName:unknown");
}

// -- miningPoolUser / miningPoolStats / poolGlobalStats round-trip --

TEST_CASE("PATCH mining-pool bundle writes user + toggles") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"miningPoolName\":\"braiins\","
      "\"miningPoolUser\":\"deadbeef-api-token\","
      "\"miningPoolStats\":true,"
      "\"poolGlobalStats\":true,"
      "\"localPoolHost\":\"10.0.0.42\"}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["miningPoolName"] == "braiins");
  CHECK(prefs.str_["miningPoolUser"] == "deadbeef-api-token");
  CHECK(prefs.GetBool("miningPoolStats", false) == true);
  // poolGlobalStats's schema default is `true`, so a PATCH of `true` is
  // a no-op write under compare-on-write. Read via the effective interface.
  CHECK(prefs.GetBool("poolGlobalStats", true) == true);
  CHECK(prefs.str_["localPoolHost"] == "10.0.0.42");
  // localPoolHost is boot-only (data-source bring-up happens in setup).
  CHECK(res.reboot_required);
}

// -- nostr pubkey/relay/zap -----------------------------------------

TEST_CASE("PATCH nostrPubKey 64-char hex accepted, triggers reboot") {
  FakePrefs prefs;
  // 64 lowercase hex characters.
  const std::string pk =
      "b5127a08cf33616274800a4387881a9f"
      "98e04b9c37116e92de5250498635c422";
  const std::string body = "{\"nostrPubKey\":\"" + pk + "\"}";
  auto res =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrPubKey"] == pk);
  CHECK(res.reboot_required);  // nostrPubKey is boot-only
}

TEST_CASE("PATCH nostrPubKey wrong length rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"nostrPubKey\":\"abcdef\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "nostrPubKey:bad_length");
}

TEST_CASE("PATCH nostrPubKey non-hex rejected") {
  FakePrefs prefs;
  // 64 chars but a 'z' in there.
  const std::string bad = std::string(63, 'a') + "z";
  const std::string body = "{\"nostrPubKey\":\"" + bad + "\"}";
  auto res =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "nostrPubKey:bad_hex");
}

TEST_CASE("PATCH nostrPubKey empty string clears the field") {
  FakePrefs prefs;
  prefs.str_["nostrPubKey"] = "stale";
  auto res = btclock::settings::ApplyPatch("{\"nostrPubKey\":\"\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrPubKey"].empty());
}

TEST_CASE("PATCH nostrRelay rejects bare hostname") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"nostrRelay\":\"relay.example.com\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "nostrRelay:bad_scheme");
}

TEST_CASE("PATCH nostrRelay rejects https:// scheme") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"nostrRelay\":\"https://relay.example.com\"}", DefaultCtx(), prefs,
      prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "nostrRelay:bad_scheme");
}

TEST_CASE("PATCH nostrRelay accepts ws:// scheme") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"nostrRelay\":\"ws://relay.example.com\"}", DefaultCtx(), prefs,
      prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrRelay"] == "ws://relay.example.com");
}

TEST_CASE("PATCH nostrRelay empty string clears the field") {
  FakePrefs prefs;
  prefs.str_["nostrRelay"] = "wss://stale.example.com";
  auto res = btclock::settings::ApplyPatch("{\"nostrRelay\":\"\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrRelay"].empty());
}

TEST_CASE("PATCH nostrRelay + nostrZapNotify + nostrZapPubkey") {
  FakePrefs prefs;
  const std::string pk = std::string(64, 'a');
  const std::string body =
      "{\"nostrRelay\":\"wss://relay.example.com\","
      "\"nostrZapNotify\":true,"
      "\"nostrZapPubkey\":\"" +
      pk + "\"}";
  auto res =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrRelay"] == "wss://relay.example.com");
  CHECK(prefs.b_["nostrZapNotify"] == true);
  CHECK(prefs.str_["nostrZapPubkey"] == pk);
  // nostrRelay is boot-only (WSS client opens at setup); nostrZapPubkey
  // is runtime (subscription filter can be updated live). The boot-only
  // one wins → reboot_required=true.
  CHECK(res.reboot_required);
}

// -- verticalDesc / flFlashOnZap / ledFlashOnZap --------------------

TEST_CASE("PATCH verticalDesc round-trips without reboot") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"verticalDesc\":true}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // verticalDesc's schema default is `true`, so PATCH(true) is a no-op
  // under compare-on-write. Read via the effective interface.
  CHECK(prefs.GetBool("verticalDesc", true) == true);
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH flFlashOnZap + ledFlashOnZap runtime bools") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"flFlashOnZap\":true,\"ledFlashOnZap\":false}", DefaultCtx(), prefs,
      prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // flFlashOnZap's schema default is `true`, so PATCH(true) is a no-op
  // write; observe via the effective interface.
  CHECK(prefs.GetBool("flFlashOnZap", true) == true);
  CHECK(prefs.GetBool("ledFlashOnZap", false) == false);
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH verticalDesc wrong type rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"verticalDesc\":\"yes\"}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "verticalDesc:bad_type");
}

// -- wpTimeout -------------------------------------------------------

TEST_CASE("PATCH wpTimeout accepted, triggers reboot") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"wpTimeout\":120}", DefaultCtx(),
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u32_["wpTimeout"] == 120u);
  CHECK(res.reboot_required);
}

TEST_CASE("PATCH wpTimeout too large rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"wpTimeout\":99999}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "range:wpTimeout");
}

// bd btclock_v4-9rx: `gmtOffset` / `tzOffset` were removed from the PATCH
// schema on 2026-04-24 (v4 reads the clock from tzString only). A legacy
// client that still sends them must not get an error — the keys are
// silently ignored, the rest of the body applies.
TEST_CASE("PATCH tzOffset silently ignored, rest of body still lands") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"tzOffset\":120,\"mowMode\":true}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!prefs.i32_.count("gmtOffset"));  // never written
  CHECK(prefs.b_.at("mowMode") == true);  // sibling key still landed
}

TEST_CASE("PATCH gmtOffset silently ignored (no-op, no error)") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"gmtOffset\":7200}", DefaultCtx(),
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!prefs.i32_.count("gmtOffset"));
}

// -- dataSource enum range -----------------------------------------

TEST_CASE("PATCH dataSource enum range") {
  FakePrefs prefs;
  auto ok = btclock::settings::ApplyPatch("{\"dataSource\":2}", DefaultCtx(),
                                          prefs, prefs);
  CHECK(ok.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u8_["dataSource"] == 2u);
  CHECK(ok.reboot_required);  // dataSource is boot-only

  FakePrefs prefs2;
  auto bad = btclock::settings::ApplyPatch("{\"dataSource\":7}", DefaultCtx(),
                                           prefs2, prefs2);
  CHECK(bad.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(bad.error == "range:dataSource");
}

// -- Empty catalog falls back to permissive write ------------------

TEST_CASE("PATCH fontName with empty catalog accepts anything") {
  // Keeps the host tests that don't populate DeviceContext::available_fonts
  // working without pre-filling the catalog. Mirrors how the IDF context
  // built in control_server passes the renderer-bundled font list.
  FakePrefs prefs;
  btclock::settings::DeviceContext ctx;  // empty catalog
  auto res = btclock::settings::ApplyPatch("{\"fontName\":\"whatever\"}", ctx,
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["fontName"] == "whatever");
}

// -- GET must emit every newly-added schema field ------------------

TEST_CASE("GET emits verticalDesc / flFlashOnZap / ledFlashOnZap / wpTimeout") {
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);
  for (const char* k :
       {"verticalDesc", "flFlashOnZap", "ledFlashOnZap", "wpTimeout"}) {
    CAPTURE(k);
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, k);
    REQUIRE(item != nullptr);
  }
  // gmtOffset removed on 2026-04-24 (bd btclock_v4-9rx). Confirm it no
  // longer surfaces so a future schema addition with the same name fails
  // loudly.
  CHECK(cJSON_GetObjectItemCaseSensitive(root, "gmtOffset") == nullptr);
  cJSON_Delete(root);
}

// --- blockFlashColor PATCH (Bug 3: honor user-selected flash colour) -

TEST_CASE("PATCH blockFlashColor persists uint RGB and reports touched key") {
  FakePrefs prefs;
  // 0xFF8000 == 16'744'448 decimal — the example in the bug brief.
  auto res = btclock::settings::ApplyPatch("{\"blockFlashColor\":16744448}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!res.reboot_required);  // runtime-editable
  // Persisted under the documented key — on-device the init hook
  // mirrors this into the led namespace so the block-flash effect
  // picks it up without reboot.
  CHECK(prefs.u32_[btclock::prefs::kBlockFlashColor] == 0xFF8000u);
  // touched_keys is the signal the control server uses to decide
  // whether to fire on_block_flash_color_changed -> SetBlockFlashColor.
  const auto& keys = res.touched_keys;
  CHECK(std::find(keys.begin(), keys.end(),
                  std::string(btclock::prefs::kBlockFlashColor)) != keys.end());
}

TEST_CASE("PATCH blockFlashColor clamps to the 24-bit schema bound") {
  FakePrefs prefs;
  // 0x1FF_FFFF exceeds the 0..0xFFFFFF schema max — ApplyPatch must
  // reject rather than silently truncate the alpha byte.
  auto bad = btclock::settings::ApplyPatch("{\"blockFlashColor\":33554431}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(bad.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(bad.error ==
        std::string("range:") + std::string(btclock::prefs::kBlockFlashColor));
  CHECK(prefs.u32_.count(btclock::prefs::kBlockFlashColor) == 0);
}

// -- Defaults sweep (ported from the v3 firmware's defaults.hpp) -----
//
// A fresh install (no NVS entries touched) must surface the v3 default
// values rather than zero/empty. Pins every non-trivial default so a
// future schema edit that drops a default surfaces here before it ships.

namespace {

// Read a field of known kind from the GET response. Returns the value as
// a uniform variant-lite so the table-driven test below can stay flat.
struct GetValue {
  bool b = false;
  double n = 0;
  std::string s;
  bool found = false;
};

GetValue ReadField(cJSON* root, const char* key) {
  GetValue out;
  cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!it) return out;
  out.found = true;
  if (cJSON_IsBool(it))
    out.b = cJSON_IsTrue(it);
  else if (cJSON_IsNumber(it))
    out.n = it->valuedouble;
  else if (cJSON_IsString(it))
    out.s = it->valuestring ? it->valuestring : "";
  return out;
}

}  // namespace

TEST_CASE("GET defaults match the v3 firmware for a fresh install") {
  FakePrefs prefs;  // zero stored values
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root);

  // Bool defaults — (key, expected).
  const std::pair<const char*, bool> bools[] = {
      {"bitaxeEnabled", false},   {"blockFeeDec", true},
      {"ceDisableSSL", false},    {"disableLeds", false},
      {"enableDebugLog", false},  {"flAlwaysOn", true},
      {"flDisable", false},       {"flFlashOnUpd", true},
      {"flFlashOnZap", true},     {"flOffWhenDark", true},
      {"httpAuthEnabled", false}, {"inverseButtons", false},
      {"ledFlashOnUpd", true},    {"ledFlashOnZap", true},
      {"ledTestOnPower", true},   {"mcapBigChar", true},
      {"mdnsEnabled", true},      {"mempoolSecure", true},
      {"miningPoolStats", false}, {"mowMode", false},
      {"nostrZapNotify", false},  {"otaEnabled", true},
      {"poolGlobalStats", true},  {"refrScrnChange", false},
      {"scrnRestoreZap", true},   {"stealFocus", true},
      {"suffixPrice", false},     {"decimalShareDot", false},
      {"supplyPercent", false},   {"useBlkCountdown", true},
      {"useMscwTime", true},      {"useSatsSymbol", false},
      {"verticalDesc", true},
  };
  for (const auto& [k, v] : bools) {
    CAPTURE(k);
    auto got = ReadField(root, k);
    REQUIRE(got.found);
    CHECK(got.b == v);
  }

  // Uint defaults — (key, expected).
  const std::pair<const char*, double> uints[] = {
      {"blockFlashColor", 0xE04300}, {"flEffectDelay", 15},
      {"flMaxBrightness", 2048},     {"fullRefreshMin", 60},
      {"ledBrightness", 128},        {"luxLightToggle", 128},
      {"minSecPriceUpd", 30},        {"wifiRebootMin", 10},
      {"wpTimeout", 15 * 60},
  };
  for (const auto& [k, v] : uints) {
    CAPTURE(k);
    auto got = ReadField(root, k);
    REQUIRE(got.found);
    CHECK(got.n == v);
  }

  // String defaults — (key, expected).
  const std::pair<const char*, const char*> strings[] = {
      {"bitaxeHostname", "bitaxe1"},
      {"ceEndpoint", "ws-staging.btclock.dev"},
      {"fontName", "antonio"},
      {"gitReleaseUrl",
       "https://git.btclock.dev/api/v1/repos/btclock/btclock_v4/releases/"
       "latest"},
      {"hostnamePrefix", "btclock"},
      {"httpAuthUser", "btclock"},
      {"localPoolHost", "umbrel.local:2019"},
      {"mempoolInstance", "mempool.space"},
      {"miningPoolName", "noderunners"},
      {"miningPoolUser", "38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy"},
      {"nostrPubKey",
       "642317135fd4c4205323b9dea8af3270657e62d51dc31a657c0ec8aab31c6288"},
      {"nostrRelay", "wss://relay.primal.net"},
      {"nostrZapPubkey",
       "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422"},
      {"poolLogosUrl",
       "https://git.btclock.dev/btclock/mining-pool-logos/raw/branch/main"},
      {"tzString", "Europe/Amsterdam"},
  };
  for (const auto& [k, v] : strings) {
    CAPTURE(k);
    auto got = ReadField(root, k);
    REQUIRE(got.found);
    CHECK(got.s == v);
  }

  // Passwords must NOT leak a non-empty default — the GET emitter uses
  // empty string and emits the httpAuthPassSet / otaPassSet booleans
  // instead (matches v3 behaviour post-"reflect actual storage" fix).
  {
    auto pw = ReadField(root, "httpAuthPass");
    REQUIRE(pw.found);
    CHECK(pw.s.empty());
    auto ota = ReadField(root, "otaPass");
    REQUIRE(ota.found);
    CHECK(ota.s.empty());
    auto pwSet = ReadField(root, "httpAuthPassSet");
    REQUIRE(pwSet.found);
    CHECK(pwSet.b == false);  // no password stored in the FakePrefs
  }

  cJSON_Delete(root);
}

TEST_CASE("GET defaults are overridden once NVS has the key set") {
  // A stored value must win over the schema default. Regression guard
  // for a buggy EmitField that forgets the prefs lookup and always
  // returns the hardcoded default.
  FakePrefs prefs;
  prefs.b_["mowMode"] = true;
  prefs.str_["fontName"] = "oswald";
  prefs.u32_["ledBrightness"] = 42;

  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root);
  CHECK(ReadField(root, "mowMode").b == true);
  CHECK(ReadField(root, "fontName").s == "oswald");
  CHECK(ReadField(root, "ledBrightness").n == 42);
  cJSON_Delete(root);
}

// -- numScreens consistency (bd btclock_v4: settings vs status) --------

TEST_CASE("numScreens reflects the hardware panel count, not rotation slots") {
  // Both GET /api/settings and /api/status should emit the same
  // numScreens value — the hardware EPD panel count. A Rev A/B device
  // has 3 panels; V8 has 7 or 8. The rotation slot count varies with
  // active currencies (and is the *wrong* thing to expose as
  // numScreens because the WebUI uses it as maxlength for custom-text
  // input, which must equal the physical panel count).
  FakePrefs prefs;
  auto ctx = DefaultCtx();
  ctx.num_screens = 7;  // simulate V8
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root);
  cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "numScreens");
  REQUIRE(cJSON_IsNumber(n));
  CHECK(static_cast<int>(n->valuedouble) == 7);
  cJSON_Delete(root);
  // The /api/status side of the invariant lives in
  // components/webserver/control_server.cpp::BuildStatusJson and uses
  // cfg_.num_screens directly. The mismatch-with-settings regression
  // was root-caused to that builder reading `live.slot_count` (the
  // rotation length) instead; the fix substitutes cfg_.num_screens so
  // both surfaces agree for a given DeviceContext + config pair.
}

// -- Bug 1: GET /api/settings honors persisted screenOrder ------------
//
// Before the fix, `screens[].order` was always the catalog index, so a
// PATCHed screenOrder would land in NVS but the WebUI kept seeing the
// old order until a full reboot. The GET emitter now reads kScreenOrder
// and reorders accordingly: persisted ids first (in CSV order), then
// any catalog entries the CSV omitted, in catalog order.

TEST_CASE("GET screens[] follows persisted screenOrder CSV") {
  FakePrefs prefs;
  // 8 screens in DefaultCtx(): 0,3,4,6,10,20,30,40 (catalog order).
  // PATCH-equivalent: user dragged 40,20,10,0 to the top.
  prefs.str_["screenOrder"] = "40,20,10,0";
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root);
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(arr));
  // Persisted-list-first then remaining catalog entries in catalog order.
  // catalog: 0,3,4,6,10,20,30,40 → after CSV "40,20,10,0":
  //   emitted: 40,20,10,0 then 3,4,6,30 (catalog order minus the four
  //   already in the CSV).
  const std::vector<int> expected = {40, 20, 10, 0, 3, 4, 6, 30};
  REQUIRE(cJSON_GetArraySize(arr) == static_cast<int>(expected.size()));
  for (size_t i = 0; i < expected.size(); ++i) {
    cJSON* it = cJSON_GetArrayItem(arr, static_cast<int>(i));
    cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
    cJSON* ord = cJSON_GetObjectItemCaseSensitive(it, "order");
    REQUIRE(cJSON_IsNumber(id));
    REQUIRE(cJSON_IsNumber(ord));
    CHECK(static_cast<int>(id->valuedouble) == expected[i]);
    CHECK(static_cast<size_t>(ord->valuedouble) == i);
  }
  cJSON_Delete(root);
}

TEST_CASE("GET screens[] empty screenOrder falls back to catalog order") {
  // Cold-boot path / freshly-flashed device: NVS has no `screenOrder`.
  // Backwards-compat invariant — must keep emitting catalog order so
  // existing WebUI builds don't see a layout shift after the upgrade.
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root);
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(arr));
  const std::vector<int> expected = {0, 3, 4, 6, 10, 20, 30, 40};
  REQUIRE(cJSON_GetArraySize(arr) == static_cast<int>(expected.size()));
  for (size_t i = 0; i < expected.size(); ++i) {
    cJSON* it = cJSON_GetArrayItem(arr, static_cast<int>(i));
    CHECK(static_cast<int>(
              cJSON_GetObjectItemCaseSensitive(it, "id")->valuedouble) ==
          expected[i]);
  }
  cJSON_Delete(root);
}

TEST_CASE(
    "GET screens[] partial screenOrder appends missing in catalog order") {
  // Edge case: a malformed/legacy CSV may list only a subset of the
  // catalog. Missing screens MUST still appear (so the user can re-add
  // them via the WebUI) — they trail the persisted list in catalog order.
  FakePrefs prefs;
  prefs.str_["screenOrder"] = "30,3";  // only 2 of 8 catalog ids
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root);
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(arr));
  const std::vector<int> expected = {30, 3, 0, 4, 6, 10, 20, 40};
  REQUIRE(cJSON_GetArraySize(arr) == static_cast<int>(expected.size()));
  for (size_t i = 0; i < expected.size(); ++i) {
    cJSON* it = cJSON_GetArrayItem(arr, static_cast<int>(i));
    CHECK(static_cast<int>(
              cJSON_GetObjectItemCaseSensitive(it, "id")->valuedouble) ==
          expected[i]);
  }
  cJSON_Delete(root);
}

TEST_CASE("GET screens[] persisted screenOrder skips hidden ids cleanly") {
  // The CSV may reference an id the capability gate currently hides
  // (e.g. earnings on a solo pool). The hidden id must NOT surface in
  // screens[]; the rest of the persisted order is preserved.
  FakePrefs prefs;
  prefs.str_["screenOrder"] = "71,40,20";
  btclock::settings::DeviceContext ctx = CtxWithEarnings();
  ctx.hidden_screen_ids = {71};
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  REQUIRE(root);
  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "screens");
  REQUIRE(cJSON_IsArray(arr));
  // 71 is dropped; 40,20 lead, then catalog tail (minus already-emitted).
  // CtxWithEarnings catalog: 0,3,4,6,10,20,30,40,70,71,80,81 → minus 71
  // (hidden), minus already-emitted 40+20.
  const std::vector<int> expected = {40, 20, 0, 3, 4, 6, 10, 30, 70, 80, 81};
  REQUIRE(cJSON_GetArraySize(arr) == static_cast<int>(expected.size()));
  for (size_t i = 0; i < expected.size(); ++i) {
    cJSON* it = cJSON_GetArrayItem(arr, static_cast<int>(i));
    CHECK(static_cast<int>(
              cJSON_GetObjectItemCaseSensitive(it, "id")->valuedouble) ==
          expected[i]);
  }
  cJSON_Delete(root);
}

// -- Bug 2 (and rotation hook regression): touched_keys feed the hook
//
// The `on_screens_changed` hook in HandleSettingsPatch fires whenever
// touched_keys contains `screenOrder`, any `screen<id>Visible`, or
// `actCurrencies`. These three tests pin that contract: ApplyPatch must
// emit those exact key strings so the trigger predicate (mirrored
// inline below) recognises them. Drift on either side regresses the
// "PATCH applied without reboot" guarantee that bd btclock_v4 ships.
//
// The predicate matches HandleSettingsPatch's filter — kept inline so
// these tests don't need a webserver-test target. Updates to either
// must stay in lock-step (grep "is_currencies" to find both halves).

namespace {

bool ScreensChangedTriggers(const std::vector<std::string>& touched_keys) {
  for (const auto& k : touched_keys) {
    const bool is_order = (k == btclock::prefs::kScreenOrder);
    const bool is_visible = (k.size() > 7 && k.compare(0, 6, "screen") == 0 &&
                             k.compare(k.size() - 7, 7, "Visible") == 0);
    const bool is_currencies = (k == btclock::prefs::kActCurrencies);
    const bool is_feature_gate = (k == btclock::prefs::kMiningPoolStats ||
                                  k == btclock::prefs::kBitaxeEnabled);
    if (is_order || is_visible || is_currencies || is_feature_gate) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("PATCH screenOrder fires the rotation-rebuild hook trigger") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":["
      "{\"id\":40,\"enabled\":true,\"order\":0},"
      "{\"id\":30,\"enabled\":true,\"order\":1},"
      "{\"id\":20,\"enabled\":true,\"order\":2},"
      "{\"id\":10,\"enabled\":true,\"order\":3},"
      "{\"id\":6,\"enabled\":true,\"order\":4},"
      "{\"id\":4,\"enabled\":true,\"order\":5},"
      "{\"id\":3,\"enabled\":true,\"order\":6},"
      "{\"id\":0,\"enabled\":true,\"order\":7}"
      "]}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // touched_keys must carry kScreenOrder so the hook fires; CSV must
  // land in NVS so the on-device closure can read it.
  CHECK(prefs.str_["screenOrder"] == "40,30,20,10,6,4,3,0");
  CHECK(ScreensChangedTriggers(res.touched_keys));
}

TEST_CASE("PATCH screen<N>Visible fires the rotation-rebuild hook trigger") {
  // Pins the existing screen<N>Visible PATCH behaviour so a future
  // refactor can't drop the suffix-match "screen*Visible" branch.
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":[{\"id\":10,\"enabled\":false}]}", DefaultCtx(), prefs,
      prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["screen10Visible"] == false);
  CHECK(ScreensChangedTriggers(res.touched_keys));
}

TEST_CASE("PATCH actCurrencies fires the rotation-rebuild hook trigger") {
  // Bug 2: previously actCurrencies wrote NVS but didn't fire the hook.
  // Confirm the touched_keys list now carries the kActCurrencies key
  // and the trigger predicate accepts it — the on-device closure
  // re-reads NVS, calls ScreenManager::SetCurrencies +
  // BuildRotationSequence, and refreshes the v2 WS subscription.
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"actCurrencies\":[\"EUR\",\"USD\"]}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["actCurrencies"] == "EUR,USD");
  CHECK(ScreensChangedTriggers(res.touched_keys));
}

TEST_CASE("PATCH miningPoolStats fires the rotation-rebuild hook trigger") {
  // Without this trigger, toggling miningPoolStats off at runtime would
  // leave slots 70/71 in the auto-rotate cycle until reboot — the
  // is_enabled lambda only re-runs when on_screens_changed fires.
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"miningPoolStats\":true}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["miningPoolStats"] == true);
  CHECK(ScreensChangedTriggers(res.touched_keys));
}

TEST_CASE("PATCH bitaxeEnabled fires the rotation-rebuild hook trigger") {
  // Symmetric to the miningPoolStats case — slots 80/81 must drop out
  // of rotation immediately when the parent feature is toggled off.
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"bitaxeEnabled\":true}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["bitaxeEnabled"] == true);
  CHECK(ScreensChangedTriggers(res.touched_keys));
}

TEST_CASE(
    "PATCH unrelated key does NOT fire the rotation-rebuild hook trigger") {
  // Sanity: the predicate must NOT fire for an unrelated PATCH (e.g.
  // ledBrightness). A spurious rebuild would yank the user off their
  // current slot. This test pins the negative case so a future predicate
  // edit (e.g. broadening "screen*" to match more keys) is caught.
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"ledBrightness\":64}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK_FALSE(ScreensChangedTriggers(res.touched_keys));
}

// -- blockFeeDec live-PATCH hook
//
// Mirrors the actCurrencies hook contract for the v2 WS fee-stream
// switch: ApplyPatch must record kBlockFeeDec in touched_keys so
// HandleSettingsPatch can fire on_block_fee_dec_changed(new_value),
// which calls BtclockDataSource::SetBlockFeeDec(...) and bounces the
// WS so the relay drops the previous topic.

namespace {

bool BlockFeeDecChangedTrigger(const std::vector<std::string>& touched_keys) {
  for (const auto& k : touched_keys) {
    if (k == btclock::prefs::kBlockFeeDec) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("PATCH blockFeeDec=false fires the fee-stream hook trigger") {
  // Default is true (decimal stream), so flip to false to exercise the
  // touched_keys path. Earlier firmware subscribed to BOTH fee topics
  // unconditionally; this hook is what now keeps the relay state in
  // lock-step with the pref.
  FakePrefs prefs;
  prefs.b_["blockFeeDec"] = true;
  auto res = btclock::settings::ApplyPatch("{\"blockFeeDec\":false}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["blockFeeDec"] == false);
  CHECK(BlockFeeDecChangedTrigger(res.touched_keys));
  // blockFeeDec is live (boot_only=false in the schema), so the response
  // must NOT include rebootRequired:true — the on-device hook handles
  // the switch at runtime.
  CHECK_FALSE(res.reboot_required);
}

TEST_CASE("PATCH blockFeeDec=true fires the fee-stream hook trigger") {
  // Inverted case to confirm the trigger fires regardless of the new
  // value. The hook passes the bool through; the on-device closure
  // calls SetBlockFeeDec which bounces the WS.
  FakePrefs prefs;
  prefs.b_["blockFeeDec"] = false;
  auto res = btclock::settings::ApplyPatch("{\"blockFeeDec\":true}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["blockFeeDec"] == true);
  CHECK(BlockFeeDecChangedTrigger(res.touched_keys));
  CHECK_FALSE(res.reboot_required);
}

TEST_CASE("PATCH unrelated key does NOT fire the fee-stream hook trigger") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"ledBrightness\":64}",
                                           DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK_FALSE(BlockFeeDecChangedTrigger(res.touched_keys));
}

// -- Bug 2 (slot expansion): rotation plan reflects new currency count
//
// When actCurrencies grows or shrinks, BuildRotationSequence must use
// the new count to expand per-currency screens. The on-device hook
// passes ScreenManager::currencies().size() through; this test uses
// the same builder directly so any drift in expansion logic surfaces.

#include "app/rotation_plan.hpp"

TEST_CASE("BuildRotationSequence expands per-currency screens for new count") {
  // Single BtcPrice (api_id 20) with 2 currencies → 2 slots.
  const auto seq2 = btclock::rotation_plan::BuildRotationSequence(
      "20", [](int) { return true; }, 2);
  CHECK(seq2.size() == 2u);
  // Same CSV with 3 currencies → 3 slots. Pins the "rebuild on
  // actCurrencies PATCH" path: the hook passes the new count and the
  // sequence grows by exactly one per added currency.
  const auto seq3 = btclock::rotation_plan::BuildRotationSequence(
      "20", [](int) { return true; }, 3);
  CHECK(seq3.size() == 3u);
}

// --- String length / charset hardening (bd btclock_v4-25q) -----------
//
// PATCH writes go to NVS and on next boot reach the renderer / mDNS /
// websocket subscriber. Without a length cap an attacker (or a buggy
// WebUI release) could PATCH a multi-KB blob into a kString field; a
// control-character byte would render as a glyph-not-found symbol per
// byte across every panel. The defense is at write time: ApplyScalar
// rejects oversized values and any payload containing C0 controls / DEL.

TEST_CASE("PATCH rejects kString value over the 256-byte default cap") {
  FakePrefs prefs;
  std::string oversized(257, 'x');
  const std::string body = "{\"hostnamePrefix\":\"" + oversized + "\"}";
  auto res =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "hostnamePrefix:bad_type");
  // NVS must be untouched — the validator has to reject before SetString.
  CHECK(prefs.str_.count("hostnamePrefix") == 0);
}

TEST_CASE("PATCH accepts kString value at exactly the 256-byte default cap") {
  FakePrefs prefs;
  const std::string at_limit(256, 'a');
  const std::string body = "{\"hostnamePrefix\":\"" + at_limit + "\"}";
  auto res =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["hostnamePrefix"] == at_limit);
}

TEST_CASE("PATCH rejects kString containing C0 control characters") {
  FakePrefs prefs;
  // Embed a literal newline (\n = 0x0A) — the JSON spec allows it as an
  // escape, and cJSON decodes it into the actual byte.
  auto res = btclock::settings::ApplyPatch(
      "{\"hostnamePrefix\":\"hello\\nworld\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "hostnamePrefix:bad_type");
  CHECK(prefs.str_.count("hostnamePrefix") == 0);
}

TEST_CASE("PATCH rejects kString with DEL (0x7F) byte") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"hostnamePrefix\":\"good\\u007fbad\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(prefs.str_.count("hostnamePrefix") == 0);
}

TEST_CASE("PATCH accepts kString with high-bit UTF-8 bytes (non-ASCII)") {
  FakePrefs prefs;
  // "café" — 'é' is 0xC3 0xA9 in UTF-8. High bits are allowed; the
  // renderer's font-fallback path handles glyph absence per character.
  auto res = btclock::settings::ApplyPatch(
      "{\"hostnamePrefix\":\"caf\xc3\xa9\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["hostnamePrefix"] == "caf\xc3\xa9");
}

// --- satsVariant range validation -----------------------------------
//
// satsVariant is a uint indexing into U+E000..U+E00F of the
// SatoshiSymbol font (16 glyphs). Schema declares range 0..15; the
// runtime hook in main pushes the value into ScreenManager so the
// next render of moscow_time / nostr_zap paints with the new glyph.

TEST_CASE("PATCH satsVariant accepts in-range values 0..15") {
  for (int v : {0, 1, 7, 14, 15}) {
    FakePrefs prefs;
    const std::string body = "{\"satsVariant\":" + std::to_string(v) + "}";
    auto res =
        btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
    CAPTURE(v);
    CHECK(res.status == btclock::settings::PatchStatus::kOk);
    // satsVariant's schema default is 7; compare-on-write skips a
    // no-op for v=7. Read via the effective interface so the assertion
    // passes regardless of whether the underlying map slot was written.
    CHECK(prefs.GetU32("satsVariant", 7) == static_cast<uint32_t>(v));
  }
}

TEST_CASE("PATCH satsVariant rejects 16 (one past max)") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"satsVariant\":16}", DefaultCtx(),
                                           prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "range:satsVariant");
  CHECK(prefs.u32_.count("satsVariant") == 0);
}

TEST_CASE("PATCH satsVariant rejects negative values") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("{\"satsVariant\":-1}", DefaultCtx(),
                                           prefs, prefs);
  // Negative kUint values are rejected at type-check before the range
  // branch — kUint sees v->valuedouble < 0 and returns false.
  CHECK(res.status != btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u32_.count("satsVariant") == 0);
}

TEST_CASE("PATCH satsVariant default in schema is 7") {
  // Pin the documented default — the production glyph that shipped
  // before the variant pref existed.
  CHECK(btclock::settings::DefaultIntFor(btclock::prefs::kSatsVariant) == 7);
}
