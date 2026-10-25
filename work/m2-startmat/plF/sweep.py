#!/usr/bin/env python3
"""Phase F sweep: free-row placement (SPR of pseudo leaves / cherries on
tc26) evaluated by the true objective (scaffold wit, jointly-free states).

Targets (plan WRIC-ARCHB-PLACEMENT.md §3):
  F.1 cherries: 02, 03, pair(02,03), 11, 12, pair(11,12)
  F.2 big-inside: 09, then crown 08 (last)
Candidate edges: all regraft edges within BFS distance D of the old
spot + a deterministic far sample.  Dedup by newick.  Log + bests.
"""
import copy
import json
import os
os.makedirs('/tmp/v0/m23/plF/keep', exist_ok=True)
import sys
import time

import kit

sys.setrecursionlimit(60000)

D_NEAR = 6
F_FAR = 12
OUT = "/tmp/v0/m23/plF/cand"
LOG = open("/tmp/v0/m23/plF/sweep.log", "a", buffering=1)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=LOG)


def rebuild(nwk_str, tag):
    p = f"{OUT}/{tag}.nwk"
    with open(p, "w") as f:
        f.write(nwk_str)  # NO trailing newline/new ';' handling here
    pb = f"{OUT}/{tag}.pb.gz"
    r = os.popen(
        f"cd /tmp/v0 && {kit.R}/wric_tension_map megatree fromnewick "
        f"{p} /tmp/v0/m23/rp1/tc26.pb.gz {pb} 2>&1 | tail -1").read()
    if "cgs=" not in r:
        raise RuntimeError(f"fromnewick {tag}: {r}")
    return pb


def wit(pb):
    r = os.popen(
        f"cd /tmp/v0 && {kit.R}/wric_tension_map scaffold wit "
        f"/tmp/v0/scaffold_bin.pb.gz {pb} /tmp/v0/m23/rp1/ids/part_ "
        f"/tmp/v0/m23/rp1/wit4/part_ 26 - 0 2>&1 | grep 'total ='").read()
    for line in r.splitlines():
        if "total = " in line:
            return int(line.split("total = ")[1].split()[0])
    raise RuntimeError("wit failed: " + r[-300:])


def bfs_depths(t, src):
    d = {src["id"]: 0}
    par = {}
    stack = [t]
    while stack:
        x = stack.pop()
        for c in x["children"]:
            par[c["id"]] = x
            stack.append(c)
    # BFS from src over the whole tree (undirected)
    from collections import deque
    q = deque([src])
    while q:
        x = q.popleft()
        for c in x["children"]:
            if c["id"] not in d:
                d[c["id"]] = d[x["id"]] + 1
                q.append(c)
        p = par.get(x["id"])
        if p is not None and p["id"] not in d:
            d[p["id"]] = d[x["id"]] + 1
            q.append(p)
    return d


def find_leaf(t, label):
    stack = [t]
    while stack:
        x = stack.pop()
        if not x["children"] and x["label"] == label:
            return x
        stack.extend(x["children"])
    raise ValueError(label)


def find_cherry(t, la, lb):
    stack = [t]
    while stack:
        x = stack.pop()
        ls = [c for c in x["children"] if not c["children"]]
        if len(x["children"]) == 2 and len(ls) == 2 and \
                {ls[0]["label"], ls[1]["label"]} == {la, lb}:
            return x
        stack.extend(x["children"])
    raise ValueError((la, lb))


def clone(t):
    """Iterative deep clone preserving node ids (deepcopy overflows on
    ~4k-deep caterpillar chains)."""
    memo = {}
    stack = [t]
    while stack:
        x = stack[-1]
        if id(x) not in memo:
            memo[id(x)] = {"id": x["id"], "children": x["children"][:],
                           "label": x["label"]}
        if all(id(c) in memo for c in x["children"]):
            memo[id(x)]["children"] = [memo[id(c)] for c in x["children"]]
            stack.pop()
        else:
            for c in x["children"]:
                if id(c) not in memo:
                    stack.append(c)
    return memo[id(t)]


_NEWID = [0]


def sweep_target(base_tree, tag, get_C):
    t0 = time.time()
    tree = clone(base_tree)
    C = get_C(tree)
    spot = kit.detach(tree, C)
    depths = bfs_depths(tree, spot)
    eds = kit.edges(tree)
    near = [(u, v) for (u, v) in eds
            if min(depths.get(u["id"], 99), depths.get(v["id"], 99)) <= D_NEAR]
    far_all = [(u, v) for (u, v) in eds if (u, v) not in near]
    far = far_all[::max(1, len(far_all) // F_FAR)][:F_FAR] if far_all else []
    log(f"{tag}: candidates near={len(near)} far={len(far)} "
        f"(edges total {len(eds)})")
    seen = set()
    best = (None, 10**9)
    for i, (u, v) in enumerate(near + far):
        w = clone(tree)
        # re-find nodes by id field (preserved by clone)
        u2 = find_node(w, u["id"])
        v2 = find_node(w, v["id"])
        C2 = clone(C)  # C is detached: clone its subtree directly
        _NEWID[0] -= 1
        kit.regraft(w, u2, v2, C2, _NEWID[0])
        s = kit.serialize(w)
        if s in seen:
            continue
        seen.add(s)
        sub = f"{tag}_{i:04d}"
        try:
            pb = rebuild(s + ";", sub)
            tot = wit(pb)
        except Exception as e:
            log(f"{tag}_{i}: FAILED {str(e)[:120]}")
            continue
        dd = min(depths.get(u["id"], 99), depths.get(v["id"], 99))
        if tot < best[1]:
            best = (sub, tot)
            log(f"{tag}_{i}: total={tot} NEW BEST (dist {dd})")
        if tot < 59715:
            open(f"/tmp/v0/m23/plF/keep/{tag}_{i}_{tot}.nwk", "w").write(s)
        elif tot < 59715:
            log(f"{tag}_{i}: total={tot} IMPROVES (dist {dd})")
        os.remove(pb)
        os.remove(f"{OUT}/{sub}.nwk")
    log(f"{tag}: DONE best={best[1]} best_tag={best[0]} "
        f"({time.time()-t0:.0f}s)")
    return best


def find_node(t, nid):
    stack = [t]
    while stack:
        x = stack.pop()
        if x["id"] == nid:
            return x
        stack.extend(x["children"])
    raise ValueError(nid)


def main():
    base_nwk = os.environ.get("BASE", kit.NWK)
    raw = open(base_nwk).read().strip()
    base = kit.parse_newick(raw.rstrip(";"))
    log(f"=== hill-climb iter on {base_nwk}")
    results = {}
    order = [("p02", lambda t: find_leaf(t, "__PART_2__")),
             ("p03", lambda t: find_leaf(t, "__PART_3__")),
             ("ch0203", lambda t: find_cherry(t, "__PART_2__", "__PART_3__")),
             ("p11", lambda t: find_leaf(t, "__PART_11__")),
             ("p12", lambda t: find_leaf(t, "__PART_12__")),
             ("ch1112", lambda t: find_cherry(t, "__PART_11__", "__PART_12__")),
             ("p09", lambda t: find_leaf(t, "__PART_9__")),
             ("p08", lambda t: find_leaf(t, "__PART_8__"))]
    if len(sys.argv) > 1:
        order = [o for o in order if o[0] in sys.argv[1:]]
    for tag, get in order:
        results[tag] = sweep_target(base, tag, get)
        json.dump(results, open("/tmp/v0/m23/plF/sweep.json", "w"), indent=1)
    log("SWEEP COMPLETE", results)


if __name__ == "__main__":
    main()
