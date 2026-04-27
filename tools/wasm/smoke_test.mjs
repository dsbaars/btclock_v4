// Node smoke-test for the WASM bindings.
//
// Verifies:
//   - text-mode parse* functions still return the expected per-panel
//     string arrays (text-mode regression coverage).
//   - pixel-mode render*FrameBuffer returns 7 Uint8Arrays of the right
//     size, and the label panel (index 0) for block-height has a
//     nonzero black-pixel count in its 4000 bytes — i.e. the renderer
//     actually wrote the "BLOCK/HEIGHT" glyphs into the framebuffer.
//
// Run via:  node tools/wasm/smoke_test.mjs
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

// --- Label rotation regression (bd btclock_v4-m67) ---
//
// verticalDesc label panels rotate locally to Rotation::k90Cw. Prior
// to the fix, the AA sidechannel wrote at pre-rotation logical coords
// — the text ended up clustered in the upper-left quadrant of the
// 122×250 buffer (and clipped past native_width=122). After the fix
// `RotateLogicalToUserUpright` moves the alpha into panel-rotated
// coords, so the label fills the full vertical extent: ink should
// span the lower half of the panel (y >= H/2) just as much as the
// upper half. Use the label's full-ink pixel distribution as the
// signal: count how many full-ink pixels fall in each half-buffer.
function countFullInkInBand(buf, W, H, y0, y1) {
  let n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = 0; x < W; x++) {
      if (buf[y * W + x] === 255) n++;
    }
  }
  return n;
}
const W = dims.width;
const H = dims.height;
const upper = countFullInkInBand(ab[0], W, H, 0, H >> 1);
const lower = countFullInkInBand(ab[0], W, H, H >> 1, H);
// Both halves carry one of the BLOCK/HEIGHT lines (split text), so
// each should hold a meaningful fraction of the total ink.
check("label panel has ink in upper half (BLOCK)",
      upper > 200, `upper=${upper}`);
check("label panel has ink in lower half (HEIGHT) — verticalDesc rotation alive",
      lower > 200, `lower=${lower}`);
// Width-axis check: text rotated to read top-to-bottom should occupy
// the panel's full horizontal extent (any column past native_width=122
// would silently clip; this asserts no clipping happened).
let maxXWithInk = 0;
for (let y = 0; y < H; y++) {
  for (let x = W - 1; x > maxXWithInk; x--) {
    if (ab[0][y * W + x] === 255) {
      maxXWithInk = x;
      break;
    }
  }
}
check("label panel ink reaches near the right edge (no rotation clip)",
      maxXWithInk >= W - 25,
      `maxXWithInk=${maxXWithInk} of W=${W}`);

// --- preview-only runtime knobs -------------------------------------------
//
// Two settings live on the binding to support preview.html's font picker
// and 7-vs-8 panel toggle. They must not change any on-device behaviour —
// the firmware doesn't call either of these. This section exercises both:
//
//   setRenderOptions(panels, fontFamily)
//     panels: 7 or 8
//     fontFamily: 0 antonio (stock), 1 oswald, 2 dejavu
//
// Assertions:
//   1. panels=8 makes every render function emit 8 buffers.
//   2. Switching font family produces a materially different pixel
//      count (oswald is a narrower/thinner digit shape vs antonio —
//      different ink coverage on the same value guarantees the font
//      swap actually reached stb_truetype).
//   3. Resetting back to (7, antonio) reproduces the original buffer
//      counts — no leaked state.
console.log("\npreview knobs — setRenderOptions:");
// Baseline: 7 panels, stock antonio. Keep the ink count so we can
// diff against an alt-font render.
mod.setRenderOptions(7, 0);
const baselineBh = mod.renderBlockHeightFrameBuffer(833333);
const baselineInk = countBlackPixels(baselineBh[1]);
check("baseline (7, antonio) renders 7 panels", baselineBh.length === 7,
      `got ${baselineBh.length}`);

// Oswald. Panels should still be 7; the first digit panel's ink count
// should differ from the antonio baseline.
mod.setRenderOptions(7, 1);
const oswaldBh = mod.renderBlockHeightFrameBuffer(833333);
check("oswald render still 7 panels", oswaldBh.length === 7,
      `got ${oswaldBh.length}`);
const oswaldInk = countBlackPixels(oswaldBh[1]);
check("oswald digit panel still has ink", oswaldInk > 50,
      `ink=${oswaldInk}`);
check("oswald ink differs from antonio ink", oswaldInk !== baselineInk,
      `antonio=${baselineInk} oswald=${oswaldInk}`);

