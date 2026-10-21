#!/usr/bin/env python3
"""Make Amiga keymaps for Fuzix from the X keyboard layouts.

  mkxkbkeymap.py [--pkg PKGFILE] OUTDIR

The layouts come from xkeyboard-config (MIT/X11 licence, its notice goes
with the keymaps as COPYING.xkb), read through libxkbcommon. AmigaOS's
own keymap files are Commodore/Hyperion's and may not be shipped; these
are free stand-ins in the same AKM1 format (see mkkeymap.py), named as
on AmigaOS. Amiga national keyboards follow their country's typewriter
layout like PC ones, with three differences applied here:

  - the key top left (raw 0x00) is ` and ~
  - the extra key next to Backspace (raw 0x0D) is \\ and |
  - Alt is AmigaOS's own Alt layer, the same on every national keyboard:
    Latin-1 symbols on the number and letter keys (Alt+A ae, Alt+O o-slash,
    Alt+S sharp s, Alt+1 superscript 1...), the five accents as dead keys
    on Alt+F G H J K; on the other keys the US character ([ ] { } ; ...)
  - a few keys where an Amiga keyboard's caps differ from the PC layout
    (Swedish Shift+3 is the pound sign, Shift+4 the dollar sign)

Dead keys (accents) compose with the next letter by Unicode's rules,
Latin-1 results only. Layouts whose letters are not Latin-1 (Polish,
Turkish, Greek, Russian) are left out: the console fonts are Latin-1.
"""
import ctypes
import os
import struct
import sys
import unicodedata

# AmigaOS name, XKB layout, variant, description
LAYOUTS = [
    ("usa2", "us", "dvorak", "USA Dvorak"),
    ("gb", "gb", "", "British"),
    ("d", "de", "", "German"),
    ("f", "fr", "", "French"),
    ("i", "it", "", "Italian"),
    ("e", "es", "", "Spanish"),
    ("s", "se", "", "Swedish and Finnish"),
    ("dk", "dk", "", "Danish"),
    ("n", "no", "", "Norwegian"),
    ("cdn", "ca", "fr-legacy", "Canadian French"),
    ("ch1", "ch", "fr", "Swiss (French)"),
    ("ch2", "ch", "", "Swiss (German)"),
    ("po", "pt", "", "Portuguese"),
    ("la", "latam", "", "Latin American"),
]

# Amiga raw key code -> Linux input key code (xkb keycode = this + 8)
RAW = {0x00: 41, 0x0B: 12, 0x0C: 13, 0x1A: 26, 0x1B: 27, 0x29: 39,
       0x2A: 40, 0x2B: 43, 0x30: 86, 0x38: 51, 0x39: 52, 0x3A: 53}
RAW.update({0x01 + i: 2 + i for i in range(10)})        # 1-0
RAW.update({0x10 + i: 16 + i for i in range(10)})       # q-p
RAW.update({0x20 + i: 30 + i for i in range(9)})        # a-l
RAW.update({0x31 + i: 44 + i for i in range(7)})        # z-m
KEYPAD = {0x0F: "0", 0x1D: "1", 0x1E: "2", 0x1F: "3", 0x2D: "4",
          0x2E: "5", 0x2F: "6", 0x3C: ".", 0x3D: "7", 0x3E: "8", 0x3F: "9"}

