#!/usr/bin/env python3
"""Every weather, over Isla Nublar and two made islands, from a few places: each draws.

    python3 tools/test/terrain.py build-dlls/jpog GAME [--keep DIR]

Headless. A view passes when the host exits cleanly, some sections are in
view, and the picture is not one colour. (`--keep` keeps the pictures.)
"""
import os
import re
import subprocess
import sys
import tempfile

WEATHERS = ["normal", "heatwave", "lightrain", "heavyrain", "tornado"]
PLACES = {"shore": "290,170,6,1.2,-0.1", "inland": "504,504,10,0.6,-0.1", "sea": "130,500,10,-0.8,-0.15", "edge": "2,2,10,0.8,-0.1",
          "far edge": "1005,1005,10,-2.4,-0.1", "high": "504,400,60,0,-0.4", "low": "504,504,1.7,2.5,0"}


def colours(path):
    d = open(path, "rb").read()
    at = int.from_bytes(d[10:14], "little")
    bpp = int.from_bytes(d[28:30], "little") // 8
    return len({d[i:i + bpp] for i in range(at, len(d) - bpp, bpp * 101)})


def main():
    jpog, game = sys.argv[1], sys.argv[2]
    keep = sys.argv[sys.argv.index("--keep") + 1] if "--keep" in sys.argv else None
    views = [("nublar", w, p) for w in WEATHERS for p in PLACES]
    views += [(seed, "normal", p) for seed in ("16031971", "7") for p in PLACES]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for island, weather, place in views:
            shot = os.path.join(os.path.abspath(keep or tmp), "%s_%s_%s.bmp" % (island, weather, place.replace(" ", "")))
            env = dict(os.environ, JPOG_WEATHER=weather, JPOG_TERRAIN_AT=PLACES[place])
            r = subprocess.run([jpog, "--game", game, "--terrain", island, "--headless", "--frames", "40", "--shot", shot],
                               env=env, capture_output=True, text=True, errors="replace", timeout=120)
            m = re.search(r"(\d+) sections in view", r.stderr)
            n = colours(shot) if os.path.exists(shot) else 0
            ok = r.returncode == 0 and m and int(m[1]) > 0 and n > 50
            bad += not ok
            if not ok:
                print("%-10s %-10s %-9s FAILED: exit %d, sections %s, %d colours" % (island, weather, place, r.returncode, m and m[1], n))
                print("   " + "\n   ".join(r.stderr.strip().splitlines()[-4:]))
    print("%d views, %d failed" % (len(views), bad))
    return bad != 0


if __name__ == "__main__":
    sys.exit(main())
