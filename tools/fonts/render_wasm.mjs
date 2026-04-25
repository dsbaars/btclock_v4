// Drive the BTClock WASM bundle from Node and dump panel-0 (the
// BLOCK/HEIGHT label panel of `renderBlockHeightFrameBuffer`) for each
// shipped font family, so the FreeType reference produced by the
// sibling Python script can be diffed against the *firmware's actual*
// rasteriser (the same stb_truetype + DrawSplitText + auto-fit code
// path the device runs).
//
// Why panel 0 specifically: RenderBlockHeightScreen lays the
// "BLOCK/HEIGHT" split label on panel 0 and the digits on panels 1..N-1,
// so panel 0 is the cleanest "labels look correct in this font?" probe.
//
// Output: per-font PGM (P5 binary grayscale) at 122x250 logical pixels,
// matching the SSD1680 2.13" panel in landscape. PGM is built into
// libgd / Pillow / sips so it loads without extra deps; the Python
// script reads them and stitches the comparison grid.
//
// Usage: node tools/fonts/render_wasm.mjs

import { fileURLToPath } from "node:url";
import path from "node:path";
import fs from "node:fs";
import createBtclockModule from "../wasm/dist/btclock_datahandler.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.join(HERE, "compare", "wasm");
fs.mkdirSync(OUT, { recursive: true });

// Mirror tools/wasm/binding.cpp::setRenderOptions ids (also documented
// in tools/wasm/preview.html's <select>).
const FAMILIES = [
  { id: 0, name: "antonio" },
  { id: 1, name: "oswald" },
  { id: 2, name: "inter" },
  { id: 3, name: "sourceSerif" },
  { id: 4, name: "merriweather" },
  { id: 5, name: "bitter" },
  { id: 6, name: "atkinson" },
];

// Native panel layout — 16-byte stride, MSB-first within each byte.
// 0 = black, 1 = white. The renderer wrote logical coords through a
// k180 rotation (see SetPixelLandscape in components/fonts/font.cpp);
// to recover the upright image we inverse-rotate while decoding.
const STRIDE = 16;
const NATIVE_W = 122;
const NATIVE_H = 250;
const LOGICAL_W = NATIVE_W;
const LOGICAL_H = NATIVE_H;

function fbToPgm(fb) {
  const px = new Uint8Array(LOGICAL_W * LOGICAL_H);
  px.fill(255);
  for (let ny = 0; ny < NATIVE_H; ++ny) {
    for (let nx = 0; nx < NATIVE_W; ++nx) {
      const byteIdx = ny * STRIDE + (nx >> 3);
      const bit = 7 - (nx & 7);
      const isWhite = (fb[byteIdx] >> bit) & 1;
      // k180 inverse: native (nx, ny) → logical (W-1-nx, H-1-ny).
      const lx = LOGICAL_W - 1 - nx;
      const ly = LOGICAL_H - 1 - ny;
      px[ly * LOGICAL_W + lx] = isWhite ? 255 : 0;
    }
  }
  return px;
}

function writePgm(filePath, px, w, h) {
  const header = Buffer.from(`P5\n${w} ${h}\n255\n`, "ascii");
  fs.writeFileSync(filePath, Buffer.concat([header, Buffer.from(px)]));
}

const mod = await createBtclockModule();

// Use a 6-digit block height so RenderBlockHeightScreen<7> stays out of
// the "now_overflow" path (block_height.cpp:48-54). With 7 digits the
// label is dropped and panel 0 becomes the leading digit; with 6 digits
// panel 0 is the BLOCK/HEIGHT split label, which is what we want to
// diff against the FreeType reference.
const blockHeight = 946577;
for (const { id, name } of FAMILIES) {
  // setRenderOptions(panels, font_family) — keep panels at 7 to match
  // Rev A/Rev B layout, which is what the device under test renders.
  mod.setRenderOptions(7, id);
  mod.setVerticalDesc(false);
  const fbs = mod.renderBlockHeightFrameBuffer(blockHeight);
  // fbs is Array<Uint8Array>; each entry is one panel's framebuffer.
  // Panel 0 = the BLOCK/HEIGHT split label.
  const panel0 = fbs[0];
  if (!panel0 || panel0.length < STRIDE * NATIVE_H) {
    console.error(`unexpected panel-0 buffer for ${name}: len=${panel0?.length}`);
    process.exit(1);
  }
  const px = fbToPgm(panel0);
  const out = path.join(OUT, `${name}.pgm`);
  writePgm(out, px, LOGICAL_W, LOGICAL_H);
  console.log(`wrote ${out}`);
}
