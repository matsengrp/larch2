#!/usr/bin/env python3
# seam_sweep.py -- greedy sequential per-part regraft placement on the fold tree (true score)
import sys, os, re, subprocess, multiprocessing as mp
sys.setrecursionlimit(100000)
D, FOLD, CG = sys.argv[1], sys.argv[2], sys.argv[3]
W = "/home/ogi/matsen/larch2-wric-10datasets/build/wric_tension_map"
os.chdir(D)
env = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/install/gcc-trunk/lib64"))

def parse(s):
    pos = [0]
    def sub():
        if s[pos[0]] == "(":
            pos[0] += 1
            kids = [sub()]
            while s[pos[0]] == ",":
                pos[0] += 1
                kids.append(sub())
            pos[0] += 1; st = pos[0]
            while pos[0] < len(s) and s[pos[0]] not in ",();": pos[0] += 1
            return (kids, s[st:pos[0]])
        st = pos[0]
        while pos[0] < len(s) and s[pos[0]] not in ",();": pos[0] += 1
        return ([], s[st:pos[0]])
    t = sub()
    return t

def ser(n): return n[1] if not n[0] else "(" + ",".join(ser(c) for c in n[0]) + ")" + n[1]
def leaves(n):
    if not n[0]: return {n[1]}
    out = set()
    for c in n[0]: out |= leaves(c)
    return out

def score(nwk):
    fn, out = f"/tmp/seam{os.getpid()}.nwk", f"/tmp/seam{os.getpid()}.pb.gz"
    open(fn, "w").write(nwk.rstrip(";") + ";")
    if os.path.exists(out): os.remove(out)
    subprocess.run([W, "megatree", "fromnewick", fn, CG, out], capture_output=True, env=env)
    if not os.path.exists(out): return 999999
    r = subprocess.run([W, "tension", out, out, "0"], capture_output=True, text=True, env=env)
    os.remove(out)
    for ln in (r.stderr + r.stdout).splitlines():
        if "witness total score = " in ln:
            return int(ln.split("witness total score = ")[1].split(" ")[0])
    return 999999

subprocess.run([W, "megatree", "tonewick", FOLD, "fold.nwk"], check=True, env=env, stderr=subprocess.DEVNULL)
root = parse(re.sub(r":[0-9.eE+-]+", "", open("fold.nwk").read().strip().rstrip(";")))
base = score(ser(root))
print("base:", base, flush=True)
parts = sorted(f for f in os.listdir("cparts") if f.endswith(".nwk"))
for pf in parts:
    pid = pf.split("_")[1].split(".")[0].zfill(2)
    pl = set(open(f"ids/part_{pid}.ids.txt").read().split())
    def find(n):
        L = leaves(n)
        if pl <= L and len(L) == len(pl) and n is not root: return n
        for c in n[0]:
            r = find(c)
            if r is not None: return r
        return None
    pn = find(root)
    if pn is None:
        print(f"{pf}: not a clade in fold, skip"); continue
    part_tree = ser(pn)
    def contract(n):
        return ([], "SEAMPART") if n is pn else ([contract(c) for c in n[0]], n[1])
    cr = contract(root)
    # insertion candidates: cut the part out cleanly, then regraft beside every node
    def contract(n):
        return ([], "SEAMPART") if n is pn else ([contract(c) for c in n[0]], n[1])
    cr0 = contract(root)
    def bypass(n):  # remove SEAMPART leaf; collapse unary nodes
        if not n[0]:
            return None if n[1] == "SEAMPART" else n
        kids = []
        for c in n[0]:
            cc = bypass(c)
            if cc is None: continue
            if isinstance(cc, list): kids.extend(cc)
            else: kids.append(cc)
        if not kids: return None
        if len(kids) == 1 and n is not cr0: return kids[0]
        return (kids, n[1])
    cr = bypass(cr0)
    def replace_at(node, path, new):
        if not path: return new
        if len(path) == 1:
            kids = list(node[0]); kids[path[0]] = new; return (kids, node[1])
        kids = list(node[0]); kids[path[0]] = replace_at(kids[path[0]], path[1:], new)
        return (kids, node[1])
    def node_at(m, p): return m if not p else node_at(m[0][p[0]], p[1:])
    def walk(n, path):
        cands.append(path)
        for i, c in enumerate(n[0]):
            walk(c, path + [i])
    cands = []
    walk(cr, [])
    trees = [replace_at(cr, p, ([([], "SEAMPART"), node_at(cr, p)], "")) for p in cands]
    def to_nwk(t):
        return ser(t) + ";"
    nws = [to_nwk(t) for t in trees]
    def splice(nwk):  # put the real part tree at SEAMPART
        out = nwk.replace("(SEAMPART)", "(" + part_tree + ")")
        out = out.replace(",SEAMPART)", "," + part_tree + ")")
        out = out.replace("(SEAMPART,", "(" + part_tree + ",")
        out = out.replace(",SEAMPART,", "," + part_tree + ",")
        return out
    cands_n = [splice(n) for n in nws]
    with mp.get_context('fork').Pool(12) as pool:
        scs = pool.map(score, cands_n)
    bi = min(range(len(scs)), key=lambda i: scs[i])
    d = scs[bi] - base
    print(f"{pf}: best {scs[bi]} delta {d:+d} (of {len(scs)} placements)", flush=True)
    if scs[bi] < base:
        base = scs[bi]
        best = cands_n[bi]
        open("seam_best.nwk", "w").write(best)
        subprocess.run([W, "megatree", "fromnewick", "seam_best.nwk", CG, "seam_best.pb.gz"],
                       capture_output=True, env=env)
        subprocess.run([W, "megatree", "tonewick", "seam_best.pb.gz", "fold.nwk"],
                       check=True, env=env, stderr=subprocess.DEVNULL)
        root = parse(re.sub(r":[0-9.eE+-]+", "", open("fold.nwk").read().strip().rstrip(";")))
print("FINAL:", base)
