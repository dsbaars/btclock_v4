// Render a single block-height font sample in the docs PCB/chrome style.
// Useful for quick A/B font comparisons without regenerating every docs PNG.
//
// Usage:
//   node tools/wasm/render_font_sample.mjs \
//     --family 2 \
//     --out docs/img/fonts/inter_regular_candidate.png

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "..", "..");
const { default: sharp } = await import(
  resolve(REPO, "data/node_modules/sharp/lib/index.js")
);
const { default: createBtclockModule } = await import(
  resolve(HERE, "dist/btclock_datahandler.js")
);

function argValue(flag, fallback = undefined) {
  const idx = process.argv.indexOf(flag);
  if (idx === -1 || idx + 1 >= process.argv.length) return fallback;
  return process.argv[idx + 1];
}

const family = Number(argValue("--family", "0"));
const outPath = argValue("--out");
const blockHeight = Number(argValue("--height", "897654"));
const panels = Number(argValue("--panels", "7")) === 8 ? 8 : 7;
const verticalDesc = argValue("--vertical-desc", "true") !== "false";
const inverted = argValue("--inverted", "false") === "true";

if (!Number.isInteger(family) || family < 0 || !outPath) {
  console.error(
    "Usage: node tools/wasm/render_font_sample.mjs --family <int> --out <path> [--height n] [--panels 7|8] [--vertical-desc true|false] [--inverted true|false]",
  );
  process.exit(2);
}

const FRAME_W_MM = 219.6;
const FRAME_H_MM = 81.25;
const PCB_INSET_MM = 0;
const PCB_W_MM = FRAME_W_MM - 2 * PCB_INSET_MM;
const PCB_H_MM = FRAME_H_MM - 2 * PCB_INSET_MM;
const SIDE_MARGIN_MM = 5;
const PANEL_W_MM = 24;
const PANEL_H_MM = 48;
const PANEL_R_MM = 1.5;
const PANEL_BEZEL_MM = 0.8;
const SCREW_INSET_MM = 4.5;
const SCREW_R_MM = 0.95;
const PX_PER_MM = 8;
const VB_PAD_MM = 2;
const OUTPUT_W_PX = Math.round((FRAME_W_MM + 2 * VB_PAD_MM) * PX_PER_MM);
const OUTPUT_H_PX = Math.round((FRAME_H_MM + 2 * VB_PAD_MM) * PX_PER_MM);
const PANEL_BG_RGB = [0xda, 0xdb, 0xde];
const PANEL_INK_RGB = [0x10, 0x10, 0x10];
const PANEL_BG_RGB_INVERTED = [0x10, 0x10, 0x10];
const PANEL_INK_RGB_INVERTED = [0xda, 0xdb, 0xde];

