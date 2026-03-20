#!/usr/bin/env python3
import sys
import struct
from pathlib import Path

PSF1_MAGIC = 0x0436
PSF2_MAGIC = 0x864AB572

def read_u16le(data, offset):
    return struct.unpack_from("<H", data, offset)[0]

def read_u32le(data, offset):
    return struct.unpack_from("<I", data, offset)[0]

def parse_psf(data):
    if len(data) < 4:
        raise ValueError("archivo demasiado pequeño para ser PSF")

    # PSF1
    if read_u16le(data, 0) == PSF1_MAGIC:
        if len(data) < 4:
            raise ValueError("header PSF1 incompleto")

        mode = data[2]
        charsize = data[3]

        glyph_count = 512 if (mode & 0x01) else 256
        width = 8
        height = charsize
        header_size = 4
        bytes_per_glyph = charsize

        expected = header_size + glyph_count * bytes_per_glyph
        if len(data) < expected:
            raise ValueError("archivo PSF1 truncado")

        glyphs = []
        offset = header_size
        for _ in range(glyph_count):
            glyph = list(data[offset:offset + bytes_per_glyph])
            glyphs.append(glyph)
            offset += bytes_per_glyph

        return {
            "version": 1,
            "width": width,
            "height": height,
            "glyph_count": glyph_count,
            "bytes_per_glyph": bytes_per_glyph,
            "glyphs": glyphs,
        }

    # PSF2
    if read_u32le(data, 0) == PSF2_MAGIC:
        if len(data) < 32:
            raise ValueError("header PSF2 incompleto")

        version = read_u32le(data, 4)
        header_size = read_u32le(data, 8)
        flags = read_u32le(data, 12)
        glyph_count = read_u32le(data, 16)
        bytes_per_glyph = read_u32le(data, 20)
        height = read_u32le(data, 24)
        width = read_u32le(data, 28)

        if version != 0:
            raise ValueError(f"version PSF2 no soportada: {version}")

        if width != 8:
            raise ValueError(
                f"solo se soportan fuentes de ancho 8 por ahora; esta fuente tiene ancho {width}"
            )

        expected = header_size + glyph_count * bytes_per_glyph
        if len(data) < expected:
            raise ValueError("archivo PSF2 truncado")

        glyphs = []
        offset = header_size
        for _ in range(glyph_count):
            glyph = list(data[offset:offset + bytes_per_glyph])
            glyphs.append(glyph)
            offset += bytes_per_glyph

        return {
            "version": 2,
            "width": width,
            "height": height,
            "glyph_count": glyph_count,
            "bytes_per_glyph": bytes_per_glyph,
            "glyphs": glyphs,
            "flags": flags,
        }

    raise ValueError("archivo no reconocido como PSF1 ni PSF2")

def emit_header(font, input_name):
    width = font["width"]
    height = font["height"]
    glyph_count = font["glyph_count"]
    glyphs = font["glyphs"]

    out = []
    out.append("#ifndef FONT_PSF_H")
    out.append("#define FONT_PSF_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f"/* Generado automáticamente desde: {input_name} */")
    out.append(f"#define FONT_PSF_WIDTH {width}")
    out.append(f"#define FONT_PSF_HEIGHT {height}")
    out.append(f"#define FONT_PSF_GLYPH_COUNT {glyph_count}")
    out.append("")
    out.append(f"static const uint8_t font_psf_data[FONT_PSF_GLYPH_COUNT][FONT_PSF_HEIGHT] = {{")

    for i, glyph in enumerate(glyphs):
        bytes_hex = ",".join(f"0x{b:02X}" for b in glyph[:height])
        out.append(f"    /* {i:3d} */ {{{bytes_hex}}},")

    out.append("};")
    out.append("")
    out.append("#endif")

    return "\n".join(out) + "\n"

def main():
    if len(sys.argv) != 3:
        print("uso: psf2h.py <entrada.psf> <salida.h>", file=sys.stderr)
        sys.exit(1)

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    data = input_path.read_bytes()
    font = parse_psf(data)
    header = emit_header(font, input_path.name)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(header, encoding="utf-8")

    print(
        f"OK: {input_path} -> {output_path} "
        f"({font['glyph_count']} glifos, {font['width']}x{font['height']})"
    )

if __name__ == "__main__":
    main()