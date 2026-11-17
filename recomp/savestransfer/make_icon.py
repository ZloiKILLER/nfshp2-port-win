#!/usr/bin/env python3
"""savestransfer.exe's icon: a PS2 memory card. python make_icon.py [out.ico] [preview.png]

Drawn at 1024 x 1024 and scaled down to each size of the .ico (16 to 256). The card as the PS2's 8 MB one looks: black, nearly
square, a tab in the middle of the top edge, notches in the sides, a ribbed grip below a curved line; blue lettering at the top, a
large white 8 MB, MEMORY CARD. The lettering is plain Arial: no maker's logo or emblem is copied.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024
HERE = os.path.dirname(os.path.abspath(__file__))


def font(size, bold=True):
    for name in (("arialbd.ttf", "Arial Bold.ttf") if bold else ("arial.ttf", "Arial.ttf")):
        for d in (os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"), "/usr/share/fonts/truetype/msttcorefonts", "/Library/Fonts"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def vgrad(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", size)
    d = ImageDraw.Draw(g)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line((0, y, w, y), fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return g


def text_center(d, cx, y, s, f, fill):
    l, t, r, b = d.textbbox((0, 0), s, font=f)
    d.text((cx - (r - l) / 2 - l, y - t), s, font=f, fill=fill)


def draw(small):
    # small: for 16 to 32 pixels, the lettering larger and fewer, the ribs coarser
    x0, y0, x1, y1 = 132, 76, 892, 960          # the card: 760 x 884, as the real one's proportions
    cx = (x0 + x1) // 2

    # the card's outline: a rounded body, a tab on the top edge, notches in the sides
    shape = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(shape)
    d.rounded_rectangle((x0, y0, x1, y1), radius=46, fill=255)
    d.rounded_rectangle((cx - 90, y0 - 26, cx + 90, y0 + 20), radius=12, fill=255)
    for sx in (x0 - 2, x1 - 22):
        d.rounded_rectangle((sx, y0 + 96, sx + 24, y0 + 168), radius=8, fill=0)

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # a soft shadow
    sh = shape.filter(ImageFilter.GaussianBlur(24)).point(lambda v: v * 150 // 255)
    img.paste(Image.new("RGBA", (S, S), (0, 0, 0, 255)), (14, 26), sh)
    img.paste(Image.new("RGBA", (S, S), (0, 0, 0, 0)), (0, 0), shape)
    # the rim: lighter at the top, then the face inside it, glossy black
    body = vgrad((S, S), (112, 115, 124), (64, 66, 72))
    img.paste(body, (0, 0), shape)
    face = shape.filter(ImageFilter.MinFilter(17))
    img.paste(vgrad((S, S), (40, 41, 46), (14, 14, 17)), (0, 0), face)

    # the grip: below a shallow curve, a little lighter, with ribs
    grip = Image.new("L", (S, S), 0)
    gd = ImageDraw.Draw(grip)
    top = 600 if small else 660
    gd.polygon([(x0, top + 40)] + [(x, top + 40 - int(48 * (1 - ((x - cx) / (x1 - cx)) ** 2))) for x in range(x0, x1 + 1, 8)] + [(x1, top + 40), (x1, y1), (x0, y1)],
               fill=255)
    grip = Image.composite(grip, Image.new("L", (S, S), 0), face)
    img.paste(vgrad((S, S), (52, 53, 59), (30, 31, 35)), (0, 0), grip)
    ribs = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ribs)
    step, wide = (80, 22) if small else (38, 8)
    for y in range(top + 60, y1 - 40, step):
        rd.line((x0 + 40, y, x1 - 40, y), fill=(10, 10, 12, 200), width=wide)
        rd.line((x0 + 40, y + wide + 1, x1 - 40, y + wide + 1), fill=(80, 82, 90, 120), width=wide // 2)
    img.paste(ribs, (0, 0), Image.composite(ribs.getchannel("A"), Image.new("L", (S, S), 0), grip))
    # the curve's edge, as a light line
    edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(edge).line([(x, top + 40 - int(48 * (1 - ((x - cx) / (x1 - cx)) ** 2))) for x in range(x0 + 20, x1 - 19, 8)], fill=(96, 98, 108, 255), width=6)
    img.alpha_composite(edge)

    # the lettering: blue at the top, the size large and white, MEMORY CARD under it
    d = ImageDraw.Draw(img)
    if small:
        text_center(d, cx, 120, "PS2", font(230), (60, 110, 232, 255))
        f8, fmb, y8 = font(250), font(120), 330
    else:
        text_center(d, cx, 150, "PS2", font(150), (52, 98, 214, 255))
        f8, fmb, y8 = font(250), font(110), 330
    l, t, r, b = d.textbbox((0, 0), "8", font=f8)
    w8 = r - l
    lm, tm, rm, bm = d.textbbox((0, 0), "MB", font=fmb)
    wmb = rm - lm
    left = cx - (w8 + 14 + wmb) / 2
    d.text((left - l, y8 - t), "8", font=f8, fill=(236, 238, 242, 255))
    d.text((left + w8 + 14 - lm, y8 + (b - t) - (bm - tm) - tm), "MB", font=fmb, fill=(236, 238, 242, 255))
    if not small:
        text_center(d, cx, 610, "MEMORY CARD", font(70), (205, 208, 216, 255))
    return img


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "savestransfer.ico")
    big, small = draw(False), draw(True)

    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = [(small if n <= 32 else big).resize((n, n), Image.LANCZOS) for n in sizes]
    frames[0].save(out, format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[1:])
    if len(sys.argv) > 2:
        # a preview: every size side by side on light and dark
        pw = sum(sizes) + 10 * (len(sizes) + 1)
        prev = Image.new("RGBA", (pw, 2 * 276), (255, 255, 255, 255))
        prev.paste(Image.new("RGBA", (pw, 276), (32, 32, 32, 255)), (0, 276))
        x = 10
        for f in frames:
            prev.alpha_composite(f, (x, 10))
            prev.alpha_composite(f, (x, 286))
            x += f.width + 10
        prev.save(sys.argv[2])


if __name__ == "__main__":
    main()
