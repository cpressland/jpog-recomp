"""Run SimJP.exe's SecuROM wrapper headless under Unicorn, and see how far it gets.

    .venv/bin/python -I tools/securom/probe.py --game GAME --iso jpog.iso --disc work/iso [-v]

Approach (a) of docs/PLAN.md: let SecuROM's own start-up run, with every
Win32 call answered in Python (tools/securom/emu, tpw-recomp's SafeDisc
emulator), drive D: the disc (its files, and raw sectors from the image),
C:\\JPOG the game folder. It stops at the first call it cannot answer, at a
fault it cannot raise, when the dispatcher slot is filled, or when code first
runs in the game's own `.text` (SecuROM handing over to the game).

What it reports: each API call (-v), when the dispatcher slot gets its value
and from where, and the stop. With --resolve, once the dispatcher is in
place, each redirected call site is called through it in turn, the API
reached recorded instead of run, and the table written as JSON.
"""
import argparse
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'emu'))
sys.path.insert(0, HERE)

import sdemu  # noqa: E402
import extra  # noqa: E402,F401  (the calls sdemu lacks)
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP  # noqa: E402

DISPATCH = 0xD8CEC0
TEXT = (0x464000, 0x733000)


def stage(root, game, disc):
    """The emulator's view: <root>/cd (drive D:) and <root>/cdrive (C:), with
    C:\\JPOG the game folder."""
    os.makedirs(root, exist_ok=True)
    for link, target in (('cd', disc), (os.path.join('cdrive', 'JPOG'), game)):
        p = os.path.join(root, link)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.islink(p):
            os.remove(p)
        os.symlink(os.path.abspath(target), p)
    os.makedirs(os.path.join(root, 'cdrive', 'WINDOWS', 'TEMP'), exist_ok=True)
    os.makedirs(os.path.join(root, 'cdrive', 'WINDOWS', 'SYSTEM32'), exist_ok=True)
    return os.path.join(root, 'cd')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', required=True, help='the game folder (install.py)')
    ap.add_argument('--iso', required=True, help='jpog.iso, for raw sector reads')
    ap.add_argument('--disc', required=True, help="the disc's files, extracted (tools/disc/iso.py)")
    ap.add_argument('--root', default='work/securom/emu', help='where the emulated drives are staged')
    ap.add_argument('--out', default='work/securom/dump')
    ap.add_argument('--resolve', help='write the call-site table here once the dispatcher is set')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--trace', help='LO-HI: log every instruction run in this range (after --trace-after API calls)')
    ap.add_argument('--trace-after', type=int, default=0)
    a = ap.parse_args()

    cd = stage(a.root, a.game, a.disc)
    emu = sdemu.Emu(cd, a.iso, a.verbose, name='simjp')
    emu.os.cmdline = 'C:\\JPOG\\SimJP.exe'
    emu.os.cmdline_a = emu.os._static(emu.os.cmdline.encode() + b'\0')
    emu.os.cmdline_w = emu.os._static(emu.os.cmdline.encode('utf-16le') + b'\0\0')
    emu.os.out_dir = a.out
    exe = os.path.join(a.root, 'cdrive', 'JPOG', 'SimJP.exe')
    mod = emu.load_pe(exe, main=True)
    emu.os.main_module = mod
    entry = mod['base'] + mod['pe'].OPTIONAL_HEADER.AddressOfEntryPoint

    seen = {}

    def on_dispatch_write(uc, access, addr, size, value, _):
        if addr <= DISPATCH < addr + size:
            v = struct.unpack('<I', bytes(uc.mem_read(DISPATCH, 4)))[0] if size < 4 else value & 0xFFFFFFFF
            seen['dispatch'] = (value & 0xFFFFFFFF, emu.reg(UC_X86_REG_EIP), emu.api_count)
            print(f'  >> dispatcher slot <- {value & 0xFFFFFFFF:#010x} from {emu.reg(UC_X86_REG_EIP):#010x} '
                  f'after {emu.api_count} API calls', flush=True)
    emu.uc.hook_add(UC_HOOK_MEM_WRITE, on_dispatch_write, begin=DISPATCH - 3, end=DISPATCH + 3)

    def on_text(uc, addr, size, _):
        if 'text' not in seen:
            seen['text'] = (addr, emu.api_count)
            emu.stop_reason = f'the game\'s own code runs: {addr:#010x}, esp {emu.reg(UC_X86_REG_ESP):#010x}'
            uc.emu_stop()
    emu.uc.hook_add(UC_HOOK_CODE, on_text, begin=TEXT[0], end=TEXT[1] - 1)

    if a.trace:
        lo, hi = (int(x, 16) for x in a.trace.split('-'))
        os.makedirs(a.out, exist_ok=True)
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md, tf = Cs(CS_ARCH_X86, CS_MODE_32), open(os.path.join(a.out, 'trace.txt'), 'w')
        from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EFLAGS)

        def trace(uc, addr, size, _):
            if emu.api_count < a.trace_after:
                return
            ins = next(md.disasm(bytes(uc.mem_read(addr, size)), addr), None)
            tf.write(f'{emu.api_count:6} {addr:08x} {(ins.mnemonic + " " + ins.op_str) if ins else "?":36} '
                     + ' '.join(f'{emu.reg(r):08x}' for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                                                             UC_X86_REG_ESP, UC_X86_REG_EFLAGS)) + '\n')
        emu.uc.hook_add(UC_HOOK_CODE, trace, begin=lo, end=hi)
    cms = next(s for s in mod['pe'].sections if s.Name.startswith(b'.cms_t'))
    n = extra.guard_divisions(emu, mod['base'] + cms.VirtualAddress,
                              mod['base'] + cms.VirtualAddress + cms.Misc_VirtualSize)
    print(f'{n} divisions in .cms_t guarded', flush=True)
    print(f'=== SimJP.exe from {entry:#x}', flush=True)
    t = time.time()
    try:
        emu.run(entry)
    except SystemExit as ex:
        print(f'=== stopped: {ex}')
    print(f'{emu.api_count} API calls, {time.time() - t:.1f}s')
    os.makedirs(a.out, exist_ok=True)
    apis = {}
    for (name, ret), c in emu.hist.items():
        apis[name] = apis.get(name, 0) + c
    with open(os.path.join(a.out, 'probe.json'), 'w') as f:
        json.dump({'stop': emu.stop_reason, 'api_calls': emu.api_count,
                   'dispatch': seen.get('dispatch'), 'text': seen.get('text'),
                   'apis': dict(sorted(apis.items(), key=lambda kv: -kv[1]))}, f, indent=1)
    with open(os.path.join(a.out, 'simjp.bin'), 'wb') as f:
        f.write(emu.read(mod['base'], mod['size']))


if __name__ == '__main__':
    main()
