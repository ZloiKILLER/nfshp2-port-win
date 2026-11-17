#!/usr/bin/env python3
"""The test of savestransfer.exe's export: a profile onto card images, into a folder card, as .psu, .max and .cbs.

    python test_export.py <save.dat> <NFS6.ICO> [--exe ...] [--work ...] [--card <a real .ps2>] [--icon-sys <the game's icon.sys>]

Everything it writes is checked by readers of its own, not saveimport's: the card's file system with every page's ECC and every
FAT chain, an LZARI decoder for .max, zlib for .cbs. Then each is imported again and must give the same save. A real card given
with --card is copied first; the copy is what is written.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import make_test_saves  # noqa: E402

FAILS = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        FAILS.append(what)


# ---- the card, read on its own ------------------------------------------------------------------------------
PAR = [bin(b).count("1") & 1 for b in range(256)]
CPM = [0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0]
COL = [sum(PAR[b & m] << i for i, m in enumerate(CPM)) for b in range(256)]


def ecc128(chunk):
    cp, l0, l1 = 0x77, 0x7F, 0x7F
    for i, b in enumerate(chunk):
        cp ^= COL[b]
        if PAR[b]:
            l0 ^= ~i
            l1 ^= i
    return bytes([cp & 0xFF, l0 & 0x7F, l1 & 0xFF])


class Card:
    def __init__(self, d):
        self.d = d
        self.page, self.ppc = struct.unpack_from("<HH", d, 0x28)
        self.cpc, self.aoff, self.aend, self.root = struct.unpack_from("<LLLL", d, 0x30)
        self.ifc = struct.unpack_from("<32L", d, 0x50)
        self.raw = self.page + 16 if len(d) == self.cpc * self.ppc * (self.page + 16) else self.page
        self.epc = self.page * self.ppc // 4

    def clus(self, n):
        return b"".join(self.d[(n * self.ppc + i) * self.raw:(n * self.ppc + i) * self.raw + self.page] for i in range(self.ppc))

    def fat(self, n):
        ind = struct.unpack("<%dL" % self.epc, self.clus(self.ifc[n // self.epc // self.epc]))
        return struct.unpack("<%dL" % self.epc, self.clus(ind[(n // self.epc) % self.epc]))[n % self.epc]

    def chain(self, c):
        out = []
        while True:
            out.append(c)
            e = self.fat(c)
            if e == 0xFFFFFFFF or not e & 0x80000000:
                return out
            c = e & 0x7FFFFFFF

    def read(self, c, n):
        return b"".join(self.clus(x + self.aoff) for x in self.chain(c))[:n]

    def ents(self, c, n):
        b = self.read(c, n * 512)
        return [struct.unpack_from("<HHL8sLL8sL", b, i * 512) + (b[i * 512 + 0x40:i * 512 + 0x60].split(b"\0")[0].decode("latin1"),) for i in range(n)]

    def root_ents(self):
        return self.ents(self.root, self.ents(self.root, 1)[0][2])

    def bad_ecc_pages(self, before):
        # pages whose ECC is wrong now and was right (or erased) before: none may be
        bad = []
        if self.raw == self.page:
            return bad
        for p in range(len(self.d) // self.raw):
            pg, sp = self.d[p * self.raw:p * self.raw + 512], self.d[p * self.raw + 512:p * self.raw + 528]
            if pg == before[p * self.raw:p * self.raw + 512] and sp == before[p * self.raw + 512:p * self.raw + 528]:
                continue
            if sp[:12] != b"".join(ecc128(pg[i * 128:(i + 1) * 128]) for i in range(4)) or sp[12:] != b"\0\0\0\0":
                bad.append(p)
        return bad

    def fat_problems(self):
        # every allocated cluster in exactly one chain of a file or directory reachable from the root
        seen = {}
        probs = []

        def walk(c, length, what):
            n = max(1, (length + 1023) // 1024)
            ch = self.chain(c)
            if len(ch) != n:
                probs.append(f"{what}: chain of {len(ch)} clusters for {length} bytes")
            for x in ch:
                if x in seen:
                    probs.append(f"{what}: cluster {x} also in {seen[x]}")
                seen[x] = what

        def walk_dir(c, count, path):
            walk(c, count * 512, path)
            for e in self.ents(c, count)[2:]:
                if not e[0] & 0x8000:
                    continue
                if e[0] & 0x20:
                    walk_dir(e[4], e[2], path + "/" + e[8])
                elif e[2]:
                    walk(e[4], e[2], path + "/" + e[8])

        walk_dir(self.root, self.root_ents()[0][2], "")
        used = {n for n in range(self.aend) if self.fat(n) & 0x80000000}
        if used != set(seen):
            probs.append(f"{len(used - set(seen))} clusters allocated to nothing, {len(set(seen) - used)} in chains but free")
        return probs


# ---- LZARI, the decoder, after Okumura's LZARI.C (for the .max written) ----------------------------------------------
def lzari_decode(src, size):
    N, F, TH = 4096, 60, 2
    NC = 256 - TH + F
    Q1, Q2, Q3, Q4 = 1 << 15, 2 << 15, 3 << 15, 4 << 15
    bits = []
    for b in src:
        bits.extend((b >> (7 - k)) & 1 for k in range(8))
    bits.extend([0] * 64)
    it = iter(bits)
    s2c = [0] * (NC + 1)
    freq = [0] * (NC + 1)
    cum = [0] * (NC + 1)
    pcum = [0] * (N + 1)
    for sym in range(NC, 0, -1):
        s2c[sym] = sym - 1
        freq[sym] = 1
        cum[sym - 1] = cum[sym] + 1
    for i in range(N, 0, -1):
        pcum[i - 1] = pcum[i] + 10000 // (i + 200)
    low, high, value = 0, Q4, 0
    for _ in range(17):
        value = 2 * value + next(it)

    def renorm():
        nonlocal low, high, value
        while True:
            if low >= Q2:
                value -= Q2; low -= Q2; high -= Q2
            elif low >= Q1 and high <= Q3:
                value -= Q1; low -= Q1; high -= Q1
            elif high > Q2:
                return
            low *= 2; high *= 2
            value = 2 * value + next(it)

    def update(sym):
        if cum[0] >= Q1 - 1:
            c = 0
            for i in range(NC, 0, -1):
                cum[i] = c
                freq[i] = (freq[i] + 1) >> 1
                c += freq[i]
            cum[0] = c
        i = sym
        while freq[i] == freq[i - 1]:
            i -= 1
        if i < sym:
            s2c[i], s2c[sym] = s2c[sym], s2c[i]
        freq[i] += 1
        for k in range(i):
            cum[k] += 1

    def search(table, x, hi):
        i, j = 1, hi
        while i < j:
            k = (i + j) // 2
            if table[k] > x:
                i = k + 1
            else:
                j = k
        return i

    buf = bytearray(b" " * (N - F) + b"\0" * F)
    r, out = N - F, bytearray()
    while len(out) < size:
        rng = high - low
        sym = search(cum, ((value - low + 1) * cum[0] - 1) // rng, NC)
        high = low + rng * cum[sym - 1] // cum[0]
        low += rng * cum[sym] // cum[0]
        renorm()
        c = s2c[sym]
        update(sym)
        if c < 256:
            out.append(c); buf[r] = c; r = (r + 1) & (N - 1)
        else:
            rng = high - low
            pos = search(pcum, ((value - low + 1) * pcum[0] - 1) // rng, N) - 1
            high = low + rng * pcum[pos] // pcum[0]
            low += rng * pcum[pos + 1] // pcum[0]
            renorm()
            i = (r - pos - 1) & (N - 1)
            for k in range(c - 255 + TH):
                b = buf[(i + k) & (N - 1)]
                out.append(b); buf[r] = b; r = (r + 1) & (N - 1)
    return bytes(out[:size])


def max_files(d):
    clen, n, length = struct.unpack_from("<LLL", d, 0x50)
    body = lzari_decode(d[0x5C:0x5C + clen - 4], length)
    off, files = 0, {}
    for _ in range(n):
        size, = struct.unpack_from("<L", body, off)
        name = body[off + 4:off + 36].split(b"\0")[0].decode()
        files[name] = body[off + 36:off + 36 + size]
        off += 36 + size
        off = (off + 8 + 15) // 16 * 16 - 8
    crc_ok = zlib.crc32(d[0x5C:0x5C + clen - 4], zlib.crc32(d[:0xC] + b"\0\0\0\0" + d[0x10:0x5C])) == struct.unpack_from("<L", d, 0xC)[0]
    return files, crc_ok


def cbs_files(d):
    hlen, dlen, flen = struct.unpack_from("<LLL", d, 8)
    body = zlib.decompress(make_test_saves.rc4(d[hlen:hlen + flen]))
    off, files = 0, {}
    while off < len(body):
        size, = struct.unpack_from("<L", body, off + 16)
        files[body[off + 32:off + 64].split(b"\0")[0].decode()] = body[off + 64:off + 64 + size]
        off += 64 + size
    return files, len(body) == dlen


def psu_files(d):
    pos, files = 1536, {}
    for _ in range(struct.unpack_from("<L", d, 4)[0] - 2):
        n, = struct.unpack_from("<L", d, pos + 4)
        files[d[pos + 0x40:pos + 0x60].split(b"\0")[0].decode()] = d[pos + 512:pos + 512 + n]
        pos += 512 + (n + 1023) // 1024 * 1024
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save")
    ap.add_argument("icon")
    ap.add_argument("--exe", default=os.path.join(ROOT, "recompwin", "savestransfer.exe"))
    ap.add_argument("--work", default=os.path.join(ROOT, "work", "savestransfer_export_test"))
    ap.add_argument("--card")
    ap.add_argument("--icon-sys")
    a = ap.parse_args()
    save, icon = open(a.save, "rb").read(), open(a.icon, "rb").read()
    make_test_saves.RC4S = [int(x, 16) for x in open(os.path.join(HERE, "cbs_rc4.txt")).read().split()]

    shutil.rmtree(a.work, ignore_errors=True)
    game = os.path.join(a.work, "game")
    os.makedirs(os.path.join(game, "data"))
    open(os.path.join(game, "data", "SLUS_203.62"), "wb").write(b"test")
    open(os.path.join(game, "data", "index.txt"), "wb").write(b"test")
    shutil.copy(a.icon, os.path.join(game, "data", "NFS6.ICO"))
    os.makedirs(os.path.join(game, "saves", "Racer"))
    shutil.copy(a.save, os.path.join(game, "saves", "Racer", "save.dat"))
    os.makedirs(os.path.join(game, "saves", "Empty"))
    out = os.path.join(a.work, "out")
    os.makedirs(out)

    def run(*args):
        r = subprocess.run([a.exe, "--game", game, *args], capture_output=True, text=True, encoding="utf-8")
        return r.returncode, r.stdout

    def imports_back(path, what, dirname="BASLUS-20362HP2"):
        # the number of the save in that folder: a card may hold others
        code, o = run("--list", path)
        found = re.findall(r"^\s+(\d+)\. .*\((\S+)\), saved", o, re.M)
        n = next((int(k) for k, dn in found if dn == dirname), 0)
        prof = "back%d" % len(os.listdir(os.path.join(game, "saves")))
        code2, _ = run("--import", str(n), "--name", prof, path)
        got = os.path.join(game, "saves", prof, "save.dat")
        check(code == 0 and n >= 1 and code2 == 0 and open(got, "rb").read() == save, f"{what}: imported back, the same save")

    def files_ok(files, dirname, what):
        icon_sys_ok = len(files.get("icon.sys", b"")) == 964 and files["icon.sys"][:4] == b"PS2D"
        if a.icon_sys:
            icon_sys_ok = files.get("icon.sys") == open(a.icon_sys, "rb").read()
        check(list(files) == [dirname, "icon.sys", "nfs6.ico"] and files[dirname] == save and files["nfs6.ico"] == icon and icon_sys_ok,
              what + ": the save, icon.sys" + (" (the game's own bytes)" if a.icon_sys else "") + " and nfs6.ico, in the game's order")

    # a blank card, with and without ECC
    for name, ecc in (("blank_ecc.ps2", True), ("blank_plain.mcd", False)):
        card = os.path.join(out, name)
        open(card, "wb").write(make_test_saves.card([], ecc))
        before = open(card, "rb").read()
        code, o = run("--export", "Racer", "--to", card)
        after = open(card, "rb").read()
        c = Card(after)
        ents = c.root_ents()
        dirs = [e for e in ents if e[8] == "BASLUS-20362HP2"]
        check(code == 0 and len(dirs) == 1, f"{name}: written, one BASLUS-20362HP2 in the root")
        if dirs:
            e = dirs[0]
            sub = c.ents(e[4], e[2])
            check(hex(e[0]) == "0x8427" and e[2] == 5 and [x[8] for x in sub[:2]] == [".", ".."] and sub[0][4] == c.root
                  and sub[0][5] == ents.index(e) and all(hex(x[0]) == "0x8497" for x in sub[2:]),
                  f"{name}: the folder's entries as the game makes them")
            files_ok({x[8]: c.read(x[4], x[2]) for x in sub[2:]}, "BASLUS-20362HP2", name)
        check(c.bad_ecc_pages(before) == [], f"{name}: every page written has its ECC")
        check(c.fat_problems() == [], f"{name}: every FAT chain whole, nothing cross-linked or lost")
        check(open(card + ".bak", "rb").read() == before, f"{name}: the card as it was kept as .bak")
        imports_back(card, name)
        code, o = run("--export", "Racer", "--to", card)
        check(code != 0 and open(card, "rb").read() == after and "already" in o, f"{name}: a second time refused, nothing written")
        code, o = run("--export", "Racer", "--to", card, "--disc", "europe")
        c2 = Card(open(card, "rb").read())
        check(code == 0 and {e[8] for e in c2.root_ents()} >= {"BASLUS-20362HP2", "BESLES-50731HP2"} and c2.fat_problems() == [],
              f"{name}: the Europe folder beside it, the card still whole")

    # deleting and writing over, on the ECC card that has a USA and a Europe save now
    card = os.path.join(out, "blank_ecc.ps2")
    blank_free = sum(1 for n in range(Card(make_test_saves.card([], True)).aend) if not Card(make_test_saves.card([], True)).fat(n) & 0x80000000)

    def free(c):
        return sum(1 for n in range(c.aend) if not c.fat(n) & 0x80000000)

    def live(c):
        return [e[8] for e in c.root_ents()[2:] if e[0] & 0x8000]

    before = open(card, "rb").read()
    count_before = Card(before).root_ents()[0][2]
    code, o = run("--delete", "BASLUS-20362HP2", "--from", card)
    c = Card(open(card, "rb").read())
    check(code == 0 and live(c) == ["BESLES-50731HP2"], "delete: the USA save off the card, the Europe one there still")
    check(c.bad_ecc_pages(before) == [] and c.fat_problems() == [], "delete: ECC and FAT whole")
    baks = sorted(f for f in os.listdir(out) if f.startswith("blank_ecc.ps2.bak"))
    # .bak (before the export), .bak2 (before the Europe one), .bak3 (before this delete)
    check(len(baks) == 3 and open(os.path.join(out, baks[-1]), "rb").read() == before, "delete: the card as it was kept as another .bak")
    imports_back(card, "delete: the Europe save", "BESLES-50731HP2")
    code, o = run("--delete", "BASLUS-20362HP2", "--from", card)
    check(code != 0, "delete: a save that is not there, refused")
    before = open(card, "rb").read()
    code, o = run("--delete", "BESLES-50731HP2", "--from", card)
    c = Card(open(card, "rb").read())
    # the root keeps the cluster it grew by (its deleted entries stay, for the next save), as the PS2's mcman leaves it
    check(code == 0 and live(c) == [] and free(c) == blank_free - (len(c.chain(c.root)) - 1) and c.fat_problems() == [] and c.bad_ecc_pages(before) == [],
          "delete: both gone, every cluster free again but the root's, the card whole")
    code, o = run("--export", "Racer", "--to", card)
    c = Card(open(card, "rb").read())
    check(code == 0 and live(c) == ["BASLUS-20362HP2"] and c.root_ents()[0][2] == count_before and c.fat_problems() == [],
          "export after a delete: the deleted entry taken again, the root no longer")
    after_one = free(c)
    before = open(card, "rb").read()
    code, o = run("--export", "Racer", "--to", card)
    check(code != 0 and open(card, "rb").read() == before, "overwrite: not without --overwrite")
    code, o = run("--export", "Racer", "--to", card, "--overwrite")
    c = Card(open(card, "rb").read())
    check(code == 0 and live(c) == ["BASLUS-20362HP2"] and free(c) == after_one and c.fat_problems() == [] and c.bad_ecc_pages(before) == [],
          "overwrite: one save, no cluster lost, ECC and FAT whole")
    imports_back(card, "overwrite")

    # a full card: refused, nothing written
    full = os.path.join(out, "full.ps2")
    d = bytearray(make_test_saves.card([("BAFILL", [("big", b"x" * (7990 * 1024))])], True))
    open(full, "wb").write(d)
    code, o = run("--export", "Racer", "--to", full)
    check(code != 0 and open(full, "rb").read() == bytes(d) and "free" in o and not os.path.exists(full + ".bak"), "a full card: refused, untouched")

    # a real card, copied
    if a.card:
        real = os.path.join(out, "real.ps2")
        shutil.copy(a.card, real)
        before = open(real, "rb").read()
        had_usa = any(e[8] == "BASLUS-20362HP2" and e[0] & 0x8000 for e in Card(before).root_ents())    # a deleted entry keeps its name
        code, o = run("--export", "Racer", "--to", real)
        if had_usa:
            check(code != 0 and open(real, "rb").read() == before, "real card: has a USA save already, refused, untouched")
            code, o = run("--export", "Racer", "--to", real, "--disc", "europe")
        c = Card(open(real, "rb").read())
        names = [e[8] for e in c.root_ents() if e[0] & 0x8000]
        check(code == 0 and c.bad_ecc_pages(before) == [] and c.fat_problems() == [], f"real card: written ({o.strip()[-60:]}), ECC and FAT whole")
        check(set(names) >= {e[8] for e in Card(before).root_ents() if e[0] & 0x8000}, "real card: every save on it before is there still")
        imports_back(real, "real card", "BESLES-50731HP2" if had_usa else "BASLUS-20362HP2")
        mine = "BESLES-50731HP2" if had_usa else "BASLUS-20362HP2"
        written = open(real, "rb").read()
        code, o = run("--delete", mine, "--from", real)
        c = Card(open(real, "rb").read())
        check(code == 0 and mine not in [e[8] for e in c.root_ents() if e[0] & 0x8000] and c.fat_problems() == [] and c.bad_ecc_pages(written) == [],
              "real card: the save written deleted again, the card whole")
        check(sorted(e[8] for e in c.root_ents() if e[0] & 0x8000) == sorted(e[8] for e in Card(before).root_ents() if e[0] & 0x8000),
              "real card: the saves of before, and only they")

    # a folder card
    folder = os.path.join(out, "Mcd009.ps2")
    os.makedirs(folder)
    code, o = run("--export", "Racer", "--to", folder)
    dirp = os.path.join(folder, "BASLUS-20362HP2")
    files = {n: open(os.path.join(dirp, n), "rb").read() for n in ["BASLUS-20362HP2", "icon.sys", "nfs6.ico"] if os.path.exists(os.path.join(dirp, n))}
    check(code == 0 and sorted(os.listdir(dirp)) == sorted(files), "folder card: the save's folder made")
    files_ok(dict(sorted(files.items(), key=lambda kv: ["BASLUS-20362HP2", "icon.sys", "nfs6.ico"].index(kv[0]))), "BASLUS-20362HP2", "folder card")
    code, o = run("--export", "Racer", "--to", folder)
    check(code != 0 and "already" in o, "folder card: a second time refused")
    imports_back(folder, "folder card")
    code, o = run("--export", "Racer", "--to", folder, "--overwrite", "--disc", "usa")
    check(code == 0 and sorted(os.listdir(dirp)) == ["BASLUS-20362HP2", "icon.sys", "nfs6.ico"], "folder card: written over (the old one to the Recycle Bin)")
    code, o = run("--delete", "BASLUS-20362HP2", "--from", folder)
    check(code == 0 and not os.path.exists(dirp), "folder card: the save deleted (to the Recycle Bin)")

    # the save files
    for ext, reader in ((".psu", lambda d: (psu_files(d), True)), (".max", max_files), (".cbs", cbs_files)):
        f = os.path.join(out, "racer" + ext)
        code, o = run("--export", "Racer", "--to", f, "--disc", "korea")
        d = open(f, "rb").read() if os.path.exists(f) else b""
        files, ok = reader(d) if d else ({}, False)
        check(code == 0 and ok, f"{ext}: written, read by a reader of its own (CRC, lengths)")
        files_ok(files, "BKSLPM-67527HP2", ext)
        code, o = run("--export", "Racer", "--to", f)
        check(code != 0 and open(f, "rb").read() == d, f"{ext}: not written over from the command line")
        imports_back(f, ext, "BKSLPM-67527HP2")

    psu = os.path.join(out, "racer.psu")
    d = open(psu, "rb").read()
    code, o = run("--delete", "BKSLPM-67527HP2", "--from", psu)
    check(code != 0 and open(psu, "rb").read() == d, "a save file: not deleted here")

    # what must make nothing
    code, o = run("--export", "Empty", "--to", os.path.join(out, "empty.psu"))
    check(code != 0 and not os.path.exists(os.path.join(out, "empty.psu")), "a profile without a save: refused")
    code, o = run("--export", "Nobody", "--to", os.path.join(out, "nobody.psu"))
    check(code != 0, "a profile that is not there: refused")
    junk = os.path.join(out, "junk.bin")
    open(junk, "wb").write(b"x" * 1000)
    code, o = run("--export", "Racer", "--to", junk)
    check(code != 0 and open(junk, "rb").read() == b"x" * 1000, "a file that is no card: refused, untouched")
    code, o = run("--export", "Racer", "--to", os.path.join(out, "x.psu"), "--disc", "mars")
    check(code != 0, "an unknown disc: refused")

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
