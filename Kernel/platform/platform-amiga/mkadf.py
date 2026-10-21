#!/usr/bin/env python3
"""Build a bootable Amiga floppy image (ADF) from bootblock code + kernel.

Layout: sectors 0-1 = bootblock ('DOS\\0', checksum, kernel size, code),
kernel image from byte offset 1024. Bootblock offset 8 carries the
512-rounded kernel size, read by the bootblock code at boot time.
"""
import struct
import sys

ADF_SIZE = 901120          # 80 cyl * 2 heads * 11 sectors * 512 bytes
BB_SIZE = 1024
HDR = 12                   # 'DOS\0' + checksum + kernel size


def carry_sum(data: bytes) -> int:
    s = 0
    for (v,) in struct.iter_unpack(">I", data):
        s += v
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s


def main() -> None:
    bootcode_path, kernel_path, out_path = sys.argv[1:4]
    bootcode = open(bootcode_path, "rb").read()
    kernel = open(kernel_path, "rb").read()

    if len(bootcode) > BB_SIZE - HDR:
        sys.exit(f"bootblock code too big: {len(bootcode)} > {BB_SIZE - HDR}")

    ksize = (len(kernel) + 511) & ~511
    if BB_SIZE + ksize > ADF_SIZE:
        sys.exit(f"kernel too big for floppy: {ksize}")

    bb = bytearray(BB_SIZE)
    bb[0:4] = b"DOS\0"
    bb[8:12] = struct.pack(">I", ksize)
    bb[HDR:HDR + len(bootcode)] = bootcode
    # checksum: full 1024-byte carry-wrapping sum must equal 0xFFFFFFFF
    bb[4:8] = struct.pack(">I", 0xFFFFFFFF - carry_sum(bytes(bb)))
    assert carry_sum(bytes(bb)) == 0xFFFFFFFF

    adf = bytearray(ADF_SIZE)
    adf[0:BB_SIZE] = bb
    adf[BB_SIZE:BB_SIZE + len(kernel)] = kernel
    open(out_path, "wb").write(adf)
    print(f"{out_path}: bootblock {len(bootcode) + HDR}B, "
          f"kernel {len(kernel)}B ({ksize}B on disk)")


if __name__ == "__main__":
    main()
