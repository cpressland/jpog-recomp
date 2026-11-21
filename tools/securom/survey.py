"""What SecuROM did to SimJP.exe, checked and written down for the steps after.

    .venv/bin/python -I tools/securom/survey.py GAME/SimJP.exe work/securom/survey.json

The disc's SimJP.exe (1.00.0000) is wrapped by SecuROM 4.x. Everything this
expects of it is checked, and any other build fails here rather than later:

* `.cms_t` (code) and `.cms_d` (data) are SecuROM's. The entry point and the
  MSVC CRT start-up are in `.cms_t`, in the clear; it calls SecuROM's own
  WinMain, not the game's. 42 KB of `.cms_t` is encrypted (ENCRYPTED), and
  nothing outside `.cms_t` points into it.
* `.text` is the game, in the clear. Its original import address table is
  intact in `.rdata` (ORIGINAL_IAT): unbound, each slot the RVA of its
  hint/name, one block of slots a DLL. The PE's import directory is
  SecuROM's own copy, in `.idata`, with the imports its own code adds.
* 3,642 of the game's `call [IAT slot]` are `call [DISPATCH]` instead: one
  slot, `0xAAAAAAAA` on disk, that SecuROM fills with a dispatcher at run
  time. The dispatcher picks the import by the call's return address. Which
  import each of those sites called is what tools/securom/resolve.py finds.
* The game's own main is GAME_MAIN, `int main(int argc, char **argv)`-shaped,
  in `.text`; the host enters there and SecuROM's start-up never runs.

The output: the sections, the original IAT as slot -> [dll, name], the
dispatcher, and every redirected call site (the address of its `ff 15`).
"""
import collections
import json
import math
import os
import re
import struct
import sys

import pefile

GAME_MD5 = '6cc8989f299f64524577915a4a0d91b0'
ORIGINAL_IAT = 0x739000
DISPATCH = 0xD8CEC0
DISPATCH_FILL = 0xAAAAAAAA
GAME_MAIN = 0x484330
ENCRYPTED = (0xD57800, 0xD61D00)
SITES = 3642
SECTIONS = ('.text', 'PSGSFD00','PSGSFD_I', 'PSGSFD_P', 'PSGSFD_B', '.rdata', '.data', '.data1',
            '.cms_t', '.cms_d', '.idata', '.rsrc')


class Mismatch(Exception):
    pass


def expect(ok, what):
    if not ok:
        raise Mismatch(what)


def entropy(b):
    c = collections.Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values()) if n else 0.0


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.sections = {s.Name.rstrip(b'\0').decode(): (self.base + s.VirtualAddress,
                                                         self.base + s.VirtualAddress + s.Misc_VirtualSize)
                         for s in self.pe.sections}

    def u32(self, va):
        return struct.unpack_from('<I', self.img, va - self.base)[0]

    def bytes(self, lo, hi):
        return self.img[lo - self.base:hi - self.base]

    def section_of(self, va):
        for n, (lo, hi) in self.sections.items():
            if lo <= va < hi:
                return n
        return None

    def name_at(self, rva):
        """An import's name from its hint/name RVA, or None."""
        if not 0x1000 < rva < len(self.img) - 2:
            return None
        n = self.img[rva + 2:rva + 256].split(b'\0')[0]
        return n.decode() if n and all(32 < c < 127 for c in n) else None


def original_iat(im):
    """slot -> (dll, name), the blocks told apart by their zero ends and named
    by the DLLs SecuROM's import directory lists each name under."""
    owners = collections.defaultdict(set)
    for d in im.pe.DIRECTORY_ENTRY_IMPORT:
        for i in d.imports:
            owners[(i.name or b'#%d' % i.ordinal).decode()].add(d.dll.decode().lower())
    blocks, cur, a = [], [], ORIGINAL_IAT
    while True:
        v = im.u32(a)
        if v == 0:
            if cur:
                blocks.append(cur)
            cur = []
        elif v & 0x80000000:
            cur.append((a, '#%d' % (v & 0xFFFF)))
        elif im.name_at(v):
            cur.append((a, im.name_at(v)))
        else:
            break
        a += 4
    expect(cur == [], 'the original IAT ends mid-block')
    iat = {}
    for block in blocks:
        votes = collections.Counter(dll for _, n in block for dll in owners.get(n, ()))
        expect(votes, f'an IAT block at {block[0][0]:#x} names no DLL SecuROM imports')
        dll, n = votes.most_common(1)[0]
        expect(n >= len(block) - 1, f'the IAT block at {block[0][0]:#x} is not one DLL\'s')
        for slot, name in block:
            iat[slot] = (dll, name)
    return iat


