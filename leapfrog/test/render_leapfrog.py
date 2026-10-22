#!/usr/bin/env python3
"""Run the leapfrog host test, then render its screen frames and an animation.
    python3 leapfrog/test/render_leapfrog.py <op1r-src path>
Needs cairocffi and Pillow; uses op1r-src's harness/render.py (SVG + lines)."""
import json, os, subprocess, sys, xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__)); PLUG = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PLUG, '..', '..', 'op1r-src')
DEMOS = os.path.join(PLUG, '..', 'demos')
SDK = os.path.join(SRC, 'synth_sdk'); sys.path.insert(0, os.path.join(SDK, 'harness'))
import render as R
from PIL import Image
exe = os.path.join(HERE, 'build', 'leapfrog_test'); os.makedirs(os.path.dirname(exe), exist_ok=True)
subprocess.run(['cc', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-DOP1_HOST', '-fsanitize=address,undefined',
                '-fno-sanitize-recover=undefined', '-I', SDK, '-I', os.path.join(SDK, 'harness'),
                os.path.join(HERE, 'leapfrog_test.c'), '-lm', '-o', exe], check=True)
chk = subprocess.run([exe], capture_output=True, text=True); print(chk.stdout.strip(), chk.stderr.strip())
if chk.returncode: sys.exit(1)
svg = ET.parse(os.path.join(PLUG, 'leapfrog.svg')).getroot()
cairo = R.cairo
def draw(ctx, fr):
    ctx.set_source_rgb(0, 0, 0); ctx.paint()
    R.draw_svg(ctx, svg, fr['nodes'])
    for x1, y1, x2, y2, w, c in fr['lines']:
        ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
        ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
frames = [json.loads(l) for l in subprocess.run([exe, 'render'], check=True, capture_output=True, text=True).stdout.splitlines() if l.startswith('{')]
S, W, H, pad, lab = R.SCALE, 320 * R.SCALE, 160 * R.SCALE, 12, 22
cols, rows = 2, (len(frames) + 1) // 2
surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
for n, fr in enumerate(frames):
    x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
    ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15)
    ctx.show_text(f"{fr['label']}   ({len(fr['lines'])} lines)"); ctx.restore()
    ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip(); ctx.scale(S, S); draw(ctx, fr); ctx.restore()
out = os.path.join(DEMOS, 'leapfrog_preview.png'); surf.write_to_png(out); print('->', out)
mx = max((max(l[1], l[3]) + l[4] / 2) / 1024 for fr in frames for l in fr['lines']); print('lowest pixel drawn: %.1f' % mx)
anim = [json.loads(l) for l in subprocess.run([exe, 'anim'], check=True, capture_output=True, text=True).stdout.splitlines() if l.startswith('{')]
imgs, S2 = [], 2
for fr in anim:
    sf = cairo.ImageSurface(cairo.FORMAT_RGB24, 320 * S2, 160 * S2); cx = cairo.Context(sf); cx.scale(S2, S2); draw(cx, fr)
    imgs.append(Image.frombuffer('RGBA', (320 * S2, 160 * S2), bytes(sf.get_data()), 'raw', 'BGRA', 0, 1).convert('RGB').quantize(64))
gif = os.path.join(DEMOS, 'leapfrog_anim.gif')
imgs[0].save(gif, save_all=True, append_images=imgs[1:], duration=50, loop=0, optimize=True)
print(len(imgs), 'frames ->', gif)
