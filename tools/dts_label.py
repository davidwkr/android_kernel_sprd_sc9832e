#!/usr/bin/env python3
"""Turn a decompiled DTS (dtc -I dtb -O dts) into a labelled source.

Raw phandle cells in well known properties (clocks, interrupt-parent,
*-supply, pinctrl-N, gpios, ...) are replaced by &label references.  Every
explicit phandle / linux,phandle property is kept, so the compiled DTB keeps
the stock phandle numbers (a stock DTB is reproduced byte for byte).  Cells
that cannot be resolved with confidence stay raw numbers.

usage: dts_label.py stock.dts out.dts
"""
import re
import sys

SINGLE = [r'interrupt-parent', r'.*-supply', r'cpu', r'trip', r'remote-end?point|remote-enpoint',
          r'ref-node', r'usb-phy', r'digital-codec', r'sprd-audio-card,headset',
          r'memory[-_]region(-re)?', r'panel-drivers', r'cpu-idle-states',
          r'pinctrl-\d+', r'next-level-cache', r'sprd,syscon-.*', r'sprd,sys-.*',
          r'sprd,.*-syscon', r'sprd,aon-glb', r'sprd,dispc', r'sprd,isp',
          r'sprd,csi', r'sprd,wdt-phandle', r'sprd,anlg_phy_g7_controller']
CELLS = {'clocks': '#clock-cells', 'assigned-clocks': '#clock-cells',
         'assigned-clock-parents': '#clock-cells', 'dmas': '#dma-cells',
         'io-channels': '#io-channel-cells', 'hwlocks': '#hwlock-cells',
         'pwms': '#pwm-cells', 'sound-dai': '#sound-dai-cells',
         'thermal-sensors': '#thermal-sensor-cells', 'iommus': '#iommu-cells',
         'resets': '#reset-cells', 'power-domains': '#power-domain-cells'}
SINGLE_RE = re.compile('^(%s)$' % '|'.join(SINGLE))
GPIO_RE = re.compile(r'^(.*-)?gpios?$')
CELL_PROP = re.compile(r'^(\t*)([\w,.#+-]+) = <([^<>]*)>;$')
ANY_PROP = re.compile(r'^(\t*)([\w,.#+-]+)(?: = (.*))?;$')
NODE_OPEN = re.compile(r'^(\t*)([\w,.@+-]*|/) \{$')


class InputError(Exception):
    pass


def build(lines):
    root = {'name': '', 'parent': None, 'props': {}, 'line': None}
    cur = None
    nodes = []
    for i, ln in enumerate(lines):
        m = NODE_OPEN.match(ln)
        if m:
            n = {'name': m.group(2), 'parent': cur, 'props': {}, 'all': {}, 'line': i}
            nodes.append(n)
            cur = n
            continue
        if ln.strip() == '};':
            if cur is None:
                raise InputError('line %d: unbalanced "};"' % (i + 1))
            cur = cur['parent']
            continue
        m = ANY_PROP.match(ln)
        if m and cur is not None:
            cur['all'][m.group(2)] = m.group(3) or ''
        m = CELL_PROP.match(ln)
        if m and cur is not None:
            try:
                cur['props'][m.group(2)] = [int(x, 16)
                                            for x in m.group(3).split()]
            except ValueError:
                raise InputError('line %d: bad cell list' % (i + 1))
    if cur is not None:
        raise InputError('unterminated node at end of file')
    return nodes


def path(n):
    parts = []
    while n and n['parent'] is not None:
        parts.append(n['name'])
        n = n['parent']
    return parts[::-1]


