"""From the disc image to a game that runs, in one go.

    .venv/bin/python -I tools/install/install.py --iso jurassic_park_operation_genesis.iso
    .venv/bin/python -I tools/install/install.py --download

Every step of the build, into one folder (by default the Mac's Application
Support, where Jurassic Park Operation Genesis.app looks; on Linux
$XDG_DATA_HOME/jurassic-park-operation-genesis):

    game/             the installed game with SimJP.exe unwrapped from SecuROM
    build/src/        the sources it builds from, copied from this repository
                      and pcrecomp (SOURCES), so the build's paths never move
    build/analysis/   each module's function catalog and lift inputs
    build/gen/        the lifted C of all twenty modules (run_lift.py)
    build/host/       the compiled game: libjpog.dylib (.so), which the app
                      loads, and jpog, the same as a program
    install.json      what each part was made from

Run again, it redoes only what is out of date: after a fix to the host only
the compile (ninja: what changed), after a lifter fix the lift and the
compile, and only after a change to the disc's steps (DATA_VERSION) the
whole of it, which needs the disc image again. The player's saves and
options are kept. --status says what a run would redo, as JSON.

Progress goes to stdout as plain lines; `@step i n title`, `@progress
fraction` and `@error text` lines are for a front end to show.
"""
import argparse
import filecmp
import hashlib
import json
import os
import shutil
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'disc'))

ISO_MD5 = 'eee1e2577237ba032b93ed37ed43ede4'
# SimJP.exe as the disc installs it (1.00.0000, English, SecuROM-wrapped).
GAME_MD5 = '6cc8989f299f64524577915a4a0d91b0'
GAME_EXE = 'SimJP.exe'
DISC_MSI = 'Jurassic Park Operation Genesis.msi'
SKIP = ()
# Bumped when the disc's steps (extract, unwrap) change what they make.
DATA_VERSION = 2
if sys.platform == 'darwin':
    DEFAULT_DEST = os.path.expanduser('~/Library/Application Support/Jurassic Park Operation Genesis')
    LIBRARY = 'libjpog.dylib'
else:
    DEFAULT_DEST = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'),
                                'jurassic-park-operation-genesis')
    LIBRARY = 'libjpog.so'
# What a player's run writes into the game folder, carried over a new one.
KEEP = ('options.ini', 'Options.ini', os.path.join('Data', 'SaveGame'))
SPACE_NEEDED = 4 << 30       # the game folder, analysis, lifted C and build at peak

# --download: the disc image, from where this project keeps a copy of it
# (JPOG_DOWNLOAD_BASE: elsewhere). Checked against the same MD5 as ever.
DOWNLOAD_BASE = os.environ.get('JPOG_DOWNLOAD_BASE', 'https://files.cpressland.io/public/jpog-recomp')
DOWNLOADS = (('disc image', 'jurassic_park_operation_genesis.iso', ISO_MD5, 'iso'),)

# What the build needs of each tree: what the app carries, and what is staged.
SOURCES = {'jpog': ('CMakeLists.txt', 'run_lift.py', 'src/host', 'tools', 'docs/patches'),
           'pcrecomp': ('runtime', 'tools', 'LICENSE')}
SDL_FRAMEWORKS = ('SDL2.framework', 'SDL2_ttf.framework')


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


def _files(top):
    """Every file under a folder (or the file itself), less Python's caches."""
    if os.path.isfile(top):
        return [top]
    return sorted(os.path.join(d, n) for d, ds, ns in os.walk(top) for n in ns
                  if '__pycache__' not in d.split(os.sep) and not n.endswith('.pyc') and n != '.DS_Store')


def tree_hash(*paths):
    """A hash of source files: what a step's output depends on."""
    h = hashlib.sha1()
    for top in paths:
        if not os.path.exists(top):
            continue
        for p in _files(top):
            h.update(os.path.relpath(p, os.path.dirname(top)).encode())
            with open(p, 'rb') as f:
                h.update(f.read().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:16]


def sync(src, rels, dst):
    """Make dst/<rel> match src/<rel>: a file is written only when its bytes
    differ, so its time says when it last changed, whatever the source's says.
    Returns the count of changed files."""
    changed = 0
    for rel in rels:
        s, d = os.path.join(src, rel), os.path.join(dst, rel)
        want = {os.path.relpath(p, s) if os.path.isdir(s) else '' for p in _files(s)} if os.path.exists(s) else set()
        if os.path.isdir(d):
            for p in _files(d):
                if os.path.relpath(p, d) not in want:
                    os.remove(p)
                    changed += 1
        for r in sorted(want):
            sp, dp = os.path.join(s, r) if r else s, os.path.join(d, r) if r else d
            if os.path.isfile(dp) and filecmp.cmp(sp, dp, shallow=False):
                continue
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            shutil.copyfile(sp, dp)
            shutil.copymode(sp, dp)
            changed += 1
    return changed


