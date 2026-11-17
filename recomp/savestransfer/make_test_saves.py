#!/usr/bin/env python3
"""Makes one save of the game in every format savestransfer.exe reads, for its test (test_savestransfer.py).

    python make_test_saves.py <save.dat> <out folder>

Written from the formats as mymc+ writes them, independently of savestransfer.cpp: card images (with and without ECC bytes, the files'
clusters out of order), a PCSX2 folder card, a save's own folder, .psu, .max (an LZARI encoder after Okumura's LZARI.C), .cbs (zlib + RC4),
and saves that must be refused (a pre-release NFS6 save, a save of the wrong length, another game).
"""
import os
import struct
import sys
import zlib

USA, EUR = "BASLUS-20362HP2", "BESLES-50731HP2"
TOD = bytes([0, 30, 45, 21, 3, 11, 0, 0])[:6] + struct.pack("<H", 2026)     # 2026-11-03 21:45:30 JST = 12:45:30 UTC


def dirent(mode, length, cluster, name, tod=TOD):
    e = struct.pack("<HHL8sLL8sL", mode, 0, length, tod, cluster, 0, tod, 0)
    return (e + b"\0" * 28 + name.encode() + b"\0" * 448)[:512]


D_DIR, D_FILE = 0x8427, 0x8497


