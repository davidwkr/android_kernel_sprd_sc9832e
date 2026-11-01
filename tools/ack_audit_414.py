#!/usr/bin/env python3
"""Audit aosp/android-4.14-stable Android-only commits against this tree.

Usage: python3 -I tools/ack_audit_414.py OUT.tsv --config PATH/.config
                                          [--aosp REF] [--stable TAG] [--msgs N]

Read-only: git is called with argument lists (no shell); only OUT.tsv is written.
Candidates: non-merge commits reachable from --aosp but not from --stable whose
subject starts with ANDROID/UPSTREAM/BACKPORT/FROMLIST/FROMGIT/CHROMIUM/
FROMDT (also inside Revert "...").

Status (first match wins):
  PRESENT         cherry-pick sha or normalized subject found in our commit
                  messages, or every hunk of the diff is already applied
  PARTIAL         some hunks applied, some not (or unclear)
  MISSING         no hunk applied
  NOT-APPLICABLE  none of the touched files exist here, docs/config/other-arch
                  only, or a revert of something we do not have
Hunk test (no shared history, so content based): the whitespace-stripped,
blank-free post-image (context + added) appears contiguously in the HEAD file
=> applied; else the pre-image (context + removed) appears => not applied; else
unknown, decided by the fraction of added lines (len>=12) present in the file.
Config gating: each .c/.S file is resolved through obj-$(CONFIG_*) chains
using the given .config (built y / n / ? unresolved).
"""
import argparse, os, re, subprocess, sys
from collections import Counter

PREFIX = r'(?:ANDROID|UPSTREAM|BACKPORT|FROMLIST|FROMGIT|CHROMIUM|FROMDT)'
SEL = re.compile(r'^(?:Revert ")?\s*' + PREFIX + r'\b', re.I)


def git(args, inp=None, check=True):
    r = subprocess.run(['git'] + args, capture_output=True, input=inp)
    if check and r.returncode != 0:
        sys.exit('git %s failed: %s' % (' '.join(args[:3]), r.stderr[:200]))
    return r


def out(args, **kw):
    return git(args, **kw).stdout.decode('utf-8', 'replace')


def norm(s):
    s = re.sub(r'^Revert "(.*)"$', r'revert \1', s)
    s = re.sub(r'^(revert )?\s*(?:(?:ANDROID|UPSTREAM|BACKPORT|FROMLIST|FROMGIT|'
               r'CHROMIUM|FROMDT|RFC)[,:]?\s*)+', r'\1', s, flags=re.I)
    s = re.sub(r'\[\s*backport:.*?\]', '', s, flags=re.I)
    return re.sub(r'[^a-z0-9]+', ' ', s.lower()).strip()


