# WRIC plateau breaking (directed, context-aware injection)

This note documents the plateau-breaking campaign run on the HBV dataset
after the profile-dedup era: the signals that direct the search, the
context-aware injector that breaks SPR plateaus, the certified score chain
it produced, the protocol that packages it, and two instructive negative
results (an isolated-objective injector and a hull/"wavelet" compression
tier).  It is meant to make the claims mechanical rather than
interpretive, in the same spirit as `WRIC-BNB-TRIM.md`.

## The plateau, certified

Native SPR (larch2, multi-run, patience) halts on HBV at parsimony 14,279.
The important fact is that this halt is *optimum-in-span*, not a time
limit: the exact, unbeamed chart B&B over the ENTIRE explored span (the
whole DAG of every accepted move, with free min-plus recombination across
all of it) returns exactly 14,279.  No further same-kind search, and no
recombination of the search's own history, can beat the incumbent.
Breaking the plateau therefore requires topologies outside the span, and
a way to find them that SPR-shaped objectives cannot provide.

## Signal 1: the tension map (`tools/wric_tension_map.cpp`)

`tension` mode computes the exact telescoping decomposition of the
coupling gap (witness total minus composite LB) into per-clade marginals
via a chain of marginal clampings (each term >= 0, sum = gap, asserted).
`closure` mode attributes the improvement between two grammars/witnesses
to clades, with transitive emit gating so local attribution sums exactly.

Reading discipline (established by experiment):

- Tension is a **ranking hypothesis** about where coupling binds, not a
  recoverable score budget.  The decomposition accounts for the CURRENT
  witness against the CURRENT relaxation; neither survives a grammar
  change.  Improvements can exceed (or miss) the assigned tau.
- Say **score improvement**, not "gap closure": the 137-point round-1 gain
  coincided with the gap GROWING 151 -> 329.
- Attribution is order-dependent at the margin: `WRIC_REVERSE_SIBLINGS=1`
  reruns the chain with flipped sibling traversal.  On HBV the top
  targets were stable (84/84, 33/33, 14/14); mid-table marginals shifted.
- `WRIC_CLADE_STATS_OUT` adds per-clade alternatives (clades/productions
  below, `alt_prods`, `tree_like`): a saturation diagnostic about the
  EXISTING language only - a new topology could still help under a
  one-profile clade by adding a new mask profile or lowering B.  Keep
  such clades as controls (round 3 did; they were null at matched
  budget).
- `WRIC_TARGETS_OUT` / `WRIC_TARGETS_TOPK` / `WRIC_TARGETS_NODES` /
  `WRIC_REFSEQ_OUT` emit target lists for the injector.

Typical cost: ~0.6 s per map on the 5k-clade HBV grammars.

## Signal 2: the context-aware injector (`tools/wric_context_spr.cpp`)