def survey(path):
    im = Image(path)
    expect(tuple(im.sections) == SECTIONS, f'sections {tuple(im.sections)}')
    ep = im.base + im.pe.OPTIONAL_HEADER.AddressOfEntryPoint
    expect(im.section_of(ep) == '.cms_t', f'the entry point {ep:#x} is not in .cms_t')

    enc = im.bytes(*ENCRYPTED)
    expect(min(entropy(enc[o:o + 4096]) for o in range(0, len(enc) - 4095, 4096)) > 7.5,
           'the encrypted part of .cms_t is not where it was')

    iat = original_iat(im)
    dlls = collections.Counter(d for d, _ in iat.values())

    lo, hi = im.sections['.text']
    text = im.bytes(lo, hi)
    pat = b'\xff\x15' + struct.pack('<I', DISPATCH)
    sites = [lo + m.start() for m in re.finditer(re.escape(pat), text)]
    expect(len(sites) == SITES, f'{len(sites)} redirected calls, not {SITES}')
    expect(im.img.count(struct.pack('<I', DISPATCH)) == SITES, 'the dispatcher slot is used other than by calls')
    expect(im.u32(DISPATCH) == DISPATCH_FILL, 'the dispatcher slot is not empty on disk')

    # The game's own calls through its IAT, untouched: how many, and to what.
    direct = collections.Counter()
    for m in re.finditer(rb'\xff[\x15\x25]', text):
        if m.start() + 6 <= len(text):
            slot = struct.unpack_from('<I', text, m.start() + 2)[0]
            if slot in iat:
                direct[slot] += 1

    # The game's main: CreateMutexA through the dispatcher, then TApplication.
    main = im.bytes(GAME_MAIN, GAME_MAIN + 0x120)
    expect(main.startswith(bytes.fromhex('558bec83ec03')), 'the game\'s main is not where it was')
    expect(GAME_MAIN + 0x1F in sites and GAME_MAIN + 0xDF in sites, 'the game\'s main lost its redirected calls')

    return {
        'exe': os.path.basename(path), 'base': im.base, 'entry': ep, 'game_main': GAME_MAIN,
        'sections': {n: [a, b] for n, (a, b) in im.sections.items()},
        'encrypted': list(ENCRYPTED),
        'dispatch': DISPATCH,
        'iat': {f'{s:#x}': list(v) for s, v in sorted(iat.items())},
        'iat_dlls': dict(sorted(dlls.items())),
        'direct_calls': sum(direct.values()),
        'sites': sites,
    }


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    import hashlib
    with open(sys.argv[1], 'rb') as f:
        got = hashlib.md5(f.read()).hexdigest()
    if got != GAME_MD5:
        sys.exit(f'{sys.argv[1]}: MD5 {got}, not the SimJP.exe this knows ({GAME_MD5})')
    try:
        s = survey(sys.argv[1])
    except Mismatch as e:
        sys.exit(f'SimJP.exe is not as expected: {e}')
    os.makedirs(os.path.dirname(os.path.abspath(sys.argv[2])), exist_ok=True)
    with open(sys.argv[2], 'w') as f:
        json.dump(s, f, indent=1)
    print(f'{len(s["iat"])} imports in the original IAT ({len(s["iat_dlls"])} DLLs), '
          f'{s["direct_calls"]} calls through it, {len(s["sites"])} through SecuROM\'s dispatcher')


if __name__ == '__main__':
    main()
