#!/usr/bin/env python3
"""Copies what the release repository holds from this working tree into it: nfshp2-port-win/ (github ZloiKILLER/nfshp2-port-win).

Only what the list below names goes there: the port's own code, the recompiler and the files the exe writes on its first start. Never the
game: no work/ (the generated code, the retail program, the disc's files), no src/ (the decompilation), no recompwin/ data. A file the list
had and does not any more is taken out of the repository; README.md, .gitignore and .git there are the repository's own and stay.

    python tools/sync_repo.py [--repo nfshp2-port-win] [--check]     --check: list what would change, change nothing
"""
import argparse
import filecmp
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (from, to): a file, or a folder with all under it; to is the same path in the repository unless given
LIST = [
    ("recomp/CMakeLists.txt", None),
    ("recomp/runtime", None),
    ("recomp/data", None),
    ("recomp/docs", None),
    ("recomp/symbols", None),
    ("tools/recomp/gen.py", None),
    ("tools/recomp/r5900.py", None),
    ("tools/recomp/make_lang.py", None),
    ("tools/recomp/make_cursor.py", None),
    ("tools/match_func.py", None),
    ("tools/sync_repo.py", None),
    ("work/ref/sdl3/SDL3-3.4.18.tar.gz", "third_party/SDL3-3.4.18.tar.gz"),
]
SKIP = {"recomp/runtime/elf_image.cpp"}         # no longer built: the program comes from the disc
OWN = {"README.md", ".gitignore", ".gitattributes", ".git"}


def wanted():
    out = {}
    for src, dst in LIST:
        dst = dst or src
        full = os.path.join(ROOT, *src.split("/"))
        if os.path.isdir(full):
            for d, dirs, files in os.walk(full):
                dirs[:] = [x for x in dirs if x != "__pycache__"]
                for f in files:
                    rel = os.path.relpath(os.path.join(d, f), full).replace("\\", "/")
                    if f"{src}/{rel}" not in SKIP:
                        out[f"{dst}/{rel}"] = os.path.join(d, f)
        elif os.path.isfile(full):
            out[dst] = full
        else:
            sys.exit(f"missing: {src}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.join(ROOT, "nfshp2-port-win"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    want = wanted()
    have = {}
    for d, dirs, files in os.walk(a.repo):
        rel_d = os.path.relpath(d, a.repo).replace("\\", "/")
        if rel_d == ".":
            dirs[:] = [x for x in dirs if x not in OWN]
            files = [f for f in files if f not in OWN]
        for f in files:
            rel = f if rel_d == "." else f"{rel_d}/{f}"
            have[rel] = os.path.join(d, f)
    added = sorted(set(want) - set(have))
    removed = sorted(set(have) - set(want))
    changed = sorted(r for r in set(want) & set(have) if not filecmp.cmp(want[r], have[r], shallow=False))
    for tag, rows in (("+", added), ("~", changed), ("-", removed)):
        for r in rows:
            print(tag, r)
    if a.check:
        return
    for r in added + changed:
        to = os.path.join(a.repo, *r.split("/"))
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.copyfile(want[r], to)
    for r in removed:
        os.remove(have[r])
    print(f"{len(added)} added, {len(changed)} changed, {len(removed)} removed")


if __name__ == "__main__":
    main()
