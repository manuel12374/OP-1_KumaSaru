#!/usr/bin/env python3
"""Generate leapfrog's graphics from the two source icons:
  - frog (Font Awesome "frog", CC BY 4.0): rasterised to horizontal runs -> frog_runs.h (drawn by code, mirrored for
    hops to the left)
  - water ("water-broken" icon): tiled along each river row as SVG paths -> leapfrog.svg (groups w0..w7, moved by code)
    python3 tools/gen_assets.py      (needs cairocffi)"""
import os, re
import cairocffi as cairo
HERE = os.path.dirname(os.path.abspath(__file__)); PLUG = os.path.dirname(HERE)
FROG = ("M400 96c41.7 0 75.9 31.8 79.7 72.5l85.6 26.3c25.4 7.8 42.8 31.3 42.8 57.9c0 21.8-11.7 41.9-30.7 52.7l-144.5 82.1"
        "l92.5 92.5h50.7c17.7 0 32 14.3 32 32s-14.3 32-32 32h-64c-8.5 0-16.6-3.4-22.6-9.4L378.9 424.2c11.7-36 3.2-77.1-25.4-105.7"
        "c-40.6-40.6-106.3-40.6-146.9-.1l-73.6 70c-6.4 6.1-6.7 16.2-.6 22.6s16.2 6.6 22.6.6l73.8-70.2l.1-.1l.1-.1c3.5-3.5 7.3-6.6 "
        "11.3-9.2c27.9-18.5 65.9-15.4 90.5 9.2c24.7 24.7 27.7 62.9 9 90.9c-2.6 3.8-5.6 7.5-9 10.9l-37 37H384c17.7 0 32 14.3 32 32"
        "s-14.3 32-32 32H96c-35.3 0-64-28.7-64-64c0-166.4 127-303.1 289.3-318.5C328.2 124.2 360.8 96 400 96m0 104c13.3 0 24-10.7 "
        "24-24s-10.7-24-24-24s-24 10.7-24 24s10.7 24 24 24")
WATER = ["M12 19C9.49345 19 7.91806 17.6547 6.86651 16.3888C6.42721 15.8599 5.58617 15.867 5.20168 16.4323C4.50078 17.4629 3.68402 18.4127 2 18.7859M22 18.7859C20.4123 18.4341 19.5955 17.5697 18.9199 16.6083C18.4965 16.0059 17.5655 16.0425 17.1055 16.6208C16.6953 17.1365 16.2063 17.6119 15.6148 18",
         "M12 9C14.5065 9 16.0819 7.65471 17.1335 6.38877C17.5728 5.85991 18.4138 5.86697 18.7983 6.43233C19.4992 7.46288 20.316 8.41274 22 8.78594M2 8.78594C3.58767 8.4341 4.40448 7.56969 5.08009 6.60834C5.50345 6.00591 6.43454 6.04252 6.89447 6.62076C7.30467 7.13646 7.79373 7.6119 8.38519 8",
         "M22 13.7859C20.4123 13.4341 19.5955 12.5697 18.9199 11.6083C18.4965 11.0059 17.5655 11.0425 17.1055 11.6208C16.0541 12.9427 14.4844 14 12 14C9.49345 14 7.91806 12.6547 6.86651 11.3888C6.42721 10.8599 5.58617 10.867 5.20168 11.4323C4.50078 12.4629 3.68402 13.4127 2 13.7859"]

def parse(d):
    """SVG path -> list of absolute segments ('M', x, y) / ('L', x, y) / ('C', x1, y1, x2, y2, x, y) / ('Z',)"""
    toks = re.findall(r'[MmLlHhVvCcSsZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?', d)
    out, i, cmd, x, y, sx, sy, lc = [], 0, None, 0.0, 0.0, 0.0, 0.0, None
    def num():
        nonlocal i; v = float(toks[i]); i += 1; return v
    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]): cmd = toks[i]; i += 1
        rel = cmd.islower(); C = cmd.upper()
        if C == 'Z': out.append(('Z',)); x, y = sx, sy; lc = None; continue
        if C == 'M':
            nx, ny = num(), num(); x, y = (x + nx, y + ny) if rel else (nx, ny); sx, sy = x, y; out.append(('M', x, y))
            cmd = 'l' if rel else 'L'; lc = None
        elif C in 'LHV':
            if C == 'L': nx, ny = num(), num(); x, y = (x + nx, y + ny) if rel else (nx, ny)
            elif C == 'H': v = num(); x = x + v if rel else v
            else: v = num(); y = y + v if rel else v
            out.append(('L', x, y)); lc = None
        elif C == 'C':
            p = [num() for _ in range(6)]
            if rel: p = [p[0] + x, p[1] + y, p[2] + x, p[3] + y, p[4] + x, p[5] + y]
            out.append(('C', *p)); lc = (p[2], p[3]); x, y = p[4], p[5]
        elif C == 'S':
            p = [num() for _ in range(4)]
            if rel: p = [p[0] + x, p[1] + y, p[2] + x, p[3] + y]
            x1, y1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            out.append(('C', x1, y1, *p)); lc = (p[0], p[1]); x, y = p[2], p[3]
    return out