# ---- card image ---------------------------------------------------------------------------------------------
def card(dirs, ecc):
    """dirs: [(name, [(file name, bytes)])]; clusters handed out from the top down, so every chain runs backwards."""
    page, ppc, cpc, alloc_off, alloc_end = 512, 2, 8192, 41, 8000      # clusters 8 (indirect) and 9-40 (FAT) before the allocatable ones
    cs = page * ppc
    clusters = {}
    fat = {}
    free = [alloc_end - 1]

    def alloc(data):
        n = max(1, (len(data) + cs - 1) // cs)
        chain = []
        for _ in range(n):
            chain.append(free[0])
            free[0] -= 1
        for i, c in enumerate(chain):
            clusters[c] = data[i * cs:(i + 1) * cs].ljust(cs, b"\0")
            fat[c] = 0x80000000 | chain[i + 1] if i + 1 < len(chain) else 0xFFFFFFFF
        return chain[0]

    root_entries = [None, dirent(D_DIR, 0, 0, "..")]
    for name, files in dirs:
        ents = [dirent(D_DIR, 0, 0, "."), dirent(D_DIR, 0, 0, "..")]
        for fname, data in files:
            ents.append(dirent(D_FILE, len(data), alloc(data), fname))
        root_entries.append(dirent(D_DIR, len(ents), alloc(b"".join(ents)), name))
    root_entries[0] = dirent(D_DIR, len(root_entries), 0, ".")
    root = b"".join(root_entries)
    # the root at cluster 0, its chain forward
    n = (len(root) + cs - 1) // cs
    for i in range(n):
        clusters[i] = root[i * cs:(i + 1) * cs].ljust(cs, b"\0")
        fat[i] = 0x80000000 | (i + 1) if i + 1 < n else 0xFFFFFFFF
    epc = cs // 4
    img = bytearray(b"\xff" * (cpc * cs))
    # indirect FAT cluster 8, FAT clusters from 9 on (absolute)
    nfat = (alloc_end + epc - 1) // epc
    ind = [9 + i for i in range(nfat)] + [0xFFFFFFFF] * (epc - nfat)
    img[8 * cs:9 * cs] = struct.pack("<%dL" % epc, *ind)
    for i in range(nfat):
        ents = [fat.get(i * epc + k, 0x7FFFFFFF) for k in range(epc)]
        img[(9 + i) * cs:(10 + i) * cs] = struct.pack("<%dL" % epc, *ents)
    for c, data in clusters.items():
        img[(c + alloc_off) * cs:(c + alloc_off + 1) * cs] = data
    sb = b"Sony PS2 Memory Card Format 1.2.0.0".ljust(40, b"\0")
    sb += struct.pack("<HHHHLLLLLL8x", page, ppc, 16, 0xFF00, cpc, alloc_off, alloc_end, 0, 1023, 1022)
    sb += struct.pack("<32L", 8, *([0] * 31)) + struct.pack("<32L", *([0xFFFFFFFF] * 32)) + bytes([2, 0x2B, 0, 0])
    img[0:len(sb)] = sb
    if not ecc:
        return bytes(img)
    out = bytearray()
    for p in range(len(img) // page):
        out += img[p * page:(p + 1) * page] + b"\0" * 16
    return bytes(out)


# ---- .psu ---------------------------------------------------------------------------------------------------
def psu(name, files):
    out = dirent(D_DIR, len(files) + 2, 0, name) + dirent(D_DIR, 0, 0, ".") + dirent(D_DIR, 0, 0, "..")
    for fname, data in files:
        out += dirent(D_FILE, len(data), 0, fname) + data + b"\0" * (-len(data) % 1024)
    return out


# ---- LZARI (Okumura), the encoder ------------------------------------------------------------------------------
N, F, THRESHOLD = 4096, 60, 2
N_CHAR = 256 - THRESHOLD + F
M = 15
Q1, Q2, Q3, Q4 = 1 << M, 2 << M, 3 << M, 4 << M
MAX_CUM = Q1 - 1


class LzariEncoder:
    def __init__(self):
        self.bits = []
        self.low, self.high, self.shifts = 0, Q4, 0
        self.char_to_sym = [0] * N_CHAR
        self.sym_to_char = [0] * (N_CHAR + 1)
        self.sym_freq = [0] * (N_CHAR + 1)
        self.sym_cum = [0] * (N_CHAR + 1)
        self.position_cum = [0] * (N + 1)
        for sym in range(N_CHAR, 0, -1):
            ch = sym - 1
            self.char_to_sym[ch] = sym
            self.sym_to_char[sym] = ch
            self.sym_freq[sym] = 1
            self.sym_cum[sym - 1] = self.sym_cum[sym] + 1
        for i in range(N, 0, -1):
            self.position_cum[i - 1] = self.position_cum[i] + 10000 // (i + 200)

    def output(self, bit):
        self.bits.append(bit)
        self.bits.extend([bit ^ 1] * self.shifts)
        self.shifts = 0

    def update(self, sym):
        f, c = self.sym_freq, self.sym_cum
        if c[0] >= MAX_CUM:
            t = 0
            for i in range(N_CHAR, 0, -1):
                c[i] = t
                f[i] = (f[i] + 1) >> 1
                t += f[i]
            c[0] = t
        i = sym
        while f[i] == f[i - 1]:
            i -= 1
        if i < sym:
            ch_i, ch_sym = self.sym_to_char[i], self.sym_to_char[sym]
            self.sym_to_char[i], self.sym_to_char[sym] = ch_sym, ch_i
            self.char_to_sym[ch_i], self.char_to_sym[ch_sym] = sym, i
        f[i] += 1
        for k in range(i):
            c[k] += 1

    def narrow(self, hi_cum, lo_cum, total):
        rng = self.high - self.low
        self.high = self.low + rng * hi_cum // total
        self.low += rng * lo_cum // total
        while True:
            if self.high <= Q2:
                self.output(0)
            elif self.low >= Q2:
                self.output(1)
                self.low -= Q2
                self.high -= Q2
            elif self.low >= Q1 and self.high <= Q3:
                self.shifts += 1
                self.low -= Q1
                self.high -= Q1
            else:
                break
            self.low *= 2
            self.high *= 2

    def char(self, ch):
        sym = self.char_to_sym[ch]
        self.narrow(self.sym_cum[sym - 1], self.sym_cum[sym], self.sym_cum[0])
        self.update(sym)

    def position(self, p):
        self.narrow(self.position_cum[p], self.position_cum[p + 1], self.position_cum[0])

    def encode(self, src):
        # the window starts as N - F spaces, as the decoder's; matches found by a 3-byte index of where each run was seen last
        buf = b" " * (N - F) + src
        start = N - F
        last = {}
        for p in range(max(0, start - N), start - 2):
            last.setdefault(buf[p:p + 3], []).append(p)
        pos = start
        while pos < len(buf):
            best_len, best_dist = 0, 0
            for cand in reversed(last.get(buf[pos:pos + 3], [])[-16:]):
                dist = pos - cand
                if dist > N - F or dist < 1:
                    continue
                n = 0
                while n < F and pos + n < len(buf) and buf[cand + n] == buf[pos + n]:
                    n += 1
                if n > best_len:
                    best_len, best_dist = n, dist
            step = best_len if best_len > THRESHOLD else 1
            if step == 1:
                self.char(buf[pos])
            else:
                self.char(255 - THRESHOLD + best_len)
                self.position(best_dist - 1)
            for k in range(step):
                last.setdefault(buf[pos + k:pos + k + 3], []).append(pos + k)
            pos += step
        self.shifts += 1
        self.output(0 if self.low < Q1 else 1)
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def max_drive(name, files):
    body = b""
    for fname, data in files:
        body += struct.pack("<L32s", len(data), fname.encode()) + data
        body += b"\0" * ((len(body) + 8 + 15) // 16 * 16 - 8 - len(body))
    comp = LzariEncoder().encode(body)
    hdr = struct.pack("<12sL32s32sLLL", b"Ps2PowerSave", 0, name.encode(), b"Hot Pursuit 2", len(comp) + 4, len(files), len(body))
    crc = zlib.crc32(comp, zlib.crc32(hdr))
    return struct.pack("<12sL32s32sLLL", b"Ps2PowerSave", crc, name.encode(), b"Hot Pursuit 2", len(comp) + 4, len(files), len(body)) + comp


# ---- .cbs ------------------------------------------------------------------------------------------------------
RC4S = None


def rc4(data):
    s = list(RC4S)
    out = bytearray(data)
    j = 0
    for k in range(len(out)):
        i = (k + 1) % 256
        j = (j + s[i]) % 256
        s[i], s[j] = s[j], s[i]
        out[k] ^= s[(s[i] + s[j]) % 256]
    return bytes(out)


def codebreaker(name, files, whole_length=False):
    body = b""
    for fname, data in files:
        body += struct.pack("<8s8sLHHLL32s", TOD, TOD, len(data), D_FILE, 0, 0, 0, fname.encode()) + data
    comp = rc4(zlib.compress(body, 9))
    title = b"Need for Speed Hot Pursuit 2".ljust(32, b"\0")
    hlen = 92 + len(title)
    flen = hlen + len(comp) if whole_length else len(comp)
    hdr = b"CFU\0" + struct.pack("<LLLL32s8s8sLLLLLL", 0x1F40, hlen, len(body), flen, name.encode(), TOD, TOD, 0, 0, D_DIR, 0, 0, 0) + title
    assert len(hdr) == hlen
    return hdr + comp


def main():
    global RC4S
    save = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    # the permutation as savestransfer.cpp has it is not used here: mymc+'s, typed again
    RC4S = [int(x, 16) for x in open(os.path.join(here, "cbs_rc4.txt")).read().split()]
    assert sorted(RC4S) == list(range(256))
    os.makedirs(out, exist_ok=True)
    icon = b"PS2D" + b"\x11" * 960
    files = [(USA, save), ("icon.sys", icon), ("nfs6.ico", b"\x22" * 3000)]
    w = lambda n, d: open(os.path.join(out, n), "wb").write(d)
    w("card_ecc.ps2", card([("BADATA-SYSTEM", [("history", b"h" * 462)]), (USA, files),
                            ("BASLUS-20362NFS6", [("BASLUS-20362NFS6", save[:-4])]), ("BESLES-50731HP2", [("BESLES-50731HP2", save[:100])])], True))
    w("card_plain.mcd", card([(EUR, [(EUR, save), ("icon.sys", icon)])], False))
    folder = os.path.join(out, "Mcd003.ps2", USA)
    os.makedirs(folder, exist_ok=True)
    open(os.path.join(folder, USA), "wb").write(save)
    open(os.path.join(out, "Mcd003.ps2", "_pcsx2_superblock"), "wb").write(b"\0" * 16)
    own = os.path.join(out, EUR)
    os.makedirs(own, exist_ok=True)
    open(os.path.join(own, EUR), "wb").write(save)
    w("save.psu", psu(USA, files))
    w("save.max", max_drive(USA, files))
    w("save.cbs", codebreaker(USA, files))
    w("save_whole.cbs", codebreaker(EUR, [(EUR, save)], whole_length=True))
    w("other_game.psu", psu("BASLUS-21000GAME", [("BASLUS-21000GAME", b"x" * 100)]))


if __name__ == "__main__":
    main()
