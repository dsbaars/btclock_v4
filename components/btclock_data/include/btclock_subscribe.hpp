// Pure-logic helpers for building btclock WS v2 subscribe frames.
//
// The production data path encodes one MessagePack `subscribe` frame
// per topic (blockheight, blockfee/blockfee2, price/<ccy>) and ships
// them to the relay on connect. These helpers cover the topic-naming
// rule alone — they do not depend on ArduinoJson or ESP-IDF, so the
// gating contract ("blockfee2 iff blockFeeDec else blockfee") can be
// pinned host-side.
//
// Why split off the topic name and not the encoder: the integer vs.
// 2-decimal stream is a relay-side schema choice, not a transport
// detail. Whatever the wire format ends up being (msgpack today, JSON
// tomorrow), we still want exactly one of the two topics in flight.

#pragma once

#include <string>
#include <vector>

namespace btclock {
namespace subscribe {

// Returns the ordered list of topic strings the v2 WS client must
// subscribe to for the given configuration. The order matches the
// on-device send order (blockheight first, then exactly one fee
// stream, then one entry per currency code) so the test fixture and
// the production helper stay in lock-step.
//
// `block_fee_dec=true`  emits "blockfee2" (2-decimal precise stream,
//                       fires on every fee tick).
// `block_fee_dec=false` emits "blockfee"  (integer rounded stream,
//                       fires only on rounded-value change).
//
// Both topics still exist on the relay — the bug we fix here was
// subscribing to *both* and double-dispatching. Callers must dispatch
// only the topic they subscribed to (see DispatchFee gating below).
inline std::vector<std::string> BuildSubscribeTopics(
    const std::vector<std::string>& currencies, bool block_fee_dec) {
  std::vector<std::string> out;
  out.reserve(2 + currencies.size());
  out.emplace_back("blockheight");
  out.emplace_back(block_fee_dec ? "blockfee2" : "blockfee");
  for (const auto& ccy : currencies) {
    if (!ccy.empty()) out.emplace_back("price:" + ccy);
  }
  return out;
}

// Human-readable subscribe summary line — mirrors the ESP_LOGI in
// SendSubscriptions. Pinned in host tests so a future log refactor
// can't quietly drop / dupe the fee-stream name in serial output.
//
// Format matches the existing production line:
//   "subscribe: blockheight + <fee> + price/[USD,EUR]"
// where <fee> is either "blockfee" or "blockfee2".
inline std::string BuildSubscribeLogLine(
    const std::vector<std::string>& currencies, bool block_fee_dec) {
  std::string ccy_list;
  for (size_t i = 0; i < currencies.size(); ++i) {
    if (i > 0) ccy_list += ",";
    ccy_list += currencies[i];
  }
  std::string out = "subscribe: blockheight + ";
  out += (block_fee_dec ? "blockfee2" : "blockfee");
  out += " + price/[";
  out += ccy_list;
  out += "]";
  return out;
}

// Dispatch gate: true iff the inbound topic name matches the topic
// the v2 client actually subscribed to for the current
// `block_fee_dec` setting. Frames that arrive for the *other* fee
// stream (e.g. relay-side leftover from a stale subscription) must
// be dropped so the snapshot fee field reflects exactly one wire
// source. Topics other than the two fee streams are passed through
// unconditionally.
inline bool ShouldDispatchTopic(const std::string& topic, bool block_fee_dec) {
  if (topic == "blockfee") return !block_fee_dec;
  if (topic == "blockfee2") return block_fee_dec;
  return true;
}

}  // namespace subscribe
}  // namespace btclock
