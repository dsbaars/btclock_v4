# Outbound proxy

Every TCP connection the firmware initiates — Mempool/Kraken WebSockets,
the Coinbase / CoinGecko / Bitaxe HTTP polls, mining-pool API calls,
Nostr relay sockets, OTA firmware downloads, mining-pool logo fetches —
can be routed through an HTTP CONNECT or SOCKS proxy. There is no
per-source override; once enabled, the proxy applies to all outbound
traffic.

The intended use cases are: running the device on a network that blocks
direct egress (corporate / hotel / Tor-only setups), tunnelling through
a known-good upstream when the local DNS / TLS path is unreliable, or
funnelling traffic through a logging / inspection box for debugging.

## Supported types

| `proxyType` | Protocol     | Remote DNS | Auth  | RFC      |
|------------:|--------------|------------|-------|----------|
| `0`         | none (direct) | n/a       | n/a   | —        |
| `1`         | HTTP CONNECT | yes        | Basic | RFC 7231 |
| `2`         | SOCKS4       | no         | none  | —        |
| `3`         | SOCKS4a      | yes        | none  | —        |
| `4`         | SOCKS5       | yes        | user/pass | RFC 1928 / 1929 |

"Remote DNS" means the proxy resolves the destination hostname. Plain
SOCKS4 forces the device to resolve first and only forwards an IPv4
literal to the proxy — pick SOCKS4a or SOCKS5 if you want the proxy to
do the DNS work (typical for Tor SOCKS, where the device cannot resolve
`.onion` itself).

Auth is only meaningful on SOCKS5 and HTTP CONNECT. SOCKS4/4a have no
authentication frame in the protocol; `proxyUser` and `proxyPass` are
ignored when `proxyType` is 2 or 3. If `proxyUser` is empty on a SOCKS5
connection the firmware sends method `0x00` (no-auth); a non-empty
username triggers `0x02` (user/pass, RFC 1929).

TLS terminates on the device, not on the proxy. The proxy sees only an
opaque TLS stream — it doesn't decrypt or inspect the application-layer
data. The device verifies the destination certificate against the IDF
certificate bundle exactly the same as a direct connection.

## Configuration fields

All seven fields are persisted under the `settings` NVS namespace and
exposed via `GET/PATCH /api/settings`. None are reboot-only — a PATCH
takes effect on the next outbound connection, and existing WebSocket
sessions reconnect through the new path automatically.

| Field           | Type   | Bounds        | Default                          |
|-----------------|--------|---------------|----------------------------------|
| `proxyEnabled`  | bool   | —             | `false`                          |
| `proxyType`     | u8     | 0..4          | `0` (none)                       |
| `proxyHost`     | string | non-empty when enabled | (empty)                  |
| `proxyPort`     | u16    | 1..65535      | `1080`                           |
| `proxyUser`     | string | —             | (empty)                          |
| `proxyPass`     | string | —             | (empty)                          |
| `proxyBypass`   | string | comma-separated globs | `*.local,192.168.*,10.*,127.0.0.1` |

`proxyPass` is suppressed in `GET /api/settings` responses (mirrors
`httpAuthPass`) — `proxyPassSet: true|false` indicates whether a password
is stored without leaking the value. Set with PATCH; clear by PATCHing
an empty string.

Example PATCH (SOCKS5 with auth):

```bash
curl -X PATCH http://<ip>/api/settings \
  -H 'Content-Type: application/json' \
  -d '{
    "proxyEnabled": true,
    "proxyType": 4,
    "proxyHost": "192.168.1.10",
    "proxyPort": 1080,
    "proxyUser": "alice",
    "proxyPass": "secret",
    "proxyBypass": "*.local,192.168.*"
  }'
```

## Bypass list

`proxyBypass` is a comma-separated list of host patterns that are
contacted **directly**, skipping the proxy. Matching is case-insensitive
and runs against the destination hostname (not the resolved IP), in
this order:

