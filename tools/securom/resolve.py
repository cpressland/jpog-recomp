"""Resolve SecuROM dispatcher calls using snapshot/restore.

Boot once to install the dispatcher, snapshot the state, then for each site
restore the snapshot and call the dispatcher to resolve the import.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'emu'))
sys.path.insert(0, HERE)

import sdemu  # noqa: E402
import extra  # noqa: E402,F401
from probe import Boot, DISPATCH  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_EBP  # noqa: E402

DISPATCH_ENTRY = 0xD5DA00  # the dispatcher's first instruction (decrypted .cms_t)
THUNKS = 0x7E000000
THUNKS_END = 0x7E100000

# Memory ranges the dispatcher mutates and must be reset between calls.
MUTABLE_RANGES = [
    (0xD5DA00, 0x6600),   # .cms_t dispatcher code (may self-modify)
    (0xD8CE00, 0x3200),   # dispatch table at 0xD8CEF0, slot at 0xD8CEC0
    (0xE00000, 0x10000),  # dispatcher's working memory: counters, tables
]


class Resolver:
    """Boot once, snapshot, then restore and drive the dispatcher per site."""

    def __init__(self, game, iso, disc, root='work/securom/emu', verbose=False):
        self.verbose = verbose
        print('booting to install dispatcher...', flush=True)
        self.boot = Boot(game, iso, disc, root, verbose, stop_at='dispatch', quiet=True)
        self.boot.run()
        self.emu = self.boot.emu
        emu = self.emu

        if emu.r32(DISPATCH) != DISPATCH_ENTRY:
            raise RuntimeError(f'dispatcher not installed: [{DISPATCH:#x}] = {emu.r32(DISPATCH):#x}')

        # Snapshot: all registers
        self.saved_regs = {}
        for reg in (sdemu.UC_X86_REG_EAX, sdemu.UC_X86_REG_EBX, sdemu.UC_X86_REG_ECX,
                    sdemu.UC_X86_REG_EDX, sdemu.UC_X86_REG_ESI, sdemu.UC_X86_REG_EDI,
                    sdemu.UC_X86_REG_EBP, sdemu.UC_X86_REG_ESP, sdemu.UC_X86_REG_EIP,
                    sdemu.UC_X86_REG_EFLAGS):
            self.saved_regs[reg] = emu.reg(reg)

        # Snapshot: mutable memory
        self.saved_mem = {}
        for addr, size in MUTABLE_RANGES:
            self.saved_mem[addr] = emu.read(addr, size)

        # Snapshot: stack (the dispatcher uses the stack)
        # We'll set up a fresh stack for each call instead of snapshotting
        self.stack_base = 0x2FFE00  # Use a fixed stack base

        print(f'dispatcher installed at {DISPATCH_ENTRY:#x}, ready to resolve', flush=True)

    def _restore(self):
        """Restore all state to the snapshot."""
        emu = self.emu
        # Restore registers
        for reg, val in self.saved_regs.items():
            emu.setreg(reg, val)
        # Restore mutable memory
        for addr, data in self.saved_mem.items():
            emu.write(addr, data)

    def resolve_site(self, site):
        """Drive the dispatcher for one call site. Returns (dll, name) or None."""
        emu = self.emu
        self._restore()

        ret_addr = site + 6  # past the `ff 15 c0 ce d8 00`

        # Set up the stack as if the game code called the dispatcher:
        # The dispatcher starts with `push ebp`, so we need:
        # [ESP] = old EBP (will be pushed by dispatcher)
        # [ESP+4] = return address (what `call` pushed)
        esp = self.stack_base - 8
        emu.setreg(sdemu.UC_X86_REG_ESP, esp)
        emu.w32(esp, 0)  # dummy old EBP
        emu.w32(esp + 4, ret_addr)  # return address
        emu.setreg(UC_X86_REG_EBP, esp)  # EBP points to the saved EBP
        emu.setreg(UC_X86_REG_EIP, DISPATCH_ENTRY)

        if self.verbose:
            print(f'  resolving {site:#x}: ret_addr={ret_addr:#x}, esp={esp:#x}', flush=True)

        # Hook: record all thunk calls
        calls = []
        def on_thunk(uc, addr, size, _):
            if THUNKS <= addr < THUNKS_END:
                key = emu.thunks.get(addr)
                if key:
                    calls.append(key)
                    if self.verbose:
                        print(f'    thunk: {key}', flush=True)
                    # Stop after we see a few calls (the dispatcher may call several)
                    if len(calls) >= 10:
                        uc.emu_stop()

        h = emu.uc.hook_add(UC_HOOK_CODE, on_thunk, begin=THUNKS, end=THUNKS_END - 1)

        # Add instruction trace for debugging
        if self.verbose:
            from capstone import Cs, CS_ARCH_X86, CS_MODE_32
            md = Cs(CS_ARCH_X86, CS_MODE_32)
            instr_count = [0]
            def trace_instr(uc, addr, size, _):
                if instr_count[0] < 100:  # Trace first 100 instructions
                    code = bytes(uc.mem_read(addr, min(size, 15)))
                    ins = next(md.disasm(code, addr), None)
                    if ins:
                        print(f'      {addr:#010x}: {ins.mnemonic:8} {ins.op_str}', flush=True)
                    instr_count[0] += 1
            h_trace = emu.uc.hook_add(UC_HOOK_CODE, trace_instr, begin=DISPATCH_ENTRY, end=DISPATCH_ENTRY + 0x1000)

        # Limit API calls to prevent infinite loops
        emu.max_api = 100

        try:
            emu.run(DISPATCH_ENTRY)
        except SystemExit:
            pass

        if self.verbose:
            emu.uc.hook_del(h_trace)

        if self.verbose:
            print(f'    total calls: {len(calls)}', flush=True)

        # The real import is typically the last non-internal call
        internal = {('kernel32.dll', 'EnterCriticalSection'),
                    ('kernel32.dll', 'LeaveCriticalSection'),
                    ('kernel32.dll', 'GetCurrentThreadId'),
                    ('kernel32.dll', 'GetTickCount'),
                    ('kernel32.dll', 'GetCurrentProcessId'),
                    ('kernel32.dll', 'QueryPerformanceCounter')}

        # Filter out internal calls and return the last one
        real_calls = [c for c in calls if c not in internal]
        return real_calls[-1] if real_calls else (calls[-1] if calls else None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', required=True, help='the game folder (install.py)')
    ap.add_argument('--iso', required=True, help='jpog.iso, for raw sector reads')
    ap.add_argument('--disc', required=True, help="the disc's files, extracted")
    ap.add_argument('--root', default='work/securom/emu')
    ap.add_argument('--survey', default='work/securom/survey.json', help='survey output from survey.py')
    ap.add_argument('--out', default='work/securom/sites.json')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--limit', type=int, default=0, help='resolve only this many sites (for testing)')
    a = ap.parse_args()

    with open(a.survey) as f:
        survey = json.load(f)
    sites = survey['sites']

    if a.limit:
        sites = sites[:a.limit]

    print(f'resolving {len(sites)} sites...', flush=True)
    t0 = time.time()
    resolver = Resolver(a.game, a.iso, a.disc, a.root, a.verbose)
    print(f'boot + snapshot: {time.time() - t0:.1f}s', flush=True)

    results = {}
    failed = []
    t1 = time.time()
    for i, site in enumerate(sites):
        got = resolver.resolve_site(site)
        if got is None:
            failed.append(site)
            if a.verbose:
                print(f'  {site:#010x}: FAILED', flush=True)
        else:
            dll, name = got
            results[f'{site:#x}'] = [dll, name]
        if (i + 1) % 100 == 0 or i + 1 == len(sites):
            elapsed = time.time() - t1
            rate = (i + 1) / elapsed if elapsed > 0 else 0
            eta = (len(sites) - i - 1) / rate if rate > 0 else 0
            print(f'  {i + 1}/{len(sites)}: {len(results)} resolved, '
                  f'{len(failed)} failed, {rate:.0f}/s, ETA {eta:.0f}s', flush=True)

    print(f'{len(results)} resolved, {len(failed)} failed in {time.time() - t1:.1f}s', flush=True)

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as f:
        json.dump({
            'resolved': len(results),
            'failed': len(failed),
            'failed_sites': [f'{s:#x}' for s in failed],
            'sites': dict(sorted(results.items())),
        }, f, indent=1)
    print(f'wrote {a.out}', flush=True)

    # Summary by DLL.
    by_dll = {}
    for dll, name in results.values():
        by_dll[dll] = by_dll.get(dll, 0) + 1
    for dll, n in sorted(by_dll.items(), key=lambda kv: -kv[1]):
        print(f'  {dll}: {n}')


if __name__ == '__main__':
    main()