# AmigaOS's Alt layer (Alt, Shift+Alt) on the number and letter keys
ALT = {
    0x01: "\u00b9!", 0x02: "\u00b2@", 0x03: "\u00b3#", 0x04: "\u00a2$", 0x05: "\u00bc%",
    0x06: "\u00bd^", 0x07: "\u00be&", 0x08: "\u00b7*", 0x09: "\u00ab(", 0x0A: "\u00bb)",
    0x10: "\u00e5\u00c5", 0x11: "\u00b0\u00b0", 0x12: "\u00a9\u00a9", 0x13: "\u00ae\u00ae",
    0x14: "\u00fe\u00de", 0x15: "\u00a4\u00a5", 0x16: "\u00b5\u00b5", 0x17: "\u00a1\u00a6",
    0x18: "\u00f8\u00d8", 0x19: "\u00b6\u00b6", 0x20: "\u00e6\u00c6", 0x21: "\u00df\u00a7",
    0x22: "\u00f0\u00d0", 0x28: "\u00a3\u00a3", 0x30: "\\|", 0x31: "\u00b1\u00ac",
    0x32: "\u00d7\u00f7", 0x33: "\u00e7\u00c7", 0x34: "\u00aa\u00aa", 0x35: "\u00ba\u00ba",
    0x36: "\u00ad\u00af", 0x37: "\u00b8\u00bf",
}
ALT_DEAD = {0x23: "dead_acute", 0x24: "dead_grave", 0x25: "dead_circumflex",
            0x26: "dead_tilde", 0x27: "dead_diaeresis"}
# Where an Amiga national keyboard differs from the PC layout: raw ->
# (level, character); level 0 plain, 1 Shift
AMIGA = {
    "se": {0x03: (1, "\u00a3"), 0x04: (1, "$")},
}

DEAD = {  # keysym name -> (combining mark, spacing accent)
    "dead_acute": ("́", "´"), "dead_grave": ("̀", "`"),
    "dead_circumflex": ("̂", "^"), "dead_tilde": ("̃", "~"),
    "dead_diaeresis": ("̈", "¨"), "dead_abovering": ("̊", "°"),
    "dead_cedilla": ("̧", "¸"),
}
KEYS = 0x68
ENTRIES = 24 + KEYS * 16


class RuleNames(ctypes.Structure):
    _fields_ = [(n, ctypes.c_char_p) for n in ("rules", "model", "layout", "variant", "options")]


xkb = ctypes.CDLL("libxkbcommon.so.0")
xkb.xkb_context_new.restype = ctypes.c_void_p
xkb.xkb_keymap_new_from_names.restype = ctypes.c_void_p
xkb.xkb_keymap_new_from_names.argtypes = [ctypes.c_void_p, ctypes.POINTER(RuleNames), ctypes.c_int]
xkb.xkb_keymap_key_get_syms_by_level.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                                 ctypes.c_uint32, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32))]
xkb.xkb_keymap_num_levels_for_key.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
xkb.xkb_keysym_to_utf32.argtypes = [ctypes.c_uint32]
xkb.xkb_keysym_to_utf32.restype = ctypes.c_uint32
xkb.xkb_keysym_get_name.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
ctx = xkb.xkb_context_new(0)


def keymap(layout, variant):
    rn = RuleNames(b"evdev", b"pc105", layout.encode(), variant.encode(), b"")
    km = xkb.xkb_keymap_new_from_names(ctx, ctypes.byref(rn), 0)
    if not km:
        raise ValueError(f"no XKB layout {layout}({variant})")
    return km


def sym(km, code, level):
    """(character or None, dead key name or None) for a key and level"""
    if level >= xkb.xkb_keymap_num_levels_for_key(km, code + 8, 0):
        return None, None
    syms = ctypes.POINTER(ctypes.c_uint32)()
    if xkb.xkb_keymap_key_get_syms_by_level(km, code + 8, 0, level, ctypes.byref(syms)) < 1:
        return None, None
    s = syms[0]
    name = ctypes.create_string_buffer(64)
    xkb.xkb_keysym_get_name(s, name, 64)
    n = name.value.decode()
    if n in DEAD:
        return None, n
    u = xkb.xkb_keysym_to_utf32(s)
    return (chr(u) if 0 < u < 0x100 else None), None


