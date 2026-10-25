#!/usr/bin/env python3
"""Architecture A: in-coupled mask master over the 26-part fold family.

Node = entry-mask set on (g, r, part) -> state.  Bound = masked composite
DP (scaffold lb with masks).  Candidate evaluation = pin(completion) +
sum of EXACT clamped PartOpts (lazy, memoized per column hash) over all
parts except the crown (slot 08, composite only -- the 7-unit blind
spot).  Incumbent U = 59715 (wit4 realization).

Phases:
  A: root X-tilde at the lb argmin profile pi-hat.
  B: single-part witness-argmin column moves (each = one mask node).
  C: greedy stacking of improving moves (pairs among improvers).
  D: time-boxed best-first entry-branching; LB_master = min open bound.

All artifacts under /tmp/v0/m23/master/.
"""
import hashlib
import json
import os
import subprocess
import sys
import time

R = "/home/ogi/matsen/larch2-wric-10datasets/build"
W = "/tmp/v0/m23/master"
NP = 26
CROWN = 8
U_INC = 59715
os.makedirs(W, exist_ok=True)
for d in ("clamp", "wit", "prof", "mask", "trim"):
    os.makedirs(f"{W}/{d}", exist_ok=True)

LOG = open(f"{W}/master.log", "a", buffering=1)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=LOG)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def lb(maskfile, prof_out):
    cmd = [f"{R}/wric_tension_map", "scaffold", "lb", "scaffold_bin.pb.gz",
           "m23/rp1/tc26.pb.gz", "m23/rp1/ids/part_", "m23/rp1/resp3full/part_",
           str(NP), prof_out, "0", maskfile]
    r = run(cmd, cwd="/tmp/v0")
    for line in r.stderr.splitlines():
        if "total = " in line:
            return int(line.split("total = ")[1].split()[0])
    raise RuntimeError("lb failed: " + r.stderr[-500:])


def pin(prof):
    cmd = [f"{R}/wric_tension_map", "scaffold", "pin", "scaffold_bin.pb.gz",
           "m23/rp1/tc26.pb.gz", "m23/rp1/ids/part_", "m23/rp1/resp3full/part_",
           str(NP), prof]
    r = run(cmd, cwd="/tmp/v0")
    for line in r.stderr.splitlines():
        if "total = " in line:
            return int(line.split("total = ")[1].split()[0])
    raise RuntimeError("pin failed: " + r.stderr[-500:])


def column_hash(prof, j):
    """Column j of a profile file -> hash.  Part-indexed: identical columns
    across different parts must be different keys (a collision here handed
    parts each other's PartOpts)."""
    h = hashlib.sha1()
    h.update(f"j={j}".encode())
    for line in open(prof):
        if line.startswith("#"):
            continue
        f = line.split()
        h.update(f"{f[0]} {f[1]} {f[3][j]}".encode())
    return h.hexdigest()[:16]


PARTOPT_MEMO = {}
WITNESS_MEMO = {}
PARTOPT_FILE = f"{W}/partopt_memo.json"
if os.path.exists(PARTOPT_FILE):
    PARTOPT_MEMO = json.load(open(PARTOPT_FILE))


def save_memo():
    json.dump(PARTOPT_MEMO, open(PARTOPT_FILE, "w"))


GRAMMAR_OF = {}
for _j in range(NP):
    with open(f"m23/rp1/resp3full/part_{_j:02d}.spr.pb.resp") as _f:
        for _line in _f:
            if _line.startswith("# part "):
                GRAMMAR_OF[_j] = _line[7:].strip()
                break
    assert GRAMMAR_OF.get(_j) and os.path.exists(
        "/tmp/v0/" + GRAMMAR_OF[_j]), (_j, GRAMMAR_OF.get(_j))


