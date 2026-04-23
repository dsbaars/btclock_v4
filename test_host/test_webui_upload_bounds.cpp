#include "doctest.h"

#include "webui_upload_bounds.hpp"

namespace {

// Partition sizes from partitions_*.csv:
//   REV A (4 MB): 0x67000  = 421 888 B
//   REV B (8 MB): 0xCD000  = 839 680 B
//   V8 (16 MB):   0x200000 = 2 097 152 B
constexpr std::size_t kRevAPartition = 0x67000;
constexpr std::size_t kRevBPartition = 0xCD000;
constexpr std::size_t kV8Partition = 0x200000;

}  // namespace

TEST_CASE("IsValidWebuiUploadSize rejects empty payloads") {
  CHECK_FALSE(btclock::IsValidWebuiUploadSize(0, kRevBPartition));
}

TEST_CASE("IsValidWebuiUploadSize rejects missing partition") {
  CHECK_FALSE(btclock::IsValidWebuiUploadSize(1024, 0));
  CHECK_FALSE(btclock::IsValidWebuiUploadSize(0, 0));
}

TEST_CASE("IsValidWebuiUploadSize accepts in-bounds payloads") {
  CHECK(btclock::IsValidWebuiUploadSize(1, kRevAPartition));
  CHECK(btclock::IsValidWebuiUploadSize(1024, kRevAPartition));
  // Equality must be accepted — the payload exactly fills the
  // partition, and `esp_partition_write` copes with that.
  CHECK(btclock::IsValidWebuiUploadSize(kRevAPartition, kRevAPartition));
  CHECK(btclock::IsValidWebuiUploadSize(kRevBPartition, kRevBPartition));
  CHECK(btclock::IsValidWebuiUploadSize(kV8Partition, kV8Partition));
}

TEST_CASE("IsValidWebuiUploadSize rejects oversize payloads") {
  CHECK_FALSE(btclock::IsValidWebuiUploadSize(kRevAPartition + 1,
                                              kRevAPartition));
  CHECK_FALSE(btclock::IsValidWebuiUploadSize(kRevBPartition + 1,
                                              kRevBPartition));
  CHECK_FALSE(btclock::IsValidWebuiUploadSize(kV8Partition + 1,
                                              kV8Partition));
  // A webui built for Rev B (820 KiB) would overflow Rev A (412 KiB)
  // if flashed to the wrong variant — this is the exact case we want
  // the 413 gate to catch before erasing the partition.
  CHECK_FALSE(
      btclock::IsValidWebuiUploadSize(kRevBPartition, kRevAPartition));
}
