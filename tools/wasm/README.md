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