def patch_pcrecomp_dinput(pc_dir):
    """Apply JPOG's two DirectInput fixes (docs/patches/) to pcrecomp's dinput.c
    if not already present. Pure Python so no git/patch binary is needed."""
    path = os.path.join(pc_dir, 'runtime', 'win32hle', 'dinput.c')
    if not os.path.isfile(path):
        return
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()
    orig = text
    # 1. DIPROP_AXISMODE: ABS is 0, REL is 1 (pcrecomp-dinput-axismode.patch)
    text = text.replace(
        'if (id == 2) { MEM32(h + 16) = (uint32_t)d->absolute; RET(DI_OK, 3); }   /* DIPROP_AXISMODE */',
        'if (id == 2) { MEM32(h + 16) = !d->absolute; RET(DI_OK, 3); }   /* DIPROP_AXISMODE: ABS 0, REL 1 */',
    )
    text = text.replace(
        'else if (id == 2) d->absolute = MEM32(h + 16) == 1, relative();   /* DIPROPAXISMODE_ABS */',
        'else if (id == 2) d->absolute = MEM32(h + 16) == 0, relative();   /* DIPROPAXISMODE_ABS 0, _REL 1 */',
    )
    # 2. Acquire/Unacquire return DI_NOEFFECT (S_FALSE = 1) on no-op (pcrecomp-dinput-acquire-noeffect.patch)
    old_acq = (
        'static void dv_Acquire(void) {\n'
        '    dev_t_ *d = DEV(A32(0));\n'
        '    if (!d->acquired) baseline(d), d->acquired = 1;\n'
        '    relative();\n'
        '    RET(DI_OK, 1);\n'
        '}\n'
        'static void dv_Unacquire(void) { DEV(A32(0))->acquired = 0; relative(); RET(DI_OK, 1); }'
    )
    new_acq = (
        'static void dv_Acquire(void) {\n'
        '    dev_t_ *d = DEV(A32(0));\n'
        '    int was = d->acquired;\n'
        '    if (!was) baseline(d), d->acquired = 1;\n'
        '    relative();\n'
        '    RET(was ? 1u : DI_OK, 1);                            /* S_FALSE (DI_NOEFFECT) if already acquired */\n'
        '}\n'
        'static void dv_Unacquire(void) {\n'
        '    dev_t_ *d = DEV(A32(0));\n'
        '    int was = d->acquired;\n'
        '    d->acquired = 0;\n'
        '    relative();\n'
        '    RET(was ? DI_OK : 1u, 1);                            /* S_FALSE (DI_NOEFFECT) if already unacquired */\n'
        '}'
    )
    text = text.replace(old_acq, new_acq)
    if text != orig:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)


def find_pcrecomp(given):
    for p in (given, os.environ.get('PCRECOMP'), os.path.join(REPO, 'pcrecomp'),
              os.path.join(REPO, 'work', 'pcrecomp'),
              os.path.join(os.path.dirname(REPO), 'pcrecomp')):
        if p and os.path.isdir(os.path.join(p, 'runtime', 'win32hle')):
            return os.path.abspath(p)
    raise Failed('no pcrecomp found: give --pcrecomp, or clone it beside this repository (README)')


def system_memory_bytes():
    try:
        return os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
    except (ValueError, OSError):
        return 0


