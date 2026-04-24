#include "show_text_parse.hpp"

#include <cctype>
#include <cstring>

#include "cJSON.h"

namespace btclock {
namespace {

// Uppercase one ASCII byte. Old firmware uses Arduino's String::toUpperCase
// which is ASCII-only; non-ASCII bytes (UTF-8 continuation bytes) pass
// through unchanged. Mirror that — avoids double-applying toupper to
// already-upper characters and keeps UTF-8 multi-byte sequences intact.
char UpperAsciiOnly(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u < 0x80) return static_cast<char>(std::toupper(u));
  return c;
}

ShowTextParseResult MakeFailure(const char* token) {
  ShowTextParseResult r;
  r.ok = false;
  r.error = token;
  return r;
}

// Build n_panels cells from a flat text buffer using the old-firmware
// one-char-per-panel heuristic. Characters past n_panels are dropped.
ShowTextParseResult SplitTextAcrossPanels(const std::string& text,
                                          std::size_t n_panels) {
  ShowTextParseResult r;
  r.ok = true;
  r.cells.resize(n_panels);
  const std::size_t take =
      text.size() < n_panels ? text.size() : n_panels;
  for (std::size_t i = 0; i < take; ++i) {
    r.cells[i].assign(1, UpperAsciiOnly(text[i]));
  }
  return r;
}

}  // namespace

ShowTextParseResult ParseShowTextBody(std::string_view body,
                                      std::size_t n_panels) {
  if (n_panels == 0) return MakeFailure("no_panels");
  // Permit an empty body so a caller that passed the text via `?t=`
  // can still reach this code path with no JSON; the caller decides
  // what to feed us. Empty body + empty text clears the display.
  std::string text;
  if (!body.empty()) {
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root) return MakeFailure("bad_json");
    if (!cJSON_IsObject(root)) {
      cJSON_Delete(root);
      return MakeFailure("bad_json");
    }
    const cJSON* t = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(t) || t->valuestring == nullptr) {
      cJSON_Delete(root);
      return MakeFailure("missing_text");
    }
    text = t->valuestring;
    cJSON_Delete(root);
  }
  return SplitTextAcrossPanels(text, n_panels);
}

ShowTextParseResult ParseShowCustomBody(std::string_view body,
                                        std::size_t n_panels) {
  if (n_panels == 0) return MakeFailure("no_panels");
  if (body.empty()) return MakeFailure("bad_json");

  cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
  if (!root) return MakeFailure("bad_json");

  // Dual-shape: bare array (old-firmware wire format) or
  // {"cells":[...]} wrapper. Normalise to a cJSON array pointer.
  cJSON* arr = nullptr;
  if (cJSON_IsArray(root)) {
    arr = root;
  } else if (cJSON_IsObject(root)) {
    cJSON* cells = cJSON_GetObjectItemCaseSensitive(root, "cells");
    if (cJSON_IsArray(cells)) arr = cells;
  }
  if (arr == nullptr) {
    cJSON_Delete(root);
    return MakeFailure("bad_json");
  }

  ShowTextParseResult r;
  r.ok = true;
  r.cells.resize(n_panels);
  const int n = cJSON_GetArraySize(arr);
  const int take = n < static_cast<int>(n_panels)
                       ? n
                       : static_cast<int>(n_panels);
  for (int i = 0; i < take; ++i) {
    const cJSON* e = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsString(e) || e->valuestring == nullptr) {
      cJSON_Delete(root);
      return MakeFailure("bad_json");
    }
    r.cells[static_cast<std::size_t>(i)] = e->valuestring;
  }
  cJSON_Delete(root);
  return r;
}

}  // namespace btclock
