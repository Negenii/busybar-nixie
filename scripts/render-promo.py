"""Compose raw 72x16 frames into the BUSY Bar device render as glowing LEDs.

Usage: render-promo.py <frames.raw> <out.gif> [fps] [width]

The frames come from app/tests/render_seq.c. One palette is built for the
whole clip and the bezel is written once: every later frame carries only the
pixels that changed, the rest transparent, so the device does not shimmer
and the file stays small.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter

W, H = 72, 16
HERE = os.path.dirname(os.path.abspath(__file__))
device = Image.open(os.path.join(HERE, "..", "docs", "busybar-device.png")).convert("RGBA")
SX0, SY0, SX1, SY1 = 15, 43, 753, 240           # the black screen inside the render
cell = (SX1 - SX0 + 1) / W                       # ~10.26 px per LED
oy = SY0 + ((SY1 - SY0 + 1) - cell * H) / 2
size = cell * 0.78
inset = (cell - size) / 2
radius = size * 0.35
TRANSPARENT = 255


def led_layer(frame):
    layer = Image.new("RGBA", device.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for y in range(H):
        for x in range(W):
            r, g, b = frame[(y * W + x) * 3:(y * W + x) * 3 + 3]
            if r == g == b == 0:
                continue
            px, py = SX0 + x * cell + inset, oy + y * cell + inset
            d.rounded_rectangle((px, py, px + size, py + size), radius=radius, fill=(r, g, b, 255))
    return layer


def compose(frame):
    leds = led_layer(frame)
    glow = leds.filter(ImageFilter.GaussianBlur(cell * 0.9))
    out = device.copy()
    out.alpha_composite(glow)
    out.alpha_composite(glow)
    out.alpha_composite(leds)
    return out.convert("RGB")


def main():
    raw = open(sys.argv[1], "rb").read()
    out_path = sys.argv[2]
    fps = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    width = int(sys.argv[4]) if len(sys.argv) > 4 else device.width
    n = len(raw) // (W * H * 3)
    rgb = []
    for i in range(n):
        im = compose(raw[i * W * H * 3:(i + 1) * W * H * 3])
        if width != im.width:
            im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
        rgb.append(im)

    # One palette for the clip, 255 colours, built from a sample of frames so
    # the bezel greys and the tube colours are both in it. Index 255 is kept
    # for "unchanged".
    sample = Image.new("RGB", (rgb[0].width, rgb[0].height * 4))
    for k, i in enumerate(range(0, n, max(1, n // 4))[:4]):
        sample.paste(rgb[i], (0, k * rgb[0].height))
    palette_img = sample.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)

    frames = []
    prev = None
    for im in rgb:
        q = im.quantize(palette=palette_img, dither=Image.Dither.NONE)
        if prev is not None:
            # everything identical to the previous frame becomes transparent
            a, b = q.load(), prev.load()
            for y in range(q.height):
                for x in range(q.width):
                    if a[x, y] == b[x, y]:
                        a[x, y] = TRANSPARENT
        else:
            pass
        frames.append(q)
        prev = im.quantize(palette=palette_img, dither=Image.Dither.NONE)

    frames[0].save(
        out_path,
        save_all=True,
        append_images=frames[1:],
        duration=round(1000 / fps),
        loop=0,
        optimize=False,
        disposal=1,
        transparency=TRANSPARENT,
    )
    rgb[n // 2].save(os.path.splitext(out_path)[0] + ".png")
    print("frames", n, "->", out_path, os.path.getsize(out_path) // 1024, "KB")


if __name__ == "__main__":
    main()
