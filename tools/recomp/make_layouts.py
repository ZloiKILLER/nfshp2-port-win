#!/usr/bin/env python3
"""The gamepad layouts patch as a table for the runtime: make_layouts.py

recomp/data/patch/SLUS-20362_1D2818AF_triggers.pnach is Silent's PCSX2 patch "Trigger control mappings": it rewrites the game's tables
of controller setups (data, not code) so that the DualShock 2 has eight, four of them with the throttle and the brake on the triggers.
Every line is a 32-bit write. This writes them as recomp/runtime/layouts_patch.h, which ps2.cpp applies once the program is loaded.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'recomp', 'data', 'patch', 'SLUS-20362_1D2818AF_triggers.pnach')
DST = os.path.join(ROOT, 'recomp', 'runtime', 'layouts_patch.h')

writes = []
for line in open(SRC):
    m = re.match(r'patch=0,EE,([0-9A-Fa-f]{8}),extended,([0-9A-Fa-f]+)', line)
    if not m:
        continue
    addr, value = int(m.group(1), 16), int(m.group(2), 16)
    assert addr >> 28 == 2, 'only 32-bit writes are known here'
    writes.append((addr & 0x0fffffff, value))
with open(DST, 'w', newline='\n') as f:
    f.write('// Made by tools/recomp/make_layouts.py from recomp/data/patch/SLUS-20362_1D2818AF_triggers.pnach: "Trigger control mappings"\n')
    f.write('// by Silent, a PCSX2 patch for SLUS-20362 (CRC 1D2818AF). Each pair is an address of the game\'s data and the word to put there.\n')
    f.write('static const unsigned kLayoutsPatch[][2] = {\n')
    for a, v in writes:
        f.write('    {0x%08x, 0x%08x},\n' % (a, v))
    f.write('};\n')
print(len(writes), 'writes ->', DST)
