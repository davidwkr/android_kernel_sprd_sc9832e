#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# sprd_bootimg.py - inspect, unpack and repack UNISOC (SPRD) Android boot
# images: header v0 with a SPRD DT table after the pages.
#
#   Copyright (C) 2026 the kwt kernel maintainers
#
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License version 2 as
#   published by the Free Software Foundation.
#
# Layout handled (all sections padded to page_size with zeros):
#
#   page 0   header (ANDROID!, 10 words, name, cmdline, id, extra_cmdline)
#            word 9 (after page_size) is dt_size, as in AOSP mkbootimg v0
#   kernel | ramdisk | second | dt table (SPRD)
#   zero fill
#   AVB vbmeta blob at align(image_end, 4096), AVB footer in the last 64
#   bytes of the partition
#
# dt_size and the id recipe below follow the vendor (CAF-style) mkbootimg v0:
# the id is SHA-1 over kernel, ramdisk, second (each followed by its size as
# le32) and, when dt_size is non-zero, the dt table plus its size.
# The recipe is detected at unpack and stored in header.json.
#
# dt-bootargs additionally runs fdtget, fdtput and dtc (device-tree-compiler).
#
# dt-bootargs OLD NEW: see cmd_dt_bootargs.
#
# Only the python3 standard library is used. Input images are treated as
# untrusted data: they are parsed, never executed. Run with "python3 -I".

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b'ANDROID!'
HDR = struct.Struct('<8s10I16s512s32s1024s')
HDR_SIZE = HDR.size                      # 1632
DEFAULT_PAD_TO = 36700160
AVB_ALIGN = 4096
AVB_FOOTER = struct.Struct('>4sIIQQQ28s')   # 64 bytes
AVB_FOOTER_MAGIC = b'AVBf'
DT_MAGIC = b'SPRD'
DT_ENTRY = struct.Struct('<8I')
FDT_MAGIC = 0xd00dfeed

FILES = {'kernel': 'kernel', 'ramdisk': 'ramdisk', 'second': 'second',
         'dt': 'dt', 'vbmeta': 'vbmeta.bin', 'extra': 'extra.bin'}


class Error(Exception):
    pass


def align(n, a):
    return (n + a - 1) // a * a


def read_file(path):
    with open(path, 'rb') as f:
        return f.read()


def write_file(path, data):
    with open(path, 'wb') as f:
        f.write(data)


# ---------------------------------------------------------------- header --

def text_or_hex(raw):
    """NUL-padded ASCII as str, otherwise 'hex:...' (lossless either way)."""
    body = raw.rstrip(b'\0')
    if (b'\0' not in body and all(32 <= c < 127 for c in body)
            and not body.startswith(b'hex:')):
        return body.decode('ascii')
    return 'hex:' + raw.hex()


def field_bytes(val, size):
    if val.startswith('hex:'):
        raw = bytes.fromhex(val[4:])
    else:
        raw = val.encode('ascii')
    if len(raw) > size:
        raise Error('field too long')
    return raw.ljust(size, b'\0')


def os_version_str(v):
    ver, patch = v >> 11, v & 0x7ff
    a, b, c = ver >> 14, (ver >> 7) & 0x7f, ver & 0x7f
    return '%d.%d.%d, patch %04d-%02d' % (a, b, c, 2000 + (patch >> 4),
                                           patch & 0xf)


def parse(img):
    if len(img) < HDR_SIZE or img[:8] != MAGIC:
        raise Error('not an Android boot image (bad magic)')
    (_, ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page, dtsz, osv,
     name, cmd, bid, xcmd) = HDR.unpack_from(img, 0)
    if page < 2048 or page & (page - 1):
        raise Error('implausible page size %d' % page)
    h = dict(kernel_size=ksz, kernel_addr=kaddr, ramdisk_size=rsz,
             ramdisk_addr=raddr, second_size=ssz, second_addr=saddr,
             tags_addr=taddr, page_size=page, dt_size=dtsz, os_version=osv,
             name=text_or_hex(name), cmdline=text_or_hex(cmd),
             extra_cmdline=text_or_hex(xcmd), id_raw=bid.hex())
    off = page
    layout = {}
    for key in ('kernel', 'ramdisk', 'second', 'dt'):
        size = h['dt_size' if key == 'dt' else key + '_size']
        layout[key] = (off, size)
        off += align(size, page)
    if off > len(img):
        raise Error('section sizes exceed the file size')
    return h, layout, off


