#!/usr/bin/env python3
"""Fail unless every version string in the tree agrees with version.h."""
import re, sys
v = re.search(r'AUDIOTARD_VERSION "([\d.]+)"',
              open('src/version.h').read()).group(1)
enc = sum(int(x) * m for x, m in zip(v.split('.'), (10000, 100, 1)))
required = [
    ('wasm/exports.c', f'return {enc};'),
    ('wasm/index.html', f'APP_V = "{v}"'),
    ('wasm/index.html', f'expect:{enc}'),
    ('wasm/index.html', f'v{v}'),
    ('index.html', f'v{v}'),
    ('README.md', f'**v{v}**'),
]
bad = [f"{p}: missing '{s}'" for p, s in required
       if s not in open(p).read()]
for p in ('wasm/index.html', 'index.html', 'README.md'):
    for hit in re.findall(r'v(\d+\.\d+\.\d+)', open(p).read()):
        if hit != v:
            bad.append(f"{p}: stale version v{hit}")
if bad:
    print("VERSION CHECK FAILED:")
    for b in bad: print(" ", b)
    sys.exit(1)
print(f"version check OK: everything agrees on {v} (encoded {enc})")
