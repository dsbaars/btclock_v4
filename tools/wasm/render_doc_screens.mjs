// Renders the firmware screens via the WASM build into composited PNGs that
// drop the panel framebuffers inside the BTClock acrylic frame outline. The
// frame outline is taken from the hardware repo (Rev A/B 2 mm acrylic front);
// dimensions below match its mm-space viewBox so the layout is true-to-life.
//
// Renders go through the AA alpha sidechannel (`render*AlphaBuffer`)
// rather than the 1-bpp framebuffer. The AA path hands back
// stb_truetype's pre-threshold coverage in user-upright panel
// orientation — smooth glyph edges and no rotation undo needed (bd
// btclock_v4-m67 wired the verticalDesc label rotation through the
// sidechannel so all panels share the same target orientation).
//
// Output:
//   docs/img/screens/<id>.png   — one composite per rotation/overlay screen
//   docs/img/fonts/<font>.png   — block-height rendered with each font face
//
// Run from repo root:
//   node tools/wasm/render_doc_screens.mjs
//
// Re-run whenever the screen renderers, fonts, or layout change.

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { mkdir, writeFile } from "node:fs/promises";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "..", "..");
// Use the sharp install from the WebUI's node_modules — there's no
// repo-wide package.json otherwise.
const { default: sharp } = await import(resolve(REPO, "data/node_modules/sharp/lib/index.js"));
const { default: createBtclockModule } = await import(resolve(HERE, "dist/btclock_datahandler.js"));

const mod = await createBtclockModule();

// Acrylic frame, in mm-space matching Rev_A_and_B_2mm-acrylic-front.svg
// (viewBox -2 -2 223.7 85.3, content rect 0..219.6 × 0..81.25).
const FRAME_W_MM = 219.6;
const FRAME_H_MM = 81.25;
const FRAME_R_MM = 2;
// PCB fills the canvas: the docs renders mirror the JLCPCB 2D viewer
// look (which shows only the PCB layer, not the acrylic faceplate
// stack-up). Keep the symbol so the existing geometry math reads
// naturally — set to 0 so panels + drill holes anchor to the canvas
// edges directly. The acrylic faceplate render is documented in
// docs/img/frame/Rev_A_and_B_2mm-acrylic-front.svg for readers who
// want the assembly-level view.
const PCB_INSET_MM = 0;
const PCB_W_MM = FRAME_W_MM - 2 * PCB_INSET_MM;
const PCB_H_MM = FRAME_H_MM - 2 * PCB_INSET_MM;
// PCB corner radius — the actual PCB outline is square (no fillet).
// Earlier renders rounded it to match a misread of the JLCPCB 2D
// viewer; the production PCB is sharp-cornered.
const PCB_R_MM = 0;
const SIDE_MARGIN_MM = 5;
// Panel cutout = the visible window of one EPD module on the front
// PCB. GDEY0213B74's active area is 23.7 × 48.55 mm; the faceplate
// cutout that exposes it is essentially the same. Earlier renders
// used 24 × 60, which overshot the cutout by ~12 mm and stretched
// the firmware's 122 × 250 px framebuffer vertically — making digits
// noticeably taller-than-life. The (122/250)≈0.488 source aspect
// matches the (24/48)≈0.5 cutout aspect cleanly.
const PANEL_W_MM = 24;
const PANEL_H_MM = 48;
const PANEL_R_MM = 1.5;
const PANEL_BEZEL_MM = 0.8;
const SCREW_INSET_MM = 4.5;
const SCREW_R_MM = 0.95;

// Per-image scale: at 8 px/mm we get a 1789×682 PNG. Two reasons we
// stay under 2000 px on the long axis:
//   1. multi-image markdown viewers (and some upload pipelines) cap
//      individual images at 2000 px and silently downsize past that;
//   2. the synthetic renders are still crisp at this density — the
//      panel inner area is 178×474 px which carries the AA glyph
//      coverage cleanly without bloating the repo.
const PX_PER_MM = 8;

// Padding around the frame in viewBox space (matches the SVG's -2 -2).
const VB_PAD_MM = 2;

const OUTPUT_W_PX = Math.round((FRAME_W_MM + 2 * VB_PAD_MM) * PX_PER_MM);
const OUTPUT_H_PX = Math.round((FRAME_H_MM + 2 * VB_PAD_MM) * PX_PER_MM);

function panelGutterMm(n) {
  return (PCB_W_MM - 2 * SIDE_MARGIN_MM - n * PANEL_W_MM) / (n - 1);
}

// Origin of panel #i in mm-space. Both buildFrameBackgroundSvg and
// renderComposite must use the same anchor — otherwise the gold
// silkscreen border drifts away from the panel content overlay.
function panelOriginMm(i, gutterMm) {
  const xMm = PCB_INSET_MM + SIDE_MARGIN_MM + i * (PANEL_W_MM + gutterMm);
  const yMm = PCB_INSET_MM + (PCB_H_MM - PANEL_H_MM) / 2;
  return { xMm, yMm };
}

