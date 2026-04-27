// Captures BTClock WebUI screenshots from a live dev server (defaults
// to http://localhost:5173/ — override via WEBUI_URL).
//
// Output (under docs/img/webui/, override via OUT_DIR):
//   overview.png             — dark, three-column desktop view
//   overview-light.png       — same composition, light theme
//   control.png              — Control card close-up (dark)
//   status.png               — Status card close-up (dark)
//   settings.png             — Settings card close-up (dark)
//   language-menu.png        — overview top strip with the language picker open
//   settings-bitaxe.png      — Settings → Extra features → Bitaxe subsection
//   settings-mining-pool.png — Settings → Extra features → Mining Pool stats
//   settings-nostr-zap.png   — Settings → Extra features → Nostr
//   settings-light-leds.png  — Settings → Displays and LEDs (whole CollapseCard)
//   settings-dnd.png         — Settings → Extra features (header → first h5)
//
// Selectors used (kept in sync with the WebUI source):
//   #control / #status / #settings              — column anchors (+page.svelte)
//   #control-card / #status-card / #settings-card — actual card boxes,
//     skip the column padding/margin
//   [data-language-menu] button[aria-haspopup="listbox"] — locale toggle
//   html[data-theme]                             — DaisyUI theme attribute
//
// Toolchain (one-time):
//   npm i -D playwright@1.59         # in any tmp dir is fine
//   npx playwright install chromium  # ~336 MB cache under ~/Library
//
// Run:
//   node tools/docs/capture_webui.mjs
//
// Notes:
// - All shots come out at 1× DPR so every PNG stays under the docs'
//   2000-px long-axis budget. The text is still legible because the
//   WebUI uses a system-default 16 px base size at this viewport.
// - The navbar in the WebUI is `position: sticky` + z-50, so per-card
//   element screenshots would otherwise composite the navbar over the
//   card's upper edge. We hide `.sticky` for the per-card pass; the
//   language-menu shot runs *before* the hide because the picker
//   lives inside the navbar.
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const BASE = process.env.WEBUI_URL || 'http://localhost:5173/';
const OUT  = process.env.OUT_DIR  || resolve(
  new URL('.', import.meta.url).pathname, '../../docs/img/webui'
);
mkdirSync(OUT, { recursive: true });

// Wide enough that the three cards stay side-by-side; height capped so
// the overview shot stays under the docs' 2000-px long-axis budget.
const VIEWPORT = { width: 1600, height: 1000 };

// Click every closed CollapseCard inside the Settings card. The
// CollapseCard primitive renders its toggle as a `<button>` with a
// chevron icon — closed has `lucide-chevron-right`, open has
// `lucide-chevron-down`. We just click anything still showing
// `chevron-right` until none remain.
async function openAllCollapseCards(page) {
  for (let pass = 0; pass < 8; pass++) {
    const closed = page.locator(
      '#settings-card button:has(svg.lucide-chevron-right)'
    );
    const n = await closed.count();
    if (n === 0) return;
    for (let i = 0; i < n; i++) {
      // Re-query each time — clicking re-renders and invalidates the locator.
      const btn = page.locator(
        '#settings-card button:has(svg.lucide-chevron-right)'
      ).first();
      if (await btn.count() === 0) break;
      await btn.click();
      await page.waitForTimeout(80);
    }
  }
}

// Flip the master toggles that gate the Bitaxe / mining-pool / Nostr-zap
// subsections so the inner fields render. The switches are SwitchField
// children with `id="bitaxeEnabled"`, `id="miningPoolStats"`,
// `id="nostrZapNotify"` — flip only if currently unchecked.
async function openInnerToggles(page) {
  for (const id of ['bitaxeEnabled', 'miningPoolStats', 'nostrZapNotify']) {
    const sw = page.locator(`#${id}`);
    if (await sw.count() === 0) continue;
    const checked = await sw.isChecked().catch(() => false);
    if (!checked) {
      // SwitchField wraps the input in a label; clicking the label
      // toggles the input. Use the input's parent label for a
      // hit-target that's actually visible.
      await sw.evaluate((el) => (el.closest('label') ?? el).click());
      await page.waitForTimeout(150);
    }
  }
}

