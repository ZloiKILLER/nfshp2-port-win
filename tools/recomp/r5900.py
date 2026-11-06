"""R5900 (PS2 EE) instruction decoder.

decode(word) -> Insn with a mnemonic and the raw fields. Only what the
recompiler needs: names follow the EE Core Instruction Set Manual.
"""

from dataclasses import dataclass

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x0C: "syscall", 0x0D: "break",
    0x0F: "sync", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x14: "dsllv",
    0x16: "dsrlv", 0x17: "dsrav", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or",
    0x26: "xor", 0x27: "nor", 0x28: "mfsa", 0x29: "mtsa", 0x2A: "slt", 0x2B: "sltu",
    0x2C: "dadd", 0x2D: "daddu", 0x2E: "dsub", 0x2F: "dsubu", 0x30: "tge", 0x31: "tgeu",
    0x32: "tlt", 0x33: "tltu", 0x34: "teq", 0x36: "tne", 0x38: "dsll", 0x3A: "dsrl",
    0x3B: "dsra", 0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32",
}
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x10: "bltzal",
          0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall", 0x18: "mtsab", 0x19: "mtsah"}
OPS = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
    0x0E: "xori", 0x0F: "lui", 0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl",
    0x18: "daddi", 0x19: "daddiu", 0x1A: "ldl", 0x1B: "ldr", 0x1E: "lq", 0x1F: "sq",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x27: "lwu", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2C: "sdl", 0x2D: "sdr",
    0x2E: "swr", 0x2F: "cache", 0x31: "lwc1", 0x33: "pref", 0x36: "lqc2", 0x37: "ld",
    0x39: "swc1", 0x3E: "sqc2", 0x3F: "sd",
}
MMI = {0x00: "madd", 0x01: "maddu", 0x04: "plzcw", 0x10: "mfhi1", 0x11: "mthi1",
       0x12: "mflo1", 0x13: "mtlo1", 0x18: "mult1", 0x19: "multu1", 0x1A: "div1",
       0x1B: "divu1", 0x20: "madd1", 0x21: "maddu1", 0x30: "pmfhl", 0x31: "pmthl",
       0x34: "psllh", 0x36: "psrlh", 0x37: "psrah", 0x3C: "psllw", 0x3E: "psrlw", 0x3F: "psraw"}
MMI0 = {0x00: "paddw", 0x01: "psubw", 0x02: "pcgtw", 0x03: "pmaxw", 0x04: "paddh",
        0x05: "psubh", 0x06: "pcgth", 0x07: "pmaxh", 0x08: "paddb", 0x09: "psubb",
        0x0A: "pcgtb", 0x10: "paddsw", 0x11: "psubsw", 0x12: "pextlw", 0x13: "ppacw",
        0x14: "paddsh", 0x15: "psubsh", 0x16: "pextlh", 0x17: "ppach", 0x18: "paddsb",
        0x19: "psubsb", 0x1A: "pextlb", 0x1B: "ppacb", 0x1E: "pext5", 0x1F: "ppac5"}
MMI1 = {0x01: "pabsw", 0x02: "pceqw", 0x03: "pminw", 0x04: "padsbh", 0x05: "pabsh",
        0x06: "pceqh", 0x07: "pminh", 0x0A: "pceqb", 0x10: "padduw", 0x11: "psubuw",
        0x12: "pextuw", 0x14: "padduh", 0x15: "psubuh", 0x16: "pextuh", 0x18: "paddub",
        0x19: "psubub", 0x1A: "pextub", 0x1B: "qfsrv"}
MMI2 = {0x00: "pmaddw", 0x02: "psllvw", 0x03: "psrlvw", 0x04: "pmsubw", 0x08: "pmfhi",
        0x09: "pmflo", 0x0A: "pinth", 0x0C: "pmultw", 0x0D: "pdivw", 0x0E: "pcpyld",
        0x10: "pmaddh", 0x11: "phmadh", 0x12: "pand", 0x13: "pxor", 0x14: "pmsubh",
        0x15: "phmsbh", 0x1A: "pexeh", 0x1B: "prevh", 0x1C: "pmulth", 0x1D: "pdivbw",
        0x1E: "pexew", 0x1F: "prot3w"}
