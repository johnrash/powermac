#!/usr/bin/env python3
# PowerMac emulator - lists each source file against its BeBox origin.
# Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
#
# usage: tools/provenance.py BEBOX [ROOT]   (BEBOX: the BeBox emulator's
# source tree; ROOT: the PowerMac tree, by default this script's)
# Prints "path<TAB>origin<TAB>same|changed (N lines)|new" per file: same
# means identical after the renames (namespace, include guards, headers),
# so fixes can go both ways unchanged.
import os, re, sys, difflib
if len(sys.argv) < 2:
    sys.exit('usage: tools/provenance.py BEBOX [ROOT]')
bebox = sys.argv[1]
root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..')
renames = [
    (r'\bbebox\b', 'powermac'), (r'\bBB_', 'PM_'), (r'\bBEBOX_', 'PM_'),
    (r'the BeBox emulator authors', 'the BeBox and PowerMac emulator authors'),
    (r'BeBox emulator -', 'PowerMac emulator -'),
    (r'BeBox emulator for Haiku -', 'PowerMac emulator for Haiku -'),
]
def norm(text):
    for a, b in renames:
        text = re.sub(a, b, text)
    return text.splitlines()
rows = []
for top in ('src', 'tests/linux', 'tools', 'package', 'artwork'):
    for dirpath, dirs, files in os.walk(os.path.join(root, top)):
        dirs[:] = [d for d in dirs if d not in ('out', '__pycache__', 'pkg')]
        for f in sorted(files):
            if not re.search(r'\.(cpp|h|py|sh|S|rdef|hvif|svg|txt|md)$|^Makefile$|^PackageInfo$', f):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, root)
            cands = [rel, rel.replace('powermac', 'bebox'),
                     rel.replace('macio/', 'hobbit/'),
                     rel.replace('powermac-img', 'bebox-img'),
                     rel.replace('PowerMac', 'BeBox')]
            origin = next((c for c in cands
                           if os.path.isfile(os.path.join(bebox, c))), None)
            if origin is None:
                rows.append((rel, '', 'new'))
                continue
            a = norm(open(os.path.join(bebox, origin), errors='replace').read())
            b = norm(open(path, errors='replace').read())
            if a == b:
                rows.append((rel, origin, 'same'))
            else:
                changed = sum(1 for l in difflib.unified_diff(a, b, n=0)
                              if l[:1] in '+-' and l[:3] not in ('+++', '---'))
                rows.append((rel, origin, 'changed (%d lines)' % changed))
for r in rows:
    print('\t'.join(r))
