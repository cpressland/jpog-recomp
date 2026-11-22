"""Statically resolve SecuROM-redirected call sites by analyzing the surrounding code.

Approach (b) from PLAN.md: analyze the code around each `call [0xD8CEC0]` to determine
which import was originally called. Uses calling convention, argument patterns, and
nearby code to narrow down candidates.

    .venv/bin/python -I tools/securom/resolve_static.py --game GAME --survey work/securom/survey.json
"""
import argparse
import json
import os
import re
import struct
import sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

DISPATCH = 0xD8CEC0
GAME_MAIN = 0x484330


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def u32(self, va):
        return struct.unpack_from('<I', self.img, va - self.base)[0]

    def bytes(self, lo, hi):
        return self.img[lo - self.base:hi - self.base]

    def disasm(self, addr, count=20):
        """Disassemble `count` instructions before and after addr."""
        # Disassemble backwards to find function start
        start = max(addr - 100, 0x401000)  # Don't go before .text
        code = self.bytes(start, addr + 100)
        instructions = []
        for ins in self.md.disasm(code, start):
            instructions.append(ins)
            if len(instructions) > 200:
                break
        return instructions


def analyze_call_site(im, site):
    """Analyze the code around a call site to determine the import."""
    # Get the instructions around the call
    instructions = im.disasm(site, 30)
    
    # Find the call instruction
    call_idx = None
    for i, ins in enumerate(instructions):
        if ins.address == site:
            call_idx = i
            break
    
    if call_idx is None:
        return None
    
    # Look at the instructions before the call to find argument pushes
    before = instructions[max(0, call_idx - 10):call_idx]
    after = instructions[call_idx + 1:min(len(instructions), call_idx + 5)]
    
    # Count pushes before the call
    pushes = []
    for ins in reversed(before):
        if ins.mnemonic == 'push':
            pushes.append(ins.op_str)
        elif ins.mnemonic in ('mov', 'lea') and 'esp' in ins.op_str:
            break  # Stop at stack frame setup
    
    # Check if ECX is set (this pointer for C++ methods)
    has_ecx = any('ecx' in ins.op_str for ins in before[-5:])
    
    # Check what happens with the return value
    uses_eax = any('eax' in ins.op_str for ins in after[:3])
    uses_al = any('al' in ins.op_str for ins in after[:3])
    
    return {
        'pushes': pushes,
        'has_ecx': has_ecx,
        'uses_eax': uses_eax,
        'uses_al': uses_al,
        'num_args': len(pushes),
    }


def resolve_sites(im, sites, iat):
    """Resolve call sites by analyzing the code patterns."""
    results = {}
    
    # Known patterns for common imports
    patterns = {
        # kernel32.dll
        'CreateMutexA': {'args': 3, 'ecx': False},
        'EnterCriticalSection': {'args': 1, 'ecx': False},
        'LeaveCriticalSection': {'args': 1, 'ecx': False},
        'InitializeCriticalSection': {'args': 1, 'ecx': False},
        'DeleteCriticalSection': {'args': 1, 'ecx': False},
        'WaitForSingleObject': {'args': 2, 'ecx': False},
        'ReleaseMutex': {'args': 1, 'ecx': False},
        'CreateEventA': {'args': 4, 'ecx': False},
        'SetEvent': {'args': 1, 'ecx': False},
        'ResetEvent': {'args': 1, 'ecx': False},
        'CloseHandle': {'args': 1, 'ecx': False},
        'GetCurrentProcess': {'args': 0, 'ecx': False},
        'GetCurrentProcessId': {'args': 0, 'ecx': False},
        'GetCurrentThread': {'args': 0, 'ecx': False},
        'GetCurrentThreadId': {'args': 0, 'ecx': False},
        'GetTickCount': {'args': 0, 'ecx': False},
        'QueryPerformanceCounter': {'args': 1, 'ecx': False},
        'QueryPerformanceFrequency': {'args': 1, 'ecx': False},
        'Sleep': {'args': 1, 'ecx': False},
        'GetLastError': {'args': 0, 'ecx': False},
        'SetLastError': {'args': 1, 'ecx': False},
        
        # user32.dll
        'MessageBoxA': {'args': 4, 'ecx': False},
        'MessageBoxW': {'args': 4, 'ecx': False},
        'PostMessageA': {'args': 4, 'ecx': False},
        'SendMessageA': {'args': 4, 'ecx': False},
        
        # Add more patterns as needed
    }
    
    for site in sites:
        analysis = analyze_call_site(im, site)
        if not analysis:
            continue
        
        # Try to match against known patterns
        matched = None
        for name, pattern in patterns.items():
            if (analysis['num_args'] == pattern['args'] and
                analysis['has_ecx'] == pattern['ecx']):
                # Find which DLL this import is from
                for slot, (dll, imp_name) in iat.items():
                    if imp_name == name:
                        matched = (dll, name)
                        break
                if matched:
                    break
        
        if matched:
            results[site] = matched
    
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', required=True, help='the game folder')
    ap.add_argument('--survey', default='work/securom/survey.json', help='survey output')
    ap.add_argument('--out', default='work/securom/sites_static.json')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    
    with open(a.survey) as f:
        survey = json.load(f)
    
    sites = survey['sites']
    iat = {int(k, 16): tuple(v) for k, v in survey['iat'].items()}
    
    exe_path = os.path.join(a.game, 'SimJP.exe')
    im = Image(exe_path)
    
    print(f'resolving {len(sites)} sites statically...', flush=True)
    results = resolve_sites(im, sites, iat)
    
    print(f'{len(results)} resolved out of {len(sites)} sites', flush=True)
    
    # Write results
    output = {
        'resolved': len(results),
        'total': len(sites),
        'sites': {f'{k:#x}': list(v) for k, v in sorted(results.items())},
    }
    
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as f:
        json.dump(output, f, indent=1)
    
    print(f'wrote {a.out}', flush=True)
    
    # Summary by DLL
    by_dll = {}
    for dll, name in results.values():
        by_dll[dll] = by_dll.get(dll, 0) + 1
    for dll, n in sorted(by_dll.items(), key=lambda kv: -kv[1]):
        print(f'  {dll}: {n}')


if __name__ == '__main__':
    main()
