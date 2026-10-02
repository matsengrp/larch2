# WRIC lazy chart + class compression + witness beam

Status: implemented and measured 2026-10-01/02 (uncommitted-to-docs draft;
binary work committed on branch `wric`).  This note documents the composite
trim path used for grammars whose coupled B&B walls on per-pattern frontiers:
lazy chart setup, class-compressed frontier rows, constant-class factorization,
the witness beam, and the empty-root certification loop.  It extends
`doc/WRIC-BNB-TRIM.md` (scores, dominance modes, exactness labels) and assumes
its conventions.

## The two walls

The coupled multi-site B&B keeps, per clade, a frontier of
equality-deduplicated per-pattern cost tables (one row per active pattern).
Two independent walls appear on wide grammars:

- **Row width.**  K active patterns x 4 states x 4 B per entry (HBV union:
  K=2,077, ~33 KB/entry at wide clades).  The HBV union primary OOM'd at
  108.8 GB inside clade 2263 (917 taxa) with only ~3.6M entries materialized.
- **Frontier cardinality.**  Under an optimal pruning bound the survivors are
  mutually incomparable and dominance removes ~0.4%; entry counts grow
  combinatorially toward the root (HBV 2260: 263,680; 2261: 738,560; 2262:
  692,480; 2263: ~18M inserted).  Quadratic dominance over ~18M entries is
  estimated >100 h and is intrinsic: the bound cannot tighten below the
  optimum.

Score-only dominance itself needed a rewrite first: a filter cascade
(saturating sum, min component, per-class minima vector) over sum-sorted
entries with parallel strided per-target checks (`apply_score_only_dominance_pruning`,
~230x on the mega-clades; same removal set by transitivity).

## Lazy chart setup (`--wric-lazy-chart on`)

