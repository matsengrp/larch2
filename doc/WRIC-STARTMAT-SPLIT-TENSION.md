# WRIC: split–optimize–fold and tension-guided partitioning on startmat

Campaign of 21–22 Oct 2026 on the startmat SARS-CoV-2 dataset (33,039 taxa,
29,903 sites). All scores are exact binary free-label parsimony unless a row
says otherwise. Working artifacts under `/tmp/v0/` (see §8).

## 1. Result summary

| pipeline | end score | Δ vs 59,887 | wall |
|---|---|---|---|
| monolithic native SPR (1 iter, 300 moves, own convention) | 59,916 | (−103 own conv.) | ~3 h |
| monolithic native DAG, binary span floor (composite LB) | 59,833 | — | 154 s |
| v0: size partition, 23 parts, graft chain | 59,821 | −66 | ~1 h |
| tension partition, 2 τ-carriers | 59,798 | −89 | ~2.9 h |
| **tension + hybrid complement (12 fragments)** | **59,778** | **−109** | ~3.3 h |

The tension-guided result beats the size partition by 43 and sits 55 below the
monolithic DAG's entire binary span floor.

## 2. Conventions (measured, not assumed)

- The usher input tree is polytomic (max arity 436). Its **single-label
  polytomic** score is 60,019 (`dagutil --dag-info`, single parse). Comb-
  binarizing the same tree (free labels) scores **59,887**: −132 with zero
  search, purely label freedom unlocked by resolution (fixed-label resolution
  is score-neutral; see WRIC notes on the {A,A,A,B,B} example).
- Native SPR's summary prints are **pre-move** values (`min_score` and the
  objective are both computed on the pre-move sampled tree; 59,946 relaxed /
  60,019 objective). The run's actual quality shows only in the output DAG:
  `--dag-info` gives min-weight parse 59,916 (own convention) out of 1.57e37
  optima; in binary terms its first parse is 59,874 and its composite floor is
  59,833.
- Native move enumeration defaults to `score <= -1`: **strictly improving
  moves only** (the MADAG neutral-acceptance flag was not used).
- Materialized chart witnesses are strictly binary; the chart never emits or
  creates polytomies (grammar prep refines ≤63-arity polytomies; the 436-crown
  required external binarization).

## 3. v0: split–optimize–fold (size partition)

Partition (`wric_partition`): 23 connected parts, 506–3,000 leaves, covering
33,032/33,039 taxa; Σ site patterns 40,608 vs 14,966 global.

Per part: comb-binarize (`wric_binarize`) → native SPR (1 iter, 50 moves,
seed 1) → chart B&B materialize (canonical flags, UB = SPR incumbent).
Σ incumbents 59,950 → Σ certified optima **59,751** (every part improved,
1–19 each); batch ~18 min. 3-seed enrichment (seeds 2+3 merged per part) was
a **complete no-op**: identical incumbents and identical optima on all 23
parts — shallow part-scale SPR is seed-robust.

Fold: splice-graft each part witness into the binarized scaffold
(`wric_subtree graft`; splice replaces the node whose taxon set equals the
ids — parts must be **clades of the host**), then dagutil same-leaf-set merge
(23 grafts + scaffold = 66,847-node fold, parses = independent part choices).

- The monolithic chart on the fold **OOM-killed at 1,213 s (~101 GB)** on the
  127 GB box (66k clades × 15k patterns ≈ 95× the HBV footprint). Structural
  reason: only **37 scaffold nodes exist outside the parts** — the fold is
  99.94% decoupled and the monolithic B&B pays full coupling freight for
  structure that is not there. (Side effect: the kernel's reclaim killed two
  tenant processes; see §7 lesson.)
- The **greedy graft chain** replaces it: graft witnesses one at a time,
  rescore (33 s via the tension-tool score trick), keep. Perfectly additive
  (every in-context delta reproduced its isolated delta; ~3 units total
  drift): 59,887 → **59,821** (12 switches, ~67 s/step). Exact optimum over
  all switch combinations remains bracketed [59,751, 59,821]; certifying it
  exactly is the v1 kernel (per-part response rows + 37-node coupled solve).

