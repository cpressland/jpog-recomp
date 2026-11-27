"""The lift's inputs for every module: headers and imports, C++ classes, the
function catalog, code reached only through data.

    .venv/bin/python -I tools/gen/analyse.py GAME WORK [module ...]

For each module of tools/gen/modules.py (all, or those named), in parallel:

    WORK/<module>/<file>             the module as lifted (rebased if it moves)
    WORK/<module>/pe_analysis.json   pcrecomp pe/pe_analyze.py
    WORK/<module>/rtti.json, rtti_seeds.json    pcrecomp cpp/rtti.py
    WORK/<module>/functions.json     pcrecomp disasm/disasm32.py
    WORK/<module>/run_seeds.json     tools/gen/data_code_ptrs.py
    WORK/<module>/*.log              what each said

PCRECOMP names the toolkit (default ../pcrecomp beside this repository; from stl-recomp).
"""
import concurrent.futures
import json
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import pefile  # noqa: E402

from modules import MODULES  # noqa: E402
from rebase import rebase  # noqa: E402


def tools():
    return os.path.join(os.environ.get('PCRECOMP', os.path.join(os.path.dirname(REPO), 'pcrecomp')), 'tools')


def analyse(game, work, module):
    name, file, base, stamp = module
    out = os.path.join(work, name)
    os.makedirs(out, exist_ok=True)
    src = os.path.join(game, file)
    got = pefile.PE(src, fast_load=True).FILE_HEADER.TimeDateStamp
    if got != stamp:
        raise SystemExit(f'{src}: PE timestamp 0x{got:08X}, this project lifts 0x{stamp:08X}')
    img = os.path.join(out, os.path.basename(file))
    rebase(src, base, img) if pefile.PE(src, fast_load=True).OPTIONAL_HEADER.ImageBase != base \
        else shutil.copyfile(src, img)
    text_start = text_end = 0
    pe_full = None
    if name == 'simjp':
        pe_full = pefile.PE(img)
        code_sections = [s for s in pe_full.sections if (s.Characteristics & 0x20000000) and not s.Name.startswith(b'.cms_')]
        if code_sections:
            text_start = min(base + s.VirtualAddress for s in code_sections)
            text_end = max(base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in code_sections)
            with open(os.path.join(out, 'code_range.json'), 'w') as f:
                json.dump({'code_start': text_start, 'code_end': text_end}, f, indent=2)
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
    start = time.time()
    for step, cmd in steps:
        with open(os.path.join(out, step + '.log'), 'w') as log:
            if subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT).returncode:
                raise SystemExit(f'{name}: {step} failed (see {out}/{step}.log)')
    if name == 'simjp' and pe_full is not None and text_end > text_start:
        # Ensure every non-null entry in SimJP.exe's _initterm C++ constructor table
        # (0x00835000..0x0083C5C0) is in the catalog or run_seeds.json: entries are
        # 0x40-byte spaced per translation unit, so data_code_ptrs.py misses isolated ones.
        funcs_set = {f['address'] for f in json.load(open(os.path.join(out, 'functions.json')))['functions']}
        seeds_path = os.path.join(out, 'run_seeds.json')
        seeds = json.load(open(seeds_path)) if os.path.exists(seeds_path) else []
        known = funcs_set | {s['address'] for s in seeds}
        img_bytes = pe_full.get_memory_mapped_image()
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
    return f'{name}: {time.time() - start:.0f} s'


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    game, work, want = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    mods = [m for m in MODULES if not want or m[0] in want]
    with concurrent.futures.ProcessPoolExecutor(min(len(mods), os.cpu_count() or 4)) as ex:
        for line in ex.map(analyse, [game] * len(mods), [work] * len(mods), mods):
            print(line, flush=True)


if __name__ == '__main__':
    main()
