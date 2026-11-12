#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# sprd_adbkey_check.py - check that PATH is an adb PUBLIC key file fit for
# /adb_keys: basename adbkey.pub, readable regular file, up to 4 KiB, exactly
# one 'QAAAA...' base64 line followed by a comment. Prints nothing about the
# content; exit status 0 if it is fine, 1 with a reason otherwise.
# Run with "python3 -I".

import os
import re
import sys

LINE = re.compile(rb'QAAAA[A-Za-z0-9+/]+={0,2} [!-~][ -~]*')


def check(path):
    if not path:
        return 'ADB_PUBKEY is not set'
    if os.path.basename(path) != 'adbkey.pub':
        return 'the key file name must be adbkey.pub'
    try:
        if not os.path.isfile(path):
            return 'not a regular file'
        with open(path, 'rb') as f:
            d = f.read(4097)
    except OSError as e:
        return 'unreadable (%s)' % e.strerror
    if len(d) > 4096:
        return 'over 4096 bytes'
    lines = d.split(b'\n')
    if lines[-1] == b'':
        lines.pop()
    if len(lines) != 1 or not LINE.fullmatch(lines[0].rstrip(b'\r')):
        return 'not a single "QAAAA... comment" public key line'
    return None


def main():
    if len(sys.argv) != 2:
        print('usage: sprd_adbkey_check.py PATH', file=sys.stderr)
        return 2
    why = check(sys.argv[1])
    if why:
        print('error: %s' % why, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
