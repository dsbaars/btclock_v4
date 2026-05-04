#include "app/boot/init_mbedtls_psram.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "mbedtls/platform.h"

namespace btclock {
namespace {

// Surgical mitigation for an internal-heap leak attributed by
// heap_trace on Rev A to:
//   esp_crt_bundle.c:489  esp_crt_ca_cb_callback
//   x509_crt.c:2578       x509_crt_verify_chain
// Every HTTPS/WSS handshake (noderunners poll @ 60 s, Nostr WSS
// reconnect, mempool secure WS) leaks ~6 small calloc'd X.509
// fragments (2–168 B each, ~140 B total per handshake). At one
// handshake per minute that's ~2 KiB / 15 min of internal-heap
// drift — exactly matched the observed rate before this hook landed.
//
// The fix routes every mbedtls calloc to PSRAM (1.95 MiB free),
// where the leak is invisible at human timescales. The companion
// SPIRAM_MALLOC_ALWAYSINTERNAL=0 in sdkconfig.defaults catches the
// same allocations via the global default-malloc path; this hook is
// belt-and-suspenders so the mitigation survives any future config
// regression that might re-pin small allocs to internal DRAM.
//
// Real upstream fix is in esp-idf's esp_crt_bundle.c — the callback
// allocates fresh structures that mbedtls_x509_crt_free doesn't
// always reach on the chain-walk's success path. File at
// https://github.com/espressif/esp-idf/issues with the four-PC
// chain above.
void* PsramCalloc(size_t n, size_t sz) {
  const size_t bytes = n * sz;
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) std::memset(p, 0, bytes);
  return p;
}

void PsramFree(void* p) {
  heap_caps_free(p);
}

}  // namespace

void InitMbedtlsPsram() {
  mbedtls_platform_set_calloc_free(&PsramCalloc, &PsramFree);
}

}  // namespace btclock
