#!/usr/bin/env python3
"""Animated preview of the monolith screen: knobs move on a script, the drone plays, frames -> GIF.
    python3 tools/make_anim.py [op1r-src path] [out.gif]
Needs cairocffi and Pillow; uses op1r-src's harness/render.py to draw each frame."""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, '..', 'op1r-src')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, 'demos', 'monolith_anim.gif')
SDK = os.path.join(SRC, 'synth_sdk'); sys.path.insert(0, os.path.join(SDK, 'harness'))
import render                                                   # draw_svg, STYLES, cairo
from PIL import Image
import xml.etree.ElementTree as ET
exe = os.path.join(HERE, 'build', 'anim'); os.makedirs(os.path.dirname(exe), exist_ok=True)
subprocess.run(['cc', '-O1', '-Wall', '-Wno-unused-parameter', '-fsanitize=address,undefined', '-DOP1_HOST',
                '-DENGINE=ENGINE_monolith', '-DSCREEN=SCREEN_monolith', '-I', SDK, '-I', os.path.join(SDK, 'harness'),
                os.path.join(HERE, 'anim_harness.c'), os.path.join(REPO, 'monolith', 'monolith.c'), '-lm', '-o', exe], check=True)
res = subprocess.run([exe, '11000', '16000', '9000', '18000', '1', '660', '-1', '3'], check=True, capture_output=True, text=True)
svg = ET.parse(os.path.join(REPO, 'monolith', 'monolith.svg')).getroot()
cairo, S = render.cairo, 2
frames = []
for line in res.stdout.splitlines():
    fr = json.loads(line)
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 320 * S, 160 * S); ctx = cairo.Context(surf)
    ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
    render.STYLES.clear(); render.draw_svg(ctx, svg, fr['nodes'], set(fr.get('hidden', ())))
    for x1, y1, x2, y2, w, c in fr['lines']:
        ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
        ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
    buf = bytes(surf.get_data())
    frames.append(Image.frombuffer('RGBA', (320 * S, 160 * S), buf, 'raw', 'BGRA', 0, 1).convert('RGB').quantize(64))
frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=50, loop=0, optimize=True)
print(f'{len(frames)} frames -> {OUT}')
