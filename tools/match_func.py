#!/usr/bin/env python3
"""
match_func.py - compile a candidate C++ source and diff it against the original
function bytes, sweeping compiler flags until something matches.

The exact flag set the original build used is not recorded anywhere, so it has
to be recovered empirically. Once a couple of small leaf functions match, the
winning flags apply to the whole module and the search collapses to a single
compile per function.

  python tools/match_func.py src/bSlotPool.cpp --func bGetSlot__FP8SlotPooli
  python tools/match_func.py src/bSlotPool.cpp --all        # every func in file
  python tools/match_func.py src/x.cpp --func f --flags "-O2 -G4"   # fixed flags
"""

import argparse
import hashlib
import itertools
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, "work", "retail", "SLUS_203.62.sym")
BIN = os.path.join(REPO, "tools", "prodg", "usr", "local", "sce", "ee",
                   "gcc", "bin")
# 2.9-ee-991111, NOT 2.95.2: the original saves $ra with sd (64-bit), which
# only 2.9 emits - 2.95.2 uses sq (128-bit). Leaf functions match under both,
# so only framed functions discriminate between the two versions.
CC = os.path.join(BIN, "ee-gcc.exe")
OBJDUMP = os.path.join(BIN, "ee-objdump.exe")

# Header search paths the ee-gcc DRIVER does not add by itself.
#
# Found 2026-08-28 by categorising 853 compile failures out of a 1305-task
# local-grind batch: 383 of them died on line 1 with "cstddef: No such file",
# another 47 on <stdint.h>. Neither was a modelling error - we invoke
# `ee-gcc.exe` (the C driver) on C++ sources, and GCC 2.x's C driver compiles
# .cpp with cc1plus but does NOT put the C++ standard header directory on the
# search path the way `g++` would. The headers were in the toolchain the whole
# time, one -I away. That single missing flag was silently failing 35% of
# every batch BEFORE any real verification could run.
#
# GXXINC pulls in <cstddef>/<cstdio>/<cstring>/...; GLIBINC is where those in
# turn find stddef.h; SYSINC is the newlib C headers. SHIMINC is ours and
# holds ONLY typedef-only stand-ins for headers this 1999 toolchain genuinely
# predates (stdint.h is C99). It goes LAST so a real header always wins.
_GCCROOT = os.path.join(REPO, "tools", "prodg", "usr", "local", "sce", "ee",
                        "gcc")
GXXINC = os.path.join(_GCCROOT, "include", "g++-2")
GLIBINC = os.path.join(_GCCROOT, "lib", "gcc-lib", "ee", "2.9-ee-991111",
                       "include")
SYSINC = os.path.join(_GCCROOT, "ee", "include")
SHIMINC = os.path.join(REPO, "tools", "shim")
STD_INCLUDES = [d for d in (GXXINC, GLIBINC, SYSINC, SHIMINC)
                if os.path.isdir(d)]

# The flag sets every tool in this repo must try, in order, before declaring a
# function unmatched. ONE central list, imported by apply_calltasks.py,
# progress.py, audit.py and the generators.
#
# Why this exists: on 2026-08-28 five separate tools each hard-coded
# "-O2 -G0 -fno-exceptions" independently, on the belief - written into the
# project plan - that the whole build used -G0. That belief is wrong for at
# least some modules. gen_opdelete.py decoded 59 operator-delete bodies
# perfectly and scored 0/59 under -G0, then 59/59 under -G4: with a 4-byte
# pointer in small data the original's single gp-relative `lw` appears, and
# without it the compiler emits a lui/lw pair, making a three-word function
# four words long. progress.py, holding the same assumption, then refused to
# COUNT those 59 verified matches.
#
# The rule the five bugs all violate: the matcher and every tool that grades
# the matcher must compile identically, or they measure their own
# configuration instead of the code.
FLAG_SETS = [
    "-O2 -G0 -fno-exceptions",
    "-O2 -G4 -fno-exceptions",
    "-O2 -G8 -fno-exceptions",
    "-O2 -G16 -fno-exceptions",
    # -O1 comes from the Klonoa 2 (kl2_lv_decomp) CMakeLists, the only other
    # public project on this exact compiler (ee-gcc 2.9-991111): it builds C
    # at -O1 and C++ at -O2 -fno-exceptions. This build mixes the two - the
    # .obj modules are EA's C++, but the .o library members (sifrpc, libpad,
    # kprintf, vfprintf, the newlib pieces) are C. Assuming -O2 everywhere is
    # the same class of mistake as assuming -G0 everywhere, so these are
    # tried too. Ordered last: they are the minority case, and FLAG_SETS is
    # walked in order with an early exit on a full match.
    "-O1 -G0 -fno-exceptions",
    "-O1 -G4 -fno-exceptions",
]

