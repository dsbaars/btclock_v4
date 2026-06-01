#include "bitaxe/bitaxe_parser.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"

namespace btclock {
namespace bitaxe {

std::string FormatBestDiff(double raw) {
  if (!(raw > 0.0)) return "0";
  // K/M/G/T/P mirror the old firmware's parseBitaxeBestDiff suffix
  // ladder. We stop at P (peta) — bitcoin's per-share difficulty rarely
  // climbs past that on a home miner, and higher suffixes would risk
  // EPD overflow on the 7-panel board.
  constexpr struct {
    double threshold;
    char suffix;
  } kLadder[] = {
      {1e15, 'P'}, {1e12, 'T'}, {1e9, 'G'}, {1e6, 'M'}, {1e3, 'K'},
  };
  for (const auto& s : kLadder) {
    if (raw >= s.threshold) {
      const double v = raw / s.threshold;
      char buf[24];
      // Old firmware's parseBitaxeBestDiff kept one decimal place
      // regardless of magnitude and then trimmed ".0". We follow the
      // same rule so a 15.6M share reads as "15.6M" rather than "16M"
      // — matters when the user is watching their best-share climb.
      std::snprintf(buf, sizeof(buf), "%.1f%c", v, s.suffix);
      // Trim a trailing ".0" so "1.0M" reads as "1M" — keeps the cell
      // count tight on narrow boards.
      std::string out = buf;
      const auto dot = out.find('.');
      if (dot != std::string::npos && dot + 2 < out.size() &&
          out[dot + 1] == '0' && (out[dot + 2] == s.suffix)) {
        out.erase(dot, 2);
      }
      return out;
    }
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu",
                static_cast<unsigned long long>(std::llround(raw)));
  return buf;
}

namespace {

// True when `s` is a bare decimal number (digits with at most one '.',
// no suffix letter / sign / exponent). AxeOS normally sends bestDiff
// pre-suffixed ("1.5T"); some forks emit a raw integer string. A raw
// number wider than the digit area would get leading-truncated by the
// renderer's RightJustifyCodepoints, so we re-suffix bare numbers below.
bool IsBareNumber(const std::string& s) {
  if (s.empty()) return false;
  bool seen_digit = false;
  bool seen_dot = false;
  for (const char c : s) {
    if (c >= '0' && c <= '9') {
      seen_digit = true;
    } else if (c == '.') {
      if (seen_dot) return false;
      seen_dot = true;
    } else {
      return false;
    }
  }
  return seen_digit;
}

}  // namespace

bool Parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return false;
  }

  cJSON* hr = cJSON_GetObjectItemCaseSensitive(root, "hashRate");
  if (cJSON_IsNumber(hr)) {
    out.hashrate_ghs = hr->valuedouble;
  }

  cJSON* bd = cJSON_GetObjectItemCaseSensitive(root, "bestDiff");
  if (cJSON_IsString(bd) && bd->valuestring && *bd->valuestring) {
    const std::string s(bd->valuestring);
    // Re-suffix a bare-number string so a long raw value can't get
    // leading-truncated by the fixed-width digit area; pre-suffixed
    // strings ("1.5T") pass through unchanged.
    out.best_diff =
        IsBareNumber(s) ? FormatBestDiff(std::strtod(s.c_str(), nullptr)) : s;
  } else if (cJSON_IsNumber(bd)) {
    out.best_diff = FormatBestDiff(bd->valuedouble);
  }

  cJSON* sa = cJSON_GetObjectItemCaseSensitive(root, "sharesAccepted");
  if (cJSON_IsNumber(sa)) {
    out.shares_accepted = static_cast<int32_t>(std::llround(sa->valuedouble));
  }

  // Accept `temp` (Gamma) or `temperature` (older forks). First one wins.
  cJSON* t = cJSON_GetObjectItemCaseSensitive(root, "temp");
  if (!cJSON_IsNumber(t)) {
    t = cJSON_GetObjectItemCaseSensitive(root, "temperature");
  }
  if (cJSON_IsNumber(t)) {
    out.temperature_c = t->valuedouble;
  }

  cJSON_Delete(root);
  return true;
}

}  // namespace bitaxe
}  // namespace btclock
