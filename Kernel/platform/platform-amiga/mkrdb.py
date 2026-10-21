#!/usr/bin/env python3
"""Make a Fuzix disk image bootable by Kickstart, with no floppy.

  mkrdb.py disk.img hdboot.hunk

The image keeps its PC partition table (MBR) in block 0, which Fuzix
reads. This adds an Amiga Rigid Disk Block in blocks 1 onwards, which
Kickstart reads (it looks for RDSK in blocks 0-15):

  block 1   RDSK  geometry: 1 head, 32 blocks a track, 32 blocks a
                  cylinder; cylinders 0-63 (blocks 0-2047) hold the RDB
  block 2   PART  "FUZIX", bootable, DosType FUZ\\0, covering the first
                  MBR partition (the Fuzix root file system)
  block 3   FSHD  the "file system" for FUZ\\0: hdboot.hunk
  block 4+  LSEG  hdboot.hunk, 492 bytes a block

Kickstart loads the hunk and starts it as the partition's handler; it
then starts Fuzix. The same layout is written by the installer.
"""
import struct
import sys

BLOCK = 512
HOSTID = 7
HEADS, SECS = 1, 32
CYLBLOCKS = HEADS * SECS
RDB_CYLS = 64                      # cylinders 0-63 belong to the RDB
DOSTYPE = 0x46555A00               # 'FUZ\0'
NONE = 0xFFFFFFFF


def finish(longs, summed):
    """Pack longs into a block with the sum-to-zero checksum in long 2."""
    longs = list(longs) + [0] * (BLOCK // 4 - len(longs))
    longs[1] = summed
    longs[2] = 0
    longs[2] = (-sum(longs[:summed])) & 0xFFFFFFFF
    return struct.pack(">128I", *longs)


def text(s, n):
    return list(struct.unpack(f">{n // 4}I", s.encode().ljust(n)[:n]))


def rdsk(cyls):
    l = [0x5244534B, 0, 0, HOSTID, BLOCK, 0,
         NONE, 2, 3, NONE] + [NONE] * 6
    l += [cyls, SECS, HEADS, 1, cyls, 0, 0, 0, cyls, cyls, 3, 0, 0, 0, 0, 0]
    l += [1, RDB_CYLS * CYLBLOCKS - 1, RDB_CYLS, cyls - 1, CYLBLOCKS, 0, 0, 0]
    l += text("FUZIX", 8) + text("FUZIX DISK", 16) + text("0.1", 4)
    return finish(l, 64)


def part(lowcyl, highcyl):
    l = [0x50415254, 0, 0, HOSTID, NONE, 1, 0, 0, 0]
    name = bytes([5]) + b"FUZIX"
    l += list(struct.unpack(">8I", name.ljust(32, b"\0")))
    l += [0] * 15
    env = [16, BLOCK // 4, 0, HEADS, 1, SECS, 2, 0, 0,
           lowcyl, highcyl, 30, 0, 0x1FE00, 0x7FFFFFFE, 0, DOSTYPE]
    l += env
    return finish(l, 64)


def fshd(first_lseg):
    l = [0x46534844, 0, 0, HOSTID, NONE, 0, 0, 0,
         DOSTYPE, 0x00000001,
         0x190,                 # patch StackSize, SegList, GlobalVec
         0, 0, 0, 0, 4096, 0, 0, first_lseg, NONE]
    return finish(l, 64)


def lsegs(data, first):
    out = []
    chunk = 123 * 4
    n = (len(data) + chunk - 1) // chunk
    for i in range(n):
        # SummedLongs also says how much data the block holds: the last
        # one must not hand the ROM's LoadSeg trailing zeros
        piece = data[i * chunk:(i + 1) * chunk]
        words = len(piece) // 4
        nxt = first + i + 1 if i + 1 < n else NONE
        l = [0x4C534547, 0, 0, HOSTID, nxt] + list(struct.unpack(f">{words}I", piece))
        out.append(finish(l, 5 + words))
    return out


def main() -> None:
    img_path, hunk_path = sys.argv[1:3]
    img = bytearray(open(img_path, "rb").read())
    hunk = open(hunk_path, "rb").read()

    if img[510:512] != b"\x55\xAA":
        sys.exit("no MBR in block 0")
    first, count = struct.unpack("<II", img[446 + 8:446 + 16])
    if first % CYLBLOCKS or count % CYLBLOCKS or first < RDB_CYLS * CYLBLOCKS:
        sys.exit(f"partition 1 ({first}+{count}) must be cylinder aligned "
                 f"and start at or after block {RDB_CYLS * CYLBLOCKS}")
    cyls = len(img) // BLOCK // CYLBLOCKS

    blocks = [rdsk(cyls), part(first // CYLBLOCKS, (first + count) // CYLBLOCKS - 1),
              fshd(4)] + lsegs(hunk, 4)
    if 1 + len(blocks) > RDB_CYLS * CYLBLOCKS:
        sys.exit("loader too big for the RDB area")
    for i, b in enumerate(blocks):
        img[(1 + i) * BLOCK:(2 + i) * BLOCK] = b
    open(img_path, "wb").write(img)
    print(f"{img_path}: RDB in blocks 1-{len(blocks)}, FUZIX partition "
          f"cylinders {first // CYLBLOCKS}-{(first + count) // CYLBLOCKS - 1}, "
          f"loader {len(hunk)} bytes in {len(blocks) - 3} LSEG blocks")


if __name__ == "__main__":
    main()
