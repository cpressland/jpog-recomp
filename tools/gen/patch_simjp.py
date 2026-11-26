"""Patch SimJP.exe to restore original imports before analysis.

The SecuROM wrapper redirected 3,642 calls through a dispatcher at 0xD8CEC0.
We use sites.json to rewrite each `call [0xD8CEC0]` back to `call [original_iat_slot]`.

    .venv/bin/python -I tools/gen/patch_simjp.py GAME WORK
"""
import json
import os
import struct
import sys

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
SECUROM_DIR = os.path.join(os.path.dirname(HERE), 'securom')


def patch(game, work):
    src = os.path.join(game, 'SimJP.exe')
    out_dir = os.path.join(work, 'simjp')
    os.makedirs(out_dir, exist_ok=True)

    sys.path.insert(0, SECUROM_DIR)
    from unwrap import read_original_iat, unwrap

    securom_out = os.path.join(os.path.dirname(os.path.abspath(work)), 'securom')
    sites_path = os.path.join(securom_out, 'sites.json')
    tail_path = os.path.join(securom_out, 'text_tail.bin')
    if not (os.path.exists(sites_path) and os.path.exists(tail_path)):
        unwrap(game, securom_out, root=os.path.join(securom_out, 'emu'))

    with open(sites_path) as f:
        sites = json.load(f)

    print(f'Loading {sites["resolved"]} resolved sites from {sites_path}')

    # Load the PE file as raw bytes and parse its intact original IAT at 0x739000
    data = bytearray(open(src, 'rb').read())
    iat_to_imp = read_original_iat(bytes(data))
    iat_map = {(dll, name): slot for slot, (dll, name) in iat_to_imp.items()}

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

    # Restore the decrypted tail of .text (0x0072F000-0x00733000, 16 KB).
    # SecuROM encrypts these 4 pages on disk; they hold the import thunks
    # (including AApplication::OnUpdate at 0x72FC86) and mainCRTStartup (0x731118).
    tail_bytes = open(tail_path, 'rb').read()
    tail_va = 0x0072F000
    tail_offset = pe.get_offset_from_rva(tail_va - base)
    if len(tail_bytes) != 0x4000:
        raise SystemExit(f'{tail_path}: expected 0x4000 bytes, got 0x{len(tail_bytes):X}')
    data[tail_offset:tail_offset + len(tail_bytes)] = tail_bytes
    print(f'Restored decrypted .text tail at 0x{tail_va:08X} ({len(tail_bytes)} bytes)')

    # Restore the original PE headers that SecuROM redirected to .cms_t/.idata:
    # - AddressOfEntryPoint -> 0x00331118 (VA 0x00731118, MSVC 6 mainCRTStartup)
    # - Import Directory    -> RVA 0x00341874, size 0x1B8 (the game's original 21 DLLs)
    # - IAT Directory       -> RVA 0x00339000, size 0x11A4 (the game's original 1,108 slots)
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    opt_off = e_lfanew + 24
    struct.pack_into('<I', data, opt_off + 16, 0x00331118)                 # AddressOfEntryPoint
    struct.pack_into('<II', data, opt_off + 96 + 1 * 8, 0x00341874, 0x1B8) # DIR_IMPORT
    struct.pack_into('<II', data, opt_off + 96 + 12 * 8, 0x00339000, 0x11A4) # DIR_IAT

    # Clear CODE/EXEC flags on SecuROM's .cms_t section so pe_analyze/disasm32
    # only span the game's contiguous code sections (.text + PSGSFD*, 0x464000..0x739000)
    # and never cross .data's uninitialized BSS into .cms_t.
    nsec = struct.unpack_from('<H', data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from('<H', data, e_lfanew + 20)[0]
    sec_off = opt_off + opt_size
    for i in range(nsec):
        o = sec_off + i * 40
        if data[o:o + 6] == b'.cms_t':
            chars = struct.unpack_from('<I', data, o + 36)[0]
            chars &= ~(0x00000020 | 0x20000000)  # IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE
            struct.pack_into('<I', data, o + 36, chars)
            break

    print('Restored entry point (0x00731118), original import/IAT (0x00741874/0x00739000), cleared .cms_t EXEC')

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
