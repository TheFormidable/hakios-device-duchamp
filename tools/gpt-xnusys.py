#!/usr/bin/env python3
"""
The new GPT of duchamp's LUN 2 with xnusys: 8 GiB off the end of userdata. Nothing touches the phone.

    tools/gpt-xnusys.py DIR

DIR holds what windows/hakios-xnusys.ps1 option 1 read off the phone: lun2-start.bin and lun2-end.bin (the first and
the last 64 blocks of 4 KB of the LUN) and info.txt (blocks=, userdata_start=, userdata_blocks= from the kernel's view).
Writes into DIR:

    lun2-start.new  the primary header and its entry array: LUN blocks 1 .. the array's last (option 2 writes it at 1)
    lun2-end.new    the backup entry array and header: up to the LUN's last block (option 2 writes it at the end)
    plan.txt        what changes, in Spanish for the owner (# lines), and the numbers option 2 checks

Everything is checked first: both headers and their CRCs, the arrays the same in both, userdata where the kernel says it
is, the new range inside userdata and clear of every other partition. Only userdata's last block changes and one empty
entry becomes xnusys; every other entry, the disk GUID and the entry order stay as they were. The carve is
tools/gpt-carve.py of hakios-mt6768 (512-byte sectors, the X6826B's eMMC) for 4 KB blocks.

duchamp's table has no empty entry: 90 partitions in an array of exactly 90 (3 blocks, room for 96). Then xnusys is
entry 90 and the headers say 91 entries. The array stays where it is and in the same 3 blocks, first and last usable
block do not move, and the bytes it grows into must be zeros in both copies (they are checked).
"""
import binascii
import hashlib
import os
import struct
import sys
import uuid

BS = 4096
REGION = 64                                  # blocks read at each end of the LUN
SHRINK = 'userdata'
NEW_NAME = 'xnusys'
NEW_BLOCKS = 8 * 1024 * 1024 * 1024 // BS    # 8 GiB
ALIGN = 1024 * 1024 // BS                    # 1 MiB
NEW_TYPE = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')     # Linux filesystem data, as on the X6826B
EMPTY = b'\0' * 16


def crc32(b):
    return binascii.crc32(b) & 0xffffffff


def name_of(e):
    return bytes(e[56:128]).decode('utf-16-le').split('\0', 1)[0]


def header(block, where):
    sig, rev, hsize, hcrc, _r, mylba, altlba, first, last = struct.unpack_from('<8sIIIIQQQQ', block, 0)
    if sig != b'EFI PART':
        sys.exit(f'gpt-xnusys: no GPT header in the {where} one')
    if not 92 <= hsize <= BS:
        sys.exit(f'gpt-xnusys: the {where} header says it is {hsize} bytes')
    h = bytearray(block[:hsize])
    struct.pack_into('<I', h, 16, 0)
    if crc32(bytes(h)) != hcrc:
        sys.exit(f'gpt-xnusys: the {where} header does not match its CRC')
    entlba, nent, esz, entcrc = struct.unpack_from('<QIII', block, 72)
    return dict(rev=rev, hsize=hsize, mylba=mylba, altlba=altlba, first=first, last=last, guid=bytes(block[56:72]),
                entlba=entlba, nent=nent, esz=esz, entcrc=entcrc)


