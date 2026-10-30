#!/usr/bin/env python3
"""Prove two device trees are equivalent.

usage: dtcompare.py [--dtc PATH] [--strict-order] STOCK NEW

STOCK / NEW may be a DTB, a DTS or an Android boot image with a SPRD DT
table (the first DTB of the table is used).  Both are decompiled with dtc and
compared strictly: every node, every property (values compared as raw dtc
text), the /memreserve/ entries, the DTB header fields (version,
last_comp_version, boot_cpuid_phys) and the phandle number of every node
(the phandle-to-node map must match exactly).  Node and property order is
ignored by default and reported separately; with --strict-order a difference
in order is a failure too.  A /__symbols__ node (added by -@ builds) is
ignored.  Exit 0 if equal, 1 if different, 2 on usage / tool / parse errors.
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
    txt = subprocess.run([dtc, '-q', '-I', 'dtb', '-O', 'dts', p],
                         check=True, capture_output=True, text=True).stdout
    nodes, stack, ph, rsv = {}, [], {}, []
    hdr = struct.unpack_from('>10I', dtb)
    meta = {'version': hdr[5], 'last_comp_version': hdr[6],
            'boot_cpuid_phys': hdr[7]}
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
        m = re.match(r'^/memreserve/\s+(\S+)\s+(\S+);$', s)
        if m:
            rsv.append((int(m.group(1), 0), int(m.group(2), 0)))
            continue
        if not stack:
            raise ValueError('unparsed line outside a node: %r' % s[:60])
        cur = nodes['/' + '/'.join(stack[1:])]
        k, _, v = s.partition(' = ')
        v = v.rstrip(';') if v else ''
        k = k.rstrip(';')
        cur[k] = re.sub(r'\s+', ' ', v.replace('< ', '<').replace(' >', '>'))
        if k in ('phandle', 'linux,phandle'):
            ph[int(v.strip('<>; '), 16)] = '/' + '/'.join(stack[1:])
    return nodes, ph, rsv, meta


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dtc', default='dtc')
    ap.add_argument('--strict-order', action='store_true')
    ap.add_argument('stock')
    ap.add_argument('new')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        try:
            sd, nd = (load_dtb(f, a.dtc, tmp) for f in (a.stock, a.new))
            (sn, sph, srs, smeta) = canon(sd, a.dtc, tmp)
            (nn, nph, nrs, nmeta) = canon(nd, a.dtc, tmp)
        except (subprocess.CalledProcessError, OSError, sb.Error, ValueError,
                KeyError, IndexError, struct.error) as e:
            print('error: %s: %s' % (type(e).__name__, e), file=sys.stderr)
            return 2
    # -@ builds (the kernel makefile) add a label table; it is not part of
    # the stock tree and carries no data the kernel consumes
    for n in (sn, nn):
        for k in [k for k in n if k == '/__symbols__']:
            del n[k]
    # strict: the phandle number of every node must be the same
    order_ok = (list(sn) == list(nn) and
                all(list(sn[k]) == list(nn[k]) for k in sn if k in nn))
    diffs = []
    if srs != nrs:
        diffs.append('memreserve: %r != %r' % (srs, nrs))
    for k in smeta:
        if smeta[k] != nmeta[k]:
            diffs.append('header %s: %r != %r' % (k, smeta[k], nmeta[k]))
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
    if not diffs and a.strict_order and not order_ok:
        diffs.append('node/property order differs (content equal)')
    print('nodes %d/%d, phandles %d/%d, byte-identical DTB: %s' %
          (len(sn), len(nn), len(sph), len(nph), 'yes' if same_bytes else 'no'))
    if diffs:
        print('DIFFERENT (%d):' % len(diffs))
        for d in diffs[:200]:
            print('  ' + d)
        return 1
    print('BYTE-IDENTICAL' if same_bytes else
          'IDENTICAL (order ignored)')
    if not same_bytes and order_ok:
        print('node/property order also equal')
    return 0


if __name__ == '__main__':
    sys.exit(main())
