"""PPM (P6) to PNG, scaled by an integer factor, no Pillow needed."""
import struct, sys, zlib

def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6"
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def write_png(path, w, h, rgb, scale):
    rows = []
    for y in range(h):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        scaled = b"".join(row[x * 3:x * 3 + 3] * scale for x in range(w))
        rows.append(b"\x00" + scaled)
    raw = b"".join(r for r in rows for _ in range(scale))
    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)

if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb, scale)
