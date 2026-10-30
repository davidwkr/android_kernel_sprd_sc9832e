#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Tests for sprd_ramdisk.py add-rc on a synthetic ramdisk built in a temp
# directory (no device dumps). Run: python3 -I tools/sprd_ramdisk_test.py

import gzip
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'sprd_ramdisk.py')
FRAG = os.path.join(HERE, '..', 'devices', 'common', 'init.watch_tweaks.rc')


def newc(ino, name, data=b'', mode=0o100644, nlink=1, mtime=0):
    nb = name.encode() + b'\0'
    f = [ino, mode, 0, 0, nlink, mtime, len(data), 0, 0, 0, 0, len(nb), 0]
    h = b'070701' + b''.join(b'%08X' % v for v in f) + nb
    h += b'\0' * (-len(h) % 4)
    return h + data + b'\0' * (-len(data) % 4)


def build(entries):
    c = b''.join(newc(*e[:2], **e[2]) if len(e) == 3 else newc(*e)
                 for e in entries)
    c += newc(0, 'TRAILER!!!', mode=0, nlink=1)
    co = zlib.compressobj(9, zlib.DEFLATED, -15, 8)
    body = co.compress(c) + co.flush()
    hdr = b'\x1f\x8b\x08\x00\0\0\0\0\x00\x03'
    return hdr + body + struct.pack('<II', zlib.crc32(c), len(c))


