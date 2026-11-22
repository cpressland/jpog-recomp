"""Unpack a UPX-compressed DLL at the base it will be lifted at.

    .venv/bin/python -I tools/gen/unupx.py game/fmod.dll 0x01600000 work/analysis/fmod/fmod.dll

`fmod.dll` (FMOD 3) ships compressed with UPX 1.02 (NRV2B, with UPX's
call/jump filter): its code is not in the file until its stub has run.
pcrecomp's tools/drm/emu_unpack.py runs such a stub under Unicorn and
rebuilds the PE from the result, but it calls the entry point as an EXE's,
with nothing on the stack. A DLL's UPX stub first checks DllMain's
`fdwReason` and decompresses only for DLL_PROCESS_ATTACH, so here the
stub is entered as Windows enters a DLL: `DllMain(hinstDLL, 1, NULL)`.

The base. A lifted module's code names its own addresses, so fmod is lifted
where the host will map it. The packed file is first moved to that base
(its stub's own relocations, tools/gen/rebase.py), and the stub, finding
itself loaded away from 0x10000000, applies the original code's
relocations as it unpacks, as on Windows. The result is fmod as it is in
memory at that base, with an import table rebuilt from the calls the stub
made to GetProcAddress, and no relocations (it does not move again).

--check unpacks at two bases and checks that the images differ only by
relocated pointers (each dword off by exactly the bases' difference), so
the stub did relocate the code.
"""
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)


def _pcrecomp_drm():
    root = os.environ.get('PCRECOMP', os.path.join(os.path.dirname(REPO), 'pcrecomp'))
    sys.path.insert(0, os.path.join(root, 'tools', 'drm'))
    import emu_unpack
    return emu_unpack


from rebase import rebase  # noqa: E402

RETURN = 0x5F0FFF00          # where DllMain "returns" to: never reached (the OEP fetch stops first)


def unpack(src, base, dst):
    """Unpack src as loaded at base; write the rebuilt PE to dst. Returns (oep, runs)."""
    eu = _pcrecomp_drm()
    from unicorn.x86_const import UC_X86_REG_ESP

    class DllUnpacker(eu.Unpacker):
        def map(self):
            super().map()
            esp = eu.STACK + eu.STACK_SIZE - 0x100
            for k, v in enumerate((RETURN, self.base, 1, 0)):     # ret, hinstDLL, DLL_PROCESS_ATTACH, lpReserved
                self._write32(esp + 4 * k, v)
            self.uc.reg_write(UC_X86_REG_ESP, esp)

    with tempfile.TemporaryDirectory() as tmp:
        moved = os.path.join(tmp, os.path.basename(src))
        rebase(src, base, moved)
        u = DllUnpacker(moved)
        oep = u.run()
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        runs = u.write(dst)
    return oep, runs


def check(src):
    """Two bases a whole number of 64 KB apart: every differing dword must be
    off by exactly that, or the stub did not relocate what it unpacked."""
    import pefile
    a, b = 0x01000000, 0x02340000
    with tempfile.TemporaryDirectory() as tmp:
        pa, pb = os.path.join(tmp, 'a.dll'), os.path.join(tmp, 'b.dll')
        unpack(src, a, pa)
        unpack(src, b, pb)
        ia = pefile.PE(pa).get_memory_mapped_image()
        ib = pefile.PE(pb).get_memory_mapped_image()
    n = min(len(ia), len(ib))
    moved = bad = 0
    delta = b - a
    for off in range(0x1000, n - 3, 4):
        x, y = struct.unpack_from('<I', ia, off)[0], struct.unpack_from('<I', ib, off)[0]
        if x == y:
            continue
        if (y - x) & 0xFFFFFFFF == delta:
            moved += 1
        else:
            bad += 1
    return moved, bad


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--check':
        moved, bad = check(sys.argv[2])
        print(f'{moved} pointers relocated, {bad} other differences')
        sys.exit(0 if moved and not bad else 1)
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    oep, runs = unpack(sys.argv[1], int(sys.argv[2], 0), sys.argv[3])
    n = sum(len(v) for v in runs.values())
    dlls = sorted({e[0] for v in runs.values() for e in v})
    print(f'OEP 0x{oep:08X}; {n} imports from {len(dlls)} DLLs ({", ".join(dlls)}); wrote {sys.argv[3]}')


if __name__ == '__main__':
    main()
