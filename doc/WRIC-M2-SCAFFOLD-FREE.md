# WRIC M2 — scaffold-free fold

Status: planned (2026-10-24).  Follows `WRIC-V1-KERNEL-PLAN.md` (M0 response
export, M1 scaffold-fixed coupled solve) and
`WRIC-CONDITIONED-MATERIALIZE.md` §9 (Phase 3, corrected instruments).

## 1. Goal and baselines

M1 certified the fold against a FIXED scaffold binary tree
(`scaffold_bin.pb.gz`, inherited from the split-tension re-partition):
**[59,726, 59,767]**, gap 41 = 1 (part-23 pin deviation) + 33 (non-crown
residuals) + 7 (crown residual).  That gap is part-INTERNAL (parse coupling
`inside_j - M_j`); it is not attacked by M2.

M2 attacks what M1 left structurally frozen: **the scaffold topology
itself**.  The collapsed scaffold (24 part pseudo-leaves + 4,197 frozen
real leaves + internal structure) has never been optimized as a whole;
its edges contribute ~8,348 pinned units.  M2 searches collapsed-megatree
topologies directly, re-conditions the parts exactly on each candidate,
and re-certificates with the coupled DP on arbitrary topologies.

| baseline | value |
|---|---|
| comb (all-pairs graft chain, v0) | 59,887 |
| SPR-only control (full 29,903-leaf tree) | 59,846 |
| v0 fold bracket | [59,781, 59,820] |
| M1/M2-entry certificate (fixed scaffold) | **[59,726, 59,767]** |
| scaffold-side pinned cost (pi-hat) | 8,348 |

Primary success: a certified realization **U < 59,767**.  Secondary: an
honest map of scaffold-topology slack (how many units the collapsed SPR
search recovers).  Non-goal: closing the 41-unit part-internal gap (that
is the in-coupled master's business, `WRIC-CONDITIONED-MATERIALIZE.md`
§10 discussion; Architecture B converges with M2's certificate layer).

## 2. Architecture

### 2.1 The collapsed megatree

`T_c`: a tree over 24 pseudo-taxa (one per part, labeled by sample id
`__PART_jj`) plus the 4,197 frozen real leaves.  Every instrument needed
already accepts it:

- `wric_tension_map scaffold {wit,lb,pin} <global> <T_c> <ids_prefix>
  ... ` — collapse_parts + the per-(g,r) DPs are scaffold-agnostic
  (M1 ran them on `scaffold_bin.pb.gz`; any topology loads via
  `load_tree_any`).
- The profile export (now **part-index column order**, post-§9.1 fix)
  re-derives pi-hat(T) for any T.

### 2.2 Profile CGs and native SPR

Per part j and profile column, `build_profile_cg` (vleaf machinery)
builds a pseudo-leaf compact genome: per position, label =
pi(g, r(site)).  Materialize T_c as a small pb DAG (4,221 leaves) and run
**native SPR** (`larch2 --dag-pb ... -o ... -n 1 --max-moves 50`) —
arity-unlimited, minutes at this size.  The pinned tree score
(parsimony over pseudo CGs + frozen CGs, UA edge vs reference) is the
search objective; it equals scaffold_at_profile(pi, T_c) exactly, so
during search with pins held fixed the inside terms Sigma_j inside_j are
constant and the SPR objective ranks topologies faithfully for THAT
profile.  Profile re-export after moves restores joint fidelity.

Known traps (all previously hit, all handled):
- arity <= 63 for any chart-based scoring of T_c
  (`polytomy_opts expand_soft_bounded,max_shapes_per_polytomy=1`);
- the materializer binarizes augmented roots — strip unary nodes after
  any chart-materialized topology (`vleaf strip` semantics);
- monolithic-chart OOM does not occur by construction (the full-grammar
  chart is never built; T_c is small).

### 2.3 The iterate

```
T <- T0 (current scaffold, collapsed)
repeat:
  1. pi_hat(T)  <- scaffold lb export on T            (seconds)
  2. CGs        <- build_profile_cg per part column   (seconds)
  3. T'         <- SPR(T_c with CGs, multi-restart)   (minutes)
  4. if pinned_score(T', pi_hat(T)) < pinned_score(T, pi_hat(T)):
       a. pi_hat(T') <- re-export on T'               (seconds)
       b. re-condition parts whose columns changed:
          clamp trim at the new pins (cheap parts seconds-minutes;
          crown ONLY if its column changed — diff first: crown pins
          were identical to its witness-argmin at T0, re-clamp is 6.4 h)
       c. U(T') <- scaffold wit over the part witnesses   (seconds)
       d. T <- T'; keep min-U incumbent
  5. plateau -> tension map + context SPR at collapsed granularity
until no improvement
```

Step 4b is the only place per-part exactness enters, and it reuses the
Phase-2/3 clamp pipeline wholesale (clampgen against the new profile,
`--chart-bnb-root-clamp ... --chart-bnb-profile-dedup on`, cold trace,
clamped-oracle validation).  Realization certification is always the
`scaffold wit` DP over actual part witness trees — never a composite.

### 2.4 Certificate semantics under topology search

Each topology T gives a per-topology bracket [LB(T), U(T)] where LB(T) is
the free coupled DP on T.  The fold-wide claims are:

- **U side (valid globally)**: any realized witness set is a fold tree;
  min over all T explored is a valid upper bound on the fold optimum.
- **LB side (topology-conditional)**: LB(T) bounds the optimum *within*
  T.  A fold-wide LB requires min over topologies, which search does not
  exhaust — same epistemic status as every SPR-based U in this campaign.
  The certificate line becomes: fold optimum <= U_best (certified
  realization), and, for the incumbent T*, optimum-within-T* in
  [LB(T*), U(T*)].  Report both; do not fold them into one bracket
  without saying which claim is made.

## 3. Milestones

### M2.0 — instrument preflight on T0 (0.25 d)

Regression-first, no search yet.
- Fixtures: `scaffold pin` on T0 with fold_lb3 must reproduce 8,348
  (pin + Sigma M == LB identity); `scaffold lb` export on T0 must
  reproduce fold_lb3 byte-identically (part-index columns); `scaffold
  wit` on the M1 witness set must reproduce 59,767.
- Build the T0 collapsed megatree pb with profile CGs; verify its native
  parsimony score == pinned scaffold cost 8,348 (this validates the CG
  construction and the UA edge convention).
- Probe: single-restart SPR on T0's megatree, pinned score delta.  This
  measures scaffold slack immediately and calibrates expectations before
  any conditioning work.  If delta == 0 after full search, M2's search
  tier is dead and the plan stops here with a documented negative.

### M2.1 — pinned SPR loop (0.5-1 d)

Steps 1-3 + acceptance on pinned score only; realization certificate at
each accepted T' via `scaffold wit` with CURRENT witnesses (no
re-conditioning yet).  Deliverable: first certified U < 59,767 (or a
measured plateau), plus the T'-family map.  Multi-restart SPR + seed
sweep as in the SPR-only control.

