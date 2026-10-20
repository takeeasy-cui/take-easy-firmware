#!/usr/bin/env python3
"""avgr_gray_convert.py — 推理 AVG（逆转裁判等）原版素材 → 16 级灰阶 PNG。

用法（把你自己提取的原版图按目录放好，然后一条命令）：
  python avgr_gray_convert.py <素材目录> <输出目录>
  目录约定（与固件包根一致，直接输出进包目录）：
    <素材目录>/bg/*.png|jpg|webp    → <输出目录>/bg/*.png     背景（≤684×830 等比）
    <素材目录>/char/*.png|jpg|webp  → <输出目录>/char/*.png   立绘（≤470×640 等比，白底合成）
  其余子目录按原样同名转换（尺寸不裁）。

处理规则（对齐引擎解码管线 book_image_file_grayscale + gc_ui_blit_gray）：
  - 灰度：标准 ITU-R BT.601 亮度 → 量化 16 级（每 16 一档，墨水屏 4bpp 观感）
  - 立绘透明通道：白底合成（避免半透明区在灰度管线里发黑）
  - 尺寸：等比缩放进目标框（不拉伸不裁切；放大不超 2 倍防马赛克）
  - 输出：8bit 灰度 PNG（引擎真机再 16 级抖动上屏）

只需 Pillow：pip install pillow
"""

import sys
from pathlib import Path

from PIL import Image

BG_MAX = (684, 830)
CHAR_MAX = (470, 640)
EXTS = {".png", ".jpg", ".jpeg", ".webp", ".bmp"}
GRAY_LEVELS = 16


def flatten_white_flat(img: Image.Image, flatten_white: bool) -> Image.Image:
    """白底合成（仅透明图）；返回可安全缩放的 RGB 图。"""
    if flatten_white and img.mode in ("RGBA", "LA", "PA"):
        base = Image.new("RGB", img.size, (255, 255, 255))
        base.paste(img.convert("RGBA"), mask=img.convert("RGBA").split()[-1])
        return base
    return img.convert("RGB")


def to_gray16(img: Image.Image, flatten_white: bool) -> Image.Image:
    if flatten_white and img.mode in ("RGBA", "LA", "PA"):
        base = Image.new("RGB", img.size, (255, 255, 255))
        base.paste(img.convert("RGBA"), mask=img.convert("RGBA").split()[-1])
        img = base
    elif flatten_white:
        img = img.convert("RGB")
    gray = img.convert("L")
    step = 256 // GRAY_LEVELS
    return gray.point(lambda v: min(255, (v // step) * step))

def fit(img: Image.Image, box: tuple) -> Image.Image:
    bw, bh = box
    w, h = img.size
    if w <= bw and h <= bh:
        return img if not (w < bw / 2 and h < bh / 2) else img.resize(
            (min(bw, w * 2), min(bh, h * 2)), Image.LANCZOS)
    scale = min(bw / w, bh / h)
    return img.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)


def convert_dir(src: Path, dst: Path, box: tuple, flatten_white: bool) -> tuple:
    n = ok = 0
    for f in sorted(src.iterdir()):
        if f.suffix.lower() not in EXTS:
            continue
        n += 1
        try:
            with Image.open(f) as im:
                im.load()
                # 顺序关键：先白底合成与等比缩放（LANCZOS 平滑），
                # 最后量化 16 档——先量化再缩放会被插值抹回连续灰度。
                base = flatten_white_flat(im, flatten_white)
                out = to_gray16(fit(base, box), False)
                dst.mkdir(parents=True, exist_ok=True)
                out.save(dst / (f.stem + ".png"), optimize=True)
            print(f"  + {f.name}  {out.size[0]}x{out.size[1]}")
            ok += 1
        except Exception as e:
            print(f"  ! 跳过 {f.name}: {e}", file=sys.stderr)
    return n, ok


def main(argv: list) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src_dir, dst_dir = Path(argv[1]), Path(argv[2])
    if not src_dir.is_dir():
        print(f"avgr_gray: 素材目录不存在 {src_dir}", file=sys.stderr)
        return 1
    total = done = 0
    for sub, box, flat in (("bg", BG_MAX, False), ("char", CHAR_MAX, True)):
        d = src_dir / sub
        if d.is_dir():
            print(f"[{sub}] → 16 灰阶（{'白底合成' if flat else '直接灰度'}, ≤{box[0]}×{box[1]}）")
            n, ok = convert_dir(d, dst_dir / sub, box, flat)
            total += n
            done += ok
    # 其余子目录（如道具图 evidence/）：原样转换，不裁尺寸、不白底。
    for d in sorted(src_dir.iterdir()):
        if d.is_dir() and d.name not in ("bg", "char"):
            print(f"[{d.name}] → 16 灰阶（原尺寸）")
            n, ok = convert_dir(d, dst_dir / d.name, (1216, 1216), False)
            total += n
            done += ok
    print(f"avgr_gray: 完成 {done}/{total}"
          f"（包根 {dst_dir}；再跑 game_pkg.py avgr 打包即可）")
    return 0 if total == done else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
