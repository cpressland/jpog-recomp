#!/usr/bin/env python3
"""Unwrap SecuROM 4.x on SimJP.exe directly from the installed game folder.

No third-party crack and no physical disc are needed — only the shipping
SimJP.exe extracted from jpog.iso by tools/install/install.py.

How it works:
  1. Boots SimJP.exe's SecuROM wrapper under Unicorn (probe.Boot) until the
     dispatcher slot [0xD8CEC0] <- 0xD5DA00 is installed (~165 API calls, ~1s).
     By this point SecuROM's own decryptors have decrypted .cms_t, 15,872 of
     the 16,384 bytes of .text's tail (0x72F000-0x733000), and populated the
     runtime switch constants in .cms_d.
  2. Decrypts the 14-byte product ID at 0xD8B518 (reverse-XOR loop at 0xD63352).
  3. Recovers the 9-byte SecuROM session key (0xE052B0) directly from the
     0xD8CEF0 call-site hash table, SimJP.exe's own 3,642 `call [0xD8CEC0]`
     sites, and the encrypted .text bounds at 0xD8B5A4..0xD8B5A8.
  4. Decrypts the remaining 512-byte OEP window (0x731018-0x731218) using
     SecuROM's exact formula (0xD5BB0A..0xD5BB69), revealing mainCRTStartup at
     0x731118 and its redirected `__p___initenv` call at 0x7311E2.
  5. Decodes all 3,643 redirected call sites through SecuROM's 12-case switch
     (0xD5DE53..0xD5DF9F) back to their original IAT slots in 0x739000..0x73A1A4,
     verifies known sites (CreateMutexA, TApplication::Create, __p___initenv),
     and writes `sites.json` and `text_tail.bin`.

Usage:
    .venv/bin/python -I tools/securom/unwrap.py --game GAME --out work/securom
"""
import argparse
import json
import os
import struct
import sys
import time

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'emu'))
sys.path.insert(0, HERE)

from probe import Boot, DISPATCH  # noqa: E402

IMAGE_BASE = 0x00400000
EXPECTED_STAMP = 0x3E434C37
TEXT_TAIL_VA = 0x0072F000
TEXT_TAIL_SIZE = 0x4000
OEP_VA = 0x00731118
OEP_WIN_VA = 0x00731018
OEP_WIN_SIZE = 0x200
ORIG_IMPORT_RVA = 0x00341874
ORIG_IMPORT_SIZE = 0x1B8
ORIG_IAT_RVA = 0x00339000
ORIG_IAT_SIZE = 0x11A4

# Known verification sites in SimJP.exe:
SITE_CREATE_MUTEX = 0x0048434F   # kernel32.dll!CreateMutexA
SITE_TAPP_CREATE = 0x0048440F    # TApplication.dll!?Create@TApplication@Toshi@@QAE_NPBDHQAPAD@Z
SITE_INITENV = 0x007311E2        # msvcrt.dll!__p___initenv (inside OEP window)


def read_original_iat(raw_exe):
    """Parse SimJP.exe's intact original import directory at RVA 0x341874.
    Returns {iat_slot_va: (dll_lower, import_name)}."""
    buf = bytearray(raw_exe)
    e_lfanew = struct.unpack_from('<I', buf, 0x3C)[0]
    opt_off = e_lfanew + 24
    struct.pack_into('<II', buf, opt_off + 96 + 1 * 8, ORIG_IMPORT_RVA, ORIG_IMPORT_SIZE)
    struct.pack_into('<II', buf, opt_off + 96 + 12 * 8, ORIG_IAT_RVA, ORIG_IAT_SIZE)
    pe = pefile.PE(data=bytes(buf))
    iat = {}
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode().lower()
        for imp in entry.imports:
            if imp.address:
                name = imp.name.decode() if imp.name else f'ordinal_{imp.ordinal}'
                iat[imp.address] = (dll, name)
    return iat


