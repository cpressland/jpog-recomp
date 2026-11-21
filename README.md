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
and no installer; the SecuROM wrapper on `SimJP.exe` has been taken apart far
enough to show the game's own code and import table are intact, and its
dispatcher can be driven headless under emulation (see
[docs/PLAN.md](docs/PLAN.md), Phase 2). Nothing is lifted or drawn yet.

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
Recovering the 3,642-entry call-site table from that dispatcher is the next
step. See [docs/RECON.md](docs/RECON.md).

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
