#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# mk_firststage_ramdisk.py - build the system-as-root first-stage ramdisk
# that an Android 10+ GSI needs on the UNISOC SL8541E watches.
#
#   Copyright (C) 2026 the kwt kernel maintainers
#
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License version 2 as
#   published by the Free Software Foundation.
#
#   mk_firststage_ramdisk.py --init FILE --init-sha256 HEX --fstab FILE
#                            -o OUT [--hardware NAME] [--manifest FILE]
#                            [--arch arm|arm64] [--allow-avb]
#
# The ramdisk is a gzip-compressed newc cpio with
#   /init                       the static first-stage init (mode 0750)
#   /fstab.<hardware>           the first-stage fstab (mode 0644)
#   /first_stage_ramdisk/fstab.<hardware>   the same fstab, see below
#   and the empty directories init expects (DIRS below).
#
# The output is reproducible: entries sorted by path, inode numbers
# assigned in that order, uid/gid 0, mtime 0, a fixed gzip header and a
# fixed zlib setting. The manifest (one "SHA256  MODE  PATH" line per entry,
# "-" for a directory) is written next to the image when --manifest is given.
#
# --init is refused unless its sha256 equals --init-sha256 and its ELF
# headers describe a static executable for --arch (class, byte order,
# machine, type, program headers; no PT_INTERP). The file is only parsed,
# never executed. The fstab is refused unless every line has 5 fields,
# mounts /system or /vendor (once each, /system required) and carries
# first_stage_mount, and has no avb flag unless --allow-avb is given.
#
# Only the python3 standard library is used. Inputs are untrusted data.
# Run with "python3 -I".

import argparse
import hashlib
import os
import re
import struct
import sys
import tempfile
import zlib

MAX_INIT = 16 << 20
MAX_FSTAB = 4096
MAX_PHNUM = 64
MTIME = 0
ELF_ARCH = {      # name: (EI_CLASS, e_machine, e_phentsize)
    'arm': (1, 40, 32),
    'arm64': (2, 183, 56),
}
PT_LOAD = 1
PT_INTERP = 3
ET_EXEC = 2
ET_DYN = 3
FSTAB_MOUNTS = ('/system', '/vendor')
HARDWARE_RE = re.compile(r'[A-Za-z0-9_.-]{1,64}')
SHA256_RE = re.compile(r'[0-9a-fA-F]{64}')
# directories, all mode 0755 and empty; init needs them to exist in the
# ramdisk because it mounts on them (or moves a mount to them) before the
# second stage and cannot create them in a read-only system image
DIRS = (
    # mount points of the early tmpfs / pseudo file systems
    'dev',                   # devtmpfs/tmpfs: nodes, /dev/block, selinux
    'proc',                  # procfs: cmdline, mounts
    'sys',                   # sysfs: block devices for wait/by-name
    'mnt',                   # tmpfs, then /mnt/vendor, /mnt/product
    # A10+ extras
    'metadata',              # /metadata mount point (A10+ first stage)
    'debug_ramdisk',         # A11+ tmpfs: userdebug adb_debug.prop
    'second_stage_resources',  # A13+ tmpfs handed to the second stage
    # the real file systems
    'system',                # the GSI system image (system-as-root)
    'vendor',                # the stock vendor partition
    # A13+ first-stage init switches its root here to boot normally
    # instead of recovery, so the fstab is looked up here as well
    'first_stage_ramdisk',
)
# /dev is left empty on purpose: the stock 8.1 ramdisks do not ship device
# nodes either, init mounts a tmpfs over it and creates console/null itself
# (the kernel's "unable to open an initial console" warning is harmless).


class Error(Exception):
    pass


def read_file(path, limit):
    with open(path, 'rb') as f:
        d = f.read(limit + 1)
    if len(d) > limit:
        raise Error('%s: over %d bytes, refusing' % (os.path.basename(path),
                                                     limit))
    return d


def check_init(d, arch):
    """Parse the ELF headers of d: a static executable for arch."""
    cls, mach, phentsize = ELF_ARCH[arch]
    if len(d) < 52 or d[:4] != b'\x7fELF':
        raise Error('init: not an ELF file')
    if d[4] != cls:
        raise Error('init: ELF class %d, %s needs %d' % (d[4], arch, cls))
    if d[5] != 1:
        raise Error('init: not little-endian')
    if d[6] != 1:
        raise Error('init: bad ELF version')
    if cls == 1:
        typ, machine, _, entry, phoff = struct.unpack_from('<HHIII', d, 16)
        phsz, phnum = struct.unpack_from('<HH', d, 42)
    else:
        typ, machine, _, entry, phoff = struct.unpack_from('<HHIQQ', d, 16)
        phsz, phnum = struct.unpack_from('<HH', d, 54)
    if machine != mach:
        raise Error('init: e_machine %d, %s needs %d' % (machine, arch, mach))
    if typ not in (ET_EXEC, ET_DYN):
        raise Error('init: ELF type %d is not an executable' % typ)
    if phsz != phentsize:
        raise Error('init: e_phentsize %d, expected %d' % (phsz, phentsize))
    if not 0 < phnum <= MAX_PHNUM:
        raise Error('init: %d program headers' % phnum)
    if phoff < (52 if cls == 1 else 64) or phoff + phnum * phsz > len(d):
        raise Error('init: program header table outside the file')
    loads = 0
    for i in range(phnum):
        ptype = struct.unpack_from('<I', d, phoff + i * phsz)[0]
        if ptype == PT_INTERP:
            raise Error('init: has PT_INTERP, it is dynamically linked; '
                        'first-stage init must be static')
        loads += ptype == PT_LOAD
    if not loads:
        raise Error('init: no PT_LOAD segment')


