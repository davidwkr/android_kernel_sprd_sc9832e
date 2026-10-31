#!/usr/bin/env python3
"""Audit aosp/android-4.4-p Android-specific commits against this tree.

Usage: python3 -I tools/ack_audit.py OUT.tsv [--config PATH/.config]
                                       [--aosp REF] [--base REF] [--tmp DIR]

Read-only: git is called with argument lists (no shell).  The only write
besides OUT.tsv is a throw-away index file under --tmp (default: a mkdtemp in
~/.cache), used for `git apply --cached --check` and removed on exit.

Stage 1 (status): PATCHID | CHERRYSHA | SUBJECT | REVAPPLIED | NOFILES | MISSING
Stage 2 (class, MISSING only): a security | b feature/infra/unbuilt |
                               c bugfix in built code | d not applicable
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

PREFIX = r'(?:ANDROID|UPSTREAM|BACKPORT|FROMLIST|FROMGIT)'
SEL = re.compile(r'^(?:Revert ")?' + PREFIX + ':')


def git(args, env=None, inp=None, check=True):
    r = subprocess.run(['git'] + args, capture_output=True, env=env,
                       input=inp, text=False)
    if check and r.returncode != 0:
        sys.exit('git %s failed: %s' % (' '.join(args[:3]), r.stderr[:200]))
    return r


def out(args, **kw):
    return git(args, **kw).stdout.decode('utf-8', 'replace')


def norm(s):
    s = re.sub(r'^Revert "(.*)"$', r'revert \1', s)
    s = re.sub(r'^(revert )?\s*(?:(?:ANDROID|UPSTREAM|BACKPORT|FROMLIST|FROMGIT|RFC)'
               r'[,:]?\s*)+', r'\1', s, flags=re.I)
    s = re.sub(r'\[\s*backport:.*?\]', '', s, flags=re.I)
    return re.sub(r'[^a-z0-9]+', ' ', s.lower()).strip()


def patch_id(sha):
    p = git(['show', '--format=', '--no-color', sha]).stdout
    r = git(['patch-id', '--stable'], inp=p).stdout.decode().split()
    return r[0] if r else None


def commit_list(rng):
    o = out(['log', '--no-merges', '--format=%H%x00%s'] + rng)
    return [l.split('\0', 1) for l in o.splitlines() if '\0' in l]


def load_candidates(aosp):
    res = []
    for h, s in commit_list([aosp, '^' + aosp + '^2']):
        if SEL.match(s):
            res.append((h, s))
    return res


def files_of(sha):
    o = out(['diff-tree', '--no-commit-id', '--name-only', '-r', '-m',
             '--first-parent', sha])
    return [l for l in o.splitlines() if l]


def body(sha):
    return out(['log', '-1', '--format=%B', sha])


# ---------- Kconfig gate resolution (no build) ----------
class Gate:
    def __init__(self, cfgpath, root):
        self.cfg = {}
        for l in open(cfgpath, errors='replace'):
            m = re.match(r'(CONFIG_\w+)=(\S+)', l)
            if m:
                self.cfg[m.group(1)] = m.group(2)
        self.root = root
        self.mk = {}

    def parse(self, d):
        if d in self.mk:
            return self.mk[d]
        txt = ''
        for n in ('Makefile', 'Kbuild'):
            r = git(['show', 'HEAD:%s/%s' % (d, n) if d else 'HEAD:' + n],
                    check=False)
            if r.returncode == 0:
                txt += r.stdout.decode('utf-8', 'replace') + '\n'
        txt = re.sub(r'\\\n', ' ', txt)
        ents = []  # (varname, op, [tokens])
        for l in txt.splitlines():
            m = re.match(r'\s*([\w\-\$\(\)]+?)\s*[:+]?=\s*(.*)', l)
            if m:
                ents.append((m.group(1), m.group(2).split('#')[0].split()))
        self.mk[d] = ents
        return ents

    def val(self, var):
        m = re.match(r'obj-\$\((CONFIG_\w+)\)$', var)
        if m:
            return self.cfg.get(m.group(1), 'n') in ('y', 'm')
        return var in ('obj-y', 'always', 'extra-y') or None

    def built(self, path):
        """True/False/None(unknown) for a .c/.S path."""
        d, f = os.path.split(path)
        b = os.path.splitext(f)[0]
        r = self._obj(d, b + '.o')
        return r

    def _obj(self, d, tok, seen=()):
        if (d, tok) in seen:
            return None
        seen = seen + ((d, tok),)
        ents = self.parse(d)
        found = False
        res = False
        for var, toks in ents:
            if tok in toks and var.startswith('obj-'):
                v = self.val(var)
                if v is None:
                    return None
                found = True
                res = res or v
        if not found:
            # composite object: foo-objs / foo-y containing tok
            for var, toks in ents:
                m = re.match(r'([\w\-]+?)-(?:objs|y|\$\((CONFIG_\w+)\))$', var)
                if m and tok in toks:
                    if m.group(2) and self.cfg.get(m.group(2), 'n') not in ('y', 'm'):
                        continue
                    r = self._obj(d, m.group(1) + '.o', seen)
                    if r:
                        return True
                    found = True
            if not found:
                return None
            return res
        if not res:
            return False
        return self._dir(d)

    def _dir(self, d):
        # parent must descend into d
        while d:
            p, n = os.path.split(d)
            ents = self.parse(p)
            ok = None
            for var, toks in ents:
                if var.startswith('obj-') and (n + '/') in toks:
                    v = self.val(var)
                    ok = v if ok is None else (ok or v)
            if ok is False:
                return False
            d = p
        return True



PRE147 = '5f8dcf617235'  # aosp: Merge 4.4.147 into android-4.4-p (vendor base kernel version)
CVE_RE = re.compile(r'CVE-\d{4}-\d{4,}')
STRONG = re.compile(r'security|vulnerab|exploit|privilege|use[- ]after[- ]free|'
                    r'\buaf\b|out[- ]of[- ]bounds|\boob\b|overflow|double[- ]free|'
                    r'info(?:rmation)?[- ]?leak|disclos|harden|sanitiz|'
                    r'pointer leak|restrict|validate|sanity check|strong ref|malicious|'
                    r'unprivileged|\bleaks?\b|permission|\bspectre\b|\bmeltdown\b|\bcve\b', re.I)
FIXW = re.compile(r'\bfix|avoid|prevent|crash|panic|null|leak|race|deadlock|oops|'
                  r'\bbug\b|correct|missing|wrong|don.t|do not|uninit|bounds|'
                  r'validate|check|handle|corrupt|hang|lock', re.I)
SENS = ['drivers/android/binder', 'drivers/staging/android/ashmem',
        'drivers/staging/android/ion', 'drivers/staging/android/lowmemory',
        'drivers/staging/android/', 'drivers/android/', 'fs/sdcardfs',
        'fs/f2fs', 'fs/ext4', 'fs/crypto', 'security/', 'net/netfilter',
        'net/ipv4/netfilter', 'net/ipv6/netfilter', 'drivers/usb/gadget',
        'net/xfrm', 'net/key', 'uid_sys_stats', 'uid_cputime', 'cpufreq_times',
        'drivers/cpufreq', 'kernel/cgroup', 'fs/proc', 'fs/fuse', 'fs/overlayfs',
        'kernel/bpf', 'mm/', 'ipc/', 'kernel/trace', 'kernel/events',
        'drivers/char/random', 'lib/', 'fs/namei', 'fs/namespace', 'net/']
OTHER_ARCH = re.compile(r'^arch/(?!arm/|arm64/)')
NONCODE = re.compile(r'^(Documentation/|build\.config|android/configs/|'
                     r'arch/[a-z0-9]+/configs/|.*\.(md|txt|rst)$|MAINTAINERS$)')


_blob = {}


def head_text(path):
    if path not in _blob:
        r = git(['show', 'HEAD:' + path], check=False)
        _blob[path] = r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else ''
    return _blob[path]


def added_present(diff):
    """Fraction of distinct added lines (len>=12) already present in HEAD's files."""
    cur, tot, hit, seen = None, 0, 0, set()
    for l in diff.decode('utf-8', 'replace').splitlines():
        if l.startswith('+++ b/'):
            cur = l[6:]
        elif l.startswith('+') and not l.startswith('+++') and cur:
            t = l[1:].strip()
            if len(t) >= 12 and (cur, t) not in seen:
                seen.add((cur, t))
                tot += 1
                hit += t in head_text(cur)
    return (hit / tot if tot >= 2 else 0.0)


