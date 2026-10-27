#!/usr/bin/env python3
"""Architecture B: candidate-tuple B&B with mixed-row bounds.

Node = partial assignment of parts to pool candidates (candidate 0 =
incumbent witness; others = diverse-optimal materializations).
Bound = scaffold lb with WRIC_MIXED (assigned parts carry EXACT tree
rows, unassigned the m-row envelope).  Leaves (full assignment) are
exact wit values of the tuple.  Incumbent = U = 59714 (post-Phase-F).

Fixed branching order: crown first (biggest residual + row gap), then
rich pools / big insides.  Parts with pool size 1 chain without
branching.  Best-first (min bound); prune >= incumbent.
"""
import heapq
import itertools
import json
import os
import subprocess
import time

R = "/home/ogi/matsen/larch2-wric-10datasets/build"
V0 = "/tmp/v0"
SCAF = f"{V0}/m23/plF/cand/tc27.pb.gz"
IDS = f"{V0}/m23/rp1/ids/part_"
RESP = f"{V0}/m23/rp1/resp3full/part_"
NP = 26
INC = 59714
W = f"{V0}/m23/archb"
MASK = f"{V0}/m23/plF/empty.mask"

LOG = open(f"{W}/archb.log", "a", buffering=1)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=LOG)


def pools():
    P = {}
    for j in range(NP):
        cand = [f"{V0}/m23/rp1/wit4/part_{j:02d}.wit.pb.gz"]
        i = 0
        while os.path.exists(f"{W}/cand/c_{j}_{i}.pb.gz"):
            cand.append(f"{W}/cand/c_{j}_{i}.pb.gz")
            i += 1
        P[j] = cand
    return P


BOUND_MEMO = {}
import os as _os
_os.makedirs(f"{W}/cfg", exist_ok=True)
EVALS = [0]


def bound(assign):
    """Mixed-row lb of the partial assignment {j: cand_idx}."""
    key = tuple(sorted(assign.items()))
    if key in BOUND_MEMO:
        return BOUND_MEMO[key]
    cfg = f"{W}/cfg/node_{abs(hash(key)) % 10**9}.txt"
    with open(cfg, "w") as f:
        for j in range(NP):
            if j in assign:
                f.write(f"{j} w {POOLS[j][assign[j]]}\n")
            else:
                f.write(f"{j} m\n")
    env = dict(os.environ)
    env["WRIC_MIXED"] = cfg
    env["LD_LIBRARY_PATH"] = os.path.expanduser("~/install/gcc-trunk/lib64")
    r = subprocess.run(
        [f"{R}/wric_tension_map", "scaffold", "lb", f"{V0}/scaffold_bin.pb.gz",
         SCAF, IDS, RESP, str(NP), "/dev/null", "0", MASK],
        capture_output=True, text=True, env=env)
    tot = None
    for line in r.stderr.splitlines():
        if "total = " in line:
            tot = int(line.split("total = ")[1].split()[0])
    if tot is None:
        raise RuntimeError("mixed lb failed: " + r.stderr[-400:])
    EVALS[0] += 1
    BOUND_MEMO[key] = tot
    os.remove(cfg)
    return tot


POOLS = {}


def main():
    global POOLS
    POOLS = pools()
    ps = {j: len(v) for j, v in POOLS.items()}
    log("B2 start; pool sizes:", ps, "sum:", sum(ps.values()))
    order = [8, 1, 2, 6, 7, 9, 15, 18, 20, 3, 4, 11, 13, 16, 21, 22, 25,
             5, 0, 10, 12, 14, 17, 19, 23, 24]
    # keep only parts present
    order = [j for j in order if j in POOLS]
    t0 = time.time()
    # node = (bound, seq, assign-dict-copy)
    root_b = bound({})
    log("root bound =", root_b)
    heap = [(root_b, 0, {})]
    seq = itertools.count(1)
    leaves = []
    opened = 0
    while heap:
        b, _, assign = heapq.heappop(heap)
        opened += 1
        if b >= INC:
            continue
        if len(assign) == len(order):
            leaves.append((b, dict(assign)))
            log(f"LEAF bound={b} assign={dict(assign)}")
            continue
        j = next(p for p in order if p not in assign)
        for k in range(len(POOLS[j])):
            child = dict(assign)
            child[j] = k
            cb = bound(child)
            if cb < INC:
                heapq.heappush(heap, (cb, next(seq), child))
            if cb < INC - 40:  # startling: log loudly
                log(f"OPEN node j={j} k={k} bound={cb}")
        if opened % 5 == 0:
            log(f"opened={opened} heap={len(heap)} evals={EVALS[0]} "
                f"({time.time()-t0:.0f}s) best_open={heap[0][0] if heap else '-'}")
    log("B2 DONE. leaves:", leaves[:10], "opened:", opened,
        "evals:", EVALS[0], f"({time.time()-t0:.0f}s)")
    json.dump({"leaves": leaves, "opened": opened, "evals": EVALS[0],
               "memo": {str(k): v for k, v in BOUND_MEMO.items()}},
              open(f"{W}/archb_result.json", "w"), indent=1)


if __name__ == "__main__":
    main()