| Pattern        | Matches                                                    |
|----------------|------------------------------------------------------------|
| `example.com`  | exact host (`example.com` only, not `api.example.com`)     |
| `*.local`      | leading-star — any host ending in `.local`                 |
| `192.168.*`    | trailing-star — any host starting with `192.168.`          |
| `*`            | match all (effectively disables the proxy)                 |

Whitespace around commas is trimmed. The default
(`*.local,192.168.*,10.*,127.0.0.1`) keeps LAN / mDNS / loopback traffic
off the proxy path so a misconfigured public proxy can't black-hole
local services.

There is no negation syntax — bypass is allow-only. If you want
"everything via the proxy except X", list X. If you want "only X via the
proxy", that is not expressible today; disable the proxy globally and
enable it from a network that already routes through the proxy upstream.

## Known limitations

- **No UDP**: the proxy is TCP-only. NTP runs over UDP and is unaffected
  by these settings — it always goes direct. If your network blocks
  outbound UDP/123, configure NTP to use a server reachable from inside
  the network, or rely on the on-device RTC drift between successful
  syncs.
- **No GSSAPI / Kerberos / NTLM**: HTTP CONNECT only sends RFC 7617
  Basic auth (base64 of `user:pass`). Enterprise proxies that require
  integrated Windows auth aren't supported.
- **No PAC / WPAD**: there is no proxy auto-config support. Set host
  and port explicitly.
- **No SOCKS5 GSSAPI auth**: the SOCKS5 implementation negotiates only
  method `0x00` (no-auth) and `0x02` (user/pass). Method `0x01` (GSSAPI)
  is not implemented — proxies that *require* it will reject the
  greeting.
- **No upstream chaining**: a single hop. There is no built-in way to
  proxy-through-a-proxy from the device side; if you need that, do it
  on the proxy host (e.g. 3proxy `parent` directive).
- **Boot order**: settings are loaded before the network stack, so a
  freshly written proxy config takes effect immediately on the next
  connection attempt. The first connection after a reboot still has to
  wait for WiFi association — measured floor is ~15 s on Rev A/B from
  cold boot, with the proxy adding under 100 ms of handshake overhead
  after that.

## Troubleshooting

Confirm the device is actually using the proxy:

```bash
curl http://<ip>/api/status | jq '.connectionStatus'
```

`connectionStatus.price`, `.V2`, `.blocks`, `.nostr` flip to `true` as
each upstream WebSocket parses its first frame. If they stay `false` for
more than ~30 s after enabling the proxy, the handshake is failing —
check the proxy server's log for SOCKS / HTTP CONNECT errors.

The serial console (`idf.py monitor`) prints one line per proxy event
under tag `proxy_socket_io`. Common failures:

- `connect(host:0) failed: 113` — port resolution lost between the
  WebSocket client and the transport. This was a real bug on early
  builds and is fixed; if you see it on a current image, please file
  an issue.
- `proxy auth required` — the upstream returned `407 Proxy
  Authentication Required` (HTTP CONNECT) or rejected the SOCKS5 method
  list. Check `proxyUser` / `proxyPass`.
- `SOCKS5 reply 0x05` — connection refused by the proxy's upstream
  policy. The destination is allowed by the proxy ACL but the upstream
  refused; not a device-side problem.

### Local test rig

A docker-compose rig at [`tools/proxy_test/`](https://git.btclock.dev/btclock/btclock_v4/src/branch/main/tools/proxy_test)
in the firmware repo brings up a 3proxy container with four listeners
(SOCKS anon / SOCKS authed / HTTP CONNECT anon / HTTP CONNECT authed).
Useful for verifying device behaviour without hammering a public proxy:

```bash
cd tools/proxy_test
docker compose up -d
docker compose logs -f proxy   # tail per-connection logs
```

The README inside that directory documents listener ports, smoke-test
curls, and how to expose `0.0.0.0` for hardware-in-the-loop testing
(the default binds to `127.0.0.1` so the listeners aren't reachable from
the LAN).
