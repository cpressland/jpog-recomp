"""The modules lifted, where each is mapped, and which build of each.

Bases are fixed (a lifted module's code names its own addresses) and must
not overlap. SimJP.exe keeps its own (0x00400000 to 0x00E1D000) and is not
in this table yet: it goes in as a separate step (docs/PLAN.md). Every DLL
of the game wants 0x10000000, where win32hle puts the guest's heap, so each
moves (tools/gen/rebase.py) to a 1 MB slot of its own from 0x01000000, above
the game's image and below the heap, so the host can map every module before
the first allocation. 0x01E00000 is held for fmod.dll, which is UPX-packed
and joins once it is unpacked (tools/gen/unupx.py).

The five shader DLLs (Shaders/D3D) are loaded by the renderer with
LoadLibrary, not at the start: ON_DEMAND. binkw32.dll is not lifted; the host
answers its calls (pcrecomp's bink.c on ffmpeg).
"""

# name, file in the game folder (/ between folders), base, PE timestamp of the build supported
MODULES = (
    # The game itself (SecuROM-wrapped, but resolved: docs/PLAN.md Phase 2)
    ('simjp',        'SimJP.exe',               0x00400000, 0x3E434C37),
    # Toshi, Blue Tongue's engine, and the game's own DLLs (disc 1.00, Feb 2003)
    ('tkernel',      'TKernelInterface.dll',    0x01000000, 0x3E434BA1),
    ('trender',      'TRenderInterface.dll',    0x01100000, 0x3E434BB9),
    ('trenderd3d',   'TRenderD3DInterface.dll', 0x01200000, 0x3E434BBC),
    ('tterrain',     'TTerrainInterface.dll',   0x01300000, 0x3E434BBE),
    ('tgui',         'TGuiInterface.dll',       0x01400000, 0x3E434BAF),
    ('tinput',       'TInputInterface.dll',     0x01500000, 0x3E434BA6),
    ('tinputdx',     'TInputDXInterface.dll',   0x01600000, 0x3E434BBA),
    ('tapplication', 'TApplication.dll',        0x01700000, 0x3E434BB6),
    ('pproperty',    'PPropertyParser.dll',     0x01800000, 0x3E434BA7),
    ('pguicore',     'PGUICore.dll',            0x01900000, 0x3E434BB2),
    ('pguirenderer', 'PGUIRenderer.dll',        0x01A00000, 0x3E434BC3),
    ('simjpterrain', 'SimJPTerrain.dll',        0x01B00000, 0x3E434BBF),
    ('units',        'Units.dll',               0x01C00000, 0x3E434D25),
    # Intel's maths library (sin, pow, ... with SSE2 paths chosen by CPUID)
    ('libmmd',       'libmmd.dll',              0x01D00000, 0x3B297748),
    # The renderer's materials and their D3D8 shaders, loaded on demand
    ('s_line',       'Shaders/D3D/TLineShaderD3D.dll',    0x02000000, 0x3E434D26),
    ('s_skin',       'Shaders/D3D/TSkinShaderD3D.dll',    0x02100000, 0x3E434BC0),
    ('s_sprite',     'Shaders/D3D/TSpriteShaderD3D.dll',  0x02200000, 0x3E434D26),
    ('s_sys',        'Shaders/D3D/TSysShaderD3D.dll',     0x02300000, 0x3E434BBD),
    ('s_terrain',    'Shaders/D3D/TTerrainShaderD3D.dll', 0x02400000, 0x3E434BC2),
)

# Modules the game loads itself, by LoadLibrary, rather than at the start.
ON_DEMAND = {m[0] for m in MODULES if m[1].startswith('Shaders/')}


def by_name(name):
    for m in MODULES:
        if m[0] == name:
            return m
    raise KeyError(name)


def check():
    """No two modules overlap, and each fits below the heap."""
    import os
    import pefile
    game = os.environ.get('JPOG_GAME', 'work/game')
    spans = []
    for name, file, base, _ in MODULES:
        size = pefile.PE(os.path.join(game, file), fast_load=True).OPTIONAL_HEADER.SizeOfImage
        spans.append((base, base + size, name))
    spans.sort()
    for (a0, a1, an), (b0, _, bn) in zip(spans, spans[1:]):
        assert a1 <= b0, f'{an} overlaps {bn}'
    assert spans[0][0] >= 0x00400000 and spans[-1][1] <= 0x10000000, 'outside 0x00400000..0x10000000'
    print(f'{len(MODULES)} modules, no overlaps, 0x{spans[0][0]:08X}..0x{spans[-1][1]:08X}')


if __name__ == '__main__':
    check()
