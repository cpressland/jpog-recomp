# Jurassic Park: Operation Genesis — Static Recompilation: Plan

A static recompilation of Blue Tongue Software's **Jurassic Park: Operation
Genesis** (Universal Interactive, 2003), from its shipping Win32 binaries to
native C on pcrecomp's `runtime/win32hle`, drawn through Metal. **macOS
first**; Linux (Steam Deck) and Windows come later, as they did for
[stl-recomp](../../stl-recomp) and [tpw-recomp](../../tpw-recomp), whose
shape this follows: disc → game folder in Python, a lift, a CMake host, a
Swift launcher app that builds the game on the player's Mac, the disc image
fetched from `files.cpressland.io`.

Everything below is from a first survey of the disc image (2026-11-21).

## 1. What we have

| File | Size | MD5 |
|---|---|---|
| `jpog.iso` | 353,101,824 | `eee1e2577237ba032b93ed37ed43ede4` |

To be put at `https://files.cpressland.io/public/jpog-recomp/jpog.iso`, so
`install.py --download` works as STL's does (`JPOG_DOWNLOAD_BASE`, resumed
with `Range`, checked by MD5). One file, no patch (see §6, open questions).

### 1.1 The disc

A plain ISO 9660 CD (STL's `tools/disc/iso.py` reads it as is: 17 files)
holding an **InstallShield 8 MSI** install, the same shape as STL's:

- `Jurassic Park Operation Genesis.msi` (ProductVersion 1.00.0000, English):
  the tables that name every file;
- `Data1.cab` (288 MB): **one** cabinet, not spanned, 2,420 files by MSI
  file key, MSZIP;
- `DirectX81/` (the DirectX 8.1 redistributable), `setup.exe`,
  `Autorun.exe`: skipped.

STL's `msi.py --tree` and `cab.py` did the whole job unchanged in this
survey: 2,420 files placed, none missing, **467 MB**, `SimJP.exe` MD5
`6cc8989f299f64524577915a4a0d91b0`. Phase 1 is mostly a copy.

```
SimJP.exe  binkw32.dll  fmod.dll  libmmd.dll  col.ini  light.ini
PGUICore  PGUIRenderer  PPropertyParser  SimJPTerrain  Units           (.dll)
TApplication  TGuiInterface  TInputDXInterface  TInputInterface
TKernelInterface  TRenderD3DInterface  TRenderInterface  TTerrainInterface (.dll)
Shaders/D3D/T{Line,Skin,Sprite,Sys,Terrain}ShaderD3D.dll
Data/  (Sound 151 MB, matlibs 115 MB, Music 50 MB, Movies 47 MB, Models, Missions, Terrain, GUI, Units, ...)
Setup/ (the graphics options tool, Setup.exe, which writes options.ini; skipped, its settings go in our launcher)
```

Data: 898 `.wav`, 30 `.ogg` music, 3 `.bik` movies, 195 `.tml` + 360
`.tmd` (Toshi materials and models), 173 `.dds`, 191 `.ini` (the game's
tuning is plain text), saves as `.sav`. The game reads all of it itself.

### 1.2 The engine

Blue Tongue's own **Toshi** engine (the same one later games of theirs use),
MSVC 6 (linker 6.0), **split across 19 DLLs** that export C++ classes by
mangled name (`?Execute@TApplication@Toshi@@QAE_NXZ`), so every call between
modules has a full signature. The game itself is `SimJP.exe` (`.text`
2.9 MB), which also **exports** 15,439 symbols that `Units.dll` (the
dinosaurs and buildings) imports back. All DLLs are relocatable and all want
`0x10000000`, so each gets a base of its own, as STL rebased `d3dx9_27`.

| Module | What | Plan |
|---|---|---|
| `SimJP.exe` | the game, **SecuROM-wrapped** (§1.3) | lift, after the wrapper is undone |
| 14 `T*`/`P*`/`SimJP*`/`Units` DLLs | Toshi kernel, render, terrain, GUI, input, properties; the game's units | lift, rebased |
| 5 `Shaders/D3D/*.dll` | materials and their **D3D8 shader bytecode** (vs 1.0/1.1, ps 1.1), loaded with `LoadLibrary` at run time | lift, rebased; attached on load, as STL's missions |
| `fmod.dll` | FMOD 3.x (`FSOUND_*`, 41 used), **UPX-packed** | unpack (`upx -d` or pcrecomp `emu_unpack.py`), then lift; it sits on DirectSound |
| `libmmd.dll` | Intel's maths library (`sin`, `pow`, ... with SSE2 paths by CPUID) | lift, CPUID answered without SSE2 at first; or bind to the host's libm |
| `binkw32.dll` | Bink 1 movies (13 calls) | **HLE**: pcrecomp's `bink.c` on ffmpeg, as STL |
| `msvcrt.dll` | Windows' own VC6 runtime: **not on the disc** | **HLE**: 91 functions in all, none of the C++ library (no msvcp60) |

What the modules ask of Windows, beyond what win32hle already answers for
TPW and STL:

| Import | What it needs | Where it comes from |
|---|---|---|
| `d3d8.dll` (`Direct3DCreate8`, in TRenderD3DInterface) | **Direct3D 8** | **new**: a D3D8 front end on STL's D3D9-on-Metal device (below) |
| `msvcrt.dll` | `malloc`/`fopen`/`sprintf`/... and `__CxxFrameHandler`, `_except_handler3`, `_setjmp3`/`longjmp`, `_beginthreadex` | **new** HLE msvcrt in win32hle; C++ EH through `eh32` (VC6's FuncInfo is the same 0x19930520 layout), `setjmp` as STL's |
| `DINPUT8` (`DirectInput8Create`) | mouse, keyboard, controller | STL's `win32hle-dinput8` (+ relative mouse, pad) |
| `dsound` (#1, `DirectSoundCreate`) + FMOD's 3D | DirectSound, 3D buffers | STL's `win32hle-dsound3d` |
| `KERNEL32`, `USER32`, `GDI32`, `WINMM` (`timeSetEvent`), `ADVAPI32` (registry), `VERSION` | the usual | mostly there; `timeSetEvent` callbacks may be new |

**D3D8 vs D3D9.** STL's `src/host/d3d9.c` + `d3d9_metal.m` + `dxso.c` are
the bulk of what's needed. D3D8 is a near-subset of D3D9 (what DXVK's and
Wine's `d3d8to9` exploit): the differences are shader *creation* (a D3D8
vertex shader carries its own declaration tokens, and FVF "shaders" are
handles), `SetStreamSource` with the stride, `SetIndices` with a base
vertex index, `CopyRects` for `StretchRect`, texture-stage states holding
what D3D9 calls sampler states, `GetBackBuffer`, and the caps struct. The
shader bytecode (vs 1.x, ps 1.1–1.4) is D3D9's own `dxso.c` already reads.
So: `d3d8.c`, a translation layer onto the D3D9 objects, rather than a new
renderer. Toshi's materials draw a lot through **fixed function**
(the terrain, sprites, GUI), which STL never needed: that is the real new
GPU work (MSL generated from texture-stage and lighting state).

### 1.3 The copy protection: SecuROM 4.x

`SimJP.exe` is wrapped by **SecuROM** (Sony DADC; sections `.cms_t` and
`.cms_d`, a Petite-packed loader DLL inside `.cms_d`, its messages stored as
`7001…` hex strings). What it changes, from this survey:

1. **The entry point and CRT startup** are moved into `.cms_t`, clear
   code: `WinMainCRTStartup` at `0xD7EF8A` calls SecuROM's own WinMain
   (`0xD61D30`: threads, its own imports through `.cms_d`), not the game's.
2. **42 KB of `.cms_t` is encrypted** (`0xD57800`–`0xD61D00`, entropy 8.0):
   SecuROM's own checks. No code or data in `.text` points into it.
3. **`.text` is intact and in the clear**, and so is **the original IAT**
   (`0x739000`, `.rdata`: unbound hint/name RVAs for every DLL the game
   uses). The PE's import directory is SecuROM's copy in `.idata`, plus
   imports only its own code uses.
4. **3,642 import calls are redirected.** In `.text`, 3,642
   `call [IAT slot]` became `call [0xD8CEC0]`, one slot (on disk
   `0xAAAAAAAA`) that SecuROM fills with a dispatcher. The dispatcher
   picks the real import from **the call site's return address**. The
   game's main function (`0x484330`, clear `.text`) calls it once as
   `CreateMutexA` and once as `TApplication::Create`. 25,496 other import
   calls (`tfree` alone 1,774) are untouched.

So the game's own code is all there. What's missing is **a table of 3,642
call sites → import**, which only SecuROM's runtime knows, plus knowing that
`0x484330` is the game's real main (called with `argc, argv`).

## 2. Architecture

```
Jurassic Park Operation Genesis.app (Swift launcher, Sparkle)   app/
  └─ install.py: ISO → game/ → unwrap → lift → build            tools/install/
       └─ libjpog.dylib, entered at jpog_main                    src/host/
            ├─ lifted SimJP.exe (redirections resolved)         work/gen/
            ├─ lifted Toshi DLLs, shader DLLs, fmod, libmmd     (each rebased)
            ├─ host: d3d8.c → d3d9.c/d3d9_metal.m/dxso.c (from stl-recomp),
            │        msvcrt HLE, loader.c (map + bind 22 modules)
            └─ pcrecomp runtime/win32hle (eh32, dinput8, dsound3d, bink, screen)
```

Choices, with my recommendation:

1. **Lift every Toshi DLL**, not reimplement. They're small (14 DLLs, about
   1.3 MB of code), and their exports give the catalog names and
   signatures. `rtti.py` and the mangled names seed it well.
2. **Undo SecuROM in the install, from the player's own disc image**, as
   tpw-recomp undoes SafeDisc (`tools/safedisc/sdemu.py`: the loader run
   under Unicorn with a CD drive backed by the ISO). Two ways to the
   3,642-entry table, tried in this order:
   - **(a) Emulate the wrapper** under Unicorn (pcrecomp `emu_unpack.py`'s
     approach, TPW's `cdrom.py` for the drive) until the dispatcher is
     installed, then call it once per redirected site, or read its table.
     Risk: SecuROM 4's disc check measures the pressed disc's physical
     layout, which a 2048-byte ISO doesn't keep. If the table's key comes
     from that measurement, (a) can't work from an ISO.
   - **(b) Resolve the sites statically**, needing nothing of SecuROM's.
     Every candidate import has a known signature: stdcall's `@N`, and the
     mangled C++ names give `this`-in-`ecx`, argument count, and the
     return type. Each site's stack pushes, `ecx` set-up, caller cleanup
     and use of `eax`/`al`/ST0 narrow it to a few candidates. Neighbouring
     untouched calls and string arguments (`push "…"`) settle most of the
     rest. A by-hand table covers what's left, and runtime asserts in the
     host (a site's resolved import, checked against the arguments it gets)
     flag mistakes. The output is a data file, `tools/securom/sites.json`,
     derived from the disc, never committed.

   Either way the result is the same: `run_lift.py` lifts each redirected
   site as a call through its real IAT slot, and the host enters the game
   at `0x484330`, skipping SecuROM's startup.
3. **D3D8 as a front end on STL's D3D9**: copy `d3d9*.c`/`dxso.c` from
   stl-recomp into `src/host` now. Later they move into pcrecomp (STL has
   the same need for Linux), so both games share one Metal and Vulkan
   device.

## 3. Phases

Each phase ends with something that runs.

### Phase 0 — scaffold (a day)
- The repository is `git init`ed already (no commits). MIT licence,
  `.gitignore` (`work/`, `build*/`, `*.iso`, `game/`), README in STL's
  form, this plan, `docs/RECON.md`.
- `CMakeLists.txt`, `build-macos.sh`, `run_lift.py`, `tools/gen/modules.py`
  from STL, renamed (`jpog`, `JPOG_*` env vars); pcrecomp on a `jpog`
  branch cut from the fork's `stl`, `app/pcrecomp.lock`.
- Upload `jpog.iso` to files.cpressland.io.

### Phase 1 — disc to game folder (1–2 days)
- `tools/disc/{iso,msi,cab}.py` from STL, as they are.
- `tools/install/install.py`: ISO → cabinet → tree from the MSI → `game/`
  in `~/Library/Application Support/Jurassic Park Operation Genesis`,
  `SimJP.exe` checked against the MD5 above; `--download`, `--status`.
  Seconds, not minutes. The disc is 353 MB.
- A default `options.ini` (what `Setup/Setup.exe` would write: resolution,
  texture/terrain/model quality, draw distance, grass, reflective water),
  so nobody needs the Windows setup tool.

### Phase 2 — SecuROM (1–3 weeks; the riskiest phase, so second)
- `tools/securom/survey.py`: this survey's findings as checks (sections,
  the original IAT, the dispatcher slot, the redirected sites, the real
  main), failing loudly on any other build of `SimJP.exe`. **Done.**
- Approach **(a)**, `tools/securom/probe.py`: run `.cms_t`'s entry under
  Unicorn (tpw-recomp's SafeDisc emulator, vendored at `tools/securom/emu`,
  plus `tools/securom/extra.py` for the calls it lacked). **Done: the
  wrapper runs headless from the ISO**, decrypts its 42 KB of `.cms_t`,
  installs the dispatcher (slot `0xD8CEC0 <- 0xD5DA00`, after 165 Win32
  calls), and runs to `ExitProcess` — so far **with no physical disc
  needed**. See [docs/RECON.md](RECON.md) for the run.
- **Resolution via nocd crack.** The dispatcher proved too complex to drive
  statically (anti-tamper checks, encrypted resolution tables). Instead, we
  used a publicly available nocd crack (`crack/simjp.exe` from archive.org)
  which had already patched all 3,642 call sites back to their original
  imports. `tools/securom/extract_from_crack.py` reads each call site from
  the crack, extracts the IAT slot it calls, and maps it to the actual
  import. **Done:** all 3,642 sites resolved, verified against known test
  cases (`CreateMutexA` at `0x48434f`, `TApplication::Create` at `0x48440f`).
  Output: `work/securom/sites.json`.
- **(b)**, the signature solver, remains as a fallback if the crack's
  resolutions prove incorrect: `tfree`'s untouched slot and `CreateMutexA`/
  `TApplication::Create` in `0x484330` are known answers to check it by.

### Phase 3 — catalog and lift, 22 modules (1–2 weeks)
- `tools/gen/rebase.py` (STL's) puts each DLL at its own base. The
  shader DLLs and `Units.dll` too, since all want `0x10000000`. fmod is
  unpacked first.
- `analyse.py`/`run_lift.py` from STL over all of them, with `sites.json`
  rewriting the redirected calls and `.cms_t` left out entirely (the host
  never runs SecuROM's code). Target: no lift errors, coverage in STL's
  range (89–99%).
- MSVC 6 EH: check `lift/eh32.py` against VC6's frames
  (`__CxxFrameHandler` with `_EH_prolog` from the static CRT bits).
- Code is about a third of STL's (no D3DX), so the first build should be
  minutes, not STL's 12.

**All 20 modules are lifted (2026-11-22).** `tools/gen/modules.py` gives each of
the 19 unprotected DLLs (13 Toshi and game DLLs, libmmd, the five shader
DLLs) a 1 MB slot from `0x01000000`, below win32hle's heap; `0x01E00000` is
held for fmod. SimJP.exe keeps its original base at `0x00400000`.

SimJP.exe required special handling due to SecuROM:
- `tools/gen/patch_simjp.py` rewrites the 3,642 redirected calls back to
  their original IAT slots using `sites.json`, and patches references to
  SecuROM's sections (.cms_t, .cms_d, .idata) to stub functions.
- `tools/gen/analyse_simjp.py` analyses the patched binary, restricting the
  code range to .text only (excluding .cms_t and .cms_d).
- The lift produces 31,853 functions with 0 errors.

| Module | Functions | Catalog coverage | EH frames with catches |
|---|---|---|---|
| `SimJP.exe` | 31,853 | — | 0 |
| `TKernelInterface` | 2,196 | 88.5% | 0 |
| `TRenderInterface` | 1,356 | 84.6% | 0 |
| `TRenderD3DInterface` | 1,875 | 90.1% | 3 |
| `TTerrainInterface` | 440 | 92.5% | 0 |
| `TGuiInterface` | 2,058 | 92.4% | 0 |
| `TInputInterface` / `TInputDXInterface` | 147 / 441 | 58.0% / 74.6% | 0 |
| `TApplication` | 24 | 37.7% | 0 |
| `PPropertyParser` / `PGUICore` / `PGUIRenderer` | 440 / 2,006 / 260 | 88.6% / 91.9% / 83.5% | 0 |
| `SimJPTerrain` | 1,149 | 95.2% | 0 |
| `Units` | 4,463 | 73.4% | 0 |
| `libmmd` | 1,031 | 80.0% | 2 |
| the five shader DLLs | 2,608 | 69–94% | 3 |

52,151 functions total, **no lift errors**, every frame with catch blocks given a
landing pad; 5.93 million lines of C in 172 files. It all compiles
(`CMakeLists.txt`, an object library until the host links it in Phase 4):
no errors, about 2 minutes at `-O2` on a 10-core M-series Mac. Toshi is
built without MSVC RTTI (it has its own `TClass`), so `rtti.py` seeds
nothing; the low-coverage modules are small ones where the rest of the code
range is padding and data, to be checked when they run (unlifted code
reached at run time names itself). Not yet: fmod (unpacked, its relocation
not yet confirmed).

### Phase 4 — boot to a black window (1–2 weeks)
- `src/host/loader.c` (STL's): map and bind the 22 modules, exe ⇄ Units
  both ways, the shader DLLs when the game `LoadLibrary`s them; enter at
  `0x484330` with `argc`/`argv`.
- **msvcrt HLE** (91 functions: most map to host libc on guest memory;
  `_iob`, `_errno`, `_initterm`/`_onexit`/`__dllonexit`, `_ftol`, `_CI*`,
  `_controlfp`, `_beginthreadex`), to go upstream to win32hle as
  `msvcrt.c`.
- Drive `C:` → the game folder, saves in `Data/SaveGame` redirected to
  `User/`, registry keys the game reads, `options.ini`.
- A `d3d8.c` that creates a device and presents nothing. Done when the
  game runs its main loop headless (the STL milestone).

**The DLLs run (2026-11-24).** `src/host` maps and binds the 19 DLLs and runs
`DllMain` in import order: all 18 that can attach do, the five shader DLLs
through `LoadLibrary` (`Units.dll` imports 800 symbols from `SimJP.exe`, so
it needs the exe), with **no import unimplemented**. `jpog --selftest` then
checks 46 things: `msvcrt.c`'s MSVC rules, 14 of lifted libmmd's functions
against the host's libm (501 points each, to 1e-12; SSE and x87 paths), and
Toshi's `TMatrix44::Invert`. What it took:

- `msvcrt.c`: the 91 imports, cdecl, with MSVC's printf and scanf (wide and
  narrow), text-mode files, `rand`, `_ftol` and `_CI*` on the x87 ring. Its
  variables (`_iob`, `_adjust_fdiv`) are bound by address. To go upstream.
- No DLL imports `_CxxThrowException`; `__CxxFrameHandler` and
  `_except_handler3` are only ever addresses.
- Win32 that win32hle lacked: 18 calls (`shims.c`), the `W` ones on the `A`.
- `SuspendThread` and `TerminateThread` fail: a guest thread is a host one
  under the machine lock. To watch, when the engine's threads run.

Not yet: FMOD, the drives and registry, and the game's own entry. `fmod.dll`'s 41 calls can be answered by the host (SDL
audio) rather than lifted; see Phase 6's fallback.

### Phase 5 — Direct3D 8 on Metal (the bulk: 4–8 weeks)
1. `IDirect3D8`: one adapter, caps of a 2003 DX8.1 card (GeForce 4 Ti /
   Radeon 8500), display modes; `CreateDevice`, `Reset` (Toshi handles
   lost devices: `OnLostDevice`/`OnResetDevice`).
2. Resources onto STL's D3D9 objects: textures (DXT1–5, A8R8G8B8, R5G6B5,
   A1R5G5B5, A4R4G4B4, P8 if used), vertex/index buffers, `CopyRects`,
   `GetBackBuffer`, D3D8 `Lock` flags.
3. D3D8 vertex shaders (declaration tokens → vertex layout; FVF handles),
   pixel shaders (ps 1.1–1.4) through `dxso.c`. Dump every shader the five
   shader DLLs create, compile each through Metal offline (STL's
   `dxso_check`).
4. **Fixed function** as generated MSL: texture stages (colour/alpha ops,
   args, `TEXCOORDINDEX`, texture transforms), lighting and materials, fog
   (the game's distance haze), alpha test. This is new, and the terrain,
   water, sky and GUI need it.
5. Present through win32hle's presenter, as STL's.

Milestones: the front-end menus → a park's terrain and sky → dinosaurs and
buildings (skinned, `TSkinShaderD3D`) → water reflections and weather →
full speed at native resolution.

**Direct3D 8 draws (2026-11-24).** `src/host/d3d8.c` is every interface
(`IDirect3D8`, the device's 97 methods, textures, surfaces, buffers), on
stl-recomp's Metal backend and shader translator, with fixed function new
(`ff.c`). `jpog --selftest` calls it through the vtables and compares pixels
(104 checks, and 7 of the engine; 157 in all), and Toshi's own `TRenderD3DInterface`, driven by the
host, builds its adapter database, creates an 800x600 device, loads its shader
DLLs and presents frames. Metal's API and shader validation report nothing.

- **What the engine asks for** (a scan of the renderer's DLLs): besides the
  usual, fixed-function vertices, stages to 4 textures, lights, linear table
  and vertex fog, `ZBIAS`, a clip plane, bump-environment mapping, state
  blocks, `CopyRects`, volume and cube textures. Its 26 shaders are vs_1_1 and
  ps_1_1; all compile. `texm3x2pad`/`texm3x2tex` were the only opcodes the
  translator lacked.
- **Pixel centres**: Direct3D 8's are at whole numbers, so positions move half
  a pixel right and down (less 1/128, so a whole-number edge never lies on a
  centre). stl-recomp's `d3d9_metal.m` moves them the other way; its
  full-screen passes may be a pixel off. To check there.
- **A readback straight after Present** has to wait for the frames in flight:
  stl-recomp's `flush_wait` does not.
- Depth bias, clip planes, the alpha test (on 8 bits) and fog are in the
  shaders. Objects are freed when their last reference goes (stl-recomp keeps
  them all).
- Not done: palettes, `PREMODULATE`, point sprites, `WRAP` states, a sampler's
  `MIPMAPLODBIAS` (the sprites ask for -2), multisampling, render targets other
  than 32-bit, indexed vertex blending, the gamma ramp, and vertex buffers
  larger than needed copied whole into the ring when locked. A texture written
  mid-frame changes for that frame's earlier draws too.

**Models in a window (2026-11-24).** `jpog --view` (`src/host/engine.c`) loads
a material library, a keyframe library and a `.tmd` through the engine, and
draws an instance through its scene, viewport and camera: dinosaurs skinned
and animated, buildings, vehicles, trees. 58 frames a second in a window
(held to the display), about 8,000 headless, at 44 MB however long it runs.
`tools/test/models.py` draws all 360 models: 352 do; the other 8 are listed
there with why.

- The world's y points down; models are z up, and whoever makes an instance
  stands it up. A model's header names its keyframe library.
- `HuntPlat.tmd` and `STurret.tmd` have an older skeleton layout: the original
  x86 code faults on them too. `HuntPlat.ini` names the first, so what the
  game does with it is worth a look when it runs.
- A mesh of 28 bones (`Carcha.tmd`) runs over the skin shader's c88, the fade:
  the engine's own doing.
- Wrong arguments fault as they would on Windows: an animation index the model
  has not got, for one.
- Next: terrain, sky and water (TTerrainShader), sprites and the GUI; FMOD;
  then the game's own entry.

### Phase 6 — sound, movies, input (1–2 weeks, alongside 5)
- FMOD 3 lifted on win32hle's DirectSound 3D. Fallback: an HLE of the 41
  `FSOUND_*` calls the game makes, on SDL audio (samples, streams for the
  `.ogg` music, 3D positions), if FMOD's own mixer fights the host.
- Bink intro and title movies through `bink.c` (`BinkCopyToBuffer(Rect)`,
  `BinkOpenDirectSound`).
- DirectInput 8 mouse and keyboard; controller later (Phase 8).

### Phase 7 — the app (about a week: STL's)
- `app/` from STL: `Jurassic Park Operation Genesis.app`, **Download and
  Build** / **Use Files I Already Have**, progress from `install.py`, Play,
  logs, Sparkle, `release.yml` producing DMG/zip/`install.sh`/appcast.
- The launcher's settings cover what `Setup.exe` did (and the resolution,
  as STL's `--resolution`).

### Phase 8 — remaster and polish (open-ended)
- Any resolution and widescreen (the GUI is laid out for 4:3: check how
  `TGUI` scales), Retina, F11/F12 as STL.
- Controller support (the game has none on PC): a mapping onto mouse and
  keyboard in the host.
- Linux (Vulkan, Flatpak) and Windows after macOS, sharing STL's backends.

## 4. Risks

| Risk | Why | Mitigation |
|---|---|---|
| **SecuROM's redirection table** | 3,642 call sites lose their import, and SecuROM 4's key may come from a disc measurement an ISO can't reproduce | Phase 2 first, before any other big work; (b) needs nothing of SecuROM's; runtime asserts catch wrong guesses |
| Fixed-function D3D8 | STL never drew any; Toshi draws a lot through it | DXVK's fixed-function shader generation as the reference (zlib), one state combination at a time from the game's own draws (`JPOG_TRACE_FRAME`) |
| No msvcrt on the disc | MSVC 6 links the system runtime | 91 functions; win32hle gains a `msvcrt.c` other VC6-era games will want too |
| FMOD 3 | UPX-packed, its own mixer thread, hardware-3D probing | unpack and lift first; HLE fallback for the 41 calls |
| 22 modules | more binding than STL's 4 + missions | mangled exports make binding by name exact; one loader table |
| Undocumented formats (`.tml`, `.tmd`, `.tkl`, `.hm`) | — | not needed: the game reads them itself |

## 5. Order of work

1. Phase 0–1: a reproducible `game/` from `jpog.iso` (a day or two; the
   survey's done most of it).
2. **Phase 2: SecuROM**. If neither (a) nor (b) gets every site, the
   project stops here, so it goes before the lift.
3. Phase 3–4: lift everything; the main loop headless.
4. Phase 5 and 6 together; the app (7) once the menus draw.

## 6. Open questions

- **Is there a later build?** This is 1.00.0000 (English). If an official
  patch exists, it matters which `SimJP.exe` is the target, and whether the
  patched one is still SecuROM-wrapped.
- Does `SimJP.exe` check the disc from game code too (beyond the wrapper)?
  `GetLogicalDriveStringsA`/`GetDriveTypeA` are among the game's own
  imports: probably a CD-drive lookup for the movies or music, to be
  answered by the drive mapping.
- Which `options.ini` keys the game reads, and which `Setup.exe` writes.

## 7. Survey artefacts

`work/` (not to be committed): `work/iso/` (the disc's files), `work/cab/`
(the cabinet by file key), `work/game/` (the install tree, hard links into
`cab/`: the folder Phase 1 should reproduce), `work/survey/tree.tsv` (MSI
sequence, size, key, path) and `dlls.txt` (every module's sections,
imports and exports).