def build(layout, variant, us):
    km = keymap(layout, variant)
    dead_names = []
    keys = {}                      # raw -> [(char, dead name)] * 4
    for raw, code in RAW.items():
        if raw == 0x00:
            keys[raw] = [("`", None), ("~", None)]
        else:
            keys[raw] = [sym(km, code, 0), sym(km, code, 1)]
        if raw in AMIGA.get(layout, {}):
            lvl, ch = AMIGA[layout][raw]
            keys[raw][lvl] = (ch, None)
        # Alt: AmigaOS's Alt layer, else the US character
        if raw in ALT:
            keys[raw] += [(ALT[raw][0], None), (ALT[raw][1], None)]
        elif raw in ALT_DEAD:
            keys[raw] += [(None, ALT_DEAD[raw])] * 2
        else:
            keys[raw] += [sym(us, code, 0), sym(us, code, 1)]
        for c, d in keys[raw]:
            if d and d not in dead_names:
                dead_names.append(d)
    keys[0x0D] = [("\\", None), ("|", None), ("\\", None), ("|", None)]
    for raw, ch in KEYPAD.items():
        keys[raw] = [(ch, None)] * 4

    ndead = len(dead_names)
    caps = bytearray(13)
    ent = bytearray(KEYS * 16)
    tables = bytearray()
    for raw, levels in keys.items():
        base = levels[0][0]
        if base and base.isalpha() and base.upper() != base:
            caps[raw >> 3] |= 1 << (raw & 7)
        for w, (c, d) in enumerate(levels):
            e = 16 * raw + 4 * w
            if d:
                ent[e + 1] = dead_names.index(d) + 1
                continue
            if not c:
                continue
            ent[e] = ord(c)
            # A letter an accent can go on: its table after each dead key
            tab = [ord(c)]
            useful = False
            for dn in dead_names:
                mark, spacing = DEAD[dn]
                x = spacing if c == " " else unicodedata.normalize("NFC", c + mark)
                if len(x) == 1 and ord(x) < 0x100:
                    tab.append(ord(x))
                    useful = True
                else:
                    tab.append(ord(c))
            if useful and ndead:
                struct.pack_into(">H", ent, e + 2, ENTRIES + len(tables))
                tables += bytes(tab)
    # Space completes an accent on its own: it is not in RAW, add it
    if ndead:
        tab = [0x20] + [ord(DEAD[dn][1]) for dn in dead_names]
        e = 16 * 0x40
        for w in range(4):
            ent[e + 4 * w] = 0x20
            struct.pack_into(">H", ent, e + 4 * w + 2, ENTRIES + len(tables))
        tables += bytes(tab)
    blob = b"AKM1" + bytes([ndead, 0, 0, 0]) + bytes(caps) + bytes(3) + bytes(ent) + bytes(tables)
    if len(blob) > 4096:
        raise ValueError(f"{layout}: {len(blob)} bytes")
    return blob


def main() -> None:
    args = sys.argv[1:]
    pkg = None
    if args[:1] == ["--pkg"]:
        pkg, args = args[1], args[2:]
    out = args[0]
    os.makedirs(out, exist_ok=True)
    us = keymap("us", "")
    done = []
    for name, layout, variant, desc in LAYOUTS:
        try:
            blob = build(layout, variant, us)
        except Exception as e:
            print(f"{name}: skipped ({e})")
            continue
        open(os.path.join(out, name), "wb").write(blob)
        done.append(name)
        print(f"{name}: {desc}, XKB {layout}{'(' + variant + ')' if variant else ''}, "
              f"{len(blob)} bytes, dead keys {blob[4]}")
    here = os.path.dirname(os.path.abspath(__file__))
    lic = os.path.join(here, "COPYING.xkb")
    if pkg:
        with open(pkg, "w") as f:
            f.write("# Generated by mkxkbkeymap.py: Amiga keymaps from the X layouts\n")
            f.write("package  amiga-keymaps\n\n")
            f.write("d 0755 /usr/lib/keymaps\n")
            rel = os.path.relpath(lic, os.path.dirname(os.path.abspath(pkg)))
            f.write(f"f 0644 /usr/lib/keymaps/COPYING.xkb\t{rel}\n")
            for name in sorted(done):
                f.write(f"f 0644 /usr/lib/keymaps/{name}\t{os.path.join(out, name)}\n")


if __name__ == "__main__":
    main()
