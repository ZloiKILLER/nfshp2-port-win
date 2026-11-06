#!/usr/bin/env python3
"""Static recompiler: retail SLUS_203.62 (EE code) -> C++.

Every function becomes `void F_<addr>(Ctx &c)`. Control flow inside a
function is goto, jal is a direct call, jr $ra returns, jr through a jump
table is a switch over the labels the .rodata tables point at, and any other
indirect jump goes through the runtime dispatcher. Branch delay slots run
before the jump, likely branches run them only when taken.

Function starts come from recomp/symbols/retail_symbols.json (frozen: an earlier
carry-over of A1.56 names, kept so the recomp stays what was tested; the decomp
uses work/retail/symbols.json) plus every jal/j target and every code pointer in
the data sections that is not inside a known function (vtables).

Output: work/recomp/gen/*.cpp, funcs.h, functable.cpp

The retail program: work/release_us/SLUS_203.62, or --elf <path> (the first start of the exe copies it to nfshp2port/data/SLUS_203.62).
"""

import bisect
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import match_func  # noqa: E402
from r5900 import BRANCHES, LIKELY, decode  # noqa: E402

RETAIL = os.path.join(REPO, "work", "release_us", "SLUS_203.62")
if "--elf" in sys.argv:
    RETAIL = sys.argv[sys.argv.index("--elf") + 1]
# SDK functions whose bodies are replaced by host implementations (HLE_<name>)
HLE_LIST = os.path.join(REPO, "recomp", "runtime", "hle_names.txt")


def write_if_changed(path, text):
    """Keep the file's timestamp when nothing changed, so ninja rebuilds only
    the chunks whose functions moved."""
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    open(path, "w").write(text)


def load_list_lines():
    """(kind, name, address or None) per line; kind is "hle" or "hook" (a line
    `hook name`). The address is for functions whose retail start retail_symbols.json does
    not know (a line `name 0x28eda0`)."""
    rows = []
    if os.path.exists(HLE_LIST):
        for line in open(HLE_LIST):
            parts = line.split("#")[0].split()
            kind = "hle"
            if parts and parts[0] == "hook":
                kind = "hook"
                parts = parts[1:]
            if parts:
                rows.append((kind, parts[0], int(parts[1], 16) if len(parts) > 1 else None))
    return rows


def load_hle_lines():
    return [(n, a) for k, n, a in load_list_lines() if k == "hle"]


def load_hle():
    return {name for name, _addr in load_hle_lines()}


def load_hooks():
    """Functions whose recompiled body stays, but is reached through
    HOOK_<name>(Ctx &, void (*orig)(Ctx &)): the host code runs first, last or
    instead of the original (recomp/runtime/pcmenu.cpp)."""
    return {n for k, n, a in load_list_lines() if k == "hook"}
SYMS = os.path.join(REPO, "recomp", "symbols", "retail_symbols.json")
OUT = os.path.join(REPO, "work", "recomp", "gen")
FUNCS_PER_FILE = 400


def load():
    blob, secs = match_func.load_elf(RETAIL)
    t = secs[".text"]
    words = [struct.unpack_from("<I", blob, t["off"] + i)[0] for i in range(0, t["size"], 4)]
    data_words = []                 # (section, word)
    for name in (".rodata", ".data", ".sdata"):
        s = secs.get(name)
        if s:
            for i in range(0, s["size"] - 3, 4):
                data_words.append((name, struct.unpack_from("<I", blob, s["off"] + i)[0]))
    return t["addr"], words, data_words


def is_prologue(w):
    return (w & 0xFFFF0000) == 0x27BD0000 and (w & 0x8000)     # addiu sp,sp,-N


