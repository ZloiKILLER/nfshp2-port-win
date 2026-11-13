#!/usr/bin/env python3
"""The sound driver as C++: gen_iop.py --irx <data>/IRX253/STREAM.IRX [--out work/recomp/gen]

STREAM.IRX (MultiStream 5.2) is the one program of the PS2's second processor, the IOP, that the port runs: the game's sound driver. It
is MIPS I code in a relocatable module. This translates it ahead of time, as gen.py does the game's own program, so that nothing
is decoded while the game runs.

The module is put where the runtime always loads it (MODULE_BASE in iop_kernel.cpp, 0x60000: it is the first and only one) and its
relocations are applied, which gives the words the runtime has in memory. Every word of .text becomes a case of a switch on the
program counter, in functions of 1024 words each: any address can be jumped to, a function's or one from a jump table, and code runs
on from one case into the next as on the processor. The IOP's kernel in the runtime (iop_kernel.cpp) is as it was: its threads are
sets of registers, and the code runs on those registers and leaves at a branch when the slice is spent or the kernel says stop.

What the generated code keeps of the processor is what the driver's code can see: the registers, hi and lo, memory through the same
accessors, a branch's delay slot run after the branch is decided. An import's stub is not translated: the runtime puts a reserved
opcode there when it loads the module, and the case calls the kernel's function that opcode names (iop_hle_call).
"""
import argparse
import os
import struct

BASE = 0x60000
CHUNK = 1024                    # words in a function
PER_FILE = 7                    # functions in a file