def find_redirected_sites(raw_exe):
    """Find all `ff 15 c0 ce d8 00` (`call [0xD8CEC0]`) call sites in .text."""
    pe = pefile.PE(data=raw_exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    code = text.get_data()
    text_va = base + text.VirtualAddress
    pat = b'\xff\x15' + struct.pack('<I', DISPATCH)
    sites = []
    pos = 0
    while True:
        i = code.find(pat, pos)
        if i < 0:
            break
        sites.append(text_va + i)
        pos = i + 1
    return sites


def recover_key9(emu, sites):
    """Recover the 9-byte SecuROM session key (0xE052B0..0xE052B8) directly from
    the 0xD8CEF0 hash table, the known call sites, and 0xD8B5A4..0xD8B5A8."""
    table_n = emu.r16(0xDAEA88)
    raws_all = [emu.r32(0xD8CEF0 + idx * 16) for idx in range(1, table_n + 1)]
    sites_p2 = {s + 2 for s in sites}

    # Step 1: recover bytes 0..6 (and low nibble of byte 6) by voting across raw_addr % 4.
    # For each entry: (raw_addr ^ ((MEM32(key + (raw_addr % 4)) << 4))) == site + 2.
    kw28 = []
    for r in range(4):
        raws = [v for v in raws_all if v != 0 and (v % 4) == r]
        votes = {}
        for raw in raws[:250]:
            nib = raw & 0xF
            for sp2 in sites_p2:
                if (sp2 & 0xF) == nib:
                    kw = (raw ^ sp2) >> 4
                    votes[kw] = votes.get(kw, 0) + 1
        top_kw, top_v = max(votes.items(), key=lambda kv: kv[1])
        if top_v < 20:
            raise RuntimeError(f'failed to recover key word r={r} (best votes={top_v})')
        kw28.append(top_kw)

    # Assemble bytes 0..5 from the overlapping 28-bit words:
    # kw28[0] gives bytes 0, 1, 2 (and low nibble of 3)
    # kw28[1] gives bytes 1, 2, 3 (and low nibble of 4)
    # kw28[2] gives bytes 2, 3, 4 (and low nibble of 5)
    # kw28[3] gives bytes 3, 4, 5 (and low nibble of 6)
    b0 = kw28[0] & 0xFF
    b1 = (kw28[0] >> 8) & 0xFF
    b2 = (kw28[0] >> 16) & 0xFF
    b3 = (kw28[1] >> 16) & 0xFF
    b4 = (kw28[2] >> 16) & 0xFF
    b5 = (kw28[3] >> 16) & 0xFF

    # Verify bytes 0..3 against encrypted .text start [0xD8B5A4]
    text_lo_dec = emu.r32(0xD8B5A4) ^ struct.unpack('<I', bytes([b0, b1, b2, b3]))[0]
    if not (0x464000 <= text_lo_dec < 0x465000):
        raise RuntimeError(f'recovered key[0..3] gave invalid text_lo 0x{text_lo_dec:08X}')

    # Bytes 6..7 from encrypted .text end [0xD8B5A8] (which decodes to 0x007311E4, in 0x730000..0x733000)
    # and kw28[3]'s high nibble check on byte 6:
    b6_lo = (kw28[3] >> 24) & 0xF
    enc_hi = emu.r32(0xD8B5A8)
    b6 = b7 = None
    for cand_b6_hi in range(16):
        cand_b6 = (cand_b6_hi << 4) | b6_lo
        for cand_b7 in range(256):
            val = enc_hi ^ struct.unpack('<I', bytes([b4, b5, cand_b6, cand_b7]))[0]
            if 0x00731000 <= val <= 0x00733000:
                b6, b7 = cand_b6, cand_b7
                break
        if b6 is not None:
            break
    if b6 is None:
        raise RuntimeError('failed to recover key[6..7] from 0xD8B5A8')

    # Step 2: recover byte 8 via the SecuROM hash function at 0xD5DB1A..0xD5DBB5.
    key8 = [b0, b1, b2, b3, b4, b5, b6, b7]
    sample_sites = [s + 2 for s in sites[:200]]
    table_next = [emu.r16(0xD8CEF4 + idx * 16) & 0x3FFF for idx in range(1, table_n + 1)]

    best_b8, best_hits = None, -1
    for b8 in range(256):
        k = bytes(key8 + [b8])
        hits = 0
        for sp2 in sample_sites:
            h = ((sp2 >> 16) & 0xFFFF) ^ (sp2 & 0xFFFF)
            h ^= (k[(h & 0xFF) % 9] << 8)
            h ^= k[((h >> 8) & 0xFF) % 9]
            idx = (h % table_n) + 1
            for _ in range(16):
                raw = raws_all[idx - 1]
                kw = struct.unpack_from('<I', k, raw % 4)[0]
                if ((raw ^ ((kw << 4) & 0xFFFFFFFF)) & 0xFFFFFFFF) == sp2:
                    hits += 1
                    break
                idx = table_next[idx - 1]
                if idx == 0:
                    break
        if hits > best_hits:
            best_hits, best_b8 = hits, b8

    if best_hits != len(sample_sites):
        raise RuntimeError(f'failed to recover key[8] cleanly ({best_hits}/{len(sample_sites)} hits)')
    return bytes(key8 + [best_b8])


def decrypt_product_id(emu):
    """Decrypt the 14-byte SecuROM product ID at 0xD8B518 (loop at 0xD63352)."""
    n = emu.r32(0xD8B5A0)
    buf = bytearray(emu.read(0xD8B518, n + 1))
    for i in range(n - 1, -1, -1):
        buf[i] ^= buf[i + 1]
    return bytes(buf[:n])


def apply_switch_op(x, sp2, op, consts, oep_enc64):
    if op in (0, 1, 2, 3, 6, 7):
        return (x ^ consts[op]) & 0xFFFFFFFF
    if op in (4, 8):
        return (x - consts[op]) & 0xFFFFFFFF
    if op in (5, 9):
        return (x + consts[op]) & 0xFFFFFFFF
    if op == 10:
        return (x ^ struct.unpack_from('<I', oep_enc64, sp2 % 0x25)[0]) & 0xFFFFFFFF
    if op == 11:
        return x
    raise ValueError(op)


def inv_switch_op(x, sp2, op, consts, oep_enc64):
    if op in (0, 1, 2, 3, 6, 7):
        return (x ^ consts[op]) & 0xFFFFFFFF
    if op in (4, 8):
        return (x + consts[op]) & 0xFFFFFFFF
    if op in (5, 9):
        return (x - consts[op]) & 0xFFFFFFFF
    if op == 10:
        return (x ^ struct.unpack_from('<I', oep_enc64, sp2 % 0x25)[0]) & 0xFFFFFFFF
    if op == 11:
        return x
    raise ValueError(op)


def unwrap(game, out_dir, root='work/securom/emu'):
    t0 = time.time()
    exe_path = os.path.join(game, 'SimJP.exe')
    raw_exe = open(exe_path, 'rb').read()
    pe = pefile.PE(data=raw_exe, fast_load=True)
    if pe.FILE_HEADER.TimeDateStamp != EXPECTED_STAMP:
        raise SystemExit(f'{exe_path}: PE timestamp 0x{pe.FILE_HEADER.TimeDateStamp:08X}, expected 0x{EXPECTED_STAMP:08X}')

    iat_to_imp = read_original_iat(raw_exe)
    imp_to_iat = {(dll, name): slot for slot, (dll, name) in iat_to_imp.items()}
    sites = find_redirected_sites(raw_exe)
    print(f'Surveyed {exe_path}: {len(iat_to_imp)} original IAT entries, {len(sites)} redirected sites')

    # 1. Boot SecuROM under Unicorn until dispatcher is installed (~1s, no CD needed).
    b = Boot(game, iso=None, disc=None, root=root, verbose=False, stop_at='dispatch', quiet=True)
    b.run()
    emu = b.emu
    if emu.r32(DISPATCH) != 0x00D5DA00:
        raise RuntimeError(f'dispatcher not installed: [{DISPATCH:#x}] = {emu.r32(DISPATCH):#x}')

    # 2. Decrypt product ID at 0xD8B518 and recover the 9-byte session key.
    product_id = decrypt_product_id(emu)
    key9 = recover_key9(emu, sites)
    print(f'Recovered product ID {product_id!r} and 9-byte key {key9.hex(" ")}')

    # 3. Capture the 64 encrypted OEP bytes (used by switch case 10) and decrypt the 512-byte OEP window.
    oep_enc64 = emu.read(OEP_VA, 64)
    text_tail = bytearray(emu.read(TEXT_TAIL_VA, TEXT_TAIL_SIZE))
    oep_off = OEP_WIN_VA - TEXT_TAIL_VA
    oep_enc512 = bytes(text_tail[oep_off:oep_off + OEP_WIN_SIZE])
    for j in range(OEP_WIN_SIZE):
        pos = 0x18 + j
        text_tail[oep_off + j] = ((oep_enc512[j] ^ key9[pos % 9]) + product_id[pos % len(product_id)]) & 0xFF

    # Verify mainCRTStartup prologue at 0x731118
    if bytes(text_tail[OEP_VA - TEXT_TAIL_VA:OEP_VA - TEXT_TAIL_VA + 5]) != b'\x55\x8b\xec\x6a\xff':
        raise RuntimeError('OEP decryption failed: unexpected bytes at 0x731118')

    # 4. Collect switch constants directly from SecuROM's initialized state.
    consts = {
        0: 0,                                                       # solved below via TApplication::Create
        1: struct.unpack_from('<I', product_id, 0)[0],              # MEM32(0xD8B518)
        2: emu.r32(0xD8B580),                                       # 0x00000000
        3: emu.r32(0xD8B560),                                       # 0x780D1CDD
        4: emu.r16(emu.r32(0xE016F0)),                              # 0xEA1A
        5: emu.r16(emu.r32(0xE078D0)),                              # 0xD520
        6: emu.r16(emu.r32(0xE00D68)),                              # 0xFE80
        7: emu.r32(emu.r32(0xE06924)),                              # 0x0000A512
        8: emu.r32(emu.r32(0xE04180)),                              # 0x00074F21
        9: 0xFD,                                                    # hardcoded at 0xD5797D
        10: 0,
        11: 0,
    }

    # Look up entries via SecuROM's exact hash function (0xD5DB1A..0xD5DC18)
    # so dummy/decoy slots in 0xD8CEF0 are never picked over the real chain entry.
    table_n = emu.r16(0xDAEA88)

    def lookup_entry(site):
        sp2 = site + 2
        h = ((sp2 >> 16) & 0xFFFF) ^ (sp2 & 0xFFFF)
        h ^= (key9[(h & 0xFF) % 9] << 8)
        h ^= key9[((h >> 8) & 0xFF) % 9]
        idx = (h % table_n) + 1
        for _ in range(64):
            raw = emu.r32(0xD8CEF0 + idx * 16)
            kw = struct.unpack_from('<I', key9, raw % 4)[0]
            if ((raw ^ ((kw << 4) & 0xFFFFFFFF)) & 0xFFFFFFFF) == sp2:
                enc = emu.r32(0xD8CEFA + idx * 16)
                lo = sp2 & 0xFFFF
                sel = (((lo << 16) + lo) ^ struct.unpack_from('<I', key9, sp2 % 4)[0]) & 0xFFFFFFFF
                ops = [((sel >> (4 * i)) & 0xF) % 12 for i in range(8)]
                return enc, ops
            idx = emu.r16(0xD8CEF4 + idx * 16) & 0x3FFF
            if idx == 0:
                break
        raise RuntimeError(f'site 0x{site:08X} not found in SecuROM hash table')

    # Solve C0 (16-bit) from known site SITE_TAPP_CREATE (which uses op 0 once).
    tapp_slot = imp_to_iat[('tapplication.dll', '?Create@TApplication@Toshi@@QAE_NPBDHQAPAD@Z')]
    enc_tapp, ops_tapp = lookup_entry(SITE_TAPP_CREATE)
    pos0 = ops_tapp.index(0)
    x = enc_tapp
    for op in ops_tapp[:pos0]:
        x = apply_switch_op(x, SITE_TAPP_CREATE + 2, op, consts, oep_enc64)
    y = tapp_slot
    for op in reversed(ops_tapp[pos0 + 1:]):
        y = inv_switch_op(y, SITE_TAPP_CREATE + 2, op, consts, oep_enc64)
    consts[0] = (x ^ y) & 0xFFFF

    # 5. Decode every call site and verify known anchors.
    def decode_site(site):
        sp2 = site + 2
        enc, ops = lookup_entry(site)
        v = enc
        for op in ops:
            v = apply_switch_op(v, sp2, op, consts, oep_enc64)
        return v

    # Cross-check CreateMutexA and __p___initenv
    cm_slot = decode_site(SITE_CREATE_MUTEX)
    if iat_to_imp.get(cm_slot) != ('kernel32.dll', 'CreateMutexA'):
        raise RuntimeError(f'verification failed: 0x{SITE_CREATE_MUTEX:08X} -> 0x{cm_slot:08X} ({iat_to_imp.get(cm_slot)})')
    initenv_slot = decode_site(SITE_INITENV)
    if iat_to_imp.get(initenv_slot) != ('msvcrt.dll', '__p___initenv'):
        raise RuntimeError(f'verification failed: 0x{SITE_INITENV:08X} -> 0x{initenv_slot:08X} ({iat_to_imp.get(initenv_slot)})')

    # Patch the redirected call inside the decrypted OEP window (0x7311E2: call [0xD8CEC0] -> call [0x739114])
    initenv_off = (SITE_INITENV + 2) - TEXT_TAIL_VA
    if struct.unpack_from('<I', text_tail, initenv_off)[0] != DISPATCH:
        raise RuntimeError(f'expected dispatcher call at 0x{SITE_INITENV:08X}')
    struct.pack_into('<I', text_tail, initenv_off, initenv_slot)

    resolved = {}
    failed = []
    for site in sites:
        slot = decode_site(site)
        if slot in iat_to_imp:
            dll, name = iat_to_imp[slot]
            resolved[site] = [dll, name]
        else:
            failed.append((site, f'decoded 0x{slot:08X} not in original IAT'))

    if failed:
        raise RuntimeError(f'{len(failed)} sites failed to resolve (first: {failed[:5]})')

    os.makedirs(out_dir, exist_ok=True)
    sites_path = os.path.join(out_dir, 'sites.json')
    with open(sites_path, 'w') as f:
        json.dump({
            'resolved': len(resolved),
            'failed': 0,
            'failed_sites': [],
            'sites': {f'{k:#x}': v for k, v in sorted(resolved.items())},
        }, f, indent=2)

    tail_path = os.path.join(out_dir, 'text_tail.bin')
    with open(tail_path, 'wb') as f:
        f.write(text_tail)

    print(f'Unwrapped SimJP.exe in {time.time() - t0:.2f}s: {len(resolved)}/{len(sites)} sites + 16 KB .text tail')
    print(f'  wrote {sites_path}')
    print(f'  wrote {tail_path}')
    return sites_path, tail_path


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', required=True, help='the game folder (from tools/install/install.py)')
    ap.add_argument('--out', default='work/securom', help='output directory for sites.json and text_tail.bin')
    ap.add_argument('--root', default='work/securom/emu', help='staging directory for Unicorn emulator')
    a = ap.parse_args()
    unwrap(a.game, a.out, a.root)


if __name__ == '__main__':
    main()
