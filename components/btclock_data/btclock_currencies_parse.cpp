#include "btclock_currencies_parse.hpp"

#include <cstring>
#include <set>

#include "cJSON.h"

namespace btclock {

namespace {

bool IsValidIso(const char* s, std::size_t len) {
  if (len != 3) return false;
  for (std::size_t i = 0; i < 3; ++i) {
    const char c = s[i];
    if (c < 'A' || c > 'Z') return false;
  }
  return true;
}

}  // namespace

std::vector<std::string> ParseCurrenciesJson(const char* body,
                                             std::size_t body_len) {
  std::vector<std::string> out;
  if (body == nullptr || body_len == 0) return out;
  // cJSON_ParseWithLength tolerates an unterminated buffer; using it
  // (instead of cJSON_Parse) keeps the caller free of "+1 for NUL"
  // contortions on the HTTP rx path.
  cJSON* root = cJSON_ParseWithLength(body, body_len);
  if (root == nullptr) return out;
  if (!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return out;
  }
  std::set<std::string> seen;
  cJSON* el = nullptr;
  cJSON_ArrayForEach(el, root) {
    if (!cJSON_IsString(el)) continue;
    const char* str = el->valuestring;
    if (str == nullptr) continue;
    const std::size_t n = std::strlen(str);
    if (!IsValidIso(str, n)) continue;
    std::string code(str, n);
    if (seen.insert(code).second) {
      out.push_back(std::move(code));
    }
  }
  cJSON_Delete(root);
  return out;
}

}  // namespace btclock