def load(path):
    f = bytearray(open(path, 'rb').read())
    assert struct.unpack_from('<I', f, 0)[0] == 0x464C457F and struct.unpack_from('<H', f, 16)[0] == 0xFF80, 'not an IRX'
    phoff, shoff = struct.unpack_from('<II', f, 28)
    phnum, = struct.unpack_from('<H', f, 44)
    shnum, = struct.unpack_from('<H', f, 48)
    mem = bytearray(0x200000)
    iopmod = memsz = 0
    for i in range(phnum):
        t, off, vaddr, _, filesz, msz = struct.unpack_from('<IIIIII', f, phoff + 32 * i)
        if t == 0x70000080:
            iopmod = off
        if t == 1:
            memsz = msz
            mem[BASE + vaddr:BASE + vaddr + filesz] = f[off:off + filesz]
    assert iopmod and memsz

    def lw(a):
        return struct.unpack_from('<I', mem, a)[0]

    def sw(a, v):
        struct.pack_into('<I', mem, a, v & 0xFFFFFFFF)

    # the relocations, as iop_kernel.cpp's load_irx applies them
    hi = []
    for i in range(shnum):
        sh = shoff + 40 * i
        if struct.unpack_from('<I', f, sh + 4)[0] != 9:
            continue
        off, size = struct.unpack_from('<II', f, sh + 16)
        for k in range(0, size, 8):
            at = BASE + struct.unpack_from('<I', f, off + k)[0]
            typ = struct.unpack_from('<I', f, off + k + 4)[0] & 0xFF
            w = lw(at)
            if typ == 2:
                sw(at, w + BASE)
            elif typ == 4:
                sw(at, (w & 0xFC000000) | ((w + (BASE >> 2)) & 0x03FFFFFF))
            elif typ == 5:
                hi.append(at)
            elif typ == 6:
                lo = w & 0xFFFF
                lo = lo - 0x10000 if lo & 0x8000 else lo
                for h in hi:
                    hw = lw(h)
                    full = (((hw & 0xFFFF) << 16) + lo + BASE) & 0xFFFFFFFF
                    sw(h, (hw & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
                hi = []
                sw(at, (w & 0xFFFF0000) | ((w + BASE) & 0xFFFF))
            elif typ not in (0, 7):
                raise SystemExit('relocation type %d' % typ)
    text_size = struct.unpack_from('<I', f, iopmod + 12)[0]
    # the import stubs: after a table's header (0x41E00000, 0, version, the library's name) pairs of "jr ra" and the ordinal
    stubs = set()
    a = BASE
    while a < BASE + text_size:
        if lw(a) == 0x41E00000:
            s = a + 20
            while lw(s) == 0x03E00008:
                stubs.add(s)
                s += 8
        a += 4
    return mem, text_size, stubs


def fnv(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def sx16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def simple(pc, op):
    """One instruction that is no branch, as C++; None for a branch or jump. As iop_cpu.cpp's interpreter had it."""
    rs, rt, rd, sa = (op >> 21) & 31, (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31
    imm = op & 0xFFFF
    s16 = sx16(imm)
    R = lambda n: 'r[%d]' % n
    addr = '(r[%d] + %du)' % (rs, s16 & 0xFFFFFFFF)

    def setr(n, expr):
        return '' if n == 0 else 'r[%d] = %s;' % (n, expr)

    major = op >> 26
    if major == 0:
        fn = op & 63
        if fn == 0x00: return setr(rd, '%s << %d' % (R(rt), sa))
        if fn == 0x02: return setr(rd, '%s >> %d' % (R(rt), sa))
        if fn == 0x03: return setr(rd, '(u32)((s32)%s >> %d)' % (R(rt), sa))
        if fn == 0x04: return setr(rd, '%s << (%s & 31)' % (R(rt), R(rs)))
        if fn == 0x06: return setr(rd, '%s >> (%s & 31)' % (R(rt), R(rs)))
        if fn == 0x07: return setr(rd, '(u32)((s32)%s >> (%s & 31))' % (R(rt), R(rs)))
        if fn in (0x08, 0x09): return None
        if fn == 0x0C: return 'iop_gen_syscall(0x%x);' % pc
        if fn == 0x0D: return 'iop_gen_break(0x%x); return 0x%x;' % (pc, pc + 4)
        if fn == 0x10: return setr(rd, 'c.hi')
        if fn == 0x11: return 'c.hi = %s;' % R(rs)
        if fn == 0x12: return setr(rd, 'c.lo')
        if fn == 0x13: return 'c.lo = %s;' % R(rs)
        if fn == 0x18: return '{ s64 p = (s64)(s32)%s * (s32)%s; c.lo = (u32)p; c.hi = (u32)(p >> 32); }' % (R(rs), R(rt))
        if fn == 0x19: return '{ u64 p = (u64)%s * %s; c.lo = (u32)p; c.hi = (u32)(p >> 32); }' % (R(rs), R(rt))
        if fn == 0x1A: return 'iop_gen_div(c, %s, %s);' % (R(rs), R(rt))
        if fn == 0x1B: return 'iop_gen_divu(c, %s, %s);' % (R(rs), R(rt))
        if fn in (0x20, 0x21): return setr(rd, '%s + %s' % (R(rs), R(rt)))
        if fn in (0x22, 0x23): return setr(rd, '%s - %s' % (R(rs), R(rt)))
        if fn == 0x24: return setr(rd, '%s & %s' % (R(rs), R(rt)))
        if fn == 0x25: return setr(rd, '%s | %s' % (R(rs), R(rt)))
        if fn == 0x26: return setr(rd, '%s ^ %s' % (R(rs), R(rt)))
        if fn == 0x27: return setr(rd, '~(%s | %s)' % (R(rs), R(rt)))
        if fn == 0x2A: return setr(rd, '(s32)%s < (s32)%s' % (R(rs), R(rt)))
        if fn == 0x2B: return setr(rd, '%s < %s' % (R(rs), R(rt)))
        return 'BAD'
    if 1 <= major <= 7: return None
    if major in (8, 9): return setr(rt, '%s + %du' % (R(rs), s16 & 0xFFFFFFFF))
    if major == 0x0A: return setr(rt, '(s32)%s < %d' % (R(rs), s16))
    if major == 0x0B: return setr(rt, '%s < %du' % (R(rs), s16 & 0xFFFFFFFF))
    if major == 0x0C: return setr(rt, '%s & %du' % (R(rs), imm))
    if major == 0x0D: return setr(rt, '%s | %du' % (R(rs), imm))
    if major == 0x0E: return setr(rt, '%s ^ %du' % (R(rs), imm))
    if major == 0x0F: return setr(rt, '%du' % (imm << 16))
    if major == 0x10: return setr(rt, '0') if rs == 0 else ''      # COP0: only mfc0 is answered, with 0
    if major == 0x20: return setr(rt, '(u32)(s8)iop_lb(%s)' % addr) or '(void)iop_lb(%s);' % addr
    if major == 0x21: return setr(rt, '(u32)(s16)iop_lh(%s)' % addr) or '(void)iop_lh(%s);' % addr
    if major == 0x22: return 'iop_gen_lwl(r[%d], %s);' % (rt, addr) if rt else ''
    if major == 0x23: return setr(rt, 'iop_lw(%s)' % addr) or '(void)iop_lw(%s);' % addr
    if major == 0x24: return setr(rt, 'iop_lb(%s)' % addr) or '(void)iop_lb(%s);' % addr
    if major == 0x25: return setr(rt, 'iop_lh(%s)' % addr) or '(void)iop_lh(%s);' % addr
    if major == 0x26: return 'iop_gen_lwr(r[%d], %s);' % (rt, addr) if rt else ''
    if major == 0x28: return 'iop_sb(%s, (u8)%s);' % (addr, R(rt))
    if major == 0x29: return 'iop_sh(%s, (u16)%s);' % (addr, R(rt))
    if major == 0x2A: return 'iop_gen_swl(%s, %s);' % (addr, R(rt))
    if major == 0x2B: return 'iop_sw(%s, %s);' % (addr, R(rt))
    if major == 0x2E: return 'iop_gen_swr(%s, %s);' % (addr, R(rt))
    return 'BAD'


def branch(pc, op):
    """(what to do before the delay slot, the condition or None for always, the target as C++ or an address)"""
    rs, rt = (op >> 21) & 31, (op >> 16) & 31
    major = op >> 26
    rel = (pc + 4 + (sx16(op) << 2)) & 0xFFFFFFFF
    if major == 0:
        pre = 'const u32 to = r[%d];' % rs
        if (op & 63) == 0x09:
            rd = (op >> 11) & 31
            if rd:
                pre += ' r[%d] = 0x%xu;' % (rd, pc + 8)
        return pre, None, 'to'
    if major == 1:
        lt = '(s32)r[%d] < 0' % rs
        pre = 'const bool t = %s(%s);' % ('!' if rt & 1 else '', lt)
        if rt & 0x10:
            pre += ' r[31] = 0x%xu;' % (pc + 8)
        return pre, 't', rel
    if major in (2, 3):
        tgt = (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2)
        return ('r[31] = 0x%xu;' % (pc + 8)) if major == 3 else '', None, tgt
    cond = {4: 'r[%d] == r[%d]' % (rs, rt), 5: 'r[%d] != r[%d]' % (rs, rt), 6: '(s32)r[%d] <= 0' % rs, 7: '(s32)r[%d] > 0' % rs}[major]
    return 'const bool t = %s;' % cond, 't', rel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--irx', required=True)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'work', 'recomp', 'gen'))
    args = ap.parse_args()
    mem, text_size, stubs = load(args.irx)
    lo, hi_ = BASE, BASE + text_size
    words = text_size // 4
    chunks = (words + CHUNK - 1) // CHUNK

    def lw(a):
        return struct.unpack_from('<I', mem, a)[0]

    bad = 0
    files = []
    for c0 in range(0, chunks, PER_FILE):
        out = ['// Made by tools/recomp/gen_iop.py from STREAM.IRX: the sound driver\'s code, a case for each of its words.',
               '#include "iop_gen.h"', '']
        for ch in range(c0, min(c0 + PER_FILE, chunks)):
            first, last = lo + ch * CHUNK * 4, min(lo + (ch + 1) * CHUNK * 4, hi_)
            out.append('u32 iop_gen_chunk_%d(IopRegs &c, u32 pc, int &budget)' % ch)
            out.append('{')
            out.append('    u32 *const r = c.r;')
            out.append('    switch (pc) {')
            out.append('    default: return iop_gen_bad(pc, 0);     // not an address of this function')
            since = 0
            pc = first
            while pc < last:
                op = lw(pc)
                since += 1
                line = '    case 0x%x: L%x: ' % (pc, pc)
                if pc in stubs:
                    # the runtime's reserved opcode is here once the module is loaded: the kernel's function, which returns to $ra
                    out.append(line + 'return iop_hle_call(c, 0x%x);' % pc)
                    since = 0
                elif (op >> 26) == 0x3F:
                    out.append(line + 'return iop_hle_call(c, 0x%x);' % pc)
                    since = 0
                else:
                    s = simple(pc, op)
                    if s == 'BAD':
                        bad += 1
                        out.append(line + 'return iop_gen_bad(0x%x, 0x%08xu);' % (pc, op))
                        since = 0
                    elif s is not None:
                        out.append(line + (s or ';'))
                    else:
                        pre, cond, tgt = branch(pc, op)
                        slot_op = lw(pc + 4)
                        slot = simple(pc + 4, slot_op) if pc + 4 < hi_ + 4 else 'BAD'
                        if pc + 4 in stubs or (slot_op >> 26) == 0x3F:
                            slot = None
                        cost = since + 1
                        since = 0
                        if slot is None or slot == 'BAD':
                            out.append(line + 'return iop_gen_bad(0x%x, 0x%08xu);    // a branch with no plain delay slot' % (pc, op))
                        else:
                            if isinstance(tgt, int):
                                inside = first <= tgt < last
                                go = ('{ budget -= %d; if (budget <= 0) return 0x%xu; goto L%x; }' % (cost, tgt, tgt)) if inside else \
                                     ('{ budget -= %d; return 0x%xu; }' % (cost, tgt))
                            else:
                                go = '{ budget -= %d; return %s; }' % (cost, tgt)
                            body = '{ %s %s ' % (pre, slot)
                            if cond:
                                body += 'if (%s) %s budget -= %d; goto L%x; }' % (cond, go, cost, pc + 8) if pc + 8 < last else \
                                        'if (%s) %s budget -= %d; return 0x%xu; }' % (cond, go, cost, pc + 8)
                            else:
                                body += go[2:-2] + ' }'
                            out.append(line + body)
                pc += 4
            out.append('    }')
            out.append('    return 0x%xu;      // on into the next function, or an address that is not this one\'s' % last)
            out.append('}')
            out.append('')
        name = 'iop_stream_%02d.cpp' % (c0 // PER_FILE)
        files.append(name)
        with open(os.path.join(args.out, name), 'w', newline='\n') as f:
            f.write('\n'.join(out))
    with open(os.path.join(args.out, 'iop_gen_table.h'), 'w', newline='\n') as f:
        f.write('// Made by tools/recomp/gen_iop.py: where the sound driver\'s code is and what it was made from.\n')
        f.write('static const u32 kIopGenBase = 0x%x, kIopGenTextSize = 0x%x, kIopGenTextHash = 0x%08x;\n' % (BASE, text_size, fnv(mem[lo:hi_])))
        f.write('static const int kIopGenChunkShift = %d;\n' % (CHUNK * 4).bit_length().__sub__(1))
        for ch in range(chunks):
            f.write('u32 iop_gen_chunk_%d(IopRegs &c, u32 pc, int &budget);\n' % ch)
        f.write('static u32 (*const kIopGenChunks[])(IopRegs &, u32, int &) = {%s};\n' % ', '.join('iop_gen_chunk_%d' % ch for ch in range(chunks)))
    print('%d words of code, %d functions in %d files, %d import stubs, %d words that are no instruction' % (words, chunks, len(files), len(stubs), bad))


if __name__ == '__main__':
    main()