### M2.2 — conditioned re-solve (0.5 d)

Step 4b: re-export profiles on the best topologies; clamp the parts whose
columns changed (cheap-first); crown guarded by the column-diff rule.
Identity check per accepted T': pin(T) + Sigma M(T) == LB(T) exactly (the
per-(g,r) argmin property must hold for every exported profile — it is
now guaranteed by the corrected export + walkcheck).  Deliverable: the
final bracket family {([LB(T), U(T)])} with U_best certified.

### M2.3 — tension-guided collapsed moves (0.5 d, optional)

At plateau, run the tension map machinery on the collapsed megatree (it
is a valid small DAG with CGs) and context-aware SPR rounds exactly as in
the plateau-breaking campaign — all tooling exists
(`wric_tension_map`, `wric_context_spr`).  Collapsed granularity means
each tau-carrier is a part boundary, so moves are directly interpretable
as re-partition candidates: feed accepted moves back as re-partition
suggestions (closing the loop with WRIC-V1-KERNEL-PLAN §7).

## 4. Risks

- **Scaffold slack may be ~0** (T0 could already be collapsed-optimal;
  the M2.0 probe decides cheaply).  Honest negative documented if so.
- **Crown column churn**: a topology change that moves the crown's
  attachment region may change its pins, forcing a 6.4 h re-clamp.  Rule:
  diff columns first; if the crown column changes and the incumbent is
  already >= the projected U, skip the topology (branch-and-bound style).
- **Polytomy/arity**: collapsed trees after SPR are naturally polytomic;
  keep arity <= 63 before any chart scoring; strip unary after
  materializations.
- **Certificate inflation**: do not mix per-topology LBs into a global
  claim (§2.4).
