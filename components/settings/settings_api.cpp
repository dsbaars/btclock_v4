#include "settings/api.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>

#include "cJSON.h"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace settings {

namespace {

// Helpers to add a *typed* field to the JSON response. cJSON's own
// Add* helpers accept a heterogeneous `double` for numbers, which
// loses precision on the 32-bit range we care about (u32 up to 2^32).
// These small wrappers are mostly for readability.
void AddString(cJSON* obj, const char* key, const std::string& val) {
  cJSON_AddStringToObject(obj, key, val.c_str());
}

void AddBool(cJSON* obj, const char* key, bool val) {
  cJSON_AddBoolToObject(obj, key, val);
}

void AddU32(cJSON* obj, const char* key, uint32_t val) {
  cJSON_AddNumberToObject(obj, key, static_cast<double>(val));
}

void AddI32(cJSON* obj, const char* key, int32_t val) {
  cJSON_AddNumberToObject(obj, key, static_cast<double>(val));
}

// Emit the value of a schema field into the GET response. Only called
// for keys the schema table knows about.
void EmitField(cJSON* root, const FieldSpec& f, const PrefsReader& prefs) {
  const char* k = f.key.data();  // string_view is NUL-terminated (points at constexpr literal)
  switch (f.kind) {
    case FieldKind::kString:
      AddString(root, k, prefs.GetString(k, ""));
      break;
    case FieldKind::kUint:
      AddU32(root, k, prefs.GetU32(k, 0));
      break;
    case FieldKind::kInt:
      AddI32(root, k, prefs.GetI32(k, 0));
      break;
    case FieldKind::kUChar:
      AddU32(root, k, prefs.GetU8(k, 0));
      break;
    case FieldKind::kBool:
      AddBool(root, k, prefs.GetBool(k, false));
      break;
  }
}

// Split comma-separated currency string into JSON array. Matches
// old-firmware getActiveCurrencies() which does the same split.
std::vector<std::string> SplitCsv(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

}  // namespace

cJSON* BuildGetResponse(const PrefsReader& prefs, const DeviceContext& ctx) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return nullptr;

  // Device facts — filled in from DeviceContext, not NVS.
  AddI32(root, "numScreens", ctx.num_screens);
  AddString(root, "hostname", ctx.hostname);
  AddString(root, "ip", ctx.ip);
  AddI32(root, "txPower", ctx.tx_power);
  AddString(root, "hwRev", ctx.hw_rev);
  AddString(root, "fsRev", ctx.fs_rev);
  if (!ctx.git_rev.empty()) AddString(root, "gitRev", ctx.git_rev);
  if (!ctx.git_tag.empty()) AddString(root, "gitTag", ctx.git_tag);
  if (!ctx.last_build_time.empty()) {
    AddString(root, "lastBuildTime", ctx.last_build_time);
  }

  // Emit every schema field. Skip a few that the old firmware surfaces
  // under alternate keys — inverted-color goes through a custom path
  // because it drives EPD fg/bg colour in lock-step with the bool.
  for (const auto& f : kFields) EmitField(root, f, prefs);

  // invertedColor, shipped with a device-dependent default. The old
  // firmware derived the default from the current EPD foreground
  // colour; without live EPD state we fall back to true (white-on-
  // black), the default background used by every shipping board.
  // Override via PATCH if a user prefers the inverse.
  AddBool(root, "invertedColor", prefs.GetBool(prefs::kInvertedColor, true));

  // timerSeconds isn't PATCHed directly — the WebUI sends
  // `timePerScreen` (minutes) and the server multiplies by 60.
  AddU32(root, "timerSeconds", prefs.GetU32(prefs::kTimerSeconds, 1800));
  AddBool(root, "timerRunning", true);  // runtime flag, not stored in NVS

