"""Icona DiskPilot in stile macOS Sequoia: squircle con gradiente + piatto disco con partizioni."""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 2048          # lavoriamo a 2x e poi riduciamo
K = S / 1024.0

def squircle_mask(size, box, n=5.0):
    x0, y0, x1, y1 = box
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    rx, ry = (x1 - x0) / 2, (y1 - y0) / 2
    v = np.abs((xx + 0.5 - cx) / rx) ** n + np.abs((yy + 0.5 - cy) / ry) ** n
    # bordo morbido di ~1.5 px
    edge = 1.5 / rx * n
    a = np.clip((1.0 - v) / edge + 0.5, 0, 1)
    return Image.fromarray((a * 255).astype(np.uint8), "L")

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))

def vgrad(size, top, bottom):
    t = np.linspace(0, 1, size, dtype=np.float32)[:, None]
    top = np.array(top, np.float32); bottom = np.array(bottom, np.float32)
    rows = top + (bottom - top) * t[..., None]
    arr = np.repeat(rows, size, axis=1)
    return Image.fromarray(arr.astype(np.uint8), "RGB" if len(top) == 3 else "RGBA")

def hexc(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)

canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
body_box = (100 * K, 100 * K, 924 * K, 924 * K)
mask = squircle_mask(S, body_box)

# ombra sotto lo squircle
shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sm = mask.point(lambda v: int(v * 0.42))
shadow.putalpha(sm)
shadow = shadow.transform((S, S), Image.AFFINE, (1, 0, 0, 0, 1, -int(14 * K)))
shadow = shadow.filter(ImageFilter.GaussianBlur(22 * K))
canvas = Image.alpha_composite(canvas, shadow)

# corpo con gradiente blu
body = vgrad(S, (98, 178, 255), (10, 84, 222)).convert("RGBA")
# alone chiaro in alto
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(glow)
gd.ellipse((180 * K, -260 * K, 844 * K, 430 * K), fill=(255, 255, 255, 60))
glow = glow.filter(ImageFilter.GaussianBlur(70 * K))
body = Image.alpha_composite(body, glow)
body.putalpha(mask)
canvas = Image.alpha_composite(canvas, body)

# bordo interno sottile (luce in alto)
rim = Image.new("RGBA", (S, S), (255, 255, 255, 0))
inner = squircle_mask(S, (body_box[0] + 3 * K, body_box[1] + 3 * K, body_box[2] - 3 * K, body_box[3] - 3 * K))
ring = Image.fromarray(np.clip(np.array(mask, np.int16) - np.array(inner, np.int16), 0, 255).astype(np.uint8), "L")
fade = vgrad(S, (110, 0, 0), (10, 0, 0)).split()[0]
ring = Image.fromarray((np.array(ring, np.float32) * np.array(fade, np.float32) / 255).astype(np.uint8), "L")
rim.putalpha(ring)
canvas = Image.alpha_composite(canvas, rim)

# piatto del disco
cx, cy = 512 * K, 520 * K
R = 318 * K
plat_shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(plat_shadow).ellipse((cx - R, cy - R + 20 * K, cx + R, cy + R + 20 * K), fill=(0, 30, 90, 120))
plat_shadow = plat_shadow.filter(ImageFilter.GaussianBlur(26 * K))
canvas = Image.alpha_composite(canvas, plat_shadow)

plat = vgrad(S, (255, 255, 255), (222, 230, 242)).convert("RGBA")
pm = Image.new("L", (S, S), 0)
ImageDraw.Draw(pm).ellipse((cx - R, cy - R, cx + R, cy + R), fill=255)
plat.putalpha(pm)
canvas = Image.alpha_composite(canvas, plat)

# anello delle partizioni
Ro, Ri = 292 * K, 168 * K
segments = [  # (gradi, colore alto, colore basso)
    (128, "#3E9BFF", "#0A6CF0"),   # NTFS
    (78, "#4CD964", "#28B04A"),    # FAT
    (62, "#FFB340", "#FF8A00"),    # EFI / sistema
    (52, "#C27BF0", "#9B4FD8"),    # ReFS
]
used = sum(s[0] for s in segments)
start = -90 - 8
gap_deg = 3.2
ringl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
for deg, c1, c2 in segments:
    layer = vgrad(S, hexc(c1)[:3], hexc(c2)[:3]).convert("RGBA")
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).pieslice((cx - Ro, cy - Ro, cx + Ro, cy + Ro), start + gap_deg / 2, start + deg - gap_deg / 2, fill=255)
    layer.putalpha(m)
    ringl = Image.alpha_composite(ringl, layer)
    start += deg
# spazio libero: grigio tratteggiato
free = Image.new("RGBA", (S, S), (214, 222, 234, 255))
fd = ImageDraw.Draw(free)
step = int(26 * K)
for x in range(-S, S * 2, step):
    fd.line((x, 0, x + S, S), fill=(196, 206, 222, 255), width=int(9 * K))
m = Image.new("L", (S, S), 0)
ImageDraw.Draw(m).pieslice((cx - Ro, cy - Ro, cx + Ro, cy + Ro), start + gap_deg / 2, -90 - 8 + 360 - gap_deg / 2, fill=255)
free.putalpha(m)
ringl = Image.alpha_composite(ringl, free)
# buco centrale
hole = Image.new("L", (S, S), 255)
ImageDraw.Draw(hole).ellipse((cx - Ri, cy - Ri, cx + Ri, cy + Ri), fill=0)
ra = np.array(ringl.split()[3], np.float32) * np.array(hole, np.float32) / 255
ringl.putalpha(Image.fromarray(ra.astype(np.uint8), "L"))
canvas = Image.alpha_composite(canvas, ringl)

# perno centrale
hub = Image.new("RGBA", (S, S), (0, 0, 0, 0))
hd = ImageDraw.Draw(hub)
hr = 62 * K
hd.ellipse((cx - hr, cy - hr, cx + hr, cy + hr), fill=(203, 213, 228, 255))
hr2 = 30 * K
hd.ellipse((cx - hr2, cy - hr2, cx + hr2, cy + hr2), fill=(150, 165, 188, 255))
hr3 = 12 * K
hd.ellipse((cx - hr3, cy - hr3, cx + hr3, cy + hr3), fill=(245, 248, 252, 255))
canvas = Image.alpha_composite(canvas, hub)

out_dir = sys.argv[1]
big = canvas.resize((1024, 1024), Image.LANCZOS)
big.save(f"{out_dir}/app_1024.png")
sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
imgs = [canvas.resize((s, s), Image.LANCZOS) for s in sizes]
imgs[-1].save(f"{out_dir}/app.ico", format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
print("ok")
