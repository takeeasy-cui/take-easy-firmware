#!/usr/bin/env python3
"""package_ime.py — take-easy 输入法词库资源打包工具。

产物（默认写到发布站资源目录）：
  1. Pico-ime.bin            — ime 分区完整镜像（TF 卡增量安装包，设备端安装用）：
                               拼音 @0（3,481,419B）+ 0xFF 填充至 0x360000 + 两分 @0x360000
                               （442,698B），总 3,981,642B；固件按固定偏移写入/读取。
  2. web/manifest.json       — 网页「资源导入」清单（esptool-js 写两段，
                               偏移/哈希与 ime 分区布局一致）。

布局常量与固件 main/ime/IME.cpp 一一对应，改动两处需同步：
  IME_PINYIN_BIN_SIZE = 3,481,419
  IME_LF_BASE_OFFSET  = 0x360000
  IME_LF_BIN_SIZE     = 442,698

用法：
  python package_ime.py <ime_table_pinyin.bin> <liangfen.bin> [--out <目录>]
只需 Python 3 标准库。
"""

import hashlib
import sys
from pathlib import Path

PINYIN_SIZE = 3481419
LF_OFFSET = 0x360000
LF_SIZE = 442698
IMAGE_TOTAL = LF_OFFSET + LF_SIZE


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = Path(__file__).resolve().parents[2] / "E站未配置"
    argv = sys.argv[1:]
    if "--out" in argv:
        out = Path(argv[argv.index("--out") + 1])
    else:
        out = Path("resources-out")
    if len(args) < 2:
        print(__doc__)
        return 1

    pinyin_path, lf_path = Path(args[0]), Path(args[1])
    pinyin = pinyin_path.read_bytes()
    lf = lf_path.read_bytes()
    if len(pinyin) != PINYIN_SIZE:
        print(f"[ERR] 拼音码表大小 {len(pinyin)} != 固件常量 {PINYIN_SIZE}")
        return 1
    if len(lf) != LF_SIZE:
        print(f"[ERR] 两分码表大小 {len(lf)} != 固件常量 {LF_SIZE}")
        return 1

    # 1. 分区镜像（TF 增量包）：pinyin + 0xFF 填充 + liangfen
    image = pinyin + b"\xff" * (LF_OFFSET - PINYIN_SIZE) + lf
    assert len(image) == IMAGE_TOTAL, len(image)

    out.mkdir(parents=True, exist_ok=True)
    image_path = out / "Pico-ime.bin"
    image_path.write_bytes(image)

    # 2. 网页清单（两段直写，绝对偏移与固件分区表一致：ime @ 0x410000）
    pinyin_off = 0x410000
    lf_off = pinyin_off + LF_OFFSET
    manifest = {
        "name": "take-easy-ime",
        "version": "1.0.1",
        "kind": "resource",
        "type": "ime-dict",
        "title": "输入法词库",
        "detail": "拼音 + 两分码表 · 约 3.9 MB · 导入后中文输入（拼音 / T9 / 两分）完整可用",
        "install_target": f"partition:ime@0x{pinyin_off:x}",
        "new_install_prompt_erase": False,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "improv": False,
                "parts": [
                    {"path": "ime_table_pinyin.bin", "offset": pinyin_off,
                     "size": len(pinyin), "sha256": sha256(pinyin)},
                    {"path": "liangfen.bin", "offset": lf_off,
                     "size": len(lf), "sha256": sha256(lf)},
                ],
            }
        ],
    }
    import json
    (out / "web-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "ime_table_pinyin.bin").write_bytes(pinyin)
    (out / "liangfen.bin").write_bytes(lf)

    print(f"[OK] Pico-ime.bin      {IMAGE_TOTAL} B  sha256 {sha256(image)}")
    print(f"[OK] ime_table_pinyin  {len(pinyin)} B  sha256 {sha256(pinyin)}")
    print(f"[OK] liangfen          {len(lf)} B  sha256 {sha256(lf)}")
    print(f"[OK] 输出目录: {out.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
