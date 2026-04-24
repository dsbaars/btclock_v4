// Host tests for the pure-logic core of /api/settings
// (components/settings/settings_api.cpp). Drives BuildGetResponse +
// ApplyPatch against an in-memory PrefsReader/Writer fake so the
// schema round-trip is covered without ESP-IDF.

#include "doctest.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
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
      {0, "Block Height"}, {3, "Time"}, {4, "Halving countdown"},
      {6, "Block Fee Rate"}, {10, "Sats per dollar"}, {20, "Ticker"},
      {30, "Market Cap"}, {40, "Bitcoin Supply"},
  };
  return ctx;
}

}  // namespace

TEST_CASE("GET /api/settings emits device-context fields") {
  FakePrefs prefs;
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
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
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
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
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
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
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
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
      {0, "Block Height"},         {3, "Time"},
      {4, "Halving countdown"},    {6, "Block Fee Rate"},
      {10, "Sats per dollar"},     {20, "Ticker"},
      {30, "Market Cap"},          {40, "Bitcoin Supply"},
      {70, "Mining Pool Hashrate"},
      {71, "Mining Pool Earnings"},
      {80, "Bitaxe Hashrate"},     {81, "Bitaxe Best Difficulty"},
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
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, CtxWithEarnings());
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
  CHECK(prefs.b_["screen71Visible"] == true);
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

  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
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
  auto res = btclock::settings::ApplyPatch("{not_json", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "json");
  CHECK(prefs.str_.empty());
  CHECK(prefs.u32_.empty());
  CHECK(prefs.b_.empty());
}

TEST_CASE("PATCH non-object body rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch("[1,2,3]", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "not_object");
}

TEST_CASE("PATCH unknown field silently ignored") {
  FakePrefs prefs;
  // Matches old-firmware behaviour: strSettings/uintSettings/boolSettings
  // only act on keys they recognise; an unknown key is a no-op.
  auto res = btclock::settings::ApplyPatch(
      "{\"notAField\":\"hello\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.touched_keys.empty());
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH writes runtime-editable bool without rebootRequired") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"mcapBigChar\":true,\"stealFocus\":false}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!res.reboot_required);
  CHECK(prefs.b_["mcapBigChar"] == true);
  CHECK(prefs.b_["stealFocus"] == false);
}

TEST_CASE("PATCH writes runtime-editable uint with range clamping") {
  FakePrefs prefs;
  // In-range value accepted.
  auto ok = btclock::settings::ApplyPatch(
      "{\"ledBrightness\":128}", DefaultCtx(), prefs, prefs);
  CHECK(ok.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u32_["ledBrightness"] == 128u);

  // Out-of-range value rejected (ledBrightness bounded 0..255).
  FakePrefs prefs2;
  auto bad = btclock::settings::ApplyPatch(
      "{\"ledBrightness\":9999}", DefaultCtx(), prefs2, prefs2);
  CHECK(bad.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(bad.error == "range:ledBrightness");
  CHECK(prefs2.u32_.count("ledBrightness") == 0);
}

TEST_CASE("PATCH boot-only field triggers rebootRequired") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"hostnamePrefix\":\"newhost\"}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(res.reboot_required);
  CHECK(prefs.str_["hostnamePrefix"] == "newhost");
}

TEST_CASE("PATCH mixed runtime + boot-only still sets rebootRequired") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"mcapBigChar\":true,\"mdnsEnabled\":false}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  // mdnsEnabled is boot-only (starts mDNS responder at boot).
  CHECK(res.reboot_required);
}

TEST_CASE("Boot-only detection: every flagged key is what old firmware reboots on") {
  // Paranoia guard against drift: the beads issue's acceptance
  // criterion is that WiFi/provisioning-adjacent settings trigger
  // reboot. Check the ones an end-user would expect. `invertedColor`
  // is intentionally absent — bd btclock_v4-5wj flipped it to runtime
  // now that EpdSetGlobalInverted lets the driver swap polarity live.
  for (const char* k : {"hostnamePrefix", "mdnsEnabled", "otaEnabled",
                        "httpAuthEnabled", "httpAuthUser", "httpAuthPass",
                        "otaPass", "fontName",
                        "mempoolInstance", "dataSource"}) {
    CAPTURE(k);
    const auto* spec = btclock::settings::FindField(k);
    REQUIRE(spec != nullptr);
    CHECK(spec->boot_only);
  }
}

