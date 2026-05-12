#include "mdi_custom_cell.hpp"

#include <cstring>

#include "mdi_codepoints.hpp"

namespace btclock {
namespace {

std::string_view TrimAsciiWs(std::string_view s) {
  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.front());
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    s.remove_prefix(1);
  }
  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.back());
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    s.remove_suffix(1);
  }
  return s;
}

bool StartsWithAsciiCi(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    unsigned char a = static_cast<unsigned char>(s[i]);
    unsigned char b = static_cast<unsigned char>(prefix[i]);
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<unsigned char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<unsigned char>(b - 'A' + 'a');
    }
    if (a != b) return false;
  }
  return true;
}

bool NormalizeIconToken(std::string_view tok, char* out, std::size_t cap) {
  if (tok.size() + 1 > cap) return false;
  std::size_t j = 0;
  for (std::size_t i = 0; i < tok.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(tok[i]);
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<unsigned char>(c - 'A' + 'a');
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out[j++] = static_cast<char>(c);
    } else {
      return false;
    }
  }
  out[j] = '\0';
  return j > 0;
}

struct NamedMdi {
  const char* name;
  std::uint32_t cp;
};

// Keep alphabetically sorted by name — mirrors tools/fonts/regen_mdi.sh.
constexpr NamedMdi kSubset[] = {
    {"alarm", mdi::kIconAlarm},
    {"arrow-down-bold", mdi::kIconArrowDownBold},
    {"arrow-up-bold", mdi::kIconArrowUpBold},
    {"bell", mdi::kIconBell},
    {"bitcoin", mdi::kIconBitcoin},
    {"clock-outline", mdi::kIconClockOutline},
    {"currency-btc", mdi::kIconCurrencyBtc},
    {"lightning-bolt", mdi::kIconLightningBolt},
    {"pickaxe", mdi::kIconPickaxe},
    {"rocket-launch", mdi::kIconRocketLaunch},
    {"wifi", mdi::kIconWifi},
    {"wifi-alert", mdi::kIconWifiAlert},
    {"wifi-off", mdi::kIconWifiOff},
};

std::uint32_t LookupIcon(const char* normalized_name) {
  for (const NamedMdi& e : kSubset) {
    if (std::strcmp(e.name, normalized_name) == 0) return e.cp;
  }
  return 0;
}

}  // namespace

bool ParseCustomCellMdi(std::string_view cell, std::uint32_t* out_codepoint) {
  const std::string_view trimmed = TrimAsciiWs(cell);
  static constexpr std::string_view kPrefix = "mdi:";
  if (!StartsWithAsciiCi(trimmed, kPrefix)) return false;

  std::string_view rest = trimmed.substr(kPrefix.size());
  rest = TrimAsciiWs(rest);
  if (rest.find('/') != std::string_view::npos) return false;

  if (rest.empty()) {
    *out_codepoint = 0;
    return true;
  }

  char buf[48];
  if (!NormalizeIconToken(rest, buf, sizeof(buf))) return false;

  *out_codepoint = LookupIcon(buf);
  return true;
}

}  // namespace btclock
