#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""quiz_check.py — 题库块完整性校验：每题 A/B/C/D/R/E 齐全、R 合法、无重复项。"""
import sys

def check(path):
    lines = open(path, encoding='utf-8').read().split('\n')
    errs, cur, n = [], None, 0
    def finish(c, num):
        for k in 'QABCDE':
            if k not in (c or {}):
                errs.append(f'题{num}: 缺 {k}')
        if c and c.get('R') not in ('A', 'B', 'C', 'D'):
            errs.append(f'题{num}: R 非法[{c.get("R")}]')
    for i, l in enumerate(lines, 1):
        if l.startswith('Q '):
            if cur is not None:
                finish(cur, n)
            n += 1
            cur = {'Q': i}
        elif cur is not None and l[:2] in ('A ','B ','C ','D ','E '):
            k = l[0]
            if k in cur:
                errs.append(f'题{n} L{i}: {k} 重复')
            cur[k] = i
        elif cur is not None and l.startswith('R '):
            cur['R'] = l[2:].strip()[:1]
    finish(cur, n)
    print(f'[{ "OK" if not errs else "FAIL" }] {path}: 题数 {n}，错误 {len(errs)}')
    for e in errs[:20]:
        print('  ✗', e)
    return 1 if errs else 0

if __name__ == '__main__':
    sys.exit(max(check(p) for p in sys.argv[1:]) if len(sys.argv) > 1 else 2)
