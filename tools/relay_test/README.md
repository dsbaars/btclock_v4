# Per-relay flap simulator

Reproduce a "one of three Nostr relays goes flaky" scenario on a live
device, so we can correlate the failure mode (TLS handshake errors,
mid-stream drops, connection storms) against `/api/diag/heap`
evolution.

## Architecture

```
   device                                rig (192.168.21.88)                          internet
   ──────                                ────────────────────                          ────────
   wss://relay.primal.net
                  │
                  │  SOCKS5 CONNECT (ATYP=domain)  → relay.primal.net:443
                  ▼
            socks5_router.py:1180
                  │
                  │  per-host destination rewrite:
                  │     relay.primal.net  → 172.28.0.10:9443
                  │     nos.lol           → 172.28.0.10:9444
                  │     nostr.dbtc.link   → 172.28.0.10:9445
                  ▼
            toxiproxy:9443  ──TCP──►  relay.primal.net:443
            toxiproxy:9444  ──TCP──►  nos.lol:443
            toxiproxy:9445  ──TCP──►  nostr.dbtc.link:443
```

The firmware's `proxy_transport` always emits SOCKS5 CONNECT with
ATYP=domain (`proxy_framing.cpp:76`), so the proxy resolves the
hostname. Our SOCKS5 server (`socks5_router.py`) inspects the
hostname and silently rewrites the upstream destination for the three
test relays, pointing each at the matching toxiproxy listener. The
device's TLS SNI stays `relay.primal.net` (from the URL hostname) and
flows through unchanged, so the upstream relay's certificate
validates without any TLS inspection or hostname rewriting on our
side. The device URLs stay clean — no port suffixes — and **no LAN
DNS plumbing is needed**.

## Bring-up

```bash
cd tools/relay_test
docker compose up -d
./bootstrap-toxiproxy.sh
```

Smoke-test the SOCKS5 path from the host (no device involved):

```bash
curl -v --max-time 6 --socks5-hostname 127.0.0.1:1180 \
     https://relay.primal.net:9443/ 2>&1 | grep -E 'subject:|HTTP/|verify'
# Expect: cert subject CN=*.primal.net, TLS verify ok.
# `--socks5-hostname` makes curl send the hostname (not pre-resolved
# IP) — same shape the firmware uses.
```

## Pointing the device at the rig

A single PATCH enables the SOCKS5 proxy. Relay URLs stay normal —
the rewrite happens inside the proxy.

```bash
DEV=192.168.20.97   # Rev B in the lab

curl -X PATCH -H 'Content-Type: application/json' -d '{
  "proxyEnabled": true,
  "proxyType":    4,
  "proxyHost":    "192.168.21.88",
  "proxyPort":    1180,
  "proxyBypass":  "*.local,192.168.*"
}' http://$DEV/api/settings

# proxyEnabled is runtime — the existing WS sources reconnect through
# the new proxy on the next iteration. No reboot needed unless
# nostrRelays themselves changed (they're boot_only).
```

Verify connections are landing on the rig:

```bash
docker compose logs -f socks5    # SOCKS5 CONNECT lines from the device
docker compose logs -f toxiproxy # TCP forwards through to the relays
```

## Apply / clear toxics

```bash
./apply-toxic.sh <relay> <toxic> [arg]
./apply-toxic.sh <relay> clear

# Examples
./apply-toxic.sh primal timeout 5000     # close every TCP after 5 s
./apply-toxic.sh dbtc   reset_peer 30000 # TCP RST 30 s into stream
./apply-toxic.sh noslol latency 2000     # 2 s extra one-way (slow,
                                         # but functional)
./apply-toxic.sh primal clear            # restore healthy upstream
```

`<relay>` is the toxiproxy proxy name (`primal` / `noslol` / `dbtc`),
not the URL host. `clear` removes every toxic on that proxy — safe to
run anytime.

## What to watch on the device

```bash
# Heap evolution — capture both healthy and toxic windows.
curl -s http://<device>/api/diag/heap | jq '.caps.internal'

# Or the long-form watcher used during the original investigation:
python3 /tmp/heap_watcher.py     # writes /tmp/heap_evolution.csv
```

The signal we expect on a leaky relay-failure path:
- `internal.allocated_blocks` ratchets up on each toxic cycle
  (allocations not freed in the failure path).
- `internal.largest_free_block` collapses faster than `total_free`,
  i.e. fragmentation rather than steady leak.
- Serial log shows `esp-aes: Failed to allocate memory` or `mbedtls`
  read errors clustered around the toxic injection.

## Tear down

```bash
./apply-toxic.sh primal clear
./apply-toxic.sh noslol clear
./apply-toxic.sh dbtc   clear
docker compose down

# And on the device — drop the proxy. Relay URLs were never changed,
# so nothing else to restore.
curl -X PATCH -H 'Content-Type: application/json' -d '{
  "proxyEnabled": false
}' http://$DEV/api/settings
```
