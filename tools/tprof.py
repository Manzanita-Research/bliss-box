#!/usr/bin/env python3
"""fm1-emu FM1_TPROF output to time by function, from the build's disassembly.
   tools/tprof.py build/emu/tprof.txt app/build/blissbox.dis"""
import re, sys, bisect
labels = []
for line in open(sys.argv[2]):
    m = re.match(r'^([A-Za-z_$][\w.$]*):\s*$', line)
    if m:
        name = m.group(1)
        continue
    m = re.match(r'^\s*([0-9a-f]+):', line)
    if m and 'name' in dir():
        labels.append((int(m.group(1), 16), name))
        del name
labels.sort()
addrs = [a for a, _ in labels]
tot = {0: {}, 1: {}}
for line in open(sys.argv[1]):
    c, pc, n = line.split()
    if c not in ('0', '1'):
        continue
    pc = int(pc, 16)
    i = bisect.bisect_right(addrs, pc) - 1
    f = labels[i][1] if i >= 0 else '?'
    tot[int(c)][f] = tot[int(c)].get(f, 0) + int(n)
for core in (0, 1):
    t = tot[core]
    s = sum(t.values())
    if not s:
        continue
    print(f"core {core}")
    for f, n in sorted(t.items(), key=lambda x: -x[1])[:28]:
        print(f"  {100.0 * n / s:5.1f}%  {f}")
