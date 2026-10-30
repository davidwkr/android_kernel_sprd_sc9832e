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
#   add-rc RAMDISK OUT --fragment FILE
#       add FILE as /init.watch_tweaks.rc (new last entry before the
#       trailer) and append "import /init.watch_tweaks.rc" to /init.rc; all
#       other entries stay byte-identical; a second run is a no-op
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
MAX_OUT = 256 << 20      # decompressed size cap (bomb guard)
MAX_IN_RAMDISK = 64 << 20  # input size caps
MAX_IN_MISC = 64 << 20     # .ko, Module.symvers
MAX_IN_KERNEL = 128 << 20
MAX_ENTRIES = 100000
MAX_RECORDS = 100000     # .rc lines / .modinfo records parsed
MAX_RC_SIZE = 1 << 20
MAX_MODULES = 16
MAX_NOTES = 100
MAX_FRAGMENT = 4096
FRAGMENT = 'init.watch_tweaks.rc'
IMPORT_RE = re.compile(rb'^[ \t]*import[ \t]+/init\.watch_tweaks\.rc[ \t]*$',
                       re.M)
PATH_MAX = 4096
MAX_XZ_MEM = 128 << 20   # xz decoder memory limit
HEX8 = re.compile(rb'[0-9A-Fa-f]{8}')


class Error(Exception):
    pass


def read_file(path, limit=MAX_IN_MISC):
    with open(path, 'rb') as f:
        d = f.read(limit + 1)
    if len(d) > limit:
        raise Error('%s: over %d MiB, refusing' % (os.path.basename(path),
                                                   limit >> 20))
    return d


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
        out = dec.decompress(d[n:], MAX_OUT + 1)
    except zlib.error as e:
        raise Error('gzip: %s' % e)
    if len(out) > MAX_OUT:
        raise Error('gzip: output over %d MiB, refusing' % (MAX_OUT >> 20))
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
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_XZ,
                                 memlimit=MAX_XZ_MEM)
    try:
        out = dec.decompress(d, MAX_OUT + 1)
    except lzma.LZMAError as e:
        raise Error('xz: %s (decoder memory limit %d MiB)' %
                    (e, MAX_XZ_MEM >> 20))
    if len(out) > MAX_OUT:
        raise Error('xz: output over %d MiB, refusing' % (MAX_OUT >> 20))
    if not dec.eof:
        raise Error('xz: truncated stream')
    used = len(d) - len(dec.unused_data)
    check = d[7] & 0x0f
    p = Packed('xz', d, out, dec.unused_data)
    for preset in (6,):
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
    __slots__ = ('off', 'f', 'name', 'data_off', 'hdr_end', 'nul', 'long')


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
        fl = [h[6 + 8 * i:14 + 8 * i] for i in range(13)]
        if not all(HEX8.fullmatch(x) for x in fl):
            raise Error('cpio: bad hex field at %d' % pos)
        vals = [int(x, 16) for x in fl]
        e = Entry()
        e.off = pos
        e.f = dict(zip(NEWC_FIELDS, vals))
        ns = e.f['namesize']
        if ns < 1 or pos + NEWC_HDR + ns > len(c):
            raise Error('cpio: bad name size at %d' % pos)
        nstart = pos + NEWC_HDR
        if c[nstart + ns - 1:nstart + ns] != b'\0':
            raise Error('cpio: name not NUL terminated at %d' % pos)
        e.long = ns > PATH_MAX   # the kernel skips such entries
        # the kernel's unpacker uses the name up to the first NUL; look at
        # no more than PATH_MAX + 1 bytes so a huge name costs no memory
        cut = c.find(b'\0', nstart, nstart + min(ns, PATH_MAX + 1))
        e.nul = cut != -1 and cut < nstart + ns - 1
        if cut == -1:
            cut = nstart + min(ns, PATH_MAX + 1)
        e.name = bytes(c[nstart:cut]).decode('utf-8', 'backslashreplace')
        e.data_off = pad4(pos + NEWC_HDR + ns)
        e.hdr_end = e.data_off
        end = pad4(e.data_off + e.f['filesize'])
        if e.data_off + e.f['filesize'] > len(c):
            raise Error('cpio: entry data past end at %d' % pos)
        out.append(e)
        if len(out) > MAX_ENTRIES:
            raise Error('cpio: more than %d entries' % MAX_ENTRIES)
        if end <= pos:
            raise Error('cpio: entry does not advance at %d' % pos)
        pos = end
        if e.name == 'TRAILER!!!':
            return out, min(pos, len(c))
        if pos > len(c):
            raise Error('cpio: missing trailer')


