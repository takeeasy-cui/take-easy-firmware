#!/usr/bin/env python3
"""Regenerate the take-easy brand asset masters in main/assets/ from the
brand sources (role-for-role swap of the retired fox art, user 2026-10-20).

Inputs (already copied into main/assets/, ASCII names):
  brand_avatar.png  2048x2048 white-bg fox head    -> fox_avatar.bin    82x82 4bpp
  brand_lock.jpg    1600x2848 white-bg brand art  -> loading_4bpp.bin + lock_4bpp.bin
                                                     (684x1216 4bpp; 开机图同时是
                                                      壁纸锁屏默认图，见 sleep.c:93)
  brand_cover.png   1600x2848 transparent version -> cover_default.bin
                                                     (684x1216 灰8+alpha8 双平面)

Firmware conventions (ui_nothing.c nothing_default_avatar82 / ui_kit.c
ui_inflate_4bpp / sleep.c lock_cover_default):
  - 4bpp: two pixels per byte, even x in the HIGH nibble, 0=black 15=white
    (runtime expands nibble*17).
  - full-screen assets rest zlib(9)-compressed: after this script rerun
    `python tools/assets_compress.py main/assets`.
  - cover_default: first W*H bytes = gray plane, next W*H bytes = alpha plane;
    a>=250 overwrites, 0<a<250 linear-blends, 0 keeps the screen beneath.
    21j: edges are hardened (alpha threshold 128, gray 16-level quantized) so
    the overlay draws crisp like the retired fox art instead of blended mush.

The avatar is contain-fitted into the previous fox avatar's content box so the
status bar (38x38 sample of the 82x82) and the settings page keep the same
visual size. Full-screen art is center-cropped to the 684:1216 aspect before
the resize; lock_4bpp is never drawn (v2 keeps it for boot-time inflate
validation) and is refreshed from the same art for consistency.
"""
import sys
from pathlib import Path
from PIL import Image, ImageFilter

W, H = 684, 1216
AV = 82
CONTENT_THRESH = 235          # darker than this counts as art content
# 原版狐狸头像母版的内容框（真值来自 21i 换图前 main/assets/fox_avatar.bin，
# git 65aae05 可溯）。硬编码避免把上一次转换产物误当参考（磁盘文件已被覆盖）。
AVATAR_REF_BOX = (6, 11, 76, 66)


def assets_dir() -> Path:
    return Path(__file__).resolve().parent.parent / "main" / "assets"


def content_bbox(img: Image.Image):
    """Bounding box of non-white pixels (art content)."""
    return img.point(lambda v: 255 if v < CONTENT_THRESH else 0).getbbox()


