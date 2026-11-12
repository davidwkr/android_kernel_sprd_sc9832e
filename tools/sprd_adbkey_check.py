#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# sprd_adbkey_check.py - check that PATH is an adb PUBLIC key file fit for
# /adb_keys: basename adbkey.pub, a regular file (not a symlink) up to 4 KiB,
# exactly one line "<base64> comment" where the base64 (strict) is Android's
# 524-byte RSAPublicKey (len=64 words, n0inv, 2048-bit modulus, rr, exponent
# 3 or 65537) with n0inv and rr consistent with the modulus. Prints nothing
# about the content; exit status 0 if it is fine, 1 with a reason otherwise.
# Run with "python3 -I".

import base64
import binascii
import os
import re
import stat
import sys

LINE = re.compile(rb'([A-Za-z0-9+/]+={0,2}) ([!-~][ -~]*)')
WORDS = 64
KEYLEN = 8 + 2 * 4 * WORDS + 4


def struct_error(raw):
    """Reason the 524 decoded bytes are not an Android RSAPublicKey, or None."""
    if len(raw) != KEYLEN:
        return 'decoded key is %d bytes, not %d' % (len(raw), KEYLEN)
    le = lambda b: int.from_bytes(b, 'little')
    words = le(raw[0:4])
    n0inv = le(raw[4:8])
    n = le(raw[8:8 + 4 * WORDS])
    rr = le(raw[8 + 4 * WORDS:8 + 8 * WORDS])
    exp = le(raw[KEYLEN - 4:])
    if words != WORDS:
        return 'key length is %d words, not %d' % (words, WORDS)
    if exp not in (3, 65537):
        return 'public exponent is not 3 or 65537'
    if not n & 1 or n >> (32 * WORDS - 1) != 1:
        return 'modulus is not an odd 2048-bit number'
    if (n0inv * n + 1) & 0xffffffff:
        return 'n0inv does not match the modulus'
    if rr != pow(1 << (32 * WORDS), 2, n):
        return 'rr does not match the modulus'
    return None


def check(path):
    if not path:
        return 'ADB_PUBKEY is not set'
    if os.path.basename(path) != 'adbkey.pub':
        return 'the key file name must be adbkey.pub'
    try:
        if not stat.S_ISREG(os.lstat(path).st_mode):
            return 'not a regular file (symlinks are refused)'
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'rb') as f:
            if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
                return 'not a regular file'
            d = f.read(4097)
    except OSError as e:
        return 'unreadable (%s)' % e.strerror
    if len(d) > 4096:
        return 'over 4096 bytes'
    lines = d.split(b'\n')
    if lines[-1] == b'':
        lines.pop()
    m = len(lines) == 1 and LINE.fullmatch(lines[0].rstrip(b'\r'))
    if not m:
        return 'not a single "<base64> comment" public key line'
    try:
        raw = base64.b64decode(m.group(1), validate=True)
    except binascii.Error:
        return 'key is not valid base64'
    return struct_error(raw)


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