def stage1(cands, base, tmp, ours_files):
    ours = commit_list([base + '..HEAD'])
    pids = {}
    for h, s in ours:
        pid = patch_id(h)
        if pid:
            pids.setdefault(pid, h)
    # all text of our messages: cherry-pick shas and subject lines
    msgs = out(['log', '--no-merges', '--format=%B%x01', base + '..HEAD'])
    shas = set(re.findall(r'\b[0-9a-f]{40}\b', msgs))
    subj = {norm(s) for _, s in ours}
    for l in msgs.splitlines():
        l = l.strip()
        if l.startswith('- '):
            subj.add(norm(l[2:]))
    subj.discard('')
    idx = os.path.join(tmp, 'index')
    env = dict(os.environ, GIT_INDEX_FILE=idx)
    git(['read-tree', 'HEAD'], env=env)
    res = {}
    for h, s in cands:
        fl = files_of(h)
        st, ev = 'MISSING', ''
        diff = git(['diff-tree', '-p', '--binary', '--no-commit-id', h]).stdout
        pid = git(['patch-id', '--stable'], inp=diff).stdout.decode().split()
        cs = set(re.findall(r'cherry picked from commit ([0-9a-f]{40})', body(h)))
        if pid and pid[0] in pids:
            st, ev = 'PATCHID', pids[pid[0]][:12]
        elif cs & shas:
            st, ev = 'CHERRYSHA', sorted(cs & shas)[0][:12]
        elif len(norm(s)) >= 15 and norm(s) in subj:
            st, ev = 'SUBJECT', ''
        elif not fl or not any(f in ours_files for f in fl):
            st = 'NOFILES'
        elif diff.strip() and git(['apply', '--cached', '--check', '-R'],
                                  env=env, inp=diff, check=False).returncode == 0:
            st = 'REVAPPLIED'
        elif added_present(diff) >= 0.8:
            st = 'ADDEDLINES'
        conf = '-'
        if st == 'MISSING':
            if git(['apply', '--cached', '--check'], env=env, inp=diff,
                   check=False).returncode == 0:
                conf = 'low'
            elif git(['apply', '--cached', '--check', '-C1'], env=env, inp=diff,
                     check=False).returncode == 0:
                conf = 'med'
            else:
                conf = 'high'
        res[h] = (st, ev, conf, fl)
    return res


