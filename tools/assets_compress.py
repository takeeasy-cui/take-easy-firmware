#!/usr/bin/env python3
"""Regenerate zlib(9)-compressed embeds for the full-screen 4bpp assets.

The firmware ships splash/lock frames as .bin.z blobs next to their raw
masters in main/assets/ (see main/CMakeLists.txt EMBED_FILES). Run this
manually whenever an original <name>.bin changes:

    python tools/assets_compress.py <assets_dir>

It writes <assets_dir>/<name>.bin.z and reports the shrink ratio.
"""

import sys
import zlib
from pathlib import Path

# cover_default.bin: 默认透明书封（灰8+alpha8 双平面，v2 2026-10-17）。
NAMES = ("loading_4bpp.bin", "lock_4bpp.bin", "cover_default.bin")


def main(assets_dir: str) -> int:
    src = Path(assets_dir)
    for name in NAMES:
        raw_path = src / name
        if not raw_path.is_file():
            print(f"assets_compress: missing source {raw_path}", file=sys.stderr)
            return 1
        raw = raw_path.read_bytes()
        blob = zlib.compress(raw, 9)
        dst = src / (name + ".z")
        dst.write_bytes(blob)
        print(
            f"assets_compress: {name} {len(raw)} -> {len(blob)} "
            f"({100.0 * len(blob) / max(1, len(raw)):.1f}%)"
        )
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