// Light-grey panel window + near-black ink. Matches the JLCPCB 2D
// render aesthetic where the panel cutout reads as bare PCB silk
// against the black soldermask, and lets the e-paper digit ink stay
// fully dark.
const PANEL_BG_RGB = [0xDA, 0xDB, 0xDE];   // matches the SVG panel rect fill
const PANEL_INK_RGB = [0x10, 0x10, 0x10];  // near-black

// Inverted ("white-on-black") palette — the firmware's `invertedColor`
// pref renders the digit panels as white text on a near-black panel
// face. The WASM layer doesn't expose the global-invert flag, so the
// docs renderer flips bg/ink at composite time when the screen opts
// into the inverted variant.
const PANEL_BG_RGB_INVERTED = [0x10, 0x10, 0x10];
const PANEL_INK_RGB_INVERTED = [0xDA, 0xDB, 0xDE];

// Convert a WASM grayscale-coverage buffer (W*H bytes, USER-UPRIGHT
// orientation — no rotation undo needed) into RGBA. Each pixel is a
// linear blend from `bg` (alpha=0, no ink) to `ink` (alpha=255, full
// ink). Pass `inverted=true` to swap the palette for the docs render
// of `invertedColor=true`.
function alphaToRgba(alpha, dims, inverted = false) {
  const bg = inverted ? PANEL_BG_RGB_INVERTED : PANEL_BG_RGB;
  const ink = inverted ? PANEL_INK_RGB_INVERTED : PANEL_INK_RGB;
  const W = dims.width;
  const H = dims.height;
  const out = Buffer.alloc(W * H * 4);
  for (let i = 0; i < W * H; i++) {
    const a = alpha[i];
    const o = i * 4;
    out[o + 0] = ((bg[0] * (255 - a) + ink[0] * a) / 255) | 0;
    out[o + 1] = ((bg[1] * (255 - a) + ink[1] * a) / 255) | 0;
    out[o + 2] = ((bg[2] * (255 - a) + ink[2] * a) / 255) | 0;
    out[o + 3] = 0xFF;
  }
  return out;
}

// Resize a panel alpha buffer directly to its final output pixel size.
// The AA buffer already carries fractional coverage along glyph edges,
// so a fast bilinear stretch suffices — Lanczos would over-sharpen and
// re-introduce ringing on the smooth edges.
async function panelResampledPng(alpha, dims, dstWPx, dstHPx, inverted = false) {
  const rgba = alphaToRgba(alpha, dims, inverted);
  return await sharp(rgba, {
    raw: { width: dims.width, height: dims.height, channels: 4 },
  })
    .resize({
      width: dstWPx,
      height: dstHPx,
      kernel: "mitchell",
      fit: "fill",
    })
    .png({ compressionLevel: 9 })
    .toBuffer();
}

// Background SVG: render the actual hardware stack — a clear acrylic
// faceplate over a slightly inset black PCB. The PCB carries the panel
// cutouts (1 mm-wide gold silkscreen border around each window) and
// the BTClock wordmark; the acrylic adds a thin gloss highlight and
// the four corner mounting holes that go through both layers.
function buildFrameBackgroundSvg(nPanels, opts = {}) {
  const panelFill = opts.inverted ? "#101010" : "#dadbde";
  const gutterMm = panelGutterMm(nPanels);

  // Each panel cutout sits inside the PCB with a thin gold silkscreen
  // border hugging the EPD window. JLCPCB's 2D viewer paints these as
  // a slim ring with a small but visible black-soldermask gap between
  // the panel face and the silkscreen — matching that look here keeps
  // the docs render visually faithful to what the bare PCB shows.
  // Drawn as a separate stroked rect that sits fully OUTSIDE the panel
  // cutout so the panel-content composite (which lands on the cutout
  // rect) doesn't paint over the silkscreen.
  const goldRingW = 0.6;
  const panels = Array.from({ length: nPanels }, (_, i) => {
    const { xMm, yMm } = panelOriginMm(i, gutterMm);
    // Stroke is centered on the rect path. We want a visible black gap
    // between the panel edge and the silkscreen ring — push the
    // centerline far enough out that the stroke's INNER edge sits a
    // ~0.4 mm gap clear of the panel cutout (≈ 3 px at 8 px/mm).
    const blackGapMm = 0.4;
    const offset = goldRingW / 2 + blackGapMm;
    const ringX = xMm - offset;
    const ringY = yMm - offset;
    const ringW = PANEL_W_MM + 2 * offset;
    const ringH = PANEL_H_MM + 2 * offset;
    return `
    <rect x="${xMm}" y="${yMm}" width="${PANEL_W_MM}" height="${PANEL_H_MM}"
          rx="${PANEL_R_MM}" ry="${PANEL_R_MM}"
          fill="${panelFill}"/>
    <rect x="${ringX}" y="${ringY}" width="${ringW}" height="${ringH}"
          rx="${PANEL_R_MM + offset}"
          ry="${PANEL_R_MM + offset}"
          fill="none" stroke="url(#enigGold)"
          stroke-width="${goldRingW}" stroke-linejoin="round"/>`;
  }).join("");

  // Corner mounting through-holes — JLCPCB's 2D viewer renders them
  // as light-grey discs against the black soldermask.
  const screws = [
    [SCREW_INSET_MM, SCREW_INSET_MM],
    [FRAME_W_MM - SCREW_INSET_MM, SCREW_INSET_MM],
    [SCREW_INSET_MM, FRAME_H_MM - SCREW_INSET_MM],
    [FRAME_W_MM - SCREW_INSET_MM, FRAME_H_MM - SCREW_INSET_MM],
  ]
    .map(
      ([cx, cy]) =>
        `<circle cx="${cx}" cy="${cy}" r="${SCREW_R_MM}" fill="#dadbde"/>`,
    )
    .join("\n  ");

  return `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg"
     viewBox="${-VB_PAD_MM} ${-VB_PAD_MM} ${FRAME_W_MM + 2 * VB_PAD_MM} ${FRAME_H_MM + 2 * VB_PAD_MM}"
     width="${OUTPUT_W_PX}" height="${OUTPUT_H_PX}">
  <defs>
    <!-- ENIG gold for silkscreen panel borders and the BTClock
         wordmark. JLCPCB's 2D viewer paints silkscreen as a near-flat
         yellow-gold; a tiny gradient avoids a plasticky flat fill. -->
    <linearGradient id="enigGold" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%"   stop-color="#f1c64a"/>
      <stop offset="100%" stop-color="#d9aa2c"/>
    </linearGradient>
  </defs>

  <!-- Black soldermask PCB filling the entire canvas — mirrors the
       JLCPCB 2D viewer rendering (PCB layer only). -->
  <rect x="0" y="0" width="${FRAME_W_MM}" height="${FRAME_H_MM}"
        rx="${PCB_R_MM}" ry="${PCB_R_MM}"
        fill="#0a0a0a"/>

  ${panels}
  ${/* Wordmark — measured against the JLCPCB 2D viewer. The italic
       silkscreen sits HIGH in the strip below the panel cutouts,
       hugging the panel-bottom edge with most of the empty space
       below the baseline: top of caps ≈ 1.75 mm below panel bottom,
       baseline ≈ 7.375 mm above the PCB bottom edge, cap height
       ≈ 7.5 mm. Earlier renders had the baseline 5.15 mm above the
       bottom (visually centred in the strip — too low vs. the real
       silkscreen) and before that 2 mm with cap height 6.5 mm. */ ''}
  ${wordmarkPath(FRAME_W_MM / 2, FRAME_H_MM - 7.375, 7.5)}

  ${screws}
</svg>`;
}