- **Bash/script traps as usual**: done-markers on witness files, UB
  sources (`at_profile` not `vleaf_ub`), stderr for DP totals.

## 5. Cost and order

M2.0 0.25 d -> M2.1 0.5-1 d -> M2.2 0.5 d -> M2.3 0.5 d.  Total 1.75-2.25 d
compute-light (crown re-clamps the only heavy items, and they are
guarded).  Everything else is seconds-to-minutes per evaluation.

## 6. Relation to the in-coupled master

The coupled DP on arbitrary topologies IS Architecture B's substrate
(`WRIC-CONDITIONED-MATERIALIZE.md` §10).  If M2 succeeds, adding
per-part frontier candidate sets (branch on parses, candidate-min rows)
upgrades the certificate layer to the master without new plumbing; if M2
plateaus at U_best with residual gap g, the master's cheap stage runs on
top of the SAME T* machinery for the 23 non-crown parts.  Sequence M2
first: it moves U (the certificate's visible side), needs no crown
branching, and every instrument it needs already exists and is validated.

## 7. M2.0 executed (2026-10-24): all fixtures pass, probe POSITIVE (14 units)

Fixtures (all on T0 = scaffold_bin, profile fold_lb3):

1. `scaffold pin` total = **8,348** (pin + Sigma M == LB identity, unchanged).
2. `scaffold lb` re-export = fold_lb3 **byte-identical**, total 59,726.
3. `scaffold wit` on the Phase-3 mixed witness set = **59,767**.

New instrument: `wric_tension_map megatree build <scaffold_tree> <global>
<ids_prefix> <np> <profile> <out.pb.gz>` — collapse_parts + per-column
`build_profile_cg` pseudo CGs + frozen-leaf CGs, emitted via
`wsub::write_tree` (8,442 nodes = 4,221 leaves + 4,220 internal + UA,
perfectly binary; UA reference = global reference).

Megatree score validation (TWO independent scorers agree):
- `tension mega0 mega0` witness total = **8,348** == pin(pi-hat) exactly
  (CG construction + UA-edge convention validated);
- native SPR's own iter-1 DAG parsimony = **8,348**.

Note: chart-bnb score-only on the megatree WITHOUT `--chart-bnb-upper-bound`
wandered >15 min (wide frontier over ~30k near-singleton patterns) — always
seed the UB (megatree active patterns = 4,718 after CG collapse; the parts'
global pattern space was 14,962).

**Probe (single SPR restart, pinned pi-hat CGs, 368 improving moves applied
across radii 2-512, 2m18s wall)**: mega1 DAG parsimony (exact DP readout via
a second SPR invocation's iter-1) = **8,334 — Delta = 14 units of
scaffold-topology slack with pins held fixed.**

Interpretation and projection: part insides are scaffold-independent, so
U(T1, pi-hat) ~= 59,768 - 14 = **59,754** before label re-optimization
(re-export of pi-hat(T1) may move more), and the slot-23-style swaps
re-derive on the new topology in M2.1.  The search tier is ALIVE; proceed
to M2.1.  Artifacts: /tmp/v0/{mega0,mega1}.pb.gz, /tmp/mega_spr.{out,time},
/tmp/mega2_spr.time.

## 8. M2.1 executed (2026-10-24): U = 59,726 (41 units), pinned search
## converged

New instruments: `megatree expand <megatree.pb.gz> <wit_prefix> <np>
<out_full.pb.gz>` (pseudo leaves -> part witness subtrees -> full fold
tree; validated on T0: reproduces 59,767 exactly) and `megatree
fromnewick <tree.nwk> <cg_source.pb.gz> <out.pb.gz>` (parses larch
newick, CGs by sample id from a megatree pb).

Descent at fixed pi-hat CGs (fold_lb3): seed sweep 1/2/3 all reach 8,334
on mega0 (robust attractor); iterative rounds (seed 7/11/13/17/19/23/29,
~2 min each, 50 moves/radius 2-512): 8,348 -> 8,334 -> 8,321 -> 8,316 ->
8,314 -> 8,312 -> 8,310 -> 8,309 -> 8,307; round 9 applied **0** improving
moves -- clean plateau at **8,307** (41 units of pinned scaffold slack).

Materialization: chart-bnb on the merged megatree DAGs is slow (wide
frontier); used instead `--diverse-sample 24 --sample-method parsimony
--diverse-newick` on the final DAG (pool = parsimony-optimal trees);
`fromnewick` tree #1 pins at **8,307 = the DAG optimum** (exact scaffold
pin DP).

**Certification on T1** (tree #1 expanded with the Phase-3 witness set
mix23): `scaffold wit ... 24 - 59726` -> **total = 59,726 (expected
59726) OK** -- the assertion-checked realization.

- U improves 59,767 -> **59,726** (41 units; 120 below the SPR-only
  control, 161 below comb).
- `scaffold lb` on T1: **LB(T1) = 59,685** = pin(T1, pi-hat) + Sigma M =
  8,307 + 51,378 exactly; **pi-hat is topology-stable** (0 of 14,962
  (g,r) rows change between T0 and T1) -- the identity holds on T1.
- The within-T1 bracket is **[59,685, 59,726]**, gap 41 -- the SAME 41,
  because the residuals r_j = inside_j - M_j are part-internal and
  scaffold-independent (predicted by the plan; now measured).  The gap
  moved with the topology rather than closing: T0's bracket [59,726,
  59,767] and T1's [59,685, 59,726] share the same part-side 41.
