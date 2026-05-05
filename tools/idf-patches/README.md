# IDF patches

Project-local patches applied to the ESP-IDF tree before every firmware
build. Each patch is a unified diff against the IDF root (relative
paths start at `components/...`).

`apply.sh` is idempotent — Forgejo CI runs it before `idf.py build`,
and local devs should run it after every `git pull` / version-bump of
the IDF tree.

## Local use

```bash
IDF_PATH=$HOME/esp/v6.0/esp-idf tools/idf-patches/apply.sh
```

The script defaults `IDF_PATH` to `$HOME/esp/v6.0/esp-idf` if unset.

## Catalog

| Patch | Upstream | Removed when |
|---|---|---|
| `0001-cross_signed_verify_memory_leak.patch` | [espressif/esp-idf#18512](https://github.com/espressif/esp-idf/issues/18512) (resolves [#18550](https://github.com/espressif/esp-idf/issues/18550)) | The fix lands in a released IDF v6.0.x and we bump to it |

### 0001-cross_signed_verify_memory_leak.patch

Fixes a per-handshake heap leak in `esp_crt_bundle.c` when
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`. The
cross-signed verification path was `calloc`'ing copies of ASN.1 named
data that the chain-walk never freed; the patch references the source
buffers directly instead. ~140 B leaked per TLS handshake on our build
prior to the patch.

Validation evidence (Rev A, dataSource=Nostr, ~12 min observation):
zero `esp_crt_bundle.c` / `x509_crt_verify_chain` frames in the
heap_trace LEAK histogram, free internal heap statistically flat across
both 90 s and 600 s windows.

When upstream releases the fix, drop this patch AND the local PSRAM
mitigation in `main/app/boot/init_mbedtls_psram.cpp` together —
restore `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` in
`sdkconfig.defaults` at the same time.
