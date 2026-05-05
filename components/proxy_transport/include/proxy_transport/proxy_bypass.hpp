#pragma once

#include <string_view>

#include "proxy_transport/proxy_config.hpp"

namespace btclock {
namespace proxy {

// Case-insensitive glob match. Supported patterns: literal ("foo"),
// leading-star ("*.local"), trailing-star ("192.168.*").
bool MatchesGlob(std::string_view pattern, std::string_view host);

// True when `dest_host` is in the config's bypass list, OR when the
// config is disabled (kind == kNone). Callers should treat a `true`
// return as "do not chain this proxy onto the connection".
bool ShouldBypass(const Config& cfg, std::string_view dest_host);

// Splits a settings-format comma-separated string into trimmed entries.
// Empty entries are dropped. Used to round-trip the proxyBypass NVS field.
void SplitBypassList(std::string_view csv, std::vector<std::string>* out);

}  // namespace proxy
}  // namespace btclock
