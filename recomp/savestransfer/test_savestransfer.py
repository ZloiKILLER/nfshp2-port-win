#!/usr/bin/env python3
"""The test of savestransfer.exe: every format, the saves it must refuse, and that it never writes over a profile.

    python test_savestransfer.py <save.dat> [--exe recompwin/savestransfer.exe] [--work work/savestransfer_test] [--card <a real .ps2>]...

<save.dat> is a real save of the game (it is only read). Everything is made new under --work, a game folder of its own included.
"""
import argparse
import calendar
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FAILS = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        FAILS.append(what)


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def tree(d):
    out = {}
    for base, dirs, files in os.walk(d):
        for f in files:
            p = os.path.join(base, f)
            out[os.path.relpath(p, d)] = (sha(p), os.path.getmtime(p))
        for x in dirs:
            out[os.path.relpath(os.path.join(base, x), d) + "/"] = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save")
    ap.add_argument("--exe", default=os.path.join(ROOT, "recompwin", "savestransfer.exe"))
    ap.add_argument("--work", default=os.path.join(ROOT, "work", "savestransfer_test"))
    ap.add_argument("--card", action="append", default=[])
    a = ap.parse_args()
    save = open(a.save, "rb").read()

    # the permutation in savestransfer.cpp is mymc+'s
    cpp = open(os.path.join(HERE, "savestransfer.cpp")).read()
    table = cpp[cpp.index("kCbsRc4[256] = {"):]
    table = [int(x, 16) for x in re.findall(r"0x[0-9a-f]{2}", table[:table.index("};")])]
    ref = [int(x, 16) for x in open(os.path.join(HERE, "cbs_rc4.txt")).read().split()]
    check(table == ref, "the CodeBreaker permutation is mymc+'s")

    shutil.rmtree(a.work, ignore_errors=True)
    src = os.path.join(a.work, "src")
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "make_test_saves.py"), a.save, src], check=True)
    game = os.path.join(a.work, "game")
    os.makedirs(os.path.join(game, "data"))
    for f in ("index.txt", "SLUS_203.62"):
        open(os.path.join(game, "data", f), "wb").write(b"test")
    os.makedirs(os.path.join(game, "saves", "Player1"))
    open(os.path.join(game, "saves", "Player1", "save.dat"), "wb").write(b"P" * 70880)
    open(os.path.join(game, "saves", "Player1", "controls.ini"), "w").write("[Keys]\n")

    def run(*args):
        r = subprocess.run([a.exe, "--game", game, *args], capture_output=True, text=True, encoding="utf-8")
        return r.returncode, r.stdout

    # one source at a time: its saves, then the first imported
    jst_1103 = calendar.timegm((2026, 11, 3, 21 - 9, 45, 30))
    cases = [  # (source, importable, refused, time on the card)
        ("card_ecc.ps2", 1, 2, jst_1103),
        ("card_plain.mcd", 1, 0, jst_1103),
        ("Mcd003.ps2", 1, 0, None),
        ("BESLES-50731HP2", 1, 0, None),
        ("save.psu", 1, 0, jst_1103),
        ("save.max", 1, 0, None),
        ("save.cbs", 1, 0, jst_1103),
        ("save_whole.cbs", 1, 0, jst_1103),
        ("other_game.psu", 0, 0, None),
    ]
    for i, (name, good, bad, when) in enumerate(cases):
        path = os.path.join(src, name)
        code, out = run("--list", path)
        n_good = len(re.findall(r"^\s+\d+\. ", out, re.M))
        n_bad = len(re.findall(r"^\s+- .* from .*: ", out, re.M))
        check(code == 0 and n_good == good and n_bad == bad, f"{name}: {n_good} to import, {n_bad} refused")
        if not good:
            check("no Hot Pursuit 2 save" in out, f"{name}: said to hold none")
            continue
        prof = f"T{i}"
        code, out = run("--import", "1", "--name", prof, path)
        made = os.path.join(game, "saves", prof, "save.dat")
        check(code == 0 and os.path.isfile(made) and open(made, "rb").read() == save, f"{name}: imported as {prof}, the same bytes")
        check(sorted(os.listdir(os.path.dirname(made))) == ["save.dat"], f"{name}: only save.dat in the new profile")
        if when:
            check(abs(os.path.getmtime(made) - when) < 2, f"{name}: the card's time kept")

    code, out = run("--list", os.path.join(src, "card_ecc.ps2"))
    check("pre-release" in out and "70876" not in out, "card_ecc.ps2: the NFS6 save refused as pre-release")
    check("its save is 100 bytes" in out, "card_ecc.ps2: the short save refused by its length")

    # names that must not make anything, and never touch Player1
    before = tree(os.path.join(game, "saves"))
    for bad in ["Player1", "pLaYeR1", "T0", "con", "Con.txt", "a/b", "a:b", "x" * 16, "   ", "..."]:
        code, out = run("--import", "1", "--name", bad, os.path.join(src, "save.psu"))
        check(code != 0, f"name {bad!r} refused")
    check(tree(os.path.join(game, "saves")) == before, "nothing in saves/ changed by the refused names")
    code, out = run("--import", "1", "--name", "Fifteen_letters", os.path.join(src, "save.psu"))
    check(code == 0, "a name of 15 letters is taken")
    code, out = run("--import", "1", "--name", "Tail. . ", os.path.join(src, "save.psu"))
    check(code == 0 and os.path.isdir(os.path.join(game, "saves", "Tail")), "spaces and dots at the end go, as in the game")

    # a save from before profiles is Player1 to be: that name is taken too
    legacy = os.path.join(a.work, "legacy")
    shutil.copytree(game, legacy)
    shutil.rmtree(os.path.join(legacy, "saves"))
    os.makedirs(os.path.join(legacy, "saves"))
    open(os.path.join(legacy, "saves", "save.dat"), "wb").write(b"L" * 70880)
    r = subprocess.run([a.exe, "--game", legacy, "--import", "1", "--name", "player1", os.path.join(src, "save.psu")], capture_output=True, text=True)
    check(r.returncode != 0 and os.listdir(os.path.join(legacy, "saves")) == ["save.dat"], "Player1 taken by a save from before profiles")

    # not a game folder
    r = subprocess.run([a.exe, "--game", src, "--list"], capture_output=True, text=True)
    check(r.returncode != 0, "a folder without the game is refused")

    for c in a.card:
        before = sha(c)
        code, out = run("--list", c)
        print(out)
        check(code == 0 and sha(c) == before, f"{c}: read, not changed")

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