  // Catalogue arrays — available fonts / pools / currencies are
  // firmware-fixed; active currencies come from NVS.
  {
    cJSON* arr = cJSON_AddArrayToObject(root, "availableFonts");
    for (const auto& n : ctx.available_fonts) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(n.c_str()));
    }
  }
  {
    cJSON* arr = cJSON_AddArrayToObject(root, "availablePools");
    for (const auto& n : ctx.available_pools) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(n.c_str()));
    }
  }
  {
    cJSON* arr = cJSON_AddArrayToObject(root, "availableCurrencies");
    for (const auto& n : ctx.available_currencies) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(n.c_str()));
    }
  }
  {
    const std::string csv =
        prefs.GetString(prefs::kActCurrencies, "USD,EUR,JPY");
    cJSON* arr = cJSON_AddArrayToObject(root, "actCurrencies");
    for (const auto& code : SplitCsv(csv)) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(code.c_str()));
    }
  }

  // Screens array. `enabled` is a per-screen bool at key
  // screen<ID>Visible (default true). `order` is the index in
  // DeviceContext::screens — the catalogue is fed in already sorted
  // by the current rotation order.
  cJSON* screens_arr = cJSON_AddArrayToObject(root, "screens");
  for (size_t i = 0; i < ctx.screens.size(); ++i) {
    const auto& s = ctx.screens[i];
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", static_cast<double>(s.id));
    cJSON_AddStringToObject(obj, "name", s.name.c_str());
    char vkey[24];
    std::snprintf(vkey, sizeof(vkey), "screen%dVisible", s.id);
    cJSON_AddBoolToObject(obj, "enabled", prefs.GetBool(vkey, true));
    cJSON_AddNumberToObject(obj, "order", static_cast<double>(i));
    cJSON_AddItemToArray(screens_arr, obj);
  }

  // DND nested block. Mirrors old firmware's dnd JSON in settings.cpp.
  cJSON* dnd = cJSON_AddObjectToObject(root, "dnd");
  AddBool(dnd, "enabled", prefs.GetBool(prefs::kDndEnabled, false));
  AddBool(dnd, "dndTimeEnabled", prefs.GetBool(prefs::kDndTimeEnabled, false));
  AddU32(dnd, "startHour", prefs.GetU32(prefs::kDndStartHour, 22));
  AddU32(dnd, "startMinute", prefs.GetU32(prefs::kDndStartMin, 0));
  AddU32(dnd, "endHour", prefs.GetU32(prefs::kDndEndHour, 7));
  AddU32(dnd, "endMinute", prefs.GetU32(prefs::kDndEndMin, 0));

  // Frontlight availability flags drive the WebUI's "Frontlight" panel
  // visibility. Boards without a PCA9685 skip the entire section.
  AddBool(root, "hasFrontlight", ctx.has_frontlight);
  AddBool(root, "hasLightLevel", ctx.has_light_level);

  // Old firmware hides these behind a strlen > 0 check for safety.
  // Matches: never ship the raw password, just a "set" indicator.
  AddBool(root, "httpAuthPassSet",
          !prefs.GetString(prefs::kHttpAuthPass, "").empty());
  AddBool(root, "otaPassSet", !prefs.GetString(prefs::kOtaPass, "").empty());
  return root;
}

namespace {

// Apply a single scalar field by kind. Returns true on success, false
// if the JSON value's type didn't match. Caller is expected to decide
// whether a type mismatch is "silently ignored" (matches old firmware
// — unknown/mistyped keys are skipped rather than rejected).
bool ApplyScalar(const FieldSpec& f, const cJSON* v, PrefsWriter& writer) {
  switch (f.kind) {
    case FieldKind::kString: {
      if (!cJSON_IsString(v) || !v->valuestring) return false;
      writer.SetString(f.key.data(), v->valuestring);
      return true;
    }
    case FieldKind::kUint: {
      if (!cJSON_IsNumber(v) || v->valuedouble < 0) return false;
      const double d = v->valuedouble;
      if (f.max_value != 0 && d > static_cast<double>(f.max_value)) return false;
      if (f.min_value != 0 && d < static_cast<double>(f.min_value)) return false;
      writer.SetU32(f.key.data(), static_cast<uint32_t>(d));
      return true;
    }
    case FieldKind::kInt: {
      if (!cJSON_IsNumber(v)) return false;
      writer.SetI32(f.key.data(), static_cast<int32_t>(v->valuedouble));
      return true;
    }
    case FieldKind::kUChar: {
      if (!cJSON_IsNumber(v) || v->valuedouble < 0) return false;
      const double d = v->valuedouble;
      if (f.max_value != 0 && d > static_cast<double>(f.max_value)) return false;
      writer.SetU8(f.key.data(), static_cast<uint8_t>(d));
      return true;
    }
    case FieldKind::kBool: {
      if (!cJSON_IsBool(v)) return false;
      writer.SetBool(f.key.data(), cJSON_IsTrue(v));
      return true;
    }
  }
  return false;
}

}  // namespace

