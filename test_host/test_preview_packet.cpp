// Host tests for the WebSocket preview frame header layout. The WebUI
// parser at data/build_gz/www/build/* mirrors this byte order — a
// silent drift here corrupts every preview packet on the client with
// no server-side error, so we pin magic, version, struct size and the
// little-endian byte placement.

#include <array>
#include <cstdint>
#include <cstring>

#include "doctest.h"
#include "preview_packet.hpp"

using btclock::kPreviewCompressionDeflate;
using btclock::kPreviewCompressionNone;
using btclock::kPreviewHeaderBytes;
using btclock::kPreviewKindPanelFrame;
using btclock::kPreviewMagic;
using btclock::kPreviewSourceBitDepth;
using btclock::kPreviewVersion;
using btclock::PreviewPanelHeader;
using btclock::WritePreviewPanelHeader;

namespace {

PreviewPanelHeader MakeHeader() {
  // Distinct, non-zero values per field so a swapped offset shows up
  // as a numeric mismatch instead of a coincidental zero match.
  return PreviewPanelHeader{
      .compression_kind = kPreviewCompressionDeflate,
      .panel_index = 5,
      .width = 0x1234,
      .height = 0x5678,
      .stride = 0x9ABC,
      .rotation_deg = 270,
      .frame_id = 0xDEADBEEFu,
      .timestamp_ms = 0x01020304u,
      .payload_size = 0xCAFEBABEu,
      .raw_size = 0x10203040u,
  };
}

}  // namespace

TEST_CASE("preview_packet: header constants match WebUI contract") {
  CHECK(kPreviewHeaderBytes == 34);
  CHECK(kPreviewVersion == 1);
  CHECK(kPreviewKindPanelFrame == 1);
  CHECK(kPreviewCompressionNone == 0);
  CHECK(kPreviewCompressionDeflate == 1);
  CHECK(kPreviewSourceBitDepth == 1);
  CHECK(kPreviewMagic[0] == 'B');
  CHECK(kPreviewMagic[1] == 'T');
  CHECK(kPreviewMagic[2] == 'F');
  CHECK(kPreviewMagic[3] == 'B');
}

TEST_CASE("preview_packet: WritePreviewPanelHeader places magic + fixed fields") {
  std::array<uint8_t, kPreviewHeaderBytes> buf{};
  WritePreviewPanelHeader(buf.data(), MakeHeader());

  CHECK(buf[0] == 'B');
  CHECK(buf[1] == 'T');
  CHECK(buf[2] == 'F');
  CHECK(buf[3] == 'B');
  CHECK(buf[4] == kPreviewVersion);
  CHECK(buf[5] == kPreviewKindPanelFrame);
  CHECK(buf[6] == kPreviewCompressionDeflate);
  CHECK(buf[7] == kPreviewSourceBitDepth);
  CHECK(buf[8] == 5);
  CHECK(buf[9] == 0);  // reserved must be zero
}

TEST_CASE("preview_packet: little-endian width/height/stride/rotation") {
  std::array<uint8_t, kPreviewHeaderBytes> buf{};
  WritePreviewPanelHeader(buf.data(), MakeHeader());

  // width 0x1234 -> 0x34, 0x12
  CHECK(buf[10] == 0x34);
  CHECK(buf[11] == 0x12);
  // height 0x5678
  CHECK(buf[12] == 0x78);
  CHECK(buf[13] == 0x56);
  // stride 0x9ABC
  CHECK(buf[14] == 0xBC);
  CHECK(buf[15] == 0x9A);
  // rotation 270 = 0x010E
  CHECK(buf[16] == 0x0E);
  CHECK(buf[17] == 0x01);
}

TEST_CASE("preview_packet: little-endian frame_id / ts / payload / raw") {
  std::array<uint8_t, kPreviewHeaderBytes> buf{};
  WritePreviewPanelHeader(buf.data(), MakeHeader());

  // frame_id 0xDEADBEEF
  CHECK(buf[18] == 0xEF);
  CHECK(buf[19] == 0xBE);
  CHECK(buf[20] == 0xAD);
  CHECK(buf[21] == 0xDE);
  // timestamp 0x01020304
  CHECK(buf[22] == 0x04);
  CHECK(buf[23] == 0x03);
  CHECK(buf[24] == 0x02);
  CHECK(buf[25] == 0x01);
  // payload_size 0xCAFEBABE
  CHECK(buf[26] == 0xBE);
  CHECK(buf[27] == 0xBA);
  CHECK(buf[28] == 0xFE);
  CHECK(buf[29] == 0xCA);
  // raw_size 0x10203040
  CHECK(buf[30] == 0x40);
  CHECK(buf[31] == 0x30);
  CHECK(buf[32] == 0x20);
  CHECK(buf[33] == 0x10);
}

TEST_CASE("preview_packet: compression_kind=None still leaves magic intact") {
  // The PreviewWorker falls back to compression=None when the
  // compressor fails — the rest of the header still has to round-trip
  // correctly so the WebUI can render the raw payload as 1-bpp bytes.
  PreviewPanelHeader h = MakeHeader();
  h.compression_kind = kPreviewCompressionNone;
  std::array<uint8_t, kPreviewHeaderBytes> buf{};
  WritePreviewPanelHeader(buf.data(), h);
  CHECK(buf[0] == 'B');
  CHECK(buf[6] == kPreviewCompressionNone);
}

TEST_CASE("preview_packet: zero-init header writes magic + reserved zero") {
  PreviewPanelHeader h{};
  std::array<uint8_t, kPreviewHeaderBytes> buf{};
  std::memset(buf.data(), 0xFFu, buf.size());
  WritePreviewPanelHeader(buf.data(), h);

  CHECK(buf[0] == 'B');
  CHECK(buf[1] == 'T');
  CHECK(buf[2] == 'F');
  CHECK(buf[3] == 'B');
  CHECK(buf[4] == kPreviewVersion);
  CHECK(buf[5] == kPreviewKindPanelFrame);
  CHECK(buf[7] == kPreviewSourceBitDepth);
  CHECK(buf[9] == 0);
  // All payload-derived fields zero -> all remaining bytes zero.
  for (std::size_t i = 10; i < buf.size(); ++i) {
    CAPTURE(i);
    CHECK(buf[i] == 0);
  }
}
