// Pure-logic nostr-settings readers. The Nostr data-source builder and
// the zap listener both pull their relay URL + pubkeys out of NVS at
// boot (and again when on_nostr_changed fires). The schema declares
// these keys in the canonical "settings" namespace where /api/settings
// PATCH writes them; before this helper existed the consumers opened
// a separate "nostr" namespace with shorthand keys, so a WebUI PATCH
// silently no-op'd. Centralising the read shape here keeps both call
// sites aligned with the schema and makes the namespace round-trip
// host-testable through FakePrefs.
//
// Defaults track schema.hpp::kFields so a fresh install matches what
// /api/settings GET reports.
//
// See bd btclock_v4-aw5 (data source) + btclock_v4-q1l (zap listener).
//
// Live-PATCH note: kNostrRelay and kNostrPubKey are flagged boot_only
// in the schema, so a change requires a reboot to take effect (the
// PATCH response carries rebootRequired=true). kNostrZapPubkey and
// kNostrZapNotify are runtime-editable; the on_nostr_changed hook in
// control_server fires for those keys so init_zap_listener can rebuild
// the listener without a reboot.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "settings/api.hpp"

namespace btclock {
namespace settings {

// Cap on the number of relays a single install can connect to. Each
// extra relay opens its own WSS (one RelayClient + SubscriptionManager
// shared between the data source and the zap listener via NIP-01 multi-
// sub). Measured cost on Rev B: ~13 KB internal SRAM + ~24 KB PSRAM per
// extra dedicated WSS, halved when shared. Four caps keeps Rev A (4 MB
// flash / 2 MB PSRAM) inside the largest-free-block fragmentation
// ceiling — adding a fifth approaches the 32 KB cliff that pinned the
// EPD render path on long-running devices.
constexpr std::size_t kMaxNostrRelays = 4;

// Snapshot of the nostr-relevant settings the data-source consumes. A
// missing or zero-length list disables the source — the caller is
// expected to skip construction in that case rather than feeding the
// empty values into the relay client.
struct NostrSourceConfig {
  // True when dataSource == 2 (the Nostr DataSourceType enum value in
  // defaults.hpp). The data-source builder gates Nostr construction
  // on this flag in addition to the URL/pubkey presence check.
  bool enabled = false;
  // Canonical multi-relay list. CSV in NVS (kNostrRelays); reader falls
  // back to the legacy singular kNostrRelay slot when the plural is
  // empty so existing installs keep working without a migration step.
  std::vector<std::string> relay_urls;
  std::string author_pubkey_hex;  // kNostrPubKey (lowercase hex, 64 chars)
};

// Cap on the number of zap-recipient pubkeys a single REQ filter can
// carry. Each entry adds 64 hex chars + quote/comma overhead (~67 B)
// to the JSON-serialised filter and one extra `#p` value the relay
// matches against; eight is generous for a personal device while
// keeping the filter under 600 B.
constexpr std::size_t kMaxZapPubkeys = 8;

// Snapshot of the nostr-relevant settings the zap listener consumes.
// The flash gates (LED + frontlight) live alongside so init_zap_listener
// can stash them in atomics in a single read pass.
struct ZapListenerConfig {
  // True when nostrZapNotify is set. The listener is wired iff this is
  // true AND the relay URL list + zap_pubkeys are valid.
  bool enabled = true;
  // Same canonical list NostrSourceConfig consumes — listener and data
  // source share the relay set so each WSS carries both NIP-78 and
  // kind-9735 subscriptions.
  std::vector<std::string> relay_urls;
  // Recipient pubkeys (lowercase hex, 64 chars each). Source of truth
  // is kNostrZapPubkeys (CSV in NVS); reader falls back to the legacy
  // singular kNostrZapPubkey when the plural slot is empty so existing
  // installs keep working without an explicit migration step.
  std::vector<std::string> zap_pubkeys;
  bool zap_screen_notify = true;         // kNostrZapNotify
  bool zap_screen_auto_restore = true;   // kScrnRestoreZap
  bool led_flash_on_zap = true;          // kLedFlashOnZap
  bool frontlight_flash_on_zap = false;  // kFlFlashOnZap
};

// Read NostrSourceConfig from the canonical "settings" namespace. Caller
// supplies a PrefsReader pointed at kSettingsNs.
NostrSourceConfig ReadNostrSourceConfig(const PrefsReader& prefs);

// Read ZapListenerConfig from the canonical "settings" namespace.
ZapListenerConfig ReadZapListenerConfig(const PrefsReader& prefs);

}  // namespace settings
}  // namespace btclock
