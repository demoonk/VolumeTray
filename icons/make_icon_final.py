#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Итоговая иконка VolumeTray: заполненный глиф, светло-фиолетовый, фон — альфа,
обрезано по альфе (crop по bbox глифа -> квадрат 1:1 с равными полями).

Шаги:
  1) маска глифа референса (icons/ref_new.png, 460x460, чёрный контур на белом);
  2) заполнение контура (flood fill от края: то, что не достижимо с края, —
     внутренность глифа, значит он становится сплошным — на 16 px тонкий контур
     не читается);
  3) цвет глифа — светло-фиолетовый, фон — прозрачный (альфа);
  4) crop по альфе: bbox глифа, затем квадрат 1:1 с одинаковым полем;
  5) записи 16, 20, 24, 32, 40, 48, 64, 128, 256 (PNG, colortype=6 = RGBA).

Только Pillow.
"""
import os
from collections import deque
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "ref_new.png")

GLYPH = (198, 154, 240)        # светло-фиолетовый
PAD = 8                        # поле после обрезки по альфе (в пикселях референса)
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
THRESH = 128                   # «тёмный пиксель» = глиф


def glyph_mask(im):
    px = im.load()
    w, h = im.size
    m = [[1 if (px[x, y][3] > 128 and 0.299 * px[x, y][0] + 0.587 * px[x, y][1] + 0.114 * px[x, y][2] < THRESH) else 0
          for x in range(w)] for y in range(h)]
    return m


def fill_holes(mask):
    """Всё, что нельзя достать floods-филлом с края, — внутренность контура."""
    h, w = len(mask), len(mask[0])
    seen = [[0] * w for _ in range(h)]
    dq = deque()
    for x in range(w):
        for y in (0, h - 1):
            if not mask[y][x]:
                dq.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if not mask[y][x]:
                dq.append((x, y))
    while dq:
        x, y = dq.popleft()
        if x < 0 or y < 0 or x >= w or y >= h or seen[y][x] or mask[y][x]:
            continue
        seen[y][x] = 1
        dq.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return [[1 if (mask[y][x] or not seen[y][x]) else 0 for x in range(w)] for y in range(h)]


def main():
    ref = Image.open(REF).convert("RGBA")
    w, h = ref.size
    filled = fill_holes(glyph_mask(ref))
    pts = [(x, y) for y in range(h) for x in range(w) if filled[y][x]]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    gw, gh = x1 - x0 + 1, y1 - y0 + 1
    side = max(gw, gh) + 2 * PAD
    ox, oy = (side - gw) // 2, (side - gh) // 2

    master = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    pm = master.load()
    for y in range(gh):
        for x in range(gw):
            if filled[y0 + y][x0 + x]:
                pm[ox + x, oy + y] = GLYPH + (255,)

    imgs = [master.resize((sz, sz), Image.LANCZOS) for sz in SIZES]
    for sz, im in zip(SIZES, imgs):
        im.save(os.path.join(HERE, "preview_%d.png" % sz))
        bg = Image.new("RGBA", (sz, sz), (32, 32, 32, 255))
        bg.alpha_composite(im)
        bg.save(os.path.join(HERE, "check_tray%d.png" % sz))

    out = os.path.join(HERE, "app.ico")
    imgs[-1].save(out, format="ICO", sizes=[(sz, sz) for sz in SIZES])

    tiles = [Image.open(os.path.join(HERE, "check_tray%d.png" % sz)).resize((96, 96), Image.NEAREST)
             for sz in SIZES[:-1]]
    cols = 4
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * 104, rows * 104), (20, 20, 20, 255))
    for i, t in enumerate(tiles):
        sheet.alpha_composite(t, ((i % cols) * 104 + 4, (i // cols) * 104 + 4))
    sheet.save(os.path.join(HERE, "check_montage_final.png"))

    print("crop by alpha: bbox %dx%d -> square %d" % (gw, gh, side))
    print("wrote", out, "sizes", SIZES)


if __name__ == "__main__":
    main()