`larch2` routes the trim through the lazy overload
`build_multisite_trim(grammar, patterns, lazy_chart, ...)` (`include/larch/lazy_chart.hpp`),
which prepares inside rows on demand and builds `active_pattern_info` —
including fully materialized per-pattern outside rows — from lazy charts
(`build_active_pattern_info_from_lazy`).  The overload calls the same grammar
trim implementation as the dense path, so all options below compose unchanged.
This matters on grammars with many clades (the enriched HBV grammar has ~4,900
clades vs the union's 2,295): dense per-pattern chart construction is the
setup bottleneck, lazy construction is not.

## Class-compressed frontier rows (`--chart-bnb-class-compress on`)

`build_multisite_frontiers_class_compressed` (`include/larch/chart_trim.hpp`)
stores one row per **row-key class** of the clade instead of one per active
pattern: a leaf class is a distinct observed state; an interior key is the
tuple of child classes over *all* productions of the clade.  Because the
multisite combine operator is pattern independent and patterns enter the DP
only through leaf rows, equal keys have identical columns in every frontier
entry; the grouping is exact.  Dedup, dominance, and bound pruning operate on
class rows with unchanged semantics (class dominance is pattern dominance).
Wide-clade compression is honestly small — classes track K x (taxa fraction),
so HBV clade 2260 has 1,809 classes against K=2,077 (1.15x) — but the builder
also releases consumed child frontiers and dedups against entries without
map-key copies, which took HBV's flat footprint from 74.7 GB to 37 GB.

## Constant-class factorization (always on with class compression)

A class whose 4-state row is identical across every surviving entry of a clade
is **dead for every decision above that clade**: no entry or completion choice
can change its contribution.  It is detected exactly two ways
(`multisite_clade_factorization`):

- *predictively*: every production of the clade maps the class to child
  classes factored at the (already processed) children, and the combined
  values agree across productions — then it is constant across all
  candidates, hence across all survivors;
- *empirically*: after dominance, any remaining class whose row is identical
  across all survivors is factored regardless of how constancy arose
  (`compact_constant_classes`).

Constancy is monotone upward (leaves are single-entry, hence fully factored).
Factored classes (a) need no per-entry row storage, (b) contribute a
per-clade constant `lb_offset` (invariant offset + per-pattern
factored contributions) added to every candidate bound, and (c) are dropped
from dominance scans **exactly**, with the full-row sort keys reconstructed
from the common factored-component sum and minimum
(`apply_score_only_dominance_pruning_compressed`), so sort order, dominance
candidate counts, removal sets, and all report scalars are bit-identical to
the uncompressed builder (`test/chart_class_compress_test.cpp`: fixtures +
40 random grammars x {off, score-only, two-pass}; ebov/seedtree/wnv end-to-end
reports identical and output DAGs byte-identical).

Measured effect: the ambiguity diagnostic predicts it — of 1,809 classes at
HBV clade 2260 only ~252 vary across the 263,681 survivors (86% of site
classes are already determined within the optimal band).  HBV whole-clade
times: 2260: 723 s -> 204 s; 2261: 3,200 -> 848 s; 2262: 1,856 -> 487 s;
clade 2263's insertion completes at ~18M entries / 65 GB where it previously
OOM'd at ~3.6M / 108 GB.  On well-behaved grammars the rates are higher
still: seedtree 99.4% factored (widest varying set 24 of 1,106 classes), wnv
140/3,338, dengue 387/3,510 — dengue's exact witness run: 13.3 s wall,
80 MB peak.

## Witness beam (`--chart-bnb-beam-after-taxa N --chart-bnb-beam-width K`)

Requires `--chart-bnb-class-compress on`.  Clades wider than N taxa keep at
most K entries with the smallest per-entry lower bounds (ties by insertion
order; truncation also runs during insertion whenever entries exceed 4K, with
the dedup index rebuilt, so peak memory stays bounded by ~4K rows).  The
primary score pass and the materialization witness build both honor the beam.

The beam exists for the cardinality wall: nothing exact can get through
HBV's root region (see above), so materialization switches to
**find-and-verify** semantics.  Soundness contract:

- the beam can only lose *completeness*, never *correctness*: every emitted
  topology is re-scored exactly (`validated_output_parsimony_min_exact`), and
- a beam that drops every optimal-completable entry fails loudly when the
  computed optimum is validated against a known exact optimum.

Provenance chains (`frontier_provenance_choice`) ride on the compressed
entries unchanged — they reference child entry indices, which row storage
never alters; frontiers are retained rather than released when provenance is
kept, and non-root entries are exported provenance-only for the topology
enumerator (`finish_multisite_topology_trace`), so retained-frontier memory
stays modest.

Measured: HBV union, cut 893 taxa, width 20K, UB 14,443, cap 1 — wall
3,378 s, peak 7.15 GB, one topology materialized and exactly validated at
14,416 (second run byte-identical, 3,478 s).  Clade 2263 under the beam:
9.79M candidates -> 17,206 entries in 158 s.

Known wall, stated plainly: **the beam caps storage, not pair enumeration.**
A beamed clade whose two children are themselves beamed enumerates W x W
candidate pairs serially; on the enriched HBV grammar width 100K stalled one
861-taxon clade for >29 min (10^10 pairs).  Pick the width for the product
(W^2 pairs ~ minutes at a few microseconds each) or extend to best-first /
budgeted pair enumeration at beamed combines.  Width 5K with cut 160 completed
the enriched grammar end-to-end: 8,985 s, 4.07 GB, materialized 14,279.

## Mask-profile dedup (`--chart-bnb-profile-dedup on`)

Requires `--chart-bnb-class-compress on`; runs post-dominance over the
compacted varying set of each clade, keeping per distinct per-class
argmin-mask profile only the entry with the smallest weighted minimum sum
B = sum_c w_c m_c (first-inserted on ties).  Soundness: parents consume
child rows only through unit-cost responses m_c + [t notin S_c], so
same-profile entries differ in every future score by exactly their B gap —
the optimum value and one optimal witness per profile survive; alternative
equal-B topologies may not (all-optima modes should keep it off).  Note the
ordering dependency: pre-dominance pools are profile-diverse (dominance
removes that clutter), so the pass must run after dominance — measuring
post-dominance (`WRIC_PROFILE_MASKS=1`) predicts the collapse exactly.

Measured on HBV (union grammar, clade 2260): 263,683 survivors -> 64
profiles at UB=14,443; 211,923 -> 62 at UB=14,416; the whole-grammar tally
at UB=14,443 was 2.42M entries -> 1,106 profiles.  With dedup on, the beam
is unnecessary on every dataset tested: the union witness materializes
14,416 in 210 s / 164 MB (byte-reproducible; beam reference 3,378 s /
7.15 GB; pre-factorization OOM at 108 GB), and the enriched-grammar
witness materializes 14,279 in 271 s / 1.68 GB (beamed reference 8,985 s /
4.07 GB).  Report fields: `profile_dedup_pruned`,
`profile_dedup_replaced` (0 on the post-dominance pass).

## The certification loop (empty-root probes)

A pass run with pruning bound X whose root frontier comes out **empty**
proves no derivation of the grammar scores <= X — *provided no clade was
truncated in that run*.  Per-entry bounds are sound lower bounds on every
completion, so an unbeamed empty root is a valid refutation.  The beam breaks
this: truncation discards entries by per-entry bound, but a discarded entry
may be the only partner with which a retained sibling entry completes within
the bound (misaligned optimal-state masks), so a beamed empty root is *not*
an infeasibility proof.  A two-clade, width-1 example: entries with unit
internal cost and optimal root masks {A} or {C} at each child, reference
state G; aligned pairs score 3, misaligned 4, every entry's best completion
is 3 — beams keeping {A} on the left and {C} on the right refute bound 3
although two score-3 trees exist.  (The tally is now part of the error
message: `empty root frontier (beam_truncated=..., beam_truncated_entries=...)`;
a recorded probe certificate must show `beam_truncated_entries=0`, or
otherwise discharge every discarded region — e.g. by open-block bookkeeping
at beamed combines.  Exact rescoring of a materialized tree certifies its
*score*, never optimality.)

A certificate therefore pairs (i) a materialized tree at score S (exact
rescoring, `validated_output_parsimony_min_exact`) with (ii) a zero-truncation
empty root at S-1.  Probe cost *shrinks* as the bound tightens (a tighter
bound kills candidates earlier).  Final state (2026-10-02, profile-dedup
builds, NO beam anywhere, so every tally is 0 by construction):
- union optimum = **14,416 certified**: witness 210 s / 164 MB + empty root
  at 14,415 in 110 s / 150 MB (tally 0);
- enriched-grammar optimum = **14,279 certified**: witness 271 s / 1.68 GB +
  empty root at 14,278 in 23 s / 1.68 GB (tally 0).
The earlier beamed certificates are superseded: the beamed 14,415 probe was
invalid (tally 434,707) and repaired the same day by the unbeamed rerun;
the beamed 14,278 probe was invalid (tally 358,563,031) and is superseded
by the dedup probe above.  Protocol: run probes unbeamed; with profile
dedup they are cheap enough that the beam has no role left on these
datasets.

Certified optima and the two objective caveats:

- **Fixed vs free labeling.**  `dagutil --edge-parsimony`'s `global_min` on an
  input DAG is the minimum over embedded trees with the DAG's *stored* node
  labels; the chart pipeline re-derives ancestral states (Sankoff), a joint
  minimization that can only match or beat it.  On ebov/seedtree/wnv/dengue
  the two coincide; on HBV they differ (14,443 fixed vs 14,416 free on the
  union).  A supplied `--chart-bnb-upper-bound` from dagutil is therefore
  always a *sound pruning bound*, but not necessarily tight.
- **Span matters.**  The optimum is only ever "best tree derivable from THIS
  DAG".  Native SPR moves create splits outside the input DAG's span; feeding
  the pipeline a richer DAG (e.g. native's merge output) changes the problem.
  HBV: union optimum 14,416 (certified); enriched (native 460 s plateau,
  ~4,900 clades) optimum 14,279 (certified, materialized via lazy + beam).
- The composite lower bound (`composite_lower_bound`) is the per-pattern
  relaxation; the coupling gap (coupled optimum minus composite bound) is 0
  on ebov but 2 (seedtree), 7 (dengue), 9 (wnv), 151 (HBV union), 329 (HBV
  enriched) — the largest gaps seen anywhere in the project.  The
  "certified" optima above are now sound on both grammars: union 14,416 and
  enriched 14,279, each via an unbeamed materialized witness plus an
  unbeamed empty-root refutation at S-1 (tally 0).

## CLI examples

Witness materialization, exact below the cut, beamed above (HBV union):

```bash
larch2 --dag-pb union.pb.gz -o out.pb.gz --trim-mode chart-bnb -n 0 \
  --chart-bnb-score-only \
  --chart-bnb-trim-application optimal-topology-materialize \
  --chart-bnb-max-exact-topologies 1 \
  --chart-bnb-upper-bound 14443 \
  --chart-bnb-class-compress on \
  --chart-bnb-beam-after-taxa 893 --chart-bnb-beam-width 20000
```

Enriched grammar (add `--wric-lazy-chart on`, cut 160, width 5000).  Note the
cap flag is REQUIRED for the witness path: without it the application falls
back to legacy exact-mask materialization, which will appear to hang on these
grammars.

Certification probe (refutation at X; expect `empty root frontier`):

```bash
larch2 --dag-pb union.pb.gz -o /tmp/probe.pb.gz --trim-mode chart-bnb -n 0 \
  --chart-bnb-score-only --chart-bnb-upper-bound 14415 \
  --chart-bnb-class-compress on \
  --chart-bnb-beam-after-taxa 893 --chart-bnb-beam-width 20000
```

## Report fields

With class compression on: `restriction_class_total/max`,
`factored_class_total`, `factored_predictive_total`,
`factored_empirical_total`, `varying_class_max`; `beam_truncated`,
`beam_truncated_entries` when any clade was truncated.  Under
`WRIC_PROFILE_FRONTIER=1` the compressed builder prints per-clade
`prof_clade=` lines including `classes=`, `varying=`, `factored=`.

## Artifacts and known noise

Session artifacts: `~/Downloads/10-datasets/larch-usher-tex-examples/zenodo/work/witness/`
(`hbv_beam.*`, `hbv_lazy5.*`, `hbv_native_full.*`, `dengue_cc.*`);
design log `~/tmp/class-scaffold.md`.  Known cosmetic nondeterminism: at HBV
mega-clade scale frontier entry counts wander by +-1 run-to-run (first
divergence at clade 2260 with identical candidate counts; wnv byte-stable);
objectives, validations, and output DAGs are unaffected (beam-run outputs
were byte-identical across runs).
