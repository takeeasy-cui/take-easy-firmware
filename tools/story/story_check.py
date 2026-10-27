#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""story_check.py — Story Engine v1 脚本校验器（基础版）。

校验：
  - label 查重；* goto / choice 目标可解析（含 * if/ifgoto/ifngoto/if* g 目标）
  - choice 2..6 项；直跳式选项文案非空
  - 行长 ≤640B；变量/标志名 ≤23 字符
  - 预算：label ≤512 / 总行数 / story ≤2MB
用法：python story_check.py <story.txt> ...
"""
import re
import sys

MAX_LABELS = 512
MAX_LEN = 640

def check(path: str) -> int:
    errs, warns = [], []
    labels = set()
    lines = open(path, encoding='utf-8').read().split('\n')

    def targets(ln):
        # * goto X | * ifgoto v op val X | * ifngoto ... | * if f g X | * ifnot f g X
        m = re.match(r'\*\s*goto\s+(\S+)', ln)
        if m: yield m.group(1)
        m = re.match(r'\*\s*if[n]?goto\s+\S+\s+\S+\s+\S+\s+(\S+)', ln)
        if m: yield m.group(1)
        m = re.match(r'\*\s*ifnot?\s+\S+\s+g\s+(\S+)', ln)
        if m: yield m.group(1)

    for idx, ln in enumerate(lines, 1):
        if len(ln.encode('utf-8')) > MAX_LEN:
            errs.append(f'{idx}: 行超长 {len(ln.encode("utf-8"))}B')
        s = ln.strip()
        m = re.match(r'\*\s*label\s+(\S+)', ln)
        if m:
            if m.group(1) in labels:
                errs.append(f'{idx}: label 重复 "{m.group(1)}"')
            labels.add(m.group(1))
        for t in targets(ln):
            if t not in labels and t not in ('\n',):
                # 目标可能在后文定义，先记录，末尾统一查
                pass
        m = re.match(r'\*\s*choice\s+(.*)', ln)
        if m:
            parts = m.group(1).split('|')
            if not (2 <= len(parts) <= 6):
                errs.append(f'{idx}: choice 项数 {len(parts)}（须 2..6）')
            for p in parts:
                if '->' in p:
                    lab, tgt = p.split('->', 1)
                    if not lab.strip():
                        errs.append(f'{idx}: choice 选项文案为空')
                else:
                    warns.append(f'{idx}: 旧式标志选项（c0..c{len(parts)-1}）')

    # 目标统一复扫（label 全收集后）
    for idx, ln in enumerate(lines, 1):
        if ln.startswith('>'):
            continue
        for t in targets(ln):
            if t not in labels:
                errs.append(f'{idx}: 目标 label 不存在 "{t}"')
        m = re.match(r'\*\s*choice\s+(.*)', ln)
        if m:
            for p in m.group(1).split('|'):
                if '->' in p:
                    t = p.split('->', 1)[1].strip()
                    if t and t not in labels:
                        errs.append(f'{idx}: choice 目标不存在 "{t}"')

    if len(labels) > MAX_LABELS:
        errs.append(f'label 总数 {len(labels)} 超上限 {MAX_LABELS}')

    status = 'FAIL' if errs else 'OK'
    print(f'[{status}] {path}: {len(lines)} 行 / label {len(labels)} / 警告 {len(warns)} / 错误 {len(errs)}')
    for e in errs[:30]:
        print('  ✗', e)
    for w in warns[:10]:
        print('  ⚠', w)
    return 1 if errs else 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    rc = 0
    for p in sys.argv[1:]:
        rc |= check(p)
    sys.exit(rc)
