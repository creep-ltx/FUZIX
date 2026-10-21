#!/usr/bin/env python3
"""Make the Extras floppies: the rest of Fuzix as tar archives written
straight onto 880K disks (no file system: `tar -x -f /dev/hda` reads them).

  mkextras.py full-ucp.txt floppy-ucp.txt srcdir outdir

The two ucp scripts are what build-filesystem-ng wrote for the full system
and for the install floppy (paths in them are relative to srcdir,
Standalone/filesystem-src). Everything in the full system that is not on
the install floppy goes on the Extras disks, except C headers (there is
no C compiler for Fuzix on the 68000). Each archive starts with an empty
etc/extras.N-of-T, which setup uses to tell the disks apart and which
stays in /etc as a record.
"""
import io
import os
import sys
import tarfile

DISK = 901120
SKIP = ("/usr/include",)


def parse(path, srcdir):
    """Return {dest: (kind, data)} from a ucp script.
    kind: 'dir' (mode), 'file' (src, mode), 'link' (target)."""
    entries = {}
    cwd = "/"
    mode_of = {}
    for line in open(path):
        p = line.split()
        if not p:
            continue
        op = p[0]
        if op == "cd":
            cwd = p[1] if len(p) > 1 else "/"
        elif op == "mkdir" and len(p) > 1:
            d = os.path.normpath(os.path.join(cwd, p[1]))
            entries[d] = ["dir", 0o755]
        elif op == "bget":
            d = os.path.normpath(os.path.join(cwd, p[2]))
            entries[d] = ["file", os.path.join(srcdir, p[1]), 0o644]
        elif op == "chmod" and len(p) > 2:
            d = os.path.normpath(os.path.join(cwd, p[2]))
            mode_of[d] = int(p[1], 8)
        elif op == "ln":
            entries[os.path.normpath(p[2])] = ["link", os.path.normpath(p[1])]
    for d, m in mode_of.items():
        if d in entries and entries[d][0] in ("dir", "file"):
            entries[d][-1] = m & 0o7777
    return entries


def info(name, size=0, mode=0o644, kind=tarfile.REGTYPE):
    t = tarfile.TarInfo(name)
    t.size = size
    t.mode = mode
    t.type = kind
    t.uid = t.gid = 0
    t.uname = t.gname = "root"
    t.mtime = 0
    return t


def main() -> None:
    full_path, floppy_path, srcdir, outdir = sys.argv[1:5]
    full = parse(full_path, srcdir)
    have = parse(floppy_path, srcdir)

    todo, links = [], []
    for dest in sorted(full):
        if dest in have or dest == "/" or dest.startswith(SKIP):
            continue
        e = full[dest]
        if e[0] == "file":
            todo.append((dest, e, os.path.getsize(e[1])))
        elif e[0] == "link":
            links.append((dest, e, 0))
        else:
            todo.append((dest, e, 0))

    # Biggest files first, each on the first disk with room (directories
    # are tiny: they go on disk 1, and any disk makes the drawers it
    # needs). Space: 512 header + data rounded up, two zero blocks at the
    # end, and the extras marker.
    def need(item):
        return 512 + (item[2] + 511) // 512 * 512

    dirs = [i for i in todo if i[1][0] == "dir"]
    files = sorted((i for i in todo if i[1][0] != "dir"), key=lambda i: -i[2])
    disks = [list(dirs)]
    used = [1024 + 512 + 512 * len(dirs)]
    for item in files:
        for n in range(len(disks)):
            if used[n] + need(item) + 512 * 8 <= DISK:   # room for drawers
                break
        else:
            disks.append([])
            used.append(1024 + 512)
            n = len(disks) - 1
        disks[n].append(item)
        used[n] += need(item)
    for d in disks:
        d.sort(key=lambda i: i[0])
    # Hard links last, so every target is already on the hard disk
    disks[-1] += links

    os.makedirs(outdir, exist_ok=True)
    total = len(disks)
    for n, items in enumerate(disks, 1):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            tar.addfile(info(f"etc/extras.{n}-of-{total}"))
            # Parent directories this disk needs that are not on it or on
            # the install floppy (a file whose drawer is on another disk)
            made = set()
            for dest, e, size in items:
                parent = os.path.dirname(dest)
                chain = []
                while parent not in ("/", "") and parent not in have and parent not in made:
                    chain.append(parent)
                    parent = os.path.dirname(parent)
                for d in reversed(chain):
                    if not any(i[0] == d for i in items):
                        tar.addfile(info(d[1:], mode=0o755, kind=tarfile.DIRTYPE))
                    made.add(d)
                name = dest[1:]
                if e[0] == "dir":
                    tar.addfile(info(name, mode=e[1], kind=tarfile.DIRTYPE))
                    made.add(dest)
                elif e[0] == "link":
                    t = info(name, kind=tarfile.LNKTYPE)
                    t.linkname = e[1][1:]
                    tar.addfile(t)
                else:
                    data = open(e[1], "rb").read()
                    tar.addfile(info(name, len(data), e[2]), io.BytesIO(data))
        data = buf.getvalue()
        if len(data) > DISK:
            sys.exit(f"disk {n} too big: {len(data)}")
        out = os.path.join(outdir, f"fuzix-extras{n}.adf")
        open(out, "wb").write(data.ljust(DISK, b"\0"))
        print(f"{out}: {len(items)} files, {len(data) // 1024}K")


if __name__ == "__main__":
    main()
