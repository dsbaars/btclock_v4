// Host tests for the /api/nwc/debug JSON builder.
//
// Pins the on-wire shape so a curl loop on the device can rely on
// stable field names + counter keys. The actual counters are filled
// on-device from atomic loads; here we craft an NwcDebugInfo by hand
// and feed it through BuildNwcDebugJson + cJSON to assert each field
// shows up in the right place. Diagnose-side equivalent to the test
// already pinning the boot-time wiring (test_nwc_client.cpp).

#include <cstdint>
#include <string>

#include "cJSON.h"
#include "doctest.h"
#include "nwc/client.hpp"
#include "nwc/debug.hpp"

using namespace btclock;

namespace {

nwc::NwcDebugInfo MakeFixture() {
  nwc::NwcDebugInfo info;
  info.enabled = true;
  info.client.state = nwc::State::kReady;
  info.client.encryption = nostr::EncryptionVariant::kNip44V2;
  info.client.balance_msat_cache = 11'000'000;  // 11_000 sats
  info.client.sub_id_info = "nwci-abcdef01";
  info.client.sub_id_rpc = "nwcr-abcdef01";

  info.client.events_total = 12;
  info.client.events_info = 1;
  info.client.events_response = 3;
  info.client.events_notif_modern = 4;
  info.client.events_notif_legacy = 4;
  info.client.events_other = 0;
  info.client.last_kind = 23197;
  info.client.last_event_ms = 1'778'600'420'000;
  info.client.last_response_ms = 1'778'600'350'000;

  info.client.decrypt_attempts = 8;
  info.client.decrypt_ok = 4;
  info.client.decrypt_fail_nip04 = 4;

  info.client.decode_notif_ok = 4;
  info.client.decode_resp_ok = 3;

  info.client.cb_on_balance_dispatched = 3;
  info.client.cb_on_payment_dispatched = 0;

  info.client.last_pay_direction = 1;  // kIncoming
  info.client.last_pay_amount_sats = 555;
  info.client.last_pay_received_ms = 1'778'600'420'000;

  info.wss_url = "wss://relay.getalby.com";
  info.wss_connected = true;
  info.reconnect_count = 0;
  info.last_connect_ms = 1'778'600'000'000;
  info.last_disconnect_ms = 0;

  info.reissue_count = 1;
  return info;
}

}  // namespace

TEST_CASE("BuildNwcDebugJson: top-level fields and shape") {
  const auto info = MakeFixture();
  const std::string body = nwc::BuildNwcDebugJson(info);
  REQUIRE_FALSE(body.empty());
  cJSON* root = cJSON_Parse(body.c_str());
  REQUIRE(root != nullptr);
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled")));
  const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
  REQUIRE(cJSON_IsString(state));
  CHECK(std::string(state->valuestring) == "kReady");
  const cJSON* enc = cJSON_GetObjectItemCaseSensitive(root, "encryption");
  REQUIRE(cJSON_IsString(enc));
  CHECK(std::string(enc->valuestring) == "nip44_v2");
  cJSON_Delete(root);
}

TEST_CASE("BuildNwcDebugJson: wss + subs block") {
  const auto info = MakeFixture();
  const std::string body = nwc::BuildNwcDebugJson(info);
  cJSON* root = cJSON_Parse(body.c_str());
  REQUIRE(root != nullptr);

  const cJSON* wss = cJSON_GetObjectItemCaseSensitive(root, "wss");
  REQUIRE(cJSON_IsObject(wss));
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wss, "connected")));
  const cJSON* url = cJSON_GetObjectItemCaseSensitive(wss, "url");
  REQUIRE(cJSON_IsString(url));
  CHECK(std::string(url->valuestring) == "wss://relay.getalby.com");
  CHECK(cJSON_GetObjectItemCaseSensitive(wss, "reconnect_count")->valueint == 0);
  CHECK(static_cast<int64_t>(
            cJSON_GetObjectItemCaseSensitive(wss, "last_connect_ms")
                ->valuedouble) == 1'778'600'000'000LL);
  CHECK(cJSON_GetObjectItemCaseSensitive(wss, "last_disconnect_ms")
            ->valueint == 0);

  const cJSON* subs = cJSON_GetObjectItemCaseSensitive(root, "subs");
  REQUIRE(cJSON_IsObject(subs));
  const cJSON* info_id = cJSON_GetObjectItemCaseSensitive(subs, "info_sub_id");
  REQUIRE(cJSON_IsString(info_id));
  CHECK(std::string(info_id->valuestring) == "nwci-abcdef01");
  const cJSON* rpc_id = cJSON_GetObjectItemCaseSensitive(subs, "rpc_sub_id");
  REQUIRE(cJSON_IsString(rpc_id));
  CHECK(std::string(rpc_id->valuestring) == "nwcr-abcdef01");
  CHECK(cJSON_GetObjectItemCaseSensitive(subs, "reissue_count")->valueint == 1);
  cJSON_Delete(root);
}