def id_digest(recipe, k, r, s, dt):
    algo, _, with_dt = recipe.partition('+')
    m = hashlib.new(algo)
    for b in (k, r, s):
        m.update(b)
        m.update(struct.pack('<I', len(b)))
    if with_dt:
        m.update(dt)
        m.update(struct.pack('<I', len(dt)))
    return m.digest().ljust(32, b'\0')


RECIPES = ('sha1+dt', 'sha1', 'sha256+dt', 'sha256')


def detect_recipe(bid, k, r, s, dt):
    for rec in RECIPES:
        if rec.endswith('+dt') and not dt:
            continue
        if id_digest(rec, k, r, s, dt) == bid:
            return rec
    return None


# ----------------------------------------------------------------- AVB ---

def find_avb(img, end):
    """Return (vbmeta_off, vbmeta_size, original_size) or None."""
    if len(img) < 64:
        return None
    f = AVB_FOOTER.unpack(img[-64:])
    if f[0] != AVB_FOOTER_MAGIC:
        return None
    orig, voff, vsz = f[3], f[4], f[5]
    if orig > voff or voff < end or voff + vsz > len(img) - 64 or img[voff:voff + 4] != b'AVB0':
        return None
    return voff, vsz, orig


def avb_hash_desc(vb):
    """First hash descriptor of a vbmeta blob, or None."""
    if len(vb) < 256 or vb[:4] != b'AVB0':
        return None
    auth, aux = struct.unpack_from('>QQ', vb, 12)
    doff, dsz = struct.unpack_from('>QQ', vb, 96)
    p = 256 + auth + doff
    end = p + dsz
    if end > len(vb):
        return None
    while p + 16 <= end:
        tag, n = struct.unpack_from('>QQ', vb, p)
        if tag == 2 and n >= 116:
            isz = struct.unpack_from('>Q', vb, p + 16)[0]
            algo = vb[p + 24:p + 56].rstrip(b'\0').decode('ascii', 'replace')
            nl, sl, dl = struct.unpack_from('>III', vb, p + 56)
            q = p + 132
            if q + nl + sl + dl > end:
                return None
            return dict(image_size=isz, algo=algo,
                        name=vb[q:q + nl].decode('ascii', 'replace'),
                        salt=vb[q + nl:q + nl + sl],
                        digest=vb[q + nl + sl:q + nl + sl + dl])
        p += 16 + align(n, 8)
    return None


def avb_hash_state(vb, image, orig):
    """Check the hash descriptor over its own image_size.

    Returns (state, descriptor, note); state is 'ok', 'stale' or 'unknown'.
    A descriptor size that differs from the footer's original_image_size, or
    that lies beyond the image, is reported in note and counts as stale.
    """
    d = avb_hash_desc(vb)
    if not d:
        return 'unknown', None, 'no hash descriptor'
    note = ''
    if d['image_size'] != orig:
        note = ('descriptor image_size %d != footer original_image_size %d'
                % (d['image_size'], orig))
    if d['image_size'] > len(image):
        return 'stale', d, note or 'descriptor image_size out of bounds'
    try:
        m = hashlib.new(d['algo'])
    except ValueError:
        return 'unknown', d, 'unsupported hash algorithm ' + d['algo']
    m.update(d['salt'])
    m.update(memoryview(image)[:d['image_size']])
    return ('ok' if m.digest() == d['digest'] and not note else 'stale'), \
        d, note


