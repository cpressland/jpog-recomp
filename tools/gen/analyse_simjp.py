"""Analyse SimJP.exe for lifting: exclude SecuROM sections, prepare for lift.

SimJP.exe is SecuROM-wrapped, but the game's code and imports are intact.
We exclude .cms_t and .cms_d (SecuROM's code) and lift only .text.

    .venv/bin/python -I tools/gen/analyse_simjp.py GAME WORK
"""
import json
import os
import shutil
import struct
import subprocess
import sys

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def tools():
    return os.path.join(os.environ.get('PCRECOMP', os.path.join(os.path.dirname(REPO), 'pcrecomp')), 'tools')


def analyse(game, work):
    name = 'simjp'
    out = os.path.join(work, name)
    os.makedirs(out, exist_ok=True)
    
    # First, patch SimJP.exe to restore original imports if still SecuROM-wrapped
    src_in = os.path.join(game, 'SimJP.exe')
    src = os.path.join(out, 'SimJP.exe')
    pe_in = pefile.PE(src_in, fast_load=True)
    if pe_in.OPTIONAL_HEADER.AddressOfEntryPoint == 0x00331118:
        shutil.copyfile(src_in, src)
    else:
        sys.path.insert(0, HERE)
        from patch_simjp import patch
        patch(game, work)
    
    pe = pefile.PE(src, fast_load=True)
    if pe.FILE_HEADER.TimeDateStamp != 0x3E434C37:
        raise SystemExit(f'{src}: PE timestamp 0x{pe.FILE_HEADER.TimeDateStamp:08X}, expected 0x3E434C37')
    
    # SimJP.exe keeps its own base (0x400000), no rebasing needed
    img = src  # Use the patched PE directly
    
    # Load the full PE to get section info
    pe = pefile.PE(img)
    base = pe.OPTIONAL_HEADER.ImageBase
    
    # Find executable code sections (.text + PSGSFD*, excluding .cms_t)
    code_sections = [s for s in pe.sections if (s.Characteristics & 0x20000000) and not s.Name.startswith(b'.cms_')]
    if not code_sections:
        raise SystemExit('No code sections found')

    text_start = min(base + s.VirtualAddress for s in code_sections)
    text_end = max(base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in code_sections)

    print(f'SimJP.exe: code at 0x{text_start:08X}-0x{text_end:08X}')
    
    # Write code range info
    with open(os.path.join(out, 'code_range.json'), 'w') as f:
        json.dump({'code_start': text_start, 'code_end': text_end}, f, indent=2)
    
    # Run pcrecomp analysis
    t, py = tools(), sys.executable
    steps = (
        ('pe', [py, os.path.join(t, 'pe', 'pe_analyze.py'), img, '--json', os.path.join(out, 'pe_analysis.json')]),
        ('rtti', [py, os.path.join(t, 'cpp', 'rtti.py'), img, '-o', os.path.join(out, 'rtti.json'),
                  '--seeds', os.path.join(out, 'rtti_seeds.json')]),
        ('disasm', [py, os.path.join(t, 'disasm', 'disasm32.py'), img, '-o', os.path.join(out, 'functions.json'),
                    '--seed-functions', os.path.join(out, 'rtti_seeds.json')]),
        ('data', [py, os.path.join(HERE, 'data_code_ptrs.py'), img, os.path.join(out, 'functions.json'),
                  os.path.join(out, 'run_seeds.json')]),
    )
    
    for step, cmd in steps:
        log_path = os.path.join(out, step + '.log')
        with open(log_path, 'w') as log:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                # For disasm, check if functions.json was created despite the error
                if step == 'disasm' and os.path.exists(os.path.join(out, 'functions.json')):
                    print(f'  {step}: completed with warnings (see {log_path})')
                    continue
                raise SystemExit(f'{name}: {step} failed (see {log_path})')
        print(f'  {step}: ok')
    
    # Ensure every non-null entry in SimJP.exe's _initterm C++ constructor table
    # (0x00835000..0x0083C5C0) is in the catalog or run_seeds.json: entries are
    # 0x40-byte spaced per translation unit, so data_code_ptrs.py's 3-in-a-row
    # heuristic misses isolated constructors.
    funcs_set = {f['address'] for f in json.load(open(os.path.join(out, 'functions.json')))['functions']}
    seeds_path = os.path.join(out, 'run_seeds.json')
    seeds = json.load(open(seeds_path)) if os.path.exists(seeds_path) else []
    known = funcs_set | {s['address'] for s in seeds}
    img_bytes = pe.get_memory_mapped_image()
    added_xc = 0
    for va in range(0x00835000, 0x0083C5C0, 4):
        fn = struct.unpack_from('<I', img_bytes, va - base)[0]
        if fn and text_start <= fn < text_end and fn not in known:
            seeds.append({'address': fn, 'from': va})
            known.add(fn)
            added_xc += 1
    if added_xc:
        with open(seeds_path, 'w') as f:
            json.dump(seeds, f, indent=2)
        print(f'  initterm seeds: added {added_xc}')

    print(f'SimJP.exe analysis complete')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    analyse(sys.argv[1], sys.argv[2])
