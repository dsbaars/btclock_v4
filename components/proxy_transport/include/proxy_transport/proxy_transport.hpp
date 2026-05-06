#pragma once

// Public IDF-side API for proxy_transport. The pure framing/bypass
// helpers live in proxy_framing.hpp and proxy_bypass.hpp and stay
// host-testable; this header is the thin layer that turns a
// btclock::proxy::Config into an esp_transport_handle_t suitable for
// esp_http_client_config_t.transport or esp_websocket_client_config_t
// .ext_transport.

#include <string_view>

#include "esp_err.h"
#include "esp_transport.h"
#include "proxy_transport/proxy_config.hpp"
#include "proxy_transport/proxy_url.hpp"

namespace btclock {
namespace proxy {

// Build a single esp_transport that:
//   1. Opens a TCP socket to the *proxy*,
//   2. Runs the protocol-specific handshake to the *destination*,
//   3. Optionally runs a TLS handshake on top (esp-tls + crt_bundle).
//
// The destination host+port flow in via the esp_transport `connect()`
// callback signature — esp_http_client / esp_websocket_client parse
// cfg.url / cfg.uri and call our transport with host+port already
// extracted, so this factory takes only the proxy config + TLS knobs.
// Bypass-list evaluation happens at connect time against the
// upper-layer-supplied host.
//
// When `cfg.kind == kNone` or the destination matches the bypass list,
// the transport performs a direct TCP connect to the destination
// instead — same external behaviour as a non-proxy build.
//
// `use_tls` controls whether the upper layer (HTTPS / WSS) needs the
// transport to handle TLS itself. esp_http_client and
// esp_websocket_client both replace their entire transport stack with
// the user-provided one (CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
// gates the http_client field), so we have to do TLS in here.
//
// `crt_bundle_attach` is typically `esp_crt_bundle_attach`; pass
// nullptr when the upper layer disables verification (rare).
//
// Ownership: caller destroys the returned handle exactly once with
// esp_transport_destroy(). The handle internally manages the socket,
// the esp_tls_t (if TLS), and any framing scratch buffers.
//
// Returns nullptr on alloc failure.
struct TransportParams {
  bool use_tls = false;
  esp_err_t (*crt_bundle_attach)(void* conf) = nullptr;
};
esp_transport_handle_t MakeProxyTransport(const Config& cfg,
                                          const TransportParams& params);

// RAII wrapper around `esp_transport_handle_t` for call sites that
// have many early-return paths (OTA, currencies, etc.). Holds the
// handle non-owning while the upper-layer client uses it; destroys it
// on scope exit. Caller passes `.get()` into cfg.transport /
// cfg.ext_transport.
class OwnedTransport {
 public:
  OwnedTransport() = default;
  explicit OwnedTransport(esp_transport_handle_t h) : h_(h) {}
  OwnedTransport(OwnedTransport&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
  OwnedTransport& operator=(OwnedTransport&& o) noexcept {
    Reset();
    h_ = o.h_;
    o.h_ = nullptr;
    return *this;
  }
  OwnedTransport(const OwnedTransport&) = delete;
  OwnedTransport& operator=(const OwnedTransport&) = delete;
  ~OwnedTransport() { Reset(); }

  esp_transport_handle_t get() const { return h_; }
  esp_transport_handle_t release() {
    auto* t = h_;
    h_ = nullptr;
    return t;
  }
  void Reset() {
    if (h_) {
      esp_transport_destroy(h_);
      h_ = nullptr;
    }
  }

 private:
  esp_transport_handle_t h_ = nullptr;
};

// Picks `use_tls` from the URL/URI scheme. Used by the call-site
// migration so each site can write
//   ParamsForUrl(my_url, esp_crt_bundle_attach)
// without re-encoding the wss/https knowledge in C++ everywhere.
inline TransportParams ParamsForUrl(
    std::string_view url, esp_err_t (*crt_bundle_attach)(void*) = nullptr) {
  TransportParams p{};
  p.use_tls = UrlImpliesTls(url);
  p.crt_bundle_attach = crt_bundle_attach;
  return p;
}

}  // namespace proxy
}  // namespace btclock
