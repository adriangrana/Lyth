#!/usr/bin/env python3
"""Convert a binary file to a C header with a const uint8_t array.

Usage: bin2h.py <input> <output.h> <array_name>
"""
import sys
from pathlib import Path

def main():
    if len(sys.argv) != 4:
        print("Usage: bin2h.py <input> <output.h> <array_name>", file=sys.stderr)
        sys.exit(1)

    in_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    name = sys.argv[3]

    data = in_path.read_bytes()
    guard = f"EMBEDDED_{name.upper()}_H"

    lines = []
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append(f"#include <stdint.h>")
    lines.append(f"#include \"string.h\"")  # OS-native size_t
    lines.append("")
    lines.append(f"/* Auto-generated from: {in_path.name} ({len(data)} bytes) */")
    lines.append(f"static const size_t {name}_size = {len(data)};")
    lines.append(f"static const uint8_t {name}_data[] = {{")

    # Emit 16 bytes per line
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ",".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_str},")

    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"OK: {in_path} -> {out_path} ({len(data)} bytes, array '{name}')")

if __name__ == "__main__":
    main()
