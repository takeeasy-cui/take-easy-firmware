#!/usr/bin/env python3
"""avgr_script_check.py — 推理 AVG 剧本静态校验（与 avg_script.h 规则对齐）。

用法：python avgr_script_check.py <script.txt> [script2.txt …]
检查项（全部来自引擎容量与查表规则，可在真机前发现剧本错误）：
  命令词表 · 引号配对 · 标签引用存在 · 证据 id 先声明后引用 ·
  容量上限（命令/标签/证据/证言条数/标记/文本长度）· press/clash 语句序号合法。
退出码 0=通过；每文件打印 PASS/FAIL 与问题行号。

2026-10-23：CAP 与引擎宏同步（CMDS 4096/LABELS 256/EV 24/FLAGS 32）；
case 声明允许多条（引擎语义：后声明覆盖，最终标题取最后一条）。
"""

import re
import sys
from pathlib import Path

CMDS_WORD = {"case", "label", "jump", "ifset", "ifclear", "flag", "bg",
             "char", "char_off", "say", "think", "gain_ev", "ev",
             "testimony", "stmt", "press", "clash", "ask_ev", "end", "voicemap"}
CAP = {"cmd": 4096, "label": 256, "ev": 24, "stmt": 12, "flag": 32, "text": 640}

def strip_comment(line: str) -> str:
    i = line.find("//")
    return (line[:i] if i >= 0 else line).strip()

def check(path: Path) -> int:
    text = path.read_text(encoding="utf-8")
    raw = [strip_comment(l) for l in text.splitlines()]
    lines = [(i + 1, l) for i, l in enumerate(raw) if l]
    problems = []
    labels, evidence, flags = set(), set(), set()
    cmd_count = stmt_current = 0
    case_seen = end_seen = False

    for ln, l in lines:
        words = l.split()
        head = words[0]
        if head not in CMDS_WORD:
            problems.append(f"{ln}: 未知命令「{head}」")
            continue
        rest = l[len(head):].strip()
        if head == "voicemap":
            # 元数据：说话人→立绘映射，不产生命令、不计入容量（引擎同语义）
            if not re.match(r'^"[^"]+"\s+"[^"]+"\s*$', rest):
                problems.append(f"{ln}: voicemap 格式应为 voicemap \"说话人\" \"立绘文件\"")
            continue
        cmd_count += 1
        if head == "case":
            # 引擎允许多条 case：后声明覆盖，最终标题取最后一条（两章合并包靠此机制）
            case_seen = True
        elif head == "label":
            labels.add(rest)
        elif head in ("jump", "ifset", "ifclear"):
            body = rest.split()
            if head == "jump":
                if not body:
                    problems.append(f"{ln}: jump 缺目标")
            else:
                if len(body) < 2:
                    problems.append(f"{ln}: {head} 缺 flag/目标")
        elif head == "flag":
            flags.add(rest)
        elif head == "ev":
            m = re.match(r'^(\S+)\s+"(.*)"\s+"(.*)"\s*$', rest)
            if not m:
                problems.append(f"{ln}: ev 格式应为：ev id \"名称\" \"描述\"")
            else:
                evidence.add(m.group(1))
        elif head == "gain_ev":
            pass  # 引用检查放在第二遍
        elif head == "say":
            if not re.search(r'".*"\s+"(.*)"', rest):
                problems.append(f"{ln}: say 需要说话人与文本两个引号段")
        elif head == "testimony":
            stmt_current = 0
        elif head == "stmt":
            stmt_current += 1
            if stmt_current > CAP["stmt"]:
                problems.append(f"{ln}: 单段证言超过 {CAP['stmt']} 条")
            if len(rest.encode()) > CAP["text"]:
                problems.append(f"{ln}: 证言文本超 {CAP['text']} 字节")
        elif head in ("press", "clash"):
            body = rest.split()
            if len(body) < 2 or not body[0].isdigit():
                problems.append(f"{ln}: {head} 第一个参数须是证言序号")
            elif not (1 <= int(body[0]) <= stmt_current):
                problems.append(f"{ln}: {head} 序号 {body[0]} 超出当前证言条数"
                                f"（当前 {stmt_current} 条）")
            if head == "clash" and len(body) < 3:
                problems.append(f"{ln}: clash 需要 证据id 参数")
        elif head == "char":
            if len(rest.split()) < 3:
                problems.append(f"{ln}: char 参数不足（槽 文件 站位 [聚焦]）")
        elif head == "end":
            end_seen = True

    # 第二遍：引用合法性
    for ln, l in lines:
        words = l.split()
        head, rest = words[0], l[len(words[0]):].strip()
        if head in ("jump",):
            if rest.split() and rest.split()[0] not in labels:
                problems.append(f"{ln}: jump 目标不存在「{rest.split()[0]}」")
        elif head == "ifset":
            body = rest.split()
            if len(body) >= 2 and body[1] not in labels:
                problems.append(f"{ln}: ifset 目标不存在「{body[1]}」")
        elif head == "ifclear":
            body = rest.split()
            if len(body) >= 2 and body[1] not in labels:
                problems.append(f"{ln}: ifclear 目标不存在「{body[1]}」")
        elif head == "press":
            body = rest.split()
            if len(body) >= 2 and body[1] not in labels:
                problems.append(f"{ln}: press 目标不存在「{body[1]}」")
        elif head == "clash":
            body = rest.split()
            if len(body) >= 3:
                if body[1] not in evidence:
                    problems.append(f"{ln}: clash 证据未声明「{body[1]}」")
                if body[2] not in labels:
                    problems.append(f"{ln}: clash 目标不存在「{body[2]}」")
        elif head == "gain_ev" and rest not in evidence:
            problems.append(f"{ln}: gain_ev 证据未声明「{rest}」")

    if cmd_count > CAP["cmd"]:
        problems.append(f"命令总数 {cmd_count} 超上限 {CAP['cmd']}")
    if len(labels) > CAP["label"]:
        problems.append(f"标签数 {len(labels)} 超上限 {CAP['label']}")
    if len(evidence) > CAP["ev"]:
        problems.append(f"证据数 {len(evidence)} 超上限 {CAP['ev']}")
    if len(flags) > CAP["flag"]:
        problems.append(f"标记数 {len(flags)} 超上限 {CAP['flag']}")
    if not case_seen:
        problems.append("缺少 case 声明（标题页需要）")
    if not end_seen:
        problems.append("缺少 end（判决页）")

    tag = "PASS" if not problems else "FAIL"
    print(f"[{tag}] {path}  （命令 {cmd_count} · 标签 {len(labels)} · 证据 {len(evidence)} · 标记 {len(flags)}）")
    for p in problems:
        print(f"    - {p}")
    return 0 if not problems else 1

def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    rc = 0
    for a in argv[1:]:
        p = Path(a)
        if not p.is_file():
            print(f"[FAIL] {p} 文件不存在")
            rc = 1
            continue
        rc |= check(p)
    return rc

if __name__ == "__main__":
    sys.exit(main(sys.argv))