const WORDMARK_UPM = 1000;
const WORDMARK_ADVANCE = 3701;
const WORDMARK_PATH_D =
  "M289 -8Q238 -8 181.5 -3.5Q125 1 81 11L242 682Q287 692 337.0 696.0Q387 700 438 700Q480 700 523.5 694.0Q567 688 602.0 670.5Q637 653 659.5 622.5Q682 592 682 544Q682 491 652.5 444.0Q623 397 553 368Q597 350 621.0 316.0Q645 282 645 237Q645 120 553.5 56.0Q462 -8 289 -8ZM278 313 227 100Q241 98 264.0 96.5Q287 95 315 95Q348 95 384.0 100.0Q420 105 449.0 119.0Q478 133 497.5 159.0Q517 185 517 226Q517 264 488.0 288.5Q459 313 392 313ZM302 410H398Q429 410 458.0 416.0Q487 422 508.5 435.5Q530 449 543.0 469.0Q556 489 556 518Q556 564 521.5 581.0Q487 598 429 598Q406 598 383.0 596.5Q360 595 346 593ZM1363 693 1337 587H1131L990 0H864L1005 587H799L824 693ZM1816 30Q1783 13 1732.0 -1.0Q1681 -15 1608 -15Q1544 -15 1494.0 4.5Q1444 24 1409.0 61.0Q1374 98 1355.5 150.0Q1337 202 1337 267Q1337 351 1365.0 430.5Q1393 510 1446.0 572.0Q1499 634 1575.0 671.5Q1651 709 1747 709Q1810 709 1856.5 695.0Q1903 681 1938 661L1890 560Q1858 579 1821.5 589.5Q1785 600 1739 600Q1676 600 1626.0 572.0Q1576 544 1540.5 498.5Q1505 453 1486.5 395.0Q1468 337 1468 277Q1468 184 1508.5 139.5Q1549 95 1627 95Q1687 95 1729.0 108.5Q1771 122 1801 136ZM2095 -10Q2048 -9 2015.5 2.0Q1983 13 1963.5 32.0Q1944 51 1935.5 77.0Q1927 103 1927 134Q1927 166 1934.5 201.5Q1942 237 1951 272L2067 756L2193 776Q2162 645 2131.0 515.5Q2100 386 2068 255Q2063 234 2057.0 213.5Q2051 193 2051 173Q2051 156 2051.5 141.5Q2052 127 2059.5 116.5Q2067 106 2082.0 99.0Q2097 92 2121 90ZM2408 -14Q2315 -14 2266.0 41.0Q2217 96 2217 186Q2217 244 2235.0 306.0Q2253 368 2290.0 419.0Q2327 470 2383.0 503.0Q2439 536 2515 536Q2608 536 2657.0 481.5Q2706 427 2706 337Q2706 278 2688.5 216.0Q2671 154 2634.5 103.0Q2598 52 2541.5 19.0Q2485 -14 2408 -14ZM2500 434Q2461 434 2430.5 411.5Q2400 389 2379.5 354.0Q2359 319 2348.0 277.5Q2337 236 2337 197Q2337 147 2356.0 118.0Q2375 89 2423 89Q2462 89 2492.5 111.5Q2523 134 2543.5 168.5Q2564 203 2575.0 245.0Q2586 287 2586 325Q2586 375 2567.0 404.5Q2548 434 2500 434ZM2780 198Q2780 266 2801.5 327.5Q2823 389 2863.0 435.5Q2903 482 2961.0 509.0Q3019 536 3093 536Q3132 536 3164.5 529.5Q3197 523 3226 510L3183 413Q3165 421 3143.0 427.0Q3121 433 3089 433Q3046 433 3011.5 415.5Q2977 398 2953.0 368.0Q2929 338 2916.0 297.0Q2903 256 2903 210Q2903 184 2908.5 162.5Q2914 141 2926.5 124.5Q2939 108 2961.0 99.0Q2983 90 3016 90Q3050 90 3081.5 99.0Q3113 108 3130 117L3140 18Q3117 7 3079.0 -3.5Q3041 -14 2993 -14Q2938 -14 2898.0 2.5Q2858 19 2831.5 47.5Q2805 76 2792.5 114.5Q2780 153 2780 198ZM3413 324Q3444 348 3476.0 374.5Q3508 401 3536.5 427.5Q3565 454 3590.0 478.5Q3615 503 3633 523H3772Q3748 495 3717.0 464.0Q3686 433 3652.0 402.0Q3618 371 3582.0 341.0Q3546 311 3512 284Q3537 258 3562.0 223.0Q3587 188 3610.0 150.0Q3633 112 3652.0 73.0Q3671 34 3683 0H3549Q3535 33 3518.0 67.0Q3501 101 3481.0 133.5Q3461 166 3439.0 194.5Q3417 223 3395 245L3335 0H3215L3396 756L3522 776Z";

function panelGutterMm(n) {
  return (PCB_W_MM - 2 * SIDE_MARGIN_MM - n * PANEL_W_MM) / (n - 1);
}

function panelOriginMm(i, gutterMm) {
  const xMm = PCB_INSET_MM + SIDE_MARGIN_MM + i * (PANEL_W_MM + gutterMm);
  const yMm = PCB_INSET_MM + (PCB_H_MM - PANEL_H_MM) / 2;
  return { xMm, yMm };
}

function wordmarkPath(cxMm, baselineMm, fontSizeMm) {
  const s = fontSizeMm / WORDMARK_UPM;
  const widthMm = WORDMARK_ADVANCE * s;
  const xMm = cxMm - widthMm / 2;
  return `<g transform="translate(${xMm} ${baselineMm}) scale(${s} ${-s})">
    <path d="${WORDMARK_PATH_D}" fill="url(#enigGold)"/>
  </g>`;
}

