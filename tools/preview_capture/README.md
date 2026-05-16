# preview_capture / capture.mjs

Pull the EPD framebuffer from a running BTClock via `/api/preview/ws` and
write one PNG per panel plus a stitched `combined.png` — no camera, no
frontlight, no glare. Useful for visual regression checks (and for AI
vision: the resulting PNGs are crisp 1-bpp renders the device just
painted, not photographs).

The binary frame layout this tool decodes lives in
`components/webserver/include/preview_packet.hpp`. The WebUI's preview
panel parses the same frames; keep this tool in sync with that header.

## Requirements

Node 20+ (uses built-in `globalThis.crypto.getRandomValues` and
`node:zlib`). No `npm install`.

## Usage

```sh
# Default host = btclock-9d5530.local (Rev B). Override with --host.
node capture.mjs --out /tmp/snap

# With HTTP Basic auth:
node capture.mjs --host btclock-rev-a.local --user admin --pass hunter2

# Stop as soon as we have all 8 panels (cuts the post-first-frame wait):
node capture.mjs --panels 8 --out /tmp/snap
```

## Output

- `<out>/panel-NN.png` — one PNG per panel index, 8-bit grayscale.
- `<out>/combined.png` — horizontal strip of all panels in index order.
- `<out>/frame-meta.json` — header fields per panel (width, stride,
  rotation, frame id, timestamp) for debugging.

## Notes

- Prefer mDNS hostnames over LAN IPs (project rule).
- 1-bpp pixel mapping follows the EPD convention: bit=0 means ink
  (rendered black), bit=1 means background (rendered white).
- If the firmware was built with `CONFIG_HTTPD_WS_SUPPORT=n`, the
  upgrade succeeds but no frames will be sent — the tool then exits
  with status 2 ("no preview frames received") after the timeout.
