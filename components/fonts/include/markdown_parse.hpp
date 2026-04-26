// Pure-logic helper extracted from font.cpp's DrawMarkdown so the
// line-splitting + bold-marker handling can be unit-tested without the
// stb_truetype + ESP-IDF rasteriser dependency. font.cpp's DrawMarkdown
// calls ParseMarkdownLines() and then renders each segment through the
// real Font + framebuffer.

#pragma once

#include <string>
#include <vector>

namespace btclock {

struct MarkdownLine {
  std::string text;
  bool is_bold = false;
};

// Whole-line bold: a line whose first non-whitespace character is '*'
// renders as bold; every '*' in the line is then stripped, and
// trailing whitespace is trimmed (so "*Hostname:* " collapses to
// "Hostname:" rather than "Hostname: "). '\r' is dropped (CRLF input
// from web forms shouldn't produce blank trailing lines). Mirrors the
// firmware's renderText() parser.
inline std::vector<MarkdownLine> ParseMarkdownLines(const char* text) {
  std::vector<MarkdownLine> lines;
  if (text == nullptr) return lines;

  std::string cur;
  auto flush = [&]() {
    MarkdownLine l;
    l.text = cur;
    l.is_bold = false;
    if (!l.text.empty() && l.text[0] == '*') {
      l.is_bold = true;
      std::string filtered;
      filtered.reserve(l.text.size());
      for (char c : l.text) {
        if (c != '*') filtered.push_back(c);
      }
      while (!filtered.empty() &&
             (filtered.back() == ' ' || filtered.back() == '\t')) {
        filtered.pop_back();
      }
      l.text = std::move(filtered);
    }
    lines.push_back(std::move(l));
    cur.clear();
  };

  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '\r') continue;
    if (*p == '\n') {
      flush();
      continue;
    }
    cur.push_back(*p);
  }
  flush();
  return lines;
}

}  // namespace btclock