def nz_bounds(img, lo, hi, skip):
    """First and last+1 non-zero offsets in img[lo:hi] outside the skip
    ranges (sorted list of (start, end)), or None. O(1) extra memory."""
    mv = memoryview(img)
    first = last = None
    pos, segs = lo, []
    for s0, e0 in skip + [(hi, hi)]:
        if s0 > pos:
            segs.append((pos, s0))
        pos = max(pos, e0)
    for s0, e0 in segs:
        for c in range(s0, e0, 1 << 20):
            ce = min(c + (1 << 20), e0)
            chunk = bytes(mv[c:ce])
            if chunk.count(0) == len(chunk):
                continue
            if first is None:
                first = c + len(chunk) - len(chunk.lstrip(b'\0'))
            last = c + len(chunk.rstrip(b'\0'))
    return None if first is None else (first, last)


# ------------------------------------------------------------ DT / FDT ---

def fdt_root_props(b):
    """Minimal FDT walk: return (totalsize, {prop: bytes}) of the root node."""
    if len(b) < 40 or struct.unpack_from('>I', b, 0)[0] != FDT_MAGIC:
        raise Error('not a DTB')
    total, off_struct, off_str = struct.unpack_from('>III', b, 4)
    if total > len(b) or off_struct >= total or off_str >= total:
        raise Error('bad DTB header')
    props, depth, p = {}, 0, off_struct
    while p + 4 <= total:
        tok = struct.unpack_from('>I', b, p)[0]
        p += 4
        if tok == 1:                                  # BEGIN_NODE
            depth += 1
            p = align(b.index(b'\0', p) + 1, 4)
        elif tok == 2:                                # END_NODE
            depth -= 1
            if depth == 0:
                break
        elif tok == 3:                                # PROP
            ln, nameoff = struct.unpack_from('>II', b, p)
            p += 8
            if depth == 1:
                s = off_str + nameoff
                props[b[s:b.index(b'\0', s)].decode('ascii', 'replace')] = \
                    b[p:p + ln]
            p = align(p + ln, 4)
        elif tok == 4:
            pass
        else:
            break
    return total, props


def dt_entries(dt):
    if dt[:4] != DT_MAGIC:
        return None
    if len(dt) < 12:
        raise Error('DT table too short')
    ver, cnt = struct.unpack_from('<II', dt, 4)
    if cnt > 256 or 12 + cnt * DT_ENTRY.size > len(dt):
        raise Error('bad DT table header')
    out = []
    for i in range(cnt):
        e = DT_ENTRY.unpack_from(dt, 12 + i * DT_ENTRY.size)
        out.append(e)
    return ver, out


def strs(v):
    return [x.decode('ascii', 'replace') for x in v.split(b'\0') if x]


# ----------------------------------------------------------------- info ---

def cmd_info(a):
    img = read_file(a.img)
    h, lay, end = parse(img)
    k, r, s, dt = (img[o:o + n] for o, n in (lay[x] for x in
                   ('kernel', 'ramdisk', 'second', 'dt')))
    print('file size      %d (0x%x)' % (len(img), len(img)))
    print('page size      %d' % h['page_size'])
    for key in ('kernel', 'ramdisk', 'second'):
        print('%-8s size %-9d addr 0x%08x  offset 0x%x' %
              (key, h[key + '_size'], h[key + '_addr'], lay[key][0]))
    print('dt       size %-9d offset 0x%x' % (h['dt_size'], lay['dt'][0]))
    print('tags addr      0x%x' % h['tags_addr'])
    print('os_version     0x%08x (%s)' % (h['os_version'],
                                          os_version_str(h['os_version'])))
    print('name           %r' % h['name'])
    print('cmdline        %r' % h['cmdline'])
    if h['extra_cmdline']:
        print('extra_cmdline  %r' % h['extra_cmdline'])
    if len(k) >= 64 and k[56:60] == b'ARM\x64':
        toff, isz = struct.unpack_from('<QQ', k, 8)
        print('kernel         arm64 Image, text_offset 0x%x, image_size %d' %
              (toff, isz))
    else:
        print('kernel         not an uncompressed arm64 Image')
    # DT table
    t = dt_entries(dt) if dt else None
    if t:
        ver, ents = t
        print('dt table       SPRD version %d, %d DTB' % (ver, len(ents)))
        for i, e in enumerate(ents):
            o, n = e[3], e[4]
            try:
                tot, p = fdt_root_props(dt[o:o + n])
                model = strs(p.get('model', b''))
                comp = strs(p.get('compatible', b''))
            except (Error, ValueError, struct.error) as ex:
                tot, model, comp = 0, ['?'], [str(ex)]
            print('  [%d] id 0x%x rev %d, table off 0x%x size %d (dtb %d) '
                  'model %s compatible %s' %
                  (i, e[0], e[1], o, n, tot, model[:1], comp))
    elif dt:
        print('dt table       present but not SPRD (magic %r)' % dt[:4])
    # trailing area
    tail = img[end:]
    nz = len(tail) - tail.count(0)
    print('trailing       %d bytes after the last page, %d non-zero' %
          (len(tail), nz))
    avb = find_avb(img, end)
    if avb:
        print('avb            vbmeta at 0x%x size %d, footer original size %d '
              '(image end 0x%x)' % (avb[0], avb[1], avb[2], end))
        st, d, note = avb_hash_state(img[avb[0]:avb[0] + avb[1]], img, avb[2])
        print('avb hash       %s (partition %r, %s)%s' %
              (st, d['name'] if d else '?', d['algo'] if d else '?',
               ', ' + note if note else ''))
    # id
    bid = bytes.fromhex(h['id_raw'])
    rec = detect_recipe(bid, k, r, s, dt)
    print('id             %s' % bid.rstrip(b'\0').hex())
    if rec:
        print('id check       OK, recipe %s' % rec)
    else:
        print('id check       MISMATCH for sha1/sha256, with and without dt')
    return 0