def check_fstab(d, allow_avb):
    """Validate the first-stage fstab; return it as is."""
    if not d:
        raise Error('fstab is empty')
    if not d.endswith(b'\n'):
        raise Error('fstab must end with a newline')
    if any((b < 0x20 and b not in (0x09, 0x0a)) or b > 0x7e for b in d):
        raise Error('fstab: printable ASCII only')
    seen = []
    for n, line in enumerate(d.decode('ascii').split('\n'), 1):
        f = line.split()
        if not f or f[0].startswith('#'):
            continue
        if len(f) != 5:
            raise Error('fstab line %d: %d fields, expected 5' % (n, len(f)))
        src, mnt, typ, opts, flags = f
        if mnt not in FSTAB_MOUNTS:
            raise Error('fstab line %d: mount point %s, only %s are '
                        'first-stage' % (n, mnt, ' and '.join(FSTAB_MOUNTS)))
        if mnt in seen:
            raise Error('fstab line %d: %s listed twice' % (n, mnt))
        seen.append(mnt)
        if not src.startswith('/dev/block/'):
            raise Error('fstab line %d: source must be a /dev/block path'
                        % n)
        fl = flags.split(',')
        if 'first_stage_mount' not in fl:
            raise Error('fstab line %d: first_stage_mount missing' % n)
        if not allow_avb and any(x.startswith('avb') for x in fl):
            raise Error('fstab line %d: avb flag (no vbmeta is checked on '
                        'these watches; --allow-avb to keep it)' % n)
    if '/system' not in seen:
        raise Error('fstab has no /system line')
    return d


def newc(ino, name, mode, nlink, data):
    nb = name.encode() + b'\0'
    f = [ino, mode, 0, 0, nlink, MTIME, len(data), 0, 0, 0, 0, len(nb), 0]
    h = b'070701' + b''.join(b'%08X' % v for v in f) + nb
    h += b'\0' * (-len(h) % 4)
    return h + data + b'\0' * (-len(data) % 4)


def build(init, fstab, hardware):
    """Return (gzip bytes, manifest text)."""
    # with no fstab in the DT, first-stage init reads fstab.<hardware>
    # (androidboot.hardware) from the root, or from /first_stage_ramdisk when
    # that is the root it switched to; it also accepts
    # androidboot.fstab_suffix=<x> to pick fstab.<x> instead, which would
    # need a kernel cmdline change and is not used here
    fname = 'fstab.' + hardware
    ents = {d: (0o040755, b'') for d in DIRS}
    ents['init'] = (0o100750, init)
    ents[fname] = (0o100644, fstab)
    ents['first_stage_ramdisk/' + fname] = (0o100644, fstab)
    cpio = b''
    man = []
    for ino, name in enumerate(sorted(ents), 1):
        mode, data = ents[name]
        isdir = mode & 0o170000 == 0o040000
        cpio += newc(ino, name, mode, 2 if isdir else 1, data)
        man.append('%s  %04o  %s\n' % ('-' * 64 if isdir else
                                       hashlib.sha256(data).hexdigest(),
                                       mode & 0o7777, name))
    cpio += newc(0, 'TRAILER!!!', 0, 1, b'')
    co = zlib.compressobj(9, zlib.DEFLATED, -15, 8)
    body = co.compress(cpio) + co.flush()
    # fixed header: no name, mtime 0, XFL 0, OS Unix
    gz = (b'\x1f\x8b\x08\x00\0\0\0\0\x00\x03' + body +
          struct.pack('<II', zlib.crc32(cpio), len(cpio)))
    return gz, ''.join(man)


def write_out(path, blob, mode=0o644):
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=d, prefix='.mk_firststage.')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(blob)
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def main():
    ap = argparse.ArgumentParser(description='build the first-stage ramdisk '
                                 'for an Android 10+ GSI')
    ap.add_argument('--init', required=True, metavar='FILE',
                    help='static first-stage init (ELF, not executed)')
    ap.add_argument('--init-sha256', required=True, metavar='HEX',
                    help='expected sha256 of --init')
    ap.add_argument('--fstab', required=True, metavar='FILE',
                    help='first-stage fstab (system and vendor only)')
    ap.add_argument('-o', '--out', required=True, metavar='OUT',
                    help='ramdisk to write (gzip newc cpio)')
    ap.add_argument('--hardware', default='sl8541e_1h10', metavar='NAME',
                    help='androidboot.hardware, names fstab.NAME '
                         '(default: %(default)s)')
    ap.add_argument('--arch', choices=sorted(ELF_ARCH), default='arm',
                    help='ELF the init must be (default: %(default)s)')
    ap.add_argument('--manifest', metavar='FILE',
                    help='write "SHA256  MODE  PATH" for every entry')
    ap.add_argument('--allow-avb', action='store_true',
                    help='accept avb flags in the fstab')
    a = ap.parse_args()
    try:
        if not HARDWARE_RE.fullmatch(a.hardware):
            raise Error('bad --hardware %r' % a.hardware)
        if not SHA256_RE.fullmatch(a.init_sha256):
            raise Error('--init-sha256 must be 64 hex digits')
        init = read_file(a.init, MAX_INIT)
        got = hashlib.sha256(init).hexdigest()
        if got != a.init_sha256.lower():
            raise Error('init sha256 %s does not match --init-sha256' % got)
        check_init(init, a.arch)
        fstab = check_fstab(read_file(a.fstab, MAX_FSTAB), a.allow_avb)
        gz, man = build(init, fstab, a.hardware)
        write_out(a.out, gz)
        if a.manifest:
            write_out(a.manifest, man.encode())
    except (Error, OSError, struct.error, UnicodeError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    sys.stdout.write(man)
    print('%s: %d bytes, sha256 %s' % (a.out, len(gz),
                                       hashlib.sha256(gz).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
