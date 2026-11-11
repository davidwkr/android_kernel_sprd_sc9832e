#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Tests for mk_firststage_ramdisk.py on synthetic ELF stubs (no vendor
# binaries). Run: python3 -I tools/mk_firststage_ramdisk_test.py

import gzip
import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'mk_firststage_ramdisk.py')
_spec = importlib.util.spec_from_file_location('mk_firststage_ramdisk', TOOL)
mk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mk)
FSTAB = os.path.join(HERE, '..', 'devices', 'common', 'gsi',
                     'fstab.sl8541e_1h10')
SYS = (b'/dev/block/by-name/system /system ext4 ro,barrier=1 '
       b'wait,first_stage_mount\n')
VND = SYS.replace(b'system', b'vendor')


def elf(cls=1, mach=40, typ=2, ptypes=(1, 1), data=1, phentsize=None,
        phoff=None, loads=None, entry=None, version=1, ehsize=None):
    """Minimal ELF: header, program header table, 4 bytes of code.

    Each PT_LOAD in ptypes gets one segment from loads (a dict of
    p_offset, p_filesz, p_memsz, p_vaddr, p_align, p_flags); the default
    maps the whole file from offset 0 at 0x8000 as R+X (page congruent),
    and the default entry is the code word.
    """
    sz = 32 if cls == 1 else 56
    ps = sz if phentsize is None else phentsize
    hsz = 52 if cls == 1 else 64
    off = hsz if phoff is None else phoff
    code = off + len(ptypes) * sz
    total = code + 4
    if entry is None:
        entry = 0x8000 + code
    ident = b'\x7fELF' + bytes([cls, data, 1]) + b'\0' * 9
    eh = hsz if ehsize is None else ehsize
    if cls == 1:
        h = ident + struct.pack('<HHIIIIIHHHHHH', typ, mach, version, entry,
                                off, 0, 0, eh, ps, len(ptypes), 0, 0, 0)
    else:
        h = ident + struct.pack('<HHIQQQIHHHHHH', typ, mach, version, entry,
                                off, 0, 0, eh, ps, len(ptypes), 0, 0, 0)
    h = h.ljust(hsz, b'\0')
    ph = b''
    for t in ptypes:
        if t == 1:
            L = dict(offset=0, filesz=total, memsz=total, vaddr=0x8000, align=4,
                     flags=5)
            L.update(loads or {})
            if cls == 1:
                e = struct.pack('<IIIIIIII', t, L['offset'], L['vaddr'],
                                L['vaddr'], L['filesz'], L['memsz'],
                                L['flags'], L['align'])
            else:
                e = struct.pack('<IIQQQQQQ', t, L['flags'], L['offset'],
                                L['vaddr'], L['vaddr'], L['filesz'],
                                L['memsz'], L['align'])
        else:
            e = struct.pack('<I', t)
        ph += e.ljust(sz, b'\0')
    return h.ljust(off, b'\0') + ph + b'code'


STATIC = elf()
DYNAMIC = elf(ptypes=(6, 3, 1, 1, 2))
FAR_PHOFF = STATIC[:28] + struct.pack('<I', 1 << 20) + STATIC[32:]


