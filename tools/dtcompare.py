#!/usr/bin/env python3
"""Prove two device trees are equal, comparing the binary FDT structure.

usage: dtcompare.py [--dtc PATH] [--strict-order] STOCK NEW

STOCK / NEW may be a DTB, a DTS (compiled first with --dtc, `-b 0`) or an
Android boot image with a SPRD DT table (the first DTB is used).  The DTBs
are parsed here, from the binary: header fields (version, last_comp_version,
boot_cpuid_phys), the memory reservation map, and the structure block (node
names, property names and the raw value bytes of every property).  Nothing is
normalised: the phandle numbers are part of the property bytes, so the
phandle-to-node map must match exactly.

Reports BYTE-IDENTICAL (same file bytes), SAME STRUCTURE IN ORDER (content
and order equal, only string table / padding / offsets differ) or SAME
CONTENT, ORDER DIFFERS (node/property order differs; exit 1 with
--strict-order), or the list of differences.  A /__symbols__ node (added by
-@ builds) is ignored.  Exit 0 equal, 1 different, 2 usage / tool / parse
error.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
import sprd_bootimg as sb  # noqa: E402

FDT_MAGIC = 0xd00dfeed
BEGIN, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


class ParseError(Exception):
    pass


def load_dtb(path, dtc, tmp):
    data = open(path, 'rb').read()
    if data[:8] == sb.MAGIC:
        h, lay, _ = sb.parse(data)
        o, n = lay['dt']
        dt = data[o:o + n]
        t = sb.dt_entries(dt)
        if not t or not t[1]:
            raise ParseError('boot image has no SPRD DT table entry')
        eo, en = t[1][0][3], t[1][0][4]
        if eo + 8 > len(dt) or eo + en > len(dt):
            raise ParseError('DT table entry outside the table')
        total = struct.unpack_from('>I', dt, eo + 4)[0]
        if total > en:
            raise ParseError('DTB larger than its DT table entry')
        return dt[eo:eo + total]
    if data[:4] == b'\xd0\x0d\xfe\xed':
        return data
    out = os.path.join(tmp, 'c.dtb')
    if os.path.getsize(path) > MAX_INPUT:
        raise ParseError('input larger than %d bytes' % MAX_INPUT)
    subprocess.run([dtc, '-q', '-I', 'dts', '-O', 'dtb', '-b', '0', '-o', out,
                    path], check=True, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=60, preexec_fn=_limits)
    if os.path.getsize(out) > MAX_INPUT:
        raise ParseError('compiled DTB too large')
    return open(out, 'rb').read()


MAX_DEPTH = 64
MAX_PATH = 4096
MAX_NAME = 256
MAX_PROPS = 500000
MAX_INPUT = 64 << 20


def _limits():
    import resource
    resource.setrlimit(resource.RLIMIT_CPU, (60, 60))
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_INPUT, MAX_INPUT))


def parse_fdt(b):
    """-> (meta, memrsv list, nodes: list of (path, [(prop, bytes)]))."""
    if len(b) < 36:
        raise ParseError('DTB too short')
    (magic, total, off_st, off_str, off_rsv, ver, lcv,
     cpuid) = struct.unpack_from('>8I', b)
    if magic != FDT_MAGIC:
        raise ParseError('bad DTB magic')
    if ver not in (16, 17) or lcv > 16 or lcv > ver:
        raise ParseError('unsupported DTB version %d (last compatible %d)'
                         % (ver, lcv))
    hsz = 40 if ver >= 17 else 36
    if total > len(b) or total < hsz or len(b) < hsz:
        raise ParseError('bad DTB totalsize')
    sz_str = struct.unpack_from('>I', b, 32)[0]
    sz_st = struct.unpack_from('>I', b, 36)[0] if ver >= 17 else None
    if off_st % 4 or off_rsv % 8 or off_rsv < hsz or off_st < hsz or off_str < hsz:
        raise ParseError('misaligned or overlapping header in DTB layout')
    if off_str + sz_str > total or off_rsv > total or off_st > total:
        raise ParseError('DTB section outside the blob')
    if sz_st is not None and off_st + sz_st > total:
        raise ParseError('DTB section outside the blob')
    meta = {'version': ver, 'last_comp_version': lcv, 'boot_cpuid_phys': cpuid}
    rsv, o = [], off_rsv
    while True:
        if o + 16 > total:
            raise ParseError('unterminated memory reservation map')
        a, s = struct.unpack_from('>QQ', b, o)
        o += 16
        if a == 0 and s == 0:
            break
        rsv.append((a, s))
    rsv_end = o
    strs = b[off_str:off_str + sz_str]
    end = off_st + sz_st if sz_st is not None else total
    nodes, stack, o = [], [], off_st
    plen, roots, done = [0], 0, False
    names, nprops = {}, 0
    cur, haskid = [], []   # active node entries, 'had a subnode' flags
    while True:
        if o + 4 > end:
            raise ParseError('structure block ends without FDT_END')
        tag = struct.unpack_from('>I', b, o)[0]
        o += 4
        if tag == BEGIN:
            if done:
                raise ParseError('second root node')
            z = b.index(b'\0', o)
            if z >= end:
                raise ParseError('node name outside the structure block')
            name = b[o:z].decode('utf-8', 'surrogateescape')
            o = (z + 4) & ~3
            if not stack:
                if name != '':
                    raise ParseError('root node must have an empty name')
                roots += 1
            elif name == '' or '/' in name:
                raise ParseError('bad node name %r' % name[:40])
            if len(stack) >= MAX_DEPTH:
                raise ParseError('node nesting deeper than %d' % MAX_DEPTH)
            plen.append(plen[-1] + len(name) + 1)
            if plen[-1] > MAX_PATH:
                raise ParseError('node path longer than %d' % MAX_PATH)
            stack.append(name)
            if haskid:
                haskid[-1] = True
            haskid.append(False)
            cur.append((('/' + '/'.join(stack[1:])), []))
            nodes.append(cur[-1])
        elif tag == END_NODE:
            if not stack:
                raise ParseError('unbalanced FDT_END_NODE')
            stack.pop()
            plen.pop()
            cur.pop()
            haskid.pop()
            if not stack:
                done = True
        elif tag == PROP:
            if not stack:
                raise ParseError('property outside a node')
            if haskid[-1]:
                raise ParseError('property after a subnode in %s' % cur[-1][0])
            ln, nameoff = struct.unpack_from('>II', b, o)
            o += 8
            if o + ln > end or nameoff >= len(strs):
                raise ParseError('property outside the blob')
            pname = names.get(nameoff)
            if pname is None:
                z = strs.index(b'\0', nameoff)
                if z - nameoff > MAX_NAME:
                    raise ParseError('property name longer than %d' % MAX_NAME)
                pname = names[nameoff] = strs[nameoff:z].decode(
                    'utf-8', 'surrogateescape')
            nprops += 1
            if nprops > MAX_PROPS:
                raise ParseError('more than %d properties' % MAX_PROPS)
            cur[-1][1].append((pname, b[o:o + ln]))
            o = (o + ln + 3) & ~3
        elif tag == NOP:
            pass
        elif tag == END:
            if stack or roots != 1:
                raise ParseError('FDT_END without exactly one complete root')
            st_end = o
            if st_end > end:
                raise ParseError('structure block longer than declared')
            ranges = sorted([(off_rsv, rsv_end), (off_st, end if sz_st is not None else st_end),
                             (off_str, off_str + sz_str)])
            if any(ranges[i][1] > ranges[i + 1][0]
                   for i in range(len(ranges) - 1)):
                raise ParseError('overlapping DTB sections')
            return meta, rsv, nodes
        else:
            raise ParseError('unknown FDT tag 0x%x' % tag)


def strip_symbols(nodes):
    return [n for n in nodes if n[0] != '/__symbols__']


def show(v):
    h = v.hex()
    return h if len(h) <= 48 else h[:48] + '...(%d bytes)' % len(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dtc', default='dtc')
    ap.add_argument('--strict-order', action='store_true',
                    help='also fail when node/property order differs')
    ap.add_argument('stock')
    ap.add_argument('new')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        try:
            sd, nd = (load_dtb(f, a.dtc, tmp) for f in (a.stock, a.new))
            smeta, srs, sn = parse_fdt(sd)
            nmeta, nrs, nn = parse_fdt(nd)
        except (subprocess.SubprocessError, OSError, sb.Error, ParseError,
                ValueError, IndexError, struct.error) as e:
            print('error: %s: %s' % (type(e).__name__, e), file=sys.stderr)
            return 2
    sn, nn = strip_symbols(sn), strip_symbols(nn)
    diffs = []
    if srs != nrs:
        diffs.append('memreserve: %r != %r' % (srs, nrs))
    for k in smeta:
        if smeta[k] != nmeta[k]:
            diffs.append('header %s: %r != %r' % (k, smeta[k], nmeta[k]))
    sm, nm = {}, {}
    for m, nodes in ((sm, sn), (nm, nn)):
        for path, props in nodes:
            if path in m:
                diffs.append('duplicate node %s' % path)
            m[path] = props
    for path in sorted(set(sm) | set(nm)):
        if path not in nm:
            diffs.append('node only in stock: %s' % path)
        elif path not in sm:
            diffs.append('node only in new: %s' % path)
        else:
            sp, np_ = sm[path], nm[path]
            si, ni = {}, {}
            for x, v in sp:
                si.setdefault(x, []).append(v)
            for x, v in np_:
                ni.setdefault(x, []).append(v)
            for k in sorted(set(si) | set(ni)):
                sv, nv = si.get(k, []), ni.get(k, [])
                if sv != nv:
                    diffs.append('%s %s: %s != %s' % (
                        path, k, [show(v) for v in sv] or 'absent',
                        [show(v) for v in nv] or 'absent'))
    order_ok = ([p for p, _ in sn] == [p for p, _ in nn] and
                all([x for x, _ in sm[p]] == [x for x, _ in nm[p]]
                    for p in sm if p in nm))
    print('nodes %d/%d, properties %d/%d' % (
        len(sn), len(nn), sum(len(p) for _, p in sn), sum(len(p) for _, p in nn)))
    if diffs:
        print('DIFFERENT (%d):' % len(diffs))
        for d in diffs[:200]:
            print('  ' + d)
        return 1
    if sd == nd:
        print('BYTE-IDENTICAL')
    elif order_ok:
        print('SAME STRUCTURE IN ORDER (bytes differ only in layout)')
    else:
        print('SAME CONTENT, ORDER DIFFERS')
        if a.strict_order:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
