// analyze_bolt.mjs — measure the ink-bbox centering of every panel on
// a wasm-rendered nostr-zap frame. Uses the alpha sidechannel so the
// coords are in LOGICAL space (the same x/y the renderer uses);
// rotations are off the table for the analysis. Pure JS post-processor.
// Run: `node tools/wasm/analyze_bolt.mjs`.

import wasmModule from './dist/btclock_datahandler.js';

const W = 122, H = 250;  // logical 2.13" panel.

function inkBbox(buf) {
  // Alpha buffer is row-major W*H bytes; 0 = white, >=128 = ink.
  let xmin = W, xmax = -1, ymin = H, ymax = -1;
  for (let y = 0; y < H; ++y) {
    const row = y * W;
    for (let x = 0; x < W; ++x) {
      if (buf[row + x] < 128) continue;
      if (x < xmin) xmin = x;
      if (x > xmax) xmax = x;
      if (y < ymin) ymin = y;
      if (y > ymax) ymax = y;
    }
  }
  if (xmax < 0) return null;
  return { xmin, xmax, ymin, ymax,
           cx: (xmin + xmax) / 2, cy: (ymin + ymax) / 2,
           w: xmax - xmin + 1, h: ymax - ymin + 1 };
}

function fmt(n) { return Math.round(n * 10) / 10; }

const Module = await wasmModule();

// Render once first so the font roles get bound (the wasm font loader
// is lazy — Ctx().fonts.icon() before any render returns an empty Font).
const fbs = Module.renderNostrZapAlphaBuffer(21000);

function dump(role, cp, px) {
  const m = Module.getCodepointMetrics(role, cp, px);
  console.log(`  ${role} U+${cp.toString(16)} @ ${px}px:`,
    `xoff=${m.xoff}, yoff=${m.yoff}, w=${m.w}, h=${m.h}, advance=${m.advance}`);
  return m;
}
console.log("Sanity — does GetMetrics return non-zero for known glyphs?");
dump("digit", 0x38, 180);   // '8' from antonio at digit size
dump("label", 0x5A, 54);    // 'Z' from antonio at label size
dump("icon",  0x5A, 130);   // 'Z' (latin Z) at icon size — should be 0 (MDI doesn't have it)
const m = dump("icon", 0xF140B, 130);  // mdi::kIconLightningBolt
const expectedXmin = (W - m.w) / 2;
console.log(`Math says bolt ink_xmin should be (panel_w - m.w)/2 = (${W} - ${m.w})/2 = ${expectedXmin}`);
console.log();

const panelCx = (W - 1) / 2;   // 60.5
const panelCy = (H - 1) / 2;   // 124.5

console.log(`Panel geometry: ${W}×${H} logical  center=(${panelCx}, ${panelCy})`);
for (let i = 0; i < fbs.length; ++i) {
  const bb = inkBbox(fbs[i]);
  if (!bb) { console.log(`panel ${i}: blank`); continue; }
  const dx = bb.cx - panelCx;
  const dy = bb.cy - panelCy;
  console.log(
    `panel ${i}: ink=${bb.w}×${bb.h} bbox=[${bb.xmin}..${bb.xmax}, ${bb.ymin}..${bb.ymax}] ` +
    `center=(${fmt(bb.cx)}, ${fmt(bb.cy)}) offset=(${fmt(dx)}, ${fmt(dy)})`);
}
