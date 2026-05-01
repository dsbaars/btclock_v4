// libFuzzer harness for the NIP-01 envelope parser at
// components/nostr/src/parser.cpp. The parser is a hand-rolled JSON
// walker (chosen to avoid pulling cJSON onto the firmware hot path),
// so it's exactly the kind of code where off-by-one and edge-case
// escape handling tend to hide. The fuzzer drives random byte strings
// through both the network-facing `ParseEnvelope` and the bare-event
// `ParseEventObject` entry points so any divergence between them
// surfaces too.
//
// Build with the ad-hoc target wired in test_host/CMakeLists.txt:
//
//   cmake -S test_host -B build-fuzz -DBTCLOCK_FUZZ=ON \
//         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz
//   ./build-fuzz/nostr_parser_fuzzer test_host/fuzz_corpus/nostr_parser/ \
//       -max_total_time=300
//
// libFuzzer's `LLVMFuzzerTestOneInput` is the standard entry shape;
// returning 0 means "no error, drive the next input" — the only way to
// signal a finding is to crash (ASan / UBSan / explicit abort), which
// the fuzzer captures and minimises into a reproducer.

#include <cstddef>
#include <cstdint>
#include <string>

#include "nostr/event.hpp"
#include "nostr/parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  // The parser takes std::string by value so the harness has to materialise
  // one. Keeping the input as raw bytes (not a NUL-terminated C string)
  // lets the fuzzer drive embedded NULs which the JSON walker has to
  // handle as just-another-byte rather than an early termination.
  const std::string frame(reinterpret_cast<const char*>(data), size);

  // Network-facing entry point: relay → client envelopes.
  btclock::nostr::Envelope env;
  (void)btclock::nostr::ParseEnvelope(frame, env);

  // Bare event-object entry point: same parser body reached via a
  // different boundary. Fuzzing both surfaces exercises the same
  // walker but with different framing assumptions, which has caught
  // entry-point-only bugs in practice.
  btclock::nostr::Event ev;
  (void)btclock::nostr::ParseEventObject(frame, ev);

  return 0;
}