class Gate:
    def __init__(self, cfgpath):
        self.cfg = {}
        for l in open(cfgpath, errors='replace'):
            m = re.match(r'(CONFIG_\w+)=(\S+)', l)
            if m:
                self.cfg[m.group(1)] = m.group(2)
        self.mk = {}

    def parse(self, d):
        if d in self.mk:
            return self.mk[d]
        txt = ''
        for n in ('Makefile', 'Kbuild'):
            r = git(['show', 'HEAD:%s/%s' % (d, n) if d else 'HEAD:' + n], check=False)
            if r.returncode == 0:
                txt += r.stdout.decode('utf-8', 'replace') + '\n'
        txt = re.sub(r'\\\n', ' ', txt)
        ents, stack = [], []   # stack of booleans: is the ifeq/ifdef branch active
        for l in txt.splitlines():
            c = re.match(r'\s*(ifeq|ifneq|ifdef|ifndef)\b\s*\(?\s*(.*?)\)?\s*$', l)
            if c:
                stack.append(self.cond(c.group(1), c.group(2)))
                continue
            if re.match(r'\s*else\b', l):
                if stack:
                    stack[-1] = not stack[-1] if stack[-1] is not None else None
                continue
            if re.match(r'\s*endif\b', l):
                if stack:
                    stack.pop()
                continue
            if any(x is False for x in stack):
                continue
            m = re.match(r'\s*([\w\-\$\(\)]+?)\s*[:+]?=\s*(.*)', l)
            if m:
                ents.append((m.group(1), m.group(2).split('#')[0].split()))
        self.mk[d] = ents
        return ents

    def cond(self, kw, arg):
        """True/False for simple CONFIG_ conditionals, None when unknown (kept active)."""
        m = re.match(r'\$\((CONFIG_\w+)\)\s*,\s*(\w*)$', arg)
        if kw in ('ifeq', 'ifneq') and m:
            r = self.cfg.get(m.group(1), '') == m.group(2)
            return r if kw == 'ifeq' else not r
        m = re.match(r'(CONFIG_\w+)$', arg)
        if kw in ('ifdef', 'ifndef') and m:
            r = self.cfg.get(m.group(1), 'n') != 'n'
            return r if kw == 'ifdef' else not r
        return None

    def val(self, var):
        m = re.match(r'obj-\$\((CONFIG_\w+)\)$', var)
        if m:
            return self.cfg.get(m.group(1), 'n') in ('y', 'm')
        return var in ('obj-y', 'always', 'extra-y') or None

    def built(self, path):
        d, f = os.path.split(path)
        return self._obj(d, os.path.splitext(f)[0] + '.o')

    def _obj(self, d, tok, seen=()):
        if (d, tok) in seen:
            return None
        seen = seen + ((d, tok),)
        ents = self.parse(d)
        found, res = False, False
        for var, toks in ents:
            if tok in toks and var.startswith('obj-'):
                v = self.val(var)
                if v is None:
                    return None
                found = True
                res = res or v
        if not found:
            for var, toks in ents:
                m = re.match(r'([\w\-]+?)-(?:objs|y|\$\((CONFIG_\w+)\))$', var)
                if m and tok in toks:
                    if m.group(2) and self.cfg.get(m.group(2), 'n') not in ('y', 'm'):
                        continue
                    if self._obj(d, m.group(1) + '.o', seen):
                        return True
                    found = True
            return False if found else None
        return self._dir(d) if res else False

    def _dir(self, d):
        while d:
            p, n = os.path.split(d)
            ok = None
            for var, toks in self.parse(p):
                if var.startswith('obj-') and (n + '/') in toks:
                    v = self.val(var)
                    ok = v if ok is None else (ok or v)
            if ok is False:
                return False
            d = p
        return True


CVE_RE = re.compile(r'CVE-\d{4}-\d{4,}')
STRONG = re.compile(r'security|vulnerab|exploit|privilege|use[- ]after[- ]free|\buaf\b|'
                    r'out[- ]of[- ]bounds|\boob\b|overflow|double[- ]free|info(?:rmation)?'
                    r'[- ]?leak|disclos|harden|sanitiz|pointer leak|restrict|validate|'
                    r'sanity check|strong ref|malicious|unprivileged|permission|spectre|'
                    r'meltdown|\bcve\b', re.I)
FIXW = re.compile(r'\bfix|avoid|prevent|crash|panic|null|leak|race|deadlock|oops|\bbug\b|'
                  r'correct|missing|wrong|don.t|do not|uninit|bounds|validate|check|'
                  r'handle|corrupt|hang|lock', re.I)
AREAS = [('binder', ('drivers/android/binder',)),
         ('ion', ('drivers/staging/android/ion', 'drivers/dma-buf')),
         ('f2fs', ('fs/f2fs',)),
         ('fscrypt', ('fs/crypto', 'fscrypt', 'fs/ext4/crypto')),
         ('selinux', ('security/selinux',)),
         ('netfilter/qtaguid/bpf', ('net/netfilter', 'netfilter', 'qtaguid', 'kernel/bpf',
                                     'net/core/filter', 'net/ipv4/netfilter',
                                     'net/ipv6/netfilter', 'net/xfrm')),
         ('usb-gadget', ('drivers/usb/gadget', 'drivers/usb/')),
         ('sched/cpufreq', ('kernel/sched', 'drivers/cpufreq', 'cpufreq', 'kernel/cgroup',
                            'uid_sys_stats', 'cpufreq_times', 'kernel/power')),
         ('mm', ('mm/',)),
         ('arm64', ('arch/arm64', 'arch/arm/')),
         ('fs-other', ('fs/',)),
         ('net-other', ('net/',)),
         ('drivers-other', ('drivers/',)),
         ('other', ('',))]
