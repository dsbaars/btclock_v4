// Pure-logic helpers for mining-pool hashrate parsing.
//
// Several pool APIs return hashrates in the form "123.45P" / "98.7T" —
// a decimal magnitude plus a single-character SI-like unit. The snapshot
// stores the hashrate as a plain decimal integer (no unit), so the
// pool sources need to expand the unit into the exponent. Factored into
// this header so the host-only parser tests can depend on it without
// pulling in the ESP-IDF HTTPS client.
//
// Unit table mirrors the old firmware's lib/btclock/utils.cpp exactly.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace btclock {
namespace mining_pools {

// Returns the decimal exponent for a hashrate unit character:
//   'Z' -> 21, 'E' -> 18, 'P' -> 15, 'T' -> 12, 'G' -> 9,
//   'M' -> 6,  'K' -> 3
// Anything else (including '0') returns 0. Matches old firmware.
inline int hashrate_multiplier(char unit) {
  switch (unit) {
    case 'Z': return 21;
    case 'E': return 18;
    case 'P': return 15;
    case 'T': return 12;
    case 'G': return 9;
    case 'M': return 6;
    case 'K': return 3;
    default:  return 0;
  }
}

// Expand a "<number><unit>" hashrate string (e.g. "123.45P") into a
// plain integer-valued decimal string ("123450000000000000"). Returns
// nullopt if the input is empty, is "0", has no parseable leading
// number, or uses an unknown unit.
inline std::optional<std::string> normalise_hashrate(const std::string& s) {
  if (s.empty() || s == "0") return std::nullopt;
  const char unit = s.back();
  const std::string value_part = s.substr(0, s.size() - 1);
  if (value_part.empty()) return std::nullopt;

  char* endp = nullptr;
  const double parsed = std::strtod(value_part.c_str(), &endp);
  if (endp == value_part.c_str()) return std::nullopt;

  const int exp = hashrate_multiplier(unit);
  if (exp == 0) return std::nullopt;  // unknown unit

  const double hashrate = parsed * std::pow(10.0, exp);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.0f", hashrate);
  return std::string(buf);
}

}  // namespace mining_pools
}  // namespace btclock