def sane(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


def pname(n):
    return n['name'].split('@')[0] if n else ''


def has(t, k):
    return k in t['all']


def compat(t):
    return t['all'].get('compatible', '')


def is_regulator(t):
    return has(t, 'regulator-name') or 'regulator' in compat(t)


def is_syscon(t):
    return 'syscon' in compat(t) or pname(t).startswith('syscon')


# property regex -> check that the target node is a valid provider for it
VALID = [
    (r'interrupt-parent', lambda t: has(t, 'interrupt-controller')),
    (r'.*-supply', is_regulator),
    (r'cpu', lambda t: t['all'].get('device_type', '') == '"cpu"'),
    (r'trip', lambda t: pname(t['parent']) == 'trips'),
    (r'remote-end?point|remote-enpoint', lambda t: pname(t).startswith('endpoint')),
    (r'memory[-_]region(-re)?', lambda t: pname(t['parent']) == 'reserved-memory'),
    (r'pinctrl-\d+', lambda t: any(pname(a).startswith('pinctrl')
                                    for a in ancestors(t))),
    (r'cpu-idle-states', lambda t: pname(t['parent']) == 'idle-states'),
    (r'panel-drivers', lambda t: pname(t['parent']) == 'lcds'),
    (r'sprd,syscon-.*|sprd,sys-.*|sprd,.*-syscon|sprd,aon-glb|'
     r'sprd,anlg_phy_g7_controller', is_syscon),
    (r'sprd,dispc', lambda t: pname(t).startswith('dispc')),
    (r'sprd,isp|ref-node', lambda t: pname(t).startswith('isp')),
    (r'sprd,csi', lambda t: pname(t).startswith('csi')),
    (r'digital-codec', lambda t: pname(t).startswith('audio-codec')),
    (r'usb-phy', lambda t: 'phy' in compat(t) or 'phy' in pname(t)),
    (r'clocks|assigned-clocks|assigned-clock-parents',
     lambda t: has(t, '#clock-cells')),
    (r'cooling-device', lambda t: has(t, '#cooling-cells')),
    (r'(.*-)?gpios?', lambda t: has(t, 'gpio-controller')),
]


def ancestors(t):
    t = t['parent']
    while t is not None:
        yield t
        t = t['parent']


def valid(name, t):
    for rx, fn in VALID:
        if re.match('^(%s)$' % rx, name):
            return bool(fn(t))
    # remaining #cells based bindings: the provider's #<x>-cells is the test
    return name in CELLS


def convert(cells, name, byph):
    """Return list of tokens (str, or node for a phandle) or None."""
    if SINGLE_RE.match(name):
        if all(c in byph and valid(name, byph[c]) for c in cells):
            return [byph[c] for c in cells]
        return None
    if name in CELLS or GPIO_RE.match(name):
        cn = CELLS.get(name, '#gpio-cells')
        out, i = [], 0
        while i < len(cells):
            t = byph.get(cells[i])
            if (t is None or cn not in t['props'] or not t['props'][cn]
                    or not valid(name, t)):
                return None
            if len(t['props'][cn]) != 1:
                return None
            k = t['props'][cn][0]
            if not 0 <= k <= 16 or i + 1 + k > len(cells):
                return None
            out.append(t)
            out += ['0x%02x' % c
                    for c in cells[i + 1:i + 1 + k]]
            i += 1 + k
        return out
    if name == 'cooling-device':
        if len(cells) % 3 or not all(
                cells[i] in byph and valid(name, byph[cells[i]])
                for i in range(0, len(cells), 3)):
            return None
        out = []
        for i in range(0, len(cells), 3):
            out.append(byph[cells[i]])
            out += ['0x%02x' % c for c in cells[i + 1:i + 3]]
        return out
    return None


def main(src, dst):
    lines = open(src).read().split('\n')
    nodes = build(lines)
    byph = {}
    for n in nodes:
        p = n['props'].get('phandle')
        if p:
            byph[p[0]] = n
    used = set()
    repl = {}
    ncells = {}
    for n in nodes:
        for k, v in n['props'].items():
            if k in ('phandle', 'linux,phandle') or not v:
                continue
            r = convert(v, k, byph)
            if r is not None:
                repl[(id(n), k)] = r
    # find which lines belong to which node: re-walk
    cur, stack = None, []
    out = []
    # label assignment
    for r in repl.values():
        for t in r:
            if isinstance(t, dict):
                used.add(id(t))
    labels, taken = {}, set()
    for n in nodes:
        if id(n) not in used:
            continue
        parts = path(n)
        k = 1
        while True:
            lab = sane('_'.join(x.replace('@', '_') for x in parts[-k:]))
            if lab[0].isdigit():
                lab = 'n_' + lab
            if lab not in taken or k >= len(parts):
                break
            k += 1
        while lab in taken:
            lab += '_'
        taken.add(lab)
        labels[id(n)] = lab
    it = iter(nodes)
    stack = []
    for ln in lines:
        m = NODE_OPEN.match(ln)
        if m:
            n = next(it)
            stack.append(n)
            if id(n) in labels:
                ln = '%s%s: %s {' % (m.group(1), labels[id(n)], m.group(2))
            out.append(ln)
            continue
        if ln.strip() == '};':
            stack.pop()
            out.append(ln)
            continue
        m = CELL_PROP.match(ln)
        if m and stack and (id(stack[-1]), m.group(2)) in repl:
            toks = ['&' + labels[id(t)] if isinstance(t, dict) else t
                    for t in repl[(id(stack[-1]), m.group(2))]]
            ln = '%s%s = <%s>;' % (m.group(1), m.group(2), ' '.join(toks))
        out.append(ln)
    hdr = ('// Reconstructed from the stock DT table of the device boot image.\n'
           '// Labels were added where a phandle could be resolved; the explicit\n'
           '// phandle / linux,phandle properties are kept on purpose so that the\n'
           '// compiled DTB uses the same phandle numbers as the stock DTB.\n')
    text = '\n'.join(out)
    text = text.replace('/dts-v1/;\n', '/dts-v1/;\n\n' + hdr, 1)
    open(dst, 'w').write(text)
    print('%s: %d labels, %d converted properties' % (dst, len(labels), len(repl)))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    try:
        main(sys.argv[1], sys.argv[2])
    except (InputError, OSError, UnicodeDecodeError, ValueError, TypeError,
            IndexError, KeyError) as e:
        print('error: %s: %s' % (type(e).__name__, e), file=sys.stderr)
        sys.exit(2)
