#include "show_text_parse.hpp"

#include <cstdint>
#include <cstring>

#include "cJSON.h"

namespace btclock {
namespace {

ShowTextParseResult MakeFailure(const char* token) {
  ShowTextParseResult r;
  r.ok = false;
  r.error = token;
  return r;
}

bool Utf8IsContinuation(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

// Length of one well-formed UTF-8 codepoint starting at `pos`, or 1 when
// `pos` does not begin a valid sequence (caller treats one raw byte).
std::size_t Utf8CodepointByteLength(std::string_view s, std::size_t pos) {
  if (pos >= s.size()) return 0;
  const unsigned char c0 = static_cast<unsigned char>(s[pos]);
  if (c0 < 0x80) return 1;

  const auto cont = [&](std::size_t j) -> bool {
    return j < s.size() && Utf8IsContinuation(static_cast<unsigned char>(s[j]));
  };

  if ((c0 & 0xE0) == 0xC0) {
    if (!cont(pos + 1)) return 1;
    const unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
    const std::uint32_t cp =
        (static_cast<std::uint32_t>(c0 & 0x1Fu) << 6) | (c1 & 0x3Fu);
    if (cp < 0x80u) return 1;
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0) {
    if (!cont(pos + 1) || !cont(pos + 2)) return 1;
    const unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
    const unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
    const std::uint32_t cp = (static_cast<std::uint32_t>(c0 & 0x0Fu) << 12) |
                             (static_cast<std::uint32_t>(c1 & 0x3Fu) << 6) |
                             (c2 & 0x3Fu);
    if (cp < 0x800u) return 1;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 1;
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0) {
    if (!cont(pos + 1) || !cont(pos + 2) || !cont(pos + 3)) return 1;
    const unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
    const unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
    const unsigned char c3 = static_cast<unsigned char>(s[pos + 3]);
    const std::uint32_t cp = (static_cast<std::uint32_t>(c0 & 0x07u) << 18) |
                             (static_cast<std::uint32_t>(c1 & 0x3Fu) << 12) |
                             (static_cast<std::uint32_t>(c2 & 0x3Fu) << 6) |
                             (c3 & 0x3Fu);
    if (cp < 0x10000u || cp > 0x10FFFFu) return 1;
    return 4;
  }
  return 1;
}

}  // namespace

ShowTextParseResult SplitShowTextAcrossPanels(std::string_view text,
                                              std::size_t n_panels) {
  ShowTextParseResult r;
  if (n_panels == 0) return MakeFailure("no_panels");
  r.ok = true;
  r.cells.assign(n_panels, std::string());
  std::size_t panel = 0;
  for (std::size_t i = 0; i < text.size() && panel < n_panels;) {
    const std::size_t seq = Utf8CodepointByteLength(text, i);
    if (seq == 0) break;
    r.cells[panel].assign(text.data() + i, seq);
    ++panel;
    i += seq;
  }
  return r;
}

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
  return SplitShowTextAcrossPanels(text, n_panels);
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
  float digit_px = 0.0f;
  if (cJSON_IsArray(root)) {
    arr = root;
  } else if (cJSON_IsObject(root)) {
    cJSON* cells = cJSON_GetObjectItemCaseSensitive(root, "cells");
    if (cJSON_IsArray(cells)) arr = cells;
    // Optional digit pixel-height override. Clamp to the digitFontPx
    // range so a stray value can't blow past the panel; out-of-range or
    // non-numeric silently falls back to auto-sizing (0).
    const cJSON* px = cJSON_GetObjectItemCaseSensitive(root, "digitPx");
    if (cJSON_IsNumber(px)) {
      const double v = px->valuedouble;
      if (v >= 20.0 && v <= 220.0) digit_px = static_cast<float>(v);
    }
  }
  if (arr == nullptr) {
    cJSON_Delete(root);
    return MakeFailure("bad_json");
  }

  ShowTextParseResult r;
  r.ok = true;
  r.digit_px = digit_px;
  r.cells.resize(n_panels);
  const int n = cJSON_GetArraySize(arr);
  const int take =
      n < static_cast<int>(n_panels) ? n : static_cast<int>(n_panels);
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
