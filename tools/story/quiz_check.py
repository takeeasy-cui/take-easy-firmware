#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""quiz_check.py — 题库块完整性校验：每题 Q/A/B(/C/D)/R/E 齐全、R 合法、无重复项。

R 语法（2026-10-27 起，随固件 quiz_engine 同步扩展）：
  R A          单选：单字母 A-D
  R ABD        多选：2-4 个不重复字母，全选对才得分（勾选+确认答题）
  R 对 / R 错  判断：也接受 T/F/t/f/√/×；判断题 C/D 可省
  判断题也可写 R A / R B（引擎按 A/B 行文本 “对/错/正确/错误” 后验转换）。"""
import sys

JUDGE_R = {'对', '错', 'T', 'F', 't', 'f', '√', '×'}
TF_TXT = {'对', '错', '正确', '错误'}


def r_kind(v):
    if v in JUDGE_R:
        return 'judge'
    if v and all(c in 'ABCD' for c in v) and len(set(v)) == len(v):
        return 'single' if len(v) == 1 else 'multi'
    return None


def check(path):
    lines = open(path, encoding='utf-8').read().split('\n')
    errs, cur, n = [], None, 0

    def finish(c, num):
        if not c:
            return
        kind = r_kind(c.get('R', ''))
        if kind is None:
            errs.append(f'题{num}: R 非法[{c.get("R")}]')
            kind = 'single'                # 按单选继续查缺失项
        # R 写了字母但 A/B 行恰为对/错文本 → 引擎后验为判断题
        if kind == 'single' and c.get('R') in ('A', 'B') \
                and c.get('TA') in TF_TXT and c.get('TB') in TF_TXT:
            kind = 'judge'
        # R 直接写判断词时，A/B 行也必须是对/错类文本（否则引擎判分对不上）
        if kind == 'judge' and c.get('R') in JUDGE_R \
                and (c.get('TA') not in TF_TXT or c.get('TB') not in TF_TXT):
            errs.append(f'题{num}: 判断题 A/B 应为 对/错（或 正确/错误）文本')
        need = 'QABRE' if kind == 'judge' else 'QABCDRE'
        for k in need:
            if k not in c:
                errs.append(f'题{num}: 缺 {k}')

    for i, l in enumerate(lines, 1):
        if l.startswith('Q '):
            if cur is not None:
                finish(cur, n)
            n += 1
            cur = {'Q': i}
        elif cur is not None and l[:2] in ('A ', 'B ', 'C ', 'D ', 'E '):
            k = l[0]
            if k in cur:
                errs.append(f'题{n} L{i}: {k} 重复')
            cur[k] = i
            cur['T' + k] = l[2:].strip()   # A/B 行文本（判断后验用）
        elif cur is not None and l.startswith('R '):
            if 'R' in cur:
                errs.append(f'题{n} L{i}: R 重复')
            cur['R'] = l[2:].strip()
    finish(cur, n)
    print(f'[{ "OK" if not errs else "FAIL" }] {path}: 题数 {n}，错误 {len(errs)}')
    for e in errs[:20]:
        print('  ✗', e)
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(max(check(p) for p in sys.argv[1:]) if len(sys.argv) > 1 else 2)
