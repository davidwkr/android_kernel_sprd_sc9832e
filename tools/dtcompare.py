#!/usr/bin/env python3
"""Prove two device trees are equivalent.

usage: dtcompare.py [--dtc PATH] STOCK NEW

STOCK / NEW may be a DTB, a DTS or an Android boot image with a SPRD DT
table (the first DTB of the table is used).  Both are decompiled with dtc
into a canonical, sorted text (properties and nodes sorted, phandle values
resolved to node paths) and compared.  Phandle numbering may differ, as long
as every reference resolves to the same node.  Exit 0 if identical, 1 if
there are differences (listed), 2 on usage / tool errors.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sprd_bootimg as sb  # noqa: E402


def load_dtb(path, dtc, tmp):
    data = open(path, 'rb').read()
    if data[:8] == sb.MAGIC:
        h, lay, _ = sb.parse(data)
        o, n = lay['dt']
        dt = data[o:o + n]
        ver, ents = sb.dt_entries(dt)
        eo, en = ents[0][3], ents[0][4]
        total = struct.unpack_from('>I', dt, eo + 4)[0]
        return dt[eo:eo + total]
    if data[:4] == b'\xd0\x0d\xfe\xed':
        return data
    out = os.path.join(tmp, 'c.dtb')
    subprocess.run([dtc, '-q', '-I', 'dts', '-O', 'dtb', '-b', '0', '-o', out, path],
                   check=True, stderr=subprocess.DEVNULL)
    return open(out, 'rb').read()


def canon(dtb, dtc, tmp):
    """-> dict path -> {prop: value text}, with phandles resolved to paths."""
    p = os.path.join(tmp, 'x.dtb')
    open(p, 'wb').write(dtb)
    txt = subprocess.run([dtc, '-q', '-I', 'dtb', '-O', 'dts', '-s', p],
                         check=True, capture_output=True, text=True).stdout
    nodes, stack, ph = {}, [], {}
    for ln in txt.split('\n'):
        s = ln.strip()
        m = re.match(r'^([\w,.@+-]*|/) \{$', s) or re.match(r'^\w+: ([\w,.@+-]*) \{$', s)
        if m:
            stack.append(m.group(1))
            nodes['/' + '/'.join(stack[1:])] = {}
            continue
        if s == '};':
            stack.pop()
            continue
        if not s or s.startswith(('/dts-v1/', '//')):
            continue
        cur = nodes['/' + '/'.join(stack[1:])]
        k, _, v = s.partition(' = ')
        v = v.rstrip(';') if v else ''
        k = k.rstrip(';')
        cur[k] = re.sub(r'\s+', ' ', v.replace('< ', '<').replace(' >', '>'))
        if k in ('phandle', 'linux,phandle'):
            ph[int(v.strip('<>; '), 16)] = '/' + '/'.join(stack[1:])
    return nodes, ph


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dtc', default='dtc')
    ap.add_argument('stock')
    ap.add_argument('new')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        try:
            sd, nd = (load_dtb(f, a.dtc, tmp) for f in (a.stock, a.new))
            (sn, sph), (nn, nph) = canon(sd, a.dtc, tmp), canon(nd, a.dtc, tmp)
        except (subprocess.CalledProcessError, OSError, sb.Error) as e:
            print('error: %s' % e, file=sys.stderr)
            return 2
    # -@ builds (the kernel makefile) add a label table; it is not part of
    # the stock tree and carries no data the kernel consumes
    for n in (sn, nn):
        for k in [k for k in n if k == '/__symbols__']:
            del n[k]
    # a phandle-valued cell is only comparable through the node it names, so
    # first make sure the phandle -> node maps agree, then compare the text
    diffs = []
    if {k: v for k, v in sph.items()} != {k: v for k, v in nph.items()}:
        s_by, n_by = {v: k for k, v in sph.items()}, {v: k for k, v in nph.items()}
        for path in sorted(set(s_by) | set(n_by)):
            if s_by.get(path) != n_by.get(path):
                diffs.append('phandle %s: stock %s new %s' %
                             (path, s_by.get(path), n_by.get(path)))
    for path in sorted(set(sn) | set(nn)):
        if path not in nn:
            diffs.append('node only in stock: %s' % path)
        elif path not in sn:
            diffs.append('node only in new: %s' % path)
        else:
            for k in sorted(set(sn[path]) | set(nn[path])):
                if sn[path].get(k) != nn[path].get(k):
                    diffs.append('%s %s: %r != %r' % (
                        path, k, sn[path].get(k), nn[path].get(k)))
    same_bytes = sd == nd
    print('nodes %d/%d, phandles %d/%d, byte-identical DTB: %s' %
          (len(sn), len(nn), len(sph), len(nph), 'yes' if same_bytes else 'no'))
    if diffs:
        print('DIFFERENT (%d):' % len(diffs))
        for d in diffs[:200]:
            print('  ' + d)
        return 1
    print('IDENTICAL')
    return 0


if __name__ == '__main__':
    sys.exit(main())
