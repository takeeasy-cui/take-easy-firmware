#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tegame2story.py — 《重返十七岁》(Do-fei/tegame, MIT) → take-easy Story Engine v1 转换器。

流程分两步：
  1. node dump_tegame.mjs（tegame 仓库根）→ out/{scenes,sprites}.json + out/bgs/*.ppm
  2. 本脚本 → story.txt + images/bg/*.png + images/char/*.png

语义映射（严格对齐 tegame game/core/scene.js）：
  - 场景 → * label s_<id>；主链沿 next，其余场景按 id 序附加。
  - goto（选项/行级）全部场内：目标条目 id → s_<scene>__<sub>；找不到则引擎语义=顺序继续（此处告警）。
  - 标题头 bg/palette/cast + 内联覆盖 → 跟踪状态变化时发射 * bg / * char（cast:null → 清槽）。
  - 立绘落位：x 中心 <107→L，<213→C，其余→R（320 宽坐标系）；夜色板用 __n 变体精灵。
  - say:'' → > 旁白；say:名 → * say 名 文；便签 → * say 薄荷便签 …；署名重写 → 过渡旁白。
  - end:true + gameover → * ending 「GAME OVER」；end:true → * goto s_<next>（无 next → * ending）。
  - 选项无 goto → 自动标签 __autoN 指向下一有效条目。

用法：
  python tegame2story.py --tegame <tegame仓库> --out <包目录> [--title 重返十七岁]
"""
import argparse
import json
import os
import struct
import sys
import zlib

# ---------------- PNG 写出（零依赖：灰度 L 与 RGBA） ----------------

def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

def write_gray_png(path: str, w: int, h: int, pixels) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(pixels[y * w:(y + 1) * w])
    png = b'\x89PNG\r\n\x1a\n'
    png += _png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
    png += _png_chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += _png_chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)

def write_rgba_png(path: str, w: int, h: int, pixels) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(pixels[y * w * 4:(y + 1) * w * 4])
    png = b'\x89PNG\r\n\x1a\n'
    png += _png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += _png_chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += _png_chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)

# ---------------- 简易画布（最近邻缩放 + PPM 读入） ----------------

def read_ppm(path: str):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'P6', path
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    assert parts[2] == b'255'
    return w, h, parts[3]

def scale_nearest(src, sw: int, sh: int, dw: int, dh: int, bpp: int):
    dst = bytearray(dw * dh * bpp)
    for y in range(dh):
        sy = y * sh // dh
        row_src = sy * sw * bpp
        row_dst = y * dw * bpp
        for x in range(dw):
            sx = x * sw // dw
            if bpp == 1:
                dst[row_dst + x] = src[row_src + sx]
            else:
                for c in range(bpp):
                    dst[row_dst + x * bpp + c] = src[row_src + sx * bpp + c]
    return dst

def quant16(v: int) -> int:
    return v & 0xF0

# ---------------- 主转换 ----------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tegame', required=True, help='tegame 仓库根（含 out/ 转储）')
    ap.add_argument('--out', required=True, help='输出包目录')
    ap.add_argument('--title', default='重返十七岁')
    args = ap.parse_args()

    tg = args.tegame
    out = args.out
    dump = os.path.join(tg, 'out')
    scenes = json.load(open(os.path.join(dump, 'scenes.json'), encoding='utf-8'))
    sprites = json.load(open(os.path.join(dump, 'sprites.json'), encoding='utf-8'))

    os.makedirs(os.path.join(out, 'images', 'bg'), exist_ok=True)
    os.makedirs(os.path.join(out, 'images', 'char'), exist_ok=True)

    # ------- 图片烘焙 -------
    bg_files = [f for f in os.listdir(os.path.join(dump, 'bgs')) if f.endswith('.ppm')]
    for f in bg_files:
        w, h, body = read_ppm(os.path.join(dump, 'bgs', f))
        assert (w, h) == (320, 240), f
        gray = bytearray(320 * 240)
        for i in range(320 * 240):
            r, g, b = body[i * 3], body[i * 3 + 1], body[i * 3 + 2]
            lum = (r * 299 + g * 587 + b * 114) // 1000
            gray[i] = quant16(lum)
        scaled = scale_nearest(gray, 320, 240, 684, 513, 1)
        write_gray_png(os.path.join(out, 'images', 'bg', f[:-4] + '.png'), 684, 513, scaled)

    # 精灵：rows 字符画 → RGBA ×3 最近邻；day + night(__n) 双变体（夜=反相鬼影）。
    S = 3
    for sid, spr in sprites.items():
        w, h, rows = spr['w'], spr['h'], spr['rows']
        variants = [('', 0x30, 0xEC), ('__n', 0xD8, 0x18)]
        for suffix, ink, paper in variants:
            W, H = w * S, h * S
            px = bytearray(W * H * 4)
            for y in range(H):
                row = rows[y // S]
                for x in range(W):
                    ch = row[x // S]
                    o = (y * W + x) * 4
                    if ch == 'K':
                        px[o] = px[o + 1] = px[o + 2] = ink
                        px[o + 3] = 255
                    elif ch == 'P':
                        px[o] = px[o + 1] = px[o + 2] = paper
                        px[o + 3] = 255
            write_rgba_png(os.path.join(out, 'images', 'char', sid + suffix + '.png'), W, H, px)

    # ------- 剧本线性化 -------
    lines_out = [f'* title {args.title}', '']

    START = 'office'
    order, seen = [], set()
    cur = START
    while cur and cur not in seen and cur in scenes:
        order.append(cur)
        seen.add(cur)
        cur = scenes[cur].get('next')
    for sid in scenes:
        if sid not in seen:
            order.append(sid)
            seen.add(sid)

    def sub_label(scene_id, sub):
        return f's_{scene_id}__{sub}'

    warn = []
    label_defs = set()

    scene_subs = {}
    for sid, sc in scenes.items():
        scene_subs[sid] = {e['id'] for e in sc.get('script', []) if 'id' in e}

    def pos_of(x, w):
        cx = x + w / 2.0
        return 'L' if cx < 107 else ('C' if cx < 213 else 'R')

    def esc_choice_label(s):
        return s.replace('|', '｜')

    for sid in order:
        # 深拷贝（注入 _auto_label 需要）
        sc = scenes[sid]
        script = [dict(e) for e in sc.get('script', [])]
        lines_out.append(f'* label s_{sid}')
        label_defs.add(f's_{sid}')

        st = {'bg': sc.get('bg') or 'blank', 'pal': sc.get('palette') or 'day',
              'cast': sc.get('cast'), 'name': ''}
        emitted = {'bg': None, 'ch': None}

        def emit_bg():
            name = f"{st['bg']}__{st['pal']}"
            if emitted['bg'] != name:
                lines_out.append(f'* bg {name}')
                emitted['bg'] = name

        def emit_cast():
            c = st['cast']
            if c is None:
                if emitted['ch'] != 'none':
                    lines_out.append('* char 0 none C 0')
                    emitted['ch'] = 'none'
                return
            spr_name = c['sprite']
            suffix = '__n' if st['pal'] == 'night' else ''
            key = (spr_name, suffix, c['x'], c['y'])
            if emitted['ch'] != key:
                w = sprites.get(spr_name, {}).get('w', 60)
                lines_out.append(f"* char 0 {spr_name}{suffix} {pos_of(c['x'], w)} 1")
                emitted['ch'] = key

        emit_bg()
        emit_cast()

        auto_n = 0
        i = 0
        n = len(script)
        while i < n:
            e = script[i]
            has_id = 'id' in e
            if has_id:
                lbl = sub_label(sid, e['id'])
                lines_out.append(f'* label {lbl}')
                label_defs.add(lbl)

            if 'choice' in e:
                opts = e.get('options', [])
                parts = []
                pending_auto = None  # (auto_label_name)
                for o in opts:
                    tgt = o.get('goto')
                    if not tgt:
                        auto_n += 1
                        t = sub_label(sid, f'auto{auto_n}')
                        pending_auto = t
                        parts.append(f"{esc_choice_label(o['label'])}->{t}")
                    elif tgt in scene_subs[sid]:
                        parts.append(f"{esc_choice_label(o['label'])}->{sub_label(sid, tgt)}")
                    elif tgt in scenes:
                        parts.append(f"{esc_choice_label(o['label'])}->s_{tgt}")
                    else:
                        warn.append(f"{sid}: 选项 goto '{tgt}' 不可解析，按顺序继续")
                        parts.append(esc_choice_label(o['label']))
                lines_out.append('* choice ' + '|'.join(parts))
                if pending_auto:
                    # 下一轮发射在该条目上
                    if i + 1 < n:
                        script[i + 1]['_auto_label'] = pending_auto
                i += 1
                continue

            if '_auto_label' in e:
                lines_out.append(f"* label {e['_auto_label']}")
                label_defs.add(e['_auto_label'])

            if 'bg' in e:
                st['bg'] = e['bg'] or 'blank'
            if 'palette' in e:
                st['pal'] = e['palette'] or 'day'
            if 'cast' in e:
                st['cast'] = e['cast']
            emit_bg()
            emit_cast()

            txt = e.get('text')
            if 'note' in e:
                nt = e['note']
                lines_out.append(f"* say 薄荷便签 「{nt.get('line', '')}」 —— 署名：{nt.get('sig', '?')}")
            elif 'rewrite' in e:
                lines_out.append('> ——（一张张翻到最后：每一张便签的落款，不知何时都换成了「高晓媛」三个字。）')
            elif txt:
                name = e.get('say', '')
                if name:
                    lines_out.append(f"* say {name} {txt}")
                else:
                    lines_out.append(f"> {txt}")
            elif not has_id and '_auto_label' not in e:
                if e.get('say'):
                    # 无 text 的名牌/氛围行（如 id:OK）→ 只更新粘性名牌
                    lines_out.append(f"* name {e['say']}")

            if 'end' in e and e['end']:
                if e.get('gameover'):
                    lines_out.append('* ending 「GAME OVER」——他没能走出这个夏天。')
                else:
                    nx = e.get('next') or sc.get('next')
                    if nx and nx in scenes:
                        lines_out.append(f"* goto s_{nx}")
                    else:
                        if sid in ('fin_reunion',):
                            lines_out.append('* ending 大团圆 · 十七岁这一年，重新开始了。')
                        elif sid in ('cafe',):
                            lines_out.append('* ending 彩蛋 · 这一刻，被夹进了你的「薄荷便签」。')
                        else:
                            lines_out.append('* ending 走出这里时，天光落在肩上，像什么都没发生过。')
                i += 1
                continue
            if 'goto' in e:
                tgt = e['goto']
                if tgt in scene_subs[sid]:
                    lines_out.append(f"* goto {sub_label(sid, tgt)}")
                elif tgt in scenes:
                    lines_out.append(f'* goto s_{tgt}')
                else:
                    warn.append(f"{sid}: 行级 goto '{tgt}' 不可解析，按顺序继续")
            i += 1

    story = '\n'.join(lines_out) + '\n'
    with open(os.path.join(out, 'story.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(story)

    n_labels = len(label_defs)
    n_text = sum(1 for l in lines_out if l.startswith('> ') or l.startswith('* say '))
    print(f"story.txt: {len(story)} B / {len(lines_out)} 行")
    print(f"场景 {len(order)} ｜ 标签 {n_labels}（上限 512）｜ 正文 {n_text} 句")
    print(f"背景 {len(bg_files)} 张（684×513）｜ 精灵 {len(sprites)}×2 变体")
    if warn:
        print('⚠ 告警：')
        for w in warn[:20]:
            print('  -', w)
        sys.exit(1)
    print('转换完成，无告警。')

if __name__ == '__main__':
    main()