def norm(name):
    """Extraction path as the kernel would see it: no leading '/', '.' or
    empty components. '..' is kept (see anomalies())."""
    return '/'.join(c for c in name.split('/') if c not in ('', '.'))


def anomalies(entries):
    """Names the swap cannot reason about: embedded NUL, '..' component,
    or two entries extracting to the same path."""
    res = []
    seen = {}
    links = set()
    for e in entries:
        if e.long:
            continue
        if (e.f['mode'] & 0o170000) == 0o120000:
            n = norm(e.name)
            links.add(n)
            for d in MODULE_DIRS:
                dd = d.rstrip('/')
                if dd == n or dd.startswith(n + '/'):
                    res.append('symlink %r is on the module dir path' %
                               e.name)
    for e in entries:
        if e.long:
            res.append('name of %d bytes over PATH_MAX (kernel skips it)' %
                       e.f['namesize'])
        if e.long:
            continue
        n = norm(e.name)
        k = n.find('/')
        while k >= 0:
            if n[:k] in links:
                res.append('%r is under symlink %r' % (e.name, n[:k]))
                break
            k = n.find('/', k + 1)
        if e.nul:
            res.append('embedded NUL in name %r' % e.name)
        if '..' in n.split('/'):
            res.append('".." component in %r' % e.name)
        if n and e.name != 'TRAILER!!!':
            if n in seen:
                res.append('duplicate path %r (%r and %r)' %
                           (n, seen[n], e.name))
            seen.setdefault(n, e.name)
    if len(res) > MAX_NOTES:
        res = res[:MAX_NOTES] + ['... %d more' % (len(res) - MAX_NOTES)]
    return [safe_name(x) for x in res]


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
        if e.f['filesize'] > MAX_RC_SIZE:
            res.append((norm(e.name), 0, '(over 1 MiB, not parsed)'))
            continue
        data = c[e.data_off:e.data_off + e.f['filesize']]
        for n, m in enumerate(re.finditer(rb'[^\n]*\n|[^\n]+', data), 1):
            if n > MAX_RECORDS:
                break
            s = m.group().strip()
            if s.startswith(b'#'):
                continue
            if RC_RE.search(s) or b'.ko' in s:
                res.append((norm(e.name), n,
                            safe_name(s.decode('utf-8', 'replace'))))
    return res


def basename(n):
    return n.rsplit('/', 1)[-1]


# ----------------------------------------------------------------- ELF ---

def elf_section(ko, want):
    """Return the bytes of section `want` of a little endian ELF64 .ko,
    or None."""
    if ko[:4] != b'\x7fELF' or len(ko) < 64:
        raise Error('not an ELF file')
    if ko[4] != 2 or ko[5] != 1:
        raise Error('not a little endian ELF64 module')
    etype, mach = struct.unpack_from('<HH', ko, 16)
    if etype != 1 or mach != 183:
        raise Error('not an AArch64 relocatable object (e_type %d, '
                    'e_machine %d)' % (etype, mach))
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
        if nm == want:
            if s[4] + s[5] > len(ko):
                raise Error('bad %s bounds' % want.decode())
            return ko[s[4]:s[4] + s[5]]
    return None


def modinfo(ko):
    """Return the .modinfo key=value list."""
    sec = elf_section(ko, b'.modinfo')
    if sec is None:
        raise Error('no .modinfo section')
    res = []
    for m in re.finditer(rb'[^\0]+', sec):
        if len(res) >= MAX_RECORDS:
            break
        res.append(m.group().decode('utf-8', 'replace'))
    return res


def layout_crc(ko):
    """CRC of module_layout in __versions (struct modversion_info: 8 byte
    crc, 56 byte name on 64 bit), or None."""
    sec = elf_section(ko, b'__versions')
    if sec is None:
        return None
    for o in range(0, len(sec) - 63, 64):
        if sec[o + 8:o + 64].split(b'\0')[0] == b'module_layout':
            return struct.unpack_from('<Q', sec, o)[0]
    return None


def symvers_layout_crc(path):
    for line in read_file(path).decode('utf-8', 'replace').splitlines():
        f = line.split('\t')
        if len(f) >= 2 and f[1] == 'module_layout':
            return int(f[0], 16)
    raise Error('module_layout not in %s' % path)