// "BTClock" wordmark baked from Ubuntu Medium Italic (weight 500, italic)
// — the font JLCPCB silkscreens for its viewer's stock board labels and
// the look the user wants matched. Path is in raw font units (UPM=1000,
// y-up); render it inside a <g transform> that scales to mm and flips
// the Y axis. Baking the path avoids any system-font dependency, so the
// docs render identically on every contributor's machine and in CI.
const WORDMARK_UPM = 1000;
const WORDMARK_ADVANCE = 3701;
const WORDMARK_PATH_D = "M289 -8Q238 -8 181.5 -3.5Q125 1 81 11L242 682Q287 692 337.0 696.0Q387 700 438 700Q480 700 523.5 694.0Q567 688 602.0 670.5Q637 653 659.5 622.5Q682 592 682 544Q682 491 652.5 444.0Q623 397 553 368Q597 350 621.0 316.0Q645 282 645 237Q645 120 553.5 56.0Q462 -8 289 -8ZM278 313 227 100Q241 98 264.0 96.5Q287 95 315 95Q348 95 384.0 100.0Q420 105 449.0 119.0Q478 133 497.5 159.0Q517 185 517 226Q517 264 488.0 288.5Q459 313 392 313ZM302 410H398Q429 410 458.0 416.0Q487 422 508.5 435.5Q530 449 543.0 469.0Q556 489 556 518Q556 564 521.5 581.0Q487 598 429 598Q406 598 383.0 596.5Q360 595 346 593ZM1363 693 1337 587H1131L990 0H864L1005 587H799L824 693ZM1816 30Q1783 13 1732.0 -1.0Q1681 -15 1608 -15Q1544 -15 1494.0 4.5Q1444 24 1409.0 61.0Q1374 98 1355.5 150.0Q1337 202 1337 267Q1337 351 1365.0 430.5Q1393 510 1446.0 572.0Q1499 634 1575.0 671.5Q1651 709 1747 709Q1810 709 1856.5 695.0Q1903 681 1938 661L1890 560Q1858 579 1821.5 589.5Q1785 600 1739 600Q1676 600 1626.0 572.0Q1576 544 1540.5 498.5Q1505 453 1486.5 395.0Q1468 337 1468 277Q1468 184 1508.5 139.5Q1549 95 1627 95Q1687 95 1729.0 108.5Q1771 122 1801 136ZM2095 -10Q2048 -9 2015.5 2.0Q1983 13 1963.5 32.0Q1944 51 1935.5 77.0Q1927 103 1927 134Q1927 166 1934.5 201.5Q1942 237 1951 272L2067 756L2193 776Q2162 645 2131.0 515.5Q2100 386 2068 255Q2063 234 2057.0 213.5Q2051 193 2051 173Q2051 156 2051.5 141.5Q2052 127 2059.5 116.5Q2067 106 2082.0 99.0Q2097 92 2121 90ZM2408 -14Q2315 -14 2266.0 41.0Q2217 96 2217 186Q2217 244 2235.0 306.0Q2253 368 2290.0 419.0Q2327 470 2383.0 503.0Q2439 536 2515 536Q2608 536 2657.0 481.5Q2706 427 2706 337Q2706 278 2688.5 216.0Q2671 154 2634.5 103.0Q2598 52 2541.5 19.0Q2485 -14 2408 -14ZM2500 434Q2461 434 2430.5 411.5Q2400 389 2379.5 354.0Q2359 319 2348.0 277.5Q2337 236 2337 197Q2337 147 2356.0 118.0Q2375 89 2423 89Q2462 89 2492.5 111.5Q2523 134 2543.5 168.5Q2564 203 2575.0 245.0Q2586 287 2586 325Q2586 375 2567.0 404.5Q2548 434 2500 434ZM2780 198Q2780 266 2801.5 327.5Q2823 389 2863.0 435.5Q2903 482 2961.0 509.0Q3019 536 3093 536Q3132 536 3164.5 529.5Q3197 523 3226 510L3183 413Q3165 421 3143.0 427.0Q3121 433 3089 433Q3046 433 3011.5 415.5Q2977 398 2953.0 368.0Q2929 338 2916.0 297.0Q2903 256 2903 210Q2903 184 2908.5 162.5Q2914 141 2926.5 124.5Q2939 108 2961.0 99.0Q2983 90 3016 90Q3050 90 3081.5 99.0Q3113 108 3130 117L3140 18Q3117 7 3079.0 -3.5Q3041 -14 2993 -14Q2938 -14 2898.0 2.5Q2858 19 2831.5 47.5Q2805 76 2792.5 114.5Q2780 153 2780 198ZM3413 324Q3444 348 3476.0 374.5Q3508 401 3536.5 427.5Q3565 454 3590.0 478.5Q3615 503 3633 523H3772Q3748 495 3717.0 464.0Q3686 433 3652.0 402.0Q3618 371 3582.0 341.0Q3546 311 3512 284Q3537 258 3562.0 223.0Q3587 188 3610.0 150.0Q3633 112 3652.0 73.0Q3671 34 3683 0H3549Q3535 33 3518.0 67.0Q3501 101 3481.0 133.5Q3461 166 3439.0 194.5Q3417 223 3395 245L3335 0H3215L3396 756L3522 776Z";