## 4. Tension measurement on the monolithic SPR grammar

`wric_dag_binarize` (new): DAG-level left comb per >2-clade production,
clade-rank renumbering (merged DAGs carry gaps), unary-squash expansion lists
(none present), CG-copy labeling (tree-Fitch hangs on DAGs — hard lesson),
optional first-parse sampler. 45,931 → 108,972 nodes, 5,218 combs.

Tension map (`wric_tension_map tension`, grammar + first-parse witness):

- witness 59,874; composite inside LB 59,833; **gap 41**, decomposed by the
  clamp chain into exactly **13 point masses** (Σ τ = 41):
  τ=18 at a 23,547-taxon crown clade (12,678/14,962 classes); τ=5 at 23 taxa;
  τ=3 at 183; seven τ=2 clades (119–1,868); τ=1 down to 20 taxa. All other
  ~66,064 clades carry zero.
- The carriers are **hierarchical**: 11 of 13 nest into two lineages; the
  maximal carriers are the crown (23,547) and clade 54132 (2,841).
- Cross-result: v0's 59,821 was already below the SPR grammar's composite
  floor (59,833) — the part-certified trees beat the whole comb-resolved
  monolithic span.

## 5. Tension-guided re-partition + hybrid

Overlap analysis: the size partition cut the crown into **17 pieces** (15
parts fully inside) and scattered carrier 54132 across parts 08/10/13 —
maximally violating the tension structure.

Tension chain (scaffold = SPR first parse, 59,874):

- carrier 54132 (2,841): incumbent 5,548 → certified 5,533; grafted −15 →
  59,859.
- crown (23,547, whole): SPR 2 h, full radius ladder to 1024, 500/500 moves;
  materialize 49 min at **92 GB peak** (cross-profile dominance on as memory
  insurance): incumbent 42,182 → certified **42,121 (−61**, the campaign's
  largest single recombination bonus, exactly where τ=18 sat); grafted −61 →
  **59,798**.

Hybrid complement (v0.5): `wric_pick_clade fragments` emits the maximal
carrier-free clades of the tension tree (12 fragments ≥100 leaves, 3,904
leaves; 2,747 leaves in sub-100 twigs stayed frozen). Same per-part pipeline,
chained: −20 → **59,778** (~9 min). Perfectly additive again.

## 6. Tooling added (this campaign)

- `tools/wric_binarize.cpp` — tree-level left comb, any-arity load, ids out.
- `tools/wric_dag_binarize.cpp` — DAG-level comb + rank renumber + unary
  squash + `--sample-first` parse witness.
- `tools/wric_pick_clade.cpp` — modes: window pick (existing), `preorder`
  (τ-carrier extraction), `restrict` (spanning-subtree restriction),
  `fragments` (maximal exclusion-free clades → complement parts).
- `tools/wric_partition.cpp` — greedy bottom-up edge cuts + FFD packing
  (committed now; used for the 23-part v0).
- `tools/wric_subtree_common.hpp` — `load_tree_any` (any-arity loader)
  moved here and shared.

## 7. Lessons / traps (all hit, all fixed)

- `splice`/`extract` are **clade-based**: a part whose ids are not a clade of
  the host is a **silent no-op** (graft "succeeds", tree unchanged). The
  fragment batch adds a graft-liveness check (score must move). Scattered
  groups cannot be grafted as parts; parts must be host clades.
- `fitch_assign_compact_genomes` requires a tree — on DAGs it deadlocks
  (0% CPU). DAG labeling here is CG-copy + `recompute_edge_mutations`.
- larch2 summary prints are pre-move; post-run quality needs `--dag-info`
  or a chart witness.
