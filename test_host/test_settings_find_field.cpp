// Host tests for the settings schema lookup helpers (FindField,
// DefaultBoolFor, DefaultIntFor, DefaultStringFor) — the foundation
// every PATCH/GET on /api/settings + every boot-time ReadBool /
// ReadU32 / ReadString call rides on.
//
// Filed under btclock_v4-ajf: a stale coredump on Rev B decoded with a
// SHA-mismatched ELF surfaced a backtrace through FindField with a
// poisoned (0xa5a5…) string_view operand. Root-cause analysis on
// current source ruled out a real lifetime bug (kFields lives in
// .rodata; the kLedTestOnPower call site passes a string-literal
// const char*). What remained actionable was hardening the contract:
// every key must round-trip, the empty / unknown / repeated-lookup
// edge cases must behave, and the schema must be free of duplicate
// or empty keys. That is what this file checks.

#include <set>
#include <string>
#include <string_view>

#include "doctest.h"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

using btclock::settings::DefaultBoolFor;
using btclock::settings::DefaultIntFor;
using btclock::settings::DefaultStringFor;
using btclock::settings::FieldKind;
using btclock::settings::FindField;
using btclock::settings::kFields;

namespace prefs = btclock::prefs;

TEST_CASE("FindField: empty key returns nullptr (explicit short-circuit)") {
  CHECK(FindField("") == nullptr);
  CHECK(FindField(std::string_view{}) == nullptr);
}

TEST_CASE("FindField: unknown key returns nullptr") {
  CHECK(FindField("doesNotExist") == nullptr);
  CHECK(FindField("blockHeight_") == nullptr);  // near-miss with kBlockHeight
  CHECK(FindField(" ledTestOnPower") == nullptr);  // leading space
}

TEST_CASE("FindField: representative known keys resolve to a FieldSpec") {
  // One key per FieldKind so a future kind addition that breaks one
  // dispatch path shows up here rather than at runtime on device.
  const auto* led_test = FindField(prefs::kLedTestOnPower);
  REQUIRE(led_test != nullptr);
  CHECK(led_test->kind == FieldKind::kBool);

  const auto* fl_max = FindField(prefs::kFlMaxBrightness);
  REQUIRE(fl_max != nullptr);
  CHECK(fl_max->kind == FieldKind::kUint);

  const auto* act_ccy = FindField(prefs::kActCurrencies);
  REQUIRE(act_ccy != nullptr);
  CHECK(act_ccy->kind == FieldKind::kString);
}

TEST_CASE("FindField: repeated lookups are idempotent and stable") {
  // Defends against any future caching/sticky-state change to the
  // lookup that could leak prior results into a subsequent call.
  const auto* a = FindField(prefs::kLedTestOnPower);
  const auto* b = FindField(prefs::kLedTestOnPower);
  CHECK(a != nullptr);
  CHECK(a == b);
}

TEST_CASE("DefaultBoolFor: kBool fields return their default_bool") {
  // kLedTestOnPower default is `false` in the current schema (v3
  // shipped true; v4 inverted to false per the comment in schema.hpp
  // and pref_keys.hpp). The exact bool doesn't matter for this test —
  // what matters is that DefaultBoolFor returns whatever kFields says.
  const auto* f = FindField(prefs::kLedTestOnPower);
  REQUIRE(f != nullptr);
  REQUIRE(f->kind == FieldKind::kBool);
  CHECK(DefaultBoolFor(prefs::kLedTestOnPower) == f->default_bool);
}

TEST_CASE("DefaultBoolFor on non-bool / unknown returns false") {
  CHECK(DefaultBoolFor(prefs::kActCurrencies) == false);  // kString
  CHECK(DefaultBoolFor("doesNotExist") == false);
  CHECK(DefaultBoolFor("") == false);
}

TEST_CASE("DefaultIntFor: kUint/kInt/kUChar fields return their default_int") {
  const auto* f = FindField(prefs::kFlMaxBrightness);
  REQUIRE(f != nullptr);
  REQUIRE(f->kind == FieldKind::kUint);
  CHECK(DefaultIntFor(prefs::kFlMaxBrightness) == f->default_int);
}

TEST_CASE("DefaultIntFor on kBool / kString / unknown returns 0") {
  CHECK(DefaultIntFor(prefs::kLedTestOnPower) == 0);  // kBool
  CHECK(DefaultIntFor(prefs::kActCurrencies) == 0);   // kString
  CHECK(DefaultIntFor("doesNotExist") == 0);
  CHECK(DefaultIntFor("") == 0);
}

TEST_CASE("DefaultStringFor: kString fields return their default_str") {
  const auto* f = FindField(prefs::kActCurrencies);
  REQUIRE(f != nullptr);
  REQUIRE(f->kind == FieldKind::kString);
  CHECK(DefaultStringFor(prefs::kActCurrencies) == f->default_str);
}

TEST_CASE("DefaultStringFor on non-string / unknown returns empty view") {
  CHECK(DefaultStringFor(prefs::kLedTestOnPower).empty());  // kBool
  CHECK(DefaultStringFor("doesNotExist").empty());
  CHECK(DefaultStringFor("").empty());
}

TEST_CASE("kFields catalog: no empty keys") {
  for (const auto& f : kFields) {
    INFO("field has empty key");
    CHECK_FALSE(f.key.empty());
  }
}

TEST_CASE("kFields catalog: no duplicate keys") {
  // A duplicated key would let the FindField lookup return the first
  // entry and silently shadow the rest. Schema editors edit
  // alphabetically by key in this file, so duplicates are easy to
  // introduce by accident.
  std::set<std::string_view> seen;
  for (const auto& f : kFields) {
    INFO("duplicate key in kFields: ", std::string(f.key));
    CHECK(seen.insert(f.key).second);
  }
}

TEST_CASE("kFields catalog: every key round-trips through FindField") {
  // Belt-and-braces — if FindField gets refactored to use a hash table
  // or a switch, the catalog must keep finding itself.
  for (const auto& f : kFields) {
    const auto* hit = FindField(f.key);
    INFO("key not findable: ", std::string(f.key));
    REQUIRE(hit != nullptr);
    CHECK(hit == &f);
  }
}

TEST_CASE(
    "kFields catalog: each key fits NVS_KEY_NAME_MAX_SIZE (15 char limit)") {
  // ESP-IDF NVS caps key names at 15 bytes. The pref_keys.hpp
  // static_assert block covers the canonical constants, but the
  // schema's std::string_view entries are constructed from those same
  // constants, so this is mostly belt-and-braces. Still: a future
  // schema edit could in principle introduce a literal string that
  // doesn't go through pref_keys.hpp.
  for (const auto& f : kFields) {
    INFO("key too long for NVS: ", std::string(f.key));
    CHECK(f.key.size() <= 15);
  }
}