- The pinned-SPR loop has converged: with CGs fixed at pi-hat, no
  improving moves remain (r9: applied=0), and pi-hat(T1) = pi-hat(T0)
  means re-pinning restarts nothing.  Further descent needs either the
  free-row objective (m-rows at pseudo leaves inside SPR -- new
  machinery) or the in-coupled master on T1's parts.

Operational notes: ps etime garbage bit us again (stale-looking r9
launch silently never started; foreground re-run fine); --diverse-newick
requires the merge pipeline path (--max-moves 0 keeps the DAG frozen);
tension-mode scoring rejects arity-3 witnesses (use scaffold pin / wit
instead, which accept any arity).

## 9. M2.3-lite executed (2026-10-24): tree-level gap-0 is vacuous; the
## DAG-level map ranks the span's unused freedom

FIRST RESULT, RETRACTED same day: the tension map on the plateau
megatree t1 printed **zero tau-carriers** (gap = 0, wrote 0 targets).
For a TREE grammar this is vacuous by construction: per-site root-label
choices are free, so the composite of a tree grammar equals the tree's
exact score and the gap is identically zero.  It certifies nothing about
other topologies.  (Kept here because the retraction is instructive.)

CORRECTED RESULT -- tension map on the SPR DAG (m21/mega_r8.pb.gz, the
full 9-round merged span) with witness t1 (8,307):

- witness total = **8,307**; composite strictly below; tau-carriers
  ranked: clade 9126 (**tau=6**, 171 taxa), 13499 (tau=4, 3,831 taxa),
  13761 (tau=2, root, 4,221 taxa), 10308 (tau=2, 258 taxa), 6871 (tau=1,
  9 taxa), then a tau=1 tail.
- The IN-SPAN optimum is exactly 8,307 (the SPR iter-1 readout is the
  exact DAG-grammar DP): no recombination within the explored span can
  beat it.  Breaking the plateau therefore requires OUT-OF-SPAN
  topologies, and the tau-ranked clades are the protocol's directed
  candidates for exactly that (injector replaces a subtree with a
  topology the span never contained).
- Matched-budget injector runs are the remaining probe: megatree-scale
  context evals (~8.4k nodes x ~30k positions each) run ~2 orders slower
  than the HBV-scale targets the tool was tuned on -- a 598-taxon clade
  exceeded 4.5 h at starts=5/sweeps=15 (killed, no artifacts).  The
  tau=6 / 171-taxon top target (m23/t9126.ids.txt) is running at
  starts=20/sweeps=30; verdict appended when it lands.

Matched-budget control (protocol formality): context-SPR injector on the
598-taxon clade adjacent to the crown pseudo-leaf (mid clade of the
crown-parent subtree).  Full-budget runs (starts=50/sweeps=60) exceed 60
min without completing -- each context eval is a full-megatree Fitch
pass (~8.4k nodes x ~30k positions), far heavier than the HBV-scale
targets the tool was tuned on.  A reduced-budget run (starts=5,
sweeps=15) is running; verdict appended below.

Operational notes: two `setsid nohup ... &` launches silently never
started (no files created) -- the `(setsid nohup ... &)` subshell pattern
is reliable, plain `... &` is not, on this box; the context-SPR log is
end-buffered (no progress stream).
