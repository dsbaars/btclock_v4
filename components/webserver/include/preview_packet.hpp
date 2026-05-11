// Binary frame format the WebUI consumes over the /api/preview/ws
// WebSocket. Header-only on purpose so the host-tests in test_host/
// can exercise the byte layout without dragging in IDF headers — the
// WebUI parser at data/build_gz/www/build/* mirrors this exact
// layout and a silent header drift would corrupt every preview
// frame on the client without any server-side error.
//
// Little-endian, fixed-size 34-byte prelude:
//   0..3   magic "BTFB"
//   4      version (kPreviewVersion)
//   5      message kind (kPreviewKindPanelFrame)
//   6      compression kind (kPreviewCompressionNone | Deflate)
//   7      bit depth (1 — 1-bpp source framebuffer)
//   8      panel index
//   9      reserved (zero)
//   10..11 width
//   12..13 height
//   14..15 stride (bytes/row)
//   16..17 rotation degrees clockwise
//   18..21 frame id
//   22..25 timestamp ms (low 32 bits)
//   26..29 compressed payload bytes
//   30..33 raw payload bytes

#pragma once

#include <cstddef>
#include <cstdint>

namespace btclock {

inline constexpr std::size_t kPreviewHeaderBytes = 34;
inline constexpr uint8_t kPreviewVersion = 1;
inline constexpr uint8_t kPreviewKindPanelFrame = 1;
inline constexpr uint8_t kPreviewCompressionNone = 0;
inline constexpr uint8_t kPreviewCompressionDeflate = 1;
inline constexpr uint8_t kPreviewSourceBitDepth = 1;
inline constexpr uint8_t kPreviewMagic[4] = {'B', 'T', 'F', 'B'};

struct PreviewPanelHeader {
  uint8_t compression_kind;
  uint8_t panel_index;
  uint16_t width;
  uint16_t height;
  uint16_t stride;
  uint16_t rotation_deg;
  uint32_t frame_id;
  uint32_t timestamp_ms;
  uint32_t payload_size;
  uint32_t raw_size;
};

namespace preview_packet_detail {

inline void WriteLe16(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFF);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void WriteLe32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFF);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace preview_packet_detail

// Write the 34-byte panel-frame header into `out`. Caller owns `out`
// and must provide at least `kPreviewHeaderBytes` bytes — no internal
// bounds check (this is on the per-frame hot path).
inline void WritePreviewPanelHeader(uint8_t* out, const PreviewPanelHeader& h) {
  using preview_packet_detail::WriteLe16;
  using preview_packet_detail::WriteLe32;
  out[0] = kPreviewMagic[0];
  out[1] = kPreviewMagic[1];
  out[2] = kPreviewMagic[2];
  out[3] = kPreviewMagic[3];
  out[4] = kPreviewVersion;
  out[5] = kPreviewKindPanelFrame;
  out[6] = h.compression_kind;
  out[7] = kPreviewSourceBitDepth;
  out[8] = h.panel_index;
  out[9] = 0;
  WriteLe16(out + 10, h.width);
  WriteLe16(out + 12, h.height);
  WriteLe16(out + 14, h.stride);
  WriteLe16(out + 16, h.rotation_deg);
  WriteLe32(out + 18, h.frame_id);
  WriteLe32(out + 22, h.timestamp_ms);
  WriteLe32(out + 26, h.payload_size);
  WriteLe32(out + 30, h.raw_size);
}

}  // namespace btclock