def emit(segs, s, tx, ty):
    f = lambda v: ('%.2f' % v).rstrip('0').rstrip('.')
    o = []
    for g in segs:
        if g[0] == 'Z': o.append('Z')
        elif g[0] in 'ML': o.append(f'{g[0]}{f(g[1] * s + tx)},{f(g[2] * s + ty)}')
        else: o.append('C' + ' '.join(f'{f(g[k] * s + tx)},{f(g[k + 1] * s + ty)}' for k in (1, 3, 5)))
    return ''.join(o)

# ---- frog: rasterise at FROG_H px tall, anchor = bottom centre, facing right ----
FROG_H = 9
segs = parse(FROG)
xs = [v for g in segs if g[0] != 'Z' for v in g[1::2]]; ys = [v for g in segs if g[0] != 'Z' for v in g[2::2]]
x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
s = FROG_H / (y1 - y0); W = int((x1 - x0) * s + 3)
surf = cairo.ImageSurface(cairo.FORMAT_A8, W, FROG_H + 2); c = cairo.Context(surf)
c.translate(1 - x0 * s, 1 - y0 * s); c.scale(s, s)
for g in segs:
    if g[0] == 'M': c.move_to(g[1], g[2])
    elif g[0] == 'L': c.line_to(g[1], g[2])
    elif g[0] == 'C': c.curve_to(*g[1:])
    else: c.close_path()
c.set_fill_rule(cairo.FILL_RULE_WINDING); c.fill()
surf.flush(); data, stride = bytes(surf.get_data()), surf.get_stride()
cx = 1 + (x1 - x0) * s / 2; by = 1 + FROG_H
runs = []
for yy in range(FROG_H + 2):
    row = [data[yy * stride + xx] > 110 for xx in range(W)]
    xx = 0
    while xx < W:
        if row[xx]:
            st = xx
            while xx < W and row[xx]: xx += 1
            runs.append((st - cx, xx - cx, yy + 0.5 - by))    # px, relative to bottom centre
        else: xx += 1
with open(os.path.join(PLUG, 'frog_runs.h'), 'w') as f:
    f.write('/* generated by tools/gen_assets.py from the Font Awesome "frog" icon (CC BY 4.0, fontawesome.com):\n'
            f' * {len(runs)} horizontal runs {{x0, x1, y}} in Q10 px, origin = bottom centre, facing right, {FROG_H} px tall */\n')
    f.write(f'#define LF_FROG_RUNS {len(runs)}\n#define LF_FROG_HALF_W {int(round((x1 - x0) * s / 2 * 1024))}\n')
    f.write('static const int16_t lf_frog[LF_FROG_RUNS][3] = {\n')
    for a, b, yy in runs: f.write(f'    {{ {int(round(a * 1024))}, {int(round(b * 1024))}, {int(round(yy * 1024))} }},\n')
    f.write('};\n')
print(len(runs), 'frog runs,', 'frog width %.1f px' % ((x1 - x0) * s))
for yy in range(FROG_H + 2): print(''.join('#' if data[yy * stride + xx] > 110 else '.' for xx in range(W)))

# ---- water: 8 river rows, a few tiles each, as groups w0..w7 ----
ROW0, ROWH, TILE = 13, 15, 12                          # must match leapfrog.c (LF_ROW0, LF_ROWH)
wsegs = [parse(p) for p in WATER]
lines = ['<?xml version="1.0" encoding="utf-8"?>',
         '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px"',
         '\t width="320px" height="160px" viewBox="0 0 320 160">',
         '<!-- generated by tools/gen_assets.py - leapfrog: rivers (water icon tiles, one group per row, moved by code) and',
         '     the panel divider; logs, frogs and text are drawn by leapfrog.c -->',
         '<line fill="none" stroke="#3A3C3E" stroke-width="1" x1="195.5" y1="4" x2="195.5" y2="136"/>']
import random
random.seed(7)
for r in range(8):
    lines.append(f'<g id="w{r}">')
    ty = ROW0 + r * ROWH + 3.5
    xs_t = sorted(random.sample(range(6, 182, 12), 4))
    for tx in xs_t:
        for ws in wsegs:
            lines.append(f'<path fill="none" stroke="#2E5B9E" stroke-width="1" stroke-linecap="round" stroke-linejoin="round" d="{emit(ws, TILE / 24, tx, ty)}"/>')
    lines.append('</g>')
lines.append('</svg>')
open(os.path.join(PLUG, 'leapfrog.svg'), 'w').write('\n'.join(lines) + '\n')
print('leapfrog.svg:', sum(1 for l in lines if l.startswith('<path')), 'water paths')
