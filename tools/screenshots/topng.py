#!/usr/bin/env python3
"""Converts the renderer's PPMs to PNG: each scene is masked to the round display, scaled 2x (nearest, so the
pixels stay crisp) and placed in a thin bezel. Also writes a contact sheet of the main scenes."""
import sys, os
from PIL import Image, ImageDraw

d = sys.argv[1] if len(sys.argv) > 1 else "out"
S = 2                                  # scale
PAD = 16                               # bezel/margin around the 240 px circle, at output scale
names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".ppm"))
out = {}
for n in names:
    im = Image.open(os.path.join(d, n + ".ppm")).convert("RGB").resize((240 * S, 240 * S), Image.NEAREST)
    size = 240 * S + 2 * PAD
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    dr = ImageDraw.Draw(canvas)
    dr.ellipse((0, 0, size - 1, size - 1), fill=(38, 38, 42, 255))          # bezel
    dr.ellipse((PAD - 3, PAD - 3, size - PAD + 2, size - PAD + 2), fill=(10, 10, 12, 255))
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, im.size[0] - 1, im.size[1] - 1), fill=255)
    canvas.paste(im, (PAD, PAD), mask)
    out[n] = canvas
    canvas.save(os.path.join(d, n + ".png"))
    print("wrote", n + ".png")

sheet_names = [n for n in ("status_optimal", "status_optimizable", "status_scanning", "history", "setup") if n in out]
if sheet_names:
    w = out[sheet_names[0]].width
    gap = 20
    sheet = Image.new("RGBA", (len(sheet_names) * w + (len(sheet_names) - 1) * gap, w), (0, 0, 0, 0))
    for i, n in enumerate(sheet_names):
        sheet.paste(out[n], (i * (w + gap), 0), out[n])
    sheet.save(os.path.join(d, "overview.png"))
    print("wrote overview.png")
