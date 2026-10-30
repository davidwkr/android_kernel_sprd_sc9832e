#!/usr/bin/env python3
"""Compare a (stock) device tree with the drivers built in this tree.

Read-only.  Run with:  python3 -I tools/dt_driver_check.py DTS CONFIG [--src DIR]
  DTS     a decompiled .dts (untrusted input; only parsed, never executed)
  CONFIG  merged kernel .config (see scripts/watch_build.sh fragments)

For every enabled DT node with a compatible it reports:
  NODRV    no of_device_id entry anywhere in the tree
  NOTBUILT driver source exists but its Kconfig gate is off in CONFIG
  MISSING  property the matched driver source reads but the node lacks
           (heuristic: grep of of_* / regulator / gpiod calls, so optional
           properties show up too; triage the cited line)
  CELLS    literal-length of_property_read_u32_array() vs node cell count
Output is plain text on stdout.  Bounded: skips files > 2 MiB, DTS > 16 MiB.
"""
import os
import stat
import re
import sys

MAXF = 2 << 20


def read(p, lim=MAXF):
    """Read at most lim bytes of a regular file; "" if not regular or larger."""
    try:
        fd = os.open(p, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    except OSError:
        fd = os.open(p, os.O_RDONLY | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            return ""
        data = b""
        while len(data) <= lim:
            chunk = os.read(fd, lim + 1 - len(data))
            if not chunk:
                break
            data += chunk
        return "" if len(data) > lim else data.decode("utf-8", "replace")
    finally:
        os.close(fd)


def esc(s):
    """Escape everything non-printable in input-derived text."""
    return re.sub(r"[^\x20-\x7e\t]", lambda m: "\\x%02x" % ord(m.group()) if ord(m.group()) < 256 else "\\u%04x" % ord(m.group()), s)


def out(line):
    print(esc(line))


# ---------------------------------------------------------------- DTS parse
class Node:
    def __init__(self, name, parent):
        self.name, self.parent, self.props, self.kids = name, parent, {}, []
        self.line = 0

    @property
    def path(self):
        if self.parent is None:
            return "/"
        return (self.parent.path.rstrip("/") + "/" + self.name)


def parse_dts(text):
    root = Node("", None)
    cur = root
    buf, lineno, start = "", 0, 0
    inq = False
    for ln, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not buf:
            if s.startswith("//") or not s or s.startswith("/dts-v1/"):
                continue
            if s.endswith("{"):
                nm = s[:-1].strip()
                if ": " in nm:
                    nm = nm.split(": ", 1)[1].strip()
                if nm == "/":
                    cur.line = ln
                    continue
                n = Node(nm, cur)
                n.line = ln
                cur.kids.append(n)
                cur = n
                continue
            if s == "};":
                if cur.parent is not None:
                    cur = cur.parent
                continue
        buf += (" " if buf else "") + s
        if buf.count('"') % 2 == 0 and buf.endswith(";"):
            m = re.match(r"([^\s=;]+)\s*(?:=\s*(.*))?;$", buf)
            if m:
                cur.props[m.group(1)] = (m.group(2) or "", ln)
            buf = ""
    return root


def walk(n):
    yield n
    st = n.props.get("status", ("", 0))[0]
    if st and "okay" not in st and "ok" not in st:
        return  # disabled parent: children are not probed either
    for k in n.kids:
        yield from walk(k)


def strs(v):
    return [re.sub(r"[^\x20-\x7e]", lambda m: "\\x%02x" % ord(m.group()), s)
            for s in re.findall(r'"([^"]*)"', v)]


def cells(v):
    c = 0
    for grp in re.findall(r"<([^>]*)>", v):
        c += len(grp.split())
    return c


# --------------------------------------------------------------- .config
def load_config(p):
    d = {}
    for line in read(p, 16 << 20).splitlines():
        m = re.match(r"CONFIG_(\w+)=(.*)", line)
        if m:
            d[m.group(1)] = m.group(2)
    return d


def on(cfg, sym):
    return cfg.get(sym) in ("y", "m")


# ---------------------------------------------------------- Makefile gating
_mk = {}


def parse_mk(d):
    if d in _mk:
        return _mk[d]
    res = {"obj": {}, "comp": {}}
    p = os.path.join(d, "Makefile")
    if os.path.isfile(p):
        txt = read(p).replace("\\\n", " ")
        for line in txt.splitlines():
            m = re.match(r"\s*(obj|[\w-]+?)-(\$\((\w+)\)|y|objs|m)\s*[:+]?=\s*(.*)", line)
            if not m:
                continue
            kind, gate, sym, rest = m.group(1), m.group(2), m.group(3), m.group(4)
            items = rest.split("#")[0].split()
            if kind == "obj":
                g = sym[7:] if sym and sym.startswith("CONFIG_") else ("y" if gate in ("y", "m") else None)
                for it in items:
                    res["obj"].setdefault(it, set()).add(g)
            else:
                for it in items:
                    res["comp"].setdefault(kind + ".o", set()).add(it)
    _mk[d] = res
    return res


def gate_file(cfg, src, path):
    """True/False/None (unknown) whether path is built."""
    d, fn = os.path.split(path)
    obj = fn[:-2] + ".o"

    def dir_gate(dd):
        if dd in ("", ".", src):
            return True
        parent, base = os.path.split(dd)
        mk = parse_mk(os.path.join(src, parent))
        gs = mk["obj"].get(base + "/")
        if gs is None:
            return None
        ok = any(g == "y" or (g and on(cfg, g)) for g in gs)
        return ok and dir_gate(parent)

    def obj_gate(dd, o, depth=0):
        mk = parse_mk(os.path.join(src, dd))
        r = False
        found = False
        if o in mk["obj"]:
            found = True
            if any(g == "y" or (g and on(cfg, g)) for g in mk["obj"][o]):
                r = True
        for comp, parts in mk["comp"].items():
            if o in parts and depth < 4:
                got = obj_gate(dd, comp, depth + 1)
                if got is not None:
                    found = True
                    r = r or got
        return r if found else None

    g = obj_gate(d, obj)
    if g is None:
        return None
    dg = dir_gate(d)
    return g and (dg is not False)


# ------------------------------------------------------------ driver index
COMPAT = re.compile(r'(?:\.compatible\s*=|(?:CLK_OF_DECLARE\w*|IRQCHIP_DECLARE|OF_DECLARE_\d|CPUIDLE_METHOD_OF_DECLARE|RESERVEDMEM_OF_DECLARE|TIMER_OF_DECLARE|CLOCKSOURCE_OF_DECLARE)\s*\(\s*\w+\s*,|of_find_compatible_node\s*\([^"]*,[^"]*,|of_device_is_compatible\s*\(\s*\w+\s*,|of_machine_is_compatible\s*\()\s*"([^"]+)"')
CALLS = re.compile(
    r"\b(of_property_read_\w+|of_get_named_gpio\w*|of_parse_phandle\w*|"
    r"of_count_phandle_with_args|of_get_property|of_find_property|"
    r"of_property_count_\w+|of_property_match_string|of_get_child_by_name|"
    r"of_get_gpio\w*)\s*\(([^;]*?)\"([^\"]+)\"([^;]*?)\)\s*[;,)]?", re.S)
REG = re.compile(r'\b(?:devm_)?regulator_get\w*\s*\(\s*[^,]+,\s*"([^"]+)"')
GPIOD = re.compile(r'\bdevm_gpiod_get\w*\s*\(\s*[^,]+,\s*"([^"]+)"')


def index_drivers(src):
    comp = {}
    for top in ("drivers", "sound", "arch/arm64", "kernel", "mm", "lib", "fs", "net"):
        for dp, dn, fn in os.walk(os.path.join(src, top)):
            for f in fn:
                if not f.endswith(".c"):
                    continue
                p = os.path.join(dp, f)
                t = read(p)
                if "compatible" not in t and "_DECLARE" not in t:
                    continue
                rel = os.path.relpath(p, src)
                for m in COMPAT.finditer(t):
                    ln = t.count("\n", 0, m.start()) + 1
                    comp.setdefault(m.group(1), []).append((rel, ln))
    return comp


def prop_reads(src, rel):
    t = read(os.path.join(src, rel))
    out = []
    for m in CALLS.finditer(t):
        fn, prop = m.group(1), m.group(3)
        if " " in prop or "%" in prop:
            continue
        ln = t.count("\n", 0, m.start(3)) + 1
        out.append((prop, ln, fn, m.group(4)))
    for m in REG.finditer(t):
        out.append((m.group(1) + "-supply", t.count("\n", 0, m.start(1)) + 1, "regulator_get", ""))
    for m in GPIOD.finditer(t):
        out.append((m.group(1) + "-gpios", t.count("\n", 0, m.start(1)) + 1, "gpiod_get", ""))
    return out


def main():
    argv = [a for a in sys.argv[1:]]
    src = "."
    if "--src" in argv:
        i = argv.index("--src")
        if i + 1 >= len(argv):
            print(__doc__)
            return 2
        src = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) != 2:
        print(__doc__)
        return 2
    cfg = load_config(argv[1])
    root = parse_dts(read(argv[0], 16 << 20))
    comp = index_drivers(src)
    for n in walk(root):
        c = n.props.get("compatible")
        if not c:
            continue
        st = n.props.get("status", ("", 0))[0]
        if st and "okay" not in st and "ok" not in st:
            continue
        cl = strs(c[0])
        hits = [(k, h) for k in cl for h in comp.get(k, [])]
        where = "%s:%d" % (n.path, c[1])
        if not hits:
            out("NODRV\t%s\t%s" % (where, ",".join(cl)))
            continue
        files = {}
        for k, (f, ln) in hits:
            files.setdefault(f, (k, ln))
        built = [f for f in files if gate_file(cfg, src, f)]
        if not built:
            unk = [f for f in files if gate_file(cfg, src, f) is None]
            out("%s\t%s\t%s\t%s" % ("UNKGATE" if unk else "NOTBUILT", where, ",".join(cl),
                                      ",".join("%s:%d" % (f, files[f][1]) for f in files)))
            continue
        # property check against built matching files (+ same-composite siblings ignored)
        for f in built:
            seen = set()
            for prop, ln, fn, tail in prop_reads(src, f):
                if prop in seen:
                    continue
                seen.add(prop)
                if fn == "of_get_child_by_name":
                    if not any(k.name.split("@")[0] == prop for k in n.kids):
                        out("MISSING\t%s\tchild-node %s\t%s:%d" % (where, prop, f, ln))
                    continue
                if prop not in n.props:
                    if fn.startswith("of_get_named_gpio") or fn.startswith("of_parse_phandle") or "of_property_read" in fn or "get_property" in fn or "find_property" in fn or fn in ("regulator_get", "gpiod_get"):
                        out("MISSING\t%s\t%s\t%s:%d %s" % (where, prop, f, ln, fn))
                elif fn == "of_property_read_u32_array":
                    m = re.search(r",\s*(\d+)\s*$", tail.strip())
                    if m and cells(n.props[prop][0]) != int(m.group(1)):
                        out("CELLS\t%s\t%s has %d, driver wants %s\t%s:%d" % (
                            where, prop, cells(n.props[prop][0]), m.group(1), f, ln))
    return 0


if __name__ == "__main__":
    sys.exit(main())
