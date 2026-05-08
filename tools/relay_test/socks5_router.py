#!/usr/bin/env python3
"""Tiny SOCKS5 server that rewrites a fixed set of destinations.

The firmware's `proxy_transport` always emits SOCKS5 CONNECT with
ATYP=domain (`proxy_framing.cpp:76`), so the proxy resolves the
hostname. We exploit that: when the device asks for one of the three
test relays, we CONNECT to the matching toxiproxy listener on the
docker bridge instead. The TLS handshake the device runs through us is
opaque (we just shovel bytes), so SNI flows through unchanged and the
real relay's certificate validates.

3proxy 0.9.5's `nsrecord` directive doesn't exist — that's why we're
not using 3proxy here. Dante would work but pulls in libwrap / a CRAN
dependency; this script is ~120 lines, no extra packages.

Listens on 0.0.0.0:1080 by default. Anonymous (no auth) — the rig is
local-LAN only.
"""

from __future__ import annotations

import asyncio
import logging
import os
import sys

# Per-relay rewrite. Hostnames the device asks for (left) are
# redirected to (host, port) on the right — pointing at the matching
# toxiproxy listener inside the compose network.
TOXIPROXY_HOST = os.environ.get("TOXIPROXY_HOST", "172.28.0.10")
OVERRIDES = {
    "relay.primal.net": (TOXIPROXY_HOST, 9443),
    "nos.lol": (TOXIPROXY_HOST, 9444),
    "nostr.dbtc.link": (TOXIPROXY_HOST, 9445),
}

LOG = logging.getLogger("socks5")


async def _read_exact(reader: asyncio.StreamReader, n: int) -> bytes:
    buf = await reader.readexactly(n)
    return buf


async def _pump(src: asyncio.StreamReader, dst: asyncio.StreamWriter) -> None:
    try:
        while True:
            chunk = await src.read(8192)
            if not chunk:
                break
            dst.write(chunk)
            await dst.drain()
    except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
        pass
    finally:
        try:
            dst.close()
        except Exception:
            pass


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    try:
        # SOCKS5 greeting: VER NMETHODS METHODS...
        ver, nmethods = await _read_exact(reader, 2)
        if ver != 0x05:
            return
        await _read_exact(reader, nmethods)
        writer.write(b"\x05\x00")  # METHOD = NO AUTH
        await writer.drain()

        # CONNECT request: VER CMD RSV ATYP ...
        ver, cmd, rsv, atyp = await _read_exact(reader, 4)
        if ver != 0x05 or cmd != 0x01:
            writer.write(b"\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00")
            await writer.drain()
            return
        if atyp == 0x03:  # domain
            (hlen,) = await _read_exact(reader, 1)
            host = (await _read_exact(reader, hlen)).decode("ascii", "ignore")
        elif atyp == 0x01:  # ipv4
            host = ".".join(str(b) for b in await _read_exact(reader, 4))
        elif atyp == 0x04:  # ipv6
            raw = await _read_exact(reader, 16)
            host = ":".join(raw[i:i + 2].hex() for i in range(0, 16, 2))
        else:
            writer.write(b"\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00")
            await writer.drain()
            return
        port_b = await _read_exact(reader, 2)
        port = port_b[0] * 256 + port_b[1]

        # Override + log.
        upstream_host, upstream_port = OVERRIDES.get(host, (host, port))
        rewritten = (upstream_host, upstream_port) != (host, port)
        LOG.info(
            "%s -> %s:%d%s",
            f"{peer[0]}:{peer[1]}" if peer else "?",
            host,
            port,
            f"  (rewritten -> {upstream_host}:{upstream_port})" if rewritten else "",
        )

        # Connect upstream + reply.
        try:
            u_reader, u_writer = await asyncio.wait_for(
                asyncio.open_connection(upstream_host, upstream_port), timeout=10
            )
        except (OSError, asyncio.TimeoutError) as exc:
            LOG.warning("connect %s:%d failed: %s", upstream_host, upstream_port, exc)
            writer.write(b"\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00")  # connection refused
            await writer.drain()
            return
        writer.write(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")  # success
        await writer.drain()

        await asyncio.gather(_pump(reader, u_writer), _pump(u_reader, writer))
    except asyncio.IncompleteReadError:
        pass
    except Exception as exc:  # noqa: BLE001
        LOG.warning("handler error from %s: %s", peer, exc)
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    server = await asyncio.start_server(handle, "0.0.0.0", 1080)
    addrs = ", ".join(str(s.getsockname()) for s in server.sockets)
    LOG.info("listening on %s; overrides=%s", addrs, list(OVERRIDES.keys()))
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
