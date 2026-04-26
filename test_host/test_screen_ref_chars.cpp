#include <fstream>
#include <sstream>
#include <string>

#include "doctest.h"

// Regression tests for the 2026-04-23 baseline-mismatch bug
// ===========================================================
//
// `btc_price.cpp` was passing `kDigitAndPuncRef` ("0123456789.,:") to
// `DrawTextCentered` for its digit panels, while the currency-symbol
// panel on the same screen — and every digit call site on the block-
// height and Moscow-time screens — passed `kDigitRef` ("0123456789").
//
// `Font::GetReferenceBox` folds the union of every codepoint's above-
// and below-baseline reach into the ref box it returns, and the comma
// has a 24-em descender on Antonio at upem=2048. So the two refs gave:
//
//   kDigitRef         → above=121  below=2   → y_baseline=184
//   kDigitAndPuncRef  → above=121  below=24  → y_baseline=173
//
// …which translated to an 11 px vertical offset on the physical panel:
// digits on the price screen rendered 11 px below where the `$` / `€`
// / `£` / `¥` on the same screen landed, and 11 px below where digits
// on block-height / Moscow-time screens landed. Integer-only formatters
// never emit `.` `,` `:` — the wider ref bought nothing.
//
// These tests scan the in-tree source so a future edit that
// re-introduces the mismatch (inline or via a constant) fails here
// before hitting the device.

namespace {

std::string ReadFile(const std::string& relpath) {
  const std::string path = std::string(BTCLOCK_PROJECT_ROOT) + "/" + relpath;
  std::ifstream f(path);
  REQUIRE_MESSAGE(f.is_open(), "could not open " << path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("kDigitRef is defined as integer digits only") {
  const std::string hdr = ReadFile("main/screens/common.hpp");
  // Widening this constant with punctuation or non-digit characters
  // would lower every digit screen's baseline uniformly — a different
  // kind of silent drift. Pin the literal.
  CHECK(hdr.find("kDigitRef = \"0123456789\"") != std::string::npos);
}

TEST_CASE("kDigitAndPuncRef was removed and is not re-introduced") {
  // The constant was live for ~a week before the mismatch was caught.
  // No screen actually needs a punct-inclusive ref (all current
  // formatters are integer-only), so the constant itself is a trap:
  // a contributor unfamiliar with the descender interaction may reach
  // for it. Keep it deleted.
  for (const char* name :
       {"common.hpp", "common.cpp", "block_height.cpp", "moscow_time.cpp",
        "btc_price.cpp", "fee_rate.cpp"}) {
    CAPTURE(name);
    const std::string contents = ReadFile(std::string("main/screens/") + name);
    CHECK(contents.find("kDigitAndPuncRef") == std::string::npos);
  }
}

TEST_CASE("screen renderers don't inline a ref_chars literal with punct") {
  // Catch-all for an inline `"0123456789.,:"` or similar snuck in past
  // the named-constant guard above. Add more explicit literals here as
  // we encounter them; a regex-based scan would be more thorough but
  // also more fragile to comment / string formatting changes.
  for (const char* name : {"block_height.cpp", "moscow_time.cpp",
                           "btc_price.cpp", "fee_rate.cpp"}) {
    CAPTURE(name);
    const std::string contents = ReadFile(std::string("main/screens/") + name);
    CHECK(contents.find("\"0123456789.\"") == std::string::npos);
    CHECK(contents.find("\"0123456789,\"") == std::string::npos);
    CHECK(contents.find("\"0123456789.,:\"") == std::string::npos);
    CHECK(contents.find("\"0123456789.,\"") == std::string::npos);
  }
}
