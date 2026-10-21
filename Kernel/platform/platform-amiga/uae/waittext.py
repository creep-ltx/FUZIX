#!/usr/bin/env python3
"""Wait until TEXT is on the emulator's screen (OCR with tesseract).

  waittext.py WAYLAND_DISPLAY TIMEOUT TEXT...

Takes a grim screenshot of cage's display about once a second and reads
the whole screen, case and spaces ignored. Tesseract needs English data:
TESSDATA_PREFIX, else ~/.local/share/tessdata if it has eng.traineddata. Exit 0 when found,
1 on timeout. Used by run.sh for "wait" lines in KEYS scripts.
"""
import os
import re
import subprocess
import sys
import tempfile
import time

disp, timeout, text = sys.argv[1], float(sys.argv[2]), " ".join(sys.argv[3:])


def fuzzy_in(want, seen):
    """OCR misreads a letter now and then (time -> tine, l -> 1): accept
    a stretch of the screen that matches all but a couple of letters."""
    n = len(want)
    if n < 10:
        return False            # short texts: exact only (false hits)
    allowed = 1 if n < 16 else 2
    # whole stretches of n letters only: zip() on a shorter one (an empty
    # screen) would count as a perfect match
    for i in range(0, len(seen) - n + 1):
        if sum(a != b for a, b in zip(want, seen[i:i + n])) <= allowed:
            return True
    return False

want = re.sub(r"\s+", "", text.lower())
env = dict(os.environ, WAYLAND_DISPLAY=disp)
local = os.path.expanduser("~/.local/share/tessdata")
if "TESSDATA_PREFIX" not in env and os.path.exists(os.path.join(local, "eng.traineddata")):
    env["TESSDATA_PREFIX"] = local
end = time.time() + timeout
with tempfile.TemporaryDirectory() as d:
    png = os.path.join(d, "s.png")
    while time.time() < end:
        if subprocess.run(["grim", png], env=env, capture_output=True).returncode == 0:
            r = subprocess.run(["tesseract", png, "-", "--psm", "6"],
                               capture_output=True, text=True, env=env)
            seen = re.sub(r"\s+", "", r.stdout.lower())
            if want in seen or fuzzy_in(want, seen):
                sys.exit(0)
        time.sleep(1)
sys.exit(1)
