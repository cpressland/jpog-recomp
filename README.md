# Jurassic Park: Operation Genesis — Static Recompilation

A static recompilation of Blue Tongue Software's **Jurassic Park: Operation
Genesis** (Universal Interactive, 2003) from its shipping Win32 binaries to
native C, targeting **macOS** first (Metal), then Linux (the Steam Deck among
them) and Windows. Built on the
[pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain and its
`runtime/win32hle`, the same way as
[stl-recomp](https://github.com/cpressland/stl-recomp) and
[tpw-recomp](https://github.com/cpressland/tpw-recomp).

**Status: early.** The game folder builds from the disc image with no Windows
and no installer; the SecuROM wrapper on `SimJP.exe` has been fully resolved
(see [docs/PLAN.md](docs/PLAN.md), Phase 2). All 3,642 redirected import
calls have been mapped back to their original targets. All 20 modules
(SimJP.exe, 19 DLLs) are lifted to C and compile. The host maps and binds the
DLLs, runs their start-up, and answers Direct3D 8 on Metal: Toshi's own
renderer runs on it, and shows the game's models, animated, its islands, with
their sky, sea and grass, and its menus, in a window (below). The game itself has not been run on it.

## The lift

With pcrecomp at `app/pcrecomp.lock`'s commit beside this repository (or
`PCRECOMP=DIR`), after the game folder:

```
.venv/bin/python -I tools/gen/analyse.py GAME work/analysis   # catalogs, seconds
.venv/bin/python -I tools/gen/analyse_simjp.py GAME work/analysis  # SimJP.exe (SecuROM)
.venv/bin/python -I run_lift.py --work work/analysis --out work/gen
cmake -S . -B build-macos -G Ninja && cmake --build build-macos
```

`tools/gen/modules.py` names the modules and the base each is lifted at
(every DLL wants `0x10000000`, so each moves to its own 1 MB from
`0x01000000`; `tools/gen/rebase.py`). SimJP.exe keeps its original base at
`0x00400000`. 52,151 functions, no lift errors, 5.93 million lines of C that
compile in about 2 minutes.

## The host

`src/host` on pcrecomp's `runtime/win32hle`, with SDL2 and SDL2_ttf from
pkg-config (Homebrew): `loader.c` maps the modules at their bases and binds
them to each other, `msvcrt.c` is Windows' C runtime (not on the disc),
`shims.c` the Win32 calls win32hle lacks, `setjmp.c` and `cpu.c` as
stl-recomp's. Direct3D 8 is `d3d8.c` (the interfaces and state), `dxso.c`
(vs_1_1 and ps_1_1 to Metal's language), `ff.c` (fixed function, likewise)
and `d3d_metal.m`; `engine.c` and `gui.c` drive the engine. Its test needs only the DLLs, lifted to a folder of their own
(`--modules` with every module but `simjp`):

```
.venv/bin/python -I run_lift.py --work work/analysis --out work/gen-dlls --modules tkernel,trender,...
cmake -S . -B build-dlls -G Ninja -DGEN_DIR=$PWD/work/gen-dlls && cmake --build build-dlls
build-dlls/jpog --game GAME --selftest
```

A model, through the engine's own scene, camera, materials and skeleton, with
no `SimJP.exe` (`src/host/engine.c`; Escape or closing the window ends it):

```
build-dlls/jpog --game GAME --view Data/Models/Trex_hi.tmd \
    --materials Data/matlibs/dinos_judithriver_b.tml --anim 3 --windowed
python3 tools/test/models.py build-dlls/jpog GAME      # all 360, headless: each loads and draws
```

An island: Isla Nublar, or one the game's generator makes from a seed:

```
JPOG_TERRAIN_AT=290,170,6,0.3,-0.2 build-dlls/jpog --game GAME --terrain nublar --windowed
python3 tools/test/terrain.py build-dlls/jpog GAME     # 49 views, headless
```

`JPOG_TERRAIN_AT` is x, z (0 to 1008), how far over the ground or sea, heading
and pitch in radians; `JPOG_WEATHER` one of `Weather.ini`'s (`normal`,
`heatwave`, `lightrain`, `heavyrain`, `tornado`); `JPOG_TERRAIN_FAR` the far
plane (150).

A dialog of `Data/GUI/main.gui`, through the game's GUI, with the mouse:

```
JPOG_GUI_BACK=bkg_fe_menu build-dlls/jpog --game GAME --gui FrontEndMainMenu_WIN --windowed
build-dlls/jpog --game GAME --gui AParkFinanceDialog --locale German --windowed
python3 tools/test/gui.py build-dlls/jpog GAME --locales     # all 134, headless
```

The mouse needs `docs/patches/pcrecomp-dinput-axismode.patch` on pcrecomp
(DirectInput's relative axes); without it the pointer runs away.
`JPOG_GUI_MOUSE=x,y[,1]` puts the pointer there (and presses) without one.

`--headless --frames N --shot FILE.bmp` keeps the last of N frames;
`JPOG_VIEW_ZOOM` is how far back, in the model's radii (0.6).
`JPOG_TEST_SHOTS=DIR` keeps the tests' pictures; `JPOG_SHOT=DIR` every
`JPOG_SHOT_EVERY`th frame presented, `JPOG_TRACE_FRAME=N` lists frame N's
draws, `JPOG_CPU=x87` reports a CPU without SSE.

Nothing of the game is in this repository, and nothing generated from it.

## What you need

One file, the disc image:

- `jpog.iso` (337 MB, MD5 `eee1e2577237ba032b93ed37ed43ede4`).

`--download` fetches it from `files.cpressland.io` (`JPOG_DOWNLOAD_BASE` for
elsewhere), resuming a stopped download, checked by MD5.

## The game folder

Python 3.10+ and one package, olefile:

```
uv venv .venv && uv pip install --python .venv/bin/python -r requirements.txt
.venv/bin/python -I tools/install/install.py --iso jpog.iso
```

or `--download` in place of `--iso`. It takes a couple of minutes and writes
`game/` (467 MB, 2,420 files) into `~/Library/Application Support/Jurassic
Park Operation Genesis` (`--dest` for elsewhere), `SimJP.exe` checked against
its MD5. Run again, it does nothing unless the game folder is gone;
`--status` says what a run would redo.

The disc is an InstallShield 8 MSI install: `Jurassic Park Operation
Genesis.msi` says where each file goes and `Data1.cab` holds them by database
key. `tools/disc/{iso,msi,cab}.py` (stl-recomp's) read all of it with no
Windows and no installer.

## The copy protection

`SimJP.exe` is wrapped by SecuROM 4.x. The game's own code and its import
table are intact and in the clear; what SecuROM changed is 3,642 import calls,
redirected through one dispatcher it installs at run time, and 42 KB of its
own encrypted code. `tools/securom/survey.py` records all of this, and
`tools/securom/probe.py` runs the wrapper headless under Unicorn
(tpw-recomp's SafeDisc emulator, at `tools/securom/emu`): it decrypts itself
and installs the dispatcher **from the disc image, with no physical disc**.

The 3,642-entry call-site table has been fully resolved using a nocd crack
as a reference. `tools/securom/extract_from_crack.py` extracts the original
import for each redirected call site. The result is `work/securom/sites.json`,
which maps each call site to its original DLL and function. See
[docs/RECON.md](docs/RECON.md).

## The engine

Blue Tongue's **Toshi** engine (MSVC 6), across 19 DLLs that export C++
classes by mangled name, plus FMOD 3, Intel's maths library, and Bink. The
renderer is **Direct3D 8**. The plan lifts every module and implements D3D8
as a front end on stl-recomp's Direct3D-9-on-Metal device, with the engine's
fixed-function drawing generated as Metal shaders. See
[docs/PLAN.md](docs/PLAN.md) for the whole survey and the phases.

## License

MIT for this repository's own code. Jurassic Park: Operation Genesis is
© Universal Interactive and its rightsholders; none of its files, and nothing
generated from them, is in this repository.
