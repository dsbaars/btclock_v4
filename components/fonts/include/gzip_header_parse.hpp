// Pure parsing for gzip member headers (RFC 1952). Header-only so the
// host-tests in test_host/ can exercise the FLG bit handling without
// pulling in miniz or ESP_LOG.
//
// Scope: identify the deflate payload offset and length inside an
// in-memory gzip blob and report ISIZE (uncompressed length, low 32
// bits). The actual deflate decompression lives in
// components/fonts/font.cpp::DecompressFontGzip and runs through
// miniz on-device. The split exists because earlier the parser
// rejected every blob with FLG != 0 — fine while the build pipeline
// always produced FLG=0 with `gzip -n -9`, but a brittle contract
// because some platform `gzip` builds set FNAME (0x08) anyway. The
// parser now skips FEXTRA / FNAME / FCOMMENT / FHCRC the way RFC 1952
// requires; the failure mode of the old strict check (silently
// blank screens at boot) is now a parse rather than a refusal.

#pragma once

#include <cstddef>
#include <cstdint>

namespace btclock {

inline constexpr std::size_t kGzipFixedHeaderLen = 10;
inline constexpr std::size_t kGzipTrailerLen = 8;

// FLG bits per RFC 1952 §2.3.1.
inline constexpr uint8_t kGzipFlagFtext = 0x01;     // ignored; informational
inline constexpr uint8_t kGzipFlagFhcrc = 0x02;     // 2-byte header CRC16
inline constexpr uint8_t kGzipFlagFextra = 0x04;    // XLEN-byte extra field
inline constexpr uint8_t kGzipFlagFname = 0x08;     // NUL-terminated string
inline constexpr uint8_t kGzipFlagFcomment = 0x10;  // NUL-terminated string
inline constexpr uint8_t kGzipFlagReserved = 0xE0;  // bits 5..7 must be zero

struct GzipMemberInfo {
  // Offset of the first deflate-stream byte from the start of the blob.
  std::size_t deflate_offset;
  // Length of the deflate stream (bytes between the variable-length
  // header and the 8-byte trailer).
  std::size_t deflate_size;
  // Uncompressed size mod 2^32 from the gzip trailer (ISIZE).
  std::uint32_t isize;
};

inline bool LooksGzipped(const uint8_t* data, std::size_t size) {
  return data != nullptr && size >= kGzipFixedHeaderLen + kGzipTrailerLen &&
         data[0] == 0x1f && data[1] == 0x8b && data[2] == 0x08;
}

// Parse a gzip member's header. Returns true on success and fills *out;
// returns false on any malformed / truncated header (including
// reserved-bit drift). Does not validate the deflate payload itself.
inline bool ParseGzipMemberHeader(const uint8_t* gz, std::size_t gz_size,
                                  GzipMemberInfo* out) {
  if (out == nullptr) return false;
  if (!LooksGzipped(gz, gz_size)) return false;

  const uint8_t flg = gz[3];
  if ((flg & kGzipFlagReserved) != 0) return false;

  std::size_t off = kGzipFixedHeaderLen;
  // Each optional field below must fit fully before the 8-byte
  // trailer; if any of them runs over, the blob is truncated and
  // returning false is the right answer.
  const std::size_t end_of_payload =
      gz_size >= kGzipTrailerLen ? gz_size - kGzipTrailerLen : 0;

  if ((flg & kGzipFlagFextra) != 0) {
    if (off + 2 > end_of_payload) return false;
    const std::size_t xlen = static_cast<std::size_t>(gz[off]) |
                             (static_cast<std::size_t>(gz[off + 1]) << 8);
    off += 2;
    if (off + xlen > end_of_payload) return false;
    off += xlen;
  }
  if ((flg & kGzipFlagFname) != 0) {
    while (off < end_of_payload && gz[off] != 0) ++off;
    if (off >= end_of_payload) return false;  // missing NUL
    ++off;                                    // skip NUL
  }
  if ((flg & kGzipFlagFcomment) != 0) {
    while (off < end_of_payload && gz[off] != 0) ++off;
    if (off >= end_of_payload) return false;
    ++off;
  }
  if ((flg & kGzipFlagFhcrc) != 0) {
    if (off + 2 > end_of_payload) return false;
    off += 2;
  }
  if (off > end_of_payload) return false;

  out->deflate_offset = off;
  out->deflate_size = end_of_payload - off;
  out->isize = static_cast<std::uint32_t>(gz[gz_size - 4]) |
               (static_cast<std::uint32_t>(gz[gz_size - 3]) << 8) |
               (static_cast<std::uint32_t>(gz[gz_size - 2]) << 16) |
               (static_cast<std::uint32_t>(gz[gz_size - 1]) << 24);
  return true;
}

}  // namespace btclock