// Crop the Settings card from the `<h5>` titled `fromHeader` down to
// (but not including) the `<h5>` titled `toHeader`. Pass `toHeader=null`
// to run to the card's bottom edge. Output is bound to the Settings
// card's horizontal extent so the subsection sits in its natural
// column.
//
// Strategy: scroll the start header to the top of the viewport, then
// take a viewport-relative screenshot whose y starts at 0. If the
// section is taller than the viewport, temporarily grow the viewport
// to fit it (capped at 1900 px to stay inside the docs' size budget).
async function captureSubsection(page, outDir, fileName, fromHeader, toHeader) {
  const bounds = await page.evaluate(({ from, to }) => {
    const card = document.getElementById('settings-card');
    if (!card) return null;
    const headers = Array.from(card.querySelectorAll('h5'));
    const start = headers.find((h) => h.textContent?.trim() === from);
    if (!start) return null;
    const end = to
      ? headers.find((h) => h.textContent?.trim() === to)
      : null;
    // Scroll the start header to the very top of the viewport so the
    // page-screenshot clip can use viewport-relative coordinates.
    start.scrollIntoView({ block: 'start', behavior: 'instant' });
    const cb = card.getBoundingClientRect();
    const sb = start.getBoundingClientRect();
    const yTop = Math.max(0, Math.round(sb.top - 8));
    // Bottom: prefer the next h5 if given. Otherwise stop at the
    // boundary of the CollapseCard the start header lives in — the
    // h5 list inside Extra-features ends with "Nostr", but the card
    // is followed by another CollapseCard ("System") whose content
    // shouldn't appear in a Nostr-zap screenshot.
    let yBotPage;
    if (end) {
      yBotPage = Math.round(end.getBoundingClientRect().top - 4);
    } else {
      const collapseHost = start.closest('[class*="card"], div');
      // Walk up to the CollapseCard wrapper — the closest ancestor
      // that contains a sibling button + an open content div. Easier
      // heuristic: find the next CollapseCard toggle button (any
      // button with a chevron-down icon) AFTER the start header in
      // document order, and stop just above it.
      const allBtns = Array.from(
        document.querySelectorAll(
          '#settings-card button:has(svg.lucide-chevron-down), ' +
          '#settings-card button:has(svg.lucide-chevron-right)'
        )
      );
      const nextBtn = allBtns.find((b) =>
        start.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING
      );
      yBotPage = nextBtn
        ? Math.round(nextBtn.getBoundingClientRect().top - 8)
        : Math.round(cb.bottom + 8);
    }
    const heightPage = yBotPage - sb.top + 8;
    return {
      x: Math.max(0, Math.round(cb.left)),
      y: yTop,
      width: Math.round(cb.width),
      height: Math.max(40, heightPage),
    };
  }, { from: fromHeader, to: toHeader });

  if (!bounds) {
    console.log(`[skip] ${fileName} — header "${fromHeader}" not found`);
    return;
  }

  await growViewportIfNeeded(page, bounds);
  await page.screenshot({ path: resolve(outDir, fileName), clip: bounds });
  console.log(`[ok] ${fileName}`);
}

// Capture an entire CollapseCard by its header text. The CollapseCard
// primitive renders as `<div><button>HEADER</button>{#if open}<div>…</div>{/if}</div>`,
// so the outer `<div>` is the screenshot target — found by walking
// up from the header button.
async function captureCollapseCard(page, outDir, fileName, headerText) {
  const bounds = await page.evaluate((header) => {
    const card = document.getElementById('settings-card');
    if (!card) return null;
    const btn = Array.from(card.querySelectorAll('button')).find(
      (b) => b.textContent?.trim().includes(header)
    );
    if (!btn) return null;
    const wrapper = btn.parentElement;
    if (!wrapper) return null;
    wrapper.scrollIntoView({ block: 'start', behavior: 'instant' });
    const wb = wrapper.getBoundingClientRect();
    return {
      x: Math.max(0, Math.round(wb.left)),
      y: Math.max(0, Math.round(wb.top - 4)),
      width: Math.round(wb.width),
      height: Math.round(wb.height + 8),
    };
  }, headerText);
  if (!bounds) {
    console.log(`[skip] ${fileName} — CollapseCard "${headerText}" not found`);
    return;
  }
  await growViewportIfNeeded(page, bounds);
  await page.screenshot({ path: resolve(outDir, fileName), clip: bounds });
  console.log(`[ok] ${fileName}`);
}

// Capture the prefix of a CollapseCard — from its header button down
// to (but not including) the named `<h5>` inside it. Used for the DND
// inputs that live in Extra-features above the first h5 ("Bitaxe").
async function captureCardPrefix(page, outDir, fileName, cardHeader, stopH5) {
  const bounds = await page.evaluate(({ header, stop }) => {
    const card = document.getElementById('settings-card');
    if (!card) return null;
    const btn = Array.from(card.querySelectorAll('button')).find(
      (b) => b.textContent?.trim().includes(header)
    );
    if (!btn) return null;
    const wrapper = btn.parentElement;
    if (!wrapper) return null;
    const stopEl = Array.from(wrapper.querySelectorAll('h5')).find(
      (h) => h.textContent?.trim() === stop
    );
    if (!stopEl) return null;
    wrapper.scrollIntoView({ block: 'start', behavior: 'instant' });
    const wb = wrapper.getBoundingClientRect();
    const sb = stopEl.getBoundingClientRect();
    const yTop = Math.max(0, Math.round(wb.top - 4));
    const yBot = Math.round(sb.top - 12);
    return {
      x: Math.max(0, Math.round(wb.left)),
      y: yTop,
      width: Math.round(wb.width),
      height: Math.max(40, yBot - yTop),
    };
  }, { header: cardHeader, stop: stopH5 });
  if (!bounds) {
    console.log(`[skip] ${fileName} — could not anchor "${cardHeader}" + "${stopH5}"`);
    return;
  }
  await growViewportIfNeeded(page, bounds);
  await page.screenshot({ path: resolve(outDir, fileName), clip: bounds });
  console.log(`[ok] ${fileName}`);
}