// Emit the wordmark centered horizontally on (cxMm, baselineMm), sized
// so the cap height roughly equals fontSizeMm.
function wordmarkPath(cxMm, baselineMm, fontSizeMm) {
  const s = fontSizeMm / WORDMARK_UPM;
  const widthMm = WORDMARK_ADVANCE * s;
  const xMm = cxMm - widthMm / 2;
  return `<g transform="translate(${xMm} ${baselineMm}) scale(${s} ${-s})">
    <path d="${WORDMARK_PATH_D}" fill="url(#enigGold)"/>
  </g>`;
}

// --- Provisioning UI ---------------------------------------------------------
//
// The first-boot panels are rendered by main/provisioning_ui.cpp using
// Atkinson + qrcodegen. Both surfaces are off-device-renderable in
// principle — the AppFonts table is wired into the WASM build — but the
// `DrawMarkdown` helper isn't currently exposed as a binding, and adding
// one to render an off-device QR would require porting qrcodegen too.
//
// For docs we fake it with native SVG: same panel geometry, real text
// content, and a QR-flavoured placeholder. Documented as such in the
// caption so readers know the on-device QR is generated live and encodes
// their actual provisioning credentials.

// Build the seven inner-panel <g> elements for the provisioning screen.
// Each panel is sized to inner-bezel dimensions so the surrounding white
// rectangle from buildFrameBackgroundSvg shows through as the bezel.
function buildProvisioningPanels(apSsid, apPw, hwName, builtDate, fwVersion) {
  const n = 7;
  const gutterMm = panelGutterMm(n);
  const innerWMm = PANEL_W_MM - 2 * PANEL_BEZEL_MM;
  const innerHMm = PANEL_H_MM - 2 * PANEL_BEZEL_MM;

  const panelOriginsMm = Array.from({ length: n }, (_, i) => {
    const { xMm, yMm } = panelOriginMm(i, gutterMm);
    return { x: xMm + PANEL_BEZEL_MM, y: yMm + PANEL_BEZEL_MM };
  });

  // Auto-shrink: pick the largest fontSize ≤ requested for which every
  // line fits within `maxTextW`. We can't use SVG textLength +
  // lengthAdjust="spacingAndGlyphs" — librsvg (which sharp uses) ignores
  // that hint and renders at the natural size, so the text overflows
  // the panel. Instead we measure with an Atkinson-bold-ish glyph
  // width upper bound and scale uniformly. Mirrors the on-device
  // DrawMarkdown auto-fit (components/fonts/font.cpp).
  const glyphWPerMm = 0.78;  // upper bound for Atkinson-bold mixed case
  const fitFontSize = (lines, requestedSize, maxTextW) => {
    let widest = 0;
    for (const ln of lines) {
      const isBold = ln.startsWith("*") && ln.endsWith("*");
      const clean = isBold ? ln.slice(1, -1) : ln;
      const w = clean.length * glyphWPerMm * requestedSize;
      if (w > widest) widest = w;
    }
    if (widest === 0 || widest <= maxTextW) return requestedSize;
    return (requestedSize * maxTextW) / widest;
  };

  const drawCenter = (
    { x, y },
    text,
    { fontSize = 5, weight = 700, italic = false } = {},
  ) => {
    const lines = text.split("\n");
    const cx = x + innerWMm / 2;
    const maxTextW = innerWMm - 1.5; // 0.75mm padding either side
    const fitted = fitFontSize(lines, fontSize, maxTextW);
    const lineH = fitted * 1.15;
    const startY =
      y + innerHMm / 2 - ((lines.length - 1) * lineH) / 2 + fitted * 0.35;
    return lines
      .map((ln, i) => {
        const isBold = ln.startsWith("*") && ln.endsWith("*");
        const clean = isBold ? ln.slice(1, -1) : ln;
        return `<text x="${cx}" y="${startY + i * lineH}"
                text-anchor="middle"
                font-family="Atkinson Hyperlegible, Helvetica, Arial, sans-serif"
                font-size="${fitted.toFixed(3)}"
                font-weight="${isBold ? 700 : weight}"
                font-style="${italic ? "italic" : "normal"}"
                fill="#222">${clean.replace(/&/g, "&amp;")}</text>`;
      })
      .join("\n      ");
  };

  // Panel 0: "Welcome!" centred large
  // Panel 1: "Bienvenidos!" centred (smaller — longer word)
  // Panel 2: EN setup instructions (multi-line, wrapped)
  // Panel 3: ES setup instructions
  // Panel 4: SSID / Password / Hostname
  // Panel 5: HW / SW / Built
  // Panel 6: WiFi-join QR (placeholder pattern)
  const fragments = [
    drawCenter(panelOriginsMm[0], "Welcome!", { fontSize: 6, weight: 700 }),
    drawCenter(panelOriginsMm[1], "Bienvenidos!", { fontSize: 5.5, weight: 700 }),
    drawCenter(
      panelOriginsMm[2],
      "To setup\nscan QR or\nconnect\nmanually",
      { fontSize: 4.6, weight: 400 },
    ),
    drawCenter(
      panelOriginsMm[3],
      "Para\nconfigurar\nescanear QR\no conectar\nmanualmente",
      { fontSize: 4.0, weight: 400 },
    ),
    drawCenter(
      panelOriginsMm[4],
      `*SSID:*\n${apSsid}\n\n*Password:*\n${apPw}\n\n*Hostname:*\n${apSsid}`,
      { fontSize: 4.0, weight: 400 },
    ),
    drawCenter(
      panelOriginsMm[5],
      // Mirrors the firmware's provisioning_ui.cpp P5 markdown:
      // *HW:* / <hwName> / 2.13" / *SW:* / <fwVersion> / *Built:* / <builtDate>.
      // The "BTClock v4" line that used to sit between *SW:* and the
      // version string was dropped on-device and is dropped here too.
      `*HW:*\n${hwName}\n2.13"\n\n*SW:*\n${fwVersion}\n\n*Built:*\n${builtDate}`,
      { fontSize: 3.6, weight: 400 },
    ),
    buildPlaceholderQrSvg(panelOriginsMm[6], innerWMm, innerHMm),
  ];

  return fragments.join("\n\n");
}

