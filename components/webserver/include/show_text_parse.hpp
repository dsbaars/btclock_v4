// Pure-logic JSON parsers for POST /api/show/text and /api/show/custom.
//
// Split out of control_server.cpp so the host-test suite can exercise
// the payload rules (truncation, JSON shape) without bringing up
// esp_http_server. Depends only on cJSON — the vendored cJSON used by
// the host-tests covers the same API surface as ESP-IDF's json
// component.
//
// Old-firmware parity source:
//   src/lib/net/webserver/actions.cpp lines 68..110 (onApiShowText,
//   onApiShowTextAdvanced). Both clamp to NUM_SCREENS; /api/show/text
//   placed one byte per panel with Arduino toUpperCase (ASCII-only). We
//   instead split UTF-8 codepoints and preserve case — selectable TTFs
//   subset U+0020-007E (includes a-z). /api/show/custom takes
//   a bare JSON array and copies each element verbatim into one panel.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace btclock {

struct ShowTextParseResult {
  // One string per panel. Length is always == n_panels on success, with
  // trailing empty strings when the caller provided fewer characters.
  std::vector<std::string> cells;
  // Optional digit pixel-height override for single-glyph cells (the
  // wrapper-object `{"cells":[...],"digitPx":N}` form of
  // /api/show/custom). 0 means "unset" -> the renderer keeps its
  // codepoint-length auto-sizing. Accepted range is 20..220; anything
  // outside it, or non-numeric, is silently ignored (falls back to 0)
  // rather than failing the request. Note this is deliberately WIDER
  // than the digitFontPx *setting*, which is bounded 80..220 — the two
  // are separate knobs and should not be conflated.
  float digit_px = 0.0f;
  // Valid iff `ok`. When false, the handler should respond 400 and
  // include `error` in the body so operators can distinguish "bad JSON"
  // from "empty text".
  bool ok = false;
  std::string error;  // short machine-readable token ("bad_json", etc.)
};

// Parse the body of POST /api/show/text.
//
// Accepts two shapes for backward-compat with the old firmware:
//   1. `{"text":"..."}` — task-specified JSON body.
//   2. The raw query string `t=...` style is handled by the caller
//      (QueryParam) *before* this function; when it fires this parser
//      only sees the JSON body.
//
// Each UTF-8 codepoint is placed on one panel left-to-right; case is kept
// as sent (font subsets include lowercase ASCII).
// Panels past the text length stay empty; extra codepoints are dropped.
// No word-wrap — same placement model as v3, but Unicode-aware (v3 was
// effectively ASCII-only because it split raw bytes).
ShowTextParseResult ParseShowTextBody(std::string_view body,
                                      std::size_t n_panels);

// Shared by POST /api/show/text (JSON `t`, query `t`, and legacy
// `{"text":"..."}`) — splits UTF-8 into one scalar value per panel.
ShowTextParseResult SplitShowTextAcrossPanels(std::string_view text,
                                              std::size_t n_panels);

// Parse the body of POST /api/show/custom.
//
// Accepts two shapes:
//   1. Bare JSON array: `["C1","C2",...]` — the old firmware's exact
//      wire format (src/lib/net/webserver/actions.cpp:90-110).
//   2. Wrapper object `{"cells":["C1","C2",...]}` — a conservative
//      forward-extension; the WebUI can migrate to this without
//      breaking existing API clients.
//
// Elements past n_panels are discarded; missing trailing elements are
// emitted as empty strings so the caller always sees exactly n_panels
// entries. Non-string array elements fail the parse with "bad_json".
ShowTextParseResult ParseShowCustomBody(std::string_view body,
                                        std::size_t n_panels);

}  // namespace btclock