class Case(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.addCleanup(self.d.cleanup)

    def path(self, n):
        return os.path.join(self.d.name, n)

    def put(self, n, data):
        with open(self.path(n), 'wb') as f:
            f.write(data)
        return self.path(n)

    def run_tool(self, init=STATIC, fstab=SYS + VND, out='rd.gz', sha=None,
                 extra=()):
        i = self.put('init', init)
        f = self.put('fstab', fstab)
        sha = sha or hashlib.sha256(init).hexdigest()
        return subprocess.run([sys.executable, '-I', TOOL, '--init', i,
                               '--init-sha256', sha, '--fstab', f,
                               '-o', self.path(out)] + list(extra),
                              capture_output=True, text=True)

    def unpack(self, p):
        with open(p, 'rb') as f:
            c = gzip.decompress(f.read())
        out, order, pos = {}, [], 0
        while True:
            fl = [int(c[pos + 6 + 8 * i:pos + 14 + 8 * i], 16)
                  for i in range(13)]
            ns, sz = fl[11], fl[6]
            nm = c[pos + 110:pos + 110 + ns - 1].decode()
            do = (pos + 110 + ns + 3) & ~3
            out[nm] = (fl, c[do:do + sz])
            order.append(nm)
            pos = (do + sz + 3) & ~3
            if nm == 'TRAILER!!!':
                self.assertEqual(pos, len(c))
                return out, order

    def refused(self, msg, **kw):
        r = self.run_tool(**kw)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn(msg, r.stderr)
        self.assertFalse(os.path.exists(self.path(kw.get('out', 'rd.gz'))))

    def test_layout(self):
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stderr)
        ents, order = self.unpack(self.path('rd.gz'))
        self.assertEqual(order[:-1], sorted(order[:-1]))
        self.assertEqual(order[-1], 'TRAILER!!!')
        self.assertEqual(set(order[:-1]), set(mk.DIRS) | {
            'init', 'fstab.sl8541e_1h10',
            'first_stage_ramdisk/fstab.sl8541e_1h10'})
        for n in mk.DIRS:
            self.assertEqual(ents[n][0][1], 0o040755, n)
            self.assertEqual(ents[n][1], b'')
        for n in order[:-1]:
            fl = ents[n][0]
            self.assertEqual((fl[2], fl[3], fl[5]), (0, 0, 0), n)
        self.assertEqual(ents['init'][0][1], 0o100750)
        self.assertEqual(ents['init'][1], STATIC)
        for n in ('fstab.sl8541e_1h10',
                  'first_stage_ramdisk/fstab.sl8541e_1h10'):
            self.assertEqual(ents[n][0][1], 0o100644)
            self.assertEqual(ents[n][1], SYS + VND)
        inos = [ents[n][0][0] for n in order[:-1]]
        self.assertEqual(inos, list(range(1, len(inos) + 1)))
        # parents come before children
        self.assertLess(order.index('first_stage_ramdisk'),
                        order.index('first_stage_ramdisk/fstab.sl8541e_1h10'))

    def test_deterministic_and_manifest(self):
        m1, m2 = self.path('m1'), self.path('m2')
        self.assertEqual(self.run_tool(out='a.gz', extra=('--manifest', m1,)
                                       ).returncode, 0)
        self.assertEqual(self.run_tool(out='b.gz', extra=('--manifest', m2,)
                                       ).returncode, 0)
        with open(self.path('a.gz'), 'rb') as a, open(self.path('b.gz'),
                                                      'rb') as b:
            self.assertEqual(a.read(), b.read())
        with open(m1) as a, open(m2) as b:
            man = a.read()
            self.assertEqual(man, b.read())
        lines = [x.split() for x in man.splitlines()]
        self.assertEqual([x[2] for x in lines],
                         sorted(x[2] for x in lines))
        d = {x[2]: x for x in lines}
        self.assertEqual(d['init'][0], hashlib.sha256(STATIC).hexdigest())
        self.assertEqual(d['init'][1], '0750')
        self.assertEqual(d['dev'][0], '-' * 64)
        self.assertEqual(d['fstab.sl8541e_1h10'][0],
                         hashlib.sha256(SYS + VND).hexdigest())

    def test_hardware(self):
        r = self.run_tool(out='h.gz', extra=('--hardware', 'x_1'))
        self.assertEqual(r.returncode, 0, r.stderr)
        _, order = self.unpack(self.path('h.gz'))
        self.assertIn('first_stage_ramdisk/fstab.x_1', order)
        self.refused('bad --hardware', out='h2.gz', 
                     extra=('--hardware', '../x'))

    def test_init_sha_mismatch(self):
        self.refused('does not match --init-sha256', sha='0' * 64)
        self.refused('64 hex digits', sha='abc')

    def test_init_dynamic_and_not_elf(self):
        self.refused('PT_INTERP', init=DYNAMIC)
        self.refused('not an ELF', init=b'#!/bin/sh\n' + b'x' * 100)

    def test_init_header_checks(self):
        for init, msg in ((elf(cls=2, mach=183), 'ELF class 2'),
                          (elf(mach=183), 'e_machine 183'),
                          (elf(typ=1), 'not an executable'),
                          (elf(typ=4), 'not an executable'),
                          (elf(data=2), 'little-endian'),
                          (elf(ptypes=(4, 4)), 'no PT_LOAD'),
                          (elf(ptypes=()), 'program headers'),
                          (elf(phentsize=20), 'e_phentsize'),
                          (FAR_PHOFF, 'outside the file'),
                          (STATIC[:40], 'not an ELF')):
            self.refused(msg, init=init)
        self.assertEqual(self.run_tool(init=elf(typ=3), out='pie.gz'
                                       ).returncode, 0)

    def test_init_load_checks(self):
        big = 0xffffff00
        a64 = dict(cls=2, mach=183)
        for init, msg in (
                (elf(version=0), 'e_version'),
                (elf(ehsize=40), 'e_ehsize'),
                (elf(ptypes=(1,), loads=dict(offset=big, filesz=4096,
                                             memsz=1)), 'outside the file'),
                (elf(loads=dict(offset=big, filesz=4096, memsz=1)),
                 'outside the file'),
                (elf(loads=dict(filesz=4, memsz=2)), 'sizes'),
                (elf(loads=dict(vaddr=0xfffffffe, filesz=4, memsz=4)),
                 'user address space'),
                (elf(loads=dict(align=3)), 'power of two'),
                (elf(loads=dict(align=0x1000, vaddr=0x8004)),
                 'not congruent'),
                (elf(loads=dict(vaddr=0x8004)), 'not congruent modulo 4096'),
                (elf(loads=dict(offset=116, vaddr=0x8000, filesz=4,
                                memsz=4), entry=0x8000),
                 'not congruent modulo 4096'),
                (elf(loads=dict(flags=4)), 'executable PT_LOAD'),
                (elf(entry=0x9000), 'executable PT_LOAD'),
                (elf(entry=0x8000 + 4096), 'executable PT_LOAD'),
                # the only PT_LOAD is empty: nothing holds the entry
                (elf(ptypes=(1,), loads=dict(filesz=0, memsz=0)),
                 'executable PT_LOAD'),
                (elf(loads=dict(filesz=0, memsz=0)), 'executable PT_LOAD'),
                # ends above TASK_SIZE_32 (0xfffff000)
                (elf(loads=dict(vaddr=0xfffff000, memsz=0x1000), entry=
                     0xfffff000), 'user address space'),
                ):
            self.refused(msg, init=init)
        a64x = ('--arch', 'arm64')
        self.refused('e_ehsize', init=elf(ehsize=52, **a64), extra=a64x)
        self.refused('user address space', init=elf(
            loads=dict(vaddr=(1 << 64) - 4096, memsz=4096, align=4096),
            entry=0xffffffffffff0000, **a64), extra=a64x)
        self.refused('user address space', init=elf(
            loads=dict(vaddr=1 << 39, memsz=4096), entry=1 << 39, **a64),
            extra=a64x)
        self.refused('user address space', init=elf(
            loads=dict(vaddr=(1 << 64) - 2, filesz=4, memsz=4), **a64), extra=a64x)
        self.refused('outside the file', init=elf(
            loads=dict(offset=(1 << 64) - 8, filesz=16, memsz=16), **a64),
            extra=a64x)
        # page congruence rather than p_align: align 0/1/4096 all load
        for ld, ent in ((dict(align=0), None), (dict(align=1), None),
                        (dict(align=0x1000), None),
                        (dict(offset=116, filesz=4, memsz=4, vaddr=0x8074,
                              align=0x1000), 0x8074),
                        (dict(vaddr=0xffffe000, memsz=4, filesz=4),
                         0xffffe000)):
            r = self.run_tool(init=elf(loads=ld, entry=ent), out='ok.gz')
            self.assertEqual(r.returncode, 0, (ld, r.stderr))
        r = self.run_tool(init=elf(loads=dict(
            vaddr=(1 << 39) - 4096, memsz=4, filesz=4),
            entry=(1 << 39) - 4096, **a64), out='ok.gz', extra=a64x)
        self.assertEqual(r.returncode, 0, r.stderr)
        # entry in the second of two segments, the first not executable
        two = elf(ptypes=(1, 1))
        self.assertEqual(self.run_tool(init=two, out='two.gz').returncode, 0)

    def test_dyn_and_bss(self):
        # an ET_DYN without PT_INTERP is not held to ELF_ET_DYN_BASE:
        # a large static PIE loads
        for kw, a in ((dict(), ()), (dict(cls=2, mach=183),
                                     ('--arch', 'arm64'))):
            r = self.run_tool(init=elf(typ=3, loads=dict(
                vaddr=0, filesz=0x60, memsz=0x60000000, flags=7),
                entry=0x80, **kw), out='pie.gz', extra=a)
            self.assertEqual(r.returncode, 0, r.stderr)
        # total_mapping_size(): last.vaddr + last.memsz - first.vaddr
        # rounded down; the last PT_LOAD is empty and at 0, first at 0x8000
        sz, hsz = 32, 52
        good = elf(typ=3, ptypes=(1, 1))
        o = hsz + sz
        e = struct.pack('<IIIIIIII', 1, 0, 0, 0, 0, 0, 4, 0)
        empty = good[:o] + e.ljust(sz, b'\0') + good[o + sz:]
        self.refused('ET_DYN mapping size', init=empty)
        # an ET_EXEC maps each segment on its own and skips empty ones
        exe = empty[:16] + struct.pack('<H', 2) + empty[18:]
        r = self.run_tool(init=exe, out='exe.gz')
        self.assertEqual(r.returncode, 0, r.stderr)
        # padzero(): a partial page of bss after RX data cannot be zeroed
        self.refused('not writable', init=elf(
            loads=dict(filesz=4, memsz=4096)))
        self.refused('not writable', init=elf(
            loads=dict(vaddr=0xffffe000, filesz=4, memsz=4096),
            entry=0xffffe000))
        # a writable segment may have a partial-page bss
        r = self.run_tool(init=elf(loads=dict(flags=7, memsz=4096 * 3)),
                          out='rw.gz')
        self.assertEqual(r.returncode, 0, r.stderr)
        # RX text, then a separate RW data+bss segment holding elf_bss
        d = elf(ptypes=(1, 1))
        o = 52 + 32
        rw = struct.pack('<IIIIIIII', 1, 0, 0x20000, 0x20000, 4, 0x3000, 6,
                         0x1000)
        d = d[:o] + rw + d[o + 32:]
        r = self.run_tool(init=d, out='rwbss.gz')
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_empty_load(self):
        # an empty extra PT_LOAD next to the executable one is legal
        for a in ((), ('--arch', 'arm64')):
            kw = dict(cls=2, mach=183) if a else {}
            sz = 56 if a else 32
            hsz = 64 if a else 52
            good = elf(ptypes=(1, 1), **kw)
            # turn the second PT_LOAD into an empty one at vaddr 0
            o = hsz + sz
            e = struct.pack('<IIQQQQQQ', 1, 4, 0, 0, 0, 0, 0, 0) if a else \
                struct.pack('<IIIIIIII', 1, 0, 0, 0, 0, 0, 4, 0)
            empty = good[:o] + e.ljust(sz, b'\0') + good[o + sz:]
            r = self.run_tool(init=empty, out='e.gz', extra=a)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_arch(self):
        a64 = elf(cls=2, mach=183)
        self.refused('ELF class 2', init=a64)
        r = self.run_tool(init=a64, out='a64.gz', extra=('--arch', 'arm64'))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.refused('ELF class 1', out='x.gz', extra=('--arch', 'arm64'))
        r = self.run_tool(init=elf(cls=2, mach=183, ptypes=(3, 1)),
                          out='x.gz', extra=('--arch', 'arm64'))
        self.assertEqual(r.returncode, 1)
        self.assertIn('PT_INTERP', r.stderr)

    def test_fstab_validation(self):
        for fs, msg in ((b'', 'fstab is empty'),
                        (SYS.rstrip(b'\n'), 'end with a newline'),
                        (SYS.replace(b'ro,barrier=1 ', b''), '4 fields'),
                        (SYS + b'x y\n', '2 fields'),
                        (SYS + VND.replace(b'/vendor', b'/data'),
                         'mount point /data'),
                        (SYS.replace(b'/system ', b'/product '),
                         'mount point /product'),
                        (SYS + SYS, 'listed twice'),
                        (VND, 'no /system line'),
                        (b'# c\n\n', 'no /system line'),
                        (SYS.replace(b',first_stage_mount', b''),
                         'first_stage_mount missing'),
                        (SYS.replace(b'wait,', b'wait,avb,'), 'avb flag'),
                        (SYS.replace(b'wait,', b'avb=vbmeta,'), 'avb flag'),
                        (SYS.replace(b'/dev/block', b'/tmp'),
                         '/dev/block path'),
                        (SYS.replace(b'ext4', b'ext\xc3\xa94'),
                         'printable ASCII'),
                        (SYS + b'x' * 5000 + b'\n', 'over 4096 bytes')):
            self.refused(msg, fstab=fs)

    def test_allow_avb_and_comments(self):
        fs = b'# first stage\n\n' + SYS.replace(b'wait,', b'wait,avb,')
        r = self.run_tool(fstab=fs, out='avb.gz', extra=('--allow-avb',))
        self.assertEqual(r.returncode, 0, r.stderr)
        ents, _ = self.unpack(self.path('avb.gz'))
        self.assertEqual(ents['fstab.sl8541e_1h10'][1], fs)   # verbatim

    def test_shipped_fstab(self):
        with open(FSTAB, 'rb') as f:
            fs = f.read()
        self.assertEqual(mk.check_fstab(fs, False), fs)


if __name__ == '__main__':
    unittest.main()