- ps `%CPU`/etime on this box are unreliable; pgrep -f self-matches the
  wrapper; kernel OOM-reclaim kills tenants — check `free` before big runs
  and watch RSS.
- Comb choice is a search-space commitment: fixed labels make resolution
  score-neutral, but the comb's extra internal labels give −132 "free"
  (polytomy single-label is a constraint, not a relaxation).

## 8. Artifacts and reproduction

`/tmp/v0/`: `scaffold_bin.pb.gz` (baseline 59,887), `parts_bin|parts_spr|
parts_wit{,3}/` + `parts.tsv`/`parts3.tsv`, `graft/` + `graft_scores.tsv`,
`chain/final.pb.gz` (59,821), `tension/spr1_bin.pb.gz` + `tau_full.txt` +
`TENSION.md`, `tau_parts/final/graft_0.pb.gz` (59,798), `hyb/final_v05.pb.gz`
(59,778) + `frag.tsv`. Input tree `~/matsen/larch/data/startmat/
startmat_no_ancestral.pb.gz` (+ refseq.txt.gz); monolithic SPR output
`/tmp/startmat_spr1.pb.gz`.

Per-part materialize (canonical): `larch2 --dag-pb <part_spr> -o <wit>
--trim-mode chart-bnb -n 0 --chart-bnb-score-only --chart-bnb-upper-bound
<incumbent> --chart-bnb-class-compress on --chart-bnb-profile-dedup on
--wric-lazy-chart on --chart-bnb-trim-application optimal-topology-materialize
--chart-bnb-max-exact-topologies 1`. Exact tree scoring:
`wric_tension_map tension t.pb.gz t.pb.gz 0` (prints score, then mismatch
throws).

## 9. Open

- v1 kernel: per-part boundary response rows + the 37-node coupled solve —
  certifies the exact fold optimum (bracket [59,751, 59,821] for the size
  fold) and unfreezes the scaffold.
- All-resolutions folds (vs single comb) — richer span, larger memory.
- Frozen sub-100-leaf twigs (2,747 leaves) and the exact-combination
  certification remain unbought.

## 10. SPR-only control (polytomic partition–SPR–sample–regraft)

Question: how much does the chart tier buy if the pipeline is SPR-only?
Setup: any-arity graft lift (`WRIC_GRAFT_ANY=1` in wric_subtree graft/extract;
`splice_any`), tension-spirit partition of the POLYTOMIC original (parts must
be host clades): crown-core 17,391 (largest crown-pure clade; the τ-carrier
23,547 set is not an original clade) + 5,598 + 1,365 + 1,150 + 1,072; 6,463
sub-1,000 leaves frozen. Per part: native SPR (1 iter, 50 moves, polytomic =
shallow ladders) -> `dagutil --sample --sample-method parsimony` (span-min
parse, the SPR-only materialize) -> any-arity graft chain, scored by
`--dag-info`; final tree comb-binarized once and rescored binary.

Results: chain 60,019 -> 59,899 own-convention (~1.5 h total; crown-core SPR
45 min) — 17 BELOW the monolithic 3 h run's span-minimum (59,916), half its
time. End-binarize +33-s rescore: 59,899 -> 59,846 binary (one-shot label
freedom recovers -53 of the raw tree's -132). Comparison table:

| pipeline | binary | wall |
|---|---|---|
| monolithic native 3h | floor 59,833 (first parse 59,874) | 3 h |
| SPR-only 5 parts | 59,846 | ~1.5 h |
| size 23 parts + chart | 59,821 | ~1 h |
| tension + chart + hybrid | 59,778 | ~3.3 h |

Conclusion: SPR-only partitioning beats monolithic SPR outright and is the
cheap baseline; the chart tier is worth 25–68 units under comparable
partitions (certified recombination + per-part binary-convention optimum),
at comparable or lower wall time. Artifacts: /tmp/spro/ (parts, spr/, best/,
graft/, final_spro.pb.gz = 59,846 binary tree).
