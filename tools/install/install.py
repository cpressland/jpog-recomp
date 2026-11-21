"""From the disc image to the game's folder, in one go.

    .venv/bin/python -I tools/install/install.py --iso jpog.iso
    .venv/bin/python -I tools/install/install.py --download

Into one folder (by default the Mac's Application Support, where the app
will look):

    game/            the installed game: what the Windows installer would
                     have put in Program Files
    install.json     what each part was made from

No installer is run. The disc is an InstallShield 8 install: a Windows
Installer database (`Jurassic Park Operation Genesis.msi`) whose tables say
where each file goes, and one cabinet, `Data1.cab`, holding the files by
their database keys. Both are read where they lie in the disc image
(tools/disc/iso.py, msi.py, cab.py, as stl-recomp's), and the files are
written straight into the game folder by their real names. The DirectX 8.1
redistributable the disc carries (`DirectX81/`) and the disc's own setup
programs are not part of the install.

The game's graphics options tool (`Setup/Setup.exe`) writes `options.ini`
beside the game; nobody runs it here, so a default one is written (OPTIONS),
and kept from then on: it is the player's.

Run again, it redoes only what is out of date: the disc's step runs again
only when it changes (DATA_VERSION) or the game folder is gone, and then
needs the disc image again. --status says what a run would redo, as JSON.
The analysis, the lift and the compile join these steps in later phases
(docs/PLAN.md).

Progress goes to stdout as plain lines; `@step i n title`, `@progress
fraction` and `@error text` lines are for a front end to show.
"""
import argparse
import hashlib
import json
import os
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'disc'))

import cab  # noqa: E402
import iso  # noqa: E402
import msi  # noqa: E402

ISO_MD5 = 'eee1e2577237ba032b93ed37ed43ede4'
# SimJP.exe as the disc installs it (1.00.0000, English, SecuROM-wrapped).
GAME_MD5 = '6cc8989f299f64524577915a4a0d91b0'
GAME_EXE = 'SimJP.exe'
DISC_MSI = 'Jurassic Park Operation Genesis.msi'
# The disc's files not installed (none of the database's are skipped so far).
SKIP = ()
# Bumped when the disc's step changes what it makes.
DATA_VERSION = 1
if sys.platform == 'darwin':
    DEFAULT_DEST = os.path.expanduser('~/Library/Application Support/Jurassic Park Operation Genesis')
else:                         # Linux, later: as stl-recomp's
    DEFAULT_DEST = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'),
                                'jurassic-park-operation-genesis')
# What a player's run writes into the game folder, carried over a new one.
KEEP = ('options.ini', 'Data/SaveGame')
SPACE_NEEDED = 1 << 30       # the game folder (467 MB) and the download (337 MB)

# What Setup/Setup.exe would write on a first run, at its middle settings
# (its keys are its own, as Setup.ini names them; the game reads the file).
# To be checked against what the game reads, once it runs (PLAN.md §6).
OPTIONS = None

# --download: the disc image, from where this project keeps a copy of it
# (JPOG_DOWNLOAD_BASE: elsewhere). Checked against the same MD5 as ever.
DOWNLOAD_BASE = os.environ.get('JPOG_DOWNLOAD_BASE', 'https://files.cpressland.io/public/jpog-recomp')
DOWNLOADS = (('disc image', 'jpog.iso', ISO_MD5, 'iso'),)


class Failed(Exception):
    pass


def say(*a):
    print(*a, flush=True)


def md5(path, progress=None):
    h, size, done = hashlib.md5(), os.path.getsize(path), 0
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done / size)
    return h.hexdigest()


def _progress(lo, hi):
    """A progress callback for one stage, as part of the whole step."""
    last = [0.0]

    def report(done, total):
        if time.time() - last[0] > 0.25 or done == total:
            last[0] = time.time()
            say(f'@progress {lo + (hi - lo) * done / max(total, 1):.3f}')
    return report


def install_files(db, cabinets, out, skip=SKIP, progress=None):
    """Write the files a database installs, from its cabinet set, under out.
    Returns how many were written."""
    tree = {key: (path, size) for key, path, size, _seq in msi.install_tree(db)}
    written = [0]

    def write(key, data, mtime):
        if key not in tree:
            raise Failed(f'the cabinet holds {key}, which the database does not install')
        path, size = tree[key]
        if skip and path.startswith(skip):
            return
        if len(data) != size:
            raise Failed(f'{path}: {len(data)} bytes in the cabinet, {size} in the database')
        cab.write_into(out)(path, data, mtime)
        written[0] += 1
    try:
        cab.CabinetSet(cabinets).extract(write, progress)
    except ValueError as e:
        raise Failed(f'the cabinet cannot be read: {e}') from None
    if written[0] != len(tree) - sum(1 for p, _ in tree.values() if skip and p.startswith(skip)):
        raise Failed(f'the database installs {len(tree)} files; the cabinet gave {written[0]}')
    return written[0]


