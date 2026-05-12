// /api/nwc/debug JSON builder.

#include "nwc/debug.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

#include "cJSON.h"

namespace btclock {
namespace nwc {
namespace {

const char* StateName(State s) {
  switch (s) {
    case State::kIdle:
      return "kIdle";
    case State::kBootstrapping:
      return "kBootstrapping";
    case State::kReady:
      return "kReady";
    case State::kFatal:
      return "kFatal";
  }
  return "kUnknown";
}

const char* EncryptionName(nostr::EncryptionVariant v) {
  return v == nostr::EncryptionVariant::kNip44V2 ? "nip44_v2" : "nip04";
}

}  // namespace

std::string BuildNwcDebugJson(const NwcDebugInfo& info) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddBoolToObject(root, "enabled", info.enabled);
  cJSON_AddStringToObject(root, "state", StateName(info.client.state));
  cJSON_AddStringToObject(root, "encryption",
                          EncryptionName(info.client.encryption));

  cJSON* wss = cJSON_AddObjectToObject(root, "wss");
  if (wss) {
    cJSON_AddBoolToObject(wss, "connected", info.wss_connected);
    cJSON_AddStringToObject(wss, "url", info.wss_url.c_str());
    cJSON_AddNumberToObject(wss, "reconnect_count",
                            static_cast<double>(info.reconnect_count));
    cJSON_AddNumberToObject(wss, "last_connect_ms",
                            static_cast<double>(info.last_connect_ms));
    cJSON_AddNumberToObject(wss, "last_disconnect_ms",
                            static_cast<double>(info.last_disconnect_ms));
    cJSON_AddNumberToObject(wss, "frames_chunk",
                            static_cast<double>(info.frames_chunk));
    cJSON_AddNumberToObject(wss, "frames_complete",
                            static_cast<double>(info.frames_complete));
    cJSON_AddNumberToObject(wss, "last_frame_bytes",
                            static_cast<double>(info.last_frame_bytes));
    cJSON_AddNumberToObject(wss, "last_evt_op_code",
                            static_cast<double>(info.last_evt_op_code));
    cJSON_AddNumberToObject(wss, "last_evt_fin",
                            static_cast<double>(info.last_evt_fin));
    cJSON_AddNumberToObject(wss, "last_evt_payload_offset",
                            static_cast<double>(info.last_evt_payload_offset));
    cJSON_AddNumberToObject(wss, "last_evt_payload_len",
                            static_cast<double>(info.last_evt_payload_len));
    cJSON_AddNumberToObject(wss, "last_evt_data_len",
                            static_cast<double>(info.last_evt_data_len));
    cJSON_AddStringToObject(wss, "last_emitted_head",
                            info.last_emitted_head.c_str());
    cJSON* hist = cJSON_AddArrayToObject(wss, "evt_history");
    if (hist) {
      for (const auto& r : info.wss_evt_history) {
        cJSON* item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddNumberToObject(item, "seq", static_cast<double>(r.seq));
        cJSON_AddNumberToObject(item, "op", static_cast<double>(r.op_code));
        cJSON_AddNumberToObject(item, "fin", static_cast<double>(r.fin));
        cJSON_AddNumberToObject(item, "emit", static_cast<double>(r.emit));
        cJSON_AddNumberToObject(item, "off",
                                static_cast<double>(r.payload_offset));
        cJSON_AddNumberToObject(item, "plen",
                                static_cast<double>(r.payload_len));
        cJSON_AddNumberToObject(item, "dlen", static_cast<double>(r.data_len));
        cJSON_AddItemToArray(hist, item);
      }
    }
  }

  cJSON* subs = cJSON_AddObjectToObject(root, "subs");
  if (subs) {
    cJSON_AddStringToObject(subs, "info_sub_id",
                            info.client.sub_id_info.c_str());
    cJSON_AddStringToObject(subs, "rpc_sub_id", info.client.sub_id_rpc.c_str());
    cJSON_AddNumberToObject(subs, "reissue_count",
                            static_cast<double>(info.reissue_count));
    cJSON_AddNumberToObject(subs, "parse_fail",
                            static_cast<double>(info.parse_fail_count));
    cJSON_AddNumberToObject(subs, "event_dispatched",
                            static_cast<double>(info.event_dispatch_count));
    cJSON_AddStringToObject(subs, "last_event_sub_id",
                            info.last_event_sub_id.c_str());
    cJSON_AddStringToObject(subs, "last_parse_fail_head",
                            info.last_parse_fail_head.c_str());
  }

