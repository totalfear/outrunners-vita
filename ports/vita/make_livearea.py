"""Makes the Vita's LiveArea pictures from the project's own icon art (ports/sdl2/icon) and Kenney Mini (CC0).
usage: python make.py REPO OUTDIR"""
import sys, math
from PIL import Image, ImageDraw

repo, out = sys.argv[1], sys.argv[2]
car = Image.open(repo + "/ports/sdl2/icon/car.png").convert("RGBA")
car = car.crop(car.getbbox())
square = Image.open(repo + "/ports/switch/icon.jpg").convert("RGB")
font = Image.open(repo + "/ports/sdl2/font/font.bmp").convert("L")

TOP, MID, LOW = (36, 22, 84), (196, 46, 104), (255, 146, 46)

def gradient(w, h, horizon=None):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        t = y / max(1, h - 1)
        if t < 0.55:
            a, b, u = TOP, MID, t / 0.55
        else:
            a, b, u = MID, LOW, (t - 0.55) / 0.45
        c = tuple(int(a[i] + (b[i] - a[i]) * u) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return im

def glyph(ch):
    i = ord(ch) - 0x20
    cell = font.crop(((i % 16) * 16, (i // 16) * 16, (i % 16) * 16 + 16, (i // 16) * 16 + 16))
    box = cell.point(lambda v: 255 if v > 128 else 0).getbbox()
    return cell, box

def text_size(text, scale):
    w = 0
    for ch in text:
        cell, box = glyph(ch)
        w += ((box[2] - box[0]) if box else 6) + 2
    return (w - 2) * scale, 16 * scale

def draw_text(im, text, scale, cx, y, fill, shadow=(20, 10, 50), outline=None):
    w, _ = text_size(text, scale)
    x = cx - w // 2
    for ch in text:
        cell, box = glyph(ch)
        if not box:
            x += 8 * scale
            continue
        part = cell.crop((box[0], 0, box[2], 16)).point(lambda v: 255 if v > 128 else 0)
        big = part.resize((part.width * scale, 16 * scale), Image.NEAREST)
        for dx, dy, col in ([(scale, scale, shadow)] if shadow else []) + [(0, 0, fill)]:
            im.paste(Image.new("RGB", big.size, col), (x + dx, y + dy), big)
        x += (box[2] - box[0] + 2) * scale

def sun(im, cx, cy, r, horizon):
    d = ImageDraw.Draw(im)
    for y in range(cy - r, min(cy + r, horizon)):
        dy = y - cy
        half = int(math.sqrt(max(0, r * r - dy * dy)))
        t = (y - (cy - r)) / (2 * r)
        col = (255, int(230 - 120 * t), int(90 - 60 * t))
        band = (y - cy) > 0 and ((y - cy) // 6) % 2 == 1 and (y - cy) // 6 >= 1
        if not band:
            d.line([(cx - half, y), (cx + half, y)], fill=col)

def road(im, horizon, w, h):
    d = ImageDraw.Draw(im)
    d.rectangle([0, horizon, w, h], fill=(28, 14, 46))
    vx = w // 2
    for i in range(-12, 13):
        d.line([(vx, horizon), (vx + i * w // 6, h)], fill=(230, 60, 150), width=2)
    y, gap = horizon + 4, 4
    while y < h:
        d.line([(0, y), (w, y)], fill=(230, 60, 150), width=2)
        gap = int(gap * 1.35) + 1
        y += gap
    d.line([(0, horizon), (w, horizon)], fill=(255, 190, 90), width=3)

def paste_car(im, height, cx, bottom):
    s = height / car.height
    c = car.resize((int(car.width * s), height), Image.LANCZOS)
    im.paste(c, (cx - c.width // 2, bottom - height), c)

def save_indexed(im, path):
    im.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)

# icon0: the desktop's icon, full bleed (the LiveArea shows it in a bubble)
save_indexed(square.resize((128, 128), Image.LANCZOS), out + "/icon0.png")

# bg: 840 x 500, the sunset road, the car, the name
W, H, HOR = 840, 500, 300
bg = gradient(W, HOR)
full = Image.new("RGB", (W, H))
full.paste(bg, (0, 0))
sun(full, W // 2, HOR - 10, 130, HOR)
road(full, HOR, W, H)
paste_car(full, 150, 150, H - 16)
draw_text(full, "OUTRUNNERS", 5, 545, 400, (255, 214, 64))
save_indexed(full, out + "/bg.png")

# startup: 280 x 158, the gate's picture behind the start button
W, H, HOR = 280, 158, 104
st = Image.new("RGB", (W, H))
st.paste(gradient(W, HOR), (0, 0))
sun(st, W // 2, HOR - 4, 46, HOR)
road(st, HOR, W, H)
paste_car(st, 58, W // 2, H - 6)
draw_text(st, "OUTRUNNERS", 2, W // 2, 10, (255, 214, 64))
save_indexed(st, out + "/startup.png")
print("made")
