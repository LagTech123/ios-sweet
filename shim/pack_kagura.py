#!/usr/bin/env python3
"""Pack a kagura shim Image into the stock Xperia boot.img v0 header.

With an optional third argument, the file is a payload embedded in the
ramdisk and /chosen linux,initrd-start/end are patched to point at it.
SWEETXNU payloads use normal gzip; KAG2TWRP payloads use gzip made of stored
deflate blocks, which the shim can unpack in place if ABL did not already
decompress the ramdisk.
"""
import gzip
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

STOCK = '/mnt/d/xz/dump/boot.img'


def gzip_stored(data):
    """Valid gzip stream made only of stored deflate blocks; the shim can
    unpack it in place with ~20 lines of code."""
    out = bytearray(b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03')
    for off in range(0, len(data), 65535):
        chunk = data[off:off + 65535]
        final = 1 if off + len(chunk) == len(data) else 0
        out.append(final)
        out += struct.pack('<HH', len(chunk), 0xffff ^ len(chunk))
        out += chunk
    out += struct.pack('<II', zlib.crc32(data) & 0xffffffff, len(data) & 0xffffffff)
    return bytes(out)


def patch_dtb(dtb, rd_start, rd_end):
    """Return dtb with /chosen linux,initrd-start/end as big-endian u64 cells."""
    with tempfile.NamedTemporaryFile(suffix='.dtb', delete=False) as f:
        tmp = f.name
        f.write(dtb)
    try:
        for prop, value in (('linux,initrd-start', rd_start), ('linux,initrd-end', rd_end)):
            cells = ['0x%08x' % (value >> 32), '0x%08x' % (value & 0xffffffff)]
            subprocess.run(['fdtput', '-t', 'x', tmp, '/chosen', prop] + cells,
                           check=True, capture_output=True)
        out = open(tmp, 'rb').read()
    finally:
        os.unlink(tmp)
    if out[:4] != b'\xd0\x0d\xfe\xed':
        raise SystemExit('patched DTB lost its magic')
    return out


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(f'usage: {sys.argv[0]} Image out-boot.img [payload]')
    img_path, out_path = sys.argv[1], sys.argv[2]
    payload_path = sys.argv[3] if len(sys.argv) == 4 else None

    stock = open(STOCK, 'rb').read()
    ks, _ka, _rs, ra, _ss, _sa, _ta, page, _dtsz, _osv = struct.unpack('<10I', stock[8:48])
    hdr = bytearray(stock[:608])
    kern = stock[page:page + ks]

    d = zlib.decompressobj(31)
    d.decompress(kern)
    dtbs = d.unused_data
    if dtbs[:4] != b'\xd0\x0d\xfe\xed':
        raise SystemExit('no appended dtb found')

    rd_start = ra
    if payload_path:
        payload = open(payload_path, 'rb').read()
        magic = payload[:8]
        if magic not in (b'SWEETXNU', b'KAG2TWRP'):
            raise SystemExit('unknown payload magic %r' % magic)
        rd_end = rd_start + len(payload)
        dtbs = patch_dtb(dtbs, rd_start, rd_end)
        if magic == b'SWEETXNU':
            rd = gzip.compress(payload, 9, mtime=0)
        else:
            rd = gzip_stored(payload)
        print('payload', magic.decode(), len(payload), 'initrd', hex(rd_start), '..', hex(rd_end))
    else:
        cpio = b'070701' + b''.join(
            b'%08X' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0)
        ) + b'TRAILER!!!\0'
        cpio += b'\0' * (-len(cpio) % 512)
        rd = gzip.compress(cpio, 9, mtime=0)

    img = open(img_path, 'rb').read()
    newk = gzip.compress(img, 9, mtime=0) + dtbs

    sha = hashlib.sha1()
    sha.update(newk)
    sha.update(struct.pack('<I', len(newk)))
    sha.update(rd)
    sha.update(struct.pack('<I', len(rd)))
    sha.update(b'')
    sha.update(struct.pack('<I', 0))

    hdr[8:12] = struct.pack('<I', len(newk))
    hdr[16:20] = struct.pack('<I', len(rd))
    hdr[20:24] = struct.pack('<I', ra)
    hdr[24:28] = struct.pack('<I', 0)
    hdr[576:608] = sha.digest().ljust(32, b'\0')

    pad = lambda b: b + b'\0' * (-len(b) % page)
    with open(out_path, 'wb') as f:
        f.write(pad(bytes(hdr)) + pad(newk) + pad(rd))
    print('kernel', len(newk), 'ramdisk', len(rd), 'dtb', len(dtbs))
    if payload_path:
        print('initrd-start', hex(rd_start), 'initrd-end', hex(rd_end))


if __name__ == '__main__':
    main()