def build_jobs():
    """How many compilers at once: one a core, capped by memory so gcc/clang
    do not hit the OOM killer on the largest lifted files (e.g. Steam Deck 16 GB)."""
    own = os.environ.get('JPOG_JOBS')
    if own and own.isdigit() and int(own) > 0:
        return int(own)
    mem = system_memory_bytes()
    cpus = os.cpu_count() or 1
    per_job = (3 << 30) if sys.platform != 'darwin' else (2 << 30)
    return max(1, min(cpus, mem // per_job)) if mem else cpus


def find_tools():
    """CMake and Ninja: $JPOG_CMAKE (the AppImage's), this Python's own
    packages (the app's), then the PATH."""
    cm = os.environ.get('JPOG_CMAKE')
    if not (cm and os.access(cm, os.X_OK)):
        try:
            import cmake
            cm = os.path.join(cmake.CMAKE_BIN_DIR, 'cmake')
        except ImportError:
            cm = shutil.which('cmake')
    nj = os.path.join(os.path.dirname(sys.executable), 'ninja')
    if not os.access(nj, os.X_OK):
        nj = shutil.which('ninja')
    return cm, nj


def install_files(db, cabinets, out, skip=SKIP, progress=None):
    """Write the files a database installs, from its cabinet set, under out.
    Returns how many were written."""
    import cab
    import msi
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
        self.pcrecomp = find_pcrecomp(a.pcrecomp)
        self.game = os.path.join(self.dest, 'game')
        self.build = os.path.join(self.dest, 'build')
        self.src = os.path.join(self.build, 'src')
        self.jpog_src = os.path.join(self.src, 'jpog')
        self.pc_src = os.path.join(self.src, 'pcrecomp')
        self.sdl = os.path.join(self.src, 'Frameworks') if a.sdl_frameworks else None
        self.ffmpeg = os.path.join(self.src, 'ffmpeg') if a.ffmpeg else None
        self.vorbis = os.path.join(self.src, 'vorbis') if a.vorbis else None
        self.analysis = os.path.join(self.build, 'analysis')
        self.gen = os.path.join(self.build, 'gen')
        self.host = os.path.join(self.build, 'host')
        self.tmp = os.path.join(self.dest, 'tmp')
        self.state_path = os.path.join(self.dest, 'install.json')
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}
        self.env = dict(os.environ, PCRECOMP=self.pc_src)

    # ---- bookkeeping ---------------------------------------------------

    def save_state(self):
        os.makedirs(self.dest, exist_ok=True)
        with open(self.state_path + '.new', 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(self.state_path + '.new', self.state_path)

    def run(self, *cmd):
        """A step's command; its output is shown as it comes."""
        say('$ ' + ' '.join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
        p = subprocess.run(cmd, cwd=self.jpog_src, env=self.env)
        if p.returncode:
            raise Failed(f'{os.path.basename(cmd[0])} {cmd[1] if len(cmd) > 1 else ""} failed ({p.returncode})')

    def py(self, script, *args):
        self.run(sys.executable, '-I', script, *args)

    # ---- the keys: what each part was made from -------------------------

    def keys(self):
        """From the sources given (the repository's, or the app's), not the staged copy."""
        tools = os.path.join(self.pcrecomp, 'tools')
        data = f'data-{DATA_VERSION}'
        analysis = tree_hash(os.path.join(tools, 'pe'), os.path.join(tools, 'cpp'),
                             os.path.join(tools, 'disasm'), os.path.join(REPO, 'tools', 'gen'))
        lift = tree_hash(os.path.join(tools, 'lift'), os.path.join(REPO, 'run_lift.py'))
        host = tree_hash(os.path.join(REPO, 'CMakeLists.txt'), os.path.join(REPO, 'src', 'host'),
                         os.path.join(REPO, 'docs', 'patches'),
                         os.path.join(self.pcrecomp, 'runtime'),
                         *(os.path.join(self.a.sdl_frameworks, f) for f in SDL_FRAMEWORKS if self.a.sdl_frameworks),
                         *(p for p in (self.a.ffmpeg, self.a.vorbis) if p))
        return {'data': data,
                'analysis': f'{data}/{analysis}',
                'lift': f'{data}/{analysis}/{lift}',
                'host': f'{data}/{analysis}/{lift}/{host}'}

    def stale(self, keys):
        """The parts out of date, in order: one stale part makes all after it so."""
        if not os.path.isfile(os.path.join(self.game, GAME_EXE)):
            self.state.pop('data', None)
        if not os.path.exists(os.path.join(self.host, LIBRARY)):
            self.state.pop('host', None)
        out = []
        for k in ('data', 'analysis', 'lift', 'host'):
            if out or self.state.get(k) != keys[k]:
                out.append(k)
        return out

    # ---- the steps -------------------------------------------------------

    def check(self):
        """The toolchain and the space; the inputs are checked when the disc's steps run."""
        missing = [m for m in ('olefile', 'pefile', 'capstone', 'unicorn')
                   if subprocess.run([sys.executable, '-I', '-c', f'import {m}'],
                                     capture_output=True).returncode]
        if missing:
            raise Failed(f'this Python lacks {", ".join(missing)} (README: Python packages)')
        if sys.platform == 'darwin':
            if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
                raise Failed("no C compiler: install Xcode's command line tools (xcode-select --install)")
        elif not (os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')):
            raise Failed('no C compiler: install clang (or gcc), e.g. the build-essential package')
        self.cmake, self.ninja = find_tools()
        if not self.cmake or not self.ninja:
            raise Failed('no cmake or ninja (README: prerequisites)')
        if self.a.sdl_frameworks:
            for f in SDL_FRAMEWORKS:
                if not os.path.isdir(os.path.join(self.a.sdl_frameworks, f)):
                    raise Failed(f'no {f} in {self.a.sdl_frameworks}')
        elif subprocess.run(['pkg-config', '--exists', 'sdl2', 'SDL2_ttf']).returncode:
            raise Failed('pkg-config cannot find sdl2 and SDL2_ttf (README: prerequisites)')
        os.makedirs(self.dest, exist_ok=True)
        free = shutil.disk_usage(self.dest).free
        if free < SPACE_NEEDED and not os.path.isdir(self.host):
            raise Failed(f'{free >> 20} MB free at {self.dest}; it needs about {SPACE_NEEDED >> 30} GB')

    def stage(self):
        """The sources, and bundled libraries, copied to where the build always finds them."""
        n = sync(REPO, SOURCES['jpog'], self.jpog_src) + sync(self.pcrecomp, SOURCES['pcrecomp'], self.pc_src)
        patch_pcrecomp_dinput(self.pc_src)
        for f in SDL_FRAMEWORKS if self.sdl else ():
            src, dst = os.path.join(self.a.sdl_frameworks, f), os.path.join(self.sdl, f)
            if not os.path.isdir(dst) or tree_hash(src) != tree_hash(dst):
                shutil.rmtree(dst, ignore_errors=True)
                shutil.copytree(src, dst, symlinks=True)
                n += 1
        for src, dst in ((self.a.ffmpeg, self.ffmpeg), (self.a.vorbis, self.vorbis)):
            if src and dst:
                n += sync(src, ('include', 'lib'), dst)
        say(f'{n} source files changed')

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
        """Install from the disc and unwrap SecuROM on SimJP.exe."""
        self.need_inputs()
        shutil.rmtree(self.tmp, ignore_errors=True)
        new = os.path.join(self.tmp, 'game')
        os.makedirs(new)

        say('installing from the disc...')
        import cab
        import iso
        import msi
        entries = {p: (lba, ln) for p, lba, ln, _ in iso.files(self.a.iso)}

        def from_iso(name):
            if name not in entries:
                raise Failed(f'the disc image has no {name}')
            lba, ln = entries[name]
            return cab.Part(self.a.iso, lba * iso.SECTOR, ln, name)
        db = msi.Database(from_iso(DISC_MSI).read(0, entries[DISC_MSI][1]))
        media = sorted(db.table('Media'), key=lambda m: m['DiskId'])
        n = install_files(db, [from_iso(m['Cabinet']) for m in media if m['Cabinet']], new,
                          progress=_progress(0, 0.7))
        say(f'{n} files')

        exe = os.path.join(new, GAME_EXE)
        got = md5(exe) if os.path.exists(exe) else 'missing'
        if got != GAME_MD5:
            raise Failed(f'{GAME_EXE} is not the build this needs (MD5 {got}, wanted {GAME_MD5})')

        say('@progress -1')
        say('unwrapping SimJP.exe...')
        patch_work = os.path.join(self.tmp, 'patch')
        self.py(os.path.join(self.jpog_src, 'tools', 'gen', 'patch_simjp.py'), new, patch_work)
        patched_exe = os.path.join(patch_work, 'simjp', GAME_EXE)
        if not os.path.isfile(patched_exe):
            raise Failed('unwrapping SimJP.exe did not produce an executable')
        os.replace(patched_exe, exe)

        for rel in KEEP:
            old = os.path.join(self.game, rel)
            if os.path.exists(old):
                dst = os.path.join(new, rel)
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.replace(old, dst)
        if os.path.exists(self.game):
            os.replace(self.game, os.path.join(self.tmp, 'old-game'))
        os.replace(new, self.game)
        shutil.rmtree(self.tmp, ignore_errors=True)
        shutil.rmtree(os.path.join(self.dest, 'downloads'), ignore_errors=True)

    def analyse(self):
        """The headers and imports, C++ classes, function catalogs, code in data."""
        shutil.rmtree(self.analysis, ignore_errors=True)
        os.makedirs(self.analysis)
        say('cataloguing all 20 modules (a few minutes)...')
        self.py(os.path.join(self.jpog_src, 'tools', 'gen', 'analyse.py'), self.game, self.analysis)

    def lift(self):
        shutil.rmtree(self.gen, ignore_errors=True)
        os.makedirs(self.gen)
        self.py(os.path.join(self.jpog_src, 'run_lift.py'), '--work', self.analysis, '--out', self.gen)

    def compile(self):
        sdl = self.sdl or subprocess.run(['pkg-config', '--cflags', '--libs', 'sdl2', 'SDL2_ttf'],
                                         capture_output=True, text=True).stdout.strip()
        gen_opt = '-O1' if (sys.platform != 'darwin' and system_memory_bytes() <= (20 << 30)) else '-O2'
        config = [self.jpog_src, self.pc_src, sdl, self.ffmpeg or '', self.vorbis or '', self.cmake, gen_opt]
        if self.state.get('host_config') != config:
            shutil.rmtree(self.host, ignore_errors=True)
        if not os.path.exists(os.path.join(self.host, 'build.ninja')):
            self.run(self.cmake, '-S', self.jpog_src, '-B', self.host, '-G', 'Ninja',
                     f'-DCMAKE_MAKE_PROGRAM={self.ninja}', f'-DPCRECOMP={self.pc_src}', f'-DGEN_DIR={self.gen}',
                     f'-DGEN_OPT={gen_opt}',
                     f'-DJPOG_SDL_FRAMEWORKS={self.sdl or ""}',
                     f'-DJPOG_FFMPEG={self.ffmpeg or ""}',
                     f'-DJPOG_VORBIS={self.vorbis or ""}')
            self.state['host_config'] = config
            self.save_state()
        say('compiling (a few minutes the first time)...')
        self.run(self.cmake, '--build', self.host, '-j', str(build_jobs()))

    # ---- the order -------------------------------------------------------

    def status(self):
        keys = self.keys()
        stale = self.stale(keys)
        say(json.dumps({'ready': not stale, 'stale': stale, 'needs_disc': 'data' in stale}))

    def go(self):
        keys = self.keys()
        if self.a.force:
            self.state = {}
        stale = self.stale(keys)
        fetch = 'data' in stale and self.a.download and not self.a.iso
        steps = [('Checking the tools', self.check, None),
                 ('Copying the sources', self.stage, None),
                 *([('Downloading the game', self.download, None)] if fetch else []),
                 ('Unpacking and unwrapping the game', self.disc, 'data'),
                 ('Analysing the game', self.analyse, 'analysis'),
                 ('Translating the game to C', self.lift, 'lift'),
                 ('Compiling', self.compile, 'host')]
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
        jpog = os.path.join(self.host, 'jpog')
        say('done. Play with Jurassic Park Operation Genesis.app, or from a terminal:' if sys.platform == 'darwin'
            else 'done. Play from a terminal:')
        say(f'  "{jpog}" --game "{self.game}" --run')


def export(out, pcrecomp):
    """The sources the app carries: SOURCES of this repository and pcrecomp."""
    pc_out = os.path.join(out, 'pcrecomp')
    n = sync(REPO, SOURCES['jpog'], os.path.join(out, 'jpog')) + \
        sync(find_pcrecomp(pcrecomp), SOURCES['pcrecomp'], pc_out)
    patch_pcrecomp_dinput(pc_out)
    say(f'{n} files to {out}')


def main():
    if os.environ.get('JPOG_PGROUP'):
        os.setpgrp()       # the app stops the whole build (compilers too) through the group
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('--iso', help='jurassic_park_operation_genesis.iso (needed the first time)')
    ap.add_argument('--download', action='store_true',
                    help='with no --iso, download the disc image (JPOG_DOWNLOAD_BASE)')
    ap.add_argument('--dest', default=DEFAULT_DEST, help='where it goes (default: %(default)s)')
    ap.add_argument('--pcrecomp', help='the pcrecomp checkout (default: ./pcrecomp, work/pcrecomp, ../pcrecomp)')
    ap.add_argument('--sdl-frameworks', help='a folder with SDL2.framework and SDL2_ttf.framework '
                    '(default: SDL from pkg-config)')
    ap.add_argument('--ffmpeg', help='a static ffmpeg folder (include/ and lib/) for Bink movies')
    ap.add_argument('--vorbis', help='a static libogg+libvorbis folder (include/ and lib/) for Ogg music')
    ap.add_argument('--force', action='store_true', help='redo every step')
    ap.add_argument('--status', action='store_true', help='print what a run would redo, as JSON')
    ap.add_argument('--export-sources', metavar='DIR', help='copy the sources the build needs to DIR, and stop')
    a = ap.parse_args()
    for attr in ('sdl_frameworks', 'ffmpeg', 'vorbis'):
        v = getattr(a, attr)
        if v:
            setattr(a, attr, os.path.abspath(v))
    try:
        if a.export_sources:
            export(a.export_sources, a.pcrecomp)
        elif a.status:
            Install(a).status()
        else:
            Install(a).go()
    except Failed as e:
        say(f'@error {e}')
        say(f'error: {e}')
        sys.exit(1)


if __name__ == '__main__':
    main()
