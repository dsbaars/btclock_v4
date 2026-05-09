# WebUI Framebuffer Preview (WebSocket)

This feature streams the **actual 1-bit EPD framebuffer** from firmware to
the WebUI over WebSocket so the preview can be pixel-accurate.

## Endpoint

- Route: `/api/preview/ws`
- Transport: WebSocket (HTTP GET upgrade)
- Auth: same Basic-auth gate as other `/api/*` routes (`httpAuthEnabled`)

The connection starts in **idle** mode. Streaming is opt-in per client:

- Start: send text `start` or JSON `{"action":"start"}`
- Stop: send text `stop` or JSON `{"action":"stop"}`

Firmware replies with a small JSON ack:

- `{"streaming":true}`
- `{"streaming":false}`

## Binary Frame Format

Every panel snapshot is one binary WS message:

- Header: fixed 34 bytes, little-endian
- Payload: deflate-compressed framebuffer bytes (zlib wrapper)

Header layout:

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `BTFB` |
| 4 | 1 | version | `1` |
| 5 | 1 | kind | `1` = panel frame |
| 6 | 1 | compression | `1` = deflate (zlib stream) |
| 7 | 1 | bit depth | `1` |
| 8 | 1 | panel index | 0-based |
| 9 | 1 | reserved | `0` |
| 10 | 2 | width | pixels |
| 12 | 2 | height | pixels |
| 14 | 2 | stride | bytes per row |
| 16 | 2 | rotation_deg | clockwise |
| 18 | 4 | frame_id | monotonically increasing |
| 22 | 4 | timestamp_ms | low 32 bits of ms time |
| 26 | 4 | payload_len | compressed bytes |
| 30 | 4 | raw_len | uncompressed bytes |

Payload is raw packed 1-bpp framebuffer bytes:

- bit = `1` => white
- bit = `0` => black
- row-major, `stride` bytes per row

## Performance / Safety Notes

- Streaming is disabled until the client explicitly starts it.
- Compression and socket sends run in a dedicated worker task.
- Main render path only publishes a latest-wins snapshot pointer/copy.
- If network is slow, older frames are dropped and latest frame wins.

This keeps normal display updates responsive while still giving a useful
live preview in the WebUI.
