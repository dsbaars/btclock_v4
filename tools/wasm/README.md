# tools/wasm

The firmware's screen renderers compiled to WebAssembly, plus tools
that consume the WASM module off-device.

## Files

| File | Purpose |
|---|---|
| `build.sh` | Compiles the screen stack to WASM. Outputs `dist/btclock_datahandler.{js,wasm}`. Requires Emscripten ≥ 3.1.0 (`brew install emscripten` or emsdk). |
| `binding.cpp` | C++ ↔ JS bindings (embind). Exposes `parse*`, `render*FrameBuffer`, `render*AlphaBuffer`, `setRenderOptions`, `setVerticalDesc`, `getPanelDimensions`. |
| `wasm_panel.hpp` | Shim `EpdPanel` that the firmware's screen renderers compile against under `BTCLOCK_WASM_BUILD`. |
| `font_wasm_aa.{cpp,hpp}` | Sidechannel that captures stb_truetype's pre-threshold alpha coverage for the AA preview path. |
| `gen_font_blobs.py` | Bundles the TTFs from `components/fonts/assets/` into a single `.cpp` for the WASM build (replaces ESP-IDF's `EMBED_FILES` mechanism). |
| `preview.html` | Browser-based interactive previewer. Run `build.sh` first, then `python3 -m http.server 8000 --directory tools/wasm` and open `http://localhost:8000/preview.html`. Lets you flip font, panel count, vertical-description, and AA toggle. |
| `smoke_test.mjs` | Node-side regression test for the bindings. `node tools/wasm/smoke_test.mjs`. |
| `analyze_bolt.mjs` | Inspector for the WASM module's symbol table — used when debugging missing exports. |
| `render_doc_screens.mjs` | **Doc-image renderer.** Composites WASM-rendered panels inside the BTClock acrylic-frame outline and writes the PNGs the user-facing docs reference. |
| `render_font_sample.mjs` | Renders one block-height sample (`family` id) in the same PCB/chrome style as docs. Fast path for typography checks. |
| `render_font_ab.sh` | End-to-end A/B helper for one font asset: builds regular + candidate renders and restores the original asset. |
| `render_font_candidates.sh` | Generic N-weight variant of `render_font_ab.sh`. Accepts repeatable `--weight name=path` pairs, hot-swaps each TTF into the chosen slot, rebuilds WASM, renders one PCB-framed PNG per weight, then restores the slot. Candidate TTFs must already be subsetted (see `tools/fonts/regen.sh` or a font-specific wrapper). |
| `render_azeret.sh` | Wrapper around `render_font_candidates.sh` that fetches AzeretMono Regular / SemiBold / Bold from `displaay/Azeret`, subsets them to BTClock's glyph range, and renders one block-height sample per weight. Re-runnable; set `AZERET_SKIP_DOWNLOAD=1` to reuse cached TTFs in `/tmp/azeret/`. |

## Doc-image renderer

`render_doc_screens.mjs` is the script that produces every PNG under
`docs/img/screens/` and `docs/img/fonts/`. Re-run after any change to
the screen renderers, the screen catalogue, or the frame layout:

```bash
node tools/wasm/render_doc_screens.mjs
```

Outputs:

- `docs/img/screens/<id>.png` — one composite per rotation/overlay screen.
- `docs/img/fonts/<font_id>.png` — block-height rendered with each font face.
- `docs/img/screens/provisioning_first_boot.png` — pure-SVG render of
  the first-boot welcome screen (no WASM call; the on-device renderer
  uses a different code path that isn't exposed via WASM bindings).
- `docs/img/MANIFEST.json` — index with metadata (timestamp,
  geometry, per-item path).

**Titles & settings terminology:** prefer firmware-facing names in `title`
strings (e.g. `priceSymMode=0`, not legacy `useSatsSymbol=false`) so handbook
cross-references stay aligned with [`docs/SETTINGS.md`](../../docs/SETTINGS.md).
Re-run this script after renaming so `MANIFEST.json` stays in sync.

Dependencies it borrows:

- The `sharp` install in `data/node_modules/sharp/` (no separate
  `package.json` at the repo root).
- The pre-built WASM bundle at `tools/wasm/dist/`. Run `build.sh`
  first if `dist/` is empty or out of date.

Rendering pipeline (per screen):

1. Call the WASM `render*FrameBuffer(...)` for each panel.
2. Convert the 1-bpp framebuffer (native SSD1680 orientation, k180)
   back to logical 122×250 RGBA, undoing the rotation transform.
3. Resize each panel directly to its final pixel size (~201×525 px at
   the default 9 px/mm) using libvips' Lanczos filter — gives smooth
   glyph edges from the 1-bpp source without the staircase that
   nearest-neighbour sampling produces.
4. Build the BTClock acrylic frame as SVG (matches the geometry from
   `Rev_A_and_B_2mm-acrylic-front.svg`: 219.6 × 81.25 mm content area,
   2 mm corner radius, 4 corner drill holes, "BTClock" wordmark).
5. Composite the per-panel PNGs onto the rasterised frame at integer
   pixel offsets.

### Why we don't use the AA bindings here

`render*AlphaBuffer` would produce smoother edges natively (raw
stb_truetype coverage instead of 1-bpp threshold + Lanczos), but
verticalDesc label panels mis-rotate in that path — the alpha
sidechannel writes pre-rotation logical coords and the panel-local
`Rotation::k90Cw` never lands. Tracked as `btclock_v4-m67`. When that
fixes, `render_doc_screens.mjs` should switch back to the alpha
buffers and drop the Lanczos resample.

### Adjusting the frame layout

Change the constants at the top of `render_doc_screens.mjs`
(`PANEL_W_MM`, `PANEL_H_MM`, `SIDE_MARGIN_MM`, `PX_PER_MM`, etc.) to
re-target the composite. The script auto-recomputes panel positions
and gutters.

### Adding a new screen

Add an entry to the `SCREENS` array with `{ id, title, fn }` where
`fn` returns the `Uint8Array[]` from `mod.render<...>FrameBuffer(...)`.
Re-run the script — the new file will appear under
`docs/img/screens/<id>.png` and in `MANIFEST.json`.

If the screen has no WASM binding (like the clock screen, which uses
real wall-clock time), set `skip: true` and instead capture a real
device photo, tracked in [`docs/img/PHOTOS_NEEDED.md`](../../docs/img/PHOTOS_NEEDED.md).

## Fast font A/B comparisons

For font weight experiments, prefer the single-sample tools instead of
re-rendering every docs screen.

1) Build one sample directly (already-built WASM):

```bash
node tools/wasm/render_font_sample.mjs \
  --family 2 \
  --out docs/img/fonts/inter_regular_candidate.png
```

2) Run full regular-vs-candidate flow (including restore):

```bash
tools/wasm/render_font_ab.sh \
  --family 2 \
  --font-id inter \
  --asset components/fonts/assets/Inter.ttf \
  --candidate /tmp/Inter-SemiBold.ttf
```

This writes:

- `docs/img/fonts/<font-id>_regular_candidate.png`
- `docs/img/fonts/<font-id>_semibold_candidate.png`

3) Render an arbitrary number of weights of the same candidate face
   in one swoop with the generic runner:

```bash
tools/wasm/render_font_candidates.sh \
  --font-id azeret \
  --asset components/fonts/assets/Inter.ttf \
  --family 2 \
  --weight regular=/tmp/AzeretMono-Regular-subset.ttf \
  --weight semibold=/tmp/AzeretMono-SemiBold-subset.ttf \
  --weight bold=/tmp/AzeretMono-Bold-subset.ttf
```

Outputs `docs/img/fonts/<font-id>_<weight>.png` per `--weight` pair and
restores the original asset + rebuilds WASM once at the end. Candidate
TTFs are expected to be pre-subsetted to BTClock's glyph range.
