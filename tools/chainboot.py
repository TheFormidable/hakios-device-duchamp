#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image -- duchamp's: header v4 (GKI).

  chainboot.py <android-boot.img> <program.bin> <out.img> [payload.bin]

The reference (hakios-mt6768) did this to a header v2 image whose kernel was gzip. duchamp's boot image is header v4: a
4096-byte header with no load addresses, the kernel (Image.lz4, LZ4 legacy frames), an empty ramdisk (the ramdisks are in
vendor_boot and init_boot, which are not touched) and the GKI boot signature. LK decompresses the kernel and enters it.

The new kernel is, compressed the way Android's was: program.bin, padded to CHAIN_OFFSET, then Android's own (uncompressed)
Image, then the optional payload (mtkloader's "XNUP"), 16-aligned. The program's arm64 header gets the Image's length in res2
(0x28), the payload's offset in res3 (0x30) and a covering image_size (0x10). The program moves Android's Image to a 2 MB
boundary and starts it (mtkloader.c, chain_android). Everything else of the image is copied as it is.

AVB: a boot_a read off the phone is the whole 64 MB partition, ending in an AVB footer ("AVBf", its last 64 bytes) that points
at the partition's own vbmeta, written right after the original image (on Isaac's phone at 0xfcd000). The new image is larger
than the old one, so flashing it alone would overwrite that vbmeta with kernel bytes and leave the old footer pointing at
garbage: libavb calls that invalid metadata, which an unlocked bootloader does not forgive. So, as magiskboot does, the output
is the whole partition again: the new image, the original vbmeta blob (unchanged: it stays validly signed, only its hash of
the image no longer matches, which is the verification error an unlocked, orange-state phone accepts) right after it, and a
new footer at the end that says where.

What LK does with an image larger than Android's own is not known yet: the check below refuses one whose uncompressed blob,
from the load address, would reach LK's device tree (BoardConfig: kernel base 0x3fff8000 + tags offset 0x07c88000).
"""
import gzip
import os
import struct
import sys

CHAIN_OFFSET = 0x40000
BOOT_PARTITION = 64 << 20               # BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864
LOAD_PA = int(os.environ.get('CHAIN_LOAD_PA', '0x40000000'), 0)   # where LK puts the kernel (platform.h): to be confirmed
DTB_PA = int(os.environ.get('CHAIN_DTB_PA', '0x47c80000'), 0)     # where LK puts its tree (BoardConfig): to be confirmed
LZ4_LEGACY = b'\x02\x21\x4c\x18'
LZ4_CHUNK = 8 << 20                     # the legacy format's block, as Linux's and LK's decompressors expect it


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def lz4_legacy_decompress(d):
    import lz4.block
    out, at = bytearray(), 4
    while at + 4 <= len(d):
        size = struct.unpack_from('<I', d, at)[0]
        if size == struct.unpack('<I', LZ4_LEGACY)[0]:      # a second frame, as `cat a.lz4 b.lz4` makes
            at += 4
            continue
        if size == 0 or at + 4 + size > len(d):
            break                                           # the padding behind the last block
        out += lz4.block.decompress(d[at + 4:at + 4 + size], uncompressed_size=LZ4_CHUNK)
        at += 4 + size
    return bytes(out)


def lz4_legacy_compress(b):
    import lz4.block
    out = bytearray(LZ4_LEGACY)
    for at in range(0, len(b), LZ4_CHUNK):
        block = lz4.block.compress(b[at:at + LZ4_CHUNK], mode='high_compression', compression=12, store_size=False)
        out += struct.pack('<I', len(block)) + block
    return bytes(out)


def unpack_kernel(kernel):
    if kernel[:4] == LZ4_LEGACY:
        return lz4_legacy_decompress(kernel), 'lz4'
    if kernel[:2] == b'\x1f\x8b':
        return gzip.decompress(kernel), 'gzip'
    return kernel, 'raw'


def pack_kernel(blob, how):
    if how == 'lz4':
        return lz4_legacy_compress(blob)
    if how == 'gzip':
        return gzip.compress(blob, 9, mtime=0)
    return blob


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    src, prog_path, out = sys.argv[1:4]
    payload = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ksize, rsize, _os_version, header_size = struct.unpack_from('<4I', d, 8)
    hver = struct.unpack_from('<I', d, 40)[0]
    if hver not in (3, 4):
        raise SystemExit(f'{src}: header v{hver}; this is the header v3/v4 tool (the X6826B\'s v2 one is hakios-mt6768\'s)')
    page = 4096
    footer = d[-64:] if d[-64:-60] == b'AVBf' else None
    sig_size = struct.unpack_from('<I', d, 1580)[0] if hver == 4 else 0

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    signature = d[off:off + sig_size]

    image, how = unpack_kernel(kernel)
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    total = (total + 0xfffff) & ~0xfffff
    struct.pack_into('<Q', prog, 0x10, total)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    text_offset = struct.unpack_from('<Q', prog, 0x08)[0]
    # the program may move itself up by its text_offset (start.S), so that much more has to be clear of LK's tree
    if LOAD_PA + text_offset + total > DTB_PA:
        raise SystemExit(f'{out}: {total:#x} bytes from {LOAD_PA:#x} (+{text_offset:#x}) reach LK\'s device tree at '
                         f'{DTB_PA:#x}: the payload has to shrink (or CHAIN_DTB_PA is not this phone\'s)')
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = pack_kernel(blob, how)

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    img = bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) + pad(signature, page)
    avb = 'no AVB footer in the source (flash it only if the phone took such images before)'
    if footer:
        major, minor, orig_size, vb_off, vb_size = struct.unpack('>IIQQQ', footer[4:36])
        vbmeta = d[vb_off:vb_off + vb_size]
        assert vbmeta[:4] == b'AVB0', 'the AVB footer does not point at a vbmeta blob'
        part_size = len(d)
        image_size = len(img)
        img = pad(img, page) + vbmeta
        if len(img) > part_size - 64:
            raise SystemExit(f'{out}: no room for the vbmeta and the footer in {part_size} bytes')
        new_footer = struct.pack('>4sIIQQQ', b'AVBf', major, minor, image_size, (image_size + page - 1) // page * page,
                                 vb_size) + b'\0' * 28
        img = img + b'\0' * (part_size - 64 - len(img)) + new_footer
        avb = (f'AVB: vbmeta moved {vb_off:#x} -> {(image_size + page - 1) // page * page:#x}, footer rewritten, '
               f'{part_size} bytes (the whole partition)')
    # 64 MB: fastboot refuses a larger one, and a script that sends its output nowhere would carry on as if it had been flashed
    if len(img) > BOOT_PARTITION:
        raise SystemExit(f'{out}: {len(img)} bytes, more than the {BOOT_PARTITION} of the boot partition '
                         f'({len(img) - BOOT_PARTITION} too many): the payload has to shrink')
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes, header v{hver} (program {len(prog)}, Image {len(image)}, payload {len(payload)}, '
          f'kernel {how} {len(new_kernel)}, {total:#x} bytes once LK has unpacked it)\n{avb}')


if __name__ == '__main__':
    main()
