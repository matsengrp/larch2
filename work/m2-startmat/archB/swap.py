#!/usr/bin/env python3
"""One-away sweep (Architecture B closing instrument): full-assignment
leaves adjacent to the incumbent tuple.  For each part j and pool
candidate k != 0, evaluate the FULL assignment {j':0 for j'!=j, j:k} --
the mixed DP at full assignment is the EXACT wit value of that tuple.
If every one-away leaf >= 59714, any improving tuple needs >= 2
simultaneous candidate swaps.

Runs the same mixed lb machinery; ~1 min/eval, ~181 leaves.
"""
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

LOG = open(f"{W}/swap.log", "a", buffering=1)


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


POOLS = pools()
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = os.path.expanduser("~/install/gcc-trunk/lib64")


def leaf(assign):
    cfg = f"{W}/cfg/swap_{abs(hash(tuple(sorted(assign.items()))))}.txt"
    with open(cfg, "w") as f:
        for j in range(NP):
            f.write(f"{j} w {POOLS[j][assign[j]]}\n")
    ENV["WRIC_MIXED"] = cfg
    r = subprocess.run(
        [f"{R}/wric_tension_map", "scaffold", "lb", f"{V0}/scaffold_bin.pb.gz",
         SCAF, IDS, RESP, str(NP), "/dev/null", "0", MASK],
        capture_output=True, text=True, env=ENV)
    tot = None
    for line in r.stderr.splitlines():
        if "total = " in line:
            tot = int(line.split("total = ")[1].split()[0])
    os.remove(cfg)
    if tot is None:
        raise RuntimeError(r.stderr[-300:])
    return tot


def main():
    t0 = time.time()
    log("one-away sweep start; pools:",
        {j: len(v) for j, v in POOLS.items()})
    base = {j: 0 for j in range(NP)}
    vals = {}
    best = (None, 10**9)
    for j in range(NP):
        for k in range(1, len(POOLS[j])):
            a = dict(base)
            a[j] = k
            v = leaf(a)
            vals[f"{j}:{k}"] = v
            if v < best[1]:
                best = (f"{j}:{k}", v)
                log(f"j={j} k={k} leaf={v} BEST so far")
            elif v < INC:
                log(f"j={j} k={k} leaf={v} below incumbent!")
    log("one-away DONE", "best:", best, f"({time.time()-t0:.0f}s)")
    json.dump(vals, open(f"{W}/swap_result.json", "w"), indent=1)
    hist = {}
    for v in vals.values():
        hist[v] = hist.get(v, 0) + 1
    log("leaf histogram:", dict(sorted(hist.items())))


if __name__ == "__main__":
    main()
