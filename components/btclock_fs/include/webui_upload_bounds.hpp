// Pure-logic helpers for validating /upload/webui request sizes before
// committing to a partition erase. No IDF includes — host tests link
// against this header directly.

#pragma once

#include <cstddef>

namespace btclock {

// Returns true iff `declared` is a plausibly-valid payload size for a
// LittleFS image destined for a `partition_size`-byte partition.
//
// Semantics:
//   * `declared == 0` → false. The client must send a Content-Length
//     header with a non-zero value; empty uploads are never valid.
//   * `partition_size == 0` → false. The storage partition isn't
//     findable on this board, so nothing fits.
//   * `declared > partition_size` → false. Reject oversize early so we
//     don't erase the partition and then fail mid-stream.
//   * Otherwise → true.
//
// Callers are still responsible for enforcing the actual byte count
// written during streaming matches `declared`; this check only gates
// the up-front Content-Length.
inline bool IsValidWebuiUploadSize(std::size_t declared,
                                   std::size_t partition_size) {
  if (declared == 0) return false;
  if (partition_size == 0) return false;
  return declared <= partition_size;
}

}  // namespace btclock
