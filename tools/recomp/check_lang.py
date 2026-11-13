#!/usr/bin/env python3
"""check_lang.py: the port's menu texts against the language files (recomp/data/lang/*.txt, written by make_lang.py).

Every string literal of the files that make the port's menus (FILES) is either a text for the player, and then every language file
must have a line for it, or it is not one (an object's name, a page's id, a key of the ini), and then it is listed in
recomp/data/lang_not_texts.txt. A literal that is neither is an error: a new text without its translations, or a new name to list.
Exit code 1 then, so the build can stop on it (recomp/CMakeLists.txt runs this before the exe is linked).

  check_lang.py            check
  check_lang.py --list     print the literals that are neither, one a line (to translate, or to append to lang_not_texts.txt)
"""
import io
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
RUNTIME = os.path.join(ROOT, 'recomp', 'runtime')
LANG = os.path.join(ROOT, 'recomp', 'data', 'lang')
FILES = ('pcsettings.cpp', 'pcmenu.cpp')
LANGS = ('FRENCH', 'GERMAN', 'ITALIAN', 'SPANISH', 'SWEDISH')


def literals(path):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    s = re.sub(r'//[^\n]*', '', s)
    s = re.sub(r'"[ \t]*\n[ \t]*"', '', s)          # "a" newline "b" is one literal
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', s):
        line = s[s.rfind('\n', 0, m.start()) + 1:s.find('\n', m.end())]
        if re.search(r'#include|getenv|fprintf|\bprintf|strcmp|strncmp|host_fatal|sscanf|snprintf', line):
            continue
        k = m.group(1).replace('\\"', '"').replace("\\'", "'").replace('\\\\', '\\')
        if re.search(r'[A-Za-z]{2}', k):
            out.append(k)
    return out


def main():
    keys = {}
    for lang in LANGS:
        ks = set()
        for l in io.open(os.path.join(LANG, lang + '.txt'), encoding='utf-8'):
            l = l.rstrip('\n')
            if l and l[0] not in ';@#' and '\t' in l:
                ks.add(l.split('\t', 1)[0])
        keys[lang] = ks
    not_texts = set()
    p = os.path.join(LANG, '..', 'lang_not_texts.txt')      # not in lang/: every .txt there is a language
    if os.path.exists(p):
        for l in io.open(p, encoding='utf-8'):
            l = l.rstrip('\n')
            if l and not l.startswith(';'):
                not_texts.add(l)
    found = []
    for f in FILES:
        for k in literals(os.path.join(RUNTIME, f)):
            if k not in found:
                found.append(k)
    # (a literal's spaces at its end are not kept by every editor: the list is compared without them)
    bad = [k for k in found if k not in not_texts and k.rstrip() not in not_texts and any(k not in keys[lang] for lang in LANGS)]
    if '--list' in sys.argv:
        sys.stdout.reconfigure(encoding='utf-8')
        for k in bad:
            print(k)
        return 0
    stale = sorted(k for k in keys[LANGS[0]] if k not in found)
    if bad:
        sys.stderr.reconfigure(encoding='utf-8')
        print('check_lang: %d texts of the menus have no translation (tools/recomp/make_lang.py) and are not in recomp/data/lang_not_texts.txt:' % len(bad),
              file=sys.stderr)
        for k in bad:
            print('    ' + k, file=sys.stderr)
        return 1
    print('check_lang: %d texts, each in %d languages; %d literals that are not texts; %d lines no menu uses' % (
        sum(1 for k in found if k in keys[LANGS[0]]), len(LANGS), sum(1 for k in found if k in not_texts or k.rstrip() in not_texts), len(stale)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