async function growViewportIfNeeded(page, bounds) {
  const targetH = Math.min(1900, bounds.y + bounds.height + 16);
  const vp = page.viewportSize();
  if (vp && targetH > vp.height) {
    await page.setViewportSize({ width: vp.width, height: targetH });
    await page.waitForTimeout(150);
  }
}

async function open(browser, { theme = 'dark' } = {}) {
  const ctx = await browser.newContext({
    viewport: VIEWPORT,
    deviceScaleFactor: 1,
    colorScheme: theme === 'light' ? 'light' : 'dark',
  });
  const page = await ctx.newPage();
  // Vite HMR keeps a websocket open, so `networkidle` never fires.
  // Use `load` + per-element waits.
  await page.goto(BASE, { waitUntil: 'load', timeout: 60000 });
  await page.waitForSelector('#control-card',  { timeout: 30000 });
  await page.waitForSelector('#status-card',   { timeout: 30000 });
  await page.waitForSelector('#settings-card', { timeout: 30000 });
  // Force the requested theme (DaisyUI reads html[data-theme]); the
  // page hydrates with the user's persisted choice otherwise.
  await page.evaluate(
    (t) => document.documentElement.setAttribute('data-theme', t),
    theme,
  );
  // Let the screen tiles paint and the SSE backfill arrive.
  await page.waitForTimeout(2500);
  return { ctx, page };
}

(async () => {
  const browser = await chromium.launch();

  // 1. Dark overview + per-card + language menu.
  {
    const { ctx, page } = await open(browser, { theme: 'dark' });

    await page.screenshot({
      path: resolve(OUT, 'overview.png'),
      clip: { x: 0, y: 0, width: VIEWPORT.width, height: VIEWPORT.height },
    });
    console.log('[ok] overview.png (dark)');

    // Language menu — capture before hiding the navbar (the picker
    // lives inside it).
    const langBtn = page.locator(
      '[data-language-menu] button[aria-haspopup="listbox"]'
    );
    await langBtn.click();
    await page.waitForSelector(
      '[data-language-menu] [role="listbox"]',
      { timeout: 5000 }
    );
    await page.waitForTimeout(200);
    await page.screenshot({
      path: resolve(OUT, 'language-menu.png'),
      clip: { x: 0, y: 0, width: VIEWPORT.width, height: 280 },
    });
    console.log('[ok] language-menu.png');
    await page.keyboard.press('Escape');

    // Per-card close-ups — hide the sticky navbar so it doesn't
    // composite over the card's upper edge.
    await page.addStyleTag({
      content: '.sticky{ display: none !important; }',
    });
    await page.waitForTimeout(200);
    for (const id of ['control', 'status', 'settings']) {
      await page.evaluate(() => window.scrollTo(0, 0));
      await page.waitForTimeout(150);
      const card = page.locator(`#${id}-card`);
      await card.screenshot({ path: resolve(OUT, `${id}.png`) });
      console.log(`[ok] ${id}.png`);
    }

    // Settings deep dives — expand every CollapseCard and flip the
    // three master switches (Bitaxe / mining-pool / Nostr-zap) so the
    // inner fields render. None of this touches NVS: the WebUI's
    // SwitchFields use two-way bindings to the local store and only
    // PATCH on Save (which we never click).
    await openAllCollapseCards(page);
    await openInnerToggles(page);
    await page.waitForTimeout(400);

    await captureSubsection(page, OUT, 'settings-bitaxe.png',
      'Bitaxe', 'Mining Pool stats');
    await captureSubsection(page, OUT, 'settings-mining-pool.png',
      'Mining Pool stats', 'Nostr');
    await captureSubsection(page, OUT, 'settings-nostr-zap.png',
      'Nostr', null);  // null = run to bottom of card

    // Whole CollapseCard "Displays and LEDs" — the LED-strip and
    // frontlight knobs are referenced from HANDBOOK § 10.
    await captureCollapseCard(page, OUT, 'settings-light-leds.png',
      'Displays and LEDs');
    // DND-scheduled inputs sit ABOVE any h5 in the Extra-features
    // card; capture from the card header down to the first h5
    // ("Bitaxe").
    await captureCardPrefix(page, OUT, 'settings-dnd.png',
      'Extra features', 'Bitaxe');

    await ctx.close();
  }

  // 2. Light-mode overview only — the per-card detail shots stay dark
  //    since the dark/light contrast is illustrated by the overview pair.
  {
    const { ctx, page } = await open(browser, { theme: 'light' });
    await page.screenshot({
      path: resolve(OUT, 'overview-light.png'),
      clip: { x: 0, y: 0, width: VIEWPORT.width, height: VIEWPORT.height },
    });
    console.log('[ok] overview-light.png');
    await ctx.close();
  }

  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