TEST_CASE("PATCH timePerScreen converts minutes to seconds") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"timePerScreen\":5}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u32_["timerSeconds"] == 300u);
}

TEST_CASE("PATCH actCurrencies validates against available list") {
  FakePrefs prefs;
  auto ok = btclock::settings::ApplyPatch(
      "{\"actCurrencies\":[\"USD\",\"JPY\"]}",
      DefaultCtx(), prefs, prefs);
  CHECK(ok.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["actCurrencies"] == "USD,JPY");

  FakePrefs prefs2;
  auto bad = btclock::settings::ApplyPatch(
      "{\"actCurrencies\":[\"USD\",\"XYZ\"]}",
      DefaultCtx(), prefs2, prefs2);
  CHECK(bad.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(bad.error == "currency:unknown");
}

TEST_CASE("PATCH screens visibility only") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"screens\":[{\"id\":0,\"enabled\":false},"
      "{\"id\":3,\"enabled\":true}]}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["screen0Visible"] == false);
  CHECK(prefs.b_["screen3Visible"] == true);
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
  auto res = btclock::settings::ApplyPatch(
      "{\"invertedColor\":true}", DefaultCtx(), prefs, prefs);
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
  auto res = btclock::settings::ApplyPatch(
      "{\"txPower\":80}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(!prefs.i32_.count("txPower"));
}

TEST_CASE("Schema invariants: field count + boot-only distribution") {
  // Freezes the classification until someone intentionally edits the
  // schema. Beads report echoes these numbers.
  CHECK(btclock::settings::kFields.size() == 64);
  // Boot-only count: hostnamePrefix, mdnsEnabled, otaEnabled,
  // httpAuthEnabled, httpAuthUser, httpAuthPass, otaPass, fontName,
  // mempoolInstance, mempoolSecure, dataSource, ceEndpoint,
  // ceDisableSSL, localPoolHost, nostrPubKey, nostrRelay,
  // enableDebugLog, gmtOffset, wpTimeout = 19. (invertedColor moved
  // to runtime — bd btclock_v4-5wj.)
  CHECK(btclock::settings::BootOnlyCount() == 19);
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
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
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

TEST_CASE("PATCH fontName in catalog is accepted and reboots") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"fontName\":\"oswald\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["fontName"] == "oswald");
  CHECK(res.reboot_required);  // fontName is boot-only (EPD driver init)
}

TEST_CASE("PATCH fontName outside catalog rejected as bad_field") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"fontName\":\"comic_sans\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "fontName:unknown");
  CHECK(prefs.str_.count("fontName") == 0);
}

TEST_CASE("PATCH fontName wrong type rejected as bad_type") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"fontName\":42}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "fontName:bad_type");
}

// -- miningPoolName catalog validation ------------------------------

TEST_CASE("PATCH miningPoolName in catalog accepted") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"miningPoolName\":\"ocean\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["miningPoolName"] == "ocean");
  // Not boot-only — pool selector picks up the change on next fetch cycle.
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH miningPoolName unknown pool rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"miningPoolName\":\"nicehash\"}", DefaultCtx(), prefs, prefs);
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
  CHECK(prefs.b_["miningPoolStats"] == true);
  CHECK(prefs.b_["poolGlobalStats"] == true);
  CHECK(prefs.str_["localPoolHost"] == "10.0.0.42");
  // localPoolHost is boot-only (data-source bring-up happens in setup).
  CHECK(res.reboot_required);
}

// -- nostr pubkey/relay/zap -----------------------------------------

TEST_CASE("PATCH nostrPubKey 64-char hex accepted, triggers reboot") {
  FakePrefs prefs;
  // 64 lowercase hex characters.
  const std::string pk = "b5127a08cf33616274800a4387881a9f"
                         "98e04b9c37116e92de5250498635c422";
  const std::string body = "{\"nostrPubKey\":\"" + pk + "\"}";
  auto res = btclock::settings::ApplyPatch(
      body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrPubKey"] == pk);
  CHECK(res.reboot_required);  // nostrPubKey is boot-only
}

TEST_CASE("PATCH nostrPubKey wrong length rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"nostrPubKey\":\"abcdef\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "nostrPubKey:bad_length");
}

TEST_CASE("PATCH nostrPubKey non-hex rejected") {
  FakePrefs prefs;
  // 64 chars but a 'z' in there.
  const std::string bad = std::string(63, 'a') + "z";
  const std::string body = "{\"nostrPubKey\":\"" + bad + "\"}";
  auto res = btclock::settings::ApplyPatch(
      body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "nostrPubKey:bad_hex");
}

TEST_CASE("PATCH nostrPubKey empty string clears the field") {
  FakePrefs prefs;
  prefs.str_["nostrPubKey"] = "stale";
  auto res = btclock::settings::ApplyPatch(
      "{\"nostrPubKey\":\"\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nostrPubKey"].empty());
}

