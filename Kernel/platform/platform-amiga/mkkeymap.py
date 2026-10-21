#!/usr/bin/env python3
"""Convert AmigaOS keymaps (DEVS:Keymaps/<name>, hunk files) for Fuzix.

  mkkeymap.py [--pkg PKGFILE] OUTDIR KEYMAPFILE...

Writes OUTDIR/<name> in the kernel's AKM1 format (loaded with the keymap
command, KBSETTRANS ioctl):

  0     "AKM1"
  4     ndead: number of dead keys (u8), 3 pad bytes
  8     caps: 13 bytes, bit (k & 7) of byte k >> 3 = key k obeys Caps Lock
  21    3 pad bytes
  24    104 keys (raw codes 0x00-0x67) x 4 qualifier sets (none, Shift,
        Alt, Shift+Alt) x 4 bytes: character, dead key number (1-15, 0 =
        not a dead key), offset of a dead-key table (u16, 0 = none)
  1688  dead-key tables: ndead + 1 bytes each, the character without a
        dead key first, then after dead key 1, 2...

AmigaOS keymap layout (RKM Devices, "Keymap"): a KeyMapNode (Node, then
eight pointers: LoKeyMapTypes, LoKeyMap, LoCapsable, LoRepeatable,
HiKeyMapTypes, HiKeyMap, HiCapsable, HiRepeatable). Lo = keys 0x00-0x3F,
Hi = 0x40-0x77. A key's type: KCF_SHIFT 1, KCF_ALT 2, KCF_CONTROL 4,
KCF_DEAD 0x20, KCF_STRING 0x40, KCF_NOP 0x80. A plain key's ULONG holds,
from the low byte: plain, then one byte per qualifier combination. String
keys of one character count as plain keys (the Russian keymaps are made
of them); longer strings (cursor and function keys) are left to the
kernel's own table.
"""
import os
import struct
import sys

KCF_SHIFT, KCF_ALT, KCF_CONTROL = 1, 2, 4
KCF_DEAD, KCF_STRING, KCF_NOP = 0x20, 0x40, 0x80
DPF_MOD, DPF_DEAD = 0x01, 0x08
KEYS = 0x68
HEADER = 24
ENTRIES = HEADER + KEYS * 4 * 4


def load_hunks(path):
    """Load a hunk file at base 0, hunks one after another; apply RELOC32."""
    d = open(path, "rb").read()
    pos = 0

    def l():
        nonlocal pos
        v = struct.unpack_from(">I", d, pos)[0]
        pos += 4
        return v

    if l() != 0x3F3:
        raise ValueError("not a hunk file")
    while l():                      # resident library names
        pass
    n, first, last = l(), l(), l()
    sizes = [l() & 0x3FFFFFFF for _ in range(last - first + 1)]
    bases, b = [], 0
    for s in sizes:
        bases.append(b)
        b += s * 4
    mem = bytearray(b)
    h = 0
    while pos < len(d) and h < len(sizes):
        t = l() & 0x3FFFFFFF
        if t in (0x3E9, 0x3EA):             # CODE, DATA
            cnt = l()
            mem[bases[h]:bases[h] + cnt * 4] = d[pos:pos + cnt * 4]
            pos += cnt * 4
        elif t == 0x3EB:                    # BSS
            l()
        elif t == 0x3EC:                    # RELOC32
            while True:
                cnt = l()
                if not cnt:
                    break
                target = l()
                for _ in range(cnt):
                    off = bases[h] + l()
                    v = struct.unpack_from(">I", mem, off)[0]
                    struct.pack_into(">I", mem, off, v + bases[target])
        elif t == 0x3F0:                    # SYMBOL
            while True:
                cnt = l()
                if not cnt:
                    break
                pos += cnt * 4 + 4
        elif t == 0x3F1:                    # DEBUG
            pos += l() * 4
        elif t == 0x3F2:                    # END
            h += 1
        else:
            raise ValueError(f"hunk type {t:#x}")
    return bytes(mem)


def u32(m, o):
    return struct.unpack_from(">I", m, o)[0]


def combos(kind):
    """Qualifier sets the key has entries for, in keymap order.
    Each as (shift, alt, ctrl)."""
    flags = [f for f in (KCF_SHIFT, KCF_ALT, KCF_CONTROL) if kind & f]
    out = []
    for i in range(1 << len(flags)):
        q = 0
        for b, f in enumerate(flags):
            if i & (1 << b):
                q |= f
        out.append(q)
    return out


