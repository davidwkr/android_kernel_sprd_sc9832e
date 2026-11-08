#!/usr/bin/env python3
"""blobcarve: carve a relocatable AArch64 object out of a stripped arm64 Image.

Run as:  python3 -I blobcarve.py --boot-img BOOT.IMG --out DIR [--cross PREFIX]
Single file on purpose: `python3 -I` does not put the script dir on sys.path.
See README.md.  Stdlib only.  Never writes stock bytes anywhere but --out.
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

TEXT_VA_DEFAULT = 0xffffff8008080000

# ---- driver description (validated against kallsyms names, fail on mismatch)
EXPECT = {  # offset-from-_text -> symbol name that must live there
    0x4d3ca8: "hrs3xxx_i2c_resume",
    0x4d4794: "hx3918_delay",
    0x4d79ec: "hr_sqrt",
    0x4eae14: "bd1xxx_release",
}
CARVE_LO, CARVE_HI = 0x4d4794, 0x4eae14      # layer + algorithm (no glue)
EXCLUDE = ("hx3918_ppg_timer_cfg", "hx3918_agc_timer_cfg")
# variables the stock code reads by value with ADRP+LDR; they live outside
# the blob and are referenced symbolically (ADRP and LDR lo12 rewritten).
EXTERNAL_VARS = ("__stack_chk_guard", "system_wq")
PAGE = 0x1000
MERGE_GAP = 0xff    # slack tolerated between objects of one cluster


class Fatal(Exception):
    pass


def die(msg):
    raise Fatal(msg)


# ------------------------------------------------------------------ image
def load_kernel(path):
    b = open(path, "rb").read()
    if b[:8] == b"ANDROID!":
        ks, = struct.unpack_from("<I", b, 8)
        ps, = struct.unpack_from("<I", b, 36)
        return b[ps:ps + ks]
    return b


class Image:
    def __init__(self, raw, text_va):
        if raw[56:60] != b"ARMd":
            die("not an arm64 Image")
        self.raw = raw
        self.text_va = text_va
        n = len(raw) // 8
        u = struct.unpack("<%dQ" % n, raw[:n * 8])
        self.abs64 = {}
        for i in range(1, n - 1):
            # Elf64_Rela: offset, info, addend ; ABS64 against symbol 6 (_text)
            if u[i] == 0x600000101 and 0 <= u[i - 1] - text_va < len(raw):
                self.abs64[u[i - 1] - text_va] = u[i + 1]
        self._load_kallsyms()

    def _load_kallsyms(self):
        import re
        d = self.raw
        ti = None
        for m in re.finditer(b"\x00\x00[\x01-\x09]\x00", d):
            q = m.start()
            if q % 2:
                continue
            v = struct.unpack_from("<256H", d, q)
            if all(0 < v[i + 1] - v[i] < 40 for i in range(255)) and v[255] > 600:
                ti = q
                break
        if ti is None:
            die("kallsyms token_index not found")
        v = struct.unpack_from("<256H", d, ti)
        ts = None
        for c in range(ti - 1300, ti - 500):
            if all(d[c + v[i] - 1] == 0 for i in range(1, 256)) and \
                    d.index(b"\0", c + v[255]) + 1 <= ti:
                ts = c
                break
        if ts is None:
            die("kallsyms token_table not found")
        tok = [d[ts + v[i]:d.index(b"\0", ts + v[i])].decode("latin1")
               for i in range(256)]
        q = ts
        while True:
            q -= 8
            if struct.unpack_from("<Q", d, q)[0] == 0 and \
                    1000 < struct.unpack_from("<Q", d, q + 8)[0] < 6000:
                break
        nm = (ts - q) // 8
        mk = struct.unpack_from("<%dQ" % nm, d, q)
        while mk[-1] == 0:
            mk = mk[:-1]
        nm = len(mk)
        last = mk[-1]
        st = None
        for s in range(q - last - 12000, q - last + 200):
            p = s
            for _ in range(256):
                p += 1 + d[p]
            if p - s == mk[1]:
                p = s
                for _ in range(512):
                    p += 1 + d[p]
                if p - s == mk[2]:
                    st = s
                    break
        if st is None:
            die("kallsyms names not found")
        ns = None
        for p in range(st - 0x400, st, 8):
            a = struct.unpack_from("<Q", d, p)[0]
            if (nm - 1) * 256 < a <= nm * 256:
                ns = (p, a)
        n = ns[1]
        types, names = [], []
        p = st
        for _ in range(n):
            l = d[p]
            p += 1
            s = "".join(tok[b] for b in d[p:p + l])
            p += l
            types.append(s[0])
            names.append(s[1:])
        # address table: last ABS64-relocated run ending before num_syms
        hi = max(k for k in self.abs64 if k < ns[0])
        base = hi + 8 - 8 * n
        addrs = []
        for i in range(n):
            a = self.abs64.get(base + 8 * i)
            addrs.append(a)
        self.ks = []
        for i in range(n):
            if addrs[i] is None:
                continue     # one slot (_etext) has no reloc
            self.ks.append((addrs[i], types[i], names[i]))
        self.ks.sort(key=lambda s: s[0])
        self.ks_addr = [s[0] for s in self.ks]
        self.by_addr = collections.defaultdict(list)
        for a, t, nme in self.ks:
            self.by_addr[a].append((t, nme))
        self.by_name = collections.defaultdict(list)
        for a, t, nme in self.ks:
            self.by_name[nme].append(a)

    def sym_at(self, a):
        """kallsyms entries (type, name) starting exactly at a"""
        return self.by_addr.get(a, [])

    def sym_cover(self, a):
        i = bisect.bisect_right(self.ks_addr, a) - 1
        return self.ks[i] if i >= 0 else None

    def word(self, off):
        return struct.unpack_from("<I", self.raw, off)[0]

    def in_file(self, off, n=1):
        return 0 <= off and off + n <= len(self.raw)


# ---------------------------------------------------------------- decoder
def sx(v, b):
    return v - (1 << b) if v & (1 << (b - 1)) else v


def rn(x):
    return (x >> 5) & 31


def rd(x):
    return x & 31


def decode_ldst(x):
    """Decode the load/store encodings that the driver may use.
    Returns dict(rt, rt2, rn, load, size, imm, mode, rm, wb) or dies."""
    v = (x >> 26) & 1
    r = dict(rt=x & 31, rt2=None, rn=rn(x), load=False, size=0, imm=0,
             mode="off", rm=None, wb=False, v=v, prfm=False, lit=False)
    if (x & 0x3f000000) == 0x08000000:               # exclusive
        r["size"] = 1 << (x >> 30)
        r["load"] = bool((x >> 22) & 1)
        r["mode"] = "excl"
        r["rs"] = (x >> 16) & 31
        if (x >> 21) & 1:                            # pair exclusive
            r["rt2"] = (x >> 10) & 31
            r["size"] *= 2
        return r
    if (x & 0x3b000000) == 0x18000000:               # literal
        r["lit"] = True
        r["load"] = True
        r["mode"] = "lit"
        opc = x >> 30
        r["prfm"] = (opc == 3)
        r["size"] = 4 if opc == 0 else 8 if opc in (1, 2) else 0
        if v:
            r["size"] = (4, 8, 16, 0)[opc]
        r["imm"] = sx((x >> 5) & 0x7ffff, 19) * 4
        return r
    if (x & 0x3a000000) == 0x28000000:               # pair
        opc = x >> 30
        r["load"] = bool((x >> 22) & 1)
        scale = (2, 3, 4)[opc] if v else (2, 3, 2)[opc]
        if opc == 3:
            die("ldst pair opc=3 at %#x" % x)
        r["size"] = 2 << scale
        r["imm"] = sx((x >> 15) & 0x7f, 7) << scale
        r["rt2"] = (x >> 10) & 31
        m = (x >> 23) & 3
        r["mode"] = {0: "off", 1: "post", 2: "off", 3: "pre"}[m]
        r["wb"] = m in (1, 3)
        if m == 0:
            die("ldnp/stnp not handled")
        return r
    if (x & 0x3b000000) == 0x39000000:               # unsigned imm
        size = x >> 30
        opc = (x >> 22) & 3
        if v:
            if opc & 2:
                size = 4
            sz = size
        else:
            sz = size
        r["size"] = 1 << sz
        r["imm"] = ((x >> 10) & 0xfff) << sz
        r["load"] = (opc != 0) if not v else bool(opc & 1)
        r["prfm"] = (not v and size == 3 and opc == 2)
        return r
    if (x & 0x3b000000) == 0x38000000:
        size = x >> 30
        opc = (x >> 22) & 3
        sz = size + (4 if (v and opc & 2) else 0) if v else size
        r["size"] = 1 << sz
        r["load"] = (opc != 0) if not v else bool(opc & 1)
        r["prfm"] = (not v and size == 3 and opc == 2)
        if (x >> 21) & 1:
            if ((x >> 10) & 3) != 2:
                die("atomic/other ldst %#x" % x)
            r["mode"] = "reg"
            r["rm"] = (x >> 16) & 31
            r["opt"] = (x >> 13) & 7
            r["S"] = (x >> 12) & 1
            return r
        r["imm"] = sx((x >> 12) & 0x1ff, 9)
        m = (x >> 10) & 3
        r["mode"] = {0: "off", 1: "post", 2: "off", 3: "pre"}[m]
        r["wb"] = m in (1, 3)
        return r
    die("unhandled load/store encoding %#010x" % x)


def writes(x):
    """GPRs written by x (excluding xzr); 'CALL' marker for BL/BLR."""
    op0 = (x >> 25) & 15
    if op0 in (8, 9):
        return [x & 31]
    if op0 in (10, 11):
        if (x & 0xfc000000) == 0x94000000 or (x & 0xfffffc1f) == 0xd63f0000:
            return ["CALL"]
        if (x & 0xfff00000) == 0xd5300000:           # mrs
            return [x & 31]
        return []
    if op0 in (5, 13):
        return [x & 31]
    if op0 in (4, 6, 12, 14):
        r = decode_ldst(x)
        w = []
        if r["lit"]:
            return [] if (r["prfm"] or r["v"]) else [r["rt"]]
        if r["mode"] == "excl":
            if r["load"]:
                w.append(r["rt"])
                if r["rt2"] is not None:
                    w.append(r["rt2"])
            else:
                w.append(r["rs"])
            return w
        if r["load"] and not r["v"] and not r["prfm"]:
            w.append(r["rt"])
            if r["rt2"] is not None:
                w.append(r["rt2"])
        if r["wb"]:
            w.append(r["rn"])
        return w
    die("unhandled instruction class %#010x (op0=%d)" % (x, op0))


# ------------------------------------------------------- function analysis
class Func:
    def __init__(self, name, lo, hi, words):
        self.name, self.lo, self.hi, self.words = name, lo, hi, words
        self.adrp = {}      # pc -> target page (offset-from-_text)
        self.adr = {}       # pc -> target
        self.bl = {}        # pc -> target
        self.b = {}         # pc -> target (B, any)
        self.local = {}     # pc -> target (bcond/cbz/tbz/ldrlit)
        self.touch = []     # (lo, hi, kind, pc, prov)
        self.dyn = []       # (pc, base A, info)
        self.escapes = []   # (pc, A, callee-or-what)
        self.indirect = []  # pc of blr/unresolved br
        self.switches = {}  # br pc -> dict(targets=[..], table=(A,n,esz))
        self.lo12 = {}      # pc -> (A) tracked address used by add/ldst imm


def _join(a, b):
    if a is None:
        return b
    if b is None:
        return a
    out = {}
    for r, (k, A, p) in a.items():
        o = b.get(r)
        if o and o[0] == k and o[1] == A:
            out[r] = (k, A, p | o[2])
    return out


def analyse_function(f, switch_targets, im):
    ro_limit = im.by_name["__end_rodata"][0]
    n = len(f.words)
    pcs = [f.lo + 4 * i for i in range(n)]
    ins = f.words
    for tbl in (f.adrp, f.adr, f.bl, f.b, f.local, f.lo12):
        tbl.clear()
    f.touch, f.dyn, f.escapes, f.indirect = [], [], [], []
    st = [None] * n
    st[0] = {}
    work = [0]
    unresolved = []

    def succ(i, x):
        pc = pcs[i]
        if (x & 0xfc000000) == 0x14000000:           # B
            t = pc + sx(x & 0x3ffffff, 26) * 4
            return [(t - f.lo) // 4] if f.lo <= t < f.hi else []
        if (x & 0xff000010) == 0x54000000:
            t = pc + sx((x >> 5) & 0x7ffff, 19) * 4
            return [i + 1, (t - f.lo) // 4]
        if (x & 0x7e000000) == 0x34000000:
            t = pc + sx((x >> 5) & 0x7ffff, 19) * 4
            return [i + 1, (t - f.lo) // 4]
        if (x & 0x7e000000) == 0x36000000:
            t = pc + sx((x >> 5) & 0x3fff, 14) * 4
            return [i + 1, (t - f.lo) // 4]
        if (x & 0xfffffc1f) == 0xd65f0000:           # RET
            return []
        if (x & 0xfffffc1f) == 0xd61f0000:           # BR
            if pc in switch_targets:
                return [(t - f.lo) // 4 for t in switch_targets[pc]]
            unresolved.append(pc)
            return []
        return [i + 1]

    seen_events = {}
    while work:
        i = work.pop()
        if i >= n:
            die("%s: falls off end of function" % f.name)
        s = dict(st[i])
        x = ins[i]
        pc = pcs[i]
        ev = []

        def val(r):
            return s.get(r) if r != 31 else None

        def touch(A, w, kind, prov):
            ev.append(("touch", A, A + max(w, 1), kind, pc, frozenset(prov)))

        op0 = (x >> 25) & 15
        killed = False
        folded = None
        if (x & 0x9f000000) == 0x90000000:           # ADRP
            imm = sx((((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3), 21)
            page = (pc & ~0xfff) + (imm << 12)
            ev.append(("adrp", pc, page))
            s[x & 31] = ("P", page, frozenset([pc]))
            killed = True
        elif (x & 0x9f000000) == 0x10000000:         # ADR
            imm = sx((((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3), 21)
            ev.append(("adr", pc, pc + imm))
            s.pop(x & 31, None)
            killed = True
        elif (x & 0x7f800000) == 0x11000000 or (x & 0x7f800000) == 0x51000000:
            sub = bool(x & 0x40000000)
            imm = ((x >> 10) & 0xfff) << (12 if (x >> 22) & 1 else 0)
            src = val(rn(x)) if (x >> 31) & 1 and not (x & 0x20000000) else None
            d = x & 31
            if src is not None:
                A = src[1] + (-imm if sub else imm)
                k = "I" if src[0] == "I" else "C"
                s[d] = (k, A, src[2])
                ev.append(("lo12", pc, A))
                if k == "C":
                    touch(A, 1, "addr", src[2])
            else:
                s.pop(d, None)
            killed = True
        elif (x & 0x7fe0ffe0) == 0x2a0003e0 and (x >> 31):   # mov xd, xm
            m = val((x >> 16) & 31)
            d = x & 31
            if m is not None:
                s[d] = m
            else:
                s.pop(d, None)
            killed = True
        elif ((x & 0x7f200000) == 0x0b000000 or (x & 0x7f200000) == 0x0b200000) \
                and (x >> 31) and not (x & 0x40000000) and not (x & 0x20000000):
            a_, b_ = val(rn(x)), val((x >> 16) & 31)
            d = x & 31
            base = a_ or b_
            if base is not None:
                s[d] = ("I", base[1], base[2])
                ev.append(("dynaddr", pc, base[1]))
            else:
                s.pop(d, None)
            killed = True
        if not killed and op0 in (4, 6, 12, 14):
            r = decode_ldst(x)
            if not r["lit"] and r["mode"] != "excl":
                b = val(r["rn"]) if r["rn"] != 31 else None
                if b is not None:
                    if r["mode"] == "reg":
                        ev.append(("dyn", pc, b[1], r["size"], r["rm"], r, b[2]))
                    elif b[0] == "I":
                        ev.append(("dyn", pc, b[1], r["size"], None, r, b[2]))
                    elif False:
                        pass
                    else:
                        eff = b[1] + (r["imm"] if r["mode"] != "post" else 0)
                        w = r["size"]
                        touch(eff, w, "ld" if r["load"] else "st", b[2])
                        ev.append(("lo12", pc, eff))
                        if r["wb"]:
                            s[r["rn"]] = ("C", b[1] + r["imm"], b[2])
                        # fold 8-byte loads of link-time pointers from
                        # read-only memory (the ABS64 pointer table)
                        if r["load"] and not r["v"] and not r["wb"] and \
                                r["size"] == 8 and eff < ro_limit and \
                                eff % 8 == 0 and im.in_file(eff, 8):
                            pv = struct.unpack_from("<Q", im.raw, eff)[0]
                            if 0 <= pv - im.text_va < (1 << 26):
                                ev.append(("slot", pc, eff, pv - im.text_va))
                                s[r["rt"]] = ("C", pv - im.text_va, frozenset())
                                folded = r["rt"]
        # clobbers
        if not killed:
            for w in writes(x):
                if w == "CALL":
                    for rg in list(s):
                        if rg <= 18 or rg == 30:
                            if rg <= 7 and s[rg][0] in ("C", "I"):
                                ev.append(("escape", pc, s[rg][1], s[rg][0]))
                                if s[rg][0] == "C":
                                    touch(s[rg][1], 1, "escape", s[rg][2])
                            del s[rg]
                    if (x & 0xfc000000) != 0x94000000:
                        ev.append(("blr", pc))
                elif w != 31 and w != folded:
                    if not (r_wb_keep(x, w, s, st, i)):
                        s.pop(w, None)
        # record events (idempotent by key)
        for e in ev:
            seen_events[tuple(e[:5])] = e
        if (x & 0xfc000000) == 0x94000000:
            t = pc + sx(x & 0x3ffffff, 26) * 4
            f.bl[pc] = t
        for j in succ(i, x):
            if not (0 <= j < n):
                die("%s: branch target outside function at %#x" % (f.name, pc))
            ns = _join(st[j], s) if st[j] is not None else s
            if st[j] is None or ns != st[j]:
                st[j] = ns
                work.append(j)
    f.unresolved = sorted(set(unresolved))
    f.states = st
    f.events = list(seen_events.values())


def r_wb_keep(x, w, s, st, i):
    """write-back to the base of a tracked pointer keeps it tracked (already
    updated by the ld/st handler); everything else kills."""
    op0 = (x >> 25) & 15
    if op0 in (4, 6, 12, 14):
        r = decode_ldst(x)
        if r["wb"] and w == r["rn"] and r["rn"] in s and not r["lit"]:
            return True
    return False


def resolve_switch(f, br_pc):
    """Recognise the GCC jump-table idiom ending in `br` at br_pc:
         [cmp wI,#K ; b.hi dflt ;] adrp/add T ; ldr{b,h,} wE,[T,wI,uxtw{#2}]
         adr xB,lbl ; add xD,xB,wE,sxt{b,h,w} #2 ; br xD
    Returns dict(targets, table=(A, nentries, esize)) or dies."""
    def W(pc):
        return f.words[(pc - f.lo) // 4]
    ldr_pc, adr_pc, add_pc = br_pc - 12, br_pc - 8, br_pc - 4
    if ldr_pc < f.lo:
        die("%s: br at %#x too close to start" % (f.name, br_pc))
    xa, xl = W(add_pc), W(ldr_pc)
    if (W(adr_pc) & 0x9f000000) != 0x10000000:
        die("%s: br %#x: no ADR before ADD" % (f.name, br_pc))
    if (xa & 0xffe00000) != 0x8b200000 or ((xa >> 10) & 7) != 2:
        die("%s: br %#x: ADD form not 'add x,x,w,sxt* #2'" % (f.name, br_pc))
    opt = (xa >> 13) & 7
    esz = {4: 1, 5: 2, 6: 4}.get(opt)
    if esz is None:
        die("%s: br %#x: unexpected extend option %d" % (f.name, br_pc, opt))
    ev = [e for e in f.events if e[0] == "dyn" and e[1] == ldr_pc]
    if len(ev) != 1:
        die("%s: br %#x: table base not tracked" % (f.name, br_pc))
    A, rm, r, prov = ev[0][2], ev[0][4], ev[0][5], ev[0][6]
    if r["size"] != esz or r["rm"] is None:
        die("%s: br %#x: table load size mismatch" % (f.name, br_pc))
    K = None
    for k in range(1, 9):
        y = W(ldr_pc - 4 * k) if ldr_pc - 4 * k >= f.lo else 0
        if (y & 0x7f80001f) == 0x7100001f and rn(y) == rm:
            K = (y >> 10) & 0xfff
            break
    if K is None:
        die("%s: br %#x: no CMP bound for jump table" % (f.name, br_pc))
    return dict(A=A, n=K + 1, esz=esz, prov=prov, ldr_pc=ldr_pc,
                base=adr_pc + sx((((W(adr_pc) >> 5) & 0x7ffff) << 2) |
                                 ((W(adr_pc) >> 29) & 3), 21))


def build_funcs(im):
    for a, nme in EXPECT.items():
        if not any(n == nme for _, n in im.sym_at(a)):
            die("image mismatch: expected %s at %#x" % (nme, a))
    funcs = []
    ks = im.ks_addr
    for a, t, n in im.ks:
        if CARVE_LO <= a < CARVE_HI and n not in EXCLUDE:
            if t not in "Tt":
                die("non-text symbol %s in carve range" % n)
            j = bisect.bisect_right(ks, a)
            e = ks[j]
            funcs.append(Func(n, a, e, list(struct.unpack_from(
                "<%dI" % ((e - a) // 4), im.raw, a))))
    names = [f.name for f in funcs]
    if len(set(names)) != len(names):
        die("duplicate carved function names")
    # contiguity: gaps allowed only for the excluded functions
    gaps = []
    for g, h in zip(funcs, funcs[1:]):
        if g.hi != h.lo:
            gaps.append((g.hi, h.lo))
    for f in funcs:
        sw = {}
        for _ in range(3):
            analyse_function(f, {k: v["targets"] for k, v in sw.items()}, im)
            if not f.unresolved:
                break
            for br in f.unresolved:
                s = resolve_switch(f, br)
                tgt = []
                for i in range(s["n"]):
                    o = s["A"] + i * s["esz"]
                    if not im.in_file(o, s["esz"]):
                        die("%s: jump table outside file" % f.name)
                    ent = int.from_bytes(im.raw[o:o + s["esz"]], "little")
                    if s["esz"] < 4 and False:
                        pass
                    ent = ent if s["esz"] == 4 else ent
                    ent = sx(ent, 8 * s["esz"])
                    t = s["base"] + 4 * ent
                    if not (f.lo <= t < f.hi) or t & 3:
                        die("%s: jump target %#x outside function" % (f.name, t))
                    tgt.append(t)
                s["targets"] = tgt
                sw[br] = s
        if f.unresolved:
            die("%s: unresolved indirect branch" % f.name)
        f.switches = sw
        for br, s in sw.items():
            f.events.append(("touch", s["A"], s["A"] + s["n"] * s["esz"], "jt",
                             s["ldr_pc"], s["prov"]))
    return funcs, gaps


# ------------------------------------------------------------------ chunks
DATA_TYPES = "bBdDrR"


class Chunk:
    def __init__(self, lo, hi):
        self.lo, self.hi = lo, hi
        self.static_hi = hi
        self.why = set()

    @property
    def base(self):
        return self.lo & ~(PAGE - 1)

    @property
    def size(self):
        return self.hi - self.lo


def mem_class(im, a):
    if a < im.by_name["__end_rodata"][0]:
        return "ro"
    return "data" if im.in_file(a) else "bss"


def next_sym_addr(im, a):
    j = bisect.bisect_right(im.ks_addr, a)
    return im.ks_addr[j] if j < len(im.ks_addr) else None


def c_string_end(im, a):
    if not im.in_file(a):
        return None
    e = im.raw.find(b"\0", a)
    s = im.raw[a:e]
    if e < 0 or not s or len(s) > 512 or s[0] < 1 or \
            any(c < 9 and i > 0 for i, c in enumerate(s)):
        return None
    return e + 1


def build_chunks(im, funcs, ext_addr):
    """Return (chunks, adrp_chunk, slots, stats).  ext_addr: {addr: name}."""
    ro_limit = im.by_name["__end_rodata"][0]
    touches = []                                  # (lo, hi, kind, pc, prov)
    slots = {}
    for f in funcs:
        for e in f.events:
            if e[0] == "touch":
                touches.append((e[1], e[2], e[3], e[4], e[5], f))
            elif e[0] == "slot":
                slots[e[2]] = e[3]
    ranges = []
    ext_pcs = collections.defaultdict(set)        # adrp pc -> ext addrs
    for lo, hi, kind, pc, prov, f in touches:
        if lo in ext_addr:
            for p in prov:
                ext_pcs[p].add(lo)
            continue
        ranges.append([lo, hi, hi, kind, prov, f.name])
    for sa, tgt in slots.items():                 # every slot target needs a home
        ranges.append([tgt, tgt + 1, tgt + 1, "slotref", frozenset(), "-"])
    # dynamic accesses: base A plus an unknown index (no static bound)
    for f in funcs:
        sw_pcs = {s["ldr_pc"] for s in f.switches.values()}
        for e in f.events:
            if (e[0] == "dyn" and e[1] not in sw_pcs) or e[0] == "dynaddr":
                A = e[2]
                if A in ext_addr:
                    continue
                ranges.append([A, A + 1, A + 1, "dyn", frozenset(), f.name])
    for r in ranges:
        lo, hi = r[0], r[1]
        if r[3] in ("addr", "escape") and lo < ro_limit:
            e = c_string_end(im, lo)
            if e:
                r[1] = max(hi, e)
        if r[3] in ("escape", "slotref", "dyn"):
            if r[1] > hi:
                continue
            cov = im.sym_cover(lo)
            if cov and cov[1] in DATA_TYPES:
                nx = next_sym_addr(im, lo)
                if nx:
                    if nx - lo > 0x4000:
                        die("object rounding at %#x would span %#x bytes"
                            % (lo, nx - lo))
                    r[1] = max(hi, nx)
    ranges.sort(key=lambda r: (r[0], r[1]))
    chunks = []
    for r in ranges:
        if chunks and r[0] <= chunks[-1].hi + MERGE_GAP and \
                mem_class(im, r[0]) == mem_class(im, chunks[-1].lo):
            c = chunks[-1]
            c.hi = max(c.hi, r[1])
            c.static_hi = max(c.static_hi, r[2])
        else:
            c = Chunk(r[0], r[1])
            c.static_hi = r[2]
            chunks.append(c)
        c.rs = getattr(c, "rs", []) + [r]
    # union chunks that share an ADRP
    parent = list(range(len(chunks)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def chunk_of(a):
        for i, c in enumerate(chunks):
            if c.lo <= a < c.hi:
                return i
        return None
    by_adrp = collections.defaultdict(set)
    for r in ranges:
        for p in r[4]:
            i = chunk_of(r[0])
            by_adrp[p].add(i)
    for p, s in by_adrp.items():
        s = sorted(s)
        for i in s[1:]:
            parent[find(i)] = find(s[0])
    groups = collections.defaultdict(list)
    for i, c in enumerate(chunks):
        groups[find(i)].append(c)
    merged = []
    for g in groups.values():
        c = Chunk(min(x.lo for x in g), max(x.hi for x in g))
        c.static_hi = max(x.static_hi for x in g)
        c.rs = [r for x in g for r in x.rs]
        if len(g) > 1:
            c.why.add("merged via shared ADRP")
        merged.append(c)
    merged.sort(key=lambda c: c.lo)
    for a, b in zip(merged, merged[1:]):
        if b.lo < a.hi:
            die("overlapping chunks after merge; rerun merge")  # pragma: no cover
    return merged, by_adrp, ext_pcs, slots


# ----------------------------------------------------------------- emitter
class Emit:
    def __init__(self, im, funcs, chunks, ext_addr, slots):
        self.im, self.funcs, self.chunks = im, funcs, chunks
        self.ext_addr = ext_addr          # addr -> external variable name
        self.slots = slots
        self.fn_by_lo = {f.lo: f for f in funcs}
        self.undef = {}                   # name -> stock offset
        self.notes = []
        self.cnt = collections.Counter()
        self.fallback = []
        self.ext_pcs_by_func = {}
        self.indirect = []
        for i, c in enumerate(chunks):
            c.idx = i
            c.sym = "hrsblob_c%d" % i
            c.cls = mem_class(im, c.lo)
            if c.cls != "bss" and not im.in_file(c.lo, c.size):
                die("chunk %#x..%#x crosses end of file image" % (c.lo, c.hi))

    # -- symbol helpers
    def ext_name(self, a):
        syms = self.im.sym_at(a)
        if not syms:
            die("branch/pointer target %#x has no kallsyms symbol" % a)
        syms = sorted(syms, key=lambda t: (not t[0].isupper(), t[1]))
        name = syms[0][1]
        if len(self.im.by_name[name]) > 1:
            name = "%s__%x" % (name, a)
        if name in self.undef and self.undef[name] != a:
            die("name clash for %s" % name)
        self.undef[name] = a
        return name

    def code_sym(self, t, what):
        """symbol expression for a code address t"""
        f = self.fn_by_lo.get(t)
        if f:
            return f.name
        for g in self.funcs:
            if g.lo <= t < g.hi:
                die("%s: target %#x is inside %s, not its entry" % (what, t, g.name))
        return self.ext_name(t)

    def chunk_at(self, a):
        for c in self.chunks:
            if c.lo <= a < c.hi:
                return c
        return None

    def data_ref(self, a, what):
        c = self.chunk_at(a)
        if c is None:
            die("%s: address %#x is in no chunk" % (what, a))
        return "%s+%#x" % (c.sym, a - c.base)

    # -- code
    def emit_function(self, f, adrp_chunk, ext_pcs, out):
        im = self.im
        lit_words = set()
        for i, x in enumerate(f.words):
            if f.states[i] is not None and (x & 0x3b000000) == 0x18000000 \
                    and (x & 0xff000000) != 0xd8000000:
                t = f.lo + 4 * i + sx((x >> 5) & 0x7ffff, 19) * 4
                if not f.lo <= t < f.hi:
                    die("%s: LDR literal at %#x leaves function" % (f.name, f.lo + 4 * i))
                lit_words.add(t)
        lo12 = {}
        for e in f.events:
            if e[0] == "lo12":
                lo12[e[1]] = e[2]
        out.append("\t.section .text.hrsblob.%s,\"ax\",%%progbits" % f.name)
        out.append("\t.balign 4")
        out.append("\t.globl %s" % f.name)
        out.append("\t.type %s,%%function" % f.name)
        out.append("%s:" % f.name)
        for i, x in enumerate(f.words):
            pc = f.lo + 4 * i
            reach = f.states[i] is not None
            line = None
            if not reach:
                if pc not in lit_words and (
                        (x & 0x9f000000) in (0x90000000, 0x10000000) or
                        (x & 0x7c000000) == 0x14000000 or
                        (x & 0xff000010) == 0x54000000 or
                        (x & 0x7e000000) in (0x34000000, 0x36000000)):
                    die("%s: unreachable PC-relative word at %#x" % (f.name, pc))
                out.append("\t.inst %#010x" % x)
                continue
            if (x & 0x9f000000) == 0x90000000:                       # ADRP
                imm = sx((((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3), 21)
                page = (pc & ~0xfff) + (imm << 12)
                if pc in ext_pcs:
                    es = ext_pcs[pc]
                    self.ext_pcs_by_func.setdefault(f.name, {})[pc] = es
                    if len(es) != 1:
                        die("%s: ADRP %#x feeds several external vars" % (f.name, pc))
                    if page != (next(iter(es)) & ~0xfff):
                        die("ADRP page mismatch")
                    self.cnt["adrp->external var"] += 1
                    line = "adrp x%d, %s" % (x & 31, self.ext_addr[next(iter(es))])
                    self.undef[self.ext_addr[next(iter(es))]] = next(iter(es))
                else:
                    c = adrp_chunk.get(pc)
                    if c is None:
                        c = self.page_chunk(page)
                        self.fallback.append((f.name, pc, page))
                    if c is None:
                        die("%s: ADRP %#x (page %#x) has no tracked use and no chunk"
                            % (f.name, pc, page))
                    self.cnt["adrp->chunk"] += 1
                    line = "adrp x%d, %s+%#x" % (x & 31, c.sym, page - c.base) \
                        if page >= c.base else \
                        "adrp x%d, %s-%#x" % (x & 31, c.sym, c.base - page)
            elif (x & 0xfc000000) in (0x94000000, 0x14000000):       # BL / B
                t = pc + sx(x & 0x3ffffff, 26) * 4
                is_bl = (x >> 31) & 1
                if not is_bl and f.lo <= t < f.hi:
                    self.cnt["b local"] += 1
                    line = None
                else:
                    self.cnt["bl" if is_bl else "b tail call"] += 1
                    line = "%s %s" % ("bl" if is_bl else "b", self.code_sym(t, f.name))
            elif (x & 0xff000010) == 0x54000000:
                self.cnt["b.cond local"] += 1
                self.local_check(f, pc, pc + sx((x >> 5) & 0x7ffff, 19) * 4)
            elif (x & 0x7e000000) == 0x34000000:
                self.cnt["cbz/cbnz local"] += 1
                self.local_check(f, pc, pc + sx((x >> 5) & 0x7ffff, 19) * 4)
            elif (x & 0x7e000000) == 0x36000000:
                self.cnt["tbz/tbnz local"] += 1
                self.local_check(f, pc, pc + sx((x >> 5) & 0x3fff, 14) * 4)
            elif (x & 0x9f000000) == 0x10000000:
                self.cnt["adr local"] += 1
                imm = sx((((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3), 21)
                self.local_check(f, pc, pc + imm)
            elif (x & 0x3b000000) == 0x18000000:
                self.cnt["ldr literal local"] += 1
            elif pc in lo12 and lo12[pc] in self.ext_addr:
                A = lo12[pc]
                if (x & 0xffc00000) != 0xf9400000 or ((x >> 10) & 0xfff) << 3 != (A & 0xfff):
                    die("%s: external var access at %#x is not LDR Xt,[Xn,#imm]" % (f.name, pc))
                self.cnt["ldr lo12 -> external var"] += 1
                line = "ldr x%d, [x%d, :lo12:%s]" % (x & 31, rn(x), self.ext_addr[A])
            elif (x & 0xfffffc1f) in (0xd63f0000, 0xd61f0000, 0xd65f0000):
                if (x & 0xfffffc1f) == 0xd63f0000:
                    self.indirect.append((f.name, pc, "blr"))
                elif (x & 0xfffffc1f) == 0xd61f0000:
                    self.cnt["br (jump table)"] += 1
            if line is None:
                out.append("\t.inst %#010x" % x)
            else:
                out.append("\t%-28s// %#x %08x" % (line, pc, x))
        out.append("\t.size %s, .-%s" % (f.name, f.name))
        out.append("")

    def local_check(self, f, pc, t):
        if not f.lo <= t < f.hi or t & 3:
            die("%s: PC-relative reference at %#x leaves the function (%#x)"
                % (f.name, pc, t))

    def page_chunk(self, page):
        for c in self.chunks:
            if c.base <= page < c.hi:
                return c
        return None

    # -- data
    def emit_chunk(self, c, out):
        im = self.im
        pad = c.lo & (PAGE - 1)
        flags = {"ro": '"a",%progbits', "data": '"aw",%progbits',
                 "bss": '"aw",%nobits'}[c.cls]
        out.append("\t.section .hrsblob.%s.%s,%s" % (c.cls, c.sym, flags))
        out.append("\t.balign 4096")
        out.append("%s:" % c.sym)
        if c.cls == "bss":
            out.append("\t.skip %d" % (pad + c.size))
            out.append("")
            return
        if pad:
            out.append("\t.skip %d" % pad)
        raw = im.raw[c.lo:c.hi]
        c.nptr = 0
        i = 0
        n = len(raw)
        # pointer slots: 8-aligned qwords holding link-time kernel VAs
        self.check_unaligned_ptrs(c)
        while i < n:
            a = c.lo + i
            if a % 8 == 0 and i + 8 <= n:
                pv = struct.unpack_from("<Q", raw, i)[0]
                if 0 <= pv - im.text_va < (1 << 25):
                    t = pv - im.text_va
                    if self.chunk_at(t):
                        ex = self.data_ref(t, "pointer at %#x" % a)
                    elif self.fn_by_lo.get(t) or im.sym_at(t):
                        ex = self.code_sym(t, "pointer at %#x" % a)
                    else:
                        die("pointer at %#x -> %#x: unknown target" % (a, t))
                    out.append("\t.quad %s\t// %#x -> %#x" % (ex, a, t))
                    c.nptr += 1
                    i += 8
                    continue
            j = min(n, i + 16, i + (8 - a % 8) if a % 8 else i + 16)
            out.append("\t.byte " + ",".join("%#04x" % b for b in raw[i:j]))
            i = j
        out.append("")

    def check_unaligned_ptrs(self, c):
        im = self.im
        raw = im.raw[c.lo:c.hi]
        for k in range(0, len(raw) - 7):
            a = c.lo + k
            if a % 8:
                pv = struct.unpack_from("<Q", raw, k)[0]
                if 0 <= pv - im.text_va < (1 << 25) and pv >> 32 == im.text_va >> 32:
                    die("possible unaligned pointer at %#x" % a)


# -------------------------------------------------------------- orchestration
def read_elf_sections(path):
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstr = struct.unpack_from("<HHH", d, 0x3a)
    secs = []
    for i in range(shnum):
        nm, ty, fl, addr, off, size = struct.unpack_from("<IIQQQQ", d, shoff + i * shentsize)
        secs.append((nm, ty, fl, addr, off, size))
    stroff = secs[shstr][4]
    out = {}
    for nm, ty, fl, addr, off, size in secs:
        e = d.index(b"\0", stroff + nm)
        out[d[stroff + nm:e].decode()] = (ty, addr, off, size)
    return d, out


def run(args):
    raw = load_kernel(args.boot_img)
    im = Image(raw, args.text_va)
    funcs, gaps = build_funcs(im)
    ext_addr = {}
    for n in EXTERNAL_VARS:
        a = [x for x in im.by_name[n] if True]
        if len(a) != 1:
            die("external var %s not unique" % n)
        ext_addr[a[0]] = n
    chunks, by_adrp, ext_pcs, slots = build_chunks(im, funcs, ext_addr)
    adrp_chunk = {}
    for c in chunks:
        for r in c.rs:
            for p in r[4]:
                if p in adrp_chunk and adrp_chunk[p] is not c:
                    die("ADRP %#x feeds two chunks" % p)
                adrp_chunk[p] = c
    em = Emit(im, funcs, chunks, ext_addr, slots)
    out = ["// generated by blobcarve; stock offsets are relative to _text",
           "\t.arch armv8-a", ""]
    for f in funcs:
        em.emit_function(f, adrp_chunk, ext_pcs, out)
    for c in chunks:
        em.emit_chunk(c, out)
    for name in EXPORT_DATA:
        a = im.by_name[name]
        if len(a) != 1:
            die("export %s not unique" % name)
        out.append("\t.globl hrsblob_%s\n\t.set hrsblob_%s, %s" % (
            name, name, em.data_ref(a[0], name)))
    os.makedirs(args.out, exist_ok=True)
    asm = os.path.join(args.out, "hrsblob.S")
    open(asm, "w").write("\n".join(out) + "\n")
    # linker script placing everything at stock VAs
    ld = ["SECTIONS {"]
    for f in funcs:
        ld.append("  .text.hrsblob.%s %#x : { *(.text.hrsblob.%s) }"
                  % (f.name, im.text_va + f.lo, f.name))
    for c in chunks:
        sec = ".hrsblob.%s.%s" % (c.cls, c.sym)
        ld.append("  %s %#x : { *(%s) }" % (sec, im.text_va + c.base, sec))
    ld.append("  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.ARM.attributes) }")
    ld.append("}")
    for n, a in sorted(em.undef.items(), key=lambda t: t[1]):
        ld.append("%s = %#x;" % (n, im.text_va + a))
    ldp = os.path.join(args.out, "roundtrip.ld")
    open(ldp, "w").write("\n".join(ld) + "\n")
    report = make_report(im, funcs, chunks, em, gaps, slots)
    if args.cross:
        verify(args, im, funcs, chunks, em, asm, ldp, report)
    open(os.path.join(args.out, "report.txt"), "w").write("\n".join(report) + "\n")
    print("\n".join(report))


EXPORT_DATA = ("hrs3xxx_t", "alg_ram", "spo2_alg_data")


def make_report(im, funcs, chunks, em, gaps, slots):
    R = []
    R.append("stock addresses are offsets from _text (VA = %#x + offset)" % im.text_va)
    R.append("")
    R.append("== carved code: %d functions, %d..%d, gaps (excluded): %s" % (
        len(funcs), CARVE_LO, CARVE_HI,
        ", ".join("%#x-%#x" % g for g in gaps)))
    R.append("")
    R.append("== chunks (lo..hi are stock offsets; static_end = from direct "
             "displacement analysis, end = after object rounding)")
    R.append("%-4s %-4s %-9s %-9s %-9s %6s %6s %5s  %s" % (
        "idx", "cls", "lo", "hi", "static_hi", "size", "ssize", "ptrs", "symbols"))
    for c in chunks:
        names = [n for a, t, n in im.ks if c.lo <= a < c.hi and t in DATA_TYPES]
        sh = ",".join(names[:4]) + ("..(%d)" % len(names) if len(names) > 4 else "")
        R.append("%-4d %-4s %-9x %-9x %-9x %6d %6d %5d  %s" % (
            c.idx, c.cls, c.lo, c.hi, c.static_hi, c.size, c.static_hi - c.lo,
            getattr(c, "nptr", 0), sh))
    R.append("")
    R.append("== pointer slots folded by dataflow (slot -> target): %s" % ", ".join(
        "%#x->%#x" % kv for kv in sorted(slots.items())))
    R.append("")
    R.append("== defined entry points (global, stock offset)")
    for f in funcs:
        R.append("  %-40s %#x  size %d" % (f.name, f.lo, f.hi - f.lo))
    R.append("")
    R.append("== undefined symbols (stock offset)")
    for n, a in sorted(em.undef.items(), key=lambda t: t[1]):
        R.append("  %-40s %#x" % (n, a))
    R.append("")
    R.append("== instruction classes with PC-relative or absolute addressing in carved code")
    for k, v in sorted(em.cnt.items()):
        R.append("  %-28s %d" % (k, v))
    R.append("  ADRPs resolved by page fallback (no tracked use): %d %s" % (
        len(em.fallback), em.fallback[:6]))
    R.append("  indirect calls (blr, no static target): %d %s" % (
        len(em.indirect), sorted({n for n, _, _ in em.indirect})))
    R.append("")
    R.append("== jump tables")
    for f in funcs:
        for br, s in f.switches.items():
            R.append("  %-34s br %#x table %#x n=%d esize=%d" % (
                f.name, br, s["A"], s["n"], s["esz"]))
    return R


def tool(prefix, name):
    return prefix + name


def sh(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode:
        die("command failed: %s\n%s" % (" ".join(cmd), p.stdout))
    return p.stdout


def verify(args, im, funcs, chunks, em, asm, ldp, R):
    obj = os.path.join(args.out, "hrsblob.o")
    elf = os.path.join(args.out, "roundtrip.elf")
    sh([tool(args.cross, "as"), "-o", obj, asm])
    sh([tool(args.cross, "ld"), "-nostdlib", "-e", "0", "--build-id=none",
        "-z", "max-page-size=4096", "-T", ldp, "-o", elf, obj])
    und = sorted(l.split()[-1] for l in sh([tool(args.cross, "objdump"), "-t", obj]).splitlines()
                 if "*UND*" in l)
    if und != sorted(em.undef):
        die("undefined symbol set mismatch: objdump %s vs emitter %s" % (und, sorted(em.undef)))
    d, secs = read_elf_sections(elf)
    bad = 0
    code_bytes = 0
    data_bytes = 0
    for f in funcs:
        ty, addr, off, size = secs[".text.hrsblob.%s" % f.name]
        want = im.raw[f.lo:f.hi]
        if addr != im.text_va + f.lo or d[off:off + size] != want:
            R.append("MISMATCH code %s" % f.name)
            bad += 1
        code_bytes += size
    for c in chunks:
        sec = ".hrsblob.%s.%s" % (c.cls, c.sym)
        ty, addr, off, size = secs[sec]
        pad = c.lo & (PAGE - 1)
        if addr != im.text_va + c.base or size != pad + c.size:
            R.append("MISMATCH chunk geometry %s" % c.sym)
            bad += 1
        elif c.cls != "bss":
            if d[off + pad:off + pad + c.size] != im.raw[c.lo:c.hi]:
                R.append("MISMATCH chunk bytes %s" % c.sym)
                bad += 1
            data_bytes += c.size
    shift_test(args, im, funcs, chunks, em, obj, ldp, R)
    R.append("")
    R.append("== round trip (linked at stock VAs vs stock Image bytes)")
    R.append("code: %d functions, %d bytes compared" % (len(funcs), code_bytes))
    R.append("data: %d copied (ro/data) bytes compared; bss chunks checked for "
             "address and size only" % data_bytes)
    R.append("undefined set (objdump) == emitter set: %d symbols" % len(und))
    R.append("RESULT: %s" % ("BYTE-IDENTICAL" if not bad else "%d MISMATCHES" % bad))
    if bad:
        die("round trip failed")


def shift_test(args, im, funcs, chunks, em, obj, ldp, R):
    """Relink with chunks moved +CS and the external variable moved +ES
    (non page-multiple) and check that only the intended words changed, by
    exactly the right amount.  Proves the carve is layout independent."""
    CS, ES = 0x40000, 0x1238
    txt = open(ldp).read().splitlines()
    out = []
    for l in txt:
        t = l.split()
        if t and t[0].startswith(".hrsblob."):
            l = "  %s %#x : %s" % (t[0], int(t[1], 0) + CS, " ".join(t[3:]))
        elif t and t[0] in em.ext_addr.values():
            l = "%s = %#x;" % (t[0], int(t[2].rstrip(";"), 0) + ES)
        out.append(l)
    p2 = os.path.join(args.out, "shifted.ld")
    open(p2, "w").write("\n".join(out) + "\n")
    elf2 = os.path.join(args.out, "shifted.elf")
    sh([tool(args.cross, "ld"), "-nostdlib", "-e", "0", "--build-id=none",
        "-z", "max-page-size=4096", "-T", p2, "-o", elf2, obj])
    d, secs = read_elf_sections(elf2)
    ext_by_pc = {}
    for f in funcs:
        for e in f.events:
            if e[0] == "lo12" and e[2] in em.ext_addr:
                ext_by_pc[e[1]] = e[2]
    n_adrp = n_ext_adrp = n_ldr = n_same = n_ptr = 0
    for f in funcs:
        ty, addr, off, size = secs[".text.hrsblob.%s" % f.name]
        for i in range(len(f.words)):
            old = f.words[i]
            new = struct.unpack_from("<I", d, off + 4 * i)[0]
            pc = f.lo + 4 * i
            if f.states[i] is None:
                if old != new:
                    die("shift test: data word changed at %#x" % pc)
                continue
            if (old & 0x9f000000) == 0x90000000:
                def pg(x):
                    imm = sx((((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3), 21)
                    return (pc & ~0xfff) + (imm << 12)
                is_ext = pc in em_ext_adrps(f, em)
                want = pg(old) + (ES & ~0xfff if is_ext else CS)
                if is_ext:
                    # new page = page of (var + ES)
                    var = next(iter(em_ext_adrps(f, em)[pc]))
                    want = (var + ES) & ~0xfff
                    n_ext_adrp += 1
                else:
                    n_adrp += 1
                if pg(new) != want or (new & 31) != (old & 31):
                    die("shift test: ADRP at %#x -> %#x, expected %#x" % (pc, pg(new), want))
            elif pc in ext_by_pc:
                var = ext_by_pc[pc]
                if ((new >> 10) & 0xfff) << 3 != (var + ES) & 0xfff or \
                        (new & ~(0xfff << 10)) != (old & ~(0xfff << 10)):
                    die("shift test: external LDR at %#x wrong" % pc)
                n_ldr += 1
            else:
                if old != new:
                    die("shift test: word at %#x changed unexpectedly" % pc)
                n_same += 1
    for c in chunks:
        sec = ".hrsblob.%s.%s" % (c.cls, c.sym)
        ty, addr, off, size = secs[sec]
        if addr != im.text_va + c.base + CS:
            die("shift test: chunk placement")
        if c.cls != "bss":
            pad = c.lo & (PAGE - 1)
            for k in range(0, c.size, 8):
                if (c.lo + k) % 8:
                    continue
                oldq = struct.unpack_from("<Q", im.raw, c.lo + k)[0]
                if 0 <= oldq - im.text_va < (1 << 25):
                    newq = struct.unpack_from("<Q", d, off + pad + k)[0]
                    if newq != oldq + CS:
                        die("shift test: pointer slot at %#x not relocated" % (c.lo + k))
                    n_ptr += 1
    R.append("")
    R.append("== layout-independence test (chunks +%#x, external var +%#x)" % (CS, ES))
    R.append("%d chunk ADRPs shifted exactly, %d external ADRPs and %d external "
             "LDR lo12 re-encoded exactly, %d data pointer slots relocated, "
             "%d other words unchanged: PASS"
             % (n_adrp, n_ext_adrp, n_ldr, n_ptr, n_same))


def em_ext_adrps(f, em):
    return em.ext_pcs_by_func.get(f.name, {})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--boot-img", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cross", help="toolchain prefix, e.g. /path/aarch64-linux-gnu-")
    ap.add_argument("--text-va", type=lambda v: int(v, 0), default=TEXT_VA_DEFAULT)
    args = ap.parse_args()
    try:
        run(args)
    except Fatal as e:
        print("blobcarve: FATAL: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