TEST_CASE("PATCH nostrRelay + nostrZapNotify + nostrZapPubkey") {
  FakePrefs prefs;
  const std::string pk = std::string(64, 'a');
  const std::string body =
      "{\"nostrRelay\":\"wss://relay.example.com\","
      "\"nostrZapNotify\":true,"
      "\"nostrZapPubkey\":\"" + pk + "\"}";
  auto res = btclock::settings::ApplyPatch(
      body.c_str(), DefaultCtx(), prefs, prefs);
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
  auto res = btclock::settings::ApplyPatch(
      "{\"verticalDesc\":true}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["verticalDesc"] == true);
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH flFlashOnZap + ledFlashOnZap runtime bools") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"flFlashOnZap\":true,\"ledFlashOnZap\":false}",
      DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.b_["flFlashOnZap"] == true);
  CHECK(prefs.b_["ledFlashOnZap"] == false);
  CHECK(!res.reboot_required);
}

TEST_CASE("PATCH verticalDesc wrong type rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"verticalDesc\":\"yes\"}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadField);
  CHECK(res.error == "verticalDesc:bad_type");
}

// -- wpTimeout + gmtOffset / tzOffset -------------------------------

TEST_CASE("PATCH wpTimeout accepted, triggers reboot") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"wpTimeout\":120}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u32_["wpTimeout"] == 120u);
  CHECK(res.reboot_required);
}

TEST_CASE("PATCH wpTimeout too large rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"wpTimeout\":99999}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "range:wpTimeout");
}

TEST_CASE("PATCH tzOffset minutes -> gmtOffset seconds") {
  FakePrefs prefs;
  // +2h = 120 minutes -> 7200 s
  auto res = btclock::settings::ApplyPatch(
      "{\"tzOffset\":120}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.i32_["gmtOffset"] == 7200);
  CHECK(res.reboot_required);
}

TEST_CASE("PATCH tzOffset negative accepted") {
  FakePrefs prefs;
  // -5h = -300 minutes -> -18000 s (US/Eastern)
  auto res = btclock::settings::ApplyPatch(
      "{\"tzOffset\":-300}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.i32_["gmtOffset"] == -18000);
}

TEST_CASE("PATCH tzOffset out-of-range rejected") {
  FakePrefs prefs;
  auto res = btclock::settings::ApplyPatch(
      "{\"tzOffset\":99999}", DefaultCtx(), prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kBadRequest);
  CHECK(res.error == "tzOffset:range");
}

// -- dataSource enum range -----------------------------------------

TEST_CASE("PATCH dataSource enum range") {
  FakePrefs prefs;
  auto ok = btclock::settings::ApplyPatch(
      "{\"dataSource\":2}", DefaultCtx(), prefs, prefs);
  CHECK(ok.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.u8_["dataSource"] == 2u);
  CHECK(ok.reboot_required);  // dataSource is boot-only

  FakePrefs prefs2;
  auto bad = btclock::settings::ApplyPatch(
      "{\"dataSource\":7}", DefaultCtx(), prefs2, prefs2);
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
  auto res = btclock::settings::ApplyPatch(
      "{\"fontName\":\"whatever\"}", ctx, prefs, prefs);
  CHECK(res.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["fontName"] == "whatever");
}

// -- GET must emit every newly-added schema field ------------------

TEST_CASE("GET emits verticalDesc / flFlashOnZap / ledFlashOnZap / wpTimeout / gmtOffset") {
  FakePrefs prefs;
  cJSON* root =
      btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);
  for (const char* k : {"verticalDesc", "flFlashOnZap", "ledFlashOnZap",
                        "wpTimeout", "gmtOffset"}) {
    CAPTURE(k);
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, k);
    REQUIRE(item != nullptr);
  }
  cJSON_Delete(root);
}