TEST_CASE("BuildNwcDebugJson: events by_kind keys are stringified ints") {
  // The WebUI / curl loop want to grep on by_kind["23197"] — pin the
  // key encoding so an accidental switch to `kind_23197` breaks here
  // before it breaks the operator's eyes.
  const auto info = MakeFixture();
  const std::string body = nwc::BuildNwcDebugJson(info);
  cJSON* root = cJSON_Parse(body.c_str());
  REQUIRE(root != nullptr);
  const cJSON* events = cJSON_GetObjectItemCaseSensitive(root, "events");
  REQUIRE(cJSON_IsObject(events));
  CHECK(cJSON_GetObjectItemCaseSensitive(events, "received_total")->valueint ==
        12);
  const cJSON* by_kind = cJSON_GetObjectItemCaseSensitive(events, "by_kind");
  REQUIRE(cJSON_IsObject(by_kind));
  CHECK(cJSON_GetObjectItemCaseSensitive(by_kind, "13194")->valueint == 1);
  CHECK(cJSON_GetObjectItemCaseSensitive(by_kind, "23195")->valueint == 3);
  CHECK(cJSON_GetObjectItemCaseSensitive(by_kind, "23196")->valueint == 4);
  CHECK(cJSON_GetObjectItemCaseSensitive(by_kind, "23197")->valueint == 4);
  CHECK(cJSON_GetObjectItemCaseSensitive(by_kind, "other")->valueint == 0);
  CHECK(cJSON_GetObjectItemCaseSensitive(events, "last_kind")->valueint ==
        23197);
  cJSON_Delete(root);
}

TEST_CASE("BuildNwcDebugJson: decrypt/decode/callbacks/balance/last_payment") {
  const auto info = MakeFixture();
  const std::string body = nwc::BuildNwcDebugJson(info);
  cJSON* root = cJSON_Parse(body.c_str());
  REQUIRE(root != nullptr);

  const cJSON* decrypt = cJSON_GetObjectItemCaseSensitive(root, "decrypt");
  REQUIRE(cJSON_IsObject(decrypt));
  CHECK(cJSON_GetObjectItemCaseSensitive(decrypt, "attempts")->valueint == 8);
  CHECK(cJSON_GetObjectItemCaseSensitive(decrypt, "ok")->valueint == 4);
  CHECK(cJSON_GetObjectItemCaseSensitive(decrypt, "fail_nip44")->valueint == 0);
  CHECK(cJSON_GetObjectItemCaseSensitive(decrypt, "fail_nip04")->valueint == 4);

  const cJSON* decode = cJSON_GetObjectItemCaseSensitive(root, "decode");
  REQUIRE(cJSON_IsObject(decode));
  CHECK(cJSON_GetObjectItemCaseSensitive(decode, "notif_ok")->valueint == 4);
  CHECK(cJSON_GetObjectItemCaseSensitive(decode, "notif_fail")->valueint == 0);
  CHECK(cJSON_GetObjectItemCaseSensitive(decode, "resp_ok")->valueint == 3);
  CHECK(cJSON_GetObjectItemCaseSensitive(decode, "resp_fail")->valueint == 0);

  const cJSON* cb = cJSON_GetObjectItemCaseSensitive(root, "callbacks");
  REQUIRE(cJSON_IsObject(cb));
  CHECK(cJSON_GetObjectItemCaseSensitive(cb, "on_payment_dispatched")
            ->valueint == 0);
  CHECK(cJSON_GetObjectItemCaseSensitive(cb, "on_balance_dispatched")
            ->valueint == 3);

  const cJSON* bal = cJSON_GetObjectItemCaseSensitive(root, "balance");
  REQUIRE(cJSON_IsObject(bal));
  CHECK(static_cast<int64_t>(
            cJSON_GetObjectItemCaseSensitive(bal, "msat_cache")
                ->valuedouble) == 11'000'000LL);

  const cJSON* lp = cJSON_GetObjectItemCaseSensitive(root, "last_payment");
  REQUIRE(cJSON_IsObject(lp));
  CHECK(cJSON_GetObjectItemCaseSensitive(lp, "direction")->valueint == 1);
  CHECK(cJSON_GetObjectItemCaseSensitive(lp, "amount_sats")->valueint == 555);
  CHECK(static_cast<int64_t>(
            cJSON_GetObjectItemCaseSensitive(lp, "received_ms")->valuedouble) ==
        1'778'600'420'000LL);
  cJSON_Delete(root);
}

TEST_CASE("BuildNwcDebugJson: zero-initialised snapshot still emits stable shape") {
  // The endpoint may be hit before InitNwc completes (operator runs
  // /api/nwc/debug seconds after boot, possibly even in AP-mode if a
  // future path enables it). Defaulted DebugSnapshot must still
  // produce a parseable, well-shaped JSON with state=kIdle and
  // every counter at zero — that's the "nwc never wired" baseline
  // the curl loop expects.
  nwc::NwcDebugInfo info;
  const std::string body = nwc::BuildNwcDebugJson(info);
  cJSON* root = cJSON_Parse(body.c_str());
  REQUIRE(root != nullptr);
  CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "enabled")));
  CHECK(std::string(cJSON_GetObjectItemCaseSensitive(root, "state")
                        ->valuestring) == "kIdle");
  CHECK(cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(root, "events"), "received_total")
            ->valueint == 0);
  cJSON_Delete(root);
}