def classify(h, s, fl, conf, gate, ours_files):
    msg = body(h)
    cves = ','.join(sorted(set(CVE_RE.findall(msg))))
    code = [f for f in fl if f in ours_files]
    live = [f for f in code if not OTHER_ARCH.match(f) and not NONCODE.match(f)]
    if s.startswith('Revert'):
        return 'd', '', 'n/a', cves, 'revert of a commit we lack'
    if not live:
        return 'd', '', '-', cves, 'only docs/config/other-arch/absent files'
    built = []
    for f in live:
        if f.endswith(('.c', '.S')):
            built.append(gate.built(f))
        elif f.endswith('.h') or f.endswith('.dts') or f.endswith('.dtsi'):
            continue
        else:
            built.append(None)
    if not built:  # header-only: judge by directory of any sibling
        built = [None]
    if any(b is True for b in built):
        bstat = 'y'
    elif all(b is False for b in built):
        bstat = 'n'
    else:
        bstat = '?'
    text = s + '\n' + msg
    sens = [p for p in SENS if any(p in f for f in live)]
    strong = bool(STRONG.search(text))
    fixw = bool(FIXW.search(s)) or 'Fixes:' in msg
    prio = 0
    if cves: prio += 5
    if strong: prio += 3
    if sens: prio += 2
    if bstat == 'y': prio += 2
    if bstat == '?': prio += 1
    if fixw: prio += 1
    sec = bool(cves) or (strong and (fixw or bool(sens)))
    if bstat == 'n':
        return 'b', str(prio), 'n', cves, 'unbuilt (CONFIG off in dm63)' + \
            (' [security-ish]' if sec else '')
    if sec:
        return 'a', str(prio), bstat, cves, ('CVE' if cves else 'security wording') + \
            (' in ' + sens[0] if sens else '')
    if fixw:
        return 'c', str(prio), bstat, cves, 'bugfix' + (' in ' + sens[0] if sens else '')
    return 'b', str(prio), bstat, cves, 'feature/infra'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--config', default=None)
    ap.add_argument('--aosp', default='aosp/android-4.4-p')
    ap.add_argument('--base', default='origin/lineage-17.1')
    ap.add_argument('--tmp', default=None)
    a = ap.parse_args()
    if not a.config:
        sys.exit('--config PATH/.config required (make O=<tmp> ARCH=arm64 dm63_defconfig)')
    root = out(['rev-parse', '--show-toplevel']).strip()
    base = out(['merge-base', 'HEAD', a.base]).strip()
    cands = load_candidates(a.aosp)
    ours_files = set(out(['ls-tree', '-r', '--name-only', 'HEAD']).splitlines())
    os.makedirs(os.path.expanduser('~/.cache'), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='ack_audit.', dir=a.tmp or os.path.expanduser('~/.cache'))
    try:
        res = stage1(cands, base, tmp, ours_files)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    gate = Gate(a.config, root)
    pre = set(out(['rev-list', '--no-merges', PRE147]).split())
    rows = []
    for h, s in cands:
        st, ev, conf, fl = res[h]
        cl, prio, bstat, cves, why = '-', '', '-', '', ''
        if st == 'MISSING':
            cl, prio, bstat, cves, why = classify(h, s, fl, conf, gate, ours_files)
        elif st == 'NOFILES':
            cl, why = 'd', 'touched files absent in our tree'
        rows.append([h[:12], st, cl, prio, bstat, conf, cves, why, ev, 'pre147' if h in pre else 'post147', str(len(fl)),
                     s.replace('\t', ' '), ';'.join(fl[:8])])
    hdr = ['aosp', 'status', 'class', 'prio', 'built', 'conflict', 'cve', 'reason',
           'match', 'era', 'nfiles', 'subject', 'files']
    with open(a.out, 'w') as f:
        f.write('\t'.join(hdr) + '\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    from collections import Counter
    print(len(rows), 'candidates;', dict(Counter(r[1] for r in rows)),
          dict(Counter(r[2] for r in rows if r[1] == 'MISSING')), file=sys.stderr)


if __name__ == '__main__':
    main()