def vermagic_diff(new, stock, rel):
    """Like the kernel's same_magic() with MODVERSIONS: the whole string
    must match. Here the expectation is the stock module's vermagic with
    only the release token replaced by `rel` (None: release unchecked).
    Returns a list of mismatch descriptions."""
    nt, st = new.split(' '), stock.split(' ')
    bad = []
    if rel is not None and nt[0] != rel:
        bad.append('release "%s", kernel is "%s"' % (nt[0], rel))
    if len(nt) != len(st):
        bad.append('field count %d, stock %d ("%s" vs "%s")' %
                   (len(nt), len(st), new, stock))
        return bad
    for k, (x, y) in enumerate(zip(nt[1:], st[1:]), 1):
        if x != y:
            bad.append('field %d "%s", stock "%s"' % (k, x, y))
    return bad


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
    p = decompress(read_file(path, MAX_IN_RAMDISK))
    entries, end = parse_cpio(p.cpio)
    return p, entries, end


def cmd_list(a):
    p, entries, end = load(a.ramdisk)
    print('compression: %s' % p.describe())
    if p.tail.strip(b'\0'):
        print('WARNING: non-zero data after the compressed member (%d '
              'bytes): concatenated archive?' % len(p.tail))
    if p.cpio[end:].strip(b'\0'):
        print('WARNING: non-zero data after the cpio trailer (%d bytes): '
              'concatenated archive?' % len(p.cpio[end:]))
    print('compressed %d bytes, cpio %d bytes, %d entries, '
          'cpio tail %d bytes, compressed tail %d bytes' %
          (len(p.raw), len(p.cpio), len(entries), len(p.cpio) - end,
           len(p.tail)))
    kinds = {0o100000: '-', 0o040000: 'd', 0o120000: 'l', 0o020000: 'c',
             0o060000: 'b', 0o010000: 'p', 0o140000: 's'}
    for x in anomalies(entries):
        print('WARNING: %s' % x)
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


def check_clean(p, entries, end, why):
    for what, t in (('data after the compressed member', p.tail),
                    ('data after the cpio trailer', p.cpio[end:])):
        if t.strip(b'\0'):
            raise Error('%s is non-zero (%d bytes): a concatenated archive '
                        'could override %s; refusing' % (what, len(t), why))
    an = anomalies(entries)
    if an:
        raise Error('ambiguous cpio names, refusing: %s' % '; '.join(an))


def write_out(path, blob):
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=d, prefix='.sprd_ramdisk.')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(blob)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def cmd_replace(a):
    if os.path.abspath(a.out) == os.path.abspath(a.ramdisk):
        raise Error('OUT must differ from RAMDISK')
    p, entries, end = load(a.ramdisk)
    check_clean(p, entries, end, 'the swapped modules')
    mods = module_entries(entries)
    if len(a.module) > MAX_MODULES:
        raise Error('more than %d --module arguments' % MAX_MODULES)
    new = {}
    total_in = 0
    for path in a.module:
        b = os.path.basename(path)
        if b in new:
            raise Error('module %s given twice' % b)
        new[b] = read_file(path)
        total_in += len(new[b])
        if total_in > MAX_IN_MISC:
            raise Error('modules total over %d MiB' % (MAX_IN_MISC >> 20))
    kimg = read_file(a.kernel, MAX_IN_KERNEL) if a.kernel else None
    rel = kernel_release(kimg) if kimg is not None else None
    want_crc = symvers_layout_crc(a.symvers) if a.symvers else None
    if a.symvers and not a.kernel:
        print('note: --symvers without --kernel; CRC still checked')
    bad = 0
    for b, ko in new.items():
        hit = [e for e in mods if basename(e.name) == b]
        if not hit:
            raise Error('ramdisk has no module named %s under %s' %
                        (b, ', '.join(MODULE_DIRS)))
        if len(hit) > 1:
            raise Error('ambiguous: %d entries named %s' % (len(hit), b))
        try:
            vm = vermagic(ko)
            sv = vermagic(p.cpio[hit[0].data_off:hit[0].data_off +
                                 hit[0].f['filesize']])
        except Error as x:
            raise Error('%s: %s' % (b, x))
        diff = vermagic_diff(vm, sv, rel)
        # the kernel's own vermagic[] (CONFIG_MODULES) is the reference for
        # the flags too: the stock module only tells what stock expected
        if kimg is not None and (b'\0' + vm.strip().encode() + b'\0') not in kimg:
            diff.append('vermagic "%s" not found in the --kernel image' % vm)
        if want_crc is not None:
            got = layout_crc(ko)
            if got is None:
                diff.append('no module_layout CRC in __versions')
            elif got != want_crc:
                diff.append('module_layout CRC 0x%x, symvers 0x%x' %
                            (got, want_crc))
        if diff:
            bad += 1
            for x in diff:
                print('!!! VERMAGIC MISMATCH: %s: %s' % (b, x),
                      file=sys.stderr)
        elif rel is None:
            print('%s: vermagic "%s" (no --kernel, release not checked)' %
                  (b, vm))
        else:
            print('%s: vermagic matches (release %s)' % (b, rel))
    if bad:
        raise Error('%d module(s) do not match; refusing' % bad)
    target = {}
    proj = len(p.cpio)
    for b in new:
        hit = [e for e in mods if basename(e.name) == b]
        if not hit:
            raise Error('ramdisk has no module named %s under %s' %
                        (b, ', '.join(MODULE_DIRS)))
        if len(hit) > 1:
            raise Error('ambiguous: %d entries named %s' % (len(hit), b))
        e = hit[0]
        if e.f['filesize'] == 0:
            raise Error('%s in the ramdisk has size 0 (the data of a hard '
                        'link set lives in another entry); not supported' % b)
        if e.f['nlink'] != 1:
            raise Error('%s has nlink %d (hard link); not supported' %
                        (b, e.f['nlink']))
        target[e.off] = new[b]
        proj += pad4(len(new[b])) - pad4(e.f['filesize'])
    if proj > MAX_OUT:
        raise Error('projected cpio size %d over the %d MiB cap' %
                    (proj, MAX_OUT >> 20))
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
    write_out(a.out, blob)
    print('replaced %d module(s); cpio %d -> %d bytes; %s; wrote %s (%d '
          'bytes)' % (len(target), len(p.cpio), len(out), p.describe(),
                      a.out, len(blob)))
    if p.params is None:
        print('note: the stock compressed stream is not reproducible; the '
              'output uses default %s settings and differs from it' % p.fmt)
    if not target:
        print('no replacements: compressed stream %s the stock one' %
              ('IDENTICAL to' if blob == p.raw else 'differs from'))
    return 0


