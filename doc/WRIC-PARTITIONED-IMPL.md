# Partitioned chart optimization — implementation guide

Continuation of `cross-profile-impl.md` (same premise: an implementer who
knows larch2's SPR functionality and the chart machinery of that document,
and nothing else).  This document covers the **partitioned optimization
line**: how a polytomic usher MAT becomes a certified near-optimal binary
tree at a scale where the monolithic chart cannot run, and the machinery
that certifies the result.  Final form only; retired experiments are named
as walls, not described.

All numbers are the startmat SARS-CoV-2 dataset (33,039 taxa, 29,903 sites),
unordered unit-cost free-label parsimony, `score_ua_edge` convention, on the
127 GB / 32-core box.  Repo: `/home/ogi/matsen/larch2-wric-10datasets`,
branch `fold-outside-demand-production-preflight`.  Tools live in
`tools/wric_*.cpp`; the tension-tool modes below are all
`build/wric_tension_map <mode> ...`.

## 1. Orientation: what partitioning adds to the chart

The chart of `cross-profile-impl.md` answers, exactly, "what is the best
tree inside this DAG?"  On a 33k-taxon fold-shaped grammar it cannot be
asked: the merged per-part DAGs alone are 66k clades × 15k patterns ≈ 95×
the HBV footprint, and the run dies at ~101 GB.  The structural fact that
makes partitioning principled rather than merely practical: after folding,
**only ~37 scaffold nodes exist outside the parts** — the object is 99.94%
decoupled, and the monolithic B&B would pay full multisite coupling freight
for coupling that is not there.

The partitioned line therefore:

1. carves the tree into **clade parts** whose interiors are optimized
   exactly by the chart, per part (cheap — the per-part footprints are
   small);
2. keeps a small **scaffold** (the tree with each part collapsed to one
   pseudo-leaf) on which **coupled DPs over the whole family are tractable**;
3. exports each part's coupling behavior as a compact **response-row
   interface** so the scaffold DPs never touch part interiors;
4. alternates **realization** (materialize a concrete tree tuple, get an
   upper bound U) with **relaxation** (composite DP, get a lower bound LB)
   until the bracket certifies what was wanted.

The certificate form is the bracket `[LB, U]`; every U quoted below is an
assertion-checked materialized tree, not a printout.

## 2. Stage 1 — from MAT to a workable binary (comb binarization)

A MAT is polytomic (startmat crown arity 436) and usher-labeled.  Two
measured facts:

- A polytomy under single-label scoring is a **constraint**: the tree's own
  score is 60,019.  Any binary resolution with *fixed* labels scores the
  same; with **free labels** the resolution's added internal nodes make it
  a relaxation — a left-comb binarization scores **59,887** (−132 with zero
  search).
- The comb is a **search-space commitment**, not an optimum.  The chart
  tiers later optimize over spans; the comb only fixes where the search
  starts.

Tools:

```
wric_binarize <in.pb.gz> <out.pb.gz> [out_ids.txt]     # tree-level left comb
wric_dag_binarize <in.pb.gz> <out.pb.gz> [--sample-first <wit>]
                                                      # DAG-level: comb per
                                                      # >2-clade production,
                                                      # clade-rank renumber
                                                      # (merged DAGs carry
                                                      # gaps), unary squash,
                                                      # CG-copy labeling,
                                                      # optional first-parse
                                                      # witness
```

Traps (all hit): `fitch_assign_compact_genomes` **deadlocks on DAGs** —
DAG labeling is CG-copy + `recompute_edge_mutations` (protobuf stores
mutations on edges; without recomputation leaf cgs collapse to the
reference and scores come out absurdly low).  larch2's SPR summary prints
are **pre-move** scores; post-run quality needs `--dag-info` or a chart
witness.

## 3. Stage 2 — partitioning

### 3.1 Size partition (the baseline carve)

```
wric_partition <tree.pb.gz> <min_leaves> <max_leaves> <out_dir> [min_part_leaves]
```