MMI3 = {0x00: "pmadduw", 0x03: "psravw", 0x08: "pmthi", 0x09: "pmtlo", 0x0A: "pinteh",
        0x0C: "pmultuw", 0x0D: "pdivuw", 0x0E: "pcpyud", 0x12: "por", 0x13: "pnor",
        0x1A: "pexch", 0x1B: "pcpyh", 0x1E: "pexcw"}
COP1_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s",
          0x05: "abs.s", 0x06: "mov.s", 0x07: "neg.s", 0x16: "rsqrt.s", 0x18: "adda.s",
          0x19: "suba.s", 0x1A: "mula.s", 0x1C: "madd.s", 0x1D: "msub.s", 0x1E: "madda.s",
          0x1F: "msuba.s", 0x24: "cvt.w.s", 0x28: "max.s", 0x29: "min.s", 0x30: "c.f.s",
          0x32: "c.eq.s", 0x34: "c.lt.s", 0x36: "c.le.s"}
# VU0 macro mode, COP2 with bit 25 set: special1 by funct, special2 by (fd<<2|funct&3)
VU_SPECIAL1 = {
    0x00: "vaddx", 0x01: "vaddy", 0x02: "vaddz", 0x03: "vaddw", 0x04: "vsubx", 0x05: "vsuby",
    0x06: "vsubz", 0x07: "vsubw", 0x08: "vmaddx", 0x09: "vmaddy", 0x0A: "vmaddz",
    0x0B: "vmaddw", 0x0C: "vmsubx", 0x0D: "vmsuby", 0x0E: "vmsubz", 0x0F: "vmsubw",
    0x10: "vmaxx", 0x11: "vmaxy", 0x12: "vmaxz", 0x13: "vmaxw", 0x14: "vminix",
    0x15: "vminiy", 0x16: "vminiz", 0x17: "vminiw", 0x18: "vmulx", 0x19: "vmuly",
    0x1A: "vmulz", 0x1B: "vmulw", 0x1C: "vmulq", 0x1D: "vmaxi", 0x1E: "vmuli", 0x1F: "vminii",
    0x20: "vaddq", 0x21: "vmaddq", 0x22: "vaddi", 0x23: "vmaddi", 0x24: "vsubq",
    0x25: "vmsubq", 0x26: "vsubi", 0x27: "vmsubi", 0x28: "vadd", 0x29: "vmadd", 0x2A: "vmul",
    0x2B: "vmax", 0x2C: "vsub", 0x2D: "vmsub", 0x2E: "vopmsub", 0x2F: "vmini", 0x30: "viadd",
    0x31: "visub", 0x32: "viaddi", 0x34: "viand", 0x35: "vior", 0x38: "vcallms",
    0x39: "vcallmsr",
}
VU_SPECIAL2 = {
    0x00: "vaddax", 0x01: "vadday", 0x02: "vaddaz", 0x03: "vaddaw", 0x04: "vsubax",
    0x05: "vsubay", 0x06: "vsubaz", 0x07: "vsubaw", 0x08: "vmaddax", 0x09: "vmadday",
    0x0A: "vmaddaz", 0x0B: "vmaddaw", 0x0C: "vmsubax", 0x0D: "vmsubay", 0x0E: "vmsubaz",
    0x0F: "vmsubaw", 0x10: "vitof0", 0x11: "vitof4", 0x12: "vitof12", 0x13: "vitof15",
    0x14: "vftoi0", 0x15: "vftoi4", 0x16: "vftoi12", 0x17: "vftoi15", 0x18: "vmulax",
    0x19: "vmulay", 0x1A: "vmulaz", 0x1B: "vmulaw", 0x1C: "vmulaq", 0x1D: "vabs",
    0x1E: "vmulai", 0x1F: "vclipw", 0x20: "vaddaq", 0x21: "vmaddaq", 0x22: "vaddai",
    0x23: "vmaddai", 0x24: "vsubaq", 0x25: "vmsubaq", 0x26: "vsubai", 0x27: "vmsubai",
    0x28: "vadda", 0x29: "vmadda", 0x2A: "vmula", 0x2C: "vsuba", 0x2D: "vmsuba",
    0x2E: "vopmula", 0x2F: "vnop", 0x30: "vmove", 0x31: "vmr32", 0x34: "vlqi", 0x35: "vsqi",
    0x36: "vlqd", 0x37: "vsqd", 0x38: "vdiv", 0x39: "vsqrt", 0x3A: "vrsqrt", 0x3B: "vwaitq",
    0x3C: "vmtir", 0x3D: "vmfir", 0x3E: "vilwr", 0x3F: "viswr", 0x40: "vrnext", 0x41: "vrget",
    0x42: "vrinit", 0x43: "vrxor",
}