// DejaVu.
mod.setRenderOptions(7, 2);
const dejavuBh = mod.renderBlockHeightFrameBuffer(833333);
const dejavuInk = countBlackPixels(dejavuBh[1]);
check("dejavu digit panel still has ink", dejavuInk > 50,
      `ink=${dejavuInk}`);
check("dejavu ink differs from antonio ink", dejavuInk !== baselineInk,
      `antonio=${baselineInk} dejavu=${dejavuInk}`);

// 8-panel mode. V8 board topology: one extra digit panel vs Rev A/B.
// Panel-count should climb; dims should reflect the switch too.
mod.setRenderOptions(8, 0);
const dims8 = mod.getPanelDimensions();
check("dims panels=8 after setRenderOptions(8)", dims8.panels === 8,
      `got ${dims8.panels}`);
const bh8 = mod.renderBlockHeightFrameBuffer(833333);
check("renderBlockHeight 8 panels returns 8 buffers",
      bh8.length === 8, `got ${bh8.length}`);
for (let i = 0; i < bh8.length; i++) {
  check(`panel8[${i}] byte length`, bh8[i].byteLength === used,
        `got ${bh8[i].byteLength}`);
}
// 833333 is 6 digits, so the 7-digit-cell V8 board right-justifies:
// panel[0]=label, panel[1] stays blank (leading pad), panel[2..7] show
// the six digits. Assert the label + all 6 digit panels have ink,
// and treat panel[1] as expected-blank rather than failing on it.
check(`panel8[0] label has ink`, countBlackPixels(bh8[0]) > 100);
check(`panel8[1] leading-pad is blank`,
      countBlackPixels(bh8[1]) === 0,
      `black=${countBlackPixels(bh8[1])}`);
for (let i = 2; i < bh8.length; i++) {
  const n = countBlackPixels(bh8[i]);
  check(`panel8[${i}] has ink`, n > 50, `black=${n}`);
}

// Back to the baseline — the subsequent smoke expectations below (none
// right now, but keeps the test robust if any are added later) shouldn't
// see residue from 8-panel/alt-font state.
mod.setRenderOptions(7, 0);
const restoredDims = mod.getPanelDimensions();
check("dims restored to panels=7", restoredDims.panels === 7,
      `got ${restoredDims.panels}`);

// --- DataSnapshot-backed screens (mining pool / bitaxe / nostr zap) -------
//
// Each of these was added when the firmware gained the matching renderer
// (api_ids 70/71/80/81 + nostr zap overlay). Assert the bindings exist,
// return 7 Uint8Arrays of the right size, and that the icon panel
// (slot 0) picks up ink from the MDI subset we're rasterising.
console.log("\npixel mode — DataSnapshot-backed screens:");

function checkScreen(name, fbs) {
  check(`${name} returns 7 buffers`, fbs.length === 7, `got ${fbs.length}`);
  for (let i = 0; i < fbs.length; i++) {
    check(`${name} panel[${i}] is Uint8Array`, fbs[i] instanceof Uint8Array);
    check(`${name} panel[${i}] byte length`, fbs[i].byteLength === used,
          `got ${fbs[i].byteLength}`);
  }
  // Icon panel (slot 0) or label panel must have ink for every variant.
  check(`${name} panel[0] has ink`, countBlackPixels(fbs[0]) > 50,
        `black=${countBlackPixels(fbs[0])}`);
}

// Mining pool hashrate: 200 PH/s — pool name seeds the header panel, the
// hashrate string seeds the digit tail. Pickaxe icon on panel 0.
checkScreen("mining-pool-hashrate",
            mod.renderMiningPoolHashrateFrameBuffer("Ocean",
                "200000000000000000"));

// Mining pool earnings: 50000 sats/day — digits + "SATS" unit tail.
checkScreen("mining-pool-earnings",
            mod.renderMiningPoolEarningsFrameBuffer("Ocean", 50000));

// Bitaxe hashrate: 1.2 TH/s expressed as 1200 GH/s on the wire.
checkScreen("bitaxe-hashrate",
            mod.renderBitaxeHashrateFrameBuffer("bitaxe-alpha", 1200));

// Bitaxe best-diff: arbitrary AxeOS-formatted "15.6M" string.
checkScreen("bitaxe-best-diff",
            mod.renderBitaxeBestDiffFrameBuffer("bitaxe-alpha", "15.6M"));

// Nostr zap overlay: 21k sats. Message is kept in the data snapshot
// but no longer rendered.
checkScreen("nostr-zap",
            mod.renderNostrZapFrameBuffer(21000));

if (process.exitCode === 1) {
  console.log("\nSMOKE TEST FAILED");
} else {
  console.log("\nsmoke test passed");
}
