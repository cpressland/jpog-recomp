#!/usr/bin/env python3
"""Extract the import resolution table from the nocd crack.

The crack has patched all 3,642 call sites back to their original imports.
We read each call site and extract which IAT slot it calls, then map that
to the actual import name.

Usage:
    python tools/securom/extract_from_crack.py
"""

import json
import struct
import pefile

def main():
    # Load the crack
    pe = pefile.PE('crack/simjp.exe')
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    
    # Build a map of IAT address -> (dll, name)
    iat_map = {}
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode()
            for imp in entry.imports:
                if imp.address:
                    name = imp.name.decode() if imp.name else f'ordinal_{imp.ordinal}'
                    iat_map[imp.address] = (dll, name)
    
    # Load the survey data to get the call sites
    with open('work/securom/survey.json', 'r') as f:
        survey = json.load(f)
    
    sites = survey['sites']
    
    # Extract the resolution for each site
    resolved = {}
    failed = []
    
    for site in sites:
        offset = site - base
        
        # Read the 6 bytes: ff 15 XX XX XX XX
        opcode = img[offset:offset+2]
        if opcode != b'\xff\x15':
            failed.append((site, f'Unexpected opcode: {opcode.hex()}'))
            continue
        
        iat_slot = struct.unpack_from('<I', img, offset + 2)[0]
        
        # Look up the import
        if iat_slot in iat_map:
            dll, name = iat_map[iat_slot]
            resolved[site] = [dll, name]
        else:
            failed.append((site, f'IAT slot {iat_slot:#x} not found in imports'))
    
    # Write the results
    output = {
        'resolved': len(resolved),
        'failed': len(failed),
        'failed_sites': [f'{s:#x}' for s, _ in failed],
        'sites': {f'{k:#x}': v for k, v in sorted(resolved.items())}
    }
    
    with open('work/securom/sites.json', 'w') as f:
        json.dump(output, f, indent=2)
    
    print(f'Resolved: {len(resolved)}/{len(sites)}')
    print(f'Failed: {len(failed)}/{len(sites)}')
    
    if failed:
        print('\nFirst 10 failures:')
        for site, reason in failed[:10]:
            print(f'  {site:#x}: {reason}')
    
    # Print summary by DLL
    dll_counts = {}
    for dll, name in resolved.values():
        dll_counts[dll] = dll_counts.get(dll, 0) + 1
    
    print('\nImports by DLL:')
    for dll, count in sorted(dll_counts.items(), key=lambda x: -x[1]):
        print(f'  {dll}: {count}')

if __name__ == '__main__':
    main()
