// Pure-logic helpers for validating /upload/firmware request sizes
// before committing to a partition erase. No IDF includes — host
// tests link against this header directly.
//
// Mirrors webui_upload_bounds.hpp. The rationale is the same: reject
// oversize payloads up-front with 413 rather than erase the target
// OTA partition, then fail mid-stream with the partition in an
// indeterminate state (esp-idf's rollback + otadata bookkeeping
// eventually recovers, but the device's visible state during the
// window is confusing).

#pragma once

#include <cstddef>

namespace btclock {

// Returns true iff `declared` is a plausibly-valid payload size for a
// firmware image destined for a `partition_size`-byte OTA partition.
//
// Semantics:
//   * `declared == 0` → false. A Content-Length-less POST provides no
//     way to tell when the image ends; the OTA handler rejects those.
//   * `partition_size == 0` → false. No next-OTA partition resolved
//     (e.g. running from a factory slot with no OTA partitions).
//   * `declared > partition_size` → false. Reject oversize early.
//   * Otherwise → true.
//
// Callers are still responsible for enforcing that the actual byte
// count written during streaming matches `declared`; this gate only
// checks the up-front Content-Length.
inline bool IsValidFirmwareUploadSize(std::size_t declared,
                                      std::size_t partition_size) {
  if (declared == 0) return false;
  if (partition_size == 0) return false;
  return declared <= partition_size;
}

}  // namespace btclock
