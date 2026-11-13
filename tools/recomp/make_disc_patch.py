#!/usr/bin/env python3
"""A patch that turns another disc's program into the USA one (SLUS_203.62), for the installer (recomp/runtime/install.cpp).

The Asia and Korea discs (SLPM-65181, SLPM-67527) have the game's program linked once more with 24 bytes less of data: one disc number
where the USA program has two, one memory card code where it has two. Everything after those bytes sits 8 or 16 lower, so every address
of it in the code is other, and the code recompiled from the USA program cannot run on that program's data. The installer puts the bytes
back and the addresses with them, and has the USA program, checked by its SHA-1.

    python tools/recomp/make_disc_patch.py <the other program> <SLUS_203.62> <out.hp2p>

The file: "HP2P1\\0\\0\\0", the SHA-1 of the other program and of the result (20 bytes each), the size (u32; the two are the same size),
the count of shifts (u32) and of runs (u32); a shift is pos, n, end (u32 each): the bytes from pos to end move up by n (the last n of them
fall off, the first n are written by the runs); then the runs, each a gap from the end of the last one (varint), a length (varint) and
that many bytes of the result.
"""
import hashlib
import struct
import sys


def sections(blob):
    shoff, = struct.unpack_from("<I", blob, 32)
    shent, shnum, shstr = struct.unpack_from("<HHH", blob, 46)
    sh = lambda i: struct.unpack_from("<10I", blob, shoff + i * shent)
    st = blob[sh(shstr)[4]:sh(shstr)[4] + sh(shstr)[5]]
    return {st[sh(i)[0]:st.find(b"\0", sh(i)[0])].decode("latin1"): (sh(i)[3], sh(i)[4], sh(i)[5], sh(i)[1]) for i in range(shnum)}


def varint(n):
    out = bytearray()
    while True:
        out.append((n & 0x7F) | (0x80 if n > 0x7F else 0))
        n >>= 7
        if not n:
            return bytes(out)


def apply_shifts(src, shifts):
    out = bytearray(src)
    for pos, n, end in shifts:
        out[pos + n:end] = src[pos:end - n]
        out[pos:pos + n] = bytes(n)
    return out


def main():
    src = open(sys.argv[1], "rb").read()
    dst = open(sys.argv[2], "rb").read()
    if len(src) != len(dst):
        sys.exit("the two programs are not the same size")
    ss, ds = sections(src), sections(dst)
    shifts = []
    for name, (addr, off, size, kind) in ds.items():
        if name not in ss or kind == 8 or not size:
            continue
        saddr, soff, ssize, _ = ss[name]
        n = size - ssize
        if n <= 0 or saddr != addr or soff != off:
            continue
        # where the bytes were taken out: the place that leaves the fewest bytes other
        x, y = dst[off:off + size], src[soff:soff + ssize]
        first = next(i for i in range(ssize) if x[i] != y[i])
        best = None
        for cut in range(max(0, first - 64), min(ssize, first + 64)):
            moved = y[:cut] + bytes(n) + y[cut:]
            d = sum(1 for i in range(size) if x[i] != moved[i])
            if best is None or d < best[0]:
                best = (d, cut)
        shifts.append((off + best[1], n, off + size))
        print(f"{name}: {n} bytes back at +{best[1]:#x}, {best[0]} bytes then differ")
    base = apply_shifts(src, shifts)
    runs = []
    i, n = 0, len(dst)
    while i < n:
        if base[i] == dst[i]:
            i += 1
            continue
        j = i + 1
        while j < n and (base[j] != dst[j] or base[j:j + 3] != dst[j:j + 3]):     # a gap of under three bytes costs more than it saves
            j += 1
        while base[j - 1] == dst[j - 1]:
            j -= 1
        runs.append((i, j))
        i = j
    body = bytearray()
    last = 0
    for i, j in runs:
        body += varint(i - last) + varint(j - i) + dst[i:j]
        last = j
    patch = (b"HP2P1\0\0\0" + hashlib.sha1(src).digest() + hashlib.sha1(dst).digest() + struct.pack("<III", len(dst), len(shifts), len(runs)) +
             b"".join(struct.pack("<III", *s) for s in shifts) + bytes(body))
    # the check the installer makes
    out = apply_shifts(src, shifts)
    at = 0
    for i, j in runs:
        out[i:j] = dst[i:j]
    if bytes(out) != dst:
        sys.exit("the patch does not give the USA program")
    open(sys.argv[3], "wb").write(patch)
    print(f"{sys.argv[3]}: {len(patch)} bytes, {len(runs)} runs, {sum(j - i for i, j in runs)} bytes of the result; "
          f"{hashlib.sha1(src).hexdigest()} -> {hashlib.sha1(dst).hexdigest()}")


if __name__ == "__main__":
    main()
