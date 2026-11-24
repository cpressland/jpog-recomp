# Brief: the game's own start-up, to its menu

For whoever does `SimJP.exe`'s side. **Goal:** `jpog --run` draws the game's
front end. Everything below the exe works without it (see `--park`); `--run`
reaches the engine's main loop and nothing happens in it, for the reasons here.

Facts are marked **[checked]** (in the files, or by a test) or **[to check]**.
Addresses are `SimJP.exe`'s own (base `0x400000`) unless a DLL is named.

## 1. Why the loop is empty

`src/host/game.c` calls `TApplication`'s constructor on `0x8B8580`, then
`0x484330` (main).

1. **The C runtime's start-up is skipped**, so no global object is constructed.
   `.data` has `0x16000` bytes in the file, to `0x84B000`; `0x8B8580` is past
   them **[checked]**, so it is zero until a static constructor fills it.
2. **The object is the wrong class.** The game's is `AApplication`, which
   derives from `Toshi::TApplication`. Its constructor is `0x466070`
   (`??0AApplication@@QAE@XZ`) and its vtable `0xCE6C40`
   (`??_7AApplication@@6B@`) **[checked: exports]**. The base's `OnUpdate`
   returns at once.
3. **The constructor is called without the machine.** `game.c` writes `g_esp`
   and `g_ecx` and calls lifted code with no `mach_enter()`. The thread then has
   no claim on the registers, and works only on what the last guest call left.
   `hle_call_guest` takes it by itself, but does not set `ecx`.
4. `main` is given no `envp`, and its `argv` has no null at the end.

**Do not** fix 2 alone by swapping the constructor. Thousands of other globals,
`TClass` registrations among them, would still be missing.

## 2. What to do

### 2.1 Run the real start-up

Enter the function that calls main, not main. The msvcrt imports say it is
MSVC 6's `mainCRTStartup` **[checked: `__p___initenv`, no `_acmdln`]**:

```
SEH frame on fs:[0]; __set_app_type; __p__fmode; __p__commode; _adjust_fdiv
__setusermatherr (perhaps); _controlfp
_initterm(xi_a, xi_z)          C initialisers
__getmainargs(...)
_initterm(xc_a, xc_z)          C++ constructors: the globals
*__p___initenv() = envp
main(argc, argv, envp)         0x484330
exit(result)                   does not return
```

All of those calls are answered and tested by `--selftest` (22 checks), outside
the game. Its address is the function holding the call to `0x484330`, near the
two `_initterm` calls **[to check]**. Nothing in the game's code calls it, so it
may be missing from the catalog. If so, add it as an entry and lift again.

If it cannot be used, do the same by hand from `game.c`: `_initterm` over both
tables (their bounds are its arguments), then main with
`argc, argv, envp`. Say in `game.c` which was done and why.

Then delete from `game.c`: the constructor call, `TAPPLICATION_CTOR`,
`TAPPLICATION_OBJ`, the hand-made `argv`.

### 2.2 Check it before going on

