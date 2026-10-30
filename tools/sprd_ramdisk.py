#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# sprd_ramdisk.py - list a UNISOC (SPRD) Android boot ramdisk (compressed
# newc cpio) and swap the vendor kernel modules in it.
#
#   Copyright (C) 2026 the kwt kernel maintainers
#
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License version 2 as
#   published by the Free Software Foundation.
#
#   list RAMDISK
#       compression, newc entries, module files and the init .rc lines
#       that load them (insmod / modprobe)
#   replace-modules RAMDISK OUT --module PATH... [--kernel Image]
#       replace the .ko entries with the same basename; every other entry,
#       its header fields, order and the trailer stay byte-identical; the
#       output is recompressed with the settings that reproduce the stock
#       compressed stream (searched), else the nearest default
#
# The ramdisk is the file written by "sprd_bootimg.py unpack". Compression is
# detected by magic: gzip and xz are handled; lz4 (legacy or frame) and
# bzip2/lzma-alone are reported and refused.
#
# Only the python3 standard library is used. Input is untrusted data: it is
# parsed in memory, entry names are never used as file paths, nothing is
# extracted or executed. Run with "python3 -I".

import argparse
import lzma
import os
import re
import struct
import sys
import tempfile
import zlib

MODULE_DIRS = ('lib/modules/',)
NEWC_MAGIC = b'070701'
NEWC_HDR = 110
NEWC_FIELDS = ('ino', 'mode', 'uid', 'gid', 'nlink', 'mtime', 'filesize',
               'devmajor', 'devminor', 'rdevmajor', 'rdevminor', 'namesize',
               'check')
MAX_ELF_SECTIONS = 4096


class Error(Exception):
    pass


def read_file(path):
    with open(path, 'rb') as f:
        return f.read()


def pad4(n):
    return (n + 3) & ~3


# ----------------------------------------------------------- compression --

def gzip_header_len(d):
    if len(d) < 18 or d[0] != 0x1f or d[1] != 0x8b or d[2] != 8:
        raise Error('bad gzip header')
    flg = d[3]
    if flg & 0xe0:
        raise Error('gzip: reserved flags set')
    p = 10
    if flg & 4:
        if p + 2 > len(d):
            raise Error('gzip: truncated extra field')
        p += 2 + struct.unpack_from('<H', d, p)[0]
    for bit in (8, 16):
        if flg & bit:
            e = d.find(b'\0', p)
            if e < 0:
                raise Error('gzip: unterminated name/comment')
            p = e + 1
    if flg & 2:
        p += 2
    if p > len(d):
        raise Error('gzip: truncated header')
    return p


class Packed:
    """A decompressed ramdisk plus what is needed to compress it again."""

    def __init__(self, fmt, raw, cpio, tail):
        self.fmt = fmt          # 'gzip' or 'xz'
        self.raw = raw          # the original compressed file
        self.cpio = cpio        # decompressed bytes
        self.tail = tail        # bytes after the compressed stream (kept)
        self.hdr = b''          # gzip header bytes
        self.params = None      # how to recompress
        self.stream_same = None  # True if recompressing reproduces raw

    def describe(self):
        if self.fmt == 'gzip':
            if self.params is None:
                return 'gzip (no zlib setting reproduces the stream)'
            return 'gzip, zlib level %d memLevel %d' % self.params
        if self.params is None:
            return 'xz (no lzma preset reproduces the stream)'
        return 'xz, preset %s check %d' % (self.params[0], self.params[1])


def deflate(data, level, mem):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, mem)
    return c.compress(data) + c.flush()


def unpack_gzip(d):
    n = gzip_header_len(d)
    dec = zlib.decompressobj(-15)
    try:
        out = dec.decompress(d[n:])
    except zlib.error as e:
        raise Error('gzip: %s' % e)
    if not dec.eof:
        raise Error('gzip: truncated stream')
    used = len(d) - n - len(dec.unused_data)
    rest = dec.unused_data
    if len(rest) < 8:
        raise Error('gzip: truncated trailer')
    crc, isize = struct.unpack_from('<II', rest)
    if crc != zlib.crc32(out) or isize != (len(out) & 0xffffffff):
        raise Error('gzip: trailer CRC/size mismatch')
    p = Packed('gzip', d, out, rest[8:])
    p.hdr = d[:n]
    deflate_orig = d[n:n + used]
    for level in (9, 6, 1, 2, 3, 4, 5, 7, 8):
        for mem in (8, 9):
            if deflate(out, level, mem) == deflate_orig:
                p.params = (level, mem)
                return p
    return p


