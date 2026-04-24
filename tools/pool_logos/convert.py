#!/usr/bin/env python3
# convert.py — vendor mining-pool logos as a C++ bitmap registry.
#
# Input: the `.bin` files from git.btclock.dev/btclock/mining-pool-logos.
# Each input is a raw 1-bit-per-pixel bitmap, MSB-first, stride =
# ceil(width/8) bytes per row, same format the old Arduino firmware fed
# to Adafruit-GFX's `drawInvertedBitmap()`. A 0 bit means ink (black);
# a 1 bit means background (white).
#
# Output: main/screens/assets/pool_logos.cpp — a C++17 source with a
# `kPoolLogos[]` table the renderer looks up by pool_name. Headers are
# committed so re-running this script is only needed when a logo file
# changes or a new pool ships.
#
# SIZING CONTRACT: logos must fit within a single panel (2.13" EPD is
# 122×250 px). The renderer paints one logo on panel 0 only — panel 1 is
# reserved for the leading digit of the hashrate / earnings value. Any
# new logo over ~122×250 would be clipped by PaintInvertedBitmap's
# letterbox. The existing entries are all single-panel (122×122 square or
# 37×230 portrait) and match v3_fci's `getLogoWidth`/`getLogoHeight`
# overrides.
#
# Usage:
#   python3 tools/pool_logos/convert.py <logos_dir> <output_cpp>
# e.g.:
#   git clone https://git.btclock.dev/btclock/mining-pool-logos /tmp/mp
#   python3 tools/pool_logos/convert.py /tmp/mp \
#       main/screens/assets/pool_logos.cpp

import argparse
import os
import pathlib
import sys
import textwrap

# Pool key -> (input filename, width, height).  The pool key matches
# `DataSnapshot::PoolStats::name` (same string the PoolDataSource sources
# set via `pool_name()`).  Widths/heights come from the old firmware's
# per-pool `getLogoWidth()` / `getLogoHeight()` overrides; kept as a
# small explicit table so a logo file with a mismatched size fails loud.
LOGOS = [
    # key,           filename,            width, height
    ("braiins",      "braiins.bin",       37,    230),
    ("gobrrr_pool",  "gobrrr.bin",        122,   122),
    ("noderunners",  "noderunners.bin",   122,   122),
    ("ocean",        "ocean.bin",         122,   122),
]


def read_bin(path: pathlib.Path, width: int, height: int) -> bytes:
    stride = (width + 7) // 8
    expected = stride * height
    data = path.read_bytes()
    if len(data) != expected:
        sys.exit(f"{path}: expected {expected} bytes ({width}x{height} @ "
                 f"stride={stride}), got {len(data)}")
    return data


def emit_bytes(name: str, data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 16):
        rows.append(", ".join(f"0x{b:02x}" for b in data[i:i+16]))
    body = ",\n    ".join(rows)
    return f"constexpr std::uint8_t {name}[] = {{\n    {body}\n}};\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("logos_dir", type=pathlib.Path,
                    help="directory containing the .bin logo files "
                         "(clone of btclock/mining-pool-logos)")
    ap.add_argument("output_cpp", type=pathlib.Path,
                    help="output .cpp path (pool_logos.cpp)")
    args = ap.parse_args()

    chunks = [textwrap.dedent("""\
        // GENERATED FILE — do not edit by hand.
        // Regenerate via tools/pool_logos/convert.py. See that script for
        // input format and source of truth.
        //
        // Each kLogo*Bitmap is a 1-bpp MSB-first byte array with
        // stride=ceil(width/8) bytes/row. Semantics match the v3 Arduino
        // firmware's `drawInvertedBitmap`: a 0 bit is ink (black pixel),
        // a 1 bit is background (no ink / white).

        #include "screens/assets/pool_logos.hpp"

        #include <cstdint>
        #include <cstring>

        namespace btclock {
        namespace pool_logos {
        namespace {
        """)]

    registry_rows = []
    for key, filename, w, h in LOGOS:
        path = args.logos_dir / filename
        data = read_bin(path, w, h)
        sym = "kLogo_" + key.replace("-", "_")
        chunks.append(emit_bytes(sym + "Bitmap", data))
        registry_rows.append(
            f"    {{\"{key}\", {sym}Bitmap, sizeof({sym}Bitmap), "
            f"{w}, {h}}},")

    chunks.append("\n"
                  "constexpr PoolLogo kLogosTable[] = {\n"
                  + "\n".join(registry_rows) + "\n"
                  "};\n"
                  "\n"
                  "}  // namespace\n"
                  "\n")

    chunks.append(textwrap.dedent("""\
        // Case-insensitive equality restricted to ASCII letters — the pool
        // keys above are all lowercase ASCII, so a 'Z'->'z' fold is all we
        // need. Keeping the comparison local (rather than depending on
        // <cctype>'s locale) keeps it safe for the EPD renderer hot path.
        static bool IEquals(const char* a, const char* b) {
          while (*a || *b) {
            char ca = *a++;
            char cb = *b++;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
            if (ca != cb) return false;
          }
          return true;
        }

        const PoolLogo* Lookup(const std::string& pool_name) {
          if (pool_name.empty()) return nullptr;
          for (const auto& entry : kLogosTable) {
            if (IEquals(entry.key, pool_name.c_str())) return &entry;
          }
          return nullptr;
        }

        }  // namespace pool_logos
        }  // namespace btclock
        """))

    args.output_cpp.parent.mkdir(parents=True, exist_ok=True)
    args.output_cpp.write_text("".join(chunks))
    print(f"wrote {args.output_cpp} "
          f"({sum(len(c) for c in chunks)} chars, "
          f"{len(LOGOS)} logos)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
