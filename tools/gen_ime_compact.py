#!/usr/bin/env python3
"""gen_ime_compact.py — take-easy 精简拼音码表生成器（B 方案）。

从全量 IME3 码表生成全新精简表（同格式，引擎零改动）：
  1. 单字区全量保留（9719 汉字 = ~97KB，生僻字也打得出来）；
  2. 词区**每桶字节配额 + 桶头截断**：桶内顺序即 YanReader 词频优先序，
     桶间只是拼音字母序（v1/v2 教训：任何跨桶全局排序都会按字母序误杀
     w/x/y/z 桶的高频词）；按桶字节占比分摊预算，每桶从头部保留，
     保底 4 组 → 每个拼音的最常用词组全景覆盖；
  3. 联想区丢弃（app 内置 BuiltinPredict + TF 用户词典仍提供联想）；
  4. 0xFF 填充到固定 0x93000，两分表拼接 @0x93000 → 完整 1MB ime 分区镜像。

固件侧配套（main/ime/IME.cpp，运行时按分区尺寸分派布局）：
  - 旧布局（ime 分区 ≥ 4MB，已发布设备 TF 升级路径）：全量表 3481419B + 两分 @0x360000；
  - 新布局（ime 分区 = 1MB，V1.0.2 起新刷机）：精简表 0x93000B + 两分 @0x93000。

格式参考（逆向自 yong_dict.cpp Im3Dictionary::parse / nextWordGroup）：
  header 12B: "IME3" + scheme(1) + codeLen(1) + rsv(2) + singleCount u32
  single index: 677 × u32 记录号（桶 = 首字母*26+次字母，末项哨兵）
  singles: singleCount × (codeLen+3+1) 定长记录
  word area: wordCount u32 + 677 × u32 字节偏移 + 变长组数据
    group = [cl:1][code:cl][n:1] + n × [wl:1][word:wl][flag:1]
  predict area（精简版丢弃）。

用法：
  python gen_ime_compact.py <ime_table_pinyin.bin> <liangfen.bin> [--out <目录>]
只需 Python 3 标准库。
"""

import hashlib
import struct
import sys
from pathlib import Path

IDX = 26 * 26 + 1          # 677
PAD = 0x93000              # 精简拼音表固长（1MB ime 分区 − 两分 0x6C09A − 余量）
MIN_GROUPS_PER_BUCKET = 4  # 每桶保底组数：任一拼音至少保留头部这几组常用词


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def parse_full(blob: bytes):
    assert blob[:4] == b"IME3", "magic"
    scheme, code_len = blob[4], blob[5]
    assert scheme == 1 and 1 <= code_len <= 6, "scheme/codeLen"
    single_count = u32(blob, 8)
    assert single_count > 0
    single_base = 12 + IDX * 4
    rec = code_len + 3 + 1
    single_end = single_base + single_count * rec
    assert single_end <= len(blob), "single area overflow"
    word_base = single_end
    word_count = u32(blob, word_base)
    idx = list(struct.unpack_from("<%dI" % IDX, blob, word_base + 4))
    data_start = word_base + 4 + IDX * 4
    data_size = idx[IDX - 1]
    assert data_start + data_size <= len(blob), "word area overflow"
    for i in range(1, IDX):
        assert idx[i] >= idx[i - 1] and idx[i] <= data_size, "index not monotonic"
    return {
        "code_len": code_len, "single_count": single_count,
        "single_end": single_end, "word_count": word_count,
        "word_idx": idx, "word_data": blob[data_start:data_start + data_size],
    }


def walk_groups(data: bytes, start: int, end: int):
    pos = start
    while pos < end:
        cl = data[pos]
        assert cl > 0 and pos + 1 + cl <= end, "bad group code"
        code = bytes(data[pos + 1:pos + 1 + cl])
        p = pos + 1 + cl
        n = data[p]; p += 1
        cands = []
        for _ in range(n):
            assert p < end, "bad candidate"
            wl = data[p]; p += 1
            assert wl > 0 and p + wl + 1 <= end, "bad word"
            cands.append((bytes(data[p:p + wl]), data[p + wl]))
            p += wl + 1
        yield cl, code, cands
        pos = p


