#!/usr/bin/env python3
"""The data half of "Playable traffic cars" (a mod by 379Felipe, v1.4.1: xdelta patches of the disc images) as a table for the port.

    python tools/recomp/make_traffic_patch.py <name>=<image.iso>,<patch.xdelta> [<name>=...] > recomp/runtime/traffic_patch.h

The mod changes two things on a disc: the cars' table (the chunk 0x34600 of GLOBAL\\GLOBALB.BUN in ZZDATA.BIN: 64 CarTypeInfo of 1376
bytes) and five places of the program. The program's are the port's hooks (native.cpp, "Playable traffic cars"); the table's go here,
as runs of bytes by their place in the table, each with the bytes the disc has there and the bytes the mod puts: the port writes
them into the table in the game's memory when Options > Extras > Playable Traffic Cars is on, and only where the memory has what the
disc had. A disc whose table is another (Europe's) has runs of its own, under its own name.

The patch is read as data: a VCDIFF (RFC 3284) with xdelta's application header and no secondary compression. The image is not
written out: each window is decoded in memory and held against the source at the same place."""
import struct
import sys


def varint(b, p):
    v = 0
    while True:
        c = b[p]
        p += 1
        v = (v << 7) | (c & 0x7f)
        if not c & 0x80:
            return v, p


def code_table():
    NOOP, ADD, RUN, COPY = 0, 1, 2, 3
    t = [(RUN, 0, 0, NOOP, 0, 0)]
    for size in range(0, 18):
        t.append((ADD, size, 0, NOOP, 0, 0))
    for mode in range(9):
        t.append((COPY, 0, mode, NOOP, 0, 0))
        for size in range(4, 19):
            t.append((COPY, size, mode, NOOP, 0, 0))
    for mode in range(6):
        for add in range(1, 5):
            for cp in range(4, 7):
                t.append((ADD, add, 0, COPY, cp, mode))
    for mode in range(6, 9):
        for add in range(1, 5):
            t.append((ADD, add, 0, COPY, 4, mode))
    for mode in range(9):
        t.append((COPY, 4, mode, ADD, 1, 0))
    return t


def changes(src_path, patch_path):
    """[(offset in the image, new bytes)]: the runs the patch changes"""
    patch = open(patch_path, 'rb').read()
    assert patch[:3] == b'\xd6\xc3\xc4', 'not VCDIFF'
    hdr = patch[4]
    p = 5
    assert not hdr & 3, 'secondary compression or a code table of its own: not handled'
    if hdr & 4:
        n, p = varint(patch, p)
        p += n
    table = code_table()
    src = open(src_path, 'rb')
    out_at = 0
    found = []
    while p < len(patch):
        win = patch[p]
        p += 1
        seg_len = seg_pos = 0
        if win & 3:
            seg_len, p = varint(patch, p)
            seg_pos, p = varint(patch, p)
        assert not win & 2, 'a window that copies from the target: not handled'
        _, p = varint(patch, p)
        tgt_len, p = varint(patch, p)
        assert patch[p] == 0, 'compressed sections'
        p += 1
        dlen, p = varint(patch, p)
        ilen, p = varint(patch, p)
        alen, p = varint(patch, p)
        if win & 4:
            p += 4
        data = patch[p:p + dlen]
        inst = patch[p + dlen:p + dlen + ilen]
        addr = patch[p + dlen + ilen:p + dlen + ilen + alen]
        p += dlen + ilen + alen
        src.seek(seg_pos)
        seg = src.read(seg_len) if win & 1 else b''
        target = bytearray()
        dp = ip = ap = 0
        near = [0] * 4
        same = [0] * (3 * 256)
        near_at = 0
        while ip < len(inst):
            entry = table[inst[ip]]
            ip += 1
            for kind, size, mode in ((entry[0], entry[1], entry[2]), (entry[3], entry[4], entry[5])):
                if kind == 0:
                    continue
                if size == 0:
                    size, ip = varint(inst, ip)
                if kind == 1:
                    target += data[dp:dp + size]
                    dp += size
                elif kind == 2:
                    target += bytes([data[dp]]) * size
                    dp += 1
                else:
                    here = seg_len + len(target)
                    if mode == 0:
                        a, ap = varint(addr, ap)
                    elif mode == 1:
                        a, ap = varint(addr, ap)
                        a = here - a
                    elif mode < 6:
                        a, ap = varint(addr, ap)
                        a += near[mode - 2]
                    else:
                        a = same[(mode - 6) * 256 + addr[ap]]
                        ap += 1
                    near[near_at] = a
                    near_at = (near_at + 1) % 4
                    same[a % (3 * 256)] = a
                    if a + size <= seg_len:
                        target += seg[a:a + size]
                    else:
                        for k in range(size):
                            x = a + k
                            target.append(seg[x] if x < seg_len else target[x - seg_len])
        assert len(target) == tgt_len
        src.seek(out_at)
        old = src.read(tgt_len)
        if old != target:
            i, n = 0, len(target)
            while i < n:
                if old[i:i + 4096] == target[i:i + 4096]:
                    i += 4096
                    continue
                if i < len(old) and old[i] == target[i]:
                    i += 1
                    continue
                j = i
                while j < n and not (j < len(old) and old[j] == target[j]):
                    j += 1
                found.append((out_at + i, bytes(target[i:j])))
                i = j
        out_at += tgt_len
    return found