# Swept in order; first full match wins. Ordering puts the likely 2002 release
# settings first so the search usually stops on the first few combinations.
OPT = ["-O2", "-O1", "-O3", "-Os", "-O0"]
GPS = ["-G0", "-G4", "-G8", "-G16"]
EXTRA = [
    ["-fno-exceptions"],
    [],
    ["-fno-strict-aliasing"],
    ["-fno-exceptions"],
    ["-fno-strict-aliasing", "-fno-exceptions"],
    ["-mgpopt"],
    ["-funroll-loops"],
    # Found live 2026-08-28 on bDriverLowPS2.o's SOUND_* wrappers: this one
    # module keeps its frame pointer ($s8) even at -O2, which the rest of the
    # 298-module build does not - a real per-module flag difference, not a
    # source-level cause. Confirmed by a compile that matched every
    # instruction present, just in a different schedule, WITHOUT this flag;
    # scheduling-only differences of that shape are exactly what a missing
    # codegen flag looks like, not what a source rewrite fixes.
    ["-fno-exceptions", "-fno-omit-frame-pointer"],
]


def load_elf(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    shoff = struct.unpack_from("<I", blob, 32)[0]
    shent = struct.unpack_from("<H", blob, 46)[0]
    shnum = struct.unpack_from("<H", blob, 48)[0]
    shstrn = struct.unpack_from("<H", blob, 50)[0]

    def sh(i):
        return struct.unpack_from("<10I", blob, shoff + i * shent)

    _, _, _, _, so, ss, _, _, _, _ = sh(shstrn)
    st = blob[so:so + ss]
    secs = {}
    for i in range(shnum):
        n, t, fl, a, o, sz, lk, inf, al, es = sh(i)
        secs[st[n:st.find(b"\0", n)].decode("latin1")] = {
            "type": t, "flags": fl, "addr": a, "off": o, "size": sz,
            "entsize": es, "link": lk, "info": inf, "addralign": al}
    return blob, secs


def symbols(blob, secs):
    sym = secs.get(".symtab")
    if not sym:
        return {}
    es = sym["entsize"] or 16
    strt = secs[".strtab"]
    sstr = blob[strt["off"]:strt["off"] + strt["size"]]
    out = {}
    for i in range(sym["size"] // es):
        nmo, val, size, info, oth, shx = struct.unpack_from(
            "<IIIBBH", blob, sym["off"] + i * es)
        if (info & 0xF) == 2:
            nm = sstr[nmo:sstr.find(b"\0", nmo)].decode("latin1")
            if nm:
                out[nm] = (val, size)
    return out


def original_bytes():
    blob, secs = load_elf(ELF)
    t = secs[".text"]
    text = blob[t["off"]:t["off"] + t["size"]]
    out = {}
    for nm, (val, size) in symbols(blob, secs).items():
        if size:
            o = val - t["addr"]
            if 0 <= o and o + size <= len(text):
                out[nm] = text[o:o + size]
    return out


# Compile cache. ee-gcc takes most of a project scan: 737 sources times six flag
# sets. The compiler is deterministic, so an object is reused only when the same
# source path, the source text, every header it can reach, the flags and the
# compiler binary are all unchanged. Nothing is accepted from the cache: gates
# still read the object, and NFSHP2_NO_CACHE=1 turns it off for a clean proof.
CACHE_DIR = os.path.join(REPO, "work", ".cache", "compile")
_INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)


def cache_enabled():
    return not os.environ.get("NFSHP2_NO_CACHE")


def _source_digest(path, search, seen, digest):
    """Feed a file and everything it can include into digest, once each.

    A conditional include counts whether or not it is taken: at worst that costs
    a needless recompile, never a stale object.
    """
    path = os.path.normcase(os.path.abspath(path))
    if path in seen:
        return
    seen.add(path)
    try:
        with open(path, "rb") as handle:
            text = handle.read()
    except OSError:
        return
    digest.update(path.encode("utf-8", "replace") + b"\0" + text + b"\0")
    here = os.path.dirname(path)
    for name in _INCLUDE.findall(text):
        name = name.decode("latin1")
        for root in [here] + search:
            candidate = os.path.join(root, name)
            if os.path.isfile(candidate):
                _source_digest(candidate, search, seen, digest)
                break
        else:
            # Missing today may exist tomorrow; let the name change the key.
            digest.update(b"missing:" + name.encode("latin1") + b"\0")


def compile_key(src, flags):
    digest = hashlib.sha256()
    stat = os.stat(CC)
    digest.update(("%d %d\0" % (stat.st_size, stat.st_mtime_ns)).encode())
    digest.update(" ".join(flags).encode() + b"\0")
    search = [os.path.dirname(os.path.abspath(src))] + STD_INCLUDES
    _source_digest(src, search, set(), digest)
    return digest.hexdigest()


def _publish(source, target):
    """Copy so that a concurrent reader sees the old file or the whole new one."""
    temp = "%s.%d.%d.tmp" % (target, os.getpid(), threading.get_ident())
    shutil.copyfile(source, temp)
    os.replace(temp, target)


def compile_src(src, flags, out_o):
    # -I the source's own directory explicitly. This old GCC fails to derive it
    # from a path containing Windows separators, so a sibling header resolves
    # when invoked as src/x.cpp but not as src\x.cpp.
    incdir = os.path.dirname(os.path.abspath(src))
    inc = ["-I", incdir]
    for d in STD_INCLUDES:
        inc += ["-I", d]

    cached = None
    if cache_enabled():
        key = compile_key(src, flags)
        cached = os.path.join(CACHE_DIR, key[:2], key)
        if os.path.exists(cached + ".err"):
            with open(cached + ".err", encoding="utf-8") as handle:
                return False, handle.read()
        if os.path.exists(cached + ".o"):
            with open(cached + ".txt", encoding="utf-8") as handle:
                stderr = handle.read()
            shutil.copyfile(cached + ".o", out_o)
            return True, stderr

    cmd = [CC, "-c", "-EL"] + inc + flags + [src, "-o", out_o]
    r = subprocess.run(cmd, capture_output=True, text=True)
    ok, stderr = r.returncode == 0, (r.stderr or "").strip()

    if cached:
        os.makedirs(os.path.dirname(cached), exist_ok=True)
        temp = "%s.%d.%d.txt" % (cached, os.getpid(), threading.get_ident())
        with open(temp, "w", encoding="utf-8") as handle:
            handle.write(stderr)
        if ok:
            os.replace(temp, cached + ".txt")
            _publish(out_o, cached + ".o")
        else:
            os.replace(temp, cached + ".err")
    return ok, stderr


def compiled_funcs(obj):
    """name -> (bytes, offset_in_text)"""
    blob, secs = load_elf(obj)
    t = secs.get(".text")
    if not t:
        return {}
    text = blob[t["off"]:t["off"] + t["size"]]
    out = {}
    for nm, (val, size) in symbols(blob, secs).items():
        out[nm] = ((text[val:val + size] if size else text[val:]), val)
    return out


def relocs(obj, func_off, size):
    """Offsets inside a function that carry a relocation.

    A call to a not-yet-written function assembles as `jal 0` plus a reloc, so
    comparing it raw would report a mismatch for every caller until the whole
    module exists. Masking relocated words lets a function be declared matched
    on its own merits.
    """
    blob, secs = load_elf(obj)
    out = set()
    for name, s in secs.items():
        # Only relocations that patch .text. Offsets in .rel.rodata /
        # .rel.data / .rel.debug are relative to THOSE sections; comparing
        # them against a .text range masked real code words at random
        # (found 2026-10-23: commandName and DumpVifPacket "matched" wrong).
        if name not in (".rel.text", ".rela.text"):
            continue
        es = s["entsize"] or 8
        for i in range(s["size"] // es):
            r_off, r_info = struct.unpack_from("<II", blob, s["off"] + i * es)
            if func_off <= r_off < func_off + size:
                out.add(r_off - func_off)
    return out


def diff_words(a, b, skip=frozenset()):
    n = max(len(a), len(b)) // 4
    rows = []
    for i in range(n):
        off = i * 4
        if off in skip:
            continue
        wa = struct.unpack_from("<I", a, off)[0] if off + 4 <= len(a) else None
        wb = struct.unpack_from("<I", b, off)[0] if off + 4 <= len(b) else None
        if wa != wb:
            rows.append((off,
                         f"{wa:08x}" if wa is not None else "----",
                         f"{wb:08x}" if wb is not None else "----"))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--func", action="append", default=[])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--flags", help="fixed flag string; skips the sweep")
    ap.add_argument("--show-diff", type=int, default=6)
    ap.add_argument("--sweep", action="store_true",
                    help="try each set in FLAG_SETS (the -G variants this "
                         "build actually uses) instead of one fixed --flags "
                         "string or the full 160-combination search")
    args = ap.parse_args()

    if not os.path.exists(CC):
        print("compiler not found:", CC, file=sys.stderr)
        sys.exit(1)

    orig = original_bytes()
    tmp = tempfile.mkdtemp(prefix="matchfn_")

    if args.sweep:
        combos = [f.split() for f in FLAG_SETS]
    else:
        combos = ([args.flags.split()] if args.flags else
                  [[o, g] + e for o, g, e in itertools.product(OPT, GPS, EXTRA)])

    print(f"source: {args.source}")
    print(f"flag combinations to try: {len(combos)}\n")

    best = None
    for idx, flags in enumerate(combos):
        obj = os.path.join(tmp, f"t{idx}.o")
        ok, err = compile_src(args.source, flags, obj)
        if not ok:
            if idx == 0:
                print("compile failed with", " ".join(flags))
                print(err[:1200])
                sys.exit(2)
            continue
        got = compiled_funcs(obj)
        targets = args.func or [k for k in got if k in orig]
        if not targets:
            continue
        res = []
        for fn in targets:
            if fn not in orig:
                res.append((fn, "no reference", 0, 0))
                continue
            a, aoff = got.get(fn, (b"", 0))
            b = orig[fn]
            skip = relocs(obj, aoff, len(b))
            d = diff_words(a, b, skip)
            if not d and len(a) == len(b):
                tag = "MATCH" if not skip else "MATCH*"
                res.append((fn, tag, 0, len(b) // 4))
            else:
                res.append((fn, "differ", len(d), len(b) // 4))
        nmatch = sum(1 for _, s, _, _ in res if s.startswith("MATCH"))
        score = (nmatch, -sum(d for _, s, d, _ in res if s == "differ"))
        if best is None or score > best[0]:
            best = (score, flags, res, obj)
        if nmatch == len(res):
            print(f"ALL MATCH with: {' '.join(flags)}")
            for fn, s, d, w in res:
                print(f"   {s:8s} {w:4d} words  {fn}")
            return

    score, flags, res, obj = best
    print(f"best flags: {' '.join(flags)}   matched {score[0]}/{len(res)}\n")
    for fn, s, d, w in sorted(res, key=lambda x: x[2]):
        print(f"   {s:8s} diff={d:3d}/{w:<4d} words  {fn}")

    got = compiled_funcs(obj)
    for fn, s, d, w in res:
        if s == "differ" and args.show_diff:
            print(f"\n--- {fn}: first {args.show_diff} differing words "
                  f"(mine vs original) ---")
            mineb, moff = got.get(fn, (b"", 0))
            for off, mine, want in diff_words(
                    mineb, orig[fn], relocs(obj, moff, len(orig[fn])))[:args.show_diff]:
                print(f"   +{off:04x}   {mine}   {want}")


if __name__ == "__main__":
    main()