PatchResult ApplyPatch(const char* body_json, const DeviceContext& ctx,
                       const PrefsReader& prefs, PrefsWriter& writer) {
  (void)prefs;  // currently only used for writes; kept for future hooks

  PatchResult result;
  cJSON* root = cJSON_Parse(body_json);
  if (!root) {
    result.status = PatchStatus::kBadRequest;
    result.error = "json";
    return result;
  }
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    result.status = PatchStatus::kBadRequest;
    result.error = "not_object";
    return result;
  }

  // Walk every top-level key. For schema fields we validate + write.
  // Nested objects (`dnd`) and arrays (`screens`, `actCurrencies`)
  // get their own handlers below.
  for (cJSON* item = root->child; item; item = item->next) {
    if (!item->string) continue;
    const std::string key = item->string;

    if (key == "screens" || key == "dnd" || key == "actCurrencies" ||
        key == "timePerScreen" || key == "txPower" || key == "invertedColor") {
      continue;  // handled below
    }

    const FieldSpec* spec = FindField(key);
    if (!spec) continue;  // unknown field — silent skip (old-firmware behaviour)

    if (!ApplyScalar(*spec, item, writer)) {
      // Type mismatch or out-of-range. Matches old firmware's generic
      // loop which `settings[k].is<T>()` gated each type — a mismatch
      // is silent. Range violations are a new safety net here; keep
      // the silent-skip to avoid breaking WebUI probes.
      if (spec->min_value != 0 || spec->max_value != 0) {
        // Explicit range — surface as error so tests can catch
        // regressions in the WebUI's bounds.
        result.status = PatchStatus::kBadRequest;
        result.error = std::string("range:") + std::string(spec->key);
        cJSON_Delete(root);
        return result;
      }
      continue;
    }
    result.touched_keys.emplace_back(key);
    if (spec->boot_only) result.reboot_required = true;
  }

  // invertedColor: also writes fgColor/bgColor siblings. Matches the
  // old firmware's special case in onApiSettingsPatch.
  {
    cJSON* inv = cJSON_GetObjectItemCaseSensitive(root, "invertedColor");
    if (cJSON_IsBool(inv)) {
      const bool v = cJSON_IsTrue(inv);
      writer.SetBool(prefs::kInvertedColor, v);
      // Match old firmware: white-on-black when inverted, else swap.
      // Numeric colour codes follow GxEPD_WHITE=0xFFFF / GxEPD_BLACK=0.
      writer.SetU32(prefs::kFgColor, v ? 0xFFFFu : 0u);
      writer.SetU32(prefs::kBgColor, v ? 0u : 0xFFFFu);
      result.touched_keys.emplace_back("invertedColor");
      result.reboot_required = true;  // kInvertedColor is boot-only
    }
  }

  // timePerScreen (minutes) -> timerSeconds (seconds). The WebUI sends
  // minutes; we store seconds for compatibility with the old pref.
  {
    cJSON* tps = cJSON_GetObjectItemCaseSensitive(root, "timePerScreen");
    if (cJSON_IsNumber(tps) && tps->valuedouble > 0 &&
        tps->valuedouble <= 60 * 60 /* sanity cap */) {
      writer.SetU32(prefs::kTimerSeconds,
                    static_cast<uint32_t>(tps->valuedouble * 60.0));
      result.touched_keys.emplace_back("timerSeconds");
    }
  }

  // txPower: accept [-1, 78] per old firmware (WIFI_POWER_* enum
  // range) plus the sentinel 80 which means "auto / reset to default".
  {
    cJSON* tx = cJSON_GetObjectItemCaseSensitive(root, "txPower");
    if (cJSON_IsNumber(tx)) {
      const int raw = static_cast<int>(tx->valuedouble);
      if (raw == 80) {
        writer.Remove(prefs::kTxPower);
        result.touched_keys.emplace_back("txPower");
      } else if (raw >= -1 && raw <= 78) {
        writer.SetI32(prefs::kTxPower, raw);
        result.touched_keys.emplace_back("txPower");
      }
    }
  }

  // actCurrencies: WebUI sends an array of codes. Validate + CSV-join
  // before persisting to match the stored form. Unknown codes are
  // rejected so the UI can't write garbage into the active-currency
  // rotation list.
  {
    cJSON* cur = cJSON_GetObjectItemCaseSensitive(root, "actCurrencies");
    if (cJSON_IsArray(cur)) {
      std::set<std::string> valid;
      for (const auto& c : ctx.available_currencies) valid.insert(c);
      std::string joined;
      for (cJSON* it = cur->child; it; it = it->next) {
        if (!cJSON_IsString(it) || !it->valuestring) {
          result.status = PatchStatus::kBadRequest;
          result.error = "currency:not_string";
          cJSON_Delete(root);
          return result;
        }
        std::string code = it->valuestring;
        if (!valid.empty() && !valid.count(code)) {
          result.status = PatchStatus::kBadRequest;
          result.error = "currency:unknown";
          cJSON_Delete(root);
          return result;
        }
        if (!joined.empty()) joined.push_back(',');
        joined.append(code);
      }
      writer.SetString(prefs::kActCurrencies, joined.c_str());
      result.touched_keys.emplace_back(prefs::kActCurrencies);
    }
  }

  // screens: mix of visibility toggles (always accepted) and an
  // optional full reorder. Old firmware validates that *either* every
  // entry carries an `order` *or* none does — partial reorders are a
  // bad-request.
  {
    cJSON* screens = cJSON_GetObjectItemCaseSensitive(root, "screens");
    if (cJSON_IsArray(screens)) {
      // Pass 1: detect reorder vs visibility-only.
      bool any_order = false;
      bool all_order = true;
      int len = 0;
      for (cJSON* s = screens->child; s; s = s->next, ++len) {
        cJSON* order = cJSON_GetObjectItemCaseSensitive(s, "order");
        if (cJSON_IsNumber(order)) any_order = true;
        else all_order = false;
      }
      if (any_order && !all_order) {
        result.status = PatchStatus::kBadRequest;
        result.error = "screens:partial_order";
        cJSON_Delete(root);
        return result;
      }
      if (any_order) {
        std::set<int> catalog;
        for (const auto& c : ctx.screens) catalog.insert(c.id);
        std::set<int> seen_ids;
        std::set<int> seen_orders;
        std::vector<std::pair<int, int>> pairs;
        for (cJSON* s = screens->child; s; s = s->next) {
          cJSON* id = cJSON_GetObjectItemCaseSensitive(s, "id");
          cJSON* order = cJSON_GetObjectItemCaseSensitive(s, "order");
          if (!cJSON_IsNumber(id) || !cJSON_IsNumber(order)) {
            result.status = PatchStatus::kBadRequest;
            result.error = "screens:bad_entry";
            cJSON_Delete(root);
            return result;
          }
          const int iid = static_cast<int>(id->valuedouble);
          const int iord = static_cast<int>(order->valuedouble);
          if (!catalog.empty() && !catalog.count(iid)) {
            result.status = PatchStatus::kBadRequest;
            result.error = "screens:unknown_id";
            cJSON_Delete(root);
            return result;
          }
          if (!seen_ids.insert(iid).second) {
            result.status = PatchStatus::kBadRequest;
            result.error = "screens:dup_id";
            cJSON_Delete(root);
            return result;
          }
          if (iord < 0 || iord >= len) {
            result.status = PatchStatus::kBadRequest;
            result.error = "screens:order_range";
            cJSON_Delete(root);
            return result;
          }
          if (!seen_orders.insert(iord).second) {
            result.status = PatchStatus::kBadRequest;
            result.error = "screens:dup_order";
            cJSON_Delete(root);
            return result;
          }
          pairs.emplace_back(iord, iid);
        }
        if (!catalog.empty() && seen_ids.size() != catalog.size()) {
          result.status = PatchStatus::kBadRequest;
          result.error = "screens:incomplete";
          cJSON_Delete(root);
          return result;
        }
        std::sort(pairs.begin(), pairs.end());
        std::string order_csv;
        for (const auto& p : pairs) {
          if (!order_csv.empty()) order_csv.push_back(',');
          order_csv.append(std::to_string(p.second));
        }
        writer.SetString(prefs::kScreenOrder, order_csv.c_str());
        result.touched_keys.emplace_back(prefs::kScreenOrder);
      }

      // Pass 2: visibility toggles (independent of reorder path).
      for (cJSON* s = screens->child; s; s = s->next) {
        cJSON* id = cJSON_GetObjectItemCaseSensitive(s, "id");
        cJSON* enabled = cJSON_GetObjectItemCaseSensitive(s, "enabled");
        if (!cJSON_IsNumber(id) || !cJSON_IsBool(enabled)) continue;
        const int iid = static_cast<int>(id->valuedouble);
        char vkey[24];
        std::snprintf(vkey, sizeof(vkey), "screen%dVisible", iid);
        writer.SetBool(vkey, cJSON_IsTrue(enabled));
        result.touched_keys.emplace_back(vkey);
      }
    }
  }

  // DND nested block. Old firmware writes hour/minute as a set so we
  // require all four or none.
  {
    cJSON* dnd = cJSON_GetObjectItemCaseSensitive(root, "dnd");
    if (cJSON_IsObject(dnd)) {
      cJSON* te = cJSON_GetObjectItemCaseSensitive(dnd, "dndTimeEnabled");
      if (cJSON_IsBool(te)) {
        writer.SetBool(prefs::kDndTimeEnabled, cJSON_IsTrue(te));
        result.touched_keys.emplace_back(prefs::kDndTimeEnabled);
      }
      cJSON* sh = cJSON_GetObjectItemCaseSensitive(dnd, "startHour");
      cJSON* sm = cJSON_GetObjectItemCaseSensitive(dnd, "startMinute");
      cJSON* eh = cJSON_GetObjectItemCaseSensitive(dnd, "endHour");
      cJSON* em = cJSON_GetObjectItemCaseSensitive(dnd, "endMinute");
      if (cJSON_IsNumber(sh) && cJSON_IsNumber(sm) &&
          cJSON_IsNumber(eh) && cJSON_IsNumber(em)) {
        const auto inRange = [](const cJSON* v, int lo, int hi) {
          return v->valuedouble >= lo && v->valuedouble <= hi;
        };
        if (!inRange(sh, 0, 23) || !inRange(eh, 0, 23) ||
            !inRange(sm, 0, 59) || !inRange(em, 0, 59)) {
          result.status = PatchStatus::kBadRequest;
          result.error = "dnd:range";
          cJSON_Delete(root);
          return result;
        }
        writer.SetU32(prefs::kDndStartHour, static_cast<uint32_t>(sh->valuedouble));
        writer.SetU32(prefs::kDndStartMin, static_cast<uint32_t>(sm->valuedouble));
        writer.SetU32(prefs::kDndEndHour, static_cast<uint32_t>(eh->valuedouble));
        writer.SetU32(prefs::kDndEndMin, static_cast<uint32_t>(em->valuedouble));
        result.touched_keys.emplace_back(prefs::kDndStartHour);
      }
    }
  }

  cJSON_Delete(root);
  return result;
}

}  // namespace settings
}  // namespace btclock
