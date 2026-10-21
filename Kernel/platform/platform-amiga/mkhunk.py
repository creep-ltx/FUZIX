#!/usr/bin/env python3
"""Make the hard disk loader into an AmigaOS load file (one code hunk,
no relocations: hdboot.S is position independent).

  mkhunk.py hdboot.bin out
"""
import struct
import sys

HUNK_HEADER, HUNK_CODE, HUNK_END = 0x3F3, 0x3E9, 0x3F2


def main() -> None:
    code = open(sys.argv[1], "rb").read()
    code += bytes(-len(code) % 4)
    n = len(code) // 4
    out = struct.pack(">IIIIII", HUNK_HEADER, 0, 1, 0, 0, n)
    out += struct.pack(">II", HUNK_CODE, n) + code + struct.pack(">I", HUNK_END)
    open(sys.argv[2], "wb").write(out)
    print(f"{sys.argv[2]}: loader {len(code)} bytes")


if __name__ == "__main__":
    main()