@dataclass
class Insn:
    word: int
    name: str
    op: int
    rs: int
    rt: int
    rd: int
    sa: int
    funct: int

    @property
    def imm(self):
        return self.word & 0xFFFF

    @property
    def simm(self):
        v = self.word & 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    @property
    def target(self):
        return (self.word & 0x3FFFFFF) << 2


def decode(w):
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    rd, sa, funct = (w >> 11) & 31, (w >> 6) & 31, w & 63
    name = None
    if op == 0:
        name = SPECIAL.get(funct)
        if w == 0:
            name = "nop"
    elif op == 1:
        name = REGIMM.get(rt)
    elif op == 0x10:
        name = {0x00: "mfc0", 0x04: "mtc0"}.get(rs)
        if rs == 0x10:
            name = {0x18: "eret", 0x38: "ei", 0x39: "di"}.get(funct, "cop0")
        elif rs == 0x08:
            name = "bc0"
    elif op == 0x11:
        if rs == 0x00:
            name = "mfc1"
        elif rs == 0x02:
            name = "cfc1"
        elif rs == 0x04:
            name = "mtc1"
        elif rs == 0x06:
            name = "ctc1"
        elif rs == 0x08:
            name = ("bc1f", "bc1t", "bc1fl", "bc1tl")[rt & 3]
        elif rs == 0x10:
            name = COP1_S.get(funct)
        elif rs == 0x14 and funct == 0x20:
            name = "cvt.s.w"
    elif op == 0x12:
        if rs & 0x10:
            if funct >= 0x3C:
                name = VU_SPECIAL2.get(((funct & 3) | (sa << 2)) if False else (((w >> 6) & 0x1F) << 2 | (funct & 3)))
            else:
                name = VU_SPECIAL1.get(funct)
        else:
            name = {0x01: "qmfc2", 0x02: "cfc2", 0x05: "qmtc2", 0x06: "ctc2"}.get(rs)
            if rs == 0x08:
                name = ("bc2f", "bc2t", "bc2fl", "bc2tl")[rt & 3]
    elif op == 0x1C:
        if funct == 0x08:
            name = MMI0.get(sa)
        elif funct == 0x28:
            name = MMI1.get(sa)
        elif funct == 0x09:
            name = MMI2.get(sa)
        elif funct == 0x29:
            name = MMI3.get(sa)
        else:
            name = MMI.get(funct)
    else:
        name = OPS.get(op)
    return Insn(w, name or "unknown", op, rs, rt, rd, sa, funct)


BRANCHES = {"beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl", "bltz", "bgez",
            "bltzl", "bgezl", "bltzal", "bgezal", "bc1f", "bc1t", "bc1fl", "bc1tl",
            "bc0", "bc2f", "bc2t", "bc2fl", "bc2tl"}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bc1fl", "bc1tl", "bc2fl", "bc2tl"}
JUMPS = {"j", "jal", "jr", "jalr"}