OTHER_ARCH = re.compile(r'^arch/(?!arm/|arm64/)')
NONCODE = re.compile(r'^(Documentation/|build\.config|android/configs/|arch/[a-z0-9]+/configs/|'
                     r'.*\.(md|txt|rst)$|MAINTAINERS$)')


def area_of(files, subj):
    for name, keys in AREAS:
        for f in files:
            if any(k in f for k in keys if k):
                return name
        if name == 'other':
            return name
    return 'other'


_norm = {}


def head_norm(path):
    """Whitespace-stripped, blank-free HEAD file as one string, or None if absent."""
    if path not in _norm:
        r = git(['show', 'HEAD:' + path], check=False)
        if r.returncode:
            _norm[path] = None
        else:
            ls = [l.strip() for l in r.stdout.decode('utf-8', 'replace').splitlines()]
            _norm[path] = '\n' + '\n'.join(l for l in ls if l) + '\n'
    return _norm[path]


def hunks(diff):
    """Yield (path, deleted, pre[], post[], added[]) per hunk; stripped, blank-free."""
    path, deleted, cur = None, False, None
    for l in diff.splitlines():
        if l.startswith('diff --git '):
            if cur:
                yield cur
            cur, path, deleted = None, None, False
        elif l.startswith('deleted file mode'):
            deleted = True
        elif l.startswith('+++ '):
            if l[4:] != '/dev/null':
                path = l[6:]
        elif l.startswith('--- a/') and path is None:
            path = l[6:]
        elif l.startswith('@@'):
            if cur:
                yield cur
            cur = [path, deleted, [], [], []]
        elif cur is not None and l[:1] in (' ', '+', '-'):
            t = l[1:].strip()
            if not t:
                continue
            if l[0] in (' ', '-'):
                cur[2].append(t)
            if l[0] in (' ', '+'):
                cur[3].append(t)
            if l[0] == '+':
                cur[4].append(t)
    if cur:
        yield cur


def judge(diff, deleted_paths):
    """Return (status, applied, notapplied, unknown) over the diff hunks."""
    ap = na = un = 0
    for path, deleted, pre, post, added in hunks(diff):
        txt = head_norm(path) if path else None
        if path is None:
            continue
        if deleted:
            if txt is None:
                ap += 1
            else:
                na += 1
            continue
        if txt is None:
            na += 1
            continue
        if post and ('\n' + '\n'.join(post) + '\n') in txt:
            ap += 1
        elif pre and ('\n' + '\n'.join(pre) + '\n') in txt:
            na += 1
        else:
            seen = {a for a in added if len(a) >= 12}
            if len(seen) >= 2 and sum(('\n' + a + '\n') in txt for a in seen) / len(seen) >= 0.8:
                ap += 1
            elif seen and not any(('\n' + a + '\n') in txt for a in seen):
                na += 1
            else:
                un += 1
    return ap, na, un


