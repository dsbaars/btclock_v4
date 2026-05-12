# NIP-47 Nostr Wallet Connect — sizing for BTClock Rev A

> Research artifact, 2026-05-12. Read-only sizing study; no firmware changes
> were made. Numbers are estimates; the implementation issues
> (`btclock_v4-nwc-*`) carry follow-up tasks to measure the deltas
> empirically as each piece lands.

## 1. TL;DR

OTA partition for Rev A is `0x1B0000` (1,769,472 B). Current
`build-rev-a/btclock_v4.bin` is **1,729,808 B**, so headroom is
**39,664 B (38.7 KiB)** — matches the brief exactly.

| Scenario (target = #3) | Flash Δ (mid ± 95% CI) | Steady RAM | Per-req peak | Fits 38.7 KiB? |
|---|---|---|---|---|
| 1. NIP-04 only, minimal, read-only | **~20 KiB** (16–24) | +5–8 KiB (shared sock) | +6–8 KiB | Yes, ~18 KiB margin |
| 2. NIP-44 v2 only, minimal, read-only | **~24 KiB** (20–28) | +5–8 KiB | +6–8 KiB | Yes, ~14 KiB margin |
| 3. **Both NIP-44 v2 + NIP-04, balance + notifications (TARGET)** | **~26 KiB** (23–31) | +5–10 KiB | +6–8 KiB | **Yes, ~8–13 KiB margin** |
| 4. Both + full (pay/make/list_transactions, dedicated socket) | **~35 KiB** (31–41) | +40–55 KiB (dedicated TLS) | +8–12 KiB | **No / borderline** — bust risk |

**Verdict:** Scenarios 1, 2, 3 all fit Rev A's OTA headroom today. The target
(scenario 3) ships everywhere with margin. Scenario 4 should be gated behind
`CONFIG_BTCLOCK_NWC_FULL=y` (Rev B / V8 only).

**Scope clarifications (v1 in scope):**

- `get_balance` (primary refresh tick) and `get_info` (handshake / capability
  discovery).
- **NIP-47 notifications kind 23196** — `payment_received` and `payment_sent`
  events pushed by the wallet, decrypted via the same NIP-44 v2 / NIP-04
  dispatcher, surfaced as a transient screen + SSE event for the WebUI.
- Single-wallet (single NWC URI) only.

**Out of scope v1:** outgoing payments (`pay_invoice` / `make_invoice` / etc.),
`list_transactions` history pagination, multi-wallet routing, LNURL-pay beyond
what NWC `pay_invoice` would give us.

The dominant new cost in every scenario is **secp256k1 schnorr sign-side** +
its precomputed_ecmult_gen table (~8–12 KiB). NIP-44 v2's primitives are cheap
(~4 KiB combined). NIP-04 itself adds essentially nothing on top of NIP-44 v2
because the crypto primitives it needs are already in the link.

---

## 2. Crypto stack for NIP-44 v2 + NIP-04 — concrete kB cost

Probed against `build-rev-a/sdkconfig`, `build-rev-a/config/sdkconfig.h`,
`build-rev-a/btclock_v4.elf` (nm), and `components/secp256k1/CMakeLists.txt`.

| Primitive | NIP-44 v2 | NIP-04 | sdkconfig today | Δ on enable |
|---|---|---|---|---|
| `MBEDTLS_AES_C` + `MBEDTLS_CIPHER_MODE_CBC` | — | Yes (AES-256-CBC) | **On** (`CONFIG_MBEDTLS_AES_C=y`, `CONFIG_MBEDTLS_CIPHER_MODE_CBC=y`) — `mbedtls_aes_setkey_enc/dec`, `mbedtls_aes_crypt_cbc` linked | **0 KiB** |
| `MBEDTLS_BASE64_C` | Yes | Yes | **On** — `mbedtls_base64_encode/decode` linked (273 + 304 B in `.text`) | **0 KiB** |
| `MBEDTLS_SHA256_C` + HMAC wrapper | Yes (HMAC-SHA256) | — | **On** — `mbedtls_sha256_*` and `mbedtls_md_hmac_*` all linked | **0 KiB** |
| `MBEDTLS_CHACHA20_C` | Yes (stream cipher) | — | **Off** (`# CONFIG_MBEDTLS_CHACHA20_C is not set`) | **+2.5–3.5 KiB** (upstream `chacha20.c` ~318 LOC, xtensa text+rodata band) |
| `MBEDTLS_HKDF_C` | Yes (Extract + Expand) | — | **Off** (no `HKDF` symbol in elf except a stray `hkdf_expand` from another library) | **+0.8–1.2 KiB** (single-file, reuses HMAC) |
| secp256k1 ECDH (shared X coord) | Yes | Yes | **secp256k1 module: off** — `ENABLE_MODULE_ECDH` not set in `components/secp256k1/CMakeLists.txt`. However `mbedtls_ecdh_calc_secret` IS linked AND `CONFIG_MBEDTLS_ECP_DP_SECP256K1_ENABLED=y` AND `CONFIG_MBEDTLS_ECDH_C=y`. Both NIPs can share **mbedTLS** ECDH on secp256k1 instead of enabling the secp256k1 ECDH module. | **0 KiB if reusing mbedTLS** (slow but ≤1 Hz). +1.5–2.5 KiB if enabling secp256k1 module for constant-time / speed. |
| secp256k1 schnorr **sign** + `keypair_create` | Yes (NIP-01 event sign) | Yes (same) | **Not linked.** `nm` shows only `secp256k1_schnorrsig_verify` (391 B), `secp256k1_xonly_pubkey_parse` (145 B), `secp256k1_schnorrsig_challenge` (103 B). **No** `schnorrsig_sign32`, `keypair_create`, `ec_pubkey_create`, `ecmult_gen` symbols today. | **+8–12 KiB**: `precomputed_ecmult_gen` table at `ECMULT_GEN_PREC_BITS=4` (~8 KiB rodata) + `secp256k1_ecmult_gen` (~0.5 KiB) + `keypair_create`/`xonly_pubkey_from_pubkey` (~0.7 KiB) + `schnorrsig_sign_internal` + nonce gen (~1.3 KiB) + serialize helpers (~0.5 KiB). |
| Padding/framing | Yes (NIP-44 padding scheme) | Yes (PKCS#7) | — | NIP-44 v2 framing ~0.6 KiB; NIP-04 wrapper (PKCS#7 + `?iv=` format) ~1.0 KiB |

### Section 2 numerical totals

**NIP-44 v2 path** (deltas over today's image):

- ChaCha20 enable: +3.0 KiB
- HKDF enable: +1.0 KiB
- Schnorr sign + ecmult_gen table: +10.0 KiB
- NIP-44 v2 framing code (HKDF-extract conversation_key, message_key derive,
  ChaCha20 stream, HMAC over AAD‖ciphertext, padding-to-power-of-2): +1.2 KiB
- ECDH: 0 KiB (reuse mbedTLS over secp256k1 curve)

**NIP-44 v2 crypto subtotal: ~15.2 KiB (12–18 KiB at 95% CI).**

**NIP-04 incremental delta on top of the above:**

- AES-CBC encrypt/decrypt + PKCS#7 pad helper (mbedTLS doesn't ship a
  separate PKCS#7 padder; trivial inline): +0.4 KiB
- NIP-04 content format (`<b64ct>?iv=<b64iv>`) + dispatcher branch on
  kind/encryption-version: +0.6 KiB
- ECDH shared point: shared with NIP-44 v2 → 0 KiB
- Sign: shared → 0 KiB

**NIP-04 incremental: ~1.0 KiB (0.7–1.5 KiB at 95% CI).** Well under the
2–3 KiB ceiling the brief sets, so the recommendation is unchanged:
**ship NIP-04 fallback alongside NIP-44 v2**.

> Caveat: the ChaCha20 and HKDF estimates extrapolate from upstream mbedTLS
> source LOC; they are not measured. Both have very small surface and
> conservative bounds. Schnorr sign-side estimate is bounded by the worst case
> of `ECMULT_GEN_PREC_BITS=4` (current pin via default) on libsecp256k1 v0.7.0.
> Worst-realistic case +14 KiB if the IDF defaults push `PREC_BITS` higher
> than 4.

---

## 3. Client state-machine + JSON-RPC layer

`components/nostr/` exists and registers: `parser.cpp`, `request_frame.cpp`,
`relay_client.cpp`, `subscription_manager.cpp`, `zap_listener.cpp`,
`event_verify.cpp`, `nostr_data_source.cpp`. Already requires
`esp_websocket_client`, `esp-tls`, `cjson`, `secp256k1`. So we have a working
relay client, subscription manager, REQ/EVENT/EOSE plumbing, signature
**verify**, and tag access (`Tag::at`, `Event::TagValue`) ready to reuse.

What needs to be added for NWC:

| Component | Est. flash | Notes |
|---|---|---|
| NWC URI parser (`nostr+walletconnect://<pubkey>?relay=<url>&secret=<hex>[&lud16=...]`) | **0.6–1.0 KiB** | Small URL/query-string parser; no new deps. |
| JSON-RPC marshalling (cJSON, already linked) — `{"method":"get_balance","params":{}}` and response decode | **1.2–2.0 KiB** | Plus per-method param/result builders. Read-only set (`get_info`, `get_balance`, `list_transactions`) = ~1.2 KiB. Full set adds `pay_invoice`, `pay_keysend`, `make_invoice`, `lookup_invoice`, `sign_message` = +1.5 KiB. |
| Encryption dispatcher (NIP-44 v2 first, fall back to NIP-04 based on `encryption` tag advertised by wallet in kind 13194 INFO event, per NIP-47) | **0.3 KiB** | |
| Event sign + publish wrapper (uses Schnorr sign side from §2) | **0.5 KiB** | Builds kind 23194 request event, signs, publishes via `RelayClient`. |
| Subscribe to kind 23195 (responses), match on `e` tag = our request id, decrypt content, dispatch JSON-RPC reply to pending future | **0.8 KiB** | Reuses `SubscriptionManager`. |
| Subscribe to **kind 23196 (notifications)** alongside 23195 — same REQ filter on `p` tag = our pubkey, decrypt via same dispatcher, route to notification handler | **+0.3 KiB** | Same path as 23195; the only delta is one extra kind in the filter and a dispatcher arm. |
| State machine: bootstrap (fetch wallet info kind 13194), idle, request-in-flight (with timeout), reconnect, exponential backoff | **1.5–2.5 KiB** | Mirrors `ZapListener::Start/Stop` shape (~1.5 KiB existing). |

**Plumbing subtotal (balance + notifications, read-only): ~5.8 KiB
(4.7–7.3 KiB at 95% CI).** Full set (outgoing payments etc.) adds ~2 KiB.

### Relay-budget invariant (3 + 1 / dynamic ≤ 4 total)

`components/settings/include/settings/nostr_config.hpp` pins
`kMaxNostrRelays = 4` — that's our internal-RAM ceiling on concurrent WSS+TLS
sessions (the existing nostr data-source / zap path uses up to 4). NWC's relay
is almost always distinct from the data-source / zap relays
(e.g. `wss://relay.getalby.com/v1` or wallet-specific). Two ways to keep RAM
flat while adding NWC:

- **A: Count NWC toward the existing 4.** Simple, no schema change, but a user
  who already runs 4 data relays must drop one before enabling NWC.
- **B: Split 3 data + 1 NWC (≤ 4 total).** Hard cap of 3 data relays whenever
  NWC is enabled, leaving 1 slot reserved for NWC. Existing 4-relay users
  with NWC disabled keep working unchanged.

**Recommended: dynamic split (B-ish).** Keep `kMaxNostrRelays = 4` permanent
across all relays. Enforce in `components/settings/settings_api.cpp` and in
the WebUI:

```
total_active_relays = data_relays.size() + (nwc.enabled ? 1 : 0)
reject if total_active_relays > 4
```

NVS schema: no break — the existing `nostrRelays` array stays. NWC adds its
own `nwc.uri` (which embeds the relay URL). On enabling NWC the API checks
the math and returns HTTP 400 with a clear error
`{"error":"max 3 data relays when NWC is enabled; drop one first"}`. No
auto-truncate — that would risk silent data loss.

WebUI implication: `data/src/lib/features/settings/sections/NostrRelayList.svelte`
already shows `relays.length/MAX_NOSTR_RELAYS`. Add a dynamic
`MAX_DATA_RELAYS = MAX_NOSTR_RELAYS - (nwcEnabled ? 1 : 0)`. Cost: trivial,
WebUI lives in the LittleFS image (separate partition, no OTA budget impact).

### Reused vs dedicated WSS socket

NWC wallets typically advertise a specific relay (e.g.
`wss://relay.getalby.com/v1`, `wss://nos.lol`,
`wss://relay.nostr.band`). It is **rare** that this URL coincides with what
the user has configured for price/zap relays (`damus.io`, `nostr.wine`, etc.).
Two options:

- **Reuse `RelayClient`** as a multi-relay manager (or instantiate a second
  relay-client object). Cost: ~+0.5–1 KiB code, +5–8 KiB heap (single extra
  WSS context: TLS session ~25 KiB only IF the URL is different and we open a
  separate `esp_websocket_client` handle; mbedTLS TLS session is the dominant
  cost).
- **Force user's NWC URI relay onto the same socket**: requires the wallet
  relay to equal the configured price relay, which won't be true in 99% of
  cases. Reject.

Realistic decision: **dedicated WSS for NWC**. Steady-state RAM delta =
+25–40 KiB heap for TLS session + 4 KiB rx buffer + 4 KiB tx buffer +
~2 KiB esp-websocket-client state. **Total ~35–50 KiB heap.** This is the
bottleneck — but it lands in internal SRAM/PSRAM, not in flash, so it does
NOT compete with the 38.7 KiB OTA headroom. Rev A has 2 MiB PSRAM headroom
for it (esp-tls's WANT_READ buffers are allocated from internal RAM though;
internal RAM headroom is ~70 KiB free in normal operation — tight but doable).

---

## 4. Settings + Webserver surface

`components/settings/` exists as a typed NVS settings system.
`components/webserver/control_server.cpp` is 3,352 LOC, uses a
trampoline-to-method pattern, `cfg.max_uri_handlers = 50` (with a comment
noting ~48 used today — **room for 1–2 new endpoints, probably 3–4 if we
register a single combined handler**).

NVS schema (v1):

- `nwc.uri` (string, single connection string — NIP-47 pairing URI; contains
  pubkey, relay, secret).
- `nwc.enabled` (bool).
- Optional `nwc.last_balance_msat` (int64) for fast display after boot.
- Optional `nwc.refresh_secs` (uint, default 60).

Webserver endpoints (single registered handler can multiplex):

- `GET /api/settings/nwc` → `{ enabled, uri_masked, refresh_secs, status }` —
  never return the secret in the URI.
- `POST /api/settings/nwc` → body `{ uri, enabled, refresh_secs }`. Validate
  `nostr+walletconnect://` scheme, hex pubkey 64 chars, valid relay URL,
  64-char hex secret.

Cost estimate:

- Settings struct + NVS load/store: ~0.8 KiB
- Two handlers + validation + trampolines: ~1.0–1.2 KiB
- WebUI: in `data/build_gz/www/` (separate LittleFS image, doesn't touch the
  app partition — free in OTA terms).

**Settings + Webserver subtotal: ~1.8–2.2 KiB.**

---

## 5. UI / screens

Existing screens under `main/screens/` already have text rendering, glyph
atlases, and the `DataSource → Screen` pattern (the brief implies
`NostrDataSource` is the model). Adding NWC:

- **Balance screen**: large-font sats display + smaller fiat conversion if
  price oracle available. Logic ~0.6 KiB, rendering reuses existing helpers.
  **~0.8–1.0 KiB.**
- **Notification screen / transient toast** (scenario 3, v1): triggered by
  kind 23196 notification arrival. Shows `+X sat received` /
  `−X sat sent` (with memo if present, trimmed) for ~5 s, then yields back
  to the regular screen rotation. Reuses the existing transient-screen pattern
  (zap notification screen is the precedent). Also fans out via SSE so the
  WebUI can flash a toast. **~0.8–1.2 KiB.**
- **Tx-list screen** (scenario 4 only): paginated list of N most-recent
  transactions, each row = timestamp + amount + memo trim. Layout + scroll
  state ~1.5–2.0 KiB.
- **NWC offline / error indicator overlay**: tiny, ~0.2 KiB.

**Read-only screens (sc. 3, with notifications): ~1.8–2.4 KiB.**
**Full (sc. 4): +1.5–2.0 KiB on top.**

---

## 6. Four scenarios — definitive sizing

Baseline reference: today, `build-rev-a/btclock_v4.bin = 1,729,808 B`; OTA
partition `0x1B0000 = 1,769,472 B`; **headroom = 39,664 B (38.7 KiB)**.

### Scenario 1: NIP-04 only, minimal, read-only (floor)

| Bucket | Mid | 95% CI |
|---|---|---|
| Schnorr sign + ecmult_gen table | 10.0 | 8–12 |
| NIP-04 wrapper (AES-CBC + b64 format + PKCS#7) | 1.0 | 0.7–1.5 |
| ECDH (mbedTLS reuse) | 0.0 | 0–2.5 (if secp256k1 ECDH module enabled instead) |
| NWC URI + JSON-RPC + state machine (read-only) | 5.5 | 4.5–7 |
| Settings + webserver | 2.0 | 1.8–2.2 |
| Balance screen | 1.0 | 0.8–1.2 |
| **Total flash Δ** | **~19.5 KiB** | **16–24 KiB** |

- Steady RAM: +5–8 KiB (state machine + per-method pending request slot,
  shared WSS socket case) OR +35–50 KiB (dedicated WSS).
- Per-request peak: +6–8 KiB (cJSON tree + AES scratch + base64 buffers).
- Heap fragmentation: low (most allocations bounded ≤1 KiB; can be moved to
  PSRAM).

### Scenario 2: NIP-44 v2 only, minimal, read-only

| Bucket | Mid | 95% CI |
|---|---|---|
| Schnorr sign + ecmult_gen table | 10.0 | 8–12 |
| ChaCha20 enable | 3.0 | 2.5–3.5 |
| HKDF enable | 1.0 | 0.8–1.2 |
| NIP-44 v2 framing wrapper | 1.2 | 0.9–1.6 |
| ECDH (mbedTLS reuse) | 0.0 | 0–2.5 |
| NWC URI + JSON-RPC + state machine (read-only) | 5.5 | 4.5–7 |
| Settings + webserver | 2.0 | 1.8–2.2 |
| Balance screen | 1.0 | 0.8–1.2 |
| **Total flash Δ** | **~23.7 KiB** | **20–28 KiB** |

RAM: same as scenario 1, +0.5 KiB for ChaCha20 ctx vs AES ctx (negligible).

### Scenario 3 (TARGET): NIP-44 v2 + NIP-04 fallback, balance + notifications

Differs from #2 by +1.0 KiB for the NIP-04 wrapper + dispatcher, +0.3 KiB for
the kind 23196 subscription, and +0.8 KiB for the notification screen.

| Bucket | Mid | 95% CI |
|---|---|---|
| Schnorr sign + ecmult_gen table | 10.0 | 8–12 |
| ChaCha20 enable | 3.0 | 2.5–3.5 |
| HKDF enable | 1.0 | 0.8–1.2 |
| NIP-44 v2 framing | 1.2 | 0.9–1.6 |
| NIP-04 wrapper + dispatcher | 1.0 | 0.7–1.5 |
| ECDH (mbedTLS reuse) | 0.0 | 0–2.5 |
| NWC URI + JSON-RPC + state machine (balance + notification path) | 5.8 | 4.7–7.3 |
| Settings + webserver (incl. 3+1 relay-budget enforcement) | 2.0 | 1.8–2.2 |
| Balance screen + notification screen | 1.8 | 1.6–2.2 |
| **Total flash Δ** | **~25.8 KiB** | **23–31 KiB** |

- Steady RAM (shared sock): +5–8 KiB. **Steady RAM (dedicated sock,
  realistic): +35–50 KiB heap (mostly TLS session — fits PSRAM).** Crucially:
  the 3+1 relay-budget invariant means the total number of concurrent WSS
  connections stays ≤ 4 — same as today — so no new internal-RAM ceiling.
- Per-request peak: +6–8 KiB scratch.
- Heap fragmentation risk: **low** for NWC traffic itself (small, infrequent),
  **moderate** at TLS-handshake reconnect time on the dedicated socket
  (one-shot ~25 KiB internal RAM peak; existing nostr code already pays this
  cost for the primary relay).

**Fit on Rev A: 38.7 − 25.8 = 12.9 KiB margin (mid). Worst-case 95% CI:
38.7 − 31 = 7.7 KiB margin. Ship.**

### Scenario 4: Both + full (outgoing pay/make/list_transactions)

| Bucket | Mid | 95% CI |
|---|---|---|
| All of scenario 3 | 25.8 | 23–31 |
| Extra JSON-RPC methods (pay_invoice, make_invoice, lookup_invoice, pay_keysend, list_transactions, sign_message) | 2.0 | 1.5–2.5 |
| Tx-list screen + pagination state | 2.0 | 1.5–2.5 |
| Invoice-display screen (QR/lnbc string render) | 1.5 | 1.0–2.0 |
| Outgoing-payment confirmation UI / web endpoint | 1.5 | 1.0–2.0 |
| Webserver: +2–3 endpoints (`POST /api/nwc/pay_invoice` etc., with auth + validation) | 2.0 | 1.5–2.5 |
| **Total flash Δ** | **~34.8 KiB** | **31–41 KiB** |

- Worst-case 95% CI = 41 KiB which **overshoots** 38.7 KiB.
- Steady RAM: same as #3 (~35–50 KiB heap dedicated sock).
- Per-request peak: 8–12 KiB (larger JSON for tx history listings).
- Heap fragmentation risk: **moderate-to-high**: `list_transactions` responses
  can be 8–16 KiB JSON; cJSON tree allocation will produce many small
  fragments. Mitigation: parse in a streaming fashion or allocate response
  buffer from PSRAM.

---

## 7. Rev A verdict + sequencing

### Does scenario 3 fit?

**Yes.** Mid estimate 25.8 KiB vs 38.7 KiB headroom → ~13 KiB margin (~33%).
Even at the 95% CI worst case (31 KiB), we still have ~7.7 KiB margin.
**Ship scenario 3 on Rev A unconditionally.**

### Does scenario 4 fit?

**Borderline / no.** Mid 34.8 KiB leaves only ~4 KiB margin, and the upper-CI
bound (41 KiB) overshoots. Given that other unrelated work (logging, screens,
fixes) typically consumes 2–5 KiB per release cycle, scenario 4 should be
considered **unsafe** for Rev A under current partitioning.

### Recommended compile-time gating

```kconfig
config BTCLOCK_NWC
    bool "Nostr Wallet Connect (NIP-47) — read-only"
    default y
    help
      Adds NIP-47 NWC client with NIP-44 v2 + NIP-04 fallback.
      Read-only (balance + info). Required for any NWC feature.

config BTCLOCK_NWC_FULL
    bool "NWC: outgoing payments + tx history"
    depends on BTCLOCK_NWC
    default n if BTCLOCK_BOARD_REV_A
    default y if BTCLOCK_BOARD_REV_B || BTCLOCK_BOARD_V8
    help
      Adds pay_invoice / make_invoice / list_transactions and the
      tx-list / invoice display screens. ~10 KiB extra flash; off on
      Rev A because the OTA partition is too tight.
```

This keeps Rev A on scenario 3 (target), Rev B and V8 on scenario 4. A
"NIP-04 only lite Rev A" variant is **not necessary** — the NIP-44 v2 path
costs only ~4 KiB more than NIP-04 alone, and it's the spec's future, so
dropping it would save almost nothing while making us a second-class client.

### Suggested implementation order

Tracked as beads epic `btclock_v4-lwf` and 8 child issues — see
`bd show btclock_v4-lwf` and `bd children btclock_v4-lwf`.

1. **lwf.1** — Enable `MBEDTLS_CHACHA20_C` and `MBEDTLS_HKDF_C` in the IDF
   defconfig fragment. Host-test the spec vectors round-trip.
2. **lwf.2** — Enable secp256k1 sign-side: pull
   `secp256k1_keypair_create` + `secp256k1_schnorrsig_sign32` via a new
   `nostr_sign.cpp` call site. Pin `ECMULT_GEN_PREC_BITS=4` explicitly in the
   component CMake so an IDF bump doesn't surprise us. Host-test a BIP-340
   test vector.
3. **lwf.3** — Implement NIP-44 v2 encrypt/decrypt + NIP-04 encrypt/decrypt +
   dispatcher under `components/nostr/src/nip4x.cpp`. Host-test the NIP-44 v2
   spec vectors verbatim (the spec repo ships an official JSON test file).
4. **lwf.4** — NWC URI parser + settings schema + webserver
   `GET/POST /api/settings/nwc` endpoints. **Plus the 3 + 1 relay-budget
   invariant** in `components/settings/settings_api.cpp` and the WebUI
   `NostrRelayList.svelte` dynamic cap. Host-test URI round-trip and
   budget-enforcement (4-relay user enables NWC → 400; NWC-enabled user adds
   4th relay → 400).
5. **lwf.5** — NWC client state machine (`components/nwc/`). Single REQ
   filter `{ kinds: [23195, 23196], "#p": [our_pubkey] }` covers both
   responses (23195) and notifications (23196). Dedicated WSS handle via a
   second `RelayClient`. Wire `get_balance` poll + kind 23196 notification
   dispatch into `NwcDataSource`.
6. **lwf.6** — Balance screen + transient notification screen under
   `main/screens/`. `NwcDataSource` updates from both the get_balance refresh
   tick AND immediate kind 23196 arrivals. Notification screen shows
   `+X sat received` / `−X sat sent` for ~5 s then yields. SSE event fans
   out to the WebUI.
7. **lwf.7** *(Rev B / V8 only, gated)* — Tx-list screen + `pay_invoice` /
   `make_invoice` / `list_transactions` JSON-RPC + outgoing webserver
   endpoints. Gated behind `CONFIG_BTCLOCK_NWC_FULL`.
8. **lwf.8** *(verification)* — After lwf.1–lwf.6 land on `main`, re-measure
   `build-rev-a/btclock_v4.bin` size and replace the §8 estimates with
   measured numbers.

---

## 8. Open questions / verification still needed

- **ChaCha20 / HKDF actual built sizes on xtensa-esp32s3 ESP-IDF v6.0.1.**
  The 3.0 / 1.0 KiB numbers above are extrapolated from upstream
  `mbedtls/library/chacha20.c` and `hkdf.c` line counts plus typical
  IDF-toolchain text+rodata ratios. **Needs follow-up verification** by
  toggling the two configs in a throwaway build and diffing `.bin` sizes.
  Confidence band: ±1.0 KiB combined.
- **Schnorr sign-side delta is the single biggest uncertainty.** The 8–12 KiB
  range comes from libsecp256k1 v0.7.0 source-LOC inspection + the default
  `ECMULT_GEN_PREC_BITS=4` table (8 KiB rodata). On an embedded build the
  linker may pull additional helpers transitively (e.g.
  `secp256k1_ec_seckey_negate`, `secp256k1_ec_pubkey_serialize`). **Needs
  follow-up verification** by enabling sign-side and measuring. Confidence
  band: ±2 KiB.
- **mbedTLS ECDH on `secp256k1` curve produces a 32-byte X coordinate
  exactly** (which is what both NIP-04 "raw" and NIP-44 v2 want before
  HKDF). This is consistent with `mbedtls_ecdh_calc_secret` source behavior
  in IDF's mbedTLS, but **verify with a known test vector** before committing
  to the "skip secp256k1 ECDH module" decision. If verification fails, the
  fallback is +1.5–2.5 KiB to enable `ENABLE_MODULE_ECDH` in
  `components/secp256k1/CMakeLists.txt` — still inside the scenario-3 margin.
- **Whether the secp256k1 ECDH module's hashfp callback** can be passed
  `secp256k1_ecdh_hash_function_default` (which applies SHA-256 to the
  compressed point) vs `NULL` (raw X). NIP-44 v2 spec wants raw X. Verified
  path: pass a custom hash function that returns the raw X-coord 32 bytes.
  **Not a sizing question, an implementation note** for the NIP-44 v2
  implementation issue.
- **NWC wallet field support for NIP-44 v2 encryption.** Per NIP-47, wallets
  advertise supported encryptions via the kind 13194 INFO event tag
  `encryption` (added in 2024). The fallback heuristic in §3 ("dispatch on
  advertised encryption") assumes wallets we care about emit this tag. Some
  older wallets don't, in which case the safe default is NIP-04. **Open
  question** for product: do we default to NIP-04 when the tag is missing, or
  refuse to talk to the wallet?
- **Internal-RAM headroom** for the dedicated WSS TLS session. mbedTLS's TLS
  rx buffer (`MBEDTLS_SSL_IN_CONTENT_LEN`) on this build wasn't probed; if
  it's the IDF default 16384 the per-connection cost is ~25 KiB internal
  heap, which is tight on Rev A under existing load. **Needs follow-up
  verification** with `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` after
  the second WSS handle is brought up. Possible mitigation: lower
  `MBEDTLS_SSL_IN_CONTENT_LEN` for NWC's relay specifically, or accept that
  the existing `RelayClient`'s TLS session is reused.
- **`max_uri_handlers = 50` budget** has ~2 handlers free today (current
  usage cited as 48 in `control_server.cpp`'s comment). If scenario 4 lands
  later with 3 extra endpoints, that budget needs a bump — trivial, but easy
  to forget.
- **Did NOT confirm by direct inspection** the exact list of headers in
  `components/nostr/include/`; the brief's `Event` / `Tag` / `RelayClient` /
  `SubscriptionManager` shapes were inferred from the symbol table only. Low
  risk because the existing `nostr_data_source.cpp` is clearly already wired
  to the right primitives, but mark this as a follow-up before the NWC client
  state-machine issue lands.