// Placeholder QR — three finder patterns in the corners + a pseudo-random
// noise pattern in the middle, sized to fit inside the panel with a
// quiet zone. Reproducible across runs (deterministic seed).
function buildPlaceholderQrSvg({ x, y }, innerWMm, innerHMm) {
  const modules = 25; // a v3 QR is 29 modules; 25 is small enough to read clean
  const qrSizeMm = Math.min(innerWMm, innerHMm) - 2; // 1mm quiet zone
  const moduleMm = qrSizeMm / modules;
  const x0 = x + (innerWMm - qrSizeMm) / 2;
  const y0 = y + (innerHMm - qrSizeMm) / 2;

  // Linear-congruential pseudo-random — deterministic across runs so the
  // committed PNG doesn't change byte-for-byte every render.
  let seed = 0xC0FFEE;
  const rand = () => {
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
    return seed;
  };

  const finder = (fx, fy) => {
    // 7×7 finder block: outer ring + centre 3×3.
    const cells = [];
    for (let dy = 0; dy < 7; dy++) {
      for (let dx = 0; dx < 7; dx++) {
        const onOuter = dx === 0 || dx === 6 || dy === 0 || dy === 6;
        const onCentre = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
        if (onOuter || onCentre) cells.push([fx + dx, fy + dy]);
      }
    }
    return cells;
  };

  const filled = new Set();
  const add = (cells) => cells.forEach(([cx, cy]) => filled.add(cy * modules + cx));
  add(finder(0, 0));
  add(finder(modules - 7, 0));
  add(finder(0, modules - 7));

  // Noise inside the data region. Skip cells inside the finder patterns.
  const inFinder = (cx, cy) =>
    (cx < 8 && cy < 8) ||
    (cx >= modules - 8 && cy < 8) ||
    (cx < 8 && cy >= modules - 8);

  for (let cy = 0; cy < modules; cy++) {
    for (let cx = 0; cx < modules; cx++) {
      if (inFinder(cx, cy)) continue;
      // ~45 % fill — visually QR-shaped without any decoder mistaking it
      // for a real code.
      if (rand() % 100 < 45) filled.add(cy * modules + cx);
    }
  }

  const rects = [];
  for (let cy = 0; cy < modules; cy++) {
    for (let cx = 0; cx < modules; cx++) {
      if (filled.has(cy * modules + cx)) {
        rects.push(
          `<rect x="${x0 + cx * moduleMm}" y="${y0 + cy * moduleMm}" width="${moduleMm}" height="${moduleMm}" fill="#000"/>`,
        );
      }
    }
  }
  return rects.join("\n      ");
}