RC = b'on boot\n    write /proc/sys/vm/dirty_expire_centisecs 200\n'
STOCK = [(1, 'init.rc', RC, 0o100750, 1, 5), (2, 'sbin', b'', 0o040755),
         (3, 'init', b'ELF', 0o100750), (4, 'lib/modules/a.ko', b'xx')]


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

    def run_tool(self, *args):
        return subprocess.run([sys.executable, '-I', TOOL] + list(args),
                              capture_output=True, text=True)

    def entries(self, p):
        r = self.run_tool('list', p)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def unpack(self, p):
        with open(p, 'rb') as f:
            c = gzip.decompress(f.read())
        out, pos = {}, 0
        order = []
        while True:
            fl = [int(c[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
            ns, sz = fl[11], fl[6]
            nm = c[pos + 110:pos + 110 + ns - 1].decode()
            do = (pos + 110 + ns + 3) & ~3
            out[nm] = (fl, c[do:do + sz])
            order.append(nm)
            pos = (do + sz + 3) & ~3
            if nm == 'TRAILER!!!':
                return out, order

    def stock(self, entries=STOCK):
        ents = [(e[0], e[1], dict(data=e[2], mode=e[3] if len(e) > 3 else 0o100644,
                                  nlink=e[4] if len(e) > 4 else 1,
                                  mtime=e[5] if len(e) > 5 else 0))
                for e in entries]
        return self.put('in.gz', build([(a, b, k) for a, b, k in ents]))

    def test_add_and_roundtrip(self):
        src = self.stock()
        out = self.path('out.gz')
        r = self.run_tool('add-rc', src, out, '--fragment', FRAG)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.entries(out)                       # re-parses
        a, oa = self.unpack(src)
        b, ob = self.unpack(out)
        self.assertEqual(ob[:-2], oa[:-1])      # same order
        self.assertEqual(ob[-2:], ['init.watch_tweaks.rc', 'TRAILER!!!'])
        with open(FRAG, 'rb') as f:
            frag = f.read()
        self.assertEqual(b['init.watch_tweaks.rc'][1], frag)
        self.assertEqual(b['init.watch_tweaks.rc'][0][1:6],
                         b['init.rc'][0][1:6])   # mode uid gid nlink mtime
        self.assertEqual(b['init.rc'][1],
                         RC + b'import /init.watch_tweaks.rc\n')
        for n in oa:
            if n in ('init.rc', 'TRAILER!!!'):
                continue
            self.assertEqual(a[n], b[n], n)
        fa, fb = list(a['init.rc'][0]), list(b['init.rc'][0])
        fa[6] = fb[6] = 0
        self.assertEqual(fa, fb)                # mode, mtime kept
        inos = [v[0][0] for n, v in b.items() if n != 'TRAILER!!!']
        self.assertEqual(len(inos), len(set(inos)))   # unique inode numbers

    def test_idempotent(self):
        src = self.stock()
        o1, o2 = self.path('o1.gz'), self.path('o2.gz')
        self.assertEqual(self.run_tool('add-rc', src, o1,
                                       '--fragment', FRAG).returncode, 0)
        r = self.run_tool('add-rc', o1, o2, '--fragment', FRAG)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('already applied', r.stdout)
        with open(o1, 'rb') as x, open(o2, 'rb') as y:
            self.assertEqual(x.read(), y.read())

    def test_missing_init_rc(self):
        src = self.stock([e for e in STOCK if e[1] != 'init.rc'])
        r = self.run_tool('add-rc', src, self.path('o.gz'),
                          '--fragment', FRAG)
        self.assertEqual(r.returncode, 1)
        self.assertIn('no /init.rc', r.stderr)
        self.assertFalse(os.path.exists(self.path('o.gz')))

    def test_init_rc_not_regular(self):
        ents = [(1, 'init.rc', b'/system/etc/init.rc', 0o120777)] + STOCK[1:]
        r = self.run_tool('add-rc', self.stock(ents), self.path('o.gz'),
                          '--fragment', FRAG)
        self.assertEqual(r.returncode, 1)
        self.assertIn('not a regular file', r.stderr)

    def test_half_applied_and_bad_fragment(self):
        ents = list(STOCK)
        ents[0] = (1, 'init.rc', RC + b'import /init.watch_tweaks.rc\n',
                   0o100750)
        r = self.run_tool('add-rc', self.stock(ents), self.path('o.gz'),
                          '--fragment', FRAG)
        self.assertEqual(r.returncode, 1)
        self.assertIn('imports /init.watch_tweaks.rc but the file is missing',
                      r.stderr)
        for bad, msg in ((b'', 'fragment is empty'),
                         (b'on boot\n\xc3\xa9\n', 'printable ASCII'),
                         (b'# only\n', 'only comments'),
                         (b'on boot', 'end with a newline')):
            fr = self.put('f.rc', bad)
            r = self.run_tool('add-rc', self.stock(), self.path('o.gz'),
                              '--fragment', fr)
            self.assertEqual(r.returncode, 1, bad)
            self.assertIn(msg, r.stderr)
        big = self.put('big.rc', b'# x\non boot\n' + b'#' * 5000 + b'\n')
        r = self.run_tool('add-rc', self.stock(), self.path('o.gz'),
                          '--fragment', big)
        self.assertEqual(r.returncode, 1)
        self.assertIn('fragment over 4096 bytes', r.stderr)

    def refused(self, src, msg, frag=FRAG):
        out = self.path('o.gz')
        r = self.run_tool('add-rc', src, out, '--fragment', frag)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn(msg, r.stderr)
        self.assertFalse(os.path.exists(out))

    def test_hard_linked_init_rc(self):
        ents = [(1, 'init.rc', RC, 0o100750, 2)] + STOCK[1:]
        self.refused(self.stock(ents), '/init.rc is a hard link (nlink 2')

    def test_existing_fragment_other_content(self):
        for data, mode in ((b'on boot\n', 0o100644), (b'', 0o120777)):
            ents = list(STOCK)
            ents[0] = (1, 'init.rc', RC + b'import /init.watch_tweaks.rc\n',
                       0o100750)
            ents.append((9, 'init.watch_tweaks.rc', data, mode))
            self.refused(self.stock(ents), 'already exists with different '
                      'content or type')

    def test_fragment_present_not_imported(self):
        with open(FRAG, 'rb') as f:
            frag = f.read()
        ents = STOCK + [(9, 'init.watch_tweaks.rc', frag, 0o100644)]
        self.refused(self.stock(ents), 'does not import it; refusing a '
                  'half-applied ramdisk')

    def test_import_tokenization(self):
        for line in (b'import "/init.watch_tweaks.rc"',
                     b'import /init.watch_tweaks.rc # note',
                     b'import \\\n  /init.watch_tweaks.rc',
                     b'import\t/init.watch_tweaks.rc'):
            ents = list(STOCK)
            ents[0] = (1, 'init.rc', RC + line + b'\n', 0o100750)
            self.refused(self.stock(ents), 'imports /init.watch_tweaks.rc '
                         'but the file is missing')
        # commented out or other path: not an import, so it is added
        for line in (b'# import /init.watch_tweaks.rc',
                     b'import /init.watch_tweaks.rc.bak'):
            ents = list(STOCK)
            ents[0] = (1, 'init.rc', RC + line + b'\n', 0o100750)
            r = self.run_tool('add-rc', self.stock(ents), self.path('o.gz'),
                              '--fragment', FRAG)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_trailing_backslash(self):
        for tail in (b'service x /x \\\n', b'service x /x \\'):
            ents = list(STOCK)
            ents[0] = (1, 'init.rc', RC + tail, 0o100750)
            self.refused(self.stock(ents), 'ends with a backslash line '
                         'continuation')

    def test_missing_final_newline(self):
        ents = list(STOCK)
        ents[0] = (1, 'init.rc', RC.rstrip(b'\n'), 0o100750)
        out = self.path('o.gz')
        r = self.run_tool('add-rc', self.stock(ents), out,
                          '--fragment', FRAG)
        self.assertEqual(r.returncode, 0, r.stderr)
        b, _ = self.unpack(out)
        self.assertEqual(b['init.rc'][1], RC.rstrip(b'\n') +
                         b'\nimport /init.watch_tweaks.rc\n')

    def test_trailing_data(self):
        raw = self.stock()
        with open(raw, 'rb') as f:
            gz = f.read()
        c = gzip.decompress(gz)
        # non-zero bytes after the cpio trailer (valid gzip of longer cpio)
        co = zlib.compressobj(9, zlib.DEFLATED, -15, 8)
        c2 = c + b'JUNK'
        body = co.compress(c2) + co.flush()
        a = self.put('t1.gz', gz[:10] + body +
                     struct.pack('<II', zlib.crc32(c2), len(c2)))
        self.refused(a, 'data after the cpio trailer is non-zero')
        # non-zero bytes after the gzip member
        b = self.put('t2.gz', gz + b'JUNK')
        self.refused(b, 'data after the compressed member is non-zero')

    def test_duplicate_and_dot_alias(self):
        ents = STOCK + [(9, './init.rc', RC, 0o100750)]
        self.refused(self.stock(ents), 'duplicate path')
        ents = [(1, './init.rc', RC, 0o100750)] + STOCK[1:]
        out = self.path('o.gz')
        r = self.run_tool('add-rc', self.stock(ents), out,
                          '--fragment', FRAG)
        self.assertEqual(r.returncode, 0, r.stderr)
        b, ob = self.unpack(out)
        self.assertEqual(ob[-2:], ['./init.watch_tweaks.rc', 'TRAILER!!!'])

    def test_same_path_out_refused(self):
        src = self.stock()
        r = self.run_tool('add-rc', src, src, '--fragment', FRAG)
        self.assertEqual(r.returncode, 1)
        self.assertIn('OUT must differ', r.stderr)


if __name__ == '__main__':
    unittest.main()
