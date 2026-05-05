#include "proxy_transport/proxy_bypass.hpp"

#include <algorithm>
#include <cctype>

namespace btclock {
namespace proxy {

namespace {

bool ICmpEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool IEndsWith(std::string_view s, std::string_view suffix) {
  if (suffix.size() > s.size()) return false;
  return ICmpEqual(s.substr(s.size() - suffix.size()), suffix);
}

bool IStartsWith(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  return ICmpEqual(s.substr(0, prefix.size()), prefix);
}

}  // namespace

bool MatchesGlob(std::string_view pattern, std::string_view host) {
  if (pattern.empty()) return false;
  // Whole-string glob.
  if (pattern == "*") return true;
  // Leading-star: "*.local" matches "foo.local" but not bare "local"
  // (the dot is part of the literal tail).
  if (pattern.front() == '*') {
    return IEndsWith(host, pattern.substr(1));
  }
  // Trailing-star: "192.168.*" matches "192.168.1.4".
  if (pattern.back() == '*') {
    return IStartsWith(host, pattern.substr(0, pattern.size() - 1));
  }
  return ICmpEqual(pattern, host);
}

bool ShouldBypass(const Config& cfg, std::string_view dest_host) {
  if (!IsEnabled(cfg)) return true;
  for (const auto& pat : cfg.bypass) {
    if (MatchesGlob(pat, dest_host)) return true;
  }
  return false;
}

void SplitBypassList(std::string_view csv, std::vector<std::string>* out) {
  out->clear();
  size_t i = 0;
  while (i < csv.size()) {
    size_t j = csv.find(',', i);
    if (j == std::string_view::npos) j = csv.size();
    auto entry = csv.substr(i, j - i);
    // Trim ASCII whitespace.
    while (!entry.empty() &&
           std::isspace(static_cast<unsigned char>(entry.front()))) {
      entry.remove_prefix(1);
    }
    while (!entry.empty() &&
           std::isspace(static_cast<unsigned char>(entry.back()))) {
      entry.remove_suffix(1);
    }
    if (!entry.empty()) out->emplace_back(entry);
    i = j + 1;
  }
}

}  // namespace proxy
}  // namespace btclock