def unpack_xz(d):
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    try:
        out = dec.decompress(d)
    except lzma.LZMAError as e:
        raise Error('xz: %s' % e)
    if not dec.eof:
        raise Error('xz: truncated stream')
    used = len(d) - len(dec.unused_data)
    check = d[7] & 0x0f
    p = Packed('xz', d, out, dec.unused_data)
    for preset in (6, 9, 9 | lzma.PRESET_EXTREME, 6 | lzma.PRESET_EXTREME,
                   0, 1, 2, 3, 4, 5, 7, 8):
        try:
            c = lzma.compress(out, format=lzma.FORMAT_XZ, check=check,
                              preset=preset)
        except lzma.LZMAError:
            continue
        if c == d[:used]:
            p.params = (preset, check)
            return p
    p.params = None
    p.check = check
    return p


def decompress(d):
    if d[:2] == b'\x1f\x8b':
        return unpack_gzip(d)
    if d[:6] == b'\xfd7zXZ\x00':
        return unpack_xz(d)
    if d[:4] == b'\x02\x21\x4c\x18':
        raise Error('ramdisk is lz4 (legacy); not supported, stopping')
    if d[:4] == b'\x04\x22\x4d\x18':
        raise Error('ramdisk is lz4 (frame); not supported, stopping')
    if d[:3] == b'BZh':
        raise Error('ramdisk is bzip2; not supported')
    if d[:6] == b'070701':
        raise Error('ramdisk is an uncompressed cpio; not supported')
    raise Error('unknown ramdisk magic %s' % d[:4].hex())


def compress(p, data):
    """Recompress data the way the stock ramdisk was; returns (bytes,
    description of the settings used)."""
    if p.fmt == 'gzip':
        level, mem = p.params or (9, 8)
        body = deflate(data, level, mem)
        trailer = struct.pack('<II', zlib.crc32(data),
                              len(data) & 0xffffffff)
        return p.hdr + body + trailer + p.tail
    preset, check = p.params if p.params else (6, getattr(p, 'check', 4))
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=check,
                         preset=preset) + p.tail


# ------------------------------------------------------------------ cpio --

class Entry:
    __slots__ = ('off', 'f', 'name', 'data_off', 'hdr_end')


