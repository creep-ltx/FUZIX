#!/usr/bin/env python3
"""Create a PTY, symlink its slave end at argv[1] for FS-UAE's serial_port,
and append everything received on the master end to the log file argv[2].

Optional argv[3]: a keystroke script driven while logging. Lines:
    sleep <seconds>
    send <text>        (\\r \\n \\t escapes recognized)

The tap holds its own slave fd open so the master never enters the EIO
"no slave" state between emulator sessions."""
import os
import pty
import select
import sys
import termios
import threading
import time
import tty

link, log = sys.argv[1], sys.argv[2]
script = sys.argv[3] if len(sys.argv) > 3 else None
master, slave = pty.openpty()
tty.setraw(slave, termios.TCSANOW)      # no echo/canonical processing
os.open(os.ttyname(slave), os.O_RDONLY | os.O_NOCTTY)  # keepalive slave fd
try:
    os.unlink(link)
except FileNotFoundError:
    pass
os.symlink(os.ttyname(slave), link)
print(f"serial tap: {link} -> {os.ttyname(slave)}, logging to {log}", flush=True)


def run_script(path):
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("sleep "):
            time.sleep(float(line[6:]))
        elif line.startswith("send "):
            text = line[5:].encode().decode("unicode_escape")
            for ch in text.encode("latin1"):    # trickle like a human types
                os.write(master, bytes([ch]))
                time.sleep(0.02)


if script:
    threading.Thread(target=run_script, args=(script,), daemon=True).start()

with open(log, "wb", 0) as f:
    while True:
        r, _, _ = select.select([master], [], [], 1)
        if master in r:
            try:
                data = os.read(master, 4096)
            except OSError:
                continue
            if not data:
                continue
            f.write(data)
