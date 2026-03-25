#!/usr/bin/env python3
"""Convert an ICO file (32bpp BGRA) to a C header with pre-decoded uint32_t pixels.

Usage: ico2h.py <input.ico> <output.h> <array_name>

Emits:  static const int <name>_w, <name>_h;
        static const uint32_t <name>_pixels[w*h];
Pixel format: 0xAARRGGBB (top-to-bottom, left-to-right).
"""
import struct, sys
from pathlib import Path


def decode_ico(data):
    """Return (width, height, pixels_argb[]) from the first 32bpp entry."""
    if len(data) < 6:
        raise ValueError("ICO too short")
    reserved, ico_type, count = struct.unpack_from("<HHH", data, 0)
    if ico_type != 1 or count < 1:
        raise ValueError(f"Not a valid ICO (type={ico_type}, count={count})")

    # Pick first 32bpp entry (or just the first entry)
    best = None
    for i in range(count):
        off = 6 + i * 16
        w = data[off] or 256
        h = data[off + 1] or 256
        bpp = struct.unpack_from("<H", data, off + 6)[0]
        size = struct.unpack_from("<I", data, off + 8)[0]
        img_off = struct.unpack_from("<I", data, off + 12)[0]
        if best is None or bpp > best[2]:
            best = (w, h, bpp, size, img_off)

    w, h, bpp, size, img_off = best
    if bpp != 32:
        raise ValueError(f"Only 32bpp ICO supported (got {bpp}bpp)")

    # Parse BMP DIB header
    dib_size = struct.unpack_from("<I", data, img_off)[0]
    bmp_w = struct.unpack_from("<i", data, img_off + 4)[0]
    bmp_h = struct.unpack_from("<i", data, img_off + 8)[0]
    # bmp_h is double (includes AND mask rows)
    pixel_h = abs(bmp_h) // 2 if abs(bmp_h) == w * 2 else abs(bmp_h)
    if pixel_h != h:
        pixel_h = h  # fallback

    pix_off = img_off + dib_size
    stride = w * 4  # 32bpp, no padding needed for 32px
    pixels = []
    # BMP is bottom-to-top, so read from last row to first
    for row in range(h - 1, -1, -1):
        row_off = pix_off + row * stride
        for col in range(w):
            p = row_off + col * 4
            if p + 4 > len(data):
                pixels.append(0)
                continue
            b, g, r, a = data[p], data[p + 1], data[p + 2], data[p + 3]
            pixels.append((a << 24) | (r << 16) | (g << 8) | b)

    return w, h, pixels


def main():
    if len(sys.argv) != 4:
        print("Usage: ico2h.py <input.ico> <output.h> <array_name>",
              file=sys.stderr)
        sys.exit(1)

    in_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    name = sys.argv[3]

    raw = in_path.read_bytes()
    w, h, pixels = decode_ico(raw)

    guard = f"EMBEDDED_ICON_{name.upper()}_H"
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        f"#include <stdint.h>",
        "",
        f"/* Auto-generated from: {in_path.name} ({w}x{h}, 32bpp ARGB) */",
        f"#define {name.upper()}_W {w}",
        f"#define {name.upper()}_H {h}",
        f"static const uint32_t {name}_pixels[{w * h}] = {{",
    ]
    # 8 pixels per line
    for i in range(0, len(pixels), 8):
        chunk = pixels[i:i + 8]
        hex_str = ",".join(f"0x{p:08X}" for p in chunk)
        lines.append(f"    {hex_str},")
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"OK: {in_path} -> {out_path} ({w}x{h}, {len(pixels)} pixels)")


if __name__ == "__main__":
    main()