def expand_4bpp(data: bytes, w: int, h: int) -> Image.Image:
    img = Image.new("L", (w, h), 255)
    px = img.load()
    for y in range(h):
        for x in range(w):
            b = data[y * (w // 2) + x // 2]
            nib = (b >> 4) if x % 2 == 0 else (b & 0x0F)
            px[x, y] = nib * 17
    return img


def pack_4bpp(img: Image.Image) -> bytes:
    w, h = img.size
    assert w % 2 == 0, "odd width cannot pack"
    px = img.load()
    out = bytearray(w * h // 2)
    i = 0
    for y in range(h):
        for x in range(0, w, 2):
            hi = min(15, round(px[x, y] / 17))
            lo = min(15, round(px[x + 1, y] / 17))
            out[i] = (hi << 4) | lo
            i += 1
    return bytes(out)


def center_crop(img: Image.Image) -> Image.Image:
    """Center-crop to the 684:1216 aspect."""
    sw, sh = img.size
    ratio = W / H
    if sh > sw / ratio:
        ch = round(sw / ratio)
        cy0 = (sh - ch) // 2
        return img.crop((0, cy0, sw, cy0 + ch))
    if sw > sh * ratio:
        cw = round(sh * ratio)
        cx0 = (sw - cw) // 2
        return img.crop((cx0, 0, cx0 + cw, sh))
    return img


def convert_avatar(assets: Path) -> None:
    box = AVATAR_REF_BOX
    # 源图背景是透明（透明区 RGB 为黑），先合成到白底再找内容框。
    raw = Image.open(assets / "brand_avatar.png").convert("RGBA")
    white = Image.new("RGBA", raw.size, (255, 255, 255, 255))
    src = Image.alpha_composite(white, raw).convert("L")
    bbox = content_bbox(src)
    if not bbox:
        sys.exit("brand_art_convert: avatar source has no content")
    content = src.crop(bbox)
    if box is None:
        # no previous master to match: keep the source composition as-is.
        canvas = src.resize((AV, AV), Image.LANCZOS)
        print(f"avatar: no previous master, direct resize (content {bbox})")
    else:
        bw, bh = box[2] - box[0], box[3] - box[1]
        scale = min(bw / content.width, bh / content.height)
        nw = max(1, round(content.width * scale))
        nh = max(1, round(content.height * scale))
        resized = content.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("L", (AV, AV), 255)
        cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
        canvas.paste(resized, (cx - nw // 2, cy - nh // 2))
        print(f"avatar: content {bbox} -> fit {nw}x{nh} into old box {box} "
              f"(center {cx},{cy})")
    out = pack_4bpp(canvas)
    assert len(out) == AV * AV // 2
    (assets / "fox_avatar.bin").write_bytes(out)
    print(f"avatar: fox_avatar.bin {len(out)} bytes")


def convert_lock(assets: Path) -> None:
    src = Image.open(assets / "brand_lock.jpg").convert("L")
    src = center_crop(src)
    img = src.resize((W, H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.5))
    out = pack_4bpp(img)
    assert len(out) == W * H // 2
    (assets / "loading_4bpp.bin").write_bytes(out)
    (assets / "lock_4bpp.bin").write_bytes(out)   # 从不绘制，仅开机校验；同图保持一致
    print(f"lock: loading_4bpp.bin + lock_4bpp.bin {len(out)} bytes each")


def convert_cover(assets: Path) -> None:
    src = Image.open(assets / "brand_cover.png").convert("RGBA")
    gray = src.convert("L")            # 601 luma of RGB, alpha ignored
    alpha = src.getchannel("A")
    gpx, apx = gray.load(), alpha.load()
    for y in range(gray.height):       # alpha==0 处灰度归白，缩放边缘不拉黑
        for x in range(gray.width):
            if apx[x, y] == 0:
                gpx[x, y] = 255
    # 21j（真机反馈「边框不清晰」）：旧狐狸图是硬边 alpha（0/255），叠加全部走
    # a≥250 直接覆盖分支，轮廓/文字锐利；BILINEAR 保留的 1~2px 软边在 4bpp 底上
    # 按 alpha 混合→灰糊边。改 LANCZOS + alpha 128 阈值硬边 + 灰度 16 级量化
    # （平涂色归整，边缘利落，与旧图硬边风格对齐；与 4bpp 调色板同词汇）。
    gray = center_crop(gray).resize((W, H), Image.LANCZOS)
    alpha = center_crop(alpha).resize((W, H), Image.LANCZOS)
    alpha = alpha.point(lambda a: 255 if a >= 128 else 0)
    gray = gray.point(lambda g: min(255, round(g / 17) * 17))
    out = gray.tobytes() + alpha.tobytes()
    assert len(out) == W * H * 2
    (assets / "cover_default.bin").write_bytes(out)
    mid = sum(1 for a in alpha.tobytes() if 0 < a < 255)
    opaque = sum(1 for a in alpha.tobytes() if a >= 250)
    print(f"cover: cover_default.bin {len(out)} bytes "
          f"(opaque {opaque}, hard-edge only, soft remnants {mid})")


def main() -> int:
    assets = assets_dir()
    for name in ("brand_avatar.png", "brand_lock.jpg", "brand_cover.png"):
        if not (assets / name).is_file():
            print(f"brand_art_convert: missing source {assets / name}", file=sys.stderr)
            return 1
    convert_avatar(assets)
    convert_lock(assets)
    convert_cover(assets)
    print("done. now run: python tools/assets_compress.py main/assets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
