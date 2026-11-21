# JPOG recon: the disc, the binaries, the SecuROM wrapper

The facts the tools and the plan rest on, from surveying `jpog.iso`
(2026-11-21). Nothing here is generated from the game and committed; the
`work/` artefacts these come from are rebuilt by the tools.

## The disc

`jpog.iso` (337 MB, MD5 `eee1e2577237ba032b93ed37ed43ede4`): a plain ISO 9660
CD, 17 files. An InstallShield 8 MSI install, the same shape as stl-recomp's:

- `Jurassic Park Operation Genesis.msi` (31 MB): ProductVersion 1.00.0000,
  English (1033), Manufacturer "Universal Interactive". Its `File`,
  `Component` and `Directory` tables place 2,420 files.
- `Data1.cab` (288 MB): one cabinet (not spanned), all folders MSZIP, each
  file named by its MSI key (`_0EF31B2F…`).
- `DirectX81/`, `setup.exe`, `Autorun.exe`, `splash.BMP`: not installed.

`tools/disc/{iso,msi,cab}.py` (stl-recomp's, unchanged) extract the lot.
`tools/install/install.py` writes the game folder, **467 MB, 2,420 files**,
identical to the by-hand survey tree. `SimJP.exe` MD5
`6cc8989f299f64524577915a4a0d91b0`.

## The modules

Blue Tongue's **Toshi** engine, MSVC 6 (linker 6.0), across 19 DLLs that
export C++ classes by mangled name, plus the game and three third-party DLLs:

| Module | Base | Notes |
|---|---|---|
| `SimJP.exe` | `0x400000` | the game, SecuROM-wrapped (below); also **exports 15,439** symbols `Units.dll` imports back |
| `T{Kernel,Render,RenderD3D,Terrain,Gui,Input,InputDX,Application}Interface.dll`, `SimJPTerrain.dll`, `Units.dll`, `PGUICore.dll`, `PGUIRenderer.dll`, `PPropertyParser.dll` | `0x10000000` (each rebased) | Toshi; relocatable |
| `Shaders/D3D/T{Line,Skin,Sprite,Sys,Terrain}ShaderD3D.dll` | `0x10000000` | materials + D3D8 shader bytecode (vs 1.0/1.1, ps 1.1); `LoadLibrary`-loaded |
| `fmod.dll` | `0x10000000` | FMOD 3.x, **UPX-packed** (UPX0/UPX1) |
| `libmmd.dll` | `0x10000000` | Intel maths, SSE2 paths by CPUID |
| `binkw32.dll` | `0x30000000` | Bink 1 movies |

The renderer calls **`Direct3DCreate8`** (`TRenderD3DInterface.dll` imports
`d3d8.dll`): Direct3D 8, not 9. No `msvcp60`; the C++ library is the engine's
own. Only **91** distinct `msvcrt.dll` functions are imported across all
modules (`__CxxFrameHandler`, `_except_handler3`, `_setjmp3`/`longjmp`,
`_beginthreadex` among them).

## The SecuROM wrapper on `SimJP.exe`

Sections: `.text PSGSFD00 PSGSFD_I PSGSFD_P PSGSFD_B .rdata .data .data1
.cms_t .cms_d .idata .rsrc`. `.cms_t`/`.cms_d` are SecuROM's; `.cms_d` holds
its messages as `7001…` hex strings and a Petite-packed loader DLL. What the
survey (`tools/securom/survey.py`) establishes:

- **The entry point is in `.cms_t`** (`0xD7EF8A`), clear code: the MSVC CRT
  start-up, calling SecuROM's own WinMain (`0xD61D30`), not the game's.
- **42 KB of `.cms_t` is encrypted** (`0xD57800`–`0xD61D00`, entropy 8.0).
  Nothing outside `.cms_t` points into it.
- **`.text` is the game, in the clear.** The **original IAT** survives at
  `0x739000` in `.rdata`: unbound, each slot the RVA of a hint/name, in one
  block per DLL — 1,108 imports across 21 DLLs, cleanly attributable.
- **3,642 of the game's `call [IAT slot]` are `call [0xD8CEC0]`**: one slot
  (`0xAAAAAAAA` on disk) SecuROM fills with a dispatcher that picks the real
  import from the call's return address. 25,496 other IAT calls are
  untouched (`?tfree@@YGXPAX@Z` alone 1,774).
- **The game's own main is `0x484330`**, `int main(int argc, char**)`-shaped:
  it calls the dispatcher once as `CreateMutexA` (`+0x1F`) and once as
  `TApplication::Create` (`+0xDF`), then `TApplication::Execute` — two
  answers to check a resolver by. SecuROM's start-up never needs to run; the
  host enters here.

## Emulating the wrapper (approach a)

`tools/securom/probe.py` runs `.cms_t`'s entry under Unicorn, using
tpw-recomp's SafeDisc emulator (`tools/securom/emu`, vendored unchanged) with
drive D: the disc and C:\JPOG the game folder, and `tools/securom/extra.py`
for the ~30 Win32 calls that emulator lacked (security-token, resources,
multimedia timer, string helpers, `GetModuleFileNameA` → the real path).

Two things the emulator needed for SecuROM specifically, both in `extra.py`:

1. **The trap flag cleared on exception entry.** SecuROM's decryptors
   single-step through SEH; the emulator left `TF` set in the handler's
   CONTEXT, so the handler stepped itself. Cleared on `save_context` and on
   entry to each handler, as Windows does.
2. **Division-by-zero raised before the `idiv` runs.** The decryptors `idiv`
   by zero on purpose (every fifth byte of the run) for a handler to supply
   the divisor and resume. Unicorn's `#DE` double-faults before the handler;
   instead each `div`/`idiv` in `.cms_t` is guarded, and a zero divisor
   raises `EXCEPTION_INT_DIVIDE_BY_ZERO` at the instruction.

**Result:** in 192 Win32 calls the wrapper decrypts the 42 KB of `.cms_t`
(into itself via `ReadProcessMemory`/`WriteProcessMemory` on
`GetCurrentProcess`), **installs the dispatcher** (`[0xD8CEC0] <- 0xD5DA00`,
after 165 calls), reads its own overlay at file offset `0x582000`, and runs
to `ExitProcess` — **with no physical disc, no `DeviceIoControl` to the CD,
no weak-sector read**. The disc check it then does (dropping `SIntf*.dll`
into TEMP) comes *after* the dispatcher is already in place.

The decrypted dispatcher (`0xD5DA00`) is a small **jump-table VM**
(`jmp [eax*4+0xD5E7CD]`) that resolves the import from the return address
behind anti-tamper and SEH. So resolving the 3,642 sites statically is
impractical; the resolver drives this dispatcher per site instead
(PLAN.md Phase 2). The decrypted image is dumped to
`work/securom/simjp-decrypted.bin` for analysis.
