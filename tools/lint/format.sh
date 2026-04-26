#!/usr/bin/env bash
# Format / verify all C++ sources under the project. Pass `--check` to
# only verify (CI mode); no arg formats in place.
#
# Excludes:
#   - build*/, managed_components/, .git/, .dolt/  (generated / vendored)
#   - tools/  (a few helpers are .cpp but not part of the firmware)
set -euo pipefail

MODE="${1:-write}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "error: $CLANG_FORMAT not found in PATH" >&2
  echo "       brew install clang-format  (or set CLANG_FORMAT=...)" >&2
  exit 127
fi

# Sources to lint. Keep in sync with the directories actually shipped
# in the firmware + host tests. NUL-delimited so paths with spaces stay
# safe; macOS bash 3.2 doesn't have `mapfile`, so xargs is the portable
# pump.
find_sources() {
  find components main test_host \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) \
    -not -path '*/build*/*' \
    -not -path '*/managed_components/*' \
    -not -path '*/vendor/*' \
    -print0
}

count_sources() {
  find_sources | tr -cd '\0' | wc -c | tr -d ' '
}

case "$MODE" in
  --check|check)
    # `--dry-run --Werror` makes clang-format exit nonzero on any diff.
    # Per-file invocation so the failing path is reported clearly.
    failed=0
    while IFS= read -r -d '' f; do
      if ! "$CLANG_FORMAT" --dry-run --Werror "$f" 2>/dev/null; then
        echo "needs reformat: $f"
        failed=$((failed + 1))
      fi
    done < <(find_sources)
    if [[ $failed -gt 0 ]]; then
      echo ""
      echo "$failed file(s) need reformatting. Run: tools/lint/format.sh"
      exit 1
    fi
    echo "ok: $(count_sources) file(s) clean"
    ;;
  write|"")
    find_sources | xargs -0 "$CLANG_FORMAT" -i
    echo "formatted $(count_sources) file(s)"
    ;;
  *)
    echo "usage: $0 [--check]" >&2
    exit 2
    ;;
esac