def new_header(block, h, nent, entries_crc):
    out = bytearray(block)
    struct.pack_into('<I', out, 16, 0)
    struct.pack_into('<I', out, 80, nent)
    struct.pack_into('<I', out, 88, entries_crc)
    struct.pack_into('<I', out, 16, crc32(bytes(out[:h['hsize']])))
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    info = dict(l.strip().split('=', 1) for l in open(os.path.join(d, 'info.txt'), encoding='utf-8-sig') if '=' in l)
    blocks = int(info['blocks'])
    start = open(os.path.join(d, 'lun2-start.bin'), 'rb').read()
    end = open(os.path.join(d, 'lun2-end.bin'), 'rb').read()
    if len(start) != REGION * BS or len(end) != REGION * BS:
        sys.exit('gpt-xnusys: the two regions are not 64 blocks each')
    end_at = blocks - REGION                                     # the LUN block lun2-end.bin starts at

    def lun(lba, n=1):                                           # n blocks of the LUN, from either region
        if lba + n <= REGION:
            return start[lba * BS:(lba + n) * BS]
        if lba >= end_at and lba + n <= blocks:
            return end[(lba - end_at) * BS:(lba - end_at + n) * BS]
        sys.exit(f'gpt-xnusys: block {lba} is in neither region that was read')

    p = header(lun(1), 'primary')
    s = header(lun(blocks - 1), 'backup')
    if p['mylba'] != 1 or p['altlba'] != blocks - 1 or s['mylba'] != blocks - 1 or s['altlba'] != 1:
        sys.exit(f'gpt-xnusys: the headers do not point at each other and the LUN\'s ends ({p["altlba"]}, {blocks - 1})')
    for k in ('first', 'last', 'guid', 'nent', 'esz', 'entcrc', 'rev', 'hsize'):
        if p[k] != s[k]:
            sys.exit(f'gpt-xnusys: the primary and backup headers differ in {k}')
    if p['esz'] != 128:
        sys.exit(f'gpt-xnusys: entries of {p["esz"]} bytes')
    nblk = (p['nent'] * p['esz'] + BS - 1) // BS
    if p['entlba'] + nblk > p['first'] or s['entlba'] <= p['last'] or s['entlba'] + nblk > blocks - 1:
        sys.exit('gpt-xnusys: the entry arrays overlap the usable blocks')
    parr = lun(p['entlba'], nblk)[:p['nent'] * p['esz']]
    sarr = lun(s['entlba'], nblk)[:p['nent'] * p['esz']]
    if crc32(parr) != p['entcrc'] or parr != sarr:
        sys.exit('gpt-xnusys: the entry arrays do not match their CRC, or each other')

    ents = [bytearray(parr[i * 128:(i + 1) * 128]) for i in range(p['nent'])]
    used = [(i, name_of(e), *struct.unpack_from('<QQ', e, 32)) for i, e in enumerate(ents) if e[0:16] != EMPTY]
    names = [u[1] for u in used]
    if NEW_NAME in names:
        sys.exit(f'gpt-xnusys: there is a {NEW_NAME} already')
    if names.count(SHRINK) != 1:
        sys.exit(f'gpt-xnusys: {names.count(SHRINK)} partitions named {SHRINK}')
    v = next(u for u in used if u[1] == SHRINK)
    vi, _n, vstart, vend = v
    if vstart != int(info['userdata_start']) or vend - vstart + 1 != int(info['userdata_blocks']):
        sys.exit(f'gpt-xnusys: userdata is {vstart}+{vend - vstart + 1} in the GPT but '
                 f'{info["userdata_start"]}+{info["userdata_blocks"]} to Android\'s kernel')
    free = next((i for i, e in enumerate(ents) if e[0:16] == EMPTY and e == bytearray(128)), None)
    nent = p['nent']
    if free is None:                                             # one more entry, in the array's own blocks
        room = nblk * BS // p['esz']
        grow = slice(nent * p['esz'], (nent + 1) * p['esz'])
        if nent + 1 > room:
            sys.exit(f'gpt-xnusys: no empty entry left and no room for one more ({nent} of {room})')
        if any(lun(p['entlba'], nblk)[grow]) or any(lun(s['entlba'], nblk)[grow]):
            sys.exit('gpt-xnusys: no empty entry left, and the bytes after the array are not zeros')
        free = nent
        nent += 1
        ents.append(bytearray(128))

    new_start = (vend + 1 - NEW_BLOCKS) // ALIGN * ALIGN
    new_end = vend
    if new_start <= vstart + 1024 * ALIGN:
        sys.exit('gpt-xnusys: userdata would be left with less than 1 GB')
    if not (p['first'] <= new_start and new_end <= p['last']):
        sys.exit('gpt-xnusys: the new range is outside the usable blocks')
    for i, nm, a, b in used:
        if i != vi and not (b < new_start or a > new_end):
            sys.exit(f'gpt-xnusys: {nm} ({a}..{b}) is in the way')

    struct.pack_into('<Q', ents[vi], 40, new_start - 1)
    e = ents[free]
    e[0:16] = NEW_TYPE.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', e, 32, new_start, new_end, 0)
    e[56:128] = NEW_NAME.encode('utf-16-le').ljust(72, b'\0')
    arr = b''.join(bytes(x) for x in ents)
    if (len(arr) + BS - 1) // BS != nblk:
        sys.exit('gpt-xnusys: the array would change size in blocks')
    arr_blocks = arr + lun(p['entlba'], nblk)[len(arr):]         # what follows the array in its last block, as it was
    acrc = crc32(arr)

    # primary: block 1 (the header) .. the array's last block; anything between them as it was
    pri = bytearray(lun(1, p['entlba'] + nblk - 1))
    pri[0:BS] = new_header(lun(1), p, nent, acrc)
    pri[(p['entlba'] - 1) * BS:(p['entlba'] - 1 + nblk) * BS] = arr_blocks
    # backup: its array's first block .. the LUN's last (the header)
    sec = bytearray(lun(s['entlba'], blocks - s['entlba']))
    sec[0:nblk * BS] = arr + lun(s['entlba'], nblk)[len(arr):]
    sec[-BS:] = new_header(lun(blocks - 1), s, nent, acrc)

    # read back as a GPT reader would: every changed byte accounted for
    np_, ns = header(bytes(pri[0:BS]), 'new primary'), header(bytes(sec[-BS:]), 'new backup')
    for k in ('mylba', 'altlba', 'first', 'last', 'guid', 'entlba', 'esz', 'rev', 'hsize'):
        assert np_[k] == p[k] and ns[k] == s[k], k
    assert np_['nent'] == ns['nent'] == nent and np_['entcrc'] == ns['entcrc'] == acrc
    assert crc32(bytes(pri[(p['entlba'] - 1) * BS:][:nent * 128])) == acrc == crc32(bytes(sec[:nent * 128]))
    old = [parr[i * 128:(i + 1) * 128] for i in range(p['nent'])] + [bytes(128)] * (nent - p['nent'])
    changed = [i for i in range(nent) if bytes(ents[i]) != old[i]]
    assert sorted(changed) == sorted([vi, free]), changed

    open(os.path.join(d, 'lun2-start.new'), 'wb').write(pri)
    open(os.path.join(d, 'lun2-end.new'), 'wb').write(sec)
    gb = lambda n: f'{n * BS / 2**30:.1f} GB'
    plan = [
        '# Tabla nueva del LUN 2 del POCO X6 Pro: solo cambian userdata (se acorta) y xnusys (nueva).',
        f'# userdata: bloques {vstart}..{vend} ({gb(vend - vstart + 1)})  ->  {vstart}..{new_start - 1} ({gb(new_start - vstart)})',
        f'# xnusys:   bloques {new_start}..{new_end} ({gb(new_end - new_start + 1)}), entrada {free}'
        + (f' (la tabla pasa de {p["nent"]} a {nent} entradas, en los mismos bloques)' if nent != p['nent'] else ''),
        f'# las otras {len(used) - 1} particiones, sin cambios',
        f'blocks={blocks}',
        f'start_new_blocks={len(pri) // BS}',
        f'end_new_at={s["entlba"]}',
        f'start_new_sha256={hashlib.sha256(pri).hexdigest()}',
        f'end_new_sha256={hashlib.sha256(sec).hexdigest()}',
        f'xnusys_bytes={(new_end - new_start + 1) * BS}',
        f'userdata_bytes={(new_start - vstart) * BS}',
    ]
    open(os.path.join(d, 'plan.txt'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(plan) + '\n')
    print('\n'.join(plan))
    print(f'gpt-xnusys: {len(used)} partitions; lun2-start.new {len(pri) // BS} blocks at 1, lun2-end.new '
          f'{len(sec) // BS} blocks at {s["entlba"]}')


if __name__ == '__main__':
    main()
