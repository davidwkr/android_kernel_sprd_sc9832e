#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Tests for sprd_adbkey_check.py with synthetic files (never a real key).
# Run: python3 -I tools/sprd_adbkey_check_test.py

import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    'chk', os.path.join(HERE, 'sprd_adbkey_check.py'))
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)

GOOD = b'QAAAAFAKEFAKEFAKE+/FAKE== nobody@nowhere\n'


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

    def test_bad(self):
        for data in (b'', b'\n', GOOD + GOOD, b'QAAAAFAKE\n',
                     b'QAAAAFAKE= \n', b'xQAAAAFAKE= c\n',
                     b'-----BEGIN PRIVATE KEY-----\n', b'QAAAA' + b'A' * 5000,
                     b'QAAAAFAKE= c\nmore\n', b'QAAAAFAKE= c\x00\n'):
            self.assertIsNotNone(chk.check(self.put(data)), data[:20])

    def test_name_missing_unset(self):
        self.assertIsNotNone(chk.check(self.put(GOOD, 'adbkey')))
        self.assertIsNotNone(chk.check(os.path.join(self.d.name, 'adbkey.pub')))
        self.assertIsNotNone(chk.check(self.d.name))
        self.assertIsNotNone(chk.check(''))
        self.assertIsNotNone(chk.check(None))


if __name__ == '__main__':
    unittest.main()