def classify(sha, subj, msg, live, bstat):
    cves = ','.join(sorted(set(CVE_RE.findall(msg))))
    text = subj + '\n' + msg
    strong = bool(STRONG.search(text))
    fixw = bool(FIXW.search(subj)) or 'Fixes:' in msg
    sens = area_of(live, subj) not in ('other', 'drivers-other', 'arm64', 'net-other', 'fs-other')
    prio = (5 if cves else 0) + (3 if strong else 0) + (2 if sens else 0) + \
           (2 if bstat == 'y' else 1 if bstat == '?' else 0) + (1 if fixw else 0)
    sec = bool(cves) or (strong and (fixw or sens))
    if bstat == 'n':
        return 'b', prio, cves, 'unbuilt in this config' + (' [security-ish]' if sec else '')
    if sec:
        return 'a', prio, cves, 'CVE' if cves else 'security wording'
    if fixw:
        return 'c', prio, cves, 'bugfix'
    return 'b', prio, cves, 'feature/infra'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--config', required=True)
    ap.add_argument('--aosp', default='aosp/android-4.14-stable')
    ap.add_argument('--stable', default='v4.14.336')
    ap.add_argument('--msgs', type=int, default=4000,
                    help='how many recent HEAD commit messages to scan for shas/subjects')
    a = ap.parse_args()
    cands = []
    for l in out(['log', '--no-merges', '--format=%H%x00%s', a.aosp, '^' + a.stable]).splitlines():
        h, _, s = l.partition('\0')
        if SEL.match(s):
            cands.append((h, s))
    ours_files = set(out(['ls-tree', '-r', '--name-only', 'HEAD']).splitlines())
    msgs = out(['log', '--no-merges', '-n', str(a.msgs), '--format=%B%x01', 'HEAD'])
    shas = set(re.findall(r'\b[0-9a-f]{40}\b', msgs))
    subj = set()
    for l in msgs.splitlines():
        l = l.strip()
        subj.add(norm(l[2:] if l.startswith('- ') else l))
    subj = {s for s in subj if len(s) >= 15}
    gate = Gate(a.config)
    rows = []
    for h, s in cands:
        msg = out(['log', '-1', '--format=%B', h])
        fl = [l for l in out(['diff-tree', '--no-commit-id', '--name-only', '-r', '-m',
                              '--first-parent', h]).splitlines() if l]
        live = [f for f in fl if f in ours_files and not OTHER_ARCH.match(f) and not NONCODE.match(f)]
        cs = set(re.findall(r'cherry picked from commit ([0-9a-f]{40})', msg))
        st, ev, cl, prio, bstat, cves, why = '', '', '-', 0, '-', '', ''
        if cs & shas:
            st, ev = 'PRESENT', 'cherrysha'
        elif len(norm(s)) >= 15 and norm(s) in subj:
            st, ev = 'PRESENT', 'subject'
        elif not fl or not any(f in ours_files for f in fl):
            st, why = 'NOT-APPLICABLE', 'touched files absent here'
        elif not live:
            st, why = 'NOT-APPLICABLE', 'docs/config/other-arch only'
        else:
            diff = git(['diff-tree', '-p', '--no-commit-id', '--no-color', '-U3', h]).stdout
            ap_, na_, un_ = judge(diff.decode('utf-8', 'replace'), ())
            if ap_ and not na_ and not un_:
                st, ev = 'PRESENT', 'applied=%d notapplied=0 unknown=0' % ap_
            elif ap_ and un_ and not na_:
                st, ev = 'PRESENT', 'applied=%d notapplied=0 unknown=%d' % (ap_, un_)
            elif ap_:
                st, ev = 'PARTIAL', 'applied=%d notapplied=%d unknown=%d' % (ap_, na_, un_)
            elif na_ or un_:
                st, ev = 'MISSING', 'applied=0 notapplied=%d unknown=%d' % (na_, un_)
            else:
                st, ev = 'PARTIAL', 'no text hunks'
            if s.startswith('Revert') and st == 'MISSING':
                st, why = 'NOT-APPLICABLE', 'revert of a change we lack'
        if st in ('MISSING', 'PARTIAL'):
            built = []
            for f in live:
                if f.endswith(('.c', '.S')):
                    built.append(gate.built(f))
            bstat = 'y' if any(b is True for b in built) else \
                    'n' if built and all(b is False for b in built) else '?'
            cl, prio, cves, why = classify(h, s, msg, live, bstat)
        rows.append([h[:12], st, cl, str(prio), bstat, area_of(live or fl, s), cves, why, ev,
                     str(len(fl)), s.replace('\t', ' '), ';'.join(fl[:8])])
    hdr = ['aosp', 'status', 'class', 'prio', 'built', 'area', 'cve', 'reason', 'evidence',
           'nfiles', 'subject', 'files']
    with open(a.out, 'w') as f:
        f.write('\t'.join(hdr) + '\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    print(len(rows), 'candidates;', dict(Counter(r[1] for r in rows)), file=sys.stderr)


if __name__ == '__main__':
    main()