def convert(path):
    m = load_hunks(path)
    name_ptr = u32(m, 10)
    ptrs = [u32(m, 14 + 4 * i) for i in range(8)]
    lo_types, lo_map, lo_caps, _, hi_types, hi_map, hi_caps, _ = ptrs

    def key(k):
        if k < 0x40:
            return m[lo_types + k], u32(m, lo_map + 4 * k), (m[lo_caps + (k >> 3)] >> (k & 7)) & 1
        k -= 0x40
        return m[hi_types + k], u32(m, hi_map + 4 * k), (m[hi_caps + (k >> 3)] >> (k & 7)) & 1

    # First pass: how many dead keys
    ndead = 0
    for k in range(KEYS):
        kind, val, _ = key(k)
        if kind & KCF_NOP or not kind & KCF_DEAD:
            continue
        for i in range(len(combos(kind))):
            flag, v = m[val + 2 * i], m[val + 2 * i + 1]
            if flag & DPF_DEAD:
                ndead = max(ndead, v & 0x0F)

    caps = bytearray(13)
    ent = bytearray(KEYS * 16)
    tables = bytearray()
    # want (shift, alt) sets: none, shift, alt, shift+alt
    want = [0, KCF_SHIFT, KCF_ALT, KCF_SHIFT | KCF_ALT]
    for k in range(KEYS):
        kind, val, capsable = key(k)
        if capsable:
            caps[k >> 3] |= 1 << (k & 7)
        if kind & KCF_NOP:
            continue
        cl = combos(kind)
        for w, q in enumerate(want):
            # The best entry: exactly these qualifiers, else drop the
            # ones this key ignores (a key without Alt types the same)
            qq = q & kind
            if qq not in cl:
                continue
            i = cl.index(qq)
            e = 16 * k + 4 * w
            if kind & KCF_STRING:
                # (length, offset) per qualifier set: one-character
                # strings are ordinary keys (the Russian keymaps); longer
                # ones (cursor and function keys) are left to the kernel
                ln, off = m[val + 2 * i], m[val + 2 * i + 1]
                if ln == 1:
                    ent[e] = m[val + off]
            elif kind & KCF_DEAD:
                flag, v = m[val + 2 * i], m[val + 2 * i + 1]
                if flag & DPF_DEAD:
                    ent[e + 1] = v & 0x0F
                elif flag & DPF_MOD:
                    tab = m[val + v:val + v + ndead + 1]
                    off = ENTRIES + len(tables)
                    tables += tab
                    ent[e] = tab[0]
                    struct.pack_into(">H", ent, e + 2, off)
                else:
                    ent[e] = v
            else:
                # plain: byte i from the low end of the long
                if i < 4:
                    ent[e] = (val >> (8 * i)) & 0xFF
    blob = b"AKM1" + bytes([ndead, 0, 0, 0]) + bytes(caps) + bytes(3) + bytes(ent) + bytes(tables)
    if len(blob) > 4096:
        raise ValueError(f"{path}: {len(blob)} bytes, more than 4096")
    name = m[name_ptr:m.index(b"\0", name_ptr)].decode("latin1")
    return name, blob


def main() -> None:
    args = sys.argv[1:]
    pkg = None
    if args[:1] == ["--pkg"]:
        pkg, args = args[1], args[2:]
    out = args[0]
    os.makedirs(out, exist_ok=True)
    done = []
    for path in args[1:]:
        try:
            name, blob = convert(path)
        except Exception as e:
            print(f"{path}: skipped ({e})")
            continue
        base = os.path.basename(path)
        open(os.path.join(out, base), "wb").write(blob)
        done.append(base)
        print(f"{base}: \"{name}\", {len(blob)} bytes, dead keys {blob[4]}")
    if pkg:
        # A file system package with them (build-filesystem-ng)
        with open(pkg, "w") as f:
            f.write("# Generated by mkkeymap.py: the converted AmigaOS keymaps\n")
            f.write("package  amiga-keymaps\n\n")
            f.write("d 0755 /usr/lib/keymaps\n")
            for base in sorted(done):
                f.write(f"f 0644 /usr/lib/keymaps/{base}\t{os.path.join(out, base)}\n")


if __name__ == "__main__":
    main()
