#!/usr/bin/env python3
"""Decode an ESP-IDF coredump, bypassing the app_elf_sha256 mismatch
check that `espcoredump.py` enforces by default.

When the coredump's embedded `app_elf_sha256` doesn't match the ELF
you pass in, `espcoredump.py` aborts with `'Invalid application image
for coredump: coredump SHA256(X) != app SHA256(Y).'` That guard
catches the common operator error of decoding against the wrong
firmware, but it also blocks legitimate decode attempts when:

  - The build was non-reproducible and the original artifacts are
    gone (every fullclean rebuild changes the .bin SHA, so
    coredump-from-yesterday + ELF-from-today never match).
  - You're decoding a coredump from one commit against a nearby
    commit's ELF (last resort when the original ELF can't be
    rebuilt).

This wrapper monkey-patches the check out and dispatches to the
regular espcoredump CLI with whatever args you pass. The patch
substitutes a stderr warning so you don't lose the diagnostic.

**Caveat that the warning text alone won't cover**: with a mismatched
ELF, only the panic-handler frames (`#0`-`#3`-ish) are structurally
reliable. The deeper frames decode against the wrong symbol table —
function names + line numbers will look plausible but can be off by
hundreds of bytes if the binaries actually diverged at that point.
Trust the panic-handler context; treat the rest as suggestive.

Reproducible builds (`CONFIG_APP_REPRODUCIBLE_BUILD=y`, on by default
in this project since 2026-05-20) eliminate the need for this
bypass in the common case: rebuild the crashing commit, get a
byte-identical ELF, decode cleanly. Use this tool when that's not
an option.

Usage:
    tools/coredump_decode/coredump_decode.py info_corefile \\
        -t elf -c dump.elf build-rev-b/btclock_v4.elf

Anything after the script name is forwarded to espcoredump.py as-is.

Prerequisites:
    source ~/esp/v6.0/esp-idf/export.sh

so the `esp_coredump` Python package + xtensa toolchain are on PATH.
"""

import inspect
import re
import sys
import textwrap

try:
    import esp_coredump.corefile.loader as _loader
    from esp_coredump.__main__ import main as _coredump_main
except ImportError as e:
    sys.stderr.write(
        f"error: esp_coredump module not importable ({e}).\n"
        "Did you `source ~/esp/v6.0/esp-idf/export.sh` first?\n"
    )
    sys.exit(127)


_PATCH_TARGET_METHOD = "_extract_elf_corefile"
_RAISE_PATTERN = re.compile(
    r"raise ESPCoreDumpLoaderError\(\s*"
    r"'Invalid application image for coredump:"
    r" coredump SHA256\(\{\}\) != app SHA256\(\{\}\)\.'\s*"
    r"\.format\(core_sha_trimmed, app_sha_trimmed\)\)",
    re.DOTALL,
)
_REPLACEMENT = (
    "__import__('sys').stderr.write("
    "'WARNING (btclock SHA-bypass): coredump SHA256(' + core_sha_trimmed + "
    "') != app SHA256(' + app_sha_trimmed + '). "
    "Trust only panic-handler frames; deeper backtrace may be "
    "symbol-resolution illusion.' + chr(10))"
)


def _install_bypass() -> None:
    """Re-exec a patched version of _extract_elf_corefile into the
    loader module so the raise becomes a warning. Patching in
    process keeps the on-disk esp_coredump install untouched, so
    parallel invocations and CI builds remain isolated.
    """
    method = getattr(_loader.EspCoreDumpLoader, _PATCH_TARGET_METHOD)
    src = textwrap.dedent(inspect.getsource(method))
    patched_src, n_subs = _RAISE_PATTERN.subn(_REPLACEMENT, src)
    if n_subs == 0:
        sys.stderr.write(
            "warning: SHA-mismatch raise not found in esp_coredump loader.py; "
            "the patch pattern may have drifted with an IDF update. Decode "
            "will still run but the original check stays active.\n"
        )
        return

    namespace = vars(_loader)
    exec(patched_src, namespace)  # noqa: S102 — intentional dynamic eval
    setattr(
        _loader.EspCoreDumpLoader,
        _PATCH_TARGET_METHOD,
        namespace[_PATCH_TARGET_METHOD],
    )


def main() -> int:
    _install_bypass()
    _coredump_main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