def partopt(prof, j, slack=80):
    """EXACT clamped PartOpt for part j at column j of prof (memoized).
    UB seeded at composite + slack; retry once at +300."""
    ch = column_hash(prof, j)
    if ch in PARTOPT_MEMO:
        return PARTOPT_MEMO[ch]
    g = GRAMMAR_OF[j]
    comp = composite_of(prof, j)
    clamp = f"{W}/clamp/p_{j}_{ch}.clamp"
    r = run([f"{R}/wric_tension_map", "vleaf", "clampgen", "scaffold_bin.pb.gz",
             prof, str(j), g, clamp], cwd="/tmp/v0")
    if not os.path.exists(clamp):
        raise RuntimeError(f"clampgen j={j} failed: {r.stderr[-300:]}")
    for attempt_ub in (comp + slack, comp + 300):
        r = run([f"{R}/bin/larch2", "--dag-pb", g, "-o",
                 f"{W}/wit/w_{j}_{ch}.pb.gz",
                 "--trim-mode", "chart-bnb", "-n", "0", "--chart-bnb-score-only",
                 "--chart-bnb-upper-bound", str(attempt_ub),
                 "--chart-bnb-root-clamp", clamp, str(j),
                 "--chart-bnb-class-compress", "on", "--chart-bnb-profile-dedup",
                 "on", "--chart-bnb-trim-application",
                 "optimal-topology-materialize",
                 "--chart-bnb-max-exact-topologies", "1"],
                cwd="/tmp/v0", timeout=7200)
        opt = None
        for line in r.stderr.splitlines():
            if "bnb_optimum:" in line:
                opt = int(line.split(":")[1])
        if opt is not None:
            PARTOPT_MEMO[ch] = opt
            WITNESS_MEMO[ch] = f"{W}/wit/w_{j}_{ch}.pb.gz"
            save_memo()
            return opt
    raise RuntimeError(f"trim j={j} failed at both UBs: {r.stderr[-300:]}")


def composite_of(prof, j):
    """Composite (m-row) value of part j's column of prof.

    Mirrors the validated UB formula: profile keyed by gidx (one line per
    pattern), resp rows 'gidx weight refA..T mA..T' with refs carrying
    the counts.
    """
    profg = {}
    for line in open(prof):
        if line.startswith("#"):
            continue
        f = line.split()
        profg[int(f[0])] = f[3]
    ap = 0
    for line in open(f"m23/rp1/resp3full/part_{j:02d}.spr.pb.resp"):
        if line.startswith("#"):
            continue
        f = line.split()
        g = int(f[0])
        col = profg.get(g)
        if col is None:
            continue
        refs = [int(x) for x in f[2:6]]
        m = [int(x) for x in f[6:10]]
        s0 = ord(col[j]) - 65
        for rr in range(4):
            if refs[rr]:
                ap += refs[rr] * (m[s0] + (1 if s0 != rr else 0))
    return ap


def witness_argmin_map(j):
    """Witness j's own per-(g,r) root states (profgen: one char per line)."""
    out = f"{W}/prof/w{j}.profile"
    if not os.path.exists(out):
        run([f"{R}/wric_tension_map", "vleaf", "profgen", "scaffold_bin.pb.gz",
             f"m23/rp1/wit4/part_{j:02d}.wit.pb.gz", out], cwd="/tmp/v0")
    m = {}
    for line in open(out):
        if line.startswith("#"):
            continue
        f = line.split()
        m[(int(f[0]), int(f[1]))] = f[3]
    return m


def profile_map(path):
    m = {}
    for line in open(path):
        if line.startswith("#"):
            continue
        f = line.split()
        m[(int(f[0]), int(f[1]))] = f[3]
    return m


def merge_profiles(base, others):
    """base profile with columns replaced by each (j, profile) in others
    (keyed by (g, r); no line-order assumptions)."""
    out = f"{W}/prof/merged_{int(time.time()*1003)%10**9}.profile"
    omaps = others  # {j: {(g, r): single-char state}}
    with open(out, "w") as f:
        f.write("# mixed profile\n")
        for line in open(base):
            if line.startswith("#"):
                continue
            fl = line.split()
            g, r, cnt = int(fl[0]), int(fl[1]), fl[2]
            states = list(fl[3])
            for j, m in omaps.items():
                states[j] = m[(g, r)]
            f.write(f"{g} {r} {cnt} " + "".join(states) + "\n")
    return out


