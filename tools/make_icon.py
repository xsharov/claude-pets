import struct
import sys
import zlib
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SUPERSAMPLE = 4


def hex_color(value, alpha=1.0):
    value = value.lstrip("#")
    return (int(value[0:2], 16) / 255.0, int(value[2:4], 16) / 255.0, int(value[4:6], 16) / 255.0, alpha)


SHAPES = [
    ("ellipse", (0.5, 0.57, 0.425, 0.385), hex_color("#B4583A")),
    ("ellipse", (0.5, 0.56, 0.4, 0.36), hex_color("#D97757")),
    ("ellipse", (0.5, 0.43, 0.27, 0.15), hex_color("#E69373", 0.55)),
    ("ellipse", (0.34, 0.9, 0.09, 0.055), hex_color("#B4583A")),
    ("ellipse", (0.66, 0.9, 0.09, 0.055), hex_color("#B4583A")),
    ("ellipse", (0.385, 0.53, 0.05, 0.075), hex_color("#1E1A18")),
    ("ellipse", (0.615, 0.53, 0.05, 0.075), hex_color("#1E1A18")),
    ("ellipse", (0.4, 0.505, 0.018, 0.022), hex_color("#FFFFFF")),
    ("ellipse", (0.63, 0.505, 0.018, 0.022), hex_color("#FFFFFF")),
    ("ellipse", (0.29, 0.66, 0.055, 0.032), hex_color("#F4A38A", 0.8)),
    ("ellipse", (0.71, 0.66, 0.055, 0.032), hex_color("#F4A38A", 0.8)),
]


def inside(shape, x, y):
    kind, (cx, cy, rx, ry), _ = shape
    if kind == "ellipse":
        dx = (x - cx) / rx
        dy = (y - cy) / ry
        return dx * dx + dy * dy <= 1.0
    return False


def render(size):
    pixels = bytearray(size * size * 4)
    step = 1.0 / (size * SUPERSAMPLE)
    for py in range(size):
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    x = (px * SUPERSAMPLE + sx + 0.5) * step
                    y = (py * SUPERSAMPLE + sy + 0.5) * step
                    r = g = b = a = 0.0
                    for shape in SHAPES:
                        if inside(shape, x, y):
                            sr, sg, sb, sa = shape[2]
                            r = sr * sa + r * (1.0 - sa)
                            g = sg * sa + g * (1.0 - sa)
                            b = sb * sa + b * (1.0 - sa)
                            a = sa + a * (1.0 - sa)
                    acc[0] += r
                    acc[1] += g
                    acc[2] += b
                    acc[3] += a
            samples = SUPERSAMPLE * SUPERSAMPLE
            alpha = acc[3] / samples
            offset = (py * size + px) * 4
            if alpha > 0:
                pixels[offset] = min(255, round(acc[0] / samples / alpha * 255))
                pixels[offset + 1] = min(255, round(acc[1] / samples / alpha * 255))
                pixels[offset + 2] = min(255, round(acc[2] / samples / alpha * 255))
            pixels[offset + 3] = min(255, round(alpha * 255))
    return bytes(pixels)


def png(size, rgba):
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "res" / "app.ico"
    images = [png(size, render(size)) for size in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    offset = 6 + 16 * len(images)
    for size, data in zip(SIZES, images):
        dimension = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    target.write_bytes(header + entries + b"".join(images))
    print(f"wrote {target} ({offset} bytes)")


if __name__ == "__main__":
    main()
