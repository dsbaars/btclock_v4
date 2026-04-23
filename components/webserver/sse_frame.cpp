// SSE frame formatter — pulled out of sse_server.cpp so the host unit
// tests can link it without dragging in esp_http_server / FreeRTOS.
// See sse_server.hpp for the frame format contract.

#include "sse_frame.hpp"

#include <cstddef>

namespace btclock {

std::string MakeSseFrame(std::string_view event_name, std::string_view data) {
  // Upper bound: every byte in `data` could be a newline, each of which
  // then expands into `data: \n`. Reserve generously to skip reallocs.
  std::string out;
  out.reserve(event_name.size() + data.size() * 2 + 16);

  if (!event_name.empty()) {
    out.append("event: ");
    out.append(event_name.data(), event_name.size());
    out.push_back('\n');
  }

  // Split on '\n'. A trailing newline in `data` yields a final empty
  // `data:` line — matching the WHATWG EventSource parser behaviour
  // (each newline terminates a data field, so N newlines give N+1
  // fields).
  size_t begin = 0;
  while (begin <= data.size()) {
    const size_t nl = data.find('\n', begin);
    const size_t end = (nl == std::string_view::npos) ? data.size() : nl;
    out.append("data:");
    if (end > begin) {
      out.push_back(' ');
      out.append(data.data() + begin, end - begin);
    }
    out.push_back('\n');
    if (nl == std::string_view::npos) break;
    begin = nl + 1;
  }

  // Frame terminator — the blank line the browser uses to dispatch.
  out.push_back('\n');
  return out;
}

std::string MakeSseComment(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 4);
  out.push_back(':');
  if (!text.empty()) {
    out.push_back(' ');
    out.append(text.data(), text.size());
  }
  out.append("\n\n");
  return out;
}

}  // namespace btclock