async function renderProvisioningComposite(outPath, opts) {
  const apSsid = opts.apSsid ?? "BTClock-A1B2";
  const apPw = opts.apPw ?? "Mq3HpRtV";
  const hwName = opts.hwName ?? "Rev B";
  const builtDate = opts.builtDate ?? "Apr 26 2026";
  // Tag-shaped placeholder. Real device value is whatever `git
  // describe --tags --always --dirty --match "v*"` baked into
  // PROJECT_VER at build time, surfaced via esp_app_desc_t::version.
  const fwVersion = opts.fwVersion ?? "v0.1.0";

  const bgSvg = buildFrameBackgroundSvg(7);
  // Splice the provisioning content in just before the closing </svg> so
  // it lands on top of the white panel rectangles (which are drawn by
  // buildFrameBackgroundSvg).
  const provisioningContent = buildProvisioningPanels(
    apSsid,
    apPw,
    hwName,
    builtDate,
    fwVersion,
  );
  const composedSvg = bgSvg.replace(
    "</svg>",
    `${provisioningContent}\n</svg>`,
  );

  await sharp(Buffer.from(composedSvg))
    .png({ compressionLevel: 9 })
    .toFile(outPath);
}

async function renderComposite(outPath, fbs, opts = {}) {
  const inverted = !!opts.inverted;
  const dims = mod.getPanelDimensions();
  const n = fbs.length;
  const gutterMm = panelGutterMm(n);

  // Build background, rasterise to OUTPUT_W_PX × OUTPUT_H_PX.
  const bgSvg = Buffer.from(buildFrameBackgroundSvg(n, { inverted }));
  let composite = sharp(bgSvg).png();

  // Inner-bound dimensions in pixels — the area inside the white bezel
  // where the panel image actually lands.
  const innerWMm = PANEL_W_MM - 2 * PANEL_BEZEL_MM;
  const innerHMm = PANEL_H_MM - 2 * PANEL_BEZEL_MM;
  const innerWPx = Math.round(innerWMm * PX_PER_MM);
  const innerHPx = Math.round(innerHMm * PX_PER_MM);

  const overlays = [];
  for (let i = 0; i < n; i++) {
    const { xMm: panelX, yMm: panelY } = panelOriginMm(i, gutterMm);
    const xMm = panelX + PANEL_BEZEL_MM;
    const yMm = panelY + PANEL_BEZEL_MM;
    // VB_PAD_MM offset because the SVG viewBox starts at (-2, -2) but the
    // composite uses absolute pixel coords from the rasterised PNG origin.
    const xPx = Math.round((xMm + VB_PAD_MM) * PX_PER_MM);
    const yPx = Math.round((yMm + VB_PAD_MM) * PX_PER_MM);
    const png = await panelResampledPng(
      fbs[i], dims, innerWPx, innerHPx, inverted,
    );
    overlays.push({ input: png, top: yPx, left: xPx });
  }

  await composite.composite(overlays).png({ compressionLevel: 9 }).toFile(outPath);
}

// --- Screen catalogue --------------------------------------------------------

