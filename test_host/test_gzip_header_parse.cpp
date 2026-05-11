// Host tests for the gzip member-header parser. The on-device
// DecompressFontGzip path uses ParseGzipMemberHeader to compute the
// deflate offset before handing bytes to miniz; this file pins the
// FLG (FNAME / FCOMMENT / FEXTRA / FHCRC) handling that the previous
// strict FLG=0 check rejected outright.

#include <cstdint>
#include <vector>

#include "doctest.h"
#include "gzip_header_parse.hpp"

using btclock::GzipMemberInfo;
using btclock::kGzipFixedHeaderLen;
using btclock::kGzipFlagFcomment;
using btclock::kGzipFlagFextra;
using btclock::kGzipFlagFhcrc;
using btclock::kGzipFlagFname;
using btclock::kGzipFlagReserved;
using btclock::kGzipTrailerLen;
using btclock::LooksGzipped;
using btclock::ParseGzipMemberHeader;

namespace {

// Build a syntactically valid gzip blob. `flg` controls which optional
// fields are present, `extra` / `fname` / `fcomment` supply their
// payloads (caller decides whether the corresponding FLG bit is set),
// `deflate` is treated opaquely (the parser doesn't validate it),
// `isize` is the trailing ISIZE field.
std::vector<uint8_t> MakeGzip(uint8_t flg, const std::vector<uint8_t>& extra,
                              const std::vector<uint8_t>& fname,
                              const std::vector<uint8_t>& fcomment,
                              bool include_fhcrc,
                              const std::vector<uint8_t>& deflate,
                              uint32_t isize) {
  std::vector<uint8_t> out;
  out.push_back(0x1f);
  out.push_back(0x8b);
  out.push_back(0x08);  // CM = deflate
  out.push_back(flg);
  for (int i = 0; i < 4; ++i) out.push_back(0);  // mtime
  out.push_back(0);                              // XFL
  out.push_back(0xFF);                           // OS = unknown

  if ((flg & kGzipFlagFextra) != 0) {
    const uint16_t xlen = static_cast<uint16_t>(extra.size());
    out.push_back(static_cast<uint8_t>(xlen & 0xFF));
    out.push_back(static_cast<uint8_t>((xlen >> 8) & 0xFF));
    out.insert(out.end(), extra.begin(), extra.end());
  }
  if ((flg & kGzipFlagFname) != 0) {
    out.insert(out.end(), fname.begin(), fname.end());
    out.push_back(0);
  }
  if ((flg & kGzipFlagFcomment) != 0) {
    out.insert(out.end(), fcomment.begin(), fcomment.end());
    out.push_back(0);
  }
  if (include_fhcrc) {
    out.push_back(0xAA);
    out.push_back(0xBB);
  }

  out.insert(out.end(), deflate.begin(), deflate.end());

  for (int i = 0; i < 4; ++i) out.push_back(0);  // CRC32 (unused by parser)
  out.push_back(static_cast<uint8_t>(isize & 0xFF));
  out.push_back(static_cast<uint8_t>((isize >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((isize >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((isize >> 24) & 0xFF));
  return out;
}

}  // namespace

TEST_CASE("gzip: LooksGzipped rejects non-gzip and short blobs") {
  CHECK_FALSE(LooksGzipped(nullptr, 0));
  const uint8_t too_short[] = {0x1f, 0x8b, 0x08, 0x00};
  CHECK_FALSE(LooksGzipped(too_short, sizeof(too_short)));
  // Right size but wrong magic.
  std::vector<uint8_t> wrong_magic(20, 0);
  CHECK_FALSE(LooksGzipped(wrong_magic.data(), wrong_magic.size()));
  // Right size but wrong CM (not deflate).
  std::vector<uint8_t> wrong_cm =
      MakeGzip(0, {}, {}, {}, false, {0xAB, 0xCD}, 7);
  wrong_cm[2] = 0x07;
  CHECK_FALSE(LooksGzipped(wrong_cm.data(), wrong_cm.size()));
}

TEST_CASE("gzip: FLG=0 hands back deflate offset == 10 and ISIZE intact") {
  const std::vector<uint8_t> deflate{0x11, 0x22, 0x33, 0x44, 0x55};
  auto blob = MakeGzip(0, {}, {}, {}, false, deflate, 0xCAFEBABEu);
  GzipMemberInfo info{};
  REQUIRE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  CHECK(info.deflate_offset == kGzipFixedHeaderLen);
  CHECK(info.deflate_size == deflate.size());
  CHECK(info.isize == 0xCAFEBABEu);
}

TEST_CASE("gzip: FNAME bit advances past NUL-terminated string") {
  const std::vector<uint8_t> deflate{0x10, 0x20};
  const std::vector<uint8_t> fname{'h', 'i'};
  auto blob = MakeGzip(kGzipFlagFname, {}, fname, {}, false, deflate, 42);
  GzipMemberInfo info{};
  REQUIRE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  // Header(10) + "hi"(2) + NUL(1) = 13
  CHECK(info.deflate_offset == 13);
  CHECK(info.deflate_size == deflate.size());
  CHECK(info.isize == 42);
}

TEST_CASE("gzip: FCOMMENT bit advances past NUL-terminated string") {
  const std::vector<uint8_t> deflate{0x10};
  const std::vector<uint8_t> comment{'a', 'b', 'c'};
  auto blob = MakeGzip(kGzipFlagFcomment, {}, {}, comment, false, deflate, 1);
  GzipMemberInfo info{};
  REQUIRE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  CHECK(info.deflate_offset == kGzipFixedHeaderLen + comment.size() + 1);
  CHECK(info.deflate_size == deflate.size());
}

TEST_CASE("gzip: FEXTRA bit honours XLEN little-endian") {
  const std::vector<uint8_t> deflate{0xAA};
  const std::vector<uint8_t> extra{0x01, 0x02, 0x03, 0x04};  // XLEN=4
  auto blob = MakeGzip(kGzipFlagFextra, extra, {}, {}, false, deflate, 99);
  GzipMemberInfo info{};
  REQUIRE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  // Header(10) + XLEN field(2) + extra(4) = 16
  CHECK(info.deflate_offset == 16);
  CHECK(info.deflate_size == deflate.size());
}

TEST_CASE("gzip: FHCRC bit consumes 2 trailing CRC bytes") {
  const std::vector<uint8_t> deflate{0xAB, 0xCD};
  auto blob = MakeGzip(kGzipFlagFhcrc, {}, {}, {}, true, deflate, 8);
  GzipMemberInfo info{};
  REQUIRE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  CHECK(info.deflate_offset == kGzipFixedHeaderLen + 2);
  CHECK(info.deflate_size == deflate.size());
}

TEST_CASE("gzip: combined FNAME + FCOMMENT + FHCRC parse correctly") {
  const std::vector<uint8_t> deflate{0x55};
  const std::vector<uint8_t> fname{'F', 'O', 'O'};
  const std::vector<uint8_t> fcomment{'B', 'A', 'R', 'B', 'A', 'Z'};
  auto blob = MakeGzip(kGzipFlagFname | kGzipFlagFcomment | kGzipFlagFhcrc, {},
                       fname, fcomment, true, deflate, 100);
  GzipMemberInfo info{};
  REQUIRE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  // Header(10) + name(3+1) + comment(6+1) + FHCRC(2) = 23
  CHECK(info.deflate_offset == 23);
  CHECK(info.deflate_size == deflate.size());
}

TEST_CASE("gzip: reserved FLG bits are rejected") {
  // Per RFC 1952 the high three FLG bits must be zero. A blob with any
  // of them set is malformed; refuse rather than guess what the
  // (future) extension means.
  for (uint8_t bit : {uint8_t{0x20}, uint8_t{0x40}, uint8_t{0x80}}) {
    auto blob = MakeGzip(bit, {}, {}, {}, false, {0xAA}, 1);
    GzipMemberInfo info{};
    CAPTURE(static_cast<unsigned>(bit));
    CHECK_FALSE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
  }
  CHECK(kGzipFlagReserved == 0xE0);  // sanity
}

TEST_CASE("gzip: truncated FNAME (no NUL) is rejected") {
  // Build a blob with FNAME set but no NUL between the header and the
  // 8-byte trailer. Parser should reject rather than read into the
  // trailer + corrupt downstream miniz.
  const std::vector<uint8_t> deflate{};
  const std::vector<uint8_t> fname(64, 'x');  // 64 bytes, no NUL emitted
  // MakeGzip would append the NUL — emit a hand-rolled blob instead.
  std::vector<uint8_t> blob;
  blob.push_back(0x1f);
  blob.push_back(0x8b);
  blob.push_back(0x08);
  blob.push_back(kGzipFlagFname);
  for (int i = 0; i < 4; ++i) blob.push_back(0);
  blob.push_back(0);
  blob.push_back(0xFF);
  blob.insert(blob.end(), fname.begin(), fname.end());
  // No NUL terminator.
  for (int i = 0; i < 4; ++i) blob.push_back(0);  // CRC32
  for (int i = 0; i < 4; ++i) blob.push_back(0);  // ISIZE
  GzipMemberInfo info{};
  CHECK_FALSE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
}

TEST_CASE("gzip: FEXTRA XLEN larger than blob is rejected") {
  std::vector<uint8_t> blob;
  blob.push_back(0x1f);
  blob.push_back(0x8b);
  blob.push_back(0x08);
  blob.push_back(kGzipFlagFextra);
  for (int i = 0; i < 4; ++i) blob.push_back(0);
  blob.push_back(0);
  blob.push_back(0xFF);
  // XLEN = 0xFFFF claims 65535 bytes of extra field but blob is tiny.
  blob.push_back(0xFF);
  blob.push_back(0xFF);
  for (int i = 0; i < 4; ++i) blob.push_back(0);
  for (int i = 0; i < 4; ++i) blob.push_back(0);
  GzipMemberInfo info{};
  CHECK_FALSE(ParseGzipMemberHeader(blob.data(), blob.size(), &info));
}

TEST_CASE("gzip: nullptr / size-0 inputs are rejected without UB") {
  GzipMemberInfo info{};
  CHECK_FALSE(ParseGzipMemberHeader(nullptr, 0, &info));
  CHECK_FALSE(ParseGzipMemberHeader(nullptr, 64, &info));
  const uint8_t bytes[] = {0x1f, 0x8b, 0x08, 0x00};
  CHECK_FALSE(ParseGzipMemberHeader(bytes, sizeof(bytes), &info));
  CHECK_FALSE(ParseGzipMemberHeader(bytes, sizeof(bytes), nullptr));
}
