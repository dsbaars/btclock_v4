// Pure-logic NWC settings reader. Mirrors the NostrSourceConfig /
// ZapListenerConfig shape so init_nwc.cpp can pull every NWC-relevant
// NVS slot in one pass without dragging an `nwc` namespace open from
// a second site. Lives next to nostr_config.hpp on purpose — both
// readers consult the canonical "settings" namespace where the
// /api/settings PATCH writes them.
//
// `uri` is the raw `nostr+walletconnect://…` string; `parsed_ok`
// indicates whether ParsePairingUri accepted it. Callers should
// consult both `enabled` and `parsed_ok` before constructing the
// NwcClient — a malformed URI would otherwise wedge the state
// machine in kFatal at first publish.

#pragma once

#include <cstdint>
#include <string>

#include "nwc/uri.hpp"
#include "settings/api.hpp"

namespace btclock {
namespace settings {

struct NwcConfig {
  // Master toggle. `enabled=false` short-circuits init_nwc (no WSS
  // opened, no RelayClient constructed, no refresh tick scheduled);
  // toggling on requires reboot per the schema's boot_only flag.
  bool enabled = false;
  // Raw URI. Empty when the user hasn't configured one — treat as
  // disabled even if `enabled=true` (the WebUI may briefly hit that
  // race during a settings save).
  std::string uri;
  // ParsePairingUri output. Populated only when `uri` was non-empty
  // and well-formed. The boot path uses these fields directly so the
  // URI is parsed once at boot rather than on every publish.
  nwc::PairingUri parsed{};
  // True iff ParsePairingUri returned kOk on `uri`. Distinct from
  // `enabled` so the boot path can log a diagnostic when the user
  // checked the toggle but the URI didn't parse.
  bool parsed_ok = false;
  // Periodic get_balance poll cadence in seconds. Bounded 15..3600 by
  // the schema. The boot path uses this to seed the refresh timer.
  uint32_t refresh_secs = 60;
  // LED + frontlight flash on every notification. Read at dispatch
  // time so a runtime PATCH lands without rebooting.
  bool flash_on_payment = true;
};

// Read every NWC-relevant slot from the canonical "settings" namespace.
// `parsed_ok` reflects ParsePairingUri's verdict; callers that need
// the structured error code should re-run the parser themselves.
NwcConfig ReadNwcConfig(const PrefsReader& prefs);

}  // namespace settings
}  // namespace btclock
