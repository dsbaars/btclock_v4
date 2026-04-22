// Pure-logic networking string helpers. Zero ESP-IDF dependencies so host
// tests under test_host/ can compile this file unchanged. Both the firmware
// (main.cpp, provisioning_ui.cpp) and the host tests include this header.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace btclock {

// "BTClock-XXXX" — last two MAC bytes (uppercase hex). Matches the SSID
// format used by the production firmware's WiFiManager AP.
inline std::string FormatApSsid(const uint8_t mac[6]) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "BTClock-%02X%02X", mac[4], mac[5]);
  return buf;
}

// Uppercase + digits + lowercase minus the visually ambiguous glyphs
// (0/O, 1/I/l). 53 characters total. Used for the generated AP password
// so a user reading it off the e-paper won't misread a character.
inline constexpr char kApPasswordCharset[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZ23456789abcdefghjkmnpqrstuvwxyz";

// Uniform sample of `len` characters from kApPasswordCharset. `rng()` must
// return a uniformly-distributed 32-bit value on each call.
template <typename Rng>
std::string GenerateApPassword(Rng rng, int len = 8) {
  constexpr int kSet = sizeof(kApPasswordCharset) - 1;
  std::string out;
  out.reserve(static_cast<size_t>(len));
  for (int i = 0; i < len; ++i) {
    out.push_back(kApPasswordCharset[rng() % kSet]);
  }
  return out;
}

// "WIFI:T:WPA;S:<ssid>;P:<password>;;" — scannable as a Wi-Fi join QR.
// Does not escape semicolons, commas, or colons in ssid/password; the
// generated SSID and kApPasswordCharset exclude those, so it's safe.
inline std::string FormatWifiQr(std::string_view ssid,
                                 std::string_view password) {
  std::string out;
  out.reserve(32 + ssid.size() + password.size());
  out.append("WIFI:T:WPA;S:");
  out.append(ssid);
  out.append(";P:");
  out.append(password);
  out.append(";;");
  return out;
}

}  // namespace btclock