Greedy bottom-up edge cuts + first-fit-decreasing packing into connected
clades in `[min, max]` (23 parts of 506–3,000 leaves on startmat), covering
all taxa.  Writes per-part id files (`part_NN.ids.txt`) — these are the
graft contracts used by every later stage.

### 3.2 Tension map (where a boundary hurts)

The tension map localizes the cost of *cutting*: for a grammar G and a
witness T, it decomposes

    tension = witness total − composite per-pattern lower bound

clade-by-clade via a clamp chain, into integer **point masses τ** at
clades.  τ at clade C is exactly how much the coupled optimum loses by
being forced to treat C's interior with per-site-independent minima.  On
the monolithic startmat grammar the entire gap 41 was 13 masses: τ=18 at a
23,547-taxon crown, τ=5 at a 2,841 clade, the rest small — and
hierarchically nested.

```
wric_tension_map tension <grammar.pb.gz> <witness.pb.gz> <expected_total>
```

`expected_total = 0` skips the witness-score assert (use when the witness
was not produced by this grammar).  Arity-3 witnesses are rejected.

### 3.3 Carrier-respecting carve

The size partition cut the crown into 17 pieces — maximally violating the
tension structure.  The fix:

- **keep maximal τ-carriers whole** as single parts (the crown: SPR radius
  ladder + chart materialize paid −61, the largest single recombination
  bonus of the campaign);
- **hybrid complement** for the rest: the maximal carrier-*free* clades are
  provably safe to cut:

```
wric_pick_clade fragments  <tree.pb.gz> <excl_ids.txt> <min_leaves> <out.tsv>
wric_pick_clade preorder   <tree.pb.gz> <index> <out_ids.txt>   # τ-carrier extraction
wric_pick_clade restrict   <tree.pb.gz> <ids.txt> <out.pb.gz>   # spanning-subtree restriction
```

Ledger for stage 2–3 on startmat: size fold 59,821 → tension carve 59,798
→ +hybrid complement 59,778.  An SPR-only control under a comparable
partition lands at 59,846 in ~1.5 h — the chart tier (not the partitioning)
is what pays (25–68 units), at comparable wall time.

## 4. Stage 3 — per-part optimization and the fold

Per part: short native SPR to build a grammar (the run's explored DAG is
the union object), then the exact chart optimum + one witness via the
`cross-profile-impl.md` §5.1 materialize command, with `--chart-bnb-upper-
bound <SPR incumbent>` seeded.  On the 23-part fold this certified every
part at its span optimum in ~18 min total.

Fold by **clade graft**:

```
wric_subtree graft <witness.pb.gz> <ids.txt> <outdir> <cand1.pb.gz> [cand2...]
wric_subtree extract <witness.pb.gz> <ids.txt> <out.pb.gz>   # WRIC_GRAFT_ANY=1 for any-arity
```

Grafting is **clade-based**: a part whose ids are not a clade of the host
is a **silent no-op** (the graft "succeeds", the tree is unchanged) —
always check the score moved.  The practical fold is the **greedy graft
chain**: graft witnesses one at a time, rescore, keep.  Measured perfectly
additive (every in-context delta reproduced its isolated delta, ~3 units
total drift).

## 5. Stage 4 — the fold-family certificate machinery

This is the part that turns the fold from a heuristic into a method.
Artifacts per family: `ids/part_NN.ids.txt`, per-part **response rows**
`part_NN.spr.pb.resp` (text, one row per active pattern:
`gidx weight refA..T mA..T`, plus `# part <grammar path>` and
`# part_lb <ΣM_j>` headers), per-part witnesses `part_NN.wit.pb.gz`, and
the **scaffold** `tc26.pb.gz`-style pb whose leaves are the frozen taxa
plus one pseudo-leaf `__PART_j__` per part.

- **Response rows (m-rows)**: per part j and pattern g,
  `m[s]` = the cheapest cost of *any* tree in part j's grammar attached in
  root state s.  This 4-vector per pattern is the part's complete coupling
  interface; `Σ_j Σ_g w_g min_s m_j[s,g]` is the **composite lower bound**
  contribution.  Export:
  `wric_tension_map response <global.pb.gz> <outdir> <part1.pb.gz> ...`.
