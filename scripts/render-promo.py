"""Compose raw 72x16 frames into the BUSY Bar device render as glowing LEDs, and write a GIF + a still."""
import sys
from PIL import Image, ImageDraw, ImageFilter
W, H = 72, 16
import os
device = Image.open(os.path.join(os.path.dirname(__file__), "..", "docs", "busybar-device.png")).convert("RGBA")
SX0, SY0, SX1, SY1 = 15, 43, 753, 240           # the black screen inside the render
cell = (SX1 - SX0 + 1) / W                       # ~10.26 px per LED
grid_h = cell * H
oy = SY0 + ((SY1 - SY0 + 1) - grid_h) / 2
size = cell * 0.78; inset = (cell - size) / 2; radius = size * 0.35

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
    # bloom: a blurred, brightened copy under the crisp LEDs
    glow = leds.filter(ImageFilter.GaussianBlur(cell * 0.9))
    out = device.copy()
    out.alpha_composite(glow)
    out.alpha_composite(glow)
    out.alpha_composite(leds)
    return out.convert("RGB")

raw = open(sys.argv[1], "rb").read()
n = len(raw) // (W * H * 3)
fps = int(sys.argv[3]) if len(sys.argv) > 3 else 30
frames = []
for i in range(n):
    f = raw[i * W * H * 3:(i + 1) * W * H * 3]
    im = compose(f)
    frames.append(im.quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE))
frames[0].save(sys.argv[2], save_all=True, append_images=frames[1:], duration=round(1000 / fps), loop=0, optimize=False, disposal=1)
compose(raw[(n // 2) * W * H * 3:(n // 2 + 1) * W * H * 3]).save(sys.argv[2].replace(".gif", ".png"))
print("frames", n, "->", sys.argv[2])