# --------------------------------------------------------------- unpack ---

def cmd_unpack(a):
    img = read_file(a.img)
    h, lay, end = parse(img)
    parts = {key: img[o:o + n] for key, (o, n) in lay.items()}
    page = h['page_size']
    # padding must be zero, otherwise a rebuild would not be exact
    zero_ok = all(img[o + n:o + align(n, page)].count(0) ==
                  align(n, page) - n for o, n in lay.values())
    zero_ok = zero_ok and img[HDR_SIZE:page].count(0) == page - HDR_SIZE
    if not zero_ok:
        raise Error('non-zero page padding: cannot rebuild exactly')
    os.makedirs(a.dir, exist_ok=True)
    managed = [os.path.join(a.dir, n) for n in
               list(FILES.values()) + ['header.json']]
    if os.listdir(a.dir) and not a.force:
        raise Error('%s is not empty (use --force to replace the managed '
                    'files)' % a.dir)
    for m in managed:
        if os.path.exists(m):
            os.remove(m)
    for key, data in parts.items():
        if data:
            write_file(os.path.join(a.dir, FILES[key]), data)
    bid = bytes.fromhex(h['id_raw'])
    h['id_recipe'] = detect_recipe(bid, parts['kernel'], parts['ramdisk'],
                                   parts['second'], parts['dt']) or 'raw'
    if h['id_recipe'] != 'raw':
        del h['id_raw']
    h['image_size'] = len(img)
    h['image_end'] = end
    # AVB blob + footer, then whatever else is non-zero after the image
    avb = find_avb(img, end)
    skip = []
    if avb:
        voff, vsz, orig = avb
        write_file(os.path.join(a.dir, FILES['vbmeta']), img[voff:voff + vsz])
        fv = AVB_FOOTER.unpack(img[-64:])
        h['avb'] = dict(major=fv[1], minor=fv[2],
                        vbmeta_offset=voff, vbmeta_size=vsz,
                        original_image_size=orig,
                        footer_tail=img[-28:].hex())
        skip = [(voff, voff + vsz), (len(img) - 64, len(img))]
    nz = nz_bounds(img, end, len(img), skip)
    if nz:
        write_file(os.path.join(a.dir, FILES['extra']), img[nz[0]:nz[1]])
        h['extra_offset'] = nz[0]
    with open(os.path.join(a.dir, 'header.json'), 'w') as f:
        json.dump(h, f, indent=2)
        f.write('\n')
    print('unpacked %s -> %s (id recipe %s%s)' %
          (os.path.basename(a.img), a.dir, h['id_recipe'],
           ', avb' if avb else ''))
    return 0


# ----------------------------------------------------------------- pack ---

