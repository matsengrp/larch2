#!/usr/bin/env python3
# loop.py -- the carve/solve/fold/escape/certify loop (guide 10.4 prototype)
# usage: loop.py <name> <init_tree.pb.gz> <carve_probe.pb.gz> <cg.pb.gz> <rounds> <MIN> <MAX> <nproc> <outdir>
import sys, os, re, time, subprocess, shutil, multiprocessing as mp
sys.setrecursionlimit(200000)
NAME, INIT, PROBE0, CG, ROUNDS, MIN, MAX, NPROC, OUT = (
    sys.argv[1], os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3]),
    os.path.abspath(sys.argv[4]), int(sys.argv[5]),
    int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8]), os.path.abspath(sys.argv[9]))
R = "/home/ogi/matsen/larch2-wric-10datasets/build"
L, W = f"{R}/bin/larch2", f"{R}/wric_tension_map"
env = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/install/gcc-trunk/lib64"))
os.makedirs(OUT, exist_ok=True)
os.chdir(OUT)
log = open(f"{NAME}.log", "a", buffering=1)

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, env=env, **kw)

def tscore(pb):
    r = run([W, "tension", pb, pb, "0"])
    for ln in (r.stderr + r.stdout).splitlines():
        m = re.search(r"witness total score = (\d+)", ln)
        if m: return int(m.group(1))
    return None

def tonewick(pb, out):
    run([W, "megatree", "tonewick", pb, out])
    s = open(out).read().strip()
    return re.sub(r":[0-9.eE+-]+", "", s.rstrip(";"))

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
    return sub()

def ser(n): return n[1] if not n[0] else "(" + ",".join(ser(c) for c in n[0]) + ")" + n[1]
def leaves(n):
    if not n[0]: return {n[1]}
    out = set()
    for c in n[0]: out |= leaves(c)
    return out

def carve(root):
    removed, parts = set(), []
    while True:
        best, bestsz = None, 0
        stack = [root]
        while stack:
            x = stack.pop()
            Lv = leaves(x)
            n = len(Lv)
            if not (Lv & removed) and bestsz < n <= MAX and n >= MIN:
                best, bestsz = x, n
            stack.extend(x[0])
        if best is None: break
        parts.append(best); removed |= leaves(best)
    return parts, leaves(root) - removed

def solve_part(args):
    i, p, seed = args
    pf = f"{p:02d}"
    d = f"r{RD}/p{pf}"
    os.makedirs(d, exist_ok=True)
    run([W, "megatree", "fromnewick", f"r{RD}/parts/part_{i}.nwk", CG, f"{d}/poly.pb.gz"])
    run([f"{R}/wric_binarize", f"{d}/poly.pb.gz", f"{d}/bin.pb.gz", f"{d}/ids.txt"])
    r = run([L, "--dag-pb", f"{d}/bin.pb.gz", "-o", f"{d}/part_{pf}.spr.pb.gz", "-n", "1",
             "--max-moves", "50", "--seed", str(seed)])
    inc = None
    for ln in r.stderr.splitlines():
        m = re.search(r"iter 1: parsimony=(\d+)", ln)
        if m: inc = int(m.group(1))
    run([f"{R}/wric_dag_binarize", f"{d}/part_{pf}.spr.pb.gz", f"{d}/part_{pf}.sbin.pb.gz"])
    ub = inc if inc is not None else 999999999
    r = run([L, "--dag-pb", f"{d}/part_{pf}.sbin.pb.gz", "-o", f"{d}/wit.pb.gz", "--trim-mode", "chart-bnb",
             "-n", "0", "--chart-bnb-score-only", "--chart-bnb-upper-bound", str(ub),
             "--chart-bnb-class-compress", "on", "--chart-bnb-profile-dedup", "on",
             "--wric-lazy-chart", "on", "--chart-bnb-trim-application",
             "optimal-topology-materialize", "--chart-bnb-max-exact-topologies", "1"])
    sc = None
    for ln in (r.stderr + r.stdout).splitlines():
        m = re.search(r"bnb_optimum: (\d+)", ln)
        if m: sc = int(m.group(1))
    return pf, sc if sc is not None else ub, f"{d}/wit.pb.gz", f"{d}/part_{pf}.sbin.pb.gz"

