#include "nostr/json_emit.hpp"

namespace btclock {
namespace nostr {
namespace json_emit {

void AppendString(std::string& out, const std::string& s) {
  out.push_back('"');
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      default:
        out.push_back(static_cast<char>(c));
        break;
    }
  }
  out.push_back('"');
}

void AppendUint(std::string& out, uint64_t n) {
  char buf[24];
  size_t i = 0;
  if (n == 0) {
    buf[i++] = '0';
  } else {
    char tmp[24];
    size_t j = 0;
    while (n != 0) {
      tmp[j++] = static_cast<char>('0' + (n % 10));
      n /= 10;
    }
    while (j != 0) buf[i++] = tmp[--j];
  }
  out.append(buf, i);
}

}  // namespace json_emit
}  // namespace nostr
}  // namespace btclock