  cJSON* events = cJSON_AddObjectToObject(root, "events");
  if (events) {
    cJSON_AddNumberToObject(events, "received_total",
                            static_cast<double>(info.client.events_total));
    cJSON* by_kind = cJSON_AddObjectToObject(events, "by_kind");
    if (by_kind) {
      cJSON_AddNumberToObject(by_kind, "13194",
                              static_cast<double>(info.client.events_info));
      cJSON_AddNumberToObject(by_kind, "23195",
                              static_cast<double>(info.client.events_response));
      cJSON_AddNumberToObject(
          by_kind, "23196",
          static_cast<double>(info.client.events_notif_legacy));
      cJSON_AddNumberToObject(
          by_kind, "23197",
          static_cast<double>(info.client.events_notif_modern));
      cJSON_AddNumberToObject(by_kind, "other",
                              static_cast<double>(info.client.events_other));
    }
    cJSON_AddNumberToObject(events, "last_kind",
                            static_cast<double>(info.client.last_kind));
    cJSON_AddNumberToObject(events, "last_ms",
                            static_cast<double>(info.client.last_event_ms));
  }

  cJSON* decrypt = cJSON_AddObjectToObject(root, "decrypt");
  if (decrypt) {
    cJSON_AddNumberToObject(decrypt, "attempts",
                            static_cast<double>(info.client.decrypt_attempts));
    cJSON_AddNumberToObject(decrypt, "ok",
                            static_cast<double>(info.client.decrypt_ok));
    cJSON_AddNumberToObject(
        decrypt, "fail_nip44",
        static_cast<double>(info.client.decrypt_fail_nip44));
    cJSON_AddNumberToObject(
        decrypt, "fail_nip04",
        static_cast<double>(info.client.decrypt_fail_nip04));
  }

  cJSON* decode = cJSON_AddObjectToObject(root, "decode");
  if (decode) {
    cJSON_AddNumberToObject(decode, "notif_ok",
                            static_cast<double>(info.client.decode_notif_ok));
    cJSON_AddNumberToObject(decode, "notif_fail",
                            static_cast<double>(info.client.decode_notif_fail));
    cJSON_AddNumberToObject(decode, "resp_ok",
                            static_cast<double>(info.client.decode_resp_ok));
    cJSON_AddNumberToObject(decode, "resp_fail",
                            static_cast<double>(info.client.decode_resp_fail));
  }

  cJSON* callbacks = cJSON_AddObjectToObject(root, "callbacks");
  if (callbacks) {
    cJSON_AddNumberToObject(
        callbacks, "on_payment_dispatched",
        static_cast<double>(info.client.cb_on_payment_dispatched));
    cJSON_AddNumberToObject(
        callbacks, "on_balance_dispatched",
        static_cast<double>(info.client.cb_on_balance_dispatched));
  }

  cJSON* notif_queue = cJSON_AddObjectToObject(root, "notif_queue");
  if (notif_queue) {
    cJSON_AddNumberToObject(notif_queue, "capacity",
                            static_cast<double>(info.notif_queue_capacity));
    cJSON_AddNumberToObject(notif_queue, "size",
                            static_cast<double>(info.notif_queue_size));
    cJSON_AddNumberToObject(notif_queue, "enqueued",
                            static_cast<double>(info.client.notif_enqueued));
    cJSON_AddNumberToObject(notif_queue, "dropped",
                            static_cast<double>(info.client.notif_dropped));
    cJSON_AddNumberToObject(notif_queue, "dispatched",
                            static_cast<double>(info.client.notif_dispatched));
    cJSON_AddNumberToObject(notif_queue, "queue_pushed",
                            static_cast<double>(info.notif_queue_pushed));
    cJSON_AddNumberToObject(notif_queue, "queue_popped",
                            static_cast<double>(info.notif_queue_popped));
    cJSON_AddNumberToObject(notif_queue, "queue_dropped",
                            static_cast<double>(info.notif_queue_dropped));
  }

  cJSON* balance = cJSON_AddObjectToObject(root, "balance");
  if (balance) {
    cJSON_AddNumberToObject(
        balance, "msat_cache",
        static_cast<double>(info.client.balance_msat_cache));
    cJSON_AddNumberToObject(balance, "last_response_ms",
                            static_cast<double>(info.client.last_response_ms));
  }

  cJSON* last_pay = cJSON_AddObjectToObject(root, "last_payment");
  if (last_pay) {
    cJSON_AddNumberToObject(
        last_pay, "direction",
        static_cast<double>(info.client.last_pay_direction));
    cJSON_AddNumberToObject(
        last_pay, "amount_sats",
        static_cast<double>(info.client.last_pay_amount_sats));
    cJSON_AddNumberToObject(
        last_pay, "received_ms",
        static_cast<double>(info.client.last_pay_received_ms));
  }

  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) return {};
  std::string out(txt);
  cJSON_free(txt);
  return out;
}

}  // namespace nwc
}  // namespace btclock
