#!/usr/bin/env python3
"""Phase F placement kit: newick surgery on tc26 + wit evaluation.

Tree model: rooted binary; nodes are dicts {children: [...], label: str}.
Leaves carry sample ids; part clades found by exact leaf-set match
(parts are clades in tc26 by construction; collapse_parts relies on it).

SPR surgery (binary-preserving):
  detach C (part clade or cherry parent):
    - parent p of C; p loses C; if p now has 1 child o, smooth p away
      (grandparent's child p -> o).  p's position = "old spot".
  regraft at edge (u, v):  new node m; u's child v -> m; m = (v, C).
Identity guards: never regraft adjacent to the old spot in ways that
reproduce the same topology (checked by newick string comparison).
"""
import os
import subprocess
import sys

R = "/home/ogi/matsen/larch2-wric-10datasets/build"
V0 = "/tmp/v0"
IDS = f"{V0}/m23/rp1/ids/part_"
WIT = f"{V0}/m23/rp1/wit4/part_"
NWK = f"{V0}/m23/rp1/tc26.nwk"


# ---------- newick parse / serialize ----------
def parse_newick(s):
    pos = [0]
    n = [0]

    def node(children, label):
        i = n[0]
        n[0] += 1
        return {"id": i, "children": children, "label": label}

    def subtree():
        if s[pos[0]] == "(":
            pos[0] += 1
            children = [subtree()]
            while s[pos[0]] == ",":
                pos[0] += 1
                children.append(subtree())
            assert s[pos[0]] == ")", f"expected ) at {pos[0]}"
            pos[0] += 1
            start = pos[0]
            while pos[0] < len(s) and s[pos[0]] not in ",();":
                pos[0] += 1
            return node(children, s[start:pos[0]])
        start = pos[0]
        while pos[0] < len(s) and s[pos[0]] not in ",();":
            pos[0] += 1
        return node([], s[start:pos[0]])

    t = subtree()
    return t


def serialize(t):
    if not t["children"]:
        return t["label"]
    return "(" + ",".join(serialize(c) for c in t["children"]) + ")" + t["label"]


# ---------- tree utilities ----------
def parent_map(t):
    par = {}
    stack = [t]
    while stack:
        x = stack.pop()
        for c in x["children"]:
            par[c["id"]] = x
            stack.append(c)
    return par


def leaves(t):
    out = []
    stack = [t]
    while stack:
        x = stack.pop()
        if not x["children"]:
            out.append(x)
        else:
            stack.extend(x["children"])
    return out


def mrca(t, target):
    """Node whose leaf-set == target (exact clade match), else None."""
    tgt = set(target)

    def dfs(x):
        got = []
        if not x["children"]:
            got = [x["label"]]
        else:
            for c in x["children"]:
                got.extend(dfs(c))
        if set(got) == tgt:
            x["_match"] = True
        return got

    dfs(t)
    stack = [t]
    while stack:
        x = stack.pop()
        if x.get("_match"):
            return x
        stack.extend(x["children"])
    return None


def detach(t, C):
    """Detach clade C (binary-preserving).  Returns the old-spot node
    (the smoothed position in the pruned tree) -- None if smoothing hit
    the root (then t is the pruned root with one child...)."""
    par = parent_map(t)
    p = par.get(C["id"])
    if p is None:
        raise ValueError("cannot detach root")
    p["children"].remove(C)
    if len(p["children"]) == 1:
        o = p["children"][0]
        gp = par.get(p["id"])
        if gp is None:
            # p was root: tree becomes o (reroot)
            raise ValueError("detach smoothed to root -- unsupported")
        gp["children"][gp["children"].index(p)] = o
        return o
    return p


def regraft(t, u, v, C, nid=-1):
    """Insert C on edge (u, v):  u->v becomes u->m->(v, C)."""
    assert v in u["children"]
    m = {"id": nid, "children": [v, C], "label": ""}
    u["children"][u["children"].index(v)] = m


def edges(t):
    """All (u, v) edges with v != root child list head order-free."""
    out = []
    stack = [t]
    while stack:
        x = stack.pop()
        for c in x["children"]:
            out.append((x, c))
            stack.append(c)
    return out


def subtree_size(x):
    return 1 + sum(subtree_size(c) for c in x["children"])


def load_target_parts():
    parts = {}
    for j in range(26):
        with open(f"{IDS}{j:02d}.ids.txt") as f:
            parts[j] = [l.strip() for l in f if l.strip()]
    return parts


# ---------- rebuild + evaluate ----------
def rebuild(nwk_str, tag, outdir):
    os.makedirs(outdir, exist_ok=True)
    p = f"{outdir}/{tag}.nwk"
    with open(p, "w") as f:
        f.write(nwk_str)
        f.write(";\n")
    pb = f"{outdir}/{tag}.pb.gz"
    r = subprocess.run(
        [f"{R}/wric_tension_map", "megatree", "fromnewick", p,
         f"{V0}/m23/rp1/tc26.pb.gz", pb],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("fromnewick failed: " + r.stderr[-300:])
    return pb


def wit_total(pb, expect=None):
    cmd = [f"{R}/wric_tension_map", "scaffold", "wit", f"{V0}/scaffold_bin.pb.gz",
           pb, IDS, WIT, "26", "-", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    for line in r.stderr.splitlines():
        if "total = " in line:
            tot = int(line.split("total = ")[1].split()[0])
            return tot
    raise RuntimeError("wit failed: " + r.stderr[-500:])


if __name__ == "__main__":
    t = parse_newick(open(NWK).read().strip().rstrip(";"))
    ls = leaves(t)
    print("leaves:", len(ls), "root label:", repr(t["label"]))
    parts = load_target_parts()
    allpart = set()
    for j, ids in parts.items():
        allpart.update(ids)
    lab = set(x["label"] for x in ls)
    print("part ids:", sum(len(v) for v in parts.values()),
          "unique:", len(allpart), "in tree:", len(allpart & lab),
          "frozen:", len(lab - allpart))
    # each part must be an exact clade
    for j, ids in parts.items():
        c = mrca(t, ids)
        print(f"part {j:02d}: clade={'YES' if c else 'NO'} size={len(ids)}"
              + (f" subtree={subtree_size(c)}" if c else ""))
