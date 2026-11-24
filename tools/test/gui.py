#!/usr/bin/env python3
"""Every dialog of the game's, through its GUI: each is made, laid out and drawn.

    python3 tools/test/gui.py build-dlls/jpog GAME [--keep DIR] [--locales]

Headless. A dialog passes when the host exits cleanly and, unless it is one of
the few with nothing of its own to show, draws something. `--locales` draws
three in each language as well.
"""
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

BACKGROUND = (0x28, 0x30, 0x20)              # gui.c's, as a BMP has it
EMPTY = {"NextMenuTemplate", "HistoryMenuDialog", "CurrentPopupMenu"}    # filled in by SimJP.exe as they are used
KNOWN = {"LoadSaveStorage": "an Xbox's: its pictures (frontxbox) are not on the PC disc, and the sprite shader faults on none"}
LANGUAGES = ["English", "UKEnglish", "German", "Italian", "Spanish", "French"]


def drawn(path):
    d = open(path, "rb").read()
    at = int.from_bytes(d[10:14], "little")
    bpp = int.from_bytes(d[28:30], "little") // 8
    n = 0
    for i in range(at, len(d) - bpp, bpp * 7):
        n += tuple(d[i:i + 3]) != BACKGROUND
    return n


def one(job):
    jpog, game, name, locale, shot = job
    r = subprocess.run([jpog, "--game", game, "--gui", name, "--locale", locale, "--headless", "--frames", "8", "--shot", shot],
                       capture_output=True, text=True, errors="replace", timeout=120, env=dict(os.environ, JPOG_GUI_MOUSE="639,479"))
    n = drawn(shot) if os.path.exists(shot) else 0
    return name, locale, r, n


def main():
    jpog, game = sys.argv[1], sys.argv[2]
    keep = os.path.abspath(sys.argv[sys.argv.index("--keep") + 1]) if "--keep" in sys.argv else None
    text = open(os.path.join(game, "Data/GUI/main.gui"), encoding="latin-1").read()
    names = [m[1] for b in re.split(r"(?m)^\[", text) if b.startswith("dialog]") and (m := re.search(r"(?m)^name = (\w+);", b))]
    names = list(dict.fromkeys(names))
    bad = []
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(6) as pool:
        out = keep or tmp
        jobs = [(jpog, game, n, "English", os.path.join(out, n + ".bmp")) for n in names]
        if "--locales" in sys.argv:
            jobs += [(jpog, game, n, l, os.path.join(out, n + "_" + l + ".bmp")) for l in LANGUAGES
                     for n in ("AParkFinanceDialog", "AWindowsVideoOptionsDialog", "FrontEndOptions")]
        for name, locale, r, n in pool.map(one, jobs):
            if name not in KNOWN and (r.returncode or (n < 60 and name not in EMPTY)):
                bad.append(name)
                print("%-34s %-9s exit %d, %d drawn: %s" % (name, locale, r.returncode, n, (r.stderr.strip().splitlines() or [""])[-1][:90]))
                for l in r.stderr.splitlines():
                    if "fault" in l or "in lifted" in l:
                        print("    " + l.strip())
    print("%d dialogs (%d drawings), %d failed, %d known not to" % (len(names), len(jobs), len(bad), len(KNOWN)))
    return bool(bad)


if __name__ == "__main__":
    sys.exit(main())
