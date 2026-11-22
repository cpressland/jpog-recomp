"""Patch SimJP.exe to restore original imports before analysis.

The SecuROM wrapper redirected 3,642 calls through a dispatcher at 0xD8CEC0.
We use sites.json to rewrite each `call [0xD8CEC0]` back to `call [original_iat_slot]`.

    .venv/bin/python -I tools/gen/patch_simjp.py GAME WORK
"""
import json
import os
import struct

import pefile


def patch(game, work):
    src = os.path.join(game, 'SimJP.exe')
    out_dir = os.path.join(work, 'simjp')
    os.makedirs(out_dir, exist_ok=True)
    
    # Load sites.json
    sites_path = os.path.join(work, '..', 'securom', 'sites.json')
    if not os.path.exists(sites_path):
        sites_path = 'work/securom/sites.json'
    
    with open(sites_path) as f:
        sites = json.load(f)
    
    print(f'Loading {sites["resolved"]} resolved sites from {sites_path}')
    
    # Load the original IAT from survey.json
    survey_path = 'work/securom/survey.json'
    with open(survey_path) as f:
        survey = json.load(f)
    
    # Build a map of (dll, name) -> IAT slot address
    iat_map = {}
    for slot_str, (dll, name) in survey['iat'].items():
        slot = int(slot_str, 16)
        iat_map[(dll.lower(), name)] = slot
    
    # Load the PE file as raw bytes
    data = bytearray(open(src, 'rb').read())
    pe = pefile.PE(data=bytes(data), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    
    # The dispatcher slot address
    DISPATCH = 0xD8CEC0
    dispatch_pattern = b'\xff\x15' + struct.pack('<I', DISPATCH)
    
    patched_count = 0
    for site_str, (dll, name) in sites['sites'].items():
        site = int(site_str, 16)
        
        # Convert VA to file offset
        rva = site - base
        offset = pe.get_offset_from_rva(rva)
        
        # Verify this site currently calls the dispatcher
        if data[offset:offset+6] != dispatch_pattern:
            print(f'Warning: site 0x{site:08X} does not call dispatcher')
            continue
        
        # Find the original IAT slot for this import
        key = (dll.lower(), name)
        if key not in iat_map:
            print(f'Warning: no IAT slot for {dll}!{name}')
            continue
        
        iat_slot = iat_map[key]
        
        # Rewrite the call to use the original IAT slot
        data[offset:offset+6] = b'\xff\x15' + struct.pack('<I', iat_slot)
        patched_count += 1
    
    print(f'Patched {patched_count} call sites')
    
    # Also patch references to .cms_t and .cms_d (SecuROM's code and data) to point to safe stubs
    # These are likely exception handlers or other control flow from SecuROM
    # We'll point them to RET instructions in .text
    cms_t_start = 0xD56000
    cms_t_end = 0xD89000
    cms_d_start = 0xD89000
    cms_d_end = 0xE0D000
    
    # Find a suitable location for stubs (RET instructions)
    # We'll use the end of .text section
    pe_full = pefile.PE(data=bytes(data))
    text_section = None
    for s in pe_full.sections:
        if s.Name.startswith(b'.text'):
            text_section = s
            break
    
    if text_section:
        # Use the last few bytes of .text for stubs
        stub_rva = text_section.VirtualAddress + text_section.Misc_VirtualSize - 32
        stub_va = base + stub_rva
        
        # Write RET instructions (one for each stub location)
        stub_offset = pe_full.get_offset_from_rva(stub_rva)
        for i in range(16):
            data[stub_offset + i] = 0xC3  # RET
        
        # Patch all references to .cms_t to point to the first stub
        patched_refs = 0
        for i in range(0, len(data) - 4, 4):
            val = struct.unpack_from('<I', data, i)[0]
            if cms_t_start <= val < cms_t_end:
                struct.pack_into('<I', data, i, stub_va)
                patched_refs += 1
        
        print(f'Patched {patched_refs} references to .cms_t -> stub at 0x{stub_va:08X}')
        
        # Patch all references to .cms_d to point to the second stub
        stub_va_2 = stub_va + 1
        patched_refs_2 = 0
        for i in range(0, len(data) - 4, 4):
            val = struct.unpack_from('<I', data, i)[0]
            if cms_d_start <= val < cms_d_end:
                struct.pack_into('<I', data, i, stub_va_2)
                patched_refs_2 += 1
        
        print(f'Patched {patched_refs_2} references to .cms_d -> stub at 0x{stub_va_2:08X}')
        
        # Patch references to .idata (SecuROM's import directory)
        idata_start = 0xE0D000
        idata_end = 0xE1C000
        stub_va_3 = stub_va + 2
        patched_refs_3 = 0
        for i in range(0, len(data) - 4, 4):
            val = struct.unpack_from('<I', data, i)[0]
            if idata_start <= val < idata_end:
                struct.pack_into('<I', data, i, stub_va_3)
                patched_refs_3 += 1
        
        if patched_refs_3 > 0:
            print(f'Patched {patched_refs_3} references to .idata -> stub at 0x{stub_va_3:08X}')
    
    # Write the patched PE
    patched_path = os.path.join(out_dir, 'SimJP.exe')
    with open(patched_path, 'wb') as f:
        f.write(data)
    
    print(f'Wrote patched PE to {patched_path}')
    
    # Write metadata
    with open(os.path.join(out_dir, 'patch_info.json'), 'w') as f:
        json.dump({
            'original': src,
            'patched': patched_path,
            'sites_patched': patched_count,
            'base': base,
        }, f, indent=2)


if __name__ == '__main__':
    import sys
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    patch(sys.argv[1], sys.argv[2])