def main():
    t0 = time.time()
    log("=== master A start; U_inc =", U_INC)

    # Phase A: root.
    mask_empty = f"{W}/mask/empty.txt"
    open(mask_empty, "w").write("#\n")
    pihat = f"{W}/prof/pihat.profile"
    b0 = lb(mask_empty, pihat)
    log("A: root bound =", b0)
    pin0 = pin(pihat)
    log("A: pin(pihat) =", pin0)
    # composite of crown column at pihat
    comp8 = composite_of(pihat, CROWN)
    log("A: crown composite at pihat =", comp8)
    # PartOpts at pihat columns (memoized; first run ~15 min)
    tA = time.time()
    total_parts = 0
    for j in range(NP):
        if j == CROWN:
            continue
        po = partopt(pihat, j)
        total_parts += po
    log(f"A: sum PartOpt(25) = {total_parts} ({time.time()-tA:.0f}s)")
    xtilde0 = pin0 + total_parts + comp8
    log("A: X~(pihat) =", xtilde0, " U - X~ =", U_INC - xtilde0)
    json.dump({"root_bound": b0, "pin": pin0, "sum_partopt": total_parts,
               "crown_comp": comp8, "xtilde": xtilde0},
              open(f"{W}/root.json", "w"), indent=1)

    # Phase B: witness-argmin column moves.
    pm0 = profile_map(pihat)
    results_b = {}
    for j in range(NP):
        if j == CROWN:
            continue
        wm = witness_argmin_map(j)
        pm = profile_map(pihat)
        assert set(pm) == set(wm), (len(pm), len(wm))
        diff = 0
        maskf = f"{W}/mask/wmove_{j}.txt"
        with open(maskf, "w") as mf:
            for key in sorted(pm):
                sp, sw = pm[key][j], wm[key]
                if sp != sw:
                    mf.write(f"{key[0]} {key[1]} {j} {ord(sw)-65}\n")
                    diff += 1
        results_b[j] = {"diff": diff}
        if diff == 0:
            log(f"B: part {j:02d} witness column == pihat (0 diff)")
            continue
        prof = f"{W}/prof/mix_{j}.profile"
        bnd = lb(maskf, prof)
        if bnd >= U_INC:
            log(f"B: part {j:02d} move bound {bnd} >= U -> pruned")
            results_b[j]["bound"] = bnd
            results_b[j]["status"] = "pruned"
            continue
        p = pin(prof)
        po_sum = sum(partopt(prof, k) for k in range(NP) if k != CROWN)
        xt = p + po_sum + composite_of(prof, CROWN)
        results_b[j].update({"bound": bnd, "pin": p, "xtilde": xt,
                             "status": "open"})
        log(f"B: part {j:02d} diff={diff} bound={bnd} pin={p} X~={xt} "
            f"delta_vs_root={xt - xtilde0}")
    json.dump(results_b, open(f"{W}/phaseB.json", "w"), indent=1, default=str)

    # Phase C: stack improving moves greedily.
    improvers = sorted(
        [(v.get("xtilde", 10**9), j) for j, v in results_b.items()
         if v.get("status") == "open"], key=lambda x: x[0])
    log("C: improvers (X~, part):", improvers[:8])
    best_mix = None
    if improvers and improvers[0][0] < xtilde0:
        stack = [j for _, j in improvers if results_b[j].get("xtilde", 10**9) < xtilde0]
        log("C: stacking", stack)
        prof = merge_profiles(pihat, {j: witness_argmin_map(j) for j in stack})
        maskf = f"{W}/mask/stack.txt"
        with open(maskf, "w") as mf:
            for j in stack:
                wm = witness_argmin_map(j)
                for key in sorted(pm0):
                    sp, sw = pm0[key][j], wm[key]
                    if sp != sw:
                        mf.write(f"{key[0]} {key[1]} {j} {ord(sw)-65}\n")
        bnd = lb(maskf, prof)
        p = pin(prof)
        po_sum = sum(partopt(prof, k) for k in range(NP) if k != CROWN)
        xt = p + po_sum + composite_of(prof, CROWN)
        log(f"C: stack {stack} bound={bnd} pin={p} X~={xt}")
        best_mix = {"parts": stack, "bound": bnd, "pin": p, "xtilde": xt,
                    "profile": prof}
        json.dump(best_mix, open(f"{W}/phaseC.json", "w"), indent=1)

    # Materialize the best X~ profiles as real trees (wit DP).
    def realize(prof, tag):
        os.makedirs(f"{W}/real/{tag}", exist_ok=True)
        for j in range(NP):
            if j == CROWN:
                continue
            partopt(prof, j)  # ensures the witness memo for its column
            ch = column_hash(prof, j)
            os.system(f"cp {WITNESS_MEMO[ch]} {W}/real/{tag}/part_{j:02d}.wit.pb.gz")
        os.system(f"cp m23/rp1/wit4/part_{CROWN:02d}.wit.pb.gz "
                  f"{W}/real/{tag}/part_{CROWN:02d}.wit.pb.gz")
        r = run([f"{R}/wric_tension_map", "scaffold", "wit", "scaffold_bin.pb.gz",
                 "m23/rp1/tc26.pb.gz", "m23/rp1/ids/part_",
                 f"{W}/real/{tag}/part_", str(NP), "-", "0"], cwd="/tmp/v0")
        for line in r.stderr.splitlines():
            if "total = " in line:
                return int(line.split("total = ")[1].split()[0])
        raise RuntimeError("wit failed: " + r.stderr[-300:])

    u_pihat = realize(pihat, "pihat")
    log("C: realized U at pihat columns =", u_pihat)
    if best_mix and best_mix["xtilde"] < xtilde0:
        u_mix = realize(best_mix["profile"], "mix")
        log("C: realized U at mixed columns =", u_mix)

    log("master A phases A-C done in", round(time.time() - t0), "s")
    save_memo()


if __name__ == "__main__":
    main()
