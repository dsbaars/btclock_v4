# tools/docs — printable booklet + WebUI screenshot capture

This directory holds two doc-tooling pieces:

1. `make_booklet.sh` (+ `booklet.yaml` / `booklet-header.tex`) — pandoc
   pipeline that turns the markdown under `docs/` into a print-ready
   A5 booklet. Same content as the
   [mkdocs-material site](https://docs.btclock.dev) — different binding.
2. `capture_webui.mjs` — Playwright script that drives a headless
   Chromium against a live BTClock WebUI (defaults to the Vite dev
   server at `http://localhost:5173/`) and writes the screenshot set
   under `docs/img/webui/`.

## Usage — booklet

```bash
tools/docs/make_booklet.sh        # English (default)
tools/docs/make_booklet.sh nl     # Nederlands quickstart edition
tools/docs/make_booklet.sh de     # Deutsch
tools/docs/make_booklet.sh es     # Español
```

Per-language editions only swap the QUICKSTART for the localised
variant — the rest of the docs are English (no full translations exist
for HANDBOOK / SETTINGS / etc. yet, mirroring the live site's
fallback behaviour).

## Outputs

```
docs/build/
├── btclock-booklet.pdf            ← single A5 PDF — the read-on-screen format
├── btclock-booklet.tex            ← intermediate LaTeX (for diffing builds)
└── btclock-booklet-impose.pdf     ← A4 fold-and-staple imposition
```

`btclock-booklet.pdf` is meant for screen reading or single-sided
print. `btclock-booklet-impose.pdf` is the A4 landscape imposition —
print double-sided, fold each sheet down the middle, and staple along
the spine. Page order is computed by `pdfjam --booklet`.

The whole `docs/build/` tree is gitignored.

## Toolchain

| Tool | Purpose | Install |
|---|---|---|
| pandoc | markdown → LaTeX → PDF | `brew install pandoc` |
| xelatex (TeX Live) | PDF engine | `brew install --cask mactex-no-gui` (full) or `brew install --cask basictex` |
| pdfjam (TeX Live) | A4 booklet imposition | bundled with TeX Live; or `tlmgr install pdfjam` |
| Inter (font) | body text | `brew install --cask font-inter` |
| DejaVu Sans Mono | code blocks | bundled on macOS / `brew install --cask font-dejavu` |

If `pdfjam` isn't installed the script still produces the A5 PDF and
just skips the imposition step.

## How it's wired

- `make_booklet.sh` — runs pandoc twice (once for PDF, once for `.tex`)
  with a fixed doc order matching the mkdocs nav.
- `booklet.yaml` — pandoc metadata block: title page, page geometry,
  fonts, link colours.
- `booklet-header.tex` — LaTeX header injected via
  `--include-in-header`: BTClock orange palette, fancyhdr running
  headers, titlesec chapter/section styling, unicode mappings for
  characters DejaVu Mono doesn't cover (₿, →, ✅, …).

The pandoc input dialect is `gfm+yaml_metadata_block` — the same
GitHub-flavoured Markdown the docs already validate against in
mkdocs.

## Style choices

- **A5** (148 × 210 mm) page size — small enough to staple as a
  pocket booklet, large enough that 10pt body text + screen renders
  stay legible.
- `documentclass=report` so each top-level Markdown heading becomes a
  `\chapter` — matches the doc-per-topic structure of the source tree.
- BTClock orange (`#E04300`) on links, chapter titles, section
  titles, and the running-header rule, so the booklet *looks* like
  the firmware.
- Alternating row shading on tables (very useful for the dense
  feature/settings tables).

## Adding a new doc

1. Create the `.md` under `docs/` and add it to the mkdocs `nav`.
2. Add the same path to the `DOC_ORDER` array in `make_booklet.sh`.
3. Run `tools/docs/make_booklet.sh` and re-page through the PDF.

## Why not `mkdocs-with-pdf` or `mkdocs-pdf-export-plugin`?

Both bind to a specific theme/render path inside mkdocs and re-do the
HTML→PDF dance through weasyprint. Pandoc + xelatex gives:

- direct LaTeX output we can tune,
- a true `\chapter`/`\section` hierarchy with proper TOC numbering,
- print-grade typography (microtype, hyphenation, ligatures),
- no dependency on a running mkdocs server.

The trade-off is that pandoc doesn't know about mkdocs-material's
admonitions, tabs, etc. — but the docs intentionally don't use those,
exactly so the markdown round-trips cleanly.

## Usage — WebUI screenshots

```bash
# Default: hits http://localhost:5173/ and writes docs/img/webui/*.png
node tools/docs/capture_webui.mjs

# Override either input or output:
WEBUI_URL=http://btclock-9d5530.local/ node tools/docs/capture_webui.mjs
OUT_DIR=/tmp/shots node tools/docs/capture_webui.mjs
```

Output (eleven PNGs, all under 2000 px on the long axis so they fit
the docs' image-size cap):

```
docs/img/webui/
├── overview.png              ← three-column layout, dark theme
├── overview-light.png        ← same composition, light theme
├── control.png               ← Control card close-up
├── status.png                ← Status card close-up
├── settings.png              ← Settings card close-up (top section)
├── language-menu.png         ← navbar with the language picker open
├── settings-bitaxe.png       ← Bitaxe subsection (Extra features)
├── settings-mining-pool.png  ← Mining-pool subsection
├── settings-nostr-zap.png    ← Nostr / zap subsection
├── settings-light-leds.png   ← Displays and LEDs CollapseCard (whole)
└── settings-dnd.png          ← Extra features prefix → first h5 (DND inputs)
```

The three "deep dive" subsection shots are produced by expanding
*every* CollapseCard inside the Settings card, then flipping the
`bitaxeEnabled` / `miningPoolStats` / `nostrZapNotify` master toggles
on so the dependent fields render. The toggles only mutate local
Svelte state — the WebUI doesn't PATCH NVS until "Save" is clicked,
which the script never does, so capture leaves the device's stored
settings unchanged.

Subsections are cropped via the `<h5>` anchor inside
`#settings-card`. The "Nostr" section's bottom edge stops at the
next CollapseCard toggle button (the System card) instead of the
card boundary, so adjacent sections don't bleed into the screenshot.

Toolchain (one-time):

```bash
mkdir -p /tmp/pw && cd /tmp/pw && npm init -y
npm install --no-save playwright@1.59.1
npx playwright install chromium     # ~336 MB into ~/Library/Caches/ms-playwright
```

The script imports `playwright` from whatever node_modules is on
`NODE_PATH` — easiest is to run it via `node /path/to/btclock_v4/tools/docs/capture_webui.mjs`
from the same `/tmp/pw` directory once Playwright is installed there.

Selectors hit:

- `#control-card` / `#status-card` / `#settings-card` — the actual
  DaisyUI card divs (anchor IDs `#control` / `#status` / `#settings`
  are the column wrappers, which include row padding).
- `[data-language-menu] button[aria-haspopup="listbox"]` — locale toggle.
- `html[data-theme]` — DaisyUI theme attribute, set per pass.

The navbar is `position: sticky` + `z-index: 50`, so per-card element
screenshots would composite the navbar over the card's upper edge.
The script hides `.sticky` for the per-card pass after the
language-menu shot has been captured (the picker lives inside the
navbar). Re-running re-overwrites the same six file names, so commits
look like a clean re-render.
