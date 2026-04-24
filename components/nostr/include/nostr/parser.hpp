// Minimal NIP-01 envelope parser.
//
// Pure-logic, no ESP-IDF / no allocator beyond std::string. The
// implementation is a tiny hand-rolled JSON walker — we don't pull in
// cJSON here because (a) it would add an include dep to every host
// test, and (b) the relay frames we care about are small, flat, and
// easy to walk with a handful of state machines.
//
// Scope: parses top-level arrays ["EVENT", sub-id, {...}], ["EOSE", ...],
// ["CLOSED", ...], ["NOTICE", ...], ["OK", ...] and the embedded event
// object's primitive fields (id, pubkey, created_at, kind, content, sig)
// plus the `tags` array-of-arrays. UTF-8 pass-through; escape sequences
// `\"`, `\\`, `\n`, `\r`, `\t`, `\/`, `\b`, `\f` are decoded. `\u` is
// passed through verbatim — relay frames targeting us (kind 30078 + kind
// 9735) don't use it in fields we read.

#pragma once

#include <string>

#include "data_core/snapshot.hpp"
#include "nostr/event.hpp"

namespace btclock {
namespace nostr {

// Decode a single relay → client frame. Returns true iff the envelope
// was recognised; `out` is populated on success. Returns false on
// malformed JSON or an envelope whose first element isn't a known
// string literal — the caller should log+drop.
bool ParseEnvelope(const std::string& frame, Envelope& out);

// Decode a bare event object (the third element of an EVENT frame).
// Exposed so tests can feed a synthetic object directly without the
// envelope wrapper.
bool ParseEventObject(const std::string& json, Event& out);

// NIP-57 helper: extract the zap amount in millisats from a kind 9735
// event. The amount is the integer value of the `amount` tag (string,
// per NIP-57). Returns true and sets `msat` iff present and parseable.
bool ExtractZapAmountMsat(const Event& ev, uint64_t& msat);

// Extract the bolt11 invoice string from a zap receipt. Returns true
// iff a `bolt11` tag is present with a non-empty value.
bool ExtractZapBolt11(const Event& ev, std::string& bolt11);

// NIP-57 gate: should this zap receipt surface as a user-visible event
// (LED flash, screen overlay, LatestZap snapshot update)?
//
// Returns true iff `ev` is a kind-9735 event with a parseable `amount`
// tag AND the decoded amount resolves to at least 1 sat (1000 msat).
// Zero-sat receipts — common when a relay forwards a malformed or
// placeholder zap — should be ignored at the source so none of the
// three downstream effects fire. Gate is kept here (alongside the
// extractor) so host tests can pin the boundary without booting the
// full zap listener machinery.
bool ShouldSurfaceZap(const Event& ev);

// Decode a single NIP-78 (kind 30078) event content + `d` tag into a
// partial DataSnapshot per ws-nostr-publish/docs/NOSTR.md:
//
//   d=blockheight  → snapshot.block_height      = uint32(content)
//   d=medianFee    → snapshot.block_fee         = int32(round(content))
//                    snapshot.block_fee_precise = double(content)
//   d=price:<CCY>  → snapshot.prices[<CCY>]     = content (verbatim string)
//
// Pure logic, no ESP-IDF / no logging — caller handles both. Returns
// true iff `d_tag` is a recognised slot AND the content parses for
// that slot. Unknown `d_tag` → false with snapshot unchanged; caller
// decides whether to log+drop. Malformed content (e.g. non-numeric
// height) → false, snapshot is left in whatever partial state it was
// in (caller should not use it on false return).
bool ParseNip78Content(const std::string& d_tag,
                       const std::string& content,
                       DataSnapshot& out);

}  // namespace nostr
}  // namespace btclock