def table_runs(src_path, patch_path):
    """the runs that fall in the cars' table, by their place in it: [(place, old bytes, new bytes)]; and the others' places"""
    found = changes(src_path, patch_path)
    src = open(src_path, 'rb')
    mark = struct.pack('<II', 0x34600, 64 * 1376 + 8)       # the chunk's header: 8 bytes of filler, then the 64 records
    first = min(off for off, _ in found if off > 0x1000000)
    src.seek(first - 0x18000)
    hay = src.read(0x18000)
    at = hay.rfind(mark)
    assert at >= 0, 'no cars table before the first run'
    table = first - 0x18000 + at + 16
    src.seek(table)
    was = src.read(64 * 1376)
    runs, others = [], []
    for off, new in found:
        if table <= off and off + len(new) <= table + 64 * 1376:
            runs.append((off - table, was[off - table:off - table + len(new)], new))
        else:
            others.append(off)
    return runs, others


def main():
    print('// Made by tools/recomp/make_traffic_patch.py from the xdelta patches of "Playable traffic cars" v1.4.1, a mod by 379Felipe')
    print('// (thanks there to Xanvier and nlgzrgn). The cars\' table (64 CarTypeInfo of 1376 bytes): runs of bytes by their place in the')
    print('// table, what a disc has there and what the mod puts there. One set for each disc whose table differs.')
    print('struct TrafficRun {')
    print('    unsigned at, n;')
    print('    const unsigned char *was, *now;')
    print('};')
    names = []
    for arg in sys.argv[1:]:
        name, rest = arg.split('=', 1)
        iso, patch = rest.rsplit(',', 1)
        runs, others = table_runs(iso, patch)
        sys.stderr.write('%s: %d runs in the table, %d bytes; %d runs elsewhere (the program)\n' % (name, len(runs), sum(len(r[2]) for r in runs), len(others)))
        blob_was = b''.join(r[1] for r in runs)
        blob_now = b''.join(r[2] for r in runs)

        def lines(blob):
            return '\n'.join('    ' + ', '.join('0x%02x' % c for c in blob[i:i + 24]) + ',' for i in range(0, len(blob), 24))
        print('static const unsigned char kTraffic%sWas[] = {\n%s\n};' % (name, lines(blob_was)))
        print('static const unsigned char kTraffic%sNow[] = {\n%s\n};' % (name, lines(blob_now)))
        print('static const TrafficRun kTraffic%s[] = {' % name)
        p = 0
        for at, old, new in runs:
            print('    {%d, %d, kTraffic%sWas + %d, kTraffic%sNow + %d},' % (at, len(new), name, p, name, p))
            p += len(new)
        print('};')
        names.append((name, len(runs)))
    print('static const struct { const char *disc; const TrafficRun *runs; int n; } kTrafficSets[] = {')
    for name, n in names:
        print('    {"%s", kTraffic%s, %d},' % (name, name, n))
    print('};')


if __name__ == '__main__':
    main()