- **Three scaffold DPs** (`wric_tension_map scaffold <mode> ...`):

  ```
  scaffold lb  <global.pb.gz> <scaffold.pb.gz> <ids_prefix> <resp_prefix> <np> <out.profile> [expected] [mask_file]
  scaffold pin <global.pb.gz> <scaffold.pb.gz> <ids_prefix> <resp_prefix> <np> <profile> [expected]
  scaffold wit <global.pb.gz> <scaffold.pb.gz> <ids_prefix> <wit_prefix>  <np> - [expected]
  ```

  `lb` = free-state DP with m-rows (**composite LB**; also writes the
  argmin **profile** π̂: per (g, r), each part's attachment state).
  `pin` = the same DP with every part forced to a given profile column
  (exact pinned realization cost).  `wit` = the DP with each part's
  *witness tree's exact rows* — jointly free states; **this is the exact
  score U of the realized tree tuple**, assertion-checkable via `expected`.
  The certificate identity: `LB = pin(π̂) + Σ part_lb`, and
  `LB ≤ family optimum ≤ U`.
- **PartOpt (clamped exact per-part optima)**: pin a part to a profile
  column and get its exact optimum with a root clamp:

  ```
  wric_tension_map vleaf clampgen <global.pb.gz> <profile> <j> <part.pb.gz> <out.clamp>
  larch2 --dag-pb <part_grammar> -o <out_wit.pb.gz> --trim-mode chart-bnb -n 0 \
    --chart-bnb-score-only --chart-bnb-upper-bound <comp+80> \
    --chart-bnb-root-clamp <out.clamp> <j> \
    --chart-bnb-class-compress on --chart-bnb-profile-dedup on \
    --chart-bnb-trim-application optimal-topology-materialize \
    --chart-bnb-max-exact-topologies 1        # bnb_optimum: on stderr
  ```

  Also: `vleaf profgen <global> <wit> <out>` (witness root states),
  `vleaf ub <global> <witness> <profile> <j>` (composite UB at a column).
- **Full-tree materialization** (megatrees):

  ```
  wric_tension_map megatree build  <scaffold_tree.pb.gz> <global.pb.gz> <np> <wit_prefix> <out.pb.gz>
  wric_tension_map megatree expand <megatree.pb.gz> <wit_prefix> <np> <out_full.pb.gz>
  wric_tension_map megatree tonewick <in.pb.gz> <out.nwk>
  wric_tension_map megatree fromnewick <tree.nwk> <cg_source.pb.gz> <out.pb.gz>
  wric_tension_map megatree relabel <in.pb.gz> <pairs_file> <out.pb.gz>   # WRIC_CG_ALIAS=a=b,...
  wric_tension_map grammar restrict <dag.pb.gz> <ids.txt> <out.pb.gz>
  wric_tension_map grammar merge <a.pb.gz> <b.pb.gz> <out.pb.gz>
  ```

  `build` makes the pseudo-leaf megatree; `expand` swaps pseudo leaves for
  witness subtrees (a real full tree, scorable independently).
  `fromnewick` takes CGs by sample id from any tree pb (use the part
  witness as cg source); grammar `restrict`/`merge` operate on spans.

Newick traps (bit repeatedly): a subtree line must end with `;` *inside*
the line; a double `;;` creates a phantom empty leaf; `fromnewick` must
receive **no trailing newline** after the final `;` (same phantom-leaf
effect).  String-replace cherry surgery needs unique intermediate markers —
chained `.replace()` on overlapping labels clobbers.

## 6. Stage 5 — the refinement tiers (59,778 → 59,703)

Each tier is optional, in this order, and each has a measured wall where it
stops paying.

### 6.1 Scaffold topology (pinned SPR on the megatree)

Materialize the fold, then run native SPR on the megatree with parts as
point taxa (pseudo-leaves carry pinned CGs): multi-seed, radius ladder
2–512, ~2 min/round; startmat converged 8,348 → 8,307 in 9 rounds with a
clean 0-move plateau.  Re-collapse the result into a new scaffold; certify
with `scaffold wit ... <expected>`; re-derive LB on the new topology.
On startmat: **U = 59,726**, LB = 59,685, the 41-unit gap proven to move
with topology (part-internal).

### 6.2 Re-partition at τ-peaks

Re-run tension maps on carrier grammars; split parts exactly at localized
peaks (e.g., one 111-taxon clade holding an entire residual r=4).  Splits
must be clade splits of the scaffold (certificate-sound: they tighten the
relaxation from below and enlarge realization freedom); complements of
non-clade splits are built by newick prune + `fromnewick`; fresh child
grammars need an enrichment pass (chained SPR) to recover merge-era
richness.  **The wall** (established over three rounds): refinement is
**certification-side only** — parents' internal coupling is free-label
structure between the pieces, and any split converts it to pinned; U does
not drop from splitting, but LBs tighten (59,685 → 59,694 via children's
poorer rows).  Stop splitting when τ-maps localize nothing.

### 6.3 Free-row placement (relocate parts under the true objective)

The surrogate of §6.1 (parts as point taxa) mis-ranks placements by up to
the pinned-vs-joint slack (measured 79 units on startmat).  Placement moves
must be evaluated by `scaffold wit` on surgically modified scaffolds:
`megatode tonewick` → regraft the `__PART_j__` leaf (Python, binary-
preserving prune/regraft; iterative clone — `deepcopy` overflows on deep
caterpillars) → `fromnewick` → `scaffold wit`.  Validate the kit by an
identity round-trip reproducing U exactly.  Radius-6 + far-sample sweeps
per part, two hill-climb iterations: on startmat every part plateaued
except the **crown** (the largest surrogate-vs-fold correction), −1 →
**59,714**.  Plateau at fixed witnesses is the stopping rule.

### 6.4 Candidate tuples (diverse pools + exact full-assignment evaluation)

The last realization tier: replace a part's witness with any *other*
optimal tree from its grammar and score the whole tuple exactly.

1. **Pools** per part grammar: `larch2 --dag-pb <grammar> -o <pool.pb.gz>
   --diverse-sample 16 --diverse-newick <pool.nwk>` (parsimony-optimal,
   maximally-diverse; one line per tree), then `fromnewick` each line with
   the part witness as cg source.  Measure pool richness: on startmat 8 of
   26 grammars yielded 16 distinct trees, 9 were degenerate.  Candidates
   can be *better free* than the incumbent witness (enriched grammars
   outrun their witnesses) — always rescore.
2. **Mixed-row exact evaluation**: `WRIC_MIXED=<cfg>` on `scaffold lb`,
   where cfg lines are `j m` (m-row envelope) or `j w <tree.pb.gz>` (that
   tree's exact rows).  At **full assignment** the DP equals `scaffold wit`
   on that tuple exactly (fixture: all-wit must reproduce U bit-for-bit);
   at partial assignment it is a diagnostic only — see §9.
3. **One-away hill-climb**: for every part j and candidate k, evaluate the
   full assignment {incumbent tuple, j→k}; adopt improving swaps greedily,
   re-sweep; stop at a zero-improvement sweep.  On startmat: slot-09 swaps
   −8 (59,714 → 59,706), then −1 chains (slots 05, 06, 21) to **59,703**,
   converged (156 ties, 24 near-misses, zero below).

A best-first B&B over partial assignments is *not* worth it: the front
grows +pool-size per expansion and stalls ~20 below the incumbent (the
tuple tier's relaxation wall); the one-away sweeps realize the same promise
directly.

## 7. The record pipeline (startmat, end to end)

```
# 0. input: startmat_no_ancestral.pb.gz (polytomic, 60,019 single-label)
wric_binarize  in.pb.gz scaffold_bin.pb.gz scaffold.ids      # 59,887 free-label
wric_partition scaffold_bin.pb.gz 500 3000 parts/            # size carve (or §3.3)
wric_tension_map tension spr1_bin.pb.gz spr1_wit.pb.gz 0     # τ-map → carriers
wric_pick_clade fragments ...                                # hybrid complement
# per part j: SPR grammar + certified witness (cross-profile §5.1 command)
wric_subtree graft ...                                       # fold chain
wric_tension_map response scaffold_bin.pb.gz resp/ parts/*.spr.pb.gz
# family artifacts: ids/ resp/ wit4/ tc26.pb.gz (+ megatode build for §6.1)
wric_tension_map scaffold lb ... 59672-mask0                 # composite LB + π̂
wric_tension_map scaffold wit ... - 59703                    # realized U, assert
```

Ledger (all U's assertion-checked):

| step | U | LB (family) |
|---|---|---|
| input (polytomic, single-label) | 60,019 | — |
| comb binarization | 59,887 | 59,887 |
| size fold (23 parts) | 59,821 | 59,751 (Σ part optima) |
| tension carve + hybrid | 59,778 | — |
| scaffold topology (§6.1) | 59,726 | 59,685 |
| re-partition (§6.2) | 59,715 | 59,694 |
| crown placement (§6.3) | 59,714 | 59,672 |
| candidate tuples (§6.4) | **59,703** | 59,672 |

Baselines: monolithic native SPR (3 h) span-floor 59,833; SPR-only
partition control 59,846; comb 59,887.  Final bracket **[59,672, 59,703]**,
gap 31.

## 8. Measured costs (canonical runs, 32-core / 127 GB box)

| operation | cost |
|---|---|
| comb binarize + tension map (monolithic grammar, 109k nodes) | min |
| per-part SPR + chart materialize (23 parts) | ~18 min batch |
| crown whole (SPR radius ladder 1024 + materialize) | ~3 h, 92 GB peak |
| graft chain | ~67 s/step |
| response export (26 parts) | min |
| scaffold lb/pin/wit | 35–60 s each |
| PartOpt clamp trim (non-crown parts) | s–min each; crown hours |
| placement sweep | ~35 s/eval; 25/target; 2 iterations ≈ 4 h |
| pool build + convert | ~12 s/part (crown: aborted at 4 h) |
| one-away sweep | ~35 s/leaf; 180 leaves ≈ 1.7 h/iteration |

Parallelism: every scaffold DP and trim is an independent process (~1–2 GB
RSS each); 12–16 concurrent streams fit the box and near-linearly speed the
sweeps.  Watch `free` first — kernel OOM-reclaim kills tenants, and the
monolithic-chart OOM is a designed wall, not an accident.

## 9. Contract boundaries and honest limits

- **U is always real**: every upper bound is a materialized tree tuple,
  `scaffold wit`-asserted.  **LB is always all-m composite**: partial
  mixed-row "bounds" are diagnostics — the resp m-rows are envelope-of-span
  in practice, **not pointwise minima over every tree** (measured:
  assigning winners lowered a mixed run below the composite LB).
- **No fixed profile realizes U**: the joint per-site freedom is worth ~2×
  the composite gap (measured 79 vs 42 on startmat).  Any tier that pins
  profiles (masks, column moves, swaps) is structurally capped above U;
  realization must go through `scaffold wit`-style joint DPs.
- **Refinement walls** (measured, respected): re-partition never lowers U
  (certification-side only); the tuple B&B front stalls below U; one-away
  convergence means further gains need ≥ 2 simultaneous swaps or trees
  outside the pools; 9/26 pools degenerate; the crown's pool was never
  sampled (4 h budget exhausted).
- **Grafts are clade contracts**; scattered id sets are silent no-ops.
  Assert the score moved.
- **Score conventions**: `scaffold wit`/`lb` totals are exact; larch2 SPR
  prints are pre-move; the tension tool's `witness total score` is exact
  free-label.  Never compare across conventions without a rescoring pass.
- **Operational traps**: `pkill -f` self-matches wrappers (kill by exact
  PID); `/tmp` reapers delete artifacts (protect under `work/`); ps
  `%CPU`/etime unreliable; `--chart-bnb-score-only` still needs `-o`;
  chart-bnb on megatrees must seed `--chart-bnb-upper-bound`.

## 10. Small-scale validation (seedtree, 597 taxa) and the scale boundary

The full line was run end-to-end on `~/matsen/larch2/data/seedtree/
seedtree.pb.gz` (597 taxa, polytomies to arity 10, fixed-label parse
1,642).  Mechanics: comb 1,637 -> clade carve 5 parts (146/131/83/50/38)
+ 149 frozen -> per-part SPR + chart (318/308/204/173/119, Sigma 1,122)
-> graft fold **U = 1,635** -> response rows -> **scaffold lb = 1,635 =
U, gap 0**: the fold-family optimum certified exactly in one shot, with
the realization triple-validated (graft chain = `scaffold wit` assert =
`megatree build`/`expand` full-tree rescore, all 1,635).

Four traps surfaced that the big campaign never hit — all now part of
the recipe:

1. **`wric_partition`'s FFD packing can emit non-clade parts on small
   trees** (unions of distant twigs sized into the window): grafts then
   silently no-op.  Carve clades from the POLYTOMIC original instead
   (the comb preserves clade leaf sets), and liveness-check every graft
   (the score must move or the part must be verifiably neutral).
2. **Binarize destroys natural clades**: the comb's caterpillar chains
   leave almost no clades >= 60 leaves — carve BEFORE binarizing, then
   `wric_binarize` each part newick.
3. **Unary productions**: extracted parts and SPR grammars can carry
   unary nodes; the chart refuses ("non-UA unary production"), and SPR
   on a unary-carrying input can corrupt the DAG ("child appears in
   multiple clades").  Squash with `wric_dag_binarize` BEFORE the
   chart -- of the input too, not just the grammar.
4. **wsub-emitted trees load with `--dag-pb`**, not `--tree-pb`.

The scale boundary, measured honestly: the MONOLITHIC chart runs fine
at 597 taxa and lands at **1,616** (1,637 SPR incumbent -> chart
recombination, matching the earlier campaign's seedtree record) — **19
below the fold family**.  The partitioned certificate is exact but the
family is poorer (fixed scaffold topology, 1-seed part grammars, 149
frozen leaves).  Use the partitioned line where the monolithic chart
cannot run (startmat: OOM at ~101 GB; here: seconds) or where its tiers
(tension carve, placement, tuples) pay; on small datasets run
monolithic first and use this line as the certificate backbone.

### 10.1 The three-dataset turnkey table (same recipe, timed)

| dataset | leaves | fixed parse | comb | partitioned U / LB | t_part | monolithic | t_mono |
|---|---:|---:|---:|---|---:|---:|---:|
| seedtree | 597 | 1,642 | 1,637 | **1,635 = LB** (gap 0) | 8.0 s | **1,616** | 5.9 s |
| wnv (zenodo) | 794 | 9,421 | 9,417 | 9,389 / 9,380 (gap 9) | 14.1 s | **9,387** | 15.9 s |
| hbv (zenodo) | 1,032 | 14,524 | 14,515 | 14,428 / 14,245 (gap 183) | 25.7 s | **14,418** | 20.9 s |

(wnv/hbv are the zenodo `data/*.pb` fixtures — the campaign bench's own
records agree with these loads node-for-node; the record pipelines of
cross-profile-impl.md §5 used other variants of these datasets.)

Readings: (1) at <= ~10^3 taxa the two arms are at runtime PARITY
(8-26 s) and the monolithic arm always scores >= the fold; (2) the
partitioned certificate stays exact everywhere (assertion OK each
time), and its LB gap REPRODUCES the campaign's coupling story — 0 on
seedtree, small on wnv, and 183 on hbv, the dataset whose coupling gap
(340 on the record variant) motivated the tension/injection tier; (3)
the input-loading quirk: `--tree-pb` on these raw pbs aborts at exit
after a successful write — the `--dag-pb` fallback loads identically
(the bench fixtures confirm), but pin the loader you use.

## 11. What is deliberately not covered

The in-coupled mask master (Architecture A) and the profile tier are
retired (capped by the 79-unit joint freedom); the crown frontier-width
attack and root-entry-class branching were designed but never built — the
bracket [59,672, 59,703] is where the line stands.  The HBV/WNV pipelines
of `cross-profile-impl.md` §5 remain the reference for the per-part chart
call; nothing in this document changes them.