def parse_cpio(c):
    """Return (entries, trailer_end). Entries up to and including the
    TRAILER!!! record; bytes after it are the caller's tail."""
    out = []
    pos = 0
    while True:
        if pos + NEWC_HDR > len(c):
            raise Error('cpio: truncated header at %d' % pos)
        h = c[pos:pos + NEWC_HDR]
        if h[:6] != NEWC_MAGIC:
            if h[:6] == b'070702':
                raise Error('cpio: CRC newc (070702) not supported')
            raise Error('cpio: bad magic at %d' % pos)
        try:
            vals = [int(h[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        except ValueError:
            raise Error('cpio: bad hex field at %d' % pos)
        e = Entry()
        e.off = pos
        e.f = dict(zip(NEWC_FIELDS, vals))
        ns = e.f['namesize']
        if ns < 1 or pos + NEWC_HDR + ns > len(c):
            raise Error('cpio: bad name size at %d' % pos)
        raw = c[pos + NEWC_HDR:pos + NEWC_HDR + ns]
        if raw[-1:] != b'\0':
            raise Error('cpio: name not NUL terminated at %d' % pos)
        e.name = raw[:-1].decode('utf-8', 'backslashreplace')
        e.data_off = pad4(pos + NEWC_HDR + ns)
        e.hdr_end = e.data_off
        end = pad4(e.data_off + e.f['filesize'])
        if e.data_off + e.f['filesize'] > len(c):
            raise Error('cpio: entry data past end at %d' % pos)
        out.append(e)
        pos = end
        if e.name == 'TRAILER!!!':
            return out, min(pos, len(c))
        if pos > len(c):
            raise Error('cpio: missing trailer')


def norm(name):
    while name.startswith('./'):
        name = name[2:]
    return name.lstrip('/')


def is_reg(e):
    return (e.f['mode'] & 0o170000) == 0o100000


def safe_name(s):
    return ''.join(ch if ch.isprintable() else '?' for ch in s)


def module_entries(entries):
    return [e for e in entries
            if is_reg(e) and e.name.endswith('.ko')
            and norm(e.name).startswith(MODULE_DIRS)]


RC_RE = re.compile(rb'\b(insmod|modprobe)\b')


def rc_lines(c, entries):
    res = []
    for e in entries:
        if not is_reg(e) or not norm(e.name).endswith('.rc'):
            continue
        data = c[e.data_off:e.data_off + e.f['filesize']]
        for n, line in enumerate(data.split(b'\n'), 1):
            s = line.strip()
            if s.startswith(b'#'):
                continue
            if RC_RE.search(s) or b'.ko' in s:
                res.append((norm(e.name), n,
                            safe_name(s.decode('utf-8', 'replace'))))
    return res


def basename(n):
    return n.rsplit('/', 1)[-1]


# ----------------------------------------------------------------- ELF ---

def modinfo(ko):
    """Return the .modinfo key=value list of a little endian ELF64 .ko."""
    if ko[:4] != b'\x7fELF' or len(ko) < 64:
        raise Error('not an ELF file')
    if ko[4] != 2 or ko[5] != 1:
        raise Error('not a little endian ELF64 module')
    shoff, = struct.unpack_from('<Q', ko, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', ko, 0x3a)
    if shentsize != 64 or not 0 < shnum <= MAX_ELF_SECTIONS \
            or shstrndx >= shnum or shoff + shnum * 64 > len(ko):
        raise Error('bad ELF section table')
    secs = [struct.unpack_from('<IIQQQQIIQQ', ko, shoff + 64 * i)
            for i in range(shnum)]
    so, ss = secs[shstrndx][4], secs[shstrndx][5]
    if so + ss > len(ko):
        raise Error('bad ELF section string table')
    strtab = ko[so:so + ss]
    for s in secs:
        nm = strtab[s[0]:strtab.find(b'\0', s[0])] if s[0] < len(strtab) \
            else b''
        if nm == b'.modinfo':
            if s[4] + s[5] > len(ko):
                raise Error('bad .modinfo bounds')
            items = ko[s[4]:s[4] + s[5]].split(b'\0')
            return [i.decode('utf-8', 'replace') for i in items if i]
    raise Error('no .modinfo section')


def vermagic(ko):
    for i in modinfo(ko):
        if i.startswith('vermagic='):
            return i[9:]
    raise Error('no vermagic in .modinfo')


def kernel_release(img):
    m = re.search(rb'Linux version ([^\s\0]+) ', img)
    if not m:
        raise Error('no "Linux version" banner in the kernel image')
    return m.group(1).decode('ascii', 'replace')


# ------------------------------------------------------------- commands ---

def load(path):
    p = decompress(read_file(path))
    entries, end = parse_cpio(p.cpio)
    return p, entries, end


def cmd_list(a):
    p, entries, end = load(a.ramdisk)
    print('compression: %s' % p.describe())
    print('compressed %d bytes, cpio %d bytes, %d entries, '
          'cpio tail %d bytes, compressed tail %d bytes' %
          (len(p.raw), len(p.cpio), len(entries), len(p.cpio) - end,
           len(p.tail)))
    kinds = {0o100000: '-', 0o040000: 'd', 0o120000: 'l', 0o020000: 'c',
             0o060000: 'b', 0o010000: 'p', 0o140000: 's'}
    for e in entries:
        f = e.f
        print('%s%04o %5d/%-5d %9d %s' % (
            kinds.get(f['mode'] & 0o170000, '?'), f['mode'] & 0o7777,
            f['uid'], f['gid'], f['filesize'], safe_name(e.name)))
    print('\nmodules under %s:' % ', '.join(MODULE_DIRS))
    for e in module_entries(entries):
        ko = p.cpio[e.data_off:e.data_off + e.f['filesize']]
        try:
            vm = vermagic(ko)
        except Error as x:
            vm = '(%s)' % x
        print('  %s  %d bytes  vermagic: %s' % (safe_name(e.name),
                                                e.f['filesize'], vm))
    print('\ninit .rc lines with insmod/modprobe or .ko (file:line):')
    for fn, n, s in rc_lines(p.cpio, entries):
        print('  %s:%d: %s' % (fn, n, s))
    return 0


def cmd_replace(a):
    if os.path.abspath(a.out) == os.path.abspath(a.ramdisk):
        raise Error('OUT must differ from RAMDISK')
    p, entries, end = load(a.ramdisk)
    mods = module_entries(entries)
    new = {}
    for path in a.module:
        b = os.path.basename(path)
        if b in new:
            raise Error('module %s given twice' % b)
        new[b] = read_file(path)
    rel = kernel_release(read_file(a.kernel)) if a.kernel else None
    bad = 0
    for b, ko in new.items():
        try:
            vm = vermagic(ko)
        except Error as x:
            raise Error('%s: %s' % (b, x))
        if rel is None:
            print('%s: vermagic "%s" (no --kernel, not checked)' % (b, vm))
        elif vm.split(' ')[0] != rel:
            bad += 1
            print('!!! VERMAGIC MISMATCH: %s has "%s", kernel release is '
                  '"%s"' % (b, vm, rel), file=sys.stderr)
        else:
            print('%s: vermagic matches kernel release %s' % (b, rel))
    if bad:
        raise Error('%d module(s) do not match the kernel; refusing' % bad)
    target = {}
    for b in new:
        hit = [e for e in mods if basename(e.name) == b]
        if not hit:
            raise Error('ramdisk has no module named %s under %s' %
                        (b, ', '.join(MODULE_DIRS)))
        if len(hit) > 1:
            raise Error('ambiguous: %d entries named %s' % (len(hit), b))
        e = hit[0]
        if e.f['nlink'] != 1 or e.f['filesize'] == 0:
            raise Error('%s is a hard link; not supported' % b)
        target[e.off] = new[b]
    out = bytearray()
    pos = 0
    for e in entries:
        if e.off in target:
            data = target[e.off]
            h = bytearray(p.cpio[e.off:e.off + NEWC_HDR])
            h[6 + 8 * 6:14 + 8 * 6] = b'%08X' % len(data) \
                if p.cpio[e.off + 6 + 48:e.off + 14 + 48].isupper() \
                else b'%08x' % len(data)
            out += p.cpio[pos:e.off]
            out += h + p.cpio[e.off + NEWC_HDR:e.data_off]
            out += data + b'\0' * (pad4(len(data)) - len(data))
            pos = pad4(e.data_off + e.f['filesize'])
    out += p.cpio[pos:]
    out = bytes(out)
    if not target:
        assert out == p.cpio
    # self-check: the result parses, only the target entries changed
    e2, end2 = parse_cpio(out)
    if len(e2) != len(entries):
        raise Error('self-check: entry count changed')
    for x, y in zip(entries, e2):
        if x.name != y.name:
            raise Error('self-check: entry order changed')
        same = (p.cpio[x.data_off:x.data_off + x.f['filesize']] ==
                out[y.data_off:y.data_off + y.f['filesize']])
        fx = dict(x.f)
        fy = dict(y.f)
        if x.off in target:
            fx['filesize'] = fy['filesize']
            same = out[y.data_off:y.data_off + y.f['filesize']] == \
                target[x.off]
        if fx != fy or not same:
            raise Error('self-check: entry %s changed unexpectedly' %
                        safe_name(x.name))
    if out[end2:] != p.cpio[end:]:
        raise Error('self-check: trailer/tail changed')
    blob = compress(p, out)
    if decompress(blob).cpio != out:
        raise Error('self-check: recompressed ramdisk does not decode')
    d = os.path.dirname(os.path.abspath(a.out))
    fd, tmp = tempfile.mkstemp(dir=d, prefix='.sprd_ramdisk.')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(blob)
        os.replace(tmp, a.out)
    except BaseException:
        os.unlink(tmp)
        raise
    print('replaced %d module(s); cpio %d -> %d bytes; %s; wrote %s (%d '
          'bytes)' % (len(target), len(p.cpio), len(out), p.describe(),
                      a.out, len(blob)))
    if not target:
        print('no replacements: compressed stream %s the stock one' %
              ('IDENTICAL to' if blob == p.raw else 'differs from'))
    return 0


def main():
    ap = argparse.ArgumentParser(description='list and patch the modules in '
                                 'a SPRD boot ramdisk')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list', help='list cpio entries, modules, insmod lines')
    p.add_argument('ramdisk')
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser('replace-modules',
                       help='swap same-named .ko files in the ramdisk')
    p.add_argument('ramdisk')
    p.add_argument('out')
    p.add_argument('--module', action='append', default=[], metavar='PATH',
                   help='new .ko (repeatable); basename picks the target')
    p.add_argument('--kernel', help='kernel Image whose release must match '
                   'each module vermagic')
    p.set_defaults(fn=cmd_replace)
    a = ap.parse_args()
    try:
        return a.fn(a)
    except (Error, OSError, struct.error, ValueError, UnicodeError,
            KeyError, TypeError, lzma.LZMAError, zlib.error) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
