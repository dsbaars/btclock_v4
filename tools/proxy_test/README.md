# Local proxy rig

Stand up a 3proxy container exposing SOCKS4/4a/5 and HTTP CONNECT
listeners on localhost, with both anonymous and Basic-auth variants.
Used to verify the firmware's `proxy_transport` component end-to-end
without depending on an internet-side proxy.

The container is built locally (multi-stage debian-slim) from a
pinned 3proxy release tag. Modeled on the upstream `Dockerfile.full`
but simplified — it builds natively on whatever architecture the
Docker daemon is running on (so arm64 on Apple Silicon, no Rosetta).
First `docker compose up -d` does the build (~1 minute); subsequent
runs are instant.

## Start

```bash
cd tools/proxy_test
docker compose up -d
docker compose logs -f proxy   # tail per-connection logs
```

## Listener map

| Port | Protocol               | Auth                      |
|------|------------------------|---------------------------|
| 1080 | SOCKS (4 / 4a / 5)     | none                      |
| 1081 | SOCKS5                 | `testuser:testpass`       |
| 3128 | HTTP CONNECT + forward | none                      |
| 3129 | HTTP CONNECT + forward | Basic `testuser:testpass` |

3proxy auto-detects the SOCKS version from the client greeting byte —
the same listener handles SOCKS4, SOCKS4a, and SOCKS5. The device
chooses which version to emit; the server doesn't care.

## Smoke-test from the host

```bash
# SOCKS5 with remote DNS (proxy resolves the target)
curl --socks5-hostname 127.0.0.1:1080 https://api.coinbase.com/v2/time

# HTTP CONNECT to HTTPS endpoint
curl --proxy http://127.0.0.1:3128 https://api.coinbase.com/v2/time

# SOCKS5 with auth
curl --socks5-hostname testuser:testpass@127.0.0.1:1081 \
     https://api.coinbase.com/v2/time

# HTTP CONNECT with Basic auth
curl --proxy-user testuser:testpass --proxy http://127.0.0.1:3129 \
     https://api.coinbase.com/v2/time

# Force SOCKS4 (no remote DNS — curl resolves first)
curl --socks4 127.0.0.1:1080 https://api.coinbase.com/v2/time

# SOCKS4a (proxy resolves)
curl --socks4a 127.0.0.1:1080 https://api.coinbase.com/v2/time
```

If any of those return a non-empty JSON body, the proxy stack is good.
Confirm the connection went through the proxy in `docker compose logs`.

## Pointing the device at the rig

Set in WebUI / via `PATCH /api/settings`:

| Field            | Value (anon SOCKS5) | Value (Basic HTTP)  |
|------------------|---------------------|---------------------|
| `proxyEnabled`   | `true`              | `true`              |
| `proxyType`      | `4` (SOCKS5)        | `1` (HTTP CONNECT)  |
| `proxyHost`      | `<host LAN IP>`     | `<host LAN IP>`     |
| `proxyPort`      | `1080`              | `3128`              |
| `proxyUser`      | (empty)             | (empty)             |
| `proxyPass`      | (empty)             | (empty)             |
| `proxyBypass`    | `*.local,192.168.*` | `*.local,192.168.*` |

Use the host's LAN IP — `127.0.0.1` won't work from the device.
`docker compose.yml` only binds to `127.0.0.1` by default; expose
`0.0.0.0` for hardware testing by editing the `ports:` block:

```yaml
ports:
  - "1080:1080"
  - "1081:1081"
  - "3128:3128"
  - "3129:3129"
```

## Stop

```bash
docker compose down
```
