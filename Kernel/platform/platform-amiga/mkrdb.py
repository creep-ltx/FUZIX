#!/usr/bin/env python3
"""Make a Fuzix hard disk image that Kickstart boots on its own.

  mkrdb.py disk.img SIZE_MB filesys.img fuzix.bin hdboot.hunk

The layout setup writes on an empty disk:

  block 0   RDSK  geometry: 1 head, 32 blocks a track (16K cylinders);
                  cylinders 0-1 hold the partition table
  block 1   PART  "FUZIX", bootable, DosType FUZ\\0, from cylinder 2
  block 2   FSHD  the "file system" for FUZ\\0: hdboot.hunk, the loader
  block 3+  LSEG  hdboot.hunk, 492 bytes a block
  FUZIX partition: the Fuzix file system, then in the last RESERVED
                  blocks the kernel (a header block "FUZK" + size, then
                  the kernel). At most 65535 blocks: Fuzix reaches only
                  that much of a device (it writes the kernel itself)

Kickstart loads hdboot from the table and starts it as the partition's
handler; it reads the kernel from the reserved blocks and starts it.
"""
import struct
import sys

BLOCK = 512
HOSTID = 7
HEADS, SECS = 1, 32
CYLBLOCKS = HEADS * SECS
RDB_CYLS = 2                       # cylinders 0-1 belong to the RDB
RESERVED = 256                     # kernel blocks at the partition end
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
         NONE, 1, 2, NONE] + [NONE] * 6
    l += [cyls, SECS, HEADS, 1, cyls, 0, 0, 0, cyls, cyls, 3, 0, 0, 0, 0, 0]
    l += [0, RDB_CYLS * CYLBLOCKS - 1, RDB_CYLS, cyls - 1, CYLBLOCKS, 0, 0, 0]
    l += text("FUZIX", 8) + text("FUZIX DISK", 16) + text("0.1", 4)
    return finish(l, 64)


def part(lowcyl, highcyl):
    l = [0x50415254, 0, 0, HOSTID, NONE, 1, 0, 0, 0]
    name = bytes([5]) + b"FUZIX"
    l += list(struct.unpack(">8I", name.ljust(32, b"\0")))
    l += [0] * 15
    env = [16, BLOCK // 4, 0, HEADS, 1, SECS, 0, 0, 0,
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
    img_path, size_mb, fs_path, kernel_path, hunk_path = sys.argv[1:6]
    blocks = int(size_mb) * 2048
    fs = open(fs_path, "rb").read()
    kernel = open(kernel_path, "rb").read()
    hunk = open(hunk_path, "rb").read()
    cyls = blocks // CYLBLOCKS
    fsblocks = (len(fs) + 511) // 512
    if (len(kernel) + 511) // 512 + 1 > RESERVED:
        sys.exit("kernel too big for the reserved blocks")
    lo = RDB_CYLS
    part_blocks = (fsblocks + RESERVED + CYLBLOCKS - 1) // CYLBLOCKS * CYLBLOCKS
    if part_blocks > 65535:
        sys.exit(f"file system too big: {fsblocks} blocks, "
                 f"at most {65535 // CYLBLOCKS * CYLBLOCKS - RESERVED}")
    hi = lo + part_blocks // CYLBLOCKS - 1
    if hi >= cyls:
        sys.exit("disk too small")

    img = bytearray(blocks * BLOCK)
    tables = [rdsk(cyls), part(lo, hi), fshd(3)] + lsegs(hunk, 3)
    if len(tables) > RDB_CYLS * CYLBLOCKS:
        sys.exit("loader too big for the partition table area")
    for i, b in enumerate(tables):
        img[i * BLOCK:(i + 1) * BLOCK] = b
    first = lo * CYLBLOCKS
    img[first * BLOCK:first * BLOCK + len(fs)] = fs
    k = first + part_blocks - RESERVED
    header = struct.pack(">4sI", b"FUZK", len(kernel)).ljust(BLOCK, b"\0")
    img[k * BLOCK:(k + 1) * BLOCK] = header
    img[(k + 1) * BLOCK:(k + 1) * BLOCK + len(kernel)] = kernel
    open(img_path, "wb").write(img)
    print(f"{img_path}: {size_mb} MB, FUZIX partition cylinders {lo}-{hi} "
          f"(kernel {len(kernel)} bytes, file system {fsblocks} blocks), "
          f"loader {len(hunk)} bytes in {len(tables) - 3} LSEG blocks, "
          f"{cyls - hi - 1} cylinders free")


if __name__ == "__main__":
    main()
