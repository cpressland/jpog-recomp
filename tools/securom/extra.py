"""The Win32 calls SimJP.exe's SecuROM wrapper makes that tpw-recomp's SafeDisc
emulator (tools/securom/emu, kept as it came) does not answer. Imported for
its side effect: each @api adds to emu/winapi.py's table.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'emu'))

from winapi import api  # noqa: E402


@api(1)
def IsProcessorFeaturePresent(e, a):
    # PF_FLOATING_POINT_PRECISION_ERRATA (0) no; MMX (3) and the rest of a
    # Pentium III's yes; nothing later.
    return 1 if a(0) in (2, 3, 6) else 0


from winapi import Handle, S  # noqa: E402


@api(0, 'GetUserDefaultLangID', 'GetSystemDefaultLangID', 'GetUserDefaultUILanguage')
def GetUserDefaultLangID(e, a):
    return 0x0409


# ---- strings -------------------------------------------------------------

@api(1)
def lstrlenA(e, a):
    return len(S(e, a(0)) or '') if a(0) else 0


@api(2)
def lstrcpyA(e, a):
    e.put_cstr(a(0), S(e, a(1)) or '')
    return a(0)


@api(2)
def lstrcatA(e, a):
    e.put_cstr(a(0), (S(e, a(0)) or '') + (S(e, a(1)) or ''))
    return a(0)


@api(2, 'lstrcmpiA', 'lstrcmpA')
def lstrcmpiA(e, a):
    x, y = (S(e, a(0)) or '').lower(), (S(e, a(1)) or '').lower()
    return (x > y) - (x < y) & 0xFFFFFFFF


# ---- security: an administrator's process token ---------------------------
# SecuROM looks for the Administrators group (S-1-5-32-544) in the token's
# groups, as its driver needs it.

ADMINS = bytes([1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 2, 0, 0])


@api(3, 'OpenProcessToken')
def OpenProcessToken(e, a):
    e.w32(a(2), e.os.new_handle(Handle('token')))
    return 1


@api(4, 'OpenThreadToken')
def OpenThreadToken(e, a):
    e.os.last_error = 1008               # ERROR_NO_TOKEN: take the process's
    return 0


@api(11)
def AllocateAndInitializeSid(e, a):
    n = a(1) & 0xFF
    sid = bytes([1, n]) + e.read(a(0), 6) + b''.join((a(2 + i) & 0xFFFFFFFF).to_bytes(4, 'little')
                                                     for i in range(n))
    p = e.alloc(len(sid) + 8)
    e.write(p, sid)
    e.w32(a(10), p)
    return 1


@api(2)
def EqualSid(e, a):
    def sid(p):
        return e.read(p, 8 + 4 * e.r8(p + 1))
    return int(sid(a(0)) == sid(a(1)))


@api(1)
def FreeSid(e, a):
    return 0


@api(5)
def GetTokenInformation(e, a):
    cls, buf, size = a(1), a(2), a(3)
    if cls != 2:                          # TokenGroups only
        e.os.last_error = 87
        return 0
    need = 4 + 8 + len(ADMINS)
    e.w32(a(4), need)
    if size < need:
        e.os.last_error = 122             # ERROR_INSUFFICIENT_BUFFER
        return 0
    e.w32(buf, 1)
    e.w32(buf + 4, buf + 12)
    e.w32(buf + 8, 0xF)                   # SE_GROUP_ENABLED | MANDATORY | ...
    e.write(buf + 12, ADMINS)
    return 1


@api(3)
def LookupPrivilegeValueA(e, a):
    e.write(a(2), (20).to_bytes(8, 'little'))   # SeDebugPrivilege's LUID
    return 1


@api(6)
def AdjustTokenPrivileges(e, a):
    e.os.last_error = 0
    return 1


# ---- threads: recorded, not run --------------------------------------------

@api(6)
def CreateThread(e, a):
    h = e.os.new_handle(Handle('thread', start=a(2), param=a(3)))
    e.os.threads = getattr(e.os, 'threads', []) + [(a(2), a(3), a(4))]
    print(f'  CreateThread start {a(2):#010x} param {a(3):#x} flags {a(4):#x} (not run)', flush=True)
    if a(5):
        e.w32(a(5), 0x2000 + 4 * len(e.os.threads))
    return h


@api(2)
def SetThreadPriorityBoost(e, a):
    return 1


@api(2)
def SetPriorityClass(e, a):
    return 1


@api(1)
def GetPriorityClass(e, a):
    return 0x20                           # NORMAL_PRIORITY_CLASS


@api(4)
def CreateSemaphoreA(e, a):
    return e.os.new_handle(Handle('semaphore', count=a(1), name=S(e, a(3)) if a(3) else None))


@api(3)
def ReleaseSemaphore(e, a):
    if a(2):
        e.w32(a(2), 0)
    return 1


# ---- resources, from the module's own .rsrc --------------------------------

def _rsrc_id(e, v):
    return v if v < 0x10000 else S(e, v)


@api(3, 'FindResourceA')
def FindResourceA(e, a):
    mod = e.modules.get(a(0) or e.os.main_module['base'])
    name, typ = _rsrc_id(e, a(1)), _rsrc_id(e, a(2))
    pe = mod['pe']
    for t in getattr(pe, 'DIRECTORY_ENTRY_RESOURCE', type('', (), {'entries': []})).entries:
        tk = t.id if t.name is None else str(t.name)
        if str(tk).lower() != str(typ).lower():
            continue
        for n in t.directory.entries:
            nk = n.id if n.name is None else str(n.name)
            if str(nk).lower() == str(name).lower():
                d = n.directory.entries[0].data.struct
                h = e.os.new_handle(Handle('rsrc', addr=mod['base'] + d.OffsetToData, size=d.Size))
                return h
    print(f'  FindResource {name!r} {typ!r}: none')
    e.os.last_error = 1813
    return 0


@api(2)
def LoadResource(e, a):
    return a(1)


@api(1)
def LockResource(e, a):
    h = e.os.handles.get(a(0))
    return h.addr if h else 0


@api(2)
def SizeofResource(e, a):
    h = e.os.handles.get(a(1))
    return h.size if h else 0


# ---- multimedia timer ------------------------------------------------------

@api(0)
def timeGetTime(e, a):
    return e.os.ms & 0xFFFFFFFF


@api(3)
def timeSetEvent(e, a):
    # A periodic callback SecuROM and the game use; recorded, not fired.
    e.os.timers = getattr(e.os, 'timers', []) + [(a(0), a(2))]
    return len(e.os.timers)


@api(1)
def timeKillEvent(e, a):
    return 0


@api(1, 'timeBeginPeriod', 'timeEndPeriod')
def timeBeginPeriod(e, a):
    return 0


# ---- the module's own path -------------------------------------------------
# SecuROM reads its own .exe (the disc signature lives in it), so the name it
# gets must be one CreateFileA can open: the command line's, C:\JPOG\SimJP.exe,
# where the game folder is mounted. tpw-recomp's returned D:\<exe>, which here
# is not a real file, so the wrapper gave up (ExitProcess) before the game ran.

@api(3)
def GetModuleFileNameA(e, a):
    h = a(0)
    if h == 0 or (e.os.main_module and h == e.os.main_module['base']):
        return e.put_cstr(a(1), e.os.cmdline, a(2))
    if h in e.modules:
        return e.put_cstr(a(1), 'C:\\WINDOWS\\SYSTEM32\\' + os.path.basename(e.modules[h]['path']), a(2))
    return 0


# ---- exceptions: the trap flag, as Windows handles it ----------------------
# The CPU clears TF on the debug trap, and Windows enters a handler with it
# clear; a single-step's CONTEXT has it clear too (a handler that wants to go
# on tracing sets it there). tpw-recomp's emulator left it set, so SecuROM's
# handler single-stepped itself.

import winapi as _w  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EFLAGS  # noqa: E402

TF = 0x100
_save_context = _w.WinState.save_context
_call_next_handler = _w.WinState._call_next_handler


def _save_context_tf(self, addr, eip):
    _save_context(self, addr, eip)
    self.emu.w32(addr + 0xC0, self.emu.r32(addr + 0xC0) & ~TF)


def _call_next_handler_tf(self, st):
    e = self.emu
    e.setreg(UC_X86_REG_EFLAGS, e.reg(UC_X86_REG_EFLAGS) & ~TF)
    _call_next_handler(self, st)


_w.WinState.save_context = _save_context_tf
_w.WinState._call_next_handler = _call_next_handler_tf


# ---- divide by zero, raised before the division runs -----------------------
# SecuROM's decryptors divide by zero on purpose (every fifth byte of its
# 42 KB run) for their handler to put a divisor in ECX and resume at the same
# idiv. Unicorn turns a #DE whose interrupt hook moves EIP into a double
# fault before the handler runs, so the division is checked instead: each
# div/idiv in the range given gets a hook that, finding its divisor zero,
# raises EXCEPTION_INT_DIVIDE_BY_ZERO with EIP at the instruction, as
# Windows does, and never lets the CPU fault.

def guard_divisions(emu, lo, hi):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_MEM, X86_OP_REG
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EIP
    import capstone.x86_const as cx
    import unicorn.x86_const as ux
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True
    regs = {getattr(cx, n): getattr(ux, 'UC_' + n) for n in dir(cx)
            if n.startswith('X86_REG_') and hasattr(ux, 'UC_' + n)}
    code = emu.read(lo, hi - lo)
    sites = {}
    for ins in md.disasm(code, lo):
        if ins.mnemonic in ('div', 'idiv'):
            sites[ins.address] = ins
    emu.div_sites = sites

    def divisor(ins):
        op = ins.operands[0]
        if op.type == X86_OP_REG:
            v = emu.reg(regs[op.reg])
            return v & ((1 << 8 * op.size) - 1)
        if op.type == X86_OP_MEM:
            m = op.mem
            a = m.disp + (emu.reg(regs[m.base]) if m.base else 0) + \
                (emu.reg(regs[m.index]) * m.scale if m.index else 0)
            return int.from_bytes(emu.read(a & 0xFFFFFFFF, op.size), 'little')
        return 1

    def hook(uc, addr, size, _):
        ins = sites.get(addr)
        if ins is not None and divisor(ins) == 0:
            emu.os.dispatch_exception(0xC0000094, addr, ())
    for a in sites:
        emu.uc.hook_add(UC_HOOK_CODE, hook, begin=a, end=a)
    return len(sites)
