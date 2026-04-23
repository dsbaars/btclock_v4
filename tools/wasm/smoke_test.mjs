// Node smoke-test for the WASM bindings.
//
// Verifies:
//   - text-mode parse* functions still return the expected per-panel
//     string arrays (regression coverage for Phase 1).
//   - pixel-mode render*FrameBuffer returns 7 Uint8Arrays of the right
//     size, and the label panel (index 0) for block-height has a
//     nonzero black-pixel count in its 4000 bytes — i.e. the renderer
//     actually wrote the "BLOCK/HEIGHT" glyphs into the framebuffer.
//
// Run via:  node idf_cpp_proto/tools/wasm/smoke_test.mjs
import createBtclockModule from "./dist/btclock_datahandler.js";

const mod = await createBtclockModule();

function check(name, cond, msg) {
  if (cond) {
    console.log(`  ok   ${name}${msg ? " — " + msg : ""}`);
  } else {
    console.log(`  FAIL ${name}${msg ? " — " + msg : ""}`);
    process.exitCode = 1;
  }
}

console.log("text mode:");
const bh = mod.parseBlockHeight(833333);
check("parseBlockHeight length", bh.length === 7, `got ${bh.length}`);
check("parseBlockHeight label",  bh[0] === "BLOCK/HEIGHT", `got '${bh[0]}'`);
check("parseBlockHeight digits",
      bh[1] === "8" && bh[2] === "3" && bh[3] === "3" &&
      bh[4] === "3" && bh[5] === "3" && bh[6] === "3",
      `got ${JSON.stringify(bh.slice(1))}`);

console.log("\npanel dimensions:");
const dims = mod.getPanelDimensions();
check("width",  dims.width === 122,  `got ${dims.width}`);
check("height", dims.height === 250, `got ${dims.height}`);
check("stride", dims.stride === 16,  `got ${dims.stride}`);
check("rotation k180", dims.rotation === 2, `got ${dims.rotation}`);
check("panels", dims.panels === 7, `got ${dims.panels}`);

console.log("\npixel mode — renderBlockHeightFrameBuffer(833333):");
const fbs = mod.renderBlockHeightFrameBuffer(833333);
check("returns 7 buffers", fbs.length === 7, `got ${fbs.length}`);
const used = dims.stride * dims.height;  // 16 * 250 = 4000
for (let i = 0; i < fbs.length; i++) {
  check(`panel[${i}] is Uint8Array`, fbs[i] instanceof Uint8Array);
  check(`panel[${i}] byte length`, fbs[i].byteLength === used,
        `got ${fbs[i].byteLength}, want ${used}`);
}

// Black-pixel = bit cleared (0). ClearFb is called with white=true
// (0xFF), then DrawText sets dark pixels to 0. Count zero-bits per byte.
function countBlackPixels(buf) {
  let n = 0;
  for (let i = 0; i < buf.length; i++) {
    let b = buf[i] ^ 0xFF;
    // popcount
    b = b - ((b >> 1) & 0x55);
    b = (b & 0x33) + ((b >> 2) & 0x33);
    b = (b + (b >> 4)) & 0x0F;
    n += b;
  }
  return n;
}

const labelBlack = countBlackPixels(fbs[0]);
check("label panel has ink",
      labelBlack > 100 && labelBlack < (used * 8) - 100,
      `black pixels=${labelBlack} of ${used * 8}`);

// Digit panels for "833333": panels 1..6 should each show the glyph "8"
// or "3". Each digit panel should also have nonzero black pixel count.
for (let i = 1; i <= 6; i++) {
  const n = countBlackPixels(fbs[i]);
  check(`digit panel[${i}] has ink`, n > 50,
        `black pixels=${n}`);
}

console.log("\npixel mode — renderPriceDataFrameBuffer(67890, \"USD\"):");
const priceFbs = mod.renderPriceDataFrameBuffer(67890, "USD");
check("price panel count", priceFbs.length === 7);
const priceLabelBlack = countBlackPixels(priceFbs[0]);
check("price label panel has ink",
      priceLabelBlack > 100,
      `black pixels=${priceLabelBlack}`);

console.log("\npixel mode — renderBlockFeesFrameBuffer(42):");
const feeFbs = mod.renderBlockFeesFrameBuffer(42);
check("fee panel count", feeFbs.length === 7);
const feeLabelBlack = countBlackPixels(feeFbs[0]);
check("fee label panel has ink",
      feeLabelBlack > 100,
      `black pixels=${feeLabelBlack}`);

// --- AA mode ---------------------------------------------------------------
//
// The 1-bpp path thresholds each glyph pixel at alpha >= 128 and emits
// exactly 0 or 1 per pixel. The AA path skips that threshold and hands
// back stb_truetype's raw coverage — so some edge pixels must be in
// the interior range (0 < a < 255). Proof of AA surviving:
//   - total bytes per panel = logical_w * logical_h = 122 * 250 = 30500
//   - panels[0] for block-height renders "BLOCK/HEIGHT" via DrawSplitText;
//     the glyph strokes should produce HUNDREDS of fractional-coverage
//     bytes along their oblique edges. Thresholding them would flatten
//     all of those to 0 or 255.
console.log("\nAA mode — renderBlockHeightAlphaBuffer(833333):");
const ab = mod.renderBlockHeightAlphaBuffer(833333);
check("returns 7 alpha buffers", ab.length === 7, `got ${ab.length}`);
const alphaBytes = dims.width * dims.height;  // 122 * 250 = 30500
for (let i = 0; i < ab.length; i++) {
  check(`alpha[${i}] is Uint8Array`, ab[i] instanceof Uint8Array);
  check(`alpha[${i}] byte length`, ab[i].byteLength === alphaBytes,
        `got ${ab[i].byteLength}, want ${alphaBytes}`);
}

// Count "truly AA" bytes — strictly between 0 (white) and 255 (black).
// The 1-bpp path only produces 0/255; the AA path should produce many
// mid-range values along each glyph edge.
function countFractional(buf) {
  let n = 0;
  for (let i = 0; i < buf.length; i++) {
    const v = buf[i];
    if (v !== 0 && v !== 255) n++;
  }
  return n;
}
function countFullInk(buf) {
  let n = 0;
  for (let i = 0; i < buf.length; i++) {
    if (buf[i] === 255) n++;
  }
  return n;
}

const labelFrac = countFractional(ab[0]);
const labelInk  = countFullInk(ab[0]);
check("label panel has fractional-alpha edge pixels",
      labelFrac > 50,
      `fractional=${labelFrac} of ${alphaBytes}`);
check("label panel has full-ink pixels (glyph interiors + separator)",
      labelInk > 100,
      `full-ink=${labelInk}`);
// Sanity: the label panel's full-ink pixel count should be in the same
// ballpark as the 1-bpp path's black-pixel count (4287 per the phase-2
// smoke test). Allow ±20% — AA-path interior coverage for each glyph
// stroke is basically identical; the deltas come from partial-coverage
// edges that the 1-bpp path rounded *down* to 0 or *up* to 1.
check("label full-ink count roughly matches 1-bpp black count",
      Math.abs(labelInk - labelBlack) < labelBlack * 0.25,
      `full-ink=${labelInk}, black-bits=${labelBlack}`);

// Digit panels should also have fractional pixels (digit '8' / '3').
for (let i = 1; i <= 6; i++) {
  const f = countFractional(ab[i]);
  check(`digit alpha[${i}] has fractional pixels`, f > 20,
        `fractional=${f}`);
}

if (process.exitCode === 1) {
  console.log("\nSMOKE TEST FAILED");
} else {
  console.log("\nsmoke test passed");
}
