#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Tests for sprd_bootimg.py dt-bootargs on a synthetic boot image with a
# two-DTB SPRD table (no device dumps). Needs dtc, fdtget and fdtput.
# Run: python3 -I tools/sprd_bootimg_test.py

import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'sprd_bootimg.py')
_spec = importlib.util.spec_from_file_location('sprd_bootimg', TOOL)
sb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sb)

ARGS = ('earlycon=sprd_serial,0x70100000,115200n8 console=ttyS1,115200n8 '
        'loglevel=1 init=/init root=/dev/ram0 rw androidboot.hardware=x')
DTS = '''/dts-v1/;
/ { model = "synthetic"; compatible = "a,b";
    chosen { bootargs = "%s"; };
    node@1 { reg = <1 2>; status = "okay"; }; };
'''
PAGE = 2048


def have_dtc():
    return all(shutil.which(x) for x in ('dtc', 'fdtget', 'fdtput'))


@unittest.skipUnless(have_dtc(), 'device-tree-compiler not installed')
class Case(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.addCleanup(self.d.cleanup)

    def path(self, n):
        return os.path.join(self.d.name, n)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, '-I', TOOL] + list(args),
                              capture_output=True, text=True)

    def dtb(self, bootargs):
        src = self.path('x.dts')
        with open(src, 'w') as f:
            f.write(DTS % bootargs)
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o',
                        self.path('x.dtb'), src], check=True)
        with open(self.path('x.dtb'), 'rb') as f:
            return f.read()

    def table(self, blobs, slack=0x800):
        n = len(blobs)
        off = PAGE
        ents, body = [], b''
        for i, b in enumerate(blobs):
            slot = sb.align(len(b) + slack, PAGE)
            ents.append((0x2668 + i, 1, 0, off, slot, 0, 0, 0))
            body += b.ljust(slot, b'\0')
            off += slot
        hdr = b'SPRD' + struct.pack('<II', 1, n) + b''.join(
            sb.DT_ENTRY.pack(*e) for e in ents)
        return hdr.ljust(PAGE, b'\0') + body

    def image(self, dt):
        k, r = b'K' * 5000, b'R' * 3000
        m = hashlib.sha1()
        for b in (k, r, b''):
            m.update(b)
            m.update(struct.pack('<I', len(b)))
        m.update(dt)
        m.update(struct.pack('<I', len(dt)))
        hdr = struct.pack('<8s10I16s512s32s1024s', b'ANDROID!', len(k),
                          0x8000, len(r), 0x1000000, 0, 0, 0x100, PAGE,
                          len(dt), 0, b'', b'', m.digest().ljust(32, b'\0'),
                          b'')
        out = hdr.ljust(PAGE, b'\0')
        for b in (k, r, b'', dt):
            out += b.ljust(sb.align(len(b), PAGE), b'\0')
        out += bytes(PAGE)
        with open(self.path('stock.img'), 'wb') as f:
            f.write(out)
        return self.path('stock.img')

    def test_edit_all_dtbs(self):
        dt = self.table([self.dtb(ARGS), self.dtb(ARGS + ' extra')])
        src = self.path('dt')
        with open(src, 'wb') as f:
            f.write(dt)
        r = self.run_tool('dt-bootargs', src, self.path('dt7'),
                          '--replace', 'loglevel=1', 'loglevel=7')
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.path('dt7'), 'rb') as f:
            out = f.read()
        self.assertEqual(len(out), len(dt))
        self.assertEqual(out[:PAGE], dt[:PAGE])     # ids, offsets, sizes
        ver, ents = sb.dt_entries(out)
        for i, e in enumerate(ents):
            blob = out[e[3]:e[3] + e[4]]
            total, _ = sb.fdt_root_props(blob)
            self.assertFalse(any(blob[total:]))     # zero padding kept
            want = (ARGS + (' extra' if i else '')).replace('loglevel=1',
                                                            'loglevel=7')
            self.assertEqual(self.get(blob[:total]), want)

    def get(self, blob):
        with open(self.path('g.dtb'), 'wb') as f:
            f.write(blob)
        return subprocess.run(['fdtget', '-t', 's', self.path('g.dtb'),
                               '/chosen', 'bootargs'], capture_output=True,
                              text=True).stdout.strip()

    def test_refusals(self):
        dt = self.table([self.dtb(ARGS), self.dtb(ARGS + ' loglevel=1')])
        src = self.path('dt')
        with open(src, 'wb') as f:
            f.write(dt)
        for old in ('loglevel=9', 'loglevel=1', '', 'e'):
            r = self.run_tool('dt-bootargs', src, self.path('o'),
                              '--replace', old, 'X')
            self.assertEqual(r.returncode, 1, old)
            self.assertFalse(os.path.exists(self.path('o')), old)
        # one DTB without it, one with it exactly once: still refused
        with open(src, 'wb') as f:
            f.write(self.table([self.dtb(ARGS), self.dtb('console=x')]))
        r = self.run_tool('dt-bootargs', src, self.path('o'),
                          '--replace', 'loglevel=1', 'loglevel=7')
        self.assertEqual(r.returncode, 1)

    def test_too_big_for_slot(self):
        src = self.path('dt')
        with open(src, 'wb') as f:
            f.write(self.table([self.dtb(ARGS)], slack=0))
        r = self.run_tool('dt-bootargs', src, self.path('o'),
                          '--replace', 'loglevel=1', 'loglevel=' + '7' * 4000)
        self.assertEqual(r.returncode, 1)
        self.assertIn('slot', r.stderr)

    def test_not_a_table(self):
        with open(self.path('dt'), 'wb') as f:
            f.write(self.dtb(ARGS))
        r = self.run_tool('dt-bootargs', self.path('dt'), self.path('o'),
                          '--replace', 'loglevel=1', 'loglevel=7')
        self.assertEqual(r.returncode, 1)

    def test_repack_id_check(self):
        img = self.image(self.table([self.dtb(ARGS)]))
        r = self.run_tool('info', img)
        self.assertIn('id check       OK', r.stdout, r.stdout + r.stderr)
        u = self.path('u')
        self.assertEqual(self.run_tool('unpack', img, u).returncode, 0)
        r = self.run_tool('dt-bootargs', os.path.join(u, 'dt'),
                          self.path('dt7'), '--replace', 'loglevel=1',
                          'loglevel=7')
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.path('new.img')
        r = self.run_tool('pack', u, out, '--dt', self.path('dt7'))
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_tool('info', out)
        self.assertIn('id check       OK', r.stdout, r.stdout + r.stderr)
        with open(img, 'rb') as f:
            a = f.read()
        with open(out, 'rb') as f:
            b = f.read()
        self.assertEqual(len(a), len(b))
        self.assertNotEqual(a, b)


if __name__ == '__main__':
    unittest.main()
