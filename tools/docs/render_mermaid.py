#!/usr/bin/env python3
"""Pre-render ```mermaid fenced blocks in a markdown file to PNGs.

The MkDocs site renders mermaid client-side via Material's bundled JS,
but pandoc/xelatex can't run JavaScript, so the booklet build needs the
diagrams baked into static images. This script walks a markdown file,
extracts every ```mermaid block, runs mermaid-cli (`mmdc`) against each
extracted .mmd file, and writes a copy of the markdown with each fence
replaced by an ![](image) reference.

Usage:
    render_mermaid.py <in.md> <out.md> <img_dir> <img_prefix> <mmdc_cmd>

The mmdc command is whatever invocation succeeds in the caller's env
— typically `mmdc` if installed, or
`npx -y --package=@mermaid-js/mermaid-cli mmdc`.

If a render fails (network down, puppeteer unhappy), the fence is left
in place as a code block tagged "mermaid (render failed)" so the rest
of the booklet still builds.
"""
from __future__ import annotations

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def render_block(content: str, out_png: Path, mmdc_cmd: list[str]) -> bool:
    with tempfile.NamedTemporaryFile(
        "w", suffix=".mmd", delete=False, encoding="utf-8"
    ) as fp:
        fp.write(content)
        mmd_path = Path(fp.name)
    try:
        cmd = mmdc_cmd + [
            "-i", str(mmd_path),
            "-o", str(out_png),
            "-b", "transparent",
            "--width", "1600",
            "--scale", "2",
        ]
        proc = subprocess.run(
            cmd, capture_output=True, text=True, check=False
        )
        if proc.returncode != 0:
            sys.stderr.write(
                f"[render_mermaid] mmdc failed for {out_png.name}:\n"
                f"  cmd: {shlex.join(cmd)}\n"
                f"  stderr: {proc.stderr.strip()}\n"
            )
            return False
        return out_png.exists() and out_png.stat().st_size > 0
    finally:
        mmd_path.unlink(missing_ok=True)


def main() -> int:
    if len(sys.argv) < 6:
        sys.stderr.write(__doc__)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    img_dir = Path(sys.argv[3])
    img_prefix = sys.argv[4]
    mmdc_cmd = shlex.split(sys.argv[5])

    img_dir.mkdir(parents=True, exist_ok=True)
    dst.parent.mkdir(parents=True, exist_ok=True)

    lines = src.read_text(encoding="utf-8").splitlines(keepends=False)
    out: list[str] = []
    in_block = False
    block_buf: list[str] = []
    block_idx = 0

    for line in lines:
        stripped = line.strip()
        if not in_block:
            if stripped == "```mermaid":
                in_block = True
                block_buf = []
                continue
            out.append(line)
            continue

        # in_block
        if stripped == "```":
            block_idx += 1
            png_name = f"{img_prefix}_{block_idx:03d}.png"
            png_path = img_dir / png_name
            content = "\n".join(block_buf) + "\n"
            ok = render_block(content, png_path, mmdc_cmd)
            if ok:
                # img_dir is sibling of dst; reference relatively.
                rel = os.path.relpath(png_path, dst.parent)
                out.append(f"![Diagram {block_idx}]({rel})")
                out.append("")
            else:
                out.append("```text")
                out.append(f"[mermaid diagram {block_idx} — render failed]")
                out.extend(block_buf)
                out.append("```")
            in_block = False
            block_buf = []
            continue

        block_buf.append(line)

    if in_block:
        sys.stderr.write(
            "[render_mermaid] reached EOF inside a ```mermaid block — "
            "input likely truncated\n"
        )
        return 1

    dst.write_text("\n".join(out) + "\n", encoding="utf-8")
    sys.stderr.write(
        f"[render_mermaid] {src.name}: {block_idx} diagram(s) → {img_dir}\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
