#include "doctest.h"

#include "ota_upload_bounds.hpp"

namespace {

// OTA app slot sizes from partitions_*.csv:
//   REV A (4 MB):  0x1B0000 = 1 769 472 B
//   REV B (8 MB):  0x370000 = 3 604 480 B
//   V8 (16 MB):    0x600000 = 6 291 456 B
// Exact numbers don't matter for these tests as long as they're
// non-zero and cover the size ordering; use the REV B slot as the
// middle reference.
constexpr std::size_t kRevAOtaSlot = 0x1B0000;
constexpr std::size_t kRevBOtaSlot = 0x370000;
constexpr std::size_t kV8OtaSlot = 0x600000;

}  // namespace

TEST_CASE("IsValidFirmwareUploadSize rejects empty payloads") {
  CHECK_FALSE(btclock::IsValidFirmwareUploadSize(0, kRevBOtaSlot));
}

TEST_CASE("IsValidFirmwareUploadSize rejects missing partition") {
  CHECK_FALSE(btclock::IsValidFirmwareUploadSize(1024, 0));
  CHECK_FALSE(btclock::IsValidFirmwareUploadSize(0, 0));
}

TEST_CASE("IsValidFirmwareUploadSize accepts in-bounds payloads") {
  CHECK(btclock::IsValidFirmwareUploadSize(1, kRevAOtaSlot));
  CHECK(btclock::IsValidFirmwareUploadSize(1500u * 1024, kRevAOtaSlot));
  // Equality must be accepted — esp_ota_write fills the whole slot.
  CHECK(btclock::IsValidFirmwareUploadSize(kRevAOtaSlot, kRevAOtaSlot));
  CHECK(btclock::IsValidFirmwareUploadSize(kRevBOtaSlot, kRevBOtaSlot));
  CHECK(btclock::IsValidFirmwareUploadSize(kV8OtaSlot, kV8OtaSlot));
}

TEST_CASE("IsValidFirmwareUploadSize rejects oversize payloads") {
  CHECK_FALSE(
      btclock::IsValidFirmwareUploadSize(kRevAOtaSlot + 1, kRevAOtaSlot));
  CHECK_FALSE(
      btclock::IsValidFirmwareUploadSize(kRevBOtaSlot + 1, kRevBOtaSlot));
  CHECK_FALSE(
      btclock::IsValidFirmwareUploadSize(kV8OtaSlot + 1, kV8OtaSlot));
  // A firmware built for V8 flashed to a Rev A slot: the 413 gate
  // catches this before esp_ota_begin erases half the partition.
  CHECK_FALSE(btclock::IsValidFirmwareUploadSize(kV8OtaSlot, kRevAOtaSlot));
}