For a target clade with taxon set T in witness W, the injector scores a
candidate subtree exactly, in the attachment context, via the mask-form
outside message

    O_p(s) = m_out,p + [s not in S_out,p]

    Q(T') = const_q + sum_{p variable in T} (m_T',p + [S_T',p & S_out,p = 0])

which is exactly the Fitch score of W with T' grafted at the attachment
edge (Sankoff cut at that edge).  S_out retains ALL minimizing outside
characters (the mask is the full argmin set), taken from ONE actual
witness topology.  The isolated objective is B_T = sum m_T',p (the
subtree's own parsimony).  Only variable-within-T sites are scored; the
outside offset is constant during search.  The evaluator is verified
against independent chart scores before any search (witness totals,
decompositions, and known graft scores must reproduce exactly).

Search: multi-start steepest-descent rooted SPR (+NNI), canonical-Newick
dedup.  Two engines: `engine=full` (rebuild+rescore per candidate; fine
to ~50 taxa) and `engine=inc` (per-slot Fitch rows, ancestor-path
recompute, undo = structural revert + recompute; scratch slot
permanentized on accept, bypassed-parent slots recycled).  The
incremental engine must reproduce the full engine's results on a small
target before trusting it on big ones; `diag` mode and
`WRIC_VERIFY_EVALS` exist because row-consistency checks cannot catch
transiently wrong candidate scores (the eval_spr stale-bypass-path bug
was found exactly this way).

Why SPR-shaped objectives fail here (measured, stable across rounds):
isolated-better subtrees score +3 to **+28 worse** in context.  The
binding objective is the context-coupled score; round 2's
isolated-objective injector closed exactly 0 of 36 grafts.

## The campaign record (HBV, all steps certified)

Certified chain (each optimum materialized exactly, probe at opt-1 empty,
unbeamed, `beam_truncated=false`):

    fixed-label 14,443 | union free 14,416 | native plateau 14,279
    -> r3 14,278 -> r4 14,273 -> r5 14,270        (r6: zero, stop)

| round | family (tau) | search | recombination | span growth |
|---|---|---|---|---|
| 3 | 32 taxa (33) | -1 | - | +3 clades |
| 4 | 629 taxa (84) | -3 (three distinct subtrees) | **-2** | +108 clades |
| 5 | 629 taxa (87) | -1 | **-2** | +93 clades |
| 6 | all (87/33/14/14) | 0 | - | - |

- The **recombination bonus**: twice the merged span's exact optimum fell
  below every single tree any search found (14,275 -> 14,273,
  14,272 -> 14,270).  Those units have no single-tree SPR analogue at any
  budget; they come from min-plus recombination across independently
  injected subtrees.
- **Stop signal**: round 6 yielded zero at matched budgets, strengthened
  by a full-neighborhood check (`diag`): all 1,550,690 SPR moves plus all
  NNI moves of the final witness's 629-taxon subtree - best is the
  witness itself.  The result is certified-local, not global (LB 13,930,
  gap 340).
- **SPR restart after injection is a no-op** (tested): two seeds x 40
  iterations on the expanded grammar found 230 consecutive
  "0 profitable moves" blocks each, span essentially unchanged,
  certificate intact.  Caveat: larch2's per-iteration "Sampling tree
  (score ...)" print is a soft-polytomy relaxed score (samplers can emit
  polytomic structures from merged DAGs); it is not comparable to the
  binary-span optima above.

## The protocol

1. SPR to plateau; **certify the plateau** with an exact probe at
   plateau-1 (~25 s on HBV).  Empty root => optimum-in-span; more
   same-kind SPR is provably pointless.
2. Tension map (~1 s); target the top-tau families.
3. Context-aware injection per target (`search` mode; witness start plus
   perturb restarts; `engine=inc` with tight caps for large families:
   starts=4-5, spr_rounds=12, spr_samples=80 was the calibrated point,
   ~13 s for 32 taxa, ~40 min for 629).
4. Merge ALL improving grafts into one grammar (`dagutil`), then B&B
   materialize (`--trim-mode chart-bnb -n 0 --chart-bnb-score-only
   --chart-bnb-upper-bound <best> --chart-bnb-class-compress on
   --chart-bnb-profile-dedup on --wric-lazy-chart on
   --chart-bnb-trim-application optimal-topology-materialize
   --chart-bnb-max-exact-topologies 1`) - this is where the
   recombination bonus lives, and it subsumes "repeat with SPR".
   Timing note (post-`f3daf31`, 2026-10-13): this materialize step is now
   ~49 s (was ~4.5 min during the campaign, via the explicit-bound
   one-witness frontend fix), and **~29 s with
   `--chart-bnb-cross-profile-dominance on`** (-41%; RSS flat; exact
   minimum + one legal witness only - guarded to this application, so the
   probe path cannot use it).  Probes remain ~24 s.
5. Probe at opt-1 to certify; remap; repeat 2-4 while productive
   (~1.5 h/round on HBV).  Stop on a zero round, optionally strengthened
   by the full-neighborhood diag.

Delivered on HBV: 14,279 -> 14,270 in three productive rounds, every
intermediate certified, ~5 h total (campaign-era timing; with the
cross-profile and one-witness-frontend speedups the post-plateau loop
steps are now roughly 2x faster).

## Negative result: hull/"wavelet" compression tiers (`tools/wric_hull_lb.cpp`)

The admissible min-plus analogue of a wavelet coefficient is a group row =
pointwise MIN over member rows (union leaf masks, one chart DP).
Measured slack (exact LB 13,930 on the r5 grammar; singleton level
validates exactly):

    groups     128    256    512    1024
    locality  13757  13616  13289  11763
    random    13868  13526  12654  10039

16x class reduction costs ~13,800 of 13,930; single-digit slack needs
~per-pattern rows.  The reason is structural: min-plus has no Parseval -
the coarse coefficient is a pointwise infimum set by the single best
member per state, while the group pays with everyone's summed weight, and
the composite LB IS the per-pattern value sum.  Site/"alignment wavelet"
grouping reduces to the same construction and dies with it.  The
compressible redundancy lives in the frontier dimension (profile dedup,
dominance - already lossless), not the pattern dimension.  Both a
coarse-LB tier and a slack-gated coarse injector evaluator are dead;
exact/heuristic mixing remains: sampler proposes, incumbent tightens B&B
pruning, exact tier certifies on-path.

## Build

Standalone tool builds (not CMake-registered):

    g++-trunk -std=c++26 -freflection -O2 -I include -I build/generated \
      tools/<tool>.cpp build/liblarch.a -lz -lpthread \
      -static-libstdc++ -static-libgcc -o build/<tool>

## Limitations

- "Certified" here always means: exact unbeamed B&B optimum over the
  stated span, plus probe-empty at opt-1.  No global optimality claim
  beyond that (the composite LB stays loose on rich grammars).
- The loop is validated end-to-end on HBV; the other sweep datasets broke
  with the cheaper levers (trajectory merging + free-label B&B) and never
  needed injection.
- Tooling is research-grade: standalone tools, env knobs, no packaging.
- Negative search outcomes are bounded by the tested move family and
  budget (200-restart/100-sweep rSPR steepest descent for smalls; tight
  caps + full-neighborhood diag for the big family), not exhaustion of
  the ~1.8e42-tree space.