# init
shutil.copy(INIT, "T0.pb.gz")
T = "T0.pb.gz"
cur = tscore(T)
log.write(f"init: {cur}\n")
best = cur
RD = 0
for RD in range(1, ROUNDS + 1):
    t0 = time.time()
    os.makedirs(f"r{RD}", exist_ok=True)
    # 1. CARVE (round 1: from the polytomic probe; later: from current T)
    src = PROBE0 if RD == 1 else T
    root = parse(tonewick(src, f"r{RD}/src.nwk"))
    parts, frozen = carve(root)
    os.makedirs(f"r{RD}/parts", exist_ok=True)
    os.makedirs(f"r{RD}/ids", exist_ok=True)
    for i, p in enumerate(parts):
        open(f"r{RD}/parts/part_{i}.nwk", "w").write(ser(p) + ";")
        open(f"r{RD}/ids/part_{i:02d}.ids.txt", "w").write("\n".join(sorted(leaves(p))) + "\n")
    NP = len(parts)
    # 2. SOLVE (parallel)
    with mp.get_context("fork").Pool(min(NPROC, NP)) as pool:
        res = pool.map(solve_part, [(i, i, RD) for i in range(NP)])
    # 3. FOLD
    shutil.copy(T, f"r{RD}/fold_cur.pb.gz")
    for pf, sc, wit, _ in res:
        os.makedirs(f"r{RD}/g_{pf}", exist_ok=True)
        run([f"{R}/wric_subtree", "graft", f"r{RD}/fold_cur.pb.gz", f"r{RD}/ids/part_{pf}.ids.txt",
             f"r{RD}/g_{pf}", wit])
        shutil.copy(f"r{RD}/g_{pf}/graft_0.pb.gz", f"r{RD}/fold_cur.pb.gz")
    U_fold = tscore(f"r{RD}/fold_cur.pb.gz")
    t_fold = time.time() - t0
    # 4. ESCAPE: round-trip fold tree -> fresh SPR (+ chart if the grammar fits)
    nxt = f"r{RD}/fold_cur.pb.gz"
    U_esc = U_fold
    esc = f"r{RD}/esc.pb.gz"
    nleaves = len(leaves(parse(tonewick(f"r{RD}/fold_cur.pb.gz", f"r{RD}/fold.nwk"))))
    nwk = tonewick(f"r{RD}/fold_cur.pb.gz", f"r{RD}/fold.nwk")
    open(f"r{RD}/fold2.nwk", "w").write(nwk + ";")
    if run([W, "megatree", "fromnewick", f"r{RD}/fold2.nwk", CG, f"r{RD}/fold_rt.pb.gz"]).returncode == 0:
        U_rt = tscore(f"r{RD}/fold_rt.pb.gz")
        if U_rt is not None and (U_fold is None or U_rt <= U_fold):
            r = run([L, "--dag-pb", f"r{RD}/fold_rt.pb.gz", "-o", f"r{RD}/esc_raw.pb.gz", "-n", "1",
                     "--max-moves", "50", "--seed", str(RD)])
            inc = None
            for ln in r.stderr.splitlines():
                m = re.search(r"iter 1: parsimony=(\d+)", ln)
                if m: inc = int(m.group(1))
            if inc is not None:
                run([f"{R}/wric_dag_binarize", f"r{RD}/esc_raw.pb.gz", f"r{RD}/esc_sbin.pb.gz"])
                if nleaves <= 1500:
                    # full escape arm: chart recombination + binary witness
                    rc = run([L, "--dag-pb", f"r{RD}/esc_sbin.pb.gz", "-o", esc, "--trim-mode", "chart-bnb",
                              "-n", "0", "--chart-bnb-score-only", "--chart-bnb-upper-bound", str(inc),
                              "--chart-bnb-class-compress", "on", "--chart-bnb-profile-dedup", "on",
                              "--wric-lazy-chart", "on", "--chart-bnb-trim-application",
                              "optimal-topology-materialize", "--chart-bnb-max-exact-topologies", "1"])
                    U_esc = tscore(esc) if os.path.exists(esc) else inc
                else:
                    # at scale: SPR-only escape (mono chart does not fit); score = incumbent
                    shutil.copy(f"r{RD}/esc_sbin.pb.gz", esc)
                    U_esc = inc
    t_esc = time.time() - t0
    # pick next T (at scale, only accept the SPR escape when strictly better:
    # its grammar may carry polytomies that break later tension scores)
    if U_esc is not None and (U_fold is None or U_esc < U_fold or (U_esc <= U_fold and nleaves <= 1500)):
        nxt, U_now = esc, U_esc
    else:
        U_now = U_fold
    shutil.copy(nxt, f"T{RD}.pb.gz")
    T = f"T{RD}.pb.gz"
    best = min(best, U_now) if U_now is not None else best
    # 5. CERTIFY (response + scaffold lb over the fold family)
    LB = None
    try:
        os.makedirs(f"r{RD}/resp", exist_ok=True)
        args = [W, "response", f"r{RD}/fold_scaffold.pb.gz", f"r{RD}/resp/"] + [s for _, _, _, s in res]
        # scaffold for response = T with parts grafted witnesses? response wants the FOLD scaffold:
        shutil.copy(f"r{RD}/fold_cur.pb.gz", f"r{RD}/fold_scaffold.pb.gz")
        run(args)
        os.makedirs(f"r{RD}/resp2", exist_ok=True)
        for pf, _, _, _ in res:
            src_f = f"r{RD}/resp/part_{pf}.sbin.pb.resp"
            if os.path.exists(src_f):
                shutil.copy(src_f, f"r{RD}/resp2/part_{pf}.spr.pb.resp")
        pi = "pi.profile"
        for c in [f"r{RD}/resp/pi.profile", "pi.profile"]:
            if os.path.exists(c): pi = c; break
        open("empty.mask", "w").write("#\n")
        r = run([W, "scaffold", "lb", f"r{RD}/fold_scaffold.pb.gz", f"r{RD}/fold_cur.pb.gz",
                 f"r{RD}/ids/part_", f"r{RD}/resp2/part_", str(NP), pi, "0", "empty.mask"])
        for ln in (r.stderr + r.stdout).splitlines():
            m = re.search(r"total = (\d+)", ln)
            if m: LB = int(m.group(1))
    except Exception as e:
        log.write(f"certify error: {e}\n")
    log.write(f"round {RD}: parts={NP} frozen={len(frozen)} U_fold={U_fold} U_esc={U_esc} "
              f"U={U_now} LB={LB} best={best} t_fold={t_fold:.0f}s t_total={t_esc:.0f}s\n")
log.write(f"DONE best={best}\n")
