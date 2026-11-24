#!/usr/bin/env python3
"""The park demo: every dinosaur, every weather, the HUD's icons, a minute of it.

    python3 tools/test/park.py build-dlls/jpog GAME [--keep DIR]

Headless. A run passes when the host exits cleanly, the herd walks and the
trees stand, the picture is not one colour, something is heard, an icon's press
is taken (and the ground's is not), and a minute takes no more memory.
"""
import os
import re
import subprocess
import sys
import tempfile
import time

from terrain import WEATHERS, colours

ICONS = ["31,100", "92,104", "137,87", "190,63", "230,31"]


def main():
    jpog, game = sys.argv[1], sys.argv[2]
    keep = sys.argv[sys.argv.index("--keep") + 1] if "--keep" in sys.argv else None
    runs = [("dinosaur%d" % k, 60, {"JPOG_PARK_FOLLOW": str(k)}, None) for k in range(12)]
    runs += [(w, 60, {"JPOG_WEATHER": w}, None) for w in WEATHERS]
    runs += [("icon" + at, 140, {"JPOG_PARK_PRESS": at}, 1) for at in ICONS]
    runs += [("ground", 60, {"JPOG_PARK_PRESS": "325,245"}, 0), ("keys", 120, {"JPOG_PARK_KEYS": "wd"}, 0)]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, frames, env, presses in runs:
            shot = os.path.join(os.path.abspath(keep or tmp), name.replace(",", "_") + ".bmp")
            r = subprocess.run([jpog, "--game", game, "--park", "--headless", "--frames", str(frames), "--shot", shot],
                               env=dict(os.environ, **env), capture_output=True, text=True, errors="replace", timeout=120)
            why = []
            if r.returncode:
                why.append("exit %d" % r.returncode)
            if "12 dinosaurs, 12 walking; 63 trees and buildings" not in r.stderr:
                why.append("not everything is there")
            if re.search(r"\[(park|gui)\] no ", r.stderr):
                why.append("a file or a dialog is missing")
            m = re.search(r"(\d+) sections.*sound peak ([\d.]+)", r.stderr)
            if not m or not int(m[1]) or float(m[2]) <= 0.005:
                why.append("sections and sound: %s" % (m and m.groups(),))
            n = colours(shot) if os.path.exists(shot) else 0
            if n <= 50:
                why.append("%d colours" % n)
            if presses is not None and r.stderr.count('" pressed') != presses:
                why.append("%d presses" % r.stderr.count('" pressed'))
            bad += bool(why)
            if why:
                print("%-12s FAILED: %s" % (name, "; ".join(why)))
    p = subprocess.Popen([jpog, "--game", game, "--park", "--headless", "--frames", "3600"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rss = []
    try:
        for wait in (20, 35):
            time.sleep(wait)
            rss.append(int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(p.pid)]).split()[0] or 0))
        code = p.wait(timeout=60)
    except Exception as e:
        p.kill()
        code = e
    if code != 0 or rss[1] - rss[0] > 2048:
        bad += 1
        print("a minute     FAILED: exit %s, %s KB" % (code, rss))
    print("%d runs, %d failed; %s KB" % (len(runs) + 1, bad, " then ".join(map(str, rss))))
    return bad != 0


if __name__ == "__main__":
    sys.exit(main())