function buildFrameBackgroundSvg(nPanels, opts = {}) {
  const panelFill = opts.inverted ? "#101010" : "#dadbde";
  const gutterMm = panelGutterMm(nPanels);
  const goldRingW = 0.6;
  const panelsSvg = Array.from({ length: nPanels }, (_, i) => {
    const { xMm, yMm } = panelOriginMm(i, gutterMm);
    const blackGapMm = 0.4;
    const offset = goldRingW / 2 + blackGapMm;
    const ringX = xMm - offset;
    const ringY = yMm - offset;
    const ringW = PANEL_W_MM + 2 * offset;
    const ringH = PANEL_H_MM + 2 * offset;
    return `
    <rect x="${xMm}" y="${yMm}" width="${PANEL_W_MM}" height="${PANEL_H_MM}"
          rx="${PANEL_R_MM}" ry="${PANEL_R_MM}" fill="${panelFill}"/>
    <rect x="${ringX}" y="${ringY}" width="${ringW}" height="${ringH}"
          rx="${PANEL_R_MM + offset}" ry="${PANEL_R_MM + offset}"
          fill="none" stroke="url(#enigGold)" stroke-width="${goldRingW}"
          stroke-linejoin="round"/>`;
  }).join("");

  const screws = [
    [SCREW_INSET_MM, SCREW_INSET_MM],
    [FRAME_W_MM - SCREW_INSET_MM, SCREW_INSET_MM],
    [SCREW_INSET_MM, FRAME_H_MM - SCREW_INSET_MM],
    [FRAME_W_MM - SCREW_INSET_MM, FRAME_H_MM - SCREW_INSET_MM],
  ]
    .map(
      ([cx, cy]) => `<circle cx="${cx}" cy="${cy}" r="${SCREW_R_MM}" fill="#dadbde"/>`,
    )
    .join("\n  ");

  return `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg"
     viewBox="${-VB_PAD_MM} ${-VB_PAD_MM} ${FRAME_W_MM + 2 * VB_PAD_MM} ${FRAME_H_MM + 2 * VB_PAD_MM}"
     width="${OUTPUT_W_PX}" height="${OUTPUT_H_PX}">
  <defs>
    <linearGradient id="enigGold" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#f1c64a"/>
      <stop offset="100%" stop-color="#d9aa2c"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="${FRAME_W_MM}" height="${FRAME_H_MM}" fill="#0a0a0a"/>
  ${panelsSvg}
  ${wordmarkPath(FRAME_W_MM / 2, FRAME_H_MM - 7.375, 7.5)}
  ${screws}
</svg>`;
}

function alphaToRgba(alpha, dims, useInverted = false) {
  const bg = useInverted ? PANEL_BG_RGB_INVERTED : PANEL_BG_RGB;
  const ink = useInverted ? PANEL_INK_RGB_INVERTED : PANEL_INK_RGB;
  const out = Buffer.alloc(dims.width * dims.height * 4);
  for (let i = 0; i < dims.width * dims.height; i++) {
    const a = alpha[i];
    const o = i * 4;
    out[o + 0] = ((bg[0] * (255 - a) + ink[0] * a) / 255) | 0;
    out[o + 1] = ((bg[1] * (255 - a) + ink[1] * a) / 255) | 0;
    out[o + 2] = ((bg[2] * (255 - a) + ink[2] * a) / 255) | 0;
    out[o + 3] = 0xff;
  }
  return out;
}

async function panelResampledPng(alpha, dims, dstWPx, dstHPx, useInverted = false) {
  const rgba = alphaToRgba(alpha, dims, useInverted);
  return sharp(rgba, {
    raw: { width: dims.width, height: dims.height, channels: 4 },
  })
    .resize({ width: dstWPx, height: dstHPx, kernel: "mitchell", fit: "fill" })
    .png({ compressionLevel: 9 })
    .toBuffer();
}

async function renderComposite(mod, out, fbs, useInverted = false) {
  const dims = mod.getPanelDimensions();
  const n = fbs.length;
  const gutterMm = panelGutterMm(n);
  const bgSvg = Buffer.from(buildFrameBackgroundSvg(n, { inverted: useInverted }));
  const innerWMm = PANEL_W_MM - 2 * PANEL_BEZEL_MM;
  const innerHMm = PANEL_H_MM - 2 * PANEL_BEZEL_MM;
  const innerWPx = Math.round(innerWMm * PX_PER_MM);
  const innerHPx = Math.round(innerHMm * PX_PER_MM);
  const overlays = [];

  for (let i = 0; i < n; i++) {
    const { xMm: panelX, yMm: panelY } = panelOriginMm(i, gutterMm);
    const xMm = panelX + PANEL_BEZEL_MM;
    const yMm = panelY + PANEL_BEZEL_MM;
    const xPx = Math.round((xMm + VB_PAD_MM) * PX_PER_MM);
    const yPx = Math.round((yMm + VB_PAD_MM) * PX_PER_MM);
    const png = await panelResampledPng(fbs[i], dims, innerWPx, innerHPx, useInverted);
    overlays.push({ input: png, top: yPx, left: xPx });
  }

  await sharp(bgSvg).composite(overlays).png({ compressionLevel: 9 }).toFile(out);
}

const mod = await createBtclockModule();
mod.setRenderOptions(panels, family);
mod.setVerticalDesc(verticalDesc);
const buffers = mod.renderBlockHeightAlphaBuffer(blockHeight);
await renderComposite(mod, outPath, buffers, inverted);
console.log(`[ok] ${outPath}`);
