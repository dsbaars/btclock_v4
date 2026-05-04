#pragma once

namespace btclock {

// Install PSRAM-routed calloc/free hooks for mbedTLS via
// mbedtls_platform_set_calloc_free. Must be called once at boot,
// before any mbedTLS function runs (i.e. before WiFi/TLS handshakes,
// before HTTPS pollers, before WSS clients). Idempotent and cheap.
void InitMbedtlsPsram();

}  // namespace btclock
