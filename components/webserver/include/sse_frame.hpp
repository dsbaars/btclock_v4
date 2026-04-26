// SSE frame formatter — pure helpers broken out of sse_server.hpp so
// host unit tests can include them without pulling FreeRTOS /
// esp_http_server into the translation unit. Device code should still
// include sse_server.hpp (which re-exports these) to avoid two
// parallel include sets.

#pragma once

#include <string>
#include <string_view>

namespace btclock {

// Format a single SSE frame body.
//
//   name="status", data="{…}"           -> "event: status\ndata: {…}\n\n"
//   name="",       data="hello"         -> "data: hello\n\n"    (anonymous)
//   name="status", data="line1\nline2"  -> "event: status\ndata: line1\ndata:
//   line2\n\n" name="",       data=""              -> "data:\n\n" (empty event)
//
// Multi-line payloads split per the EventSource spec — each '\n' in
// `data` starts a new `data:` line. A trailing '\n' in `data` becomes
// an empty final `data:` line, matching the WHATWG parser's N -> N+1
// field expansion.
std::string MakeSseFrame(std::string_view event_name, std::string_view data);

// SSE "comment" keep-alive frame (`: <text>\n\n`). Browsers ignore
// comments but the bytes reset idle-timeouts on intermediaries.
std::string MakeSseComment(std::string_view text);

}  // namespace btclock
