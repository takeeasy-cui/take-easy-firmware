#!/usr/bin/env python3
"""game_pkg.py — take-easy 游戏资源包打包工具（TF 卡压缩包）。

分类命名规则（与固件 main/game/game_pkg.c 一一对应，改动两处需同步）：
  推理 AVG   : avgr-<game>-<case>.zip   包根 = 案件目录（script.txt + images/…）
  文字游戏   : text-<name>.zip          包根 = story.txt（+ 可选图片）
  步步高 BBK : bbk-<name>/              ★目录直挂（DAT.LIB 数十 MB，不走 zip）

zip 约束：单层文件结构、单条目 ≤ 8 MiB（固件解压上限）、不加密。
同名目录（如 /sdcard/games/avgr-gyakuten-case1/）也可直接放置（免 zip）。

用法：
  python game_pkg.py avgr  <案件目录>              → avgr-<目录名>.zip
  python game_pkg.py text  <故事目录>              → text-<目录名>.zip
  python game_pkg.py bbk   <资源目录>              → 校验并列出 bbk-<名> 需手拷的目录
  python game_pkg.py --out <输出目录> avgr <目录>  → 指定输出位置
产物默认写到输入目录的父目录（即 games/ 下，与 TF 卡结构一致）。

只需 Python 3 标准库：全新电脑零依赖。
"""

import subprocess
import sys
import zipfile
from pathlib import Path

ENTRY_LIMIT = 8 * 1024 * 1024  # 固件 zip_reader 单条目解压上限
PREFIX = {"avgr": "avgr", "text": "text", "bbk": "bbk"}


def precheck_avgr(src_dir: Path) -> bool:
    """打包前跑剧本静态校验：解析器失败的剧本绝不能进包（2026-10-18 教训）。"""
    script = src_dir / "script.txt"
    if not script.is_file():
        print("game_pkg: avgr 包缺 script.txt", file=sys.stderr)
        return False
    checker = Path(__file__).resolve().parent / "avgr_script_check.py"
    if not checker.is_file():   # 工具缺失时静默跳过（不阻塞离线打包）
        return True
    r = subprocess.run([sys.executable, str(checker), str(script)],
                       capture_output=True, text=True)
    print(r.stdout.strip())
    return r.returncode == 0


def pack_zip(cat: str, src_dir: Path, out_dir: Path, name: str = None) -> int:
    if not src_dir.is_dir():
        print(f"game_pkg: 目录不存在 {src_dir}", file=sys.stderr)
        return 1
    if cat == "avgr" and not precheck_avgr(src_dir):
        print(f"game_pkg: 剧本校验未通过，拒绝打包（先用 avgr_script_check.py 修复）",
              file=sys.stderr)
        return 1
    pkg = name or src_dir.name
    zip_name = f"{PREFIX[cat]}-{pkg}.zip" if not pkg.startswith(PREFIX[cat] + "-") else f"{pkg}.zip"
    zip_path = out_dir / zip_name

    files = sorted(p for p in src_dir.rglob("*") if p.is_file())
    if not files:
        print(f"game_pkg: 空目录 {src_dir}", file=sys.stderr)
        return 1

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for f in files:
            rel = f.relative_to(src_dir).as_posix()
            if len(rel.split("/")) > 3:      # 包约定浅层（script/images/bg/xx.png）
                print(f"game_pkg: 跳过深层文件 {rel}（包约定 ≤2 层子目录）")
                continue
            if f.name.startswith("."):
                continue
            if f.stat().st_size > ENTRY_LIMIT:
                print(f"game_pkg: 条目超 8MiB，跳过 {rel}（大资源请用目录直挂）")
                continue
            zf.write(f, rel)
            print(f"  + {rel}  ({f.stat().st_size} B)")
    print(f"game_pkg: {zip_path}  ({zip_path.stat().st_size} B, {len(files)} 项)")
    print(f"  拷贝到 TF 卡 /sdcard/games/ 即可（同名目录直挂亦可）。")
    return 0


def check_bbk(src_dir: Path) -> int:
    """BBK 资源目录校验：打印就绪状态与拷贝指引（目录直挂，不打 zip）。"""
    if not src_dir.is_dir():
        print(f"game_pkg: 目录不存在 {src_dir}", file=sys.stderr)
        return 1
    need = ["DAT.LIB", "HZK16", "ASC16"]
    optional = ["FMJHD.BIN"]
    missing = [n for n in need if not (src_dir / n).is_file()]
    have_hd = (src_dir / optional[0]).is_file()
    print(f"game_pkg: bbk 资源目录 {src_dir.name}/")
    for n in need:
        print(f"  {'OK ' if (src_dir / n).is_file() else '缺失'} {n}")
    print(f"  {'OK ' if have_hd else '可选'} FMJHD.BIN"
          f"（7x 高清库，缺失自动降级原版点阵）")
    if missing:
        print(f"game_pkg: 缺少必需文件，无法使用", file=sys.stderr)
        return 1
    print(f"  就绪。请在 TF 卡建 /sdcard/games/bbk-{src_dir.name}/ 并把上述文件复制进去"
          f"（目录直挂，不打 zip）。")
    return 0


def main(argv: list) -> int:
    out_dir = None
    name = None
    args = argv[:]
    if "--out" in args:
        i = args.index("--out")
        if i + 1 >= len(args):
            print(__doc__, file=sys.stderr)
            return 2
        out_dir = Path(args[i + 1])
        del args[i:i + 2]
    if "--name" in args:
        i = args.index("--name")
        if i + 1 >= len(args):
            print(__doc__, file=sys.stderr)
            return 2
        name = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2 or args[0] not in PREFIX:
        print(__doc__, file=sys.stderr)
        return 2
    cat, src = args
    src_dir = Path(src).resolve()
    out_dir = out_dir or src_dir.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    if cat == "bbk":
        return check_bbk(src_dir)
    return pack_zip(cat, src_dir, out_dir, name)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
