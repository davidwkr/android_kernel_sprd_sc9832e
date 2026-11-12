#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Tests for sprd_adbkey_check.py with synthetic files (never a real key). The
# good key is a throwaway 2048-bit key made with openssl in a temp dir; only
# its public half is converted to Android's format, nothing is kept.
# Run: python3 -I tools/sprd_adbkey_check_test.py

import base64
import importlib.util
import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    'chk', os.path.join(HERE, 'sprd_adbkey_check.py'))
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)

def android_pubkey(n, exp=65537):
    words = 64
    n0inv = (-pow(n, -1, 1 << 32)) % (1 << 32)
    rr = pow(1 << (32 * words), 2, n)
    return (words.to_bytes(4, 'little') + n0inv.to_bytes(4, 'little') +
            n.to_bytes(4 * words, 'little') + rr.to_bytes(4 * words, 'little') +
            exp.to_bytes(4, 'little'))


def make_modulus():
    with tempfile.TemporaryDirectory() as d:
        k = os.path.join(d, 'k.pem')
        subprocess.run(['openssl', 'genrsa', '-out', k, '2048'], check=True,
                       stderr=subprocess.DEVNULL)
        out = subprocess.run(['openssl', 'rsa', '-in', k, '-noout', '-modulus'],
                             check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout
    return int(out.decode().strip().split('=')[1], 16)


N = make_modulus()
RAW = android_pubkey(N)


def line(raw, comment=b'nobody@nowhere'):
    return base64.b64encode(raw) + b' ' + comment + b'\n'


GOOD = line(RAW)


class Case(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.addCleanup(self.d.cleanup)

    def put(self, data, name='adbkey.pub'):
        p = os.path.join(self.d.name, name)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def test_good(self):
        self.assertIsNone(chk.check(self.put(GOOD)))
        self.assertIsNone(chk.check(self.put(GOOD.rstrip(b'\n'))))
        self.assertIsNone(chk.check(self.put(line(android_pubkey(N, 3)))))

    def test_bad_line(self):
        for data in (b'', b'\n', GOOD + GOOD, b'QAAAAFAKE\n',
                     b'QAAAAFAKE= \n', b'xQAAAAFAKE= c\n',
                     b'-----BEGIN PRIVATE KEY-----\n', b'QAAAA' + b'A' * 5000,
                     GOOD + b'more\n', GOOD[:-1] + b'\x00\n',
                     b'QAAAAFAKEFAKEFAKE+/FAKE== nobody@nowhere\n'):
            self.assertIsNotNone(chk.check(self.put(data)), data[:20])

    def test_bad_base64(self):
        b64, c = GOOD.split(b' ', 1)
        for bad in (b64[:-1], b64 + b'A', b64[:10] + b'!' + b64[11:],
                    b64.rstrip(b'=')):
            self.assertIsNotNone(chk.check(self.put(bad + b' ' + c)), bad[-8:])

    def test_bad_structure(self):
        def mod(off, val, n=4):
            r = bytearray(RAW)
            r[off:off + n] = val
            return line(bytes(r))
        w = 4 * 64
        for data in (line(RAW[:-1]), line(RAW + b'\0'),
                     mod(0, (63).to_bytes(4, 'little')),
                     mod(4, (1).to_bytes(4, 'little')),
                     mod(8 + 2 * w, (5).to_bytes(4, 'little')),
                     mod(8 + 2 * w, (0).to_bytes(4, 'little')),
                     mod(8, b'\0', 1), mod(8 + w - 1, b'\x7f', 1),
                     mod(8 + w, b'\x01', 1), line(android_pubkey(N, 17))):
            self.assertIsNotNone(chk.check(self.put(data)))

    def test_symlink_and_nonregular(self):
        real = self.put(GOOD, 'real.pub')
        link = os.path.join(self.d.name, 'adbkey.pub')
        os.symlink(real, link)
        self.assertIsNotNone(chk.check(link))
        fifo = os.path.join(self.d.name, 'sub', 'adbkey.pub')
        os.makedirs(os.path.dirname(fifo))
        os.mkfifo(fifo)
        self.assertIsNotNone(chk.check(fifo))

    def test_name_missing_unset(self):
        self.assertIsNotNone(chk.check(self.put(GOOD, 'adbkey')))
        self.assertIsNotNone(chk.check(os.path.join(self.d.name, 'adbkey.pub')))
        self.assertIsNotNone(chk.check(self.d.name))
        self.assertIsNotNone(chk.check(''))
        self.assertIsNotNone(chk.check(None))


if __name__ == '__main__':
    unittest.main()