HDR_INT = ('kernel_size', 'kernel_addr', 'ramdisk_size', 'ramdisk_addr',
           'second_size', 'second_addr', 'tags_addr', 'page_size', 'dt_size',
           'os_version', 'image_size', 'image_end')
HDR_STR = ('name', 'cmdline', 'extra_cmdline', 'id_recipe')
AVB_INT = ('major', 'minor', 'vbmeta_offset', 'vbmeta_size',
           'original_image_size')


def check_header(h, d):
    """Refuse an incomplete or inconsistent header.json instead of
    filling in defaults (a silent default changes the image)."""
    if not isinstance(h, dict):
        raise Error('header.json: not an object')
    for k in HDR_INT:
        if not isinstance(h.get(k), int) or isinstance(h.get(k), bool):
            raise Error('header.json: missing or bad %s' % k)
    for k in HDR_STR:
        if not isinstance(h.get(k), str):
            raise Error('header.json: missing or bad %s' % k)
    if 'extra_offset' in h and not isinstance(h['extra_offset'], int):
        raise Error('header.json: bad extra_offset')
    pg = h['page_size']
    if pg < 2048 or pg & (pg - 1):
        raise Error('header.json: implausible page_size %d' % pg)
    if h['id_recipe'] == 'raw':
        try:
            raw = bytes.fromhex(h.get('id_raw', ''))
        except ValueError:
            raise Error('header.json: bad id_raw')
        if len(raw) != 32:
            raise Error('header.json: id_raw must be 32 bytes')
    vb = os.path.exists(os.path.join(d, FILES['vbmeta']))
    avb = h.get('avb')
    if avb is None:
        if vb:
            raise Error('header.json has no avb section but %s exists; '
                        'unpack again' % FILES['vbmeta'])
        return
    if not isinstance(avb, dict):
        raise Error('header.json: bad avb section')
    for k in AVB_INT:
        if not isinstance(avb.get(k), int):
            raise Error('header.json: missing or bad avb.%s' % k)
    try:
        tail = bytes.fromhex(avb.get('footer_tail'))
    except (TypeError, ValueError):
        raise Error('header.json: missing or bad avb.footer_tail')
    if len(tail) != 28:
        raise Error('header.json: avb.footer_tail must be 28 bytes')
    if not vb:
        raise Error('header.json has an avb section but %s is missing; '
                    'unpack again' % FILES['vbmeta'])