class Install:
    def __init__(self, a):
        self.a = a
        self.dest = os.path.abspath(a.dest)
        self.game = os.path.join(self.dest, 'game')
        self.tmp = os.path.join(self.dest, 'tmp')
        self.state_path = os.path.join(self.dest, 'install.json')
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}

    # ---- bookkeeping ---------------------------------------------------

    def save_state(self):
        os.makedirs(self.dest, exist_ok=True)
        with open(self.state_path + '.new', 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(self.state_path + '.new', self.state_path)

    def keys(self):
        return {'data': f'data-{DATA_VERSION}'}

    def stale(self, keys):
        """The parts out of date, in order: one stale part makes all after it so."""
        if not os.path.isfile(os.path.join(self.game, GAME_EXE)):
            self.state.pop('data', None)
        out = []
        for k in ('data',):
            if out or self.state.get(k) != keys[k]:
                out.append(k)
        return out

    # ---- the steps -------------------------------------------------------

    def check(self):
        missing = [m for m in ('olefile',) if not _importable(m)]
        if missing:
            raise Failed(f'this Python lacks {", ".join(missing)} (README: Python packages)')
        os.makedirs(self.dest, exist_ok=True)
        free = shutil.disk_usage(self.dest).free
        if free < SPACE_NEEDED and not os.path.isdir(self.game):
            raise Failed(f'{free >> 20} MB free at {self.dest}; it needs about {SPACE_NEEDED >> 20} MB')

    def download(self):
        """The disc image, into downloads/: resumed where a last try stopped,
        checked, and kept only until the game folder is made."""
        folder = os.path.join(self.dest, 'downloads')
        os.makedirs(folder, exist_ok=True)
        try:
            ctx = ssl.create_default_context()
            urllib.request.urlopen(urllib.request.Request(DOWNLOAD_BASE + '/' + DOWNLOADS[0][1], method='HEAD'),
                                   context=ctx, timeout=30).close()
        except ssl.SSLError:
            ctx = ssl.create_default_context(cafile='/etc/ssl/cert.pem')    # macOS's own roots
        except (urllib.error.URLError, OSError) as e:
            raise Failed(f'cannot reach {DOWNLOAD_BASE}: {getattr(e, "reason", e)}')
        sizes = {}
        for what, name, want, _ in DOWNLOADS:
            with urllib.request.urlopen(urllib.request.Request(f'{DOWNLOAD_BASE}/{name}', method='HEAD'),
                                        context=ctx, timeout=30) as r:
                sizes[name] = int(r.headers['Content-Length'])
        total, before = sum(sizes.values()), 0
        for what, name, want, arg in DOWNLOADS:
            path = os.path.join(folder, name)
            have = os.path.getsize(path) if os.path.exists(path) else 0
            if have > sizes[name]:
                os.remove(path)
                have = 0
            if have < sizes[name]:
                say(f'downloading the {what} ({sizes[name] >> 20} MB)...')
                req = urllib.request.Request(f'{DOWNLOAD_BASE}/{name}')
                if have:
                    req.add_header('Range', f'bytes={have}-')
                    say(f'  resuming at {have >> 20} MB')
                try:
                    with urllib.request.urlopen(req, context=ctx, timeout=60) as r, \
                            open(path, 'ab' if have and r.status == 206 else 'wb') as f:
                        if r.status != 206:
                            have = 0
                        last = 0.0
                        for chunk in iter(lambda: r.read(1 << 20), b''):
                            f.write(chunk)
                            have += len(chunk)
                            if time.time() - last > 0.25:
                                last = time.time()
                                say(f'@progress {(before + have) / total:.3f}')
                except (urllib.error.URLError, OSError) as e:
                    raise Failed(f'the download of the {what} stopped ({getattr(e, "reason", e)}); '
                                 'trying again carries on from there')
            say(f'checking the {what}...')
            got = md5(path)
            if got != want:
                os.remove(path)
                raise Failed(f'the downloaded {what} is not the expected file (MD5 {got[:8]}…, '
                             f'wanted {want[:8]}…); trying again downloads it afresh')
            setattr(self.a, arg, path)
            before += sizes[name]
        say('@progress 1')

    def need_inputs(self):
        path = self.a.iso
        if not path:
            raise Failed('the game must be set up from the disc: give --iso, or --download')
        if not os.path.isfile(path):
            raise Failed(f'no disc image at {path}')
        say('checking the disc image...')
        got = md5(path, lambda f: say(f'@progress {f:.3f}'))
        if got != ISO_MD5:
            raise Failed(f'{os.path.basename(path)} is not the disc image this needs: check it is the '
                         f'right file, downloaded whole (MD5 {got[:8]}…, wanted {ISO_MD5[:8]}…)')

    def disc(self):
        """Install from the disc: a new game folder, the player's files carried over."""
        self.need_inputs()
        shutil.rmtree(self.tmp, ignore_errors=True)
        new = os.path.join(self.tmp, 'game')
        os.makedirs(new)

        say('installing from the disc...')
        entries = {p: (lba, ln) for p, lba, ln, _ in iso.files(self.a.iso)}

        def from_iso(name):
            if name not in entries:
                raise Failed(f'the disc image has no {name}')
            lba, ln = entries[name]
            return cab.Part(self.a.iso, lba * iso.SECTOR, ln, name)
        db = msi.Database(from_iso(DISC_MSI).read(0, entries[DISC_MSI][1]))
        media = sorted(db.table('Media'), key=lambda m: m['DiskId'])
        n = install_files(db, [from_iso(m['Cabinet']) for m in media if m['Cabinet']], new,
                          progress=_progress(0, 1))
        say(f'{n} files')

        exe = os.path.join(new, GAME_EXE)
        got = md5(exe) if os.path.exists(exe) else 'missing'
        if got != GAME_MD5:
            raise Failed(f'{GAME_EXE} is not the build this needs (MD5 {got}, wanted {GAME_MD5})')
        for rel in KEEP:
            old = os.path.join(self.game, rel)
            if os.path.exists(old):
                dst = os.path.join(new, rel)
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.replace(old, dst)
        if OPTIONS is not None and not os.path.exists(os.path.join(new, 'options.ini')):
            with open(os.path.join(new, 'options.ini'), 'w', newline='\r\n') as f:
                f.write(OPTIONS)
        if os.path.exists(self.game):
            os.replace(self.game, os.path.join(self.tmp, 'old-game'))
        os.replace(new, self.game)
        shutil.rmtree(self.tmp, ignore_errors=True)
        shutil.rmtree(os.path.join(self.dest, 'downloads'), ignore_errors=True)

    # ---- the order -------------------------------------------------------

    def status(self):
        stale = self.stale(self.keys())
        say(json.dumps({'ready': not stale, 'stale': stale, 'needs_disc': 'data' in stale}))

    def go(self):
        keys = self.keys()
        if self.a.force:
            self.state = {}
        stale = self.stale(keys)
        fetch = 'data' in stale and self.a.download and not self.a.iso
        steps = [('Checking the tools', self.check, None),
                 *([('Downloading the game', self.download, None)] if fetch else []),
                 ('Installing the game', self.disc, 'data')]
        todo = [s for s in steps if s[2] is None or s[2] in stale]
        for i, (title, fn, key) in enumerate(todo, 1):
            say(f'@step {i} {len(todo)} {title}')
            say(f'==> [{i}/{len(todo)}] {title}')
            t = time.time()
            if key:
                self.state.pop(key, None)     # half-made until it finishes
                self.save_state()
            fn()
            if key:
                self.state[key] = keys[key]
                self.save_state()
            say(f'    {title.lower()}: {time.time() - t:.0f} s')
        say('@done')
        say(f'done: the game folder is {self.game}')


def _importable(name):
    try:
        __import__(name)
        return True
    except ImportError:
        return False


def main():
    if os.environ.get('JPOG_PGROUP'):
        os.setpgrp()       # the app stops the whole build through the group
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('--iso', help='jpog.iso (needed the first time)')
    ap.add_argument('--download', action='store_true',
                    help='with no --iso, download it (JPOG_DOWNLOAD_BASE)')
    ap.add_argument('--dest', default=DEFAULT_DEST, help='where it goes (default: %(default)s)')
    ap.add_argument('--force', action='store_true', help='redo every step')
    ap.add_argument('--status', action='store_true', help='print what a run would redo, as JSON')
    a = ap.parse_args()
    try:
        if a.status:
            Install(a).status()
        else:
            Install(a).go()
    except Failed as e:
        say(f'@error {e}')
        say(f'error: {e}')
        sys.exit(1)


if __name__ == '__main__':
    main()