| Check | Expected |
|---|---|
| `MEM32(0x8B8580)` when main starts | `0xCE6C40`, if the object is there **[to check: `ecx` at main's `Create` call says where it is]** |
| `_initterm` entries | both tables run to the end; log each count and the last entry begun |
| `_onexit` | below 4,096 (`MAX_ONEXIT`, `msvcrt.c`); above it returns 0, quietly |
| argc, argv | `1`, `C:\SimJP.exe`, null |

A crash inside a constructor is the likeliest first failure. Log the entry's
address before calling it; `sub_<address>` is the C function.

### 2.3 What should happen next

`TApplication.dll` (base `0x01700000`), **[checked: read from its code]**:

- `Create(name, argc, argv)` (`0x01701260`): stores the name, makes the kernel
  (0x58 bytes), then calls virtual `+4`, `OnCreate(argc, argv)`. Returns 0 if
  that does.
- `Execute` (`0x017011E0`): until the kernel's `Update` is not 1, or virtual
  `+8`, `OnUpdate(delta)`, returns 0, or bit 1 of `+0x10` is set. Then virtual
  `+0xC`, `OnDestroy`.
- `AApplication`'s vtable: `+0` destructor `0x465100`, `+4` `OnCreate`
  `0x4846C0`, `+8` `OnUpdate` `0x72FC86`, `+0xC` `OnDestroy` `0x484DC0`
  **[values checked in the file; names by the base's order]**.
- **Look at `0x72FC86` first.** The other three begin as functions do; the bytes
  there (`bc 52 b7 7e 6b f9 ...`) do not, and that is the game's every frame.
  Nothing more is known of it here: the exe's code is the other side's. If the
  lift has nothing sound at that address, start-up can succeed and the loop
  still not run.
- Object: `+0` vtable, `+4` kernel, `+8` name, `+0x10` flags.

The exe has 15,439 exports, about 8,000 of them named, so progress can be named. Put a breakpoint or a
log on these, in roughly the order expected **[addresses checked; order to
check]**:

| | Function | Address | Then look at |
|---|---|---|---|
| 1 | `ARootTask` constructor | `0x47A120` | |
| 2 | `AllocateRenderInterface` / `CreateRenderInterface` | `0x47C5E0` / `0x47C7E0` | `TRenderD3D` loading, adapters |
| 3 | `CreateDisplayDevice` | `0x47CEA0` | `[d3d8] CreateDevice` |
| 4 | `CreateARenderer` | `0x47CB80` | shader DLLs |
| 5 | `CreateInputSystem` | `0x47CBB0` | DirectInput |
| 6 | `CreateSoundSystem` | `0x47D1A0` | FMOD (`fsound.c`) |
| 7 | `CreateGUISystem` | `0x47CBE0` | `Data/GUI/main.gui` |
| 8 | `AMoviePlayer` constructor | `0x6D74B0` | Bink (`movies.c`) |
| 9 | `AFrontEndController` / `AFrontEndState` constructors | `0x484FF0` / `0x48A550` | |
| 10 | `FinishFrontEndLoading` | `0x47CC00` | the menu should draw |

## 3. Tools

- `--trace`: every import call. `--verbose`: stubs, Direct3D summary, input.
- `JPOG_SHOT=DIR JPOG_SHOT_EVERY=60`: pictures of what is presented. Absolute
  path: the host changes folder. `JPOG_TRACE_FRAME=N`: frame N's draws.
- `HLE_FILETRACE`, `HLE_REGTRACE`, `HLE_MSGTRACE`, `HLE_DITRACE`, `HLE_TRACE_EH`
  (set to 1).
- `lldb -- build/jpog --game GAME --run --windowed --headless`, then
  `b sub_0047C7E0`. `hle_fatal` aborts, so a backtrace names the culprit.
- `--frames` does nothing with `--run`. Worth adding first: count frames in
  `hle_screen_frame_hook` and `hle_exit(0)`, so a run can be bounded and
  tested.
- **Which side is at fault?** If a screen is wrong in `--run`, look for the
  same thing in `--park`, `--gui`, `--terrain`, `--view`, `--movie` or
  `--listen`. Right there and wrong in `--run` points at the exe's lift or
  start-up. Wrong in both is the host's: report it with the view's command.

## 4. Likely trouble

- `__CxxFrameHandler` and `_except_handler3` abort if reached as functions
  (`msvcrt.c`); `win32hle_eh32` does the unwinding. `HLE_TRACE_EH=1` first.
- `SuspendThread` and `TerminateThread` fail (`shims.c`).
- The mouse needs `docs/patches/pcrecomp-dinput-axismode.patch` until
  sp00nznet/pcrecomp#198 lands and `app/pcrecomp.lock` moves.
- `JPOG_CPU=x87` reports a CPU without SSE, to rule out a lifted SSE path.

## 5. Done when

1. `--run` starts through the C runtime, with the four checks of 2.2 logged.
2. The ten milestones are reached, or the first not reached is named with its
   backtrace.
3. `JPOG_SHOT` pictures show the front end, headless and `--windowed`.
4. A run is bounded (`--frames`) and exits 0 with no unimplemented import.
5. These still pass, on the DLL-only build (README, "The host"):

```
build-dlls/jpog --game GAME --selftest            # 277 checks, 0 failed
python3 tools/test/park.py   build-dlls/jpog GAME # 25 runs, 0 failed
python3 tools/test/gui.py    build-dlls/jpog GAME --locales # 0 failed, 1 known not to draw
python3 tools/test/terrain.py build-dlls/jpog GAME # 49 views, 0 failed
python3 tools/test/models.py build-dlls/jpog GAME # 352 of 360, 8 known not to
build-dlls/jpog --game GAME --listen all          # 928 played, 0 wrong
```

State what was seen, not what should be: "reached 6, aborted in X" is a good
result. Keep the log and the pictures.

## 6. Ground rules

- **Yours:** `game.c`, the exe's import patching in `loader.c`, the disc checks,
  `tools/securom`, the exe's analysis and lift.
- **Not yours without asking:** `d3d8.c`, `d3d_metal.m`, `dxso.c`, `ff.c`,
  `fsound.c`, `movies.c`, `engine.c`, `terrain.c`, `gui.c`, `park.c`,
  `msvcrt.c`, the tests. A missing call may be added to `shims.c`, with a check
  in `selftest.c`.
- No registry of our own: pcrecomp's is used, seeded in `main.c`.
- The game is never edited on disk, and no file of the game's, nor anything
  generated from it, is committed.
- Never rewrite published history (`filter-branch` lost 12 commits'
  signatures). Signed commits, one change each, not pushed unless asked.
- The shared pcrecomp checkout is not to be changed: use a clone
  (`-DPCRECOMP=`).