def cmd_pack(a):
    with open(os.path.join(a.dir, 'header.json')) as f:
        h = json.load(f)
    check_header(h, a.dir)

    def part(key, override):
        p = override or os.path.join(a.dir, FILES[key])
        if os.path.exists(p):
            return read_file(p)
        if override:
            raise Error('missing file %s' % p)
        saved = h['dt_size' if key == 'dt' else key + '_size']
        if saved:
            raise Error('missing %s (header.json says %d bytes); unpack again '
                        'or pass it explicitly' % (p, saved))
        return b''
    k = part('kernel', a.kernel)
    if not k:
        raise Error('empty kernel')
    r = part('ramdisk', a.ramdisk)
    s = part('second', None)
    dt = part('dt', a.dt)
    changed = bool(a.kernel or a.ramdisk or a.dt)
    page = h['page_size']

    if len(k) >= 64 and k[56:60] == b'ARM\x64':
        toff, isz = struct.unpack_from('<QQ', k, 8)
        base = h['kernel_addr'] + toff
        top = base + max(isz, len(k))
        if top > h['ramdisk_addr']:
            raise Error('kernel (text_offset 0x%x, runtime size %d, file '
                        'size %d) ends at 0x%x, past the ramdisk address '
                        '0x%x. Note: the header addresses may not be the '
                        'real SPRD U-Boot load addresses.' %
                        (toff, isz, len(k), top, h['ramdisk_addr']))
    elif a.kernel:
        print('warning: --kernel is not an uncompressed arm64 Image',
              file=sys.stderr)
    if dt and dt[:4] != DT_MAGIC:
        print('warning: dt table has no SPRD magic', file=sys.stderr)

    rec = h['id_recipe']
    if rec == 'raw':
        bid = bytes.fromhex(h['id_raw'])
        if changed:
            print('warning: stock id recipe unknown, keeping the old id',
                  file=sys.stderr)
    else:
        bid = id_digest(rec, k, r, s, dt)
    hdr = HDR.pack(MAGIC, len(k), h['kernel_addr'], len(r), h['ramdisk_addr'],
                   len(s), h['second_addr'], h['tags_addr'], page, len(dt),
                   h['os_version'], field_bytes(h['name'], 16),
                   field_bytes(h['cmdline'], 512), bid,
                   field_bytes(h['extra_cmdline'], 1024))
    out = bytearray(hdr.ljust(page, b'\0'))
    for b in (k, r, s, dt):
        out += b.ljust(align(len(b), page), b'\0')
    end = len(out)

    pad_to = a.partition_size or h['image_size']
    avb = h.get('avb')
    if avb and not a.no_avb and not a.partition_size:
        print('warning: no --partition-size, assuming the stock size %d. '
              'The AVB footer goes at the end of the partition: for any '
              'partition other than the stock boot one (e.g. recovery) '
              'read its real size and pass --partition-size.' % pad_to,
              file=sys.stderr)
    extra_p = os.path.join(a.dir, FILES['extra'])
    out += bytes(max(0, pad_to - end))
    if len(out) > pad_to:
        raise Error('image needs %d bytes, partition is %d' % (end, pad_to))
    if os.path.exists(extra_p):
        e = read_file(extra_p)
        off = h['extra_offset']
        limit = pad_to - 64 if (avb and not a.no_avb) else pad_to
        if off + len(e) > limit or off < end:
            raise Error('extra trailing data does not fit (it may not '
                        'overlap the image or the AVB footer)')
        out[off:off + len(e)] = e
    elif 'extra_offset' in h:
        raise Error('missing %s (header.json records trailing data at 0x%x); '
                    'unpack again' % (extra_p, h['extra_offset']))
    if avb and not a.no_avb:
        vb = read_file(os.path.join(a.dir, FILES['vbmeta']))
        if end == h['image_end']:
            voff = avb['vbmeta_offset']
        else:
            voff = align(end, AVB_ALIGN)
        if voff < end or voff % AVB_ALIGN:
            raise Error('vbmeta offset 0x%x overlaps the image (end 0x%x) or '
                        'is not %d-aligned' % (voff, end, AVB_ALIGN))
        if voff + len(vb) > pad_to - 64:
            raise Error('no room for the AVB vbmeta blob')
        if os.path.exists(extra_p):
            eo, el = h['extra_offset'], len(read_file(extra_p))
            if voff < eo + el and eo < voff + len(vb):
                raise Error('vbmeta blob overlaps the saved trailing data')
        out[voff:voff + len(vb)] = vb
        out[-64:] = AVB_FOOTER.pack(
            AVB_FOOTER_MAGIC, avb['major'], avb['minor'], end, voff,
            len(vb),
            bytes.fromhex(avb['footer_tail']))
        st, d, note = avb_hash_state(vb, out, end)
        if st != 'ok':
            print('warning: AVB hash descriptor (partition %r) %s for this '
                  'image; only valid on an unlocked (orange) device. For a '
                  'recovery test, reuse the stock recovery vbmeta.bin.' %
                  (d['name'] if d else '?', st + (' (%s)' % note if note else '')),
                  file=sys.stderr)
    # self-check: the result must parse back to exactly what was asked for
    ph, lay, pend = parse(bytes(out))
    for f in ('kernel_addr', 'ramdisk_addr', 'second_addr', 'tags_addr',
              'page_size', 'os_version'):
        if ph[f] != h[f]:
            raise Error('self-check: %s mismatch' % f)
    for key, data in (('kernel', k), ('ramdisk', r), ('second', s),
                      ('dt', dt)):
        o, n = lay[key]
        if n != len(data) or out[o:o + n] != data:
            raise Error('self-check: %s section mismatch' % key)
    if pend != end:
        raise Error('self-check: image end mismatch')
    if bytes(out[:HDR_SIZE]) != hdr[:HDR_SIZE]:
        raise Error('self-check: header bytes mismatch')
    if os.path.exists(extra_p):
        if out[off:off + len(e)] != e:
            raise Error('self-check: trailing data mismatch')
    if avb and not a.no_avb:
        fa = find_avb(bytes(out), end)
        if not fa or fa[0] != voff or fa[1] != len(vb) or fa[2] != end:
            raise Error('self-check: AVB footer does not describe the image')
    write_file(a.out, bytes(out))
    print('packed %s: kernel %d ramdisk %d dt %d, image end 0x%x, total %d, '
          'id %s' % (a.out, len(k), len(r), len(dt), end, len(out),
                     bid.rstrip(b'\0').hex()[:16]))
    return 0


