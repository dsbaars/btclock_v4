# Real-device photos still wanted

The synthetic WASM-rendered PNGs under `docs/img/screens/` and
`docs/img/fonts/` cover the panel layouts we can reproduce off-device,
and `docs/img/webui/` carries the WebUI screenshots driven by
`tools/docs/capture_webui.mjs` (Playwright + headless Chromium against
a live BTClock or the Vite dev server). Some doc surfaces still
benefit from a real-device shot — e-paper has a look the synthetic
render can't match (panel bezels, frontlight halo, LED bleed, ambient
reflection).

The current camera setup (Continuity Camera on Rev A, C920 on Rev B —
see `MEMORY.md`) is OK for verification but not crisp enough for
publication-grade docs. When a better camera is available, capture
the shots below and drop them under `docs/img/photos/<board>/`. The
HANDBOOK can then swap the synthetic PNG for the real photo where
applicable.

| File path | Board | Subject | Notes |
|---|---|---|---|
| `docs/img/photos/rev_b/hero.jpg` | Rev B | Three-quarter studio shot of the assembled clock against a neutral background | The README hero shot. |
| `docs/img/photos/rev_a/hero.jpg` | Rev A | Same composition, Rev A | For BUILD_FROM_SOURCE / Quickstart "what variant do I have" comparison. |
| `docs/img/photos/rev_b/back.jpg` | Rev B | Back panel — USB-C port, button row, MAC sticker | Quickstart wiring section. |
| `docs/img/photos/rev_b/buttons.jpg` | Rev B | Close-up of the four-button cluster, top-down | Handbook → On-device controls. |
| `docs/img/photos/rev_b/provisioning.jpg` | Rev B | Panels in provisioning mode showing SSID / password / QR | Quickstart step 1. A synthetic SVG render is in `docs/img/screens/provisioning_first_boot.png` as a fallback (uses a placeholder QR; the real device renders a real WiFi-join QR). |
| `docs/img/photos/rev_b/frontlight_on.jpg` | Rev B | Frontlight at 100% in a dim room | Handbook → Frontlight (Rev B). |
| `docs/img/photos/rev_b/frontlight_off.jpg` | Rev B | Same scene with frontlight off, ambient lighting only | Handbook → Frontlight (Rev B). |
| `docs/img/photos/rev_b/leds_block_flash.jpg` | Rev B | LEDs mid-block-flash (orange `#E04300` default) | Handbook → LEDs. |
| `docs/img/photos/rev_b/clock_screen.jpg` | Rev B | The Time/clock screen displaying real time — synthetic render not available, see `tools/wasm/render_doc_screens.mjs` `clock` skip flag | Handbook → Screen catalogue → Time. |
| `docs/img/photos/rev_b/debug_overlay.jpg` | Rev B | Debug overlay (button-4 toggle) | Handbook → Debug overlay. |

Photos already on disk under `photos/` (taken with the lower-quality
camera) are usable as a fallback if the better-camera retake is
delayed. The HANDBOOK references synthetic renders by default; swap
to real photos under `docs/img/photos/` once available.
