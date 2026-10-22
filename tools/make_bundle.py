#!/usr/bin/env python3
"""Build the firmware bundle: op1r 44 (all official plugins) + every plugin in release/, named
op1r44_SceriffoRosso_<version>.op1.
    python3 tools/make_bundle.py <version> <op1r 44 .op1 (from op1r's patch_op1.py)> <op1r checkout> [out dir]
Every file of the op1r image stays byte-identical; the release/*.op1p files are added under plugins/."""
import copy, glob, hashlib, os, sys
ver, base, op1r = sys.argv[1], sys.argv[2], sys.argv[3]
out_dir = sys.argv[4] if len(sys.argv) > 4 else '.'
sys.path.insert(0, op1r)
import patch_op1 as P
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(base, 'rb').read()
_, members = P.unpack(src)
idx = max(i for i, (m, _) in enumerate(members) if m.name.startswith('plugins/'))
add = {}
for f in sorted(glob.glob(os.path.join(REPO, 'release', '*.op1p')), reverse=True):
    info = copy.copy(members[idx][0]); info.name = 'plugins/' + os.path.basename(f)
    add[info.name] = open(f, 'rb').read(); members.insert(idx + 1, (info, add[info.name]))
out = P.pack_op1(P.pack_tar(members))
_, chk = P.unpack(out); _, m0 = P.unpack(src)
a = {m.name: d for m, d in m0}; b = {m.name: d for m, d in chk}
assert set(b) - set(a) == set(add) and all(a[k] == b[k] for k in a) and all(b[k] == v for k, v in add.items())
path = os.path.join(out_dir, f'op1r44_SceriffoRosso_{ver}.op1')
open(path, 'wb').write(out)
print(path, '\n  plugins added:', ', '.join(sorted(n[8:] for n in add)), '\n  sha256', hashlib.sha256(out).hexdigest())