def gb2312_level1() -> str:
    """GB2312 一级字库（16~55 区 ×94 位 = 3755 常用字）。
    逐编码点解码即得，零外部语料；用于 rank0 候选的"常用度"过滤。"""
    out = []
    for qu in range(16, 56):
        for wei in range(1, 95):
            try:
                out.append(bytes([0xA0 + qu, 0xA0 + wei]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(out)


def word_positions(info) -> dict:
    """全量表 词→(bucket, group_seq) 位置表（组内任意位次均命中）。"""
    table = {}
    idx, data = info["word_idx"], info["word_data"]
    for b in range(IDX - 1):
        for seq, (cl, code, cands) in enumerate(walk_groups(data, idx[b], idx[b + 1])):
            for w, _f in cands:
                try:
                    text = w.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                if text not in table:
                    table[text] = (b, seq)
    return table


def build(full: bytes):
    """B 方案核心：必保词组白名单 + 桶内等距采样 × rank0-only。

    全表结构事实（v1~v3 排错教训）：
      - 桶间按拼音字母序、桶内组按 code 字母序——两级都无频次语义，
        任何"头部/全局排序"截断都会误杀字母序靠后的 w/x/y/z 词；
      - 组内候选位次是唯一频率信号（YanReader 频序），必过词全部 rank0。
    策略：
      1. MUST_WORDS 所在组整组保留（人工高频先验，保证常用口语全覆盖）；
      2. 其余预算按桶均匀分摊，桶内**等距采样**组（code 空间最大覆盖面，
         每个拼音前缀尽量都能出词）；
      3. 采样组只保 rank0 候选且须全 GB2312 一级字（生僻词组不浪费预算；
         GB2312 一级 = 3755 最常用字集）。
      组内平均 1.16 条候选，rank0-only 后每组 ~13B，覆盖组数最大化。
    装配实测超 PAD 即整体收缩采样比例重来（最多 4 轮）。
    """
    info = parse_full(full)
    common = set(gb2312_level1())
    budget0 = PAD - info["single_end"] - 4 - IDX * 4 - 4096
    assert budget0 > 0

    # 第一遍：逐桶枚举组（组头 cl+1+1 + Σ(词+1长+1旗)），统计字节
    buckets = []        # [(b, [(seq, cl, code, cands, nbytes), ...]), ...]
    full_word_bytes = 0
    for b in range(IDX - 1):
        groups = []
        start = info["word_idx"][b]
        end = info["word_idx"][b + 1]
        for seq, (cl, code, cands) in enumerate(walk_groups(info["word_data"], start, end)):
            nb = 1 + cl + 1 + sum(1 + len(w) + 1 for w, _f in cands)
            groups.append((seq, cl, code, cands, nb))
            full_word_bytes += nb
        buckets.append((b, groups))

    # 必保组：MUST_WORDS → (b, seq)；整组保留
    pos = word_positions(info)
    must_keys = {}
    missing_whitelist = []
    for t in MUST_WORDS:
        p = pos.get(t)
        if p:
            must_keys[p] = t
        else:
            missing_whitelist.append(t)
    must_bytes = 0
    for b, groups in buckets:
        for seq, cl, code, cands, nb in groups:
            if (b, seq) in must_keys:
                must_bytes += nb
    assert must_bytes < budget0, "whitelist too large"

    rest_budget0 = budget0 - must_bytes
    ratio = rest_budget0 / (full_word_bytes - must_bytes)
    for attempt in range(4):
        # 第二遍：必保组整组 + 桶内等距采样组（rank0-only，一级字过滤）
        chosen = {}
        for b, groups in buckets:
            rest = [g for g in groups if (b, g[0]) not in must_keys]
            for seq, cl, code, cands, nb in groups:
                if (b, seq) in must_keys:
                    chosen[(b, seq)] = (code, cands)          # 整组
            quota_groups = int(len(rest) * ratio)
            if quota_groups < MIN_GROUPS_PER_BUCKET and rest:
                quota_groups = min(MIN_GROUPS_PER_BUCKET, len(rest))  # 每桶保底几组常用位
            step = max(1, -(-len(rest) // quota_groups)) if quota_groups else 0
            for i, (seq, cl, code, cands, nb) in enumerate(rest):
                if step and i % step:
                    continue
                w0, flag0 = cands[0]
                try:
                    text = w0.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                if any(ch not in common for ch in text):
                    continue
                chosen[(b, seq)] = (code, [(w0, flag0)])      # rank0-only

        # 第三遍：按原表组序重建词区
        data = bytearray()
        idx = []
        kept_total, kept_groups = 0, 0
        for b in range(IDX):
            idx.append(len(data))
            start = info["word_idx"][b]
            end = info["word_idx"][b + 1] if b + 1 < IDX else info["word_idx"][IDX - 1]
            for seq, (cl, code, cands) in enumerate(walk_groups(info["word_data"], start, end)):
                hit = chosen.get((b, seq))
                if not hit:
                    continue
                data.append(cl); data.extend(code)
                data.append(len(hit[1]))
                kept_groups += 1
                kept_total += len(hit[1])
                for w, flag in hit[1]:
                    data.append(len(w)); data.extend(w); data.append(flag)
        assert idx[IDX - 1] == len(data), "sentinel"
        total = info["single_end"] + 4 + IDX * 4 + len(data)
        # 超采只来自采样部分（必保组字节固定）：按超出比例收缩采样比例重来
        sampled = len(data) - must_bytes
        if total > PAD:
            ratio *= rest_budget0 / max(1, sampled)
            continue
        if PAD - total > 8192 and attempt < 2:
            # 预算没用满：放大采样比例逼近用满（rank0-only 每组 ~13B，组数配额
            # 按全组字节比例算，天然花不满）；放大后若超 PAD 再走上面的收缩分支
            ratio *= min(2.0, (rest_budget0 + (PAD - total) - 4096) / max(1, sampled))
            continue
        break
    assert total <= PAD, f"table {total}B exceeds PAD {PAD}B"

    out = bytearray(full[:info["single_end"]])
    out += struct.pack("<I", kept_total)
    out += struct.pack("<%dI" % IDX, *idx)
    out += data
    print(f"词区预算 {budget0}B（必保组 {len(must_keys)} 组 {must_bytes}B，"
          f"采样比例 {ratio:.4f}，{attempt + 1} 轮收敛）")
    if missing_whitelist:
        print(f"注意：白名单 {len(missing_whitelist)} 词全量表本就无：{'、'.join(missing_whitelist)}")
    print(f"保留 {kept_groups} 组 {kept_total} 词；总大小 {total}B")
    return bytes(out), info, kept_total, kept_groups, 0


# 必过词表：高频口语/时间/生活/情感/地名/设备场景词。任一在精简表中丢失
# 且全量表本有 → 生成失败（exit 1）。全量表本就无的标 "--"，不计失败。
MUST_WORDS = [
    "我们", "你们", "他们", "你好", "谢谢", "再见", "喜欢", "知道", "现在", "没有",
    "什么", "怎么", "因为", "所以", "但是", "如果", "已经", "还是", "就是", "不是",
    "今天", "明天", "昨天", "晚上", "早上", "时间", "时候", "以后", "一直", "一起",
    "朋友", "孩子", "老师", "学生", "电话", "电脑", "手机", "音乐", "游戏", "小说",
    "天气", "名字", "地方", "东西", "事情", "工作", "学习", "生活", "世界", "问题",
    "高兴", "快乐", "难过", "生气", "感觉", "觉得", "明白", "清楚", "简单", "容易",
    "困难", "重要", "特别", "非常", "真的", "可以", "应该", "需要", "希望", "开始",
    "中国", "北京", "上海", "深圳", "广州",
    "电池", "充电", "系统", "设置", "网络", "密码", "蓝牙", "键盘", "输入", "拼音",
    "输入法", "文件", "照片", "声音",
    "对不起", "没关系", "不知道", "为什么", "怎么样", "有意思",
]


def verify_compact(blob: bytes, full: bytes):
    """全组精确搜词：把两张表的词区全部词条建成 词→(code, rank) 索引，
    必过词逐一核对；缺失时回全量表归因（区分“被筛选丢弃”与“原表本无”）。
    不再用“首组抽验”（v1 缺陷：只看每桶首组，结论不可靠）。"""
    def word_index(data_blob: bytes) -> dict:
        info = parse_full(data_blob)
        table = {}
        idx, data = info["word_idx"], info["word_data"]
        for b in range(IDX - 1):
            for cl, code, cands in walk_groups(data, idx[b], idx[b + 1]):
                for rank, (w, _flag) in enumerate(cands):
                    try:
                        text = w.decode("utf-8")
                    except UnicodeDecodeError:
                        continue
                    cur = table.get(text)
                    if cur is None or rank < cur[1]:
                        table[text] = (code.decode("ascii", "replace"), rank)
        return table

    compact_tab = word_index(blob)
    full_tab = word_index(full)
    miss, absent = [], []
    for t in MUST_WORDS:
        hit = compact_tab.get(t)
        if hit:
            print(f"  OK   {t!r:<8} code={hit[0]} 组内位次={hit[1]}")
            continue
        raw = full_tab.get(t)
        if raw:
            print(f"  MISS {t!r:<8} 全量表有 code={raw[0]} 位次={raw[1]}（被筛选丢弃）")
            miss.append(t)
        else:
            print(f"  --   {t!r:<8} 全量表本就无此词（不计失败）")
            absent.append(t)
    print(f"必过词 {len(MUST_WORDS)}：精简命中 {len(MUST_WORDS) - len(miss) - len(absent)}，"
          f"丢弃 {len(miss)}，原表缺 {len(absent)}")
    if miss:
        print(f"[ERR] 必过词丢失: {'、'.join(miss)}")
    return miss


def main() -> int:
    argv = sys.argv[1:]
    out_dir = Path(argv[argv.index("--out") + 1]) if "--out" in argv else Path(".")
    pos = [a for a in argv if not a.startswith("--") and not a[1:].isdigit()]
    if len(pos) < 2:
        print(__doc__)
        return 1
    full = Path(pos[0]).read_bytes()
    liangfen = Path(pos[1]).read_bytes()
    assert len(liangfen) == 442698, "liangfen size"

    compact, info, words, groups, dropped = build(full)
    print(f"全量表: 单字 {info['single_count']}，词组 {info['word_count']} 条")
    print(f"精简表: 词组 {words} 条（{groups} 组，每桶保底 {MIN_GROUPS_PER_BUCKET}）")
    print(f"精简拼音表大小: {len(compact)}B（{len(compact)/1024:.1f}KB），预算 {PAD}B")
    if len(compact) > PAD:
        print(f"[ERR] 超过 {PAD:#x} 填充预算，请减小 --k")
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    padded = compact + b"\xff" * (PAD - len(compact))
    image = padded + liangfen
    (out_dir / "ime_compact_pinyin.bin").write_bytes(padded)
    (out_dir / "liangfen.bin").write_bytes(liangfen)
    (out_dir / "ime_partition_image.bin").write_bytes(image)

    print("必过词全组精确搜词（精简表 vs 全量表）:")
    miss = verify_compact(padded, full)
    # 单字抽检：与全量表逐字节一致
    assert padded[:info["single_end"]] == full[:info["single_end"]], "single area changed"
    print("单字区与全量表逐字节一致 ✓")
    print(f"[OK] ime_partition_image.bin {len(image)}B（拼音 {len(padded)}B@0 + 两分 @0x{PAD:X}）")
    print(f"     sha256(ime_compact_pinyin) {hashlib.sha256(padded).hexdigest()}")
    print(f"     sha256(liangfen)           {hashlib.sha256(liangfen).hexdigest()}")
    print(f"     sha256(ime_image)          {hashlib.sha256(image).hexdigest()}")
    return 1 if miss else 0


if __name__ == "__main__":
    raise SystemExit(main())
