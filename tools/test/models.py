"""Every model of the game through the engine, headless: that it loads, draws
something, and nothing faults.

    python3 tools/test/models.py build-dlls/jpog GAME [OUT]

Each is drawn by `jpog --view` (src/host/engine.c) for a few frames, from as
far as its bounding sphere says, and its last frame compared with the
background. OUT (default work/models) keeps the pictures.
"""
import concurrent.futures
import os
import struct
import subprocess
import sys

# What is not expected to draw, and why; anything else that does not is a failure.
KNOWN = {
    'HuntPlat.tmd': 'an older skeleton layout: the original code faults on it too (run under Unicorn)',
    'STurret.tmd': 'the same',
    'VentP90.tmd': 'its level-of-detail table does not read as the others do (not looked into)',
    'VentP90_Lo.tmd': 'the same',
    'VentP90_hi.tmd': 'the same',
    'Carcha.tmd': 'a mesh of 28 bones: the engine uploads the last over its own fade constant, c88',
    'GateS90_hi.tmd': 'thin wires, seen only with its materials (Fences_Hi.tml)',
    'Tornado.tmd': 'no mesh to see (not looked into)',
}
BACKGROUND = (0x90, 0x70, 0x50)              # engine.c's viewport colour, as a BMP has it: BGR


def drawn(path):
    """How many pixels are not the background (every other one looked at)."""
    d = open(path, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    row = (w * 3 + 3) // 4 * 4
    bg = bytes(BACKGROUND)
    return sum(d[54 + y * row + 3 * x:57 + y * row + 3 * x] != bg for y in range(0, h, 2) for x in range(0, w, 2))


def one(jpog, game, out, name):
    shot = os.path.join(out, name[:-4] + '.bmp')
    if os.path.exists(shot):
        os.remove(shot)
    env = dict(os.environ, JPOG_VIEW_ZOOM='1.2')
    try:
        p = subprocess.run([jpog, '--game', game, '--view', 'Data/Models/' + name, '--headless', '--frames', '4',
                            '--shot', shot], capture_output=True, env=env, timeout=120)
    except subprocess.TimeoutExpired:
        return name, 'did not end'
    log = p.stderr.decode(errors='replace')
    if p.returncode or '===' in log:
        why = [x for x in log.splitlines() if '===' in x or 'did not load' in x or 'HLE' in x]
        return name, f'exit {p.returncode}: {(why or [log[-120:]])[0].strip()}'
    if not os.path.exists(shot):
        return name, 'no picture'
    n = drawn(shot)
    return name, None if n else 'nothing drawn'


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    jpog, game = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    out = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else 'work/models')
    os.makedirs(out, exist_ok=True)
    names = sorted(n for n in os.listdir(os.path.join(game, 'Data', 'Models')) if n.lower().endswith('.tmd'))
    with concurrent.futures.ThreadPoolExecutor(6) as ex:
        bad = [r for r in ex.map(lambda n: one(jpog, game, out, n), names) if r[1]]
    new = [(n, w) for n, w in bad if n not in KNOWN]
    for name, why in new:
        print(f'{name}: {why}')
    print(f'{len(names) - len(bad)} of {len(names)} models drawn; {len(bad) - len(new)} known not to')
    sys.exit(1 if new else 0)


if __name__ == '__main__':
    main()