def read_fragment(path):
    with open(path, 'rb') as f:
        d = f.read(MAX_FRAGMENT + 1)
    if len(d) > MAX_FRAGMENT:
        raise Error('fragment over %d bytes, refusing' % MAX_FRAGMENT)
    if not d.strip():
        raise Error('fragment is empty')
    if not d.endswith(b'\n'):
        raise Error('fragment must end with a newline')
    if any(b not in (9, 10) and not 32 <= b < 127 for b in d):
        raise Error('fragment must be printable ASCII (tab and LF only)')
    if not any(l.strip() and not l.lstrip().startswith(b'#')
               for l in d.split(b'\n')):
        raise Error('fragment has only comments')
    return d


def hdr_bytes(like, f):
    up = any(c in b'ABCDEF' for c in like[6:NEWC_HDR])
    fmt = '%08X' if up else '%08x'
    return NEWC_MAGIC + ''.join(fmt % f[k] for k in NEWC_FIELDS).encode()


def cmd_add_rc(a):
    if os.path.abspath(a.out) == os.path.abspath(a.ramdisk):
        raise Error('OUT must differ from RAMDISK')
    frag = read_fragment(a.fragment)
    p, entries, end = load(a.ramdisk)
    check_clean(p, entries, end, '/init.rc')
    rcs = [e for e in entries if norm(e.name) == 'init.rc']
    if not rcs:
        raise Error('ramdisk has no /init.rc; refusing')
    rc = rcs[0]
    if not is_reg(rc):
        raise Error('/init.rc is not a regular file; refusing')
    if rc.f['nlink'] != 1 or rc.f['filesize'] == 0:
        raise Error('/init.rc is a hard link (nlink %d, size %d); not '
                    'supported' % (rc.f['nlink'], rc.f['filesize']))
    if rc.f['filesize'] > MAX_RC_SIZE:
        raise Error('/init.rc over 1 MiB; refusing')
    rcdata = p.cpio[rc.data_off:rc.data_off + rc.f['filesize']]
    have = [e for e in entries if norm(e.name) == FRAGMENT]
    imported = bool(IMPORT_RE.search(rcdata))
    if have:
        x = have[0]
        same = is_reg(x) and \
            p.cpio[x.data_off:x.data_off + x.f['filesize']] == frag
        if not same:
            raise Error('/%s already exists with different content or '
                        'type; refusing' % FRAGMENT)
        if not imported:
            raise Error('/%s exists but /init.rc does not import it; '
                        'refusing a half-applied ramdisk' % FRAGMENT)
        write_out(a.out, p.raw)
        print('already applied: /%s present and imported; wrote an '
              'unchanged copy %s' % (FRAGMENT, a.out))
        return 0
    if imported:
        raise Error('/init.rc imports /%s but the file is missing; '
                    'refusing' % FRAGMENT)
    newrc = rcdata + (b'' if rcdata.endswith(b'\n') else b'\n') + \
        b'import /' + FRAGMENT.encode() + b'\n'
    if len(newrc) > MAX_RC_SIZE:
        raise Error('new /init.rc over 1 MiB; refusing')
    tr = entries[-1]
    ino = max(e.f['ino'] for e in entries) + 1
    if ino > 0xffffffff:
        raise Error('no free inode number')
    name = rc.name[:len(rc.name) - len('init.rc')] + FRAGMENT
    nb = name.encode() + b'\0'
    f = dict(rc.f, ino=ino, nlink=1,
             filesize=len(frag), rdevmajor=0, rdevminor=0,
             namesize=len(nb), check=0)
    h = hdr_bytes(p.cpio[rc.off:rc.off + NEWC_HDR], f)
    ent = h + nb + b'\0' * (pad4(NEWC_HDR + len(nb)) - NEWC_HDR - len(nb)) \
        + frag + b'\0' * (pad4(len(frag)) - len(frag))
    h2 = hdr_bytes(p.cpio[rc.off:rc.off + NEWC_HDR],
                   dict(rc.f, filesize=len(newrc)))
    out = (p.cpio[:rc.off] + h2 + p.cpio[rc.off + NEWC_HDR:rc.data_off] +
           newrc + b'\0' * (pad4(len(newrc)) - len(newrc)) +
           p.cpio[pad4(rc.data_off + rc.f['filesize']):tr.off] + ent +
           p.cpio[tr.off:])
    if len(out) > MAX_OUT:
        raise Error('projected cpio size over the %d MiB cap' %
                    (MAX_OUT >> 20))
    # self-check: the result parses; only init.rc and the new entry differ
    e2, end2 = parse_cpio(out)
    if len(e2) != len(entries) + 1:
        raise Error('self-check: entry count')
    old = iter(entries)
    for y in e2:
        if y is e2[-2]:
            if y.name != name or y.f != f or \
                    out[y.data_off:y.data_off + len(frag)] != frag:
                raise Error('self-check: new entry wrong')
            continue
        x = next(old)
        fx = dict(x.f)
        data = p.cpio[x.data_off:x.data_off + x.f['filesize']]
        if x is rc:
            fx['filesize'] = len(newrc)
            data = newrc
        if x.name != y.name or fx != y.f or \
                out[y.data_off:y.data_off + y.f['filesize']] != data:
            raise Error('self-check: entry %s changed unexpectedly' %
                        safe_name(x.name))
    if out[end2:] != p.cpio[end:]:
        raise Error('self-check: trailer/tail changed')
    blob = compress(p, out)
    if decompress(blob).cpio != out:
        raise Error('self-check: recompressed ramdisk does not decode')
    write_out(a.out, blob)
    print('added /%s (%d bytes) and its import; cpio %d -> %d bytes; %s; '
          'wrote %s (%d bytes)' % (FRAGMENT, len(frag), len(p.cpio), len(out),
                                   p.describe(), a.out, len(blob)))
    if p.params is None:
        print('note: the stock compressed stream is not reproducible; the '
              'output uses default %s settings' % p.fmt)
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
    p.add_argument('--kernel', help='kernel Image: each new module vermagic '
                   'must equal the stock one with the release replaced by '
                   "this Image's release")
    p.add_argument('--symvers', help='Module.symvers of the kernel build: '
                   'also compare the module_layout CRC in __versions')
    p.set_defaults(fn=cmd_replace)
    p = sub.add_parser('add-rc', help='add /%s and import it from /init.rc'
                       % FRAGMENT)
    p.add_argument('ramdisk')
    p.add_argument('out')
    p.add_argument('--fragment', required=True, metavar='FILE',
                   help='init fragment (ASCII, up to %d bytes)' % MAX_FRAGMENT)
    p.set_defaults(fn=cmd_add_rc)
    a = ap.parse_args()
    try:
        return a.fn(a)
    except (Error, OSError, struct.error, ValueError, UnicodeError,
            KeyError, TypeError, lzma.LZMAError, zlib.error, MemoryError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
