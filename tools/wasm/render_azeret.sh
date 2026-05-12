#!/usr/bin/env bash
# Azeret Mono (displaay/Azeret on GitHub) is a contemporary monospace
# typeface. This wrapper script downloads three weights from the
# upstream release tree, subsets them to BTClock's standard glyph
# range, and hands the trio off to render_font_candidates.sh which
# does the WASM rebuild + sample render for each weight in turn.
#
# Outputs:
#   docs/img/fonts/azeret_regular.png
#   docs/img/fonts/azeret_semibold.png
#   docs/img/fonts/azeret_bold.png
#
# Env knobs:
#   AZERET_TMP_DIR       — where to stash the fetched + subset TTFs
#                           (default /tmp/azeret).
#   AZERET_SKIP_DOWNLOAD — non-empty = reuse already-downloaded TTFs
#                           in AZERET_TMP_DIR.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TMP_DIR="${AZERET_TMP_DIR:-/tmp/azeret}"
COMMON_UNICODES="U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF"

mkdir -p "${TMP_DIR}"

if [[ -z "${AZERET_SKIP_DOWNLOAD:-}" ]]; then
  for w in Regular SemiBold Bold; do
    src="${TMP_DIR}/AzeretMono-${w}.ttf"
    if [[ ! -s "${src}" ]]; then
      echo "[fetch] AzeretMono-${w}.ttf"
      curl -sSL -o "${src}" \
        "https://github.com/displaay/Azeret/raw/main/fonts/ttf/AzeretMono-${w}.ttf"
    fi
  done
fi

# pyftsubset usually lives in a pyenv-managed Python (3.12 on this
# host). Only force-add a fallback path if the operator's environment
# doesn't already expose it.
if ! command -v pyftsubset >/dev/null 2>&1; then
  if command -v pyenv >/dev/null 2>&1; then
    PYENV_ROOT="$(pyenv root)"
    for v in 3.12.3 3.12.5 3.11.13 miniconda3-latest; do
      cand="${PYENV_ROOT}/versions/${v}/bin/pyftsubset"
      if [[ -x "${cand}" ]]; then
        export PATH="${PYENV_ROOT}/versions/${v}/bin:${PATH}"
        break
      fi
    done
  fi
fi
if ! command -v pyftsubset >/dev/null 2>&1; then
  echo "pyftsubset not found — pip install fonttools" >&2
  exit 127
fi

for w in Regular SemiBold Bold; do
  src="${TMP_DIR}/AzeretMono-${w}.ttf"
  out="${TMP_DIR}/AzeretMono-${w}-subset.ttf"
  if [[ ! -s "${src}" ]]; then
    echo "Missing source: ${src} (unset AZERET_SKIP_DOWNLOAD to fetch)" >&2
    exit 1
  fi
  echo "[subset] AzeretMono-${w}.ttf -> $(basename "${out}")"
  pyftsubset "${src}" \
    --output-file="${out}" \
    --unicodes="${COMMON_UNICODES}" \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
done

# Swap into the Inter slot (family=2). The slot is arbitrary — every
# AppFonts role pulls from the single TTF currently parked at the
# asset path, so the choice only fixes the `--family` selector value.
exec "${HERE}/render_font_candidates.sh" \
  --font-id azeret \
  --asset components/fonts/assets/Inter.ttf \
  --family 2 \
  --weight regular="${TMP_DIR}/AzeretMono-Regular-subset.ttf" \
  --weight semibold="${TMP_DIR}/AzeretMono-SemiBold-subset.ttf" \
  --weight bold="${TMP_DIR}/AzeretMono-Bold-subset.ttf"