# ------------------------------------------------------------- dt-table ---

def cmd_dt_table(a):
    """Rebuild a one-DTB SPRD DT table around a new DTB, keeping the stock
    header and entry fields (board id, revision, flags) so the bootloader
    selects it the same way; only the DTB, its padded size and the table
    size change."""
    old = read_file(a.stock_dt)
    dtb = read_file(a.dtb)
    t = dt_entries(old)
    if not t:
        raise Error('%s is not a SPRD DT table' % a.stock_dt)
    ver, ents = t
    if len(ents) != 1:
        raise Error('stock table has %d DTBs, only 1 is supported' % len(ents))
    if len(dtb) < 8 or struct.unpack_from('>I', dtb, 0)[0] != FDT_MAGIC:
        raise Error('%s is not a flattened device tree' % a.dtb)
    e = list(ents[0])
    off = e[3]
    if off < 12 + DT_ENTRY.size or off % 0x800:
        raise Error('unexpected stock DTB offset 0x%x' % off)
    e[4] = align(len(dtb), 0x800)
    out = bytearray(off + e[4])
    out[:off] = old[:off]
    DT_ENTRY.pack_into(out, 12, *e)
    out[off:off + len(dtb)] = dtb
    write_file(a.out, bytes(out))
    print('dt table %s: id 0x%x rev %d, dtb %d bytes at 0x%x, table %d' %
          (a.out, e[0], e[1], len(dtb), off, len(out)))
    return 0


# --------------------------------------------------------- dt-bootargs ---

def fdt_tool(name):
    path = shutil.which(name)
    if not path:
        raise Error('%s (device-tree-compiler) not found in PATH' % name)
    return path


def run_fdt(argv):
    r = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                       env={'PATH': os.environ.get('PATH', '/usr/bin')})
    if r.returncode:
        raise Error('%s failed: %s' % (os.path.basename(argv[0]),
                                       r.stderr.decode('utf-8', 'replace')
                                       .strip()))
    return r.stdout


