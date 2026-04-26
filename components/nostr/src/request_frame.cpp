// NIP-01 REQ / CLOSE frame builders.
//
// Split from subscription_manager.cpp so the host-test build can link
// these pure-logic helpers without pulling in the RelayClient (which
// depends on esp_websocket_client and therefore on ESP-IDF).

#include <cstdio>
#include <string>

#include "nostr/subscription_manager.hpp"

namespace btclock {
namespace nostr {
namespace {

// Append a JSON string literal (properly quoted) to `out`. We escape `"`
// and `\` only — all NIP-01 filter values we emit are hex / ASCII and
// never contain control characters.
void AppendString(std::string& out, const std::string& s) {
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
}

void AppendStringArray(std::string& out, const std::vector<std::string>& v) {
  out.push_back('[');
  for (size_t i = 0; i < v.size(); ++i) {
    if (i > 0) out.push_back(',');
    AppendString(out, v[i]);
  }
  out.push_back(']');
}

}  // namespace

std::string BuildReqJson(const std::string& sub_id, const Filter& f) {
  std::string out;
  out.reserve(128);
  out.append("[\"REQ\",");
  AppendString(out, sub_id);
  out.push_back(',');
  out.push_back('{');

  bool first = true;
  auto sep = [&] {
    if (first)
      first = false;
    else
      out.push_back(',');
  };

  if (!f.kinds.empty()) {
    sep();
    out.append("\"kinds\":[");
    for (size_t i = 0; i < f.kinds.size(); ++i) {
      if (i > 0) out.push_back(',');
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(f.kinds[i]));
      out.append(buf);
    }
    out.push_back(']');
  }
  if (!f.authors.empty()) {
    sep();
    out.append("\"authors\":");
    AppendStringArray(out, f.authors);
  }
  if (!f.d_tags.empty()) {
    sep();
    out.append("\"#d\":");
    AppendStringArray(out, f.d_tags);
  }
  if (!f.p_tags.empty()) {
    sep();
    out.append("\"#p\":");
    AppendStringArray(out, f.p_tags);
  }
  if (f.since > 0) {
    sep();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\"since\":%llu",
                  static_cast<unsigned long long>(f.since));
    out.append(buf);
  }
  if (f.limit > 0) {
    sep();
    char buf[24];
    std::snprintf(buf, sizeof(buf), "\"limit\":%d", f.limit);
    out.append(buf);
  }

  out.push_back('}');
  out.push_back(']');
  return out;
}

std::string BuildCloseJson(const std::string& sub_id) {
  std::string out;
  out.append("[\"CLOSE\",");
  AppendString(out, sub_id);
  out.push_back(']');
  return out;
}

}  // namespace nostr
}  // namespace btclock
