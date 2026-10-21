#!/usr/bin/env python3
"""Build a bootable Amiga floppy image (ADF) holding the Fuzix kernel.

  mkadf.py bootblock.bin fuzix.bin out.adf [filesys.img]

Layout: sectors 0-1 = bootblock ('DOS\\0', checksum, kernel size, code),
the kernel from byte 1024. Bootblock offset 8 carries the 512-rounded
kernel size, read by the bootblock code at boot time.

With a file system image, it goes at sector FS_START and sector 0 also
gets an MBR partition table (bytes 446-511, past the boot code) with one
Fuzix partition (type 0x7E) covering it. The kernel then takes its root
file system from the floppy.
"""
import struct
import sys

ADF_SIZE = 901120          # 80 cyl * 2 heads * 11 sectors * 512 bytes
SECTORS = ADF_SIZE // 512
BB_SIZE = 1024
HDR = 12                   # 'DOS\0' + checksum + kernel size
FS_START = 200             # first sector of the file system
MBR_TABLE = 446


def carry_sum(data: bytes) -> int:
    s = 0
    for (v,) in struct.iter_unpack(">I", data):
        s += v
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s


def main() -> None:
    bootcode_path, kernel_path, out_path = sys.argv[1:4]
    fs_path = sys.argv[4] if len(sys.argv) > 4 else None
    bootcode = open(bootcode_path, "rb").read()
    kernel = open(kernel_path, "rb").read()

    if len(bootcode) > MBR_TABLE - HDR:
        sys.exit(f"bootblock code too big: {len(bootcode)} > {MBR_TABLE - HDR}")

    ksize = (len(kernel) + 511) & ~511
    limit = FS_START * 512 if fs_path else ADF_SIZE
    if BB_SIZE + ksize > limit:
        sys.exit(f"kernel too big: {ksize} bytes, room for {limit - BB_SIZE}")

    bb = bytearray(BB_SIZE)
    bb[0:4] = b"DOS\0"
    bb[8:12] = struct.pack(">I", ksize)
    bb[HDR:HDR + len(bootcode)] = bootcode

    adf = bytearray(ADF_SIZE)
    if fs_path:
        fs = open(fs_path, "rb").read()
        blocks = SECTORS - FS_START
        if len(fs) > blocks * 512:
            sys.exit(f"file system too big: {len(fs)} bytes, room for {blocks * 512}")
        # Partition 1: not active, type 0x7E (Fuzix), LBA start and count
        entry = bytes([0, 0, 0, 0, 0x7E, 0, 0, 0]) + struct.pack("<II", FS_START, blocks)
        bb[MBR_TABLE:MBR_TABLE + 16] = entry
        bb[510:512] = b"\x55\xAA"
        adf[FS_START * 512:FS_START * 512 + len(fs)] = fs

    # checksum: full 1024-byte carry-wrapping sum must equal 0xFFFFFFFF
    bb[4:8] = struct.pack(">I", 0xFFFFFFFF - carry_sum(bytes(bb)))
    assert carry_sum(bytes(bb)) == 0xFFFFFFFF

    adf[0:BB_SIZE] = bb
    adf[BB_SIZE:BB_SIZE + len(kernel)] = kernel
    open(out_path, "wb").write(adf)
    print(f"{out_path}: bootblock {len(bootcode) + HDR}B, "
          f"kernel {len(kernel)}B ({ksize}B on disk)"
          + (f", file system {SECTORS - FS_START} blocks" if fs_path else ""))


if __name__ == "__main__":
    main()