def cmd_dt_bootargs(a):
    """Replace a substring of /chosen/bootargs in every DTB of a SPRD DT
    table with fdtput on the extracted blobs. The table header, the entry
    fields (ids, revisions, offsets, slot sizes) and the zero padding stay;
    a DTB that no longer fits its slot is refused. Each DTB must hold the
    substring exactly once."""
    if not a.old:
        raise Error('empty --replace string')
    dt = read_file(a.dt)
    t = dt_entries(dt)
    if not t:
        raise Error('%s is not a SPRD DT table' % a.dt)
    ver, ents = t
    fdtget, fdtput, dtc = (fdt_tool(x) for x in ('fdtget', 'fdtput', 'dtc'))
    out = bytearray(dt)
    done = []
    with tempfile.TemporaryDirectory(prefix='sprd_bootimg.') as tmp:
        for i, e in enumerate(ents):
            off, slot = e[3], e[4]
            if off < 12 + len(ents) * DT_ENTRY.size or \
                    off + slot > len(dt):
                raise Error('DTB %d: bad offset or size' % i)
            blob = dt[off:off + slot]
            total, _ = fdt_root_props(blob)
            if total > slot:
                raise Error('DTB %d: totalsize over its slot' % i)
            if any(blob[total:]):
                raise Error('DTB %d: non-zero data after the blob, cannot '
                            'keep the padding' % i)
            src = os.path.join(tmp, 'in%d.dtb' % i)
            dst = os.path.join(tmp, 'out%d.dtb' % i)
            with open(src, 'wb') as f:
                f.write(blob[:total])
            cur = run_fdt([fdtget, '-t', 's', src, '/chosen', 'bootargs'])
            cur = cur.decode('utf-8', 'replace').rstrip('\n')
            n = cur.count(a.old)
            if n != 1:
                raise Error('DTB %d: %r found %d times in the bootargs, '
                            'need exactly once' % (i, a.old, n))
            new = cur.replace(a.old, a.new)
            shutil.copyfile(src, dst)
            run_fdt([fdtput, '-t', 's', dst, '/chosen', 'bootargs', new])
            nb = read_file(dst)
            ntotal, _ = fdt_root_props(nb)
            if ntotal != len(nb) or ntotal > slot:
                raise Error('DTB %d: edited blob (%d bytes) does not fit '
                            'its %d byte slot' % (i, len(nb), slot))
            # only the bootargs property may differ
            ts = [run_fdt([dtc, '-q', '-I', 'dtb', '-O', 'dts', p])
                  .decode('utf-8', 'replace').splitlines()
                  for p in (src, dst)]
            diff = [(x, y) for x, y in zip(*ts) if x != y]
            if len(ts[0]) != len(ts[1]) or len(diff) != 1 or \
                    'bootargs' not in diff[0][0]:
                raise Error('DTB %d: the edit changed more than bootargs' % i)
            if run_fdt([fdtget, '-t', 's', dst, '/chosen', 'bootargs']) \
                    .decode('utf-8', 'replace').rstrip('\n') != new:
                raise Error('DTB %d: bootargs readback mismatch' % i)
            out[off:off + slot] = nb.ljust(slot, b'\0')
            done.append((i, cur, new))
    assert len(out) == len(dt)
    write_file(a.out, bytes(out))
    for i, cur, new in done:
        print('DTB %d bootargs: %s -> %s' % (i, cur, new))
    print('dt table %s: %d DTB(s), layout and ids unchanged' %
          (a.out, len(done)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]
                                 if __doc__ else None)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info', help='show header, DT table, id check')
    p.add_argument('img')
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser('unpack', help='split an image into files')
    p.add_argument('img')
    p.add_argument('dir')
    p.add_argument('--force', action='store_true',
                   help='unpack into a non-empty dir, deleting old parts')
    p.set_defaults(fn=cmd_unpack)
    p = sub.add_parser('pack', help='rebuild an image from an unpacked dir')
    p.add_argument('dir')
    p.add_argument('out')
    p.add_argument('--kernel', help='replacement kernel Image')
    p.add_argument('--ramdisk', help='replacement ramdisk')
    p.add_argument('--dt', help='replacement SPRD DT table')
    p.add_argument('--partition-size', '--pad-to', dest='partition_size',
                   type=int, default=0,
                   help='target partition size in bytes; zero fill up to it '
                        'and put the AVB footer at its end (default: stock '
                        'boot size, normally %d; pass it explicitly for '
                        'recovery)' % DEFAULT_PAD_TO)
    p.add_argument('--no-avb', action='store_true',
                   help='drop the AVB vbmeta blob and footer')
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser('dt-table',
                       help='replace the DTB of a one-DTB SPRD DT table')
    p.add_argument('stock_dt', help='stock DT table (the dt file of unpack)')
    p.add_argument('dtb', help='new DTB')
    p.add_argument('out')
    p.set_defaults(fn=cmd_dt_table)
    p = sub.add_parser('dt-bootargs',
                       help='replace a substring of /chosen/bootargs in '
                            'every DTB of a SPRD DT table')
    p.add_argument('dt', help='DT table (the dt file of unpack)')
    p.add_argument('out')
    p.add_argument('--replace', nargs=2, required=True,
                   metavar=('OLD', 'NEW'), dest='pair',
                   help='OLD must occur exactly once in each DTB')
    p.set_defaults(fn=cmd_dt_bootargs)
    a = ap.parse_args()
    if getattr(a, 'pair', None):
        a.old, a.new = a.pair
    try:
        return a.fn(a)
    except (Error, OSError, struct.error, ValueError, UnicodeError,
            KeyError, TypeError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
