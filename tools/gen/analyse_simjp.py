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
    
    # First, patch SimJP.exe to restore original imports
    # This must be done before analysis to avoid references to the dispatcher
    sys.path.insert(0, HERE)
    from patch_simjp import patch
    patch(game, work)
    
    # Use the patched PE for analysis
    src = os.path.join(out, 'SimJP.exe')
    
    pe = pefile.PE(src, fast_load=True)
    if pe.FILE_HEADER.TimeDateStamp != 0x3E434C37:
        raise SystemExit(f'{src}: PE timestamp 0x{pe.FILE_HEADER.TimeDateStamp:08X}, expected 0x3E434C37')
    
    # SimJP.exe keeps its own base (0x400000), no rebasing needed
    img = src  # Use the patched PE directly
    
    # Load the full PE to get section info
    pe = pefile.PE(img)
    base = pe.OPTIONAL_HEADER.ImageBase
    
    # Find .text section (the game's code)
    text_section = None
    for s in pe.sections:
        if s.Name.startswith(b'.text'):
            text_section = s
            break
    
    if not text_section:
        raise SystemExit('No .text section found')
    
    text_start = base + text_section.VirtualAddress
    text_end = base + text_section.VirtualAddress + text_section.Misc_VirtualSize
    
    print(f'SimJP.exe: .text at 0x{text_start:08X}-0x{text_end:08X}')
    
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
        ('data', [py, '-I', os.path.join(HERE, 'data_code_ptrs.py'), img, os.path.join(out, 'functions.json'),
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
    
    # Note: we don't patch code_end here because there are references from .text to .cms_t
    # (exception handlers, etc.). The lift will need to handle .cms_t specially.
    
    print(f'SimJP.exe analysis complete')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    analyse(sys.argv[1], sys.argv[2])