const SCREENS = [
  {
    id: "block_height",
    title: "Block height",
    fn: () => mod.renderBlockHeightAlphaBuffer(923936),
  },
  {
    id: "clock",
    title: "Time",
    // 13:37 on 26/4 — the same date string the provisioning render
    // uses, so the docs telegraph that all the synthetic renders
    // share a moment in time. hideLeadZero=false to show the full
    // HH:MM form most users will see.
    fn: () => mod.renderClockAlphaBuffer(13, 37, 26, 4, false),
  },
  {
    id: "halving_countdown",
    title: "Halving countdown",
    fn: () => mod.renderHalvingCountdownAlphaBuffer(923936, false),
  },
  {
    id: "block_fee_rate",
    title: "Block fee rate",
    fn: () => mod.renderBlockFeesAlphaBuffer(42),
  },
  {
    id: "moscow_time",
    title: "Sats per dollar",
    fn: () => mod.renderSatsPerCurrencyAlphaBuffer(95432, "USD", true),
  },
  {
    id: "btc_price",
    title: "BTC ticker",
    fn: () => mod.renderPriceDataAlphaBuffer(95432, "USD"),
  },
  {
    id: "btc_price_suffix",
    title: "BTC ticker (suffix mode)",
    // Same price as btc_price.png, but with the suffixPrice flag on
    // — collapses "$95,432" into "$95.4k" so the docs can show the
    // compact-mode impact without bumping the price past 7 digits to
    // auto-trigger.
    fn: () =>
      mod.renderPriceDataWithFlagsAlphaBuffer(95432, "USD", true, false),
  },
  {
    id: "btc_price_mow",
    title: "BTC ticker (Million-Of-Watoshis)",
    // mowMode prints the price in MOW units (millions of watoshis per
    // dollar) rather than dollars per BTC; suffix path is implied.
    fn: () =>
      mod.renderPriceDataWithFlagsAlphaBuffer(95432, "USD", true, true, false),
  },
  {
    id: "btc_price_suffix_sharedot",
    title: "BTC ticker (suffix + share dot)",
    // suffixPrice + suffixShareDot — the decimal point folds into the
    // preceding digit cell, freeing one panel for an extra digit.
    fn: () =>
      mod.renderPriceDataWithFlagsAlphaBuffer(95432, "USD", true, false, true),
  },
  {
    id: "block_height_inverted",
    title: "Block height (invertedColor=true)",
    inverted: true,
    fn: () => mod.renderBlockHeightAlphaBuffer(923936),
  },
  {
    id: "moscow_time_no_sats_symbol",
    title: "Sats per dollar (priceSymMode=0)",
    // Moscow-time render with the sats-glyph cell suppressed — matches
    // firmware `priceSymMode` 0 (no marker). Same price as moscow_time.png
    // so the docs can A/B against `priceSymMode=1`.
    fn: () =>
      mod.renderSatsPerCurrencyWithFlagsAlphaBuffer(95432, "USD", false),
  },
  {
    id: "halving_countdown_time",
    title: "Halving countdown (useBlkCountdown=false)",
    // Time-remaining form (years/days/hours/minutes) instead of
    // blocks-remaining.
    fn: () => mod.renderHalvingCountdownWithFlagsAlphaBuffer(923936, false),
  },
  {
    id: "block_fee_rate_decimal",
    title: "Block fee rate (blockFeeDec=true)",
    // Fractional sats/vB — when the value is small enough the layout
    // engine renders a "X.Y" decimal form instead of integer "X".
    fn: () => mod.renderBlockFeesDecimalAlphaBuffer(4.7),
  },
  {
    id: "market_cap",
    title: "Market cap",
    fn: () => mod.renderMarketCapAlphaBuffer(923936, 95432, "USD", false),
  },
  {
    id: "bitcoin_supply",
    title: "Bitcoin supply",
    // big_chars=true → "19.9M" one-char-per-panel form (the default
    // mode the device renders); show_percent=false.
    fn: () => mod.renderBitcoinSupplyAlphaBuffer(923936, true, false),
  },
  {
    id: "bitcoin_supply_percent",
    title: "Bitcoin supply (percent)",
    // show_percent=true → "93.48 %" form, taking precedence over
    // big_chars per RenderBitcoinSupplyScreen's branch order.
    fn: () => mod.renderBitcoinSupplyAlphaBuffer(923936, true, true),
  },
  {
    id: "mining_pool_hashrate",
    title: "Mining pool hashrate",
    // Switched from "Ocean" to "noderunners" — the WASM stub vendors
    // the noderunners 122x122 logo so the pool screen renders with
    // the actual logo bitmap rather than the text-split fallback. The
    // hashrate is a believable Noderunners pool slice circa Apr 2026
    // (~3 PH/s = 3e15 H/s).
    fn: () =>
      mod.renderMiningPoolHashrateAlphaBuffer(
        "noderunners",
        "3000000000000000",
      ),
  },
  {
    id: "mining_pool_earnings",
    title: "Mining pool earnings",
    fn: () => mod.renderMiningPoolEarningsAlphaBuffer("noderunners", 75000),
  },
  {
    id: "bitaxe_hashrate",
    title: "Bitaxe hashrate",
    fn: () => mod.renderBitaxeHashrateAlphaBuffer("bitaxe-alpha", 1500),
  },
  {
    id: "bitaxe_best_diff",
    title: "Bitaxe best difficulty",
    fn: () => mod.renderBitaxeBestDiffAlphaBuffer("bitaxe-alpha", "15.6M"),
  },
  {
    id: "nostr_zap",
    title: "Nostr zap overlay",
    fn: () => mod.renderNostrZapAlphaBuffer(21000),
  },
  {
    id: "debug",
    title: "Debug overlay",
    // Realistic snapshot. fwVersion is a tag-shaped string for the
    // docs render — the on-device value is whatever `git describe
    // --tags --always --dirty --match "v*"` returned at build time
    // (e.g. "v0.1.0", "abc1234", or "abc1234-dirty"). hwName here
    // matches Rev B.
    fn: () =>
      mod.renderDebugAlphaBuffer(
        "192.168.20.97",
        "home-wifi",
        135 * 1024,         // 135 KB free internal heap
        1850 * 1024,        // 1.85 MB free PSRAM
        "Rev B",
        "v0.1.0",
        "Apr 26 2026",
        7 * 3600 + 42 * 60, // 7h 42m uptime
      ),
  },
];