def find_functions(base, words, data_words):
    end = base + len(words) * 4
    names = {}
    for r in json.load(open(SYMS)):
        if r["how"] == "neighbour" and r.get("ratio", 1) < 0.5:
            continue
        names[int(r["addr"], 16)] = r["name"]
    for _kind, name, addr in load_list_lines():
        if addr is not None:
            names = {a: n for a, n in names.items() if n != name}
            names[addr] = name
    starts = set(names)
    starts.add(0x100008)
    definite = {0x100008}
    for i, w in enumerate(words):
        ins = decode(w)
        if ins.name == "jal":
            definite.add(((base + i * 4) & 0xF0000000) | ins.target)
    starts |= definite
    def after_return(a):
        # the previous function ended with `jr ra; <delay slot>` (maybe nop padding)
        k = (a - base) // 4
        return any(k - j >= 0 and words[k - j] == 0x03E00008 for j in (2, 3, 4)) and \
            all(words[k - i] == 0 for i in range(1, next((j for j in (2, 3, 4)
                                                          if words[k - j] == 0x03E00008), 2) - 1))

    def entry_like(a):
        return is_prologue(words[(a - base) // 4]) or after_return(a)

    def after_jump(a):
        # previous code ends in an unconditional jump plus delay slot (maybe nops)
        k = (a - base) // 4
        for pad in range(4):
            j = k - 2 - pad
            if j < 0 or any(words[k - 1 - p] for p in range(pad)):
                break
            prev = decode(words[j])
            if prev.name in ("jr", "j") or words[j] >> 16 == 0x1000:     # b = beq zero,zero
                return True
        return False

    def frameless_region(a, limit=512):
        """End of a function that starts at `a` without a prologue: follow its
        branches until every path returns with `jr ra`. None if it leaves
        the area, sets up a frame or grows past `limit` instructions."""
        todo, seen, top = [a], set(), a
        while todo:
            pc = todo.pop()
            while True:
                if pc in seen:
                    break
                if not (base <= pc < end) or pc - a > limit * 4 or pc < a:
                    return None
                seen.add(pc)
                w = words[(pc - base) // 4]
                ins = decode(w)
                top = max(top, pc + 4)
                if pc != a and is_prologue(w):
                    return None
                if w == 0x03E00008:                     # jr ra: path ends after the delay slot
                    top = max(top, pc + 8)
                    break
                if ins.name in BRANCHES:
                    todo.append(pc + 4 + ins.simm * 4)
                    if w >> 16 == 0x1000:               # b: no fall-through
                        top = max(top, pc + 8)
                        break
                elif ins.name == "j":
                    t = ((pc + 4) & 0xF0000000) | ins.target
                    if a <= t < a + limit * 4:
                        todo.append(t)
                    top = max(top, pc + 8)          # a far `j` is a tail call: path ends
                    break
                elif ins.name in ("jr", "jalr") and ins.name == "jr":
                    return None                         # computed jump: not a simple method
                pc += 4
        return top

    # code pointers in data. .data/.sdata hold vtables and the static constructor
    # list (entries). .rodata holds switch tables, but also some vtables whose
    # small methods (`jr ra; nop` and the like) have no prologue. Those must not
    # split the function around them - splitting switches at case labels broke
    # cMC::MCSync - so they become extra entries: a separate copy of the leaf
    # code, reachable only through the indirect-call table.
    extra = set()
    for sec, w in data_words:
        if base <= w < end and w % 4 == 0 and w not in starts:
            if is_prologue(words[(w - base) // 4]) or (sec != ".rodata" and after_jump(w)):
                starts.add(w)
                definite.add(w)
            elif sec == ".rodata" and after_jump(w) and frameless_region(w):
                extra.add(w)
    # code pointers built in code with lui/addiu (callbacks, interrupt handlers)
    hi = {}
    for i, w in enumerate(words):
        ins = decode(w)
        if ins.name == "lui":
            hi[ins.rt] = ins.imm << 16
        elif ins.name == "addiu" and ins.rs in hi:
            t = (hi[ins.rs] + ins.simm) & 0xFFFFFFFF
            if base <= t < end and t % 4 == 0 and entry_like(t):
                starts.add(t)
        elif ins.name in ("jr", "j"):
            hi.clear()
    # GCC emits `addiu sp,sp,-N` only as a prologue: every one starts a function
    # (static constructor tables, for one, are only built at run time)
    for i, w in enumerate(words):
        if is_prologue(w):
            starts.add(base + i * 4)
    starts = {s for s in starts if base <= s < end}
    starts = fix_boundaries(base, words, starts, definite)
    extra = {e for e in extra if e not in starts}

    return sorted(starts), names, {e: frameless_region(e) for e in extra}


def fix_boundaries(base, words, starts, definite):
    """Make every branch stay inside its function.

    A conditional branch that crosses a start means that start is false (it was
    guessed by position) - merge it away unless something calls it directly. A
    `j` into the middle of other code is a tail jump: its target is an entry.
    """
    for _ in range(20):
        st = sorted(starts)
        changed = False
        for i, w in enumerate(words):
            ins = decode(w)
            if ins.name not in BRANCHES and ins.name != "j":
                continue
            a = base + i * 4
            t = ((a + 4) & 0xF0000000) | ins.target if ins.name == "j" else a + 4 + ins.simm * 4
            if t in starts:
                continue
            k = bisect.bisect_right(st, a) - 1
            lo = st[k] if k >= 0 else base
            hi = st[k + 1] if k + 1 < len(st) else 1 << 32
            if lo <= t < hi:
                continue
            if ins.name == "j" and not (min(a, t) < hi and max(a, t) >= lo and abs(t - a) < 0x400):
                starts.add(t)
                changed = True
                continue
            between = [s for s in st if min(a, t) < s <= max(a, t)]
            removable = [s for s in between if s not in definite]
            if removable and len(removable) == len(between):
                starts.difference_update(removable)
            else:
                starts.add(t)
            changed = True
        if not changed:
            break
    return starts


def cname(addr):
    return f"F_{addr:08x}"


# Loops that only wait for an interrupt to change a variable: they sleep instead of spinning (ps2_idle).
# WaitUntilVSync, eWaitUntilRenderingDone twice: 75% of the game thread in a race at 30 fps.
# MainLoop's wait for the next tick: 48% on a movie.
IDLE_LOOPS = {0x111d90, 0x111e00, 0x111f0c, 0x12f648}


class FuncGen:
    def __init__(self, base, words, start, stop, funcs, jt_labels, names, hle=frozenset(), hooks=frozenset()):
        self.hle = hle
        self.hooks = hooks
        self.base, self.words = base, words
        self.start, self.stop = start, stop
        self.funcs, self.jt = funcs, jt_labels
        self.names = names
        self.out = []
        self.unimpl = set()

    def w(self, a):
        return self.words[(a - self.base) // 4]

    def inside(self, a):
        return self.start <= a < self.stop

    # ---- operand helpers -------------------------------------------------
    @staticmethod
    def set32(r, e):
        return "" if r == 0 else f"c.r[{r}].d[0] = (u64)(s64)(s32)({e});"

    @staticmethod
    def set64(r, e):
        return "" if r == 0 else f"c.r[{r}].d[0] = (u64)({e});"

    def stmt(self, a, ins):
        """C++ for one non-control-flow instruction."""
        n, rs, rt, rd, sa = ins.name, ins.rs, ins.rt, ins.rd, ins.sa
        RS, RT = f"c.r[{rs}]", f"c.r[{rt}]"
        S64, T64 = f"{RS}.d[0]", f"{RT}.d[0]"
        S32, T32 = f"{RS}.w[0]", f"{RT}.w[0]"
        imm, simm = ins.imm, ins.simm
        ea = f"(u32)({S32} + {simm})"
        s32, s64 = self.set32, self.set64
        tbl = {
            "nop": "", "sync": "", "pref": "", "cache": "",
            "ei": "op_ei(c);", "di": "op_di(c);",       # Status.EIE: interrupts wait while it is clear
            "sll": s32(rd, f"{T32} << {sa}"),
            "srl": s32(rd, f"{T32} >> {sa}"),
            "sra": s32(rd, f"(s32){T32} >> {sa}"),
            "sllv": s32(rd, f"{T32} << ({S32} & 31)"),
            "srlv": s32(rd, f"{T32} >> ({S32} & 31)"),
            "srav": s32(rd, f"(s32){T32} >> ({S32} & 31)"),
            "dsll": s64(rd, f"{T64} << {sa}"),
            "dsrl": s64(rd, f"{T64} >> {sa}"),
            "dsra": s64(rd, f"(s64){T64} >> {sa}"),
            "dsll32": s64(rd, f"{T64} << {sa + 32}"),
            "dsrl32": s64(rd, f"{T64} >> {sa + 32}"),
            "dsra32": s64(rd, f"(s64){T64} >> {sa + 32}"),
            "dsllv": s64(rd, f"{T64} << ({S32} & 63)"),
            "dsrlv": s64(rd, f"{T64} >> ({S32} & 63)"),
            "dsrav": s64(rd, f"(s64){T64} >> ({S32} & 63)"),
            "movz": "" if rd == 0 else f"if ({T64} == 0) c.r[{rd}].d[0] = {S64};",
            "movn": "" if rd == 0 else f"if ({T64} != 0) c.r[{rd}].d[0] = {S64};",
            "mfhi": s64(rd, "c.hi"), "mflo": s64(rd, "c.lo"),
            "mthi": f"c.hi = {S64};", "mtlo": f"c.lo = {S64};",
            "mfhi1": s64(rd, "c.hi1"), "mflo1": s64(rd, "c.lo1"),
            "mthi1": f"c.hi1 = {S64};", "mtlo1": f"c.lo1 = {S64};",
            "mfsa": s64(rd, "c.sa"), "mtsa": f"c.sa = {S64};",
            "mtsab": f"c.sa = (({S32} & 15) ^ ({imm} & 15)) * 8;",
            "mtsah": f"c.sa = (({S32} & 7) ^ ({imm} & 7)) * 16;",
            "mult": f"op_mult(c, {rd}, {rs}, {rt}, 0);",
            "multu": f"op_multu(c, {rd}, {rs}, {rt}, 0);",
            "mult1": f"op_mult(c, {rd}, {rs}, {rt}, 1);",
            "multu1": f"op_multu(c, {rd}, {rs}, {rt}, 1);",
            "madd": f"op_madd(c, {rd}, {rs}, {rt}, 0);",
            "maddu": f"op_maddu(c, {rd}, {rs}, {rt}, 0);",
            "madd1": f"op_madd(c, {rd}, {rs}, {rt}, 1);",
            "maddu1": f"op_maddu(c, {rd}, {rs}, {rt}, 1);",
            "div": f"op_div(c, {rs}, {rt}, 0);", "divu": f"op_divu(c, {rs}, {rt}, 0);",
            "div1": f"op_div(c, {rs}, {rt}, 1);", "divu1": f"op_divu(c, {rs}, {rt}, 1);",
            "add": s32(rd, f"{S32} + {T32}"), "addu": s32(rd, f"{S32} + {T32}"),
            "sub": s32(rd, f"{S32} - {T32}"), "subu": s32(rd, f"{S32} - {T32}"),
            "dadd": s64(rd, f"{S64} + {T64}"), "daddu": s64(rd, f"{S64} + {T64}"),
            "dsub": s64(rd, f"{S64} - {T64}"), "dsubu": s64(rd, f"{S64} - {T64}"),
            "and": s64(rd, f"{S64} & {T64}"), "or": s64(rd, f"{S64} | {T64}"),
            "xor": s64(rd, f"{S64} ^ {T64}"), "nor": s64(rd, f"~({S64} | {T64})"),
            "slt": s64(rd, f"(s64){S64} < (s64){T64}"),
            "sltu": s64(rd, f"{S64} < {T64}"),
            "addi": s32(rt, f"{S32} + {simm}"), "addiu": s32(rt, f"{S32} + {simm}"),
            "daddi": s64(rt, f"{S64} + {simm}"), "daddiu": s64(rt, f"{S64} + {simm}"),
            "slti": s64(rt, f"(s64){S64} < {simm}"),
            "sltiu": s64(rt, f"{S64} < (u64)(s64){simm}"),
            "andi": s64(rt, f"{S64} & {imm}"), "ori": s64(rt, f"{S64} | {imm}"),
            "xori": s64(rt, f"{S64} ^ {imm}"),
            "lui": s32(rt, f"0x{imm << 16:x}u"),
            "lb": s64(rt, f"(s64)(s8)LB({ea})"), "lbu": s64(rt, f"LB({ea})"),
            "lh": s64(rt, f"(s64)(s16)LH({ea})"), "lhu": s64(rt, f"LH({ea})"),
            "lw": s64(rt, f"(s64)(s32)LW({ea})"), "lwu": s64(rt, f"LW({ea})"),
            "ld": s64(rt, f"LD({ea})"),
            "lq": "" if rt == 0 else f"c.r[{rt}].q = LQ({ea});",
            "sb": f"SB({ea}, (u8){T32});", "sh": f"SH({ea}, (u16){T32});",
            "sw": f"SW({ea}, {T32});", "sd": f"SD({ea}, {T64});",
            "sq": f"SQ({ea}, {RT}.q);",
            "lwl": f"op_lwl(c, {rt}, {ea});", "lwr": f"op_lwr(c, {rt}, {ea});",
            "ldl": f"op_ldl(c, {rt}, {ea});", "ldr": f"op_ldr(c, {rt}, {ea});",
            "swl": f"op_swl(c, {rt}, {ea});", "swr": f"op_swr(c, {rt}, {ea});",
            "sdl": f"op_sdl(c, {rt}, {ea});", "sdr": f"op_sdr(c, {rt}, {ea});",
            "lwc1": f"c.f[{rt}].u = LW({ea});", "swc1": f"SW({ea}, c.f[{rt}].u);",
            "lqc2": f"op_lqc2(c, {rt}, {ea});", "sqc2": f"SQ({ea}, c.vf[{rt}].q);",
            "mfc1": s32(rt, f"c.f[{rd}].u"), "mtc1": f"c.f[{rd}].u = {T32};",
            "cfc1": s32(rt, f"op_cfc1(c, {rd})"), "ctc1": f"op_ctc1(c, {rd}, {T32});",
            "mfc0": s32(rt, f"op_mfc0(c, {rd})"), "mtc0": f"op_mtc0(c, {rd}, {T32});",
            "qmfc2": "" if rt == 0 else f"c.r[{rt}].q = c.vf[{rd}].q;",
            "qmtc2": f"op_qmtc2(c, {rd}, {RT}.q);",
            "cfc2": s32(rt, f"op_cfc2(c, {rd})"), "ctc2": f"op_ctc2(c, {rd}, {T32});",
            "syscall": f"ps2_syscall(c, 0x{a:x});",
            "break": f"ps2_break(c, 0x{a:x});",
            "eret": f"ps2_unimpl(c, 0x{a:x}, 0x{ins.word:08x});",
            "teq": "", "tne": "", "tge": "", "tgeu": "", "tlt": "", "tltu": "",
        }
        if n in tbl:
            return tbl[n]
        if n.endswith(".s") or n == "cvt.s.w":
            return f"FPU_{n.replace('.', '_')}(c, {sa}, {rd}, {rt});"
        if n.startswith("v"):
            return f"VU_{n}(c, 0x{ins.word:08x}u);"
        if n.startswith("p") or n == "qfsrv":
            return f"MMI_{n}(c, {rd}, {rs}, {rt}, {sa});"
        self.unimpl.add(n)
        return f"ps2_unimpl(c, 0x{a:x}, 0x{ins.word:08x});"

    def cond(self, ins):
        n, S, T = ins.name, f"c.r[{ins.rs}].d[0]", f"c.r[{ins.rt}].d[0]"
        return {
            "beq": f"{S} == {T}", "beql": f"{S} == {T}",
            "bne": f"{S} != {T}", "bnel": f"{S} != {T}",
            "blez": f"(s64){S} <= 0", "blezl": f"(s64){S} <= 0",
            "bgtz": f"(s64){S} > 0", "bgtzl": f"(s64){S} > 0",
            "bltz": f"(s64){S} < 0", "bltzl": f"(s64){S} < 0", "bltzal": f"(s64){S} < 0",
            "bgez": f"(s64){S} >= 0", "bgezl": f"(s64){S} >= 0", "bgezal": f"(s64){S} >= 0",
            "bc1f": "!(c.fcr31 & FCR31_C)", "bc1fl": "!(c.fcr31 & FCR31_C)",
            "bc1t": "(c.fcr31 & FCR31_C)", "bc1tl": "(c.fcr31 & FCR31_C)",
            # CPCOND0: DMA done (always), CPCOND2: VU0 busy (never)
            "bc0": "((0x%x & 1) != 0)" % ins.rt,
            "bc2f": "true", "bc2fl": "true", "bc2t": "false", "bc2tl": "false",
        }[n]

    def goto(self, target, src=None):
        if self.inside(target):
            # loop back-edges also take interrupts: games spin on VBlank counters
            if src is not None and target <= src:
                if target in IDLE_LOOPS:
                    return f"{{ ps2_idle(c); goto L_{target:x}; }}"
                return f"{{ if (g_irq_pending) ps2_take_interrupts(c); goto L_{target:x}; }}"
            return f"goto L_{target:x};"
        return f"{self.call(target)} return;"

    def call(self, target):
        if target in self.funcs:
            return f"{cname(target)}(c);"
        return f"ps2_call(c, 0x{target:x});"

    def gen(self):
        labels = set(self.jt)
        a = self.start
        while a < self.stop:
            ins = decode(self.w(a))
            if ins.name in BRANCHES:
                t = a + 4 + ins.simm * 4
                if self.inside(t):
                    labels.add(t)
            elif ins.name == "j":
                t = ((a + 4) & 0xF0000000) | ins.target
                if self.inside(t):
                    labels.add(t)
            a += 4
        name = self.names.get(self.start, "")
        o = self.out
        hooked = name in self.hooks
        o.append(f"// {name}" if name else f"// sub_{self.start:08x}")
        if name in self.hle:
            o.append(f"void {cname(self.start)}(Ctx &c) {{ HLE_{name}(c); }}\n")
            return o
        # restrict: guest memory is never the Ctx, so a register stays known across a load or a store
        o.append(f"void {cname(self.start)}{'_orig' if hooked else ''}(Ctx &__restrict c)\n{{")
        # interrupts are taken at function entry (see ps2_take_interrupts)
        o.append("    if (g_irq_pending) ps2_take_interrupts(c);")
        a = self.start
        while a < self.stop:
            if a in labels:
                o.append(f"L_{a:x}:;")
            ins = decode(self.w(a))
            n = ins.name
            is_ctl = n in BRANCHES or n in ("j", "jal", "jr", "jalr")
            if not is_ctl:
                o.append("    " + self.stmt(a, ins))
                a += 4
                continue
            ds_addr = a + 4
            ds = self.stmt(ds_addr, decode(self.w(ds_addr))) if ds_addr < self.stop else ""
            after = a + 8
            if n in BRANCHES:
                t = a + 4 + ins.simm * 4
                link = n in ("bltzal", "bgezal")
                linkst = f"c.r[31].d[0] = 0x{after:x};" if link else ""
                if n in LIKELY:
                    o.append(f"    if ({self.cond(ins)}) {{ {ds} {self.goto(t, a)} }}")
                elif link:
                    o.append(f"    {{ bool k = {self.cond(ins)}; {ds} {linkst} "
                             f"if (k) {{ {self.call(t)} }} }}")
                else:
                    o.append(f"    {{ bool k = {self.cond(ins)}; {ds} if (k) {self.goto(t, a)} }}")
            elif n == "j":
                t = ((a + 4) & 0xF0000000) | ins.target
                o.append(f"    {ds} {self.goto(t, a)}")
            elif n == "jal":
                t = ((a + 4) & 0xF0000000) | ins.target
                o.append(f"    {ds} c.r[31].d[0] = 0x{after:x}; {self.call(t)}")
            elif n == "jr":
                if ins.rs == 31:
                    o.append(f"    {ds} return;")
                else:
                    cases = " ".join(f"case 0x{x:x}: goto L_{x:x};" for x in sorted(self.jt))
                    o.append(f"    {{ u32 t = c.r[{ins.rs}].w[0]; {ds} switch (t) {{ {cases} "
                             f"default: ps2_call(c, t); return; }} }}")
            elif n == "jalr":
                rd = ins.rd
                link = f"c.r[{rd}].d[0] = 0x{after:x};" if rd else ""
                o.append(f"    {{ u32 t = c.r[{ins.rs}].w[0]; {ds} {link} ps2_call(c, t); }}")
            if ds_addr in labels:
                # something jumps straight into the delay slot
                o.append(f"    goto L_{after:x};" if after < self.stop else "    return;")
                o.append(f"L_{ds_addr:x}:;")
                o.append("    " + ds)
                labels.add(after)
            a = after
        nxt = self.stop
        o.append(f"    {self.call(nxt)} // falls through")
        o.append("}\n")
        # labels referenced after the fact (delay slot fallthrough) must exist
        text = "\n".join(o)
        for lab in sorted(labels):
            if f"L_{lab:x}:;" not in text and f"goto L_{lab:x};" in text:
                o.insert(-2, f"L_{lab:x}:; return;")
        if hooked:
            o.append(f"void HOOK_{name}(Ctx &c, void (*orig)(Ctx &));")
            o.append(f"void {cname(self.start)}(Ctx &c) {{ HOOK_{name}(c, {cname(self.start)}_orig); }}\n")
        return o


def main():
    base, words, data_words = load()
    end = base + len(words) * 4
    starts, names, extra = find_functions(base, words, data_words)
    sset = set(starts)
    # jump-table labels: code pointers in data that land inside a function
    jt = {}
    for _sec, w in data_words:
        if base <= w < end and w % 4 == 0 and w not in sset:
            i = bisect.bisect_right(starts, w) - 1
            if i >= 0:
                jt.setdefault(starts[i], set()).add(w)
    os.makedirs(OUT, exist_ok=True)
    hle = load_hle()
    hooks = load_hooks()
    missing = (hle | hooks) - {names[s] for s in starts if s in names}
    if missing:
        print("HLE names not found among retail functions:", " ".join(sorted(missing)))
    unimpl = set()
    decls = []
    files = 0
    for k in range(0, len(starts), FUNCS_PER_FILE):
        chunk = starts[k:k + FUNCS_PER_FILE]
        body = ['#include "ps2.h"', '#include "funcs.h"', ""]
        for i, s in enumerate(chunk):
            stop = starts[k + i + 1] if k + i + 1 < len(starts) else end
            g = FuncGen(base, words, s, stop, sset, jt.get(s, set()), names, hle, hooks)
            body += g.gen()
            unimpl |= g.unimpl
        write_if_changed(os.path.join(OUT, f"code_{files:03d}.cpp"), "\n".join(body))
        files += 1
    # extra entries: copies of frameless methods that vtables in .rodata point into
    body = ['#include "ps2.h"', '#include "funcs.h"', ""]
    for e, stop in sorted(extra.items()):
        body += FuncGen(base, words, e, stop, sset, set(), names, hle, hooks).gen()
    write_if_changed(os.path.join(OUT, f"code_{files:03d}.cpp"), "\n".join(body))
    files += 1
    # an earlier run may have written more chunks: empty them so nothing is defined twice
    stale = files
    while os.path.exists(os.path.join(OUT, f"code_{stale:03d}.cpp")):
        write_if_changed(os.path.join(OUT, f"code_{stale:03d}.cpp"), "// unused chunk\n")
        stale += 1
    entries = sorted(set(starts) | set(extra))
    for s in entries:
        decls.append(f"void {cname(s)}(Ctx &c);")
    decls += [f"void HLE_{n}(Ctx &c);" for n in sorted(hle)]
    write_if_changed(os.path.join(OUT, "funcs.h"),
                     "#pragma once\n#include \"ps2.h\"\n" + "\n".join(decls) + "\n")
    tab = ['#include "ps2.h"', '#include "funcs.h"', "",
           "const FuncEntry g_functable[] = {"]
    tab += [f"    {{0x{s:x}, {cname(s)}}}," for s in entries]
    tab += ["};", f"const int g_functable_size = {len(entries)};", ""]
    write_if_changed(os.path.join(OUT, "functable.cpp"), "\n".join(tab))
    print(f"functions {len(starts)} (+{len(extra)} leaf entries)  files {files}  jump-table funcs {len(jt)}  "
          f"named {sum(1 for s in starts if s in names)}")
    if unimpl:
        print("unimplemented:", " ".join(sorted(unimpl)))


if __name__ == "__main__":
    main()