// `family` must match FontFamily's numeric value in main/fonts_app.hpp.
const FONTS = [
  { id: "antonio", title: "Antonio (default)", family: 0 },
  { id: "oswald", title: "Oswald", family: 1 },
  { id: "inter", title: "Inter", family: 2 },
  { id: "sourceSerif", title: "Source Serif", family: 3 },
  { id: "merriweather", title: "Merriweather", family: 4 },
  { id: "bitter", title: "Bitter", family: 5 },
  { id: "atkinson", title: "Atkinson Hyperlegible", family: 6 },
  { id: "antonioSemiBold", title: "Antonio SemiBold", family: 7 },
  { id: "antonioBold", title: "Antonio Bold", family: 8 },
  { id: "oswaldBold", title: "Oswald Bold", family: 9 },
  { id: "interBold", title: "Inter Bold", family: 10 },
  { id: "sourceSerifBold", title: "Source Serif Bold", family: 11 },
  { id: "merriweatherBold", title: "Merriweather Bold", family: 12 },
  { id: "bitterBold", title: "Bitter Bold", family: 13 },
  { id: "atkinsonBold", title: "Atkinson Hyperlegible Bold", family: 14 },
  { id: "openRunde", title: "Open Runde", family: 15 },
  { id: "roboto", title: "Roboto", family: 16 },
  { id: "robotoBold", title: "Roboto Bold", family: 17 },
  { id: "notoSans", title: "Noto Sans", family: 18 },
  { id: "notoSansBold", title: "Noto Sans Bold", family: 19 },
  { id: "ubuntu", title: "Ubuntu", family: 20 },
  { id: "ubuntuBold", title: "Ubuntu Bold", family: 21 },
  { id: "azeret", title: "Azeret Mono", family: 22 },
  { id: "azeretSemiBold", title: "Azeret Mono SemiBold", family: 23 },
];

const generated = [];

async function main() {
  const screensDir = resolve(REPO, "docs/img/screens");
  const fontsDir = resolve(REPO, "docs/img/fonts");
  await mkdir(screensDir, { recursive: true });
  await mkdir(fontsDir, { recursive: true });

  mod.setRenderOptions(7, 0);
  mod.setVerticalDesc(true);

  for (const s of SCREENS) {
    if (s.skip) {
      console.log(`[skip] ${s.id} — no wasm binding`);
      continue;
    }
    const fbs = s.fn();
    const out = resolve(screensDir, `${s.id}.png`);
    await renderComposite(out, fbs, { inverted: !!s.inverted });
    generated.push({ kind: "screen", id: s.id, title: s.title, path: out });
    console.log(`[ok]   ${out}`);
  }

  // verticalDesc OFF variant of the block-height screen, paired with
  // the default verticalDesc-on render to illustrate the label-rotation
  // toggle in the docs.
  {
    mod.setVerticalDesc(false);
    const fbs = mod.renderBlockHeightAlphaBuffer(923936);
    const out = resolve(screensDir, "block_height_horizontal_desc.png");
    await renderComposite(out, fbs);
    generated.push({
      kind: "screen",
      id: "block_height_horizontal_desc",
      title: "Block height (verticalDesc=false)",
      path: out,
    });
    console.log(`[ok]   ${out}`);
    mod.setVerticalDesc(true);
  }

  // Provisioning first-boot screen (Rev A + Rev B both render the 7-panel
  // layout; V8 has an 8th panel that's not rendered here).
  {
    const out = resolve(screensDir, "provisioning_first_boot.png");
    await renderProvisioningComposite(out, {
      apSsid: "BTClock-A1B2",
      apPw: "Mq3HpRtV",
      hwName: "Rev B",
      // __DATE__ + esp_app_get_description()->version on a tagged build.
      builtDate: "May 03 2026",
      fwVersion: "4.0.0-beta.8",
    });
    generated.push({
      kind: "screen",
      id: "provisioning_first_boot",
      title: "Provisioning (first boot)",
      path: out,
    });
    console.log(`[ok]   ${out}`);
  }

  for (const f of FONTS) {
    mod.setRenderOptions(7, f.family);
    const fbs = mod.renderBlockHeightAlphaBuffer(897654);
    const out = resolve(fontsDir, `${f.id}.png`);
    await renderComposite(out, fbs);
    generated.push({ kind: "font", id: f.id, title: f.title, path: out });
    console.log(`[ok]   ${out}`);
  }

  mod.setRenderOptions(7, 0);

  const manifest = {
    rendered_at: new Date().toISOString(),
    px_per_mm: PX_PER_MM,
    panel_geometry_mm: { w: PANEL_W_MM, h: PANEL_H_MM, side_margin: SIDE_MARGIN_MM },
    frame_geometry_mm: { w: FRAME_W_MM, h: FRAME_H_MM, r: FRAME_R_MM },
    items: generated.map((g) => ({
      kind: g.kind,
      id: g.id,
      title: g.title,
      path: `docs/img/${g.kind === "font" ? "fonts" : "screens"}/${g.id}.png`,
    })),
  };
  await writeFile(
    resolve(REPO, "docs/img/MANIFEST.json"),
    JSON.stringify(manifest, null, 2) + "\n",
  );
  console.log(`[ok]   docs/img/MANIFEST.json (${manifest.items.length} items)`);
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
