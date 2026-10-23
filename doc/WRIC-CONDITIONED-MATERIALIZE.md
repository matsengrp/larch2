# WRIC conditioned materialize plan (v1 step: close the fold bracket)

Goal: shrink the re-partitioned fold bracket **[59,726, 59,768]** (gap 42 =
41 part-internal coupling + 1 profile loss; scaffold coupling measured at
zero) toward a single certified number, by evaluating each part exactly
under the LB profile (`npart/fold_lb2.profile`) instead of under its own
free optimum.

## 1. The math

For part j with grammar G_j and a root-state profile `a_j(·)` (one state per
site, delivered per (pattern, ref-state) by the scaffold DP):

    PartOpt_j(a_j) = min over derivations D of G_j with root CG = a_j
                     (at every active site) of cost(D).

Key quantities:

- `Σ_s m_j(a_j(s), s)`  (response rows at the profile) LOWER-bounds
  PartOpt_j — per-site minima let each site pick its own derivation.
  Summed over parts + scaffold-at-profile = exactly the LB (the DP chose
  the profile as argmin).  This is the composite trap a third time: the
  clamp must live INSIDE the coupled chart, not in the evaluation.
- `U' = LB + Σ_j residual_j`,  `residual_j = PartOpt_j(a_j) − Σ_s m_j(a_j(s),s) ≥ 0`.
- Certificate after the run:  `[ LB , min(U, U') ]`, exact iff Σ residual = 0.

Note residual_j ≥ 0 even for gap-0 parts (the LB profile can disagree with
a part's own per-site argmin when outvoted by scaffold + siblings); the
measured 1-unit profile loss says disagreements are rare.

## 2. Phase 1 — biased materialize (no chart surgery)

Penalty clamp via a virtual leaf: append a leaf whose CG equals the profile
to the part root (added to every root production ⇒ every derivation carries
it).  The optimum then pays ≤ 1 per site to deviate from the profile —
materialized trees strongly favor profile-following derivations.

Work items:
1. **Profile→CG builder**: positions come from the part's leaf mutation
   union; per position, classify its pattern (restriction signature — same
   dict machinery as M0) + ref base (from the reference) → profile state.
2. **`wric_add_leaf`**: load part DAG, append virtual leaf under the root in
   all root productions (arity stays ≤ 63).
3. **Strip step**: drop the virtual leaf from the materialized witness
   (squash the resulting unary root) → graftable part tree.
4. Batch over 24 parts; rescore via the existing scaffold wit-mode DP.

Acceptance / instrumentation:
- Realized root profile vs LB profile: sites paying the 1-penalty to
  deviate are exactly Phase-3 branch candidates — log them.
- `U'' = DP(witness rows)`; bracket becomes `[LB, min(U, U'', U')]` where
  U' is implied by realized-conditioned scores.
- Kill criterion: if U'' > U (bias hurt), keep U and proceed to Phase 2.

Effort ~0.5 day; machine ~1.5 h (crown materialize dominates).

## 3. Phase 2 — true root clamp in the chart (if Phase 1 leaves residual)

Mask the root row to one state per class inside the score-only/materialize
path (`--chart-bnb-root-clamp <profile>`): the mask must apply wherever the
root row is consumed — bound computation AND materialization (otherwise the
returned tree can violate the clamp silently).

Validation first (regression-first, per campaign protocol):
1. gap-0 parts clamped at their composite-argmin profile: conditioned
   optimum == certified optimum == part_lb (triple equality).
2. Clamp at a certified witness's own realized root profile:
   conditioned optimum == that part's certified optimum exactly.
3. Monotonicity: PartOpt(any profile) ≥ Σ_s m_j(profile) and ≥ lb_j;
   ≤ opt_j + Hamming-ish sanity.
4. Crown last; cross-profile dominance stays OFF unless memory demands
   (then it must be re-validated on one fixture first).

Effort ~0.5–1 day (the narrow-but-production-touching part); machine same
as Phase 1.

## 4. Phase 3 — profile branch-and-bound (only if Σ residual > 0)

If U' − LB = r > 0 after Phase 2: branch over profiles with the rows-DP as
master.  Branching unit: one part's attachment state at one (pattern,
ref-state) — implement as a row mask on that pseudo-leaf entry in the
master DP (trivial: zero out the disallowed state's row entry and re-run;
37 s per node).  Bound = masked rows-DP (valid LB by construction);
incumbent = best conditioned evaluation from Phase 2.  Depth bounded by r
(integer residuals; total ≤ 42 at entry).  Emit the exact optimum + one
witness when bound meets incumbent.

Effort ~0.25 day (master masking only; loop + logging).

## 5. Sequence and success criteria

1 → 2 → 3, each phase's bracket strictly ≤ the previous.  Outcomes:
- **best case**: Σ residual = 0 ⇒ fold optimum certified exactly
  (expected value 59,726–59,750; every phase-1 log line tightens the guess).
- **floor case**: bracket stalls at [59,726, 59,7xx] with r localized to
  the crown (its gap is 7 of 41) — still the tightest certified range at
  this scale, and the residuals quantify exactly what richer crown
  exploration (M2 / deeper SPR) would buy.

Honesty notes: U' can exceed U (profile suboptimality) — the certificate
always reports min; residuals are integers so Phase 3 terminates; no
independence assumption is introduced anywhere (the clamp is inside the
coupled chart, the master keeps exact scaffold coupling).

## 6. Phase 1 executed (2026-10-22, commit follows)

Implementation: `wric_tension_map vleaf {add,strip,ub}` — profile parser,
profile->CG builder (positions from the global pattern tables, invariant
patterns pinned to their uniform state), augmented-grammar writer (leaf
__VLEAF__ under every UA-child production at a fresh clade index; edge
mutations via to_edge_mutations), unary-squashing strip, and an exact
biased-UB scorer (witness rows + UA transition + bias; validated 24/24:
bnb_optimum <= ub everywhere, equality often).

End-to-end smoke (slot 23): aug optimum 212 == ub == witness biased cost;
stripped witness rescores 211 == certified optimum.  Note the materializer
binarizes the augmented root production, so the virtual leaf can sit below
the root; strip squashes the leftover unary node.

Batch over 24 slots (crown 25 min with cross-profile):
- every biased_opt <= ub (UB formula exact);
- bias penalties Sigma(biased_opt - certified) = 52 (crown pays only 2,
  carrier 3, part_05 6; many small parts 0-4);
- grammar rows pinned at the LB profile sit Sigma 101 above per-part minima
  (worst: part_04 and part_05 12 each, carrier 10) -- the joint profile is
  genuinely suboptimal for several parts in isolation.

Certificate outcome (kill criterion of the plan):
- all-biased realization U'' = 59,769 -- one WORSE than U = 59,768;
- all 24 single swaps from the original base: 59,768 (swap 13 -> 59,769);
- bracket therefore unchanged: **[59,726, 59,768]**, U robust.

Reading: the LB profile is not realizable at the per-site minima for most
parts (bias penalties 1-6), and the parts whose profiles were "outvoted"
(part_05: pinned 16 above its own optimum) pay on the U side too.  Phase 2
(true root clamp) remains the lever: it yields exact per-part residuals
PartOpt_j - pinned_j (currently only bracketed by 0 <= residual and the
52-unit bias evidence), which is precisely the Phase-3 branch guide.
Artifacts: /tmp/v0/npart/bias/ (aug, biased_wit, wit2, bias.tsv, logs).

## 7. Phase 2 executed (2026-10-22/23)

Implementation (readout-only design, after two corrections):
- `root_state_clamp_map` (chart_options, null by default): per local
  pattern, PINNED SITE SUBGROUPS (ref, forced, count).  Subgroups are
  necessary: distinct global profile rows can restrict to the same local
  pattern with different pins (measured: rare; carrier has 2).
- Exact subgroup readouts in the trim objective: clamped invariant offset
  (pinned invariant patterns pay [a!=u]+[a!=r], uncovered sites stay free),
  weighted root scores (setup composite, selected-topology scoring), and
  `lower_bound_for_entry` at the ROOT clade only (below the root a pinned
  readout is not a valid bound -- leaf rows carry INF at unpresentable
  states; the free outside bound remains valid everywhere).
- `profile_dedup` is UNSOUND under the clamped readout (same-mask entries
  differ at pinned non-argmin states, not by the B gap): the CLI forces it
  off under clamp (this disables the lazy one-witness route); score-only
  dominance and cross-profile dominance stay available.  SPR/lazy search
  scorers reject a non-null clamp loudly.
- Validation oracle: Fitch/rebuilt validators are free-label and
  pattern-index specific; under clamp the emitted topologies' exact
  clamped re-score is the oracle (throw-on-mismatch).
- CLI `--chart-bnb-root-clamp <file> <col>` (part-local subgroup file;
  excess-count validation).  Generator: `wric_tension_map vleaf clampgen`
  (signature matching over sorted sample ids; reports conflicts, coverage,
  clamped invariant delta).  `vleaf profgen` emits witness-argmin profiles
  for fixtures.

Fixtures (all pass): gap-0 part at resp-argmin profile -> 963 = certified
= part_lb (triple equality); witness-argmin clamp -> 211 = certified;
joint profile slot 23 -> 212 = biased_opt = at_profile (theory-pinned).

Results (24/24, crown 6.5 h without dedup vs 25 min biased):
- Exact per-part residuals r_j = PartOpt_j - clamped composite_j:
  11 parts at 0 (profile realizable at per-site minima); top: part_04 10,
  crown 7, part_03/frag_01 4, part_07/frag_231 3.  **Sum = 44.**
- U'_exact (profile-pinned realization) = 59,726 + 44 = **59,770** --
  two ABOVE U: the LB profile is provably not the fold's optimal profile.
- Mixed realization (23 clamped + crown certified, free labels): 59,768
  == U.  All-clamped witness realization: 59,768.
- Certificate unchanged: **[59,726, 59,768]**, gap 42, now fully
  explained: 44 units of pinned-realization loss at the LB profile minus
  2 units recovered by free-label state mixing.

Reading: Phase 2 delivered the exact Phase-3 branch guide (part_04 and
crown hold 17 of the 44), and certified that no profile-following fold
realization beats U by this route -- closing the remaining 42 needs
branching AWAY from the LB profile (Phase 3: re-profile + re-clamp per
branch; crown branches cost ~6.5 h each without a clamp-sound dedup).
Artifacts: /tmp/v0/npart/clamp/ (clamp files, wit/, clamp.tsv, logs).

## 8. Clamp-sound dedup (2026-10-23): the correction's correction

The retraction of the Phase-2 unsoundness verdict was itself wrong, and
the Fitch-form assertion caught it live:

- Strong claim (FALSE): under unit-Hamming, every frontier row is a Fitch
  form row[s] = m + [s notin S].  Binary parse sums are M + k_t where k_t
  = the number of children whose mask misses t -- k in [1, arity], not
  {0, 1}.  Observed live: clade 185 row [3,1,3,2] (mask {1}, m=1, off-mask
  offsets 2/2/1); the route-(a) assertion fired on the first fixture run.
- Weak claim (TRUE): the unit-edge response is m + [t notin S] for ANY
  off-mask offsets >= 1 -- parents and every min-plus consumer see only
  (m, S).  This is what makes mask-only dedup sound for FREE readouts.
- Consequence: under the clamped root readout (which reads row[a] =
  m + k_a at pinned states, k_a entry-specific), same-mask entries can
  differ by more than the B gap -- mask-only dedup is genuinely unsound
  under clamp, as the Phase-2 verdict said.

Route (b) implemented (pin-augmented keys): when
chart_options.root_state_clamp is set, the frontier build augments the
ROOT clade's dedup key, per varying class, with the class row's values at
every state pinned by the clamp map (pinned_of_class from
class_of_pattern x subgroups).  Same-mask, same-pinned-reads entries
provably differ by exactly their B gap in every future score (responses
depend only on (m, S); pinned reads are pinned by the key), so min-B is
sound under the clamped readout.  Non-root clades are never read directly
by the clamp and keep mask-only keys.  Threaded as
multisite_frontier_build_options::root_state_clamp at all three build
sites (score pass, main build, witness trace); the apply-side dedup is no
longer disabled under clamp; the lazy one-witness route stays forced off
under clamp.  The route-(a) fitch_form_assert flag was removed (strict
Fitch form is violated by ordinary binary parses).

Validation: fixtures 3/3 (963/211/212); full 24-part batch with dedup on
reproduces the dedup-off PartOpt/composite table EXACTLY (24/24; crown
PartOpt 29,851 composite 29,844, validation success); Sum residual = 44,
U'_exact = 59,770 unchanged.  No cross-profile needed.

Performance verdict (honest): NO crown speedup -- 6.4 h with dedup vs
6.5 h without, and profile_dedup_pruned = 109 for the whole crown run:
under clamp, dedup barely fires for this grammar (the dominant
frontier-width bottleneck is elsewhere, likely per-entry row width over
~10.8k root-region classes plus dominance/UB interaction).  Phase 3
crown re-clamps therefore stay ~6.5 h each; profile branching that
avoids re-clamping the crown (or a width attack) is the remaining path
to cheap Phase-3 branches.  The 23 non-crown parts remain
seconds-to-minutes each with dedup on.

## 9. Phase 3 executed (2026-10-23): column-permutation correction, pinned
## scaffold DP, and U = 59,767

### 9.1 The column permutation (critical instrumentation fix)

`scaffold lb`'s profile export emitted its per-part state columns in
pseudo-node TRAVERSAL (slot) order while every consumer (clampgen,
build_profile_cg, the pin DP) reads column j as part j.  The slot order is
[10,0,12,2,11,9,6,4,1,13,5,14,15,3,16,8,17,18,19,20,7,21,22,23] -- only
parts 21/22/23 align, which is why the slot-23 fixture passed and nothing
crashed.  Consequences, all now corrected:

- Phase-1's vleaf bias campaign and Phase-2's 24-part clamp batch pinned
  each part at ANOTHER part's LB-profile pins (sigma-permuted).  Those
  numbers were real measurements at those pins, but not "the fold LB
  profile's restriction".  The dedup validation (24/24 exactness, fixtures
  3/3) is machinery-level and stands.
- Phase-2's U'_exact = 59,770 is void (permuted columns AND the scaffold
  side was never directly computed).  Corrected below.
- The M1 certificate [59,726, 59,768] was column-free and stands.
- The top-down export walk itself is CORRECT (descending order matches the
  post-order flatten: parent indices are larger; verified by
  WRIC_WALK_CHECK: pinned-at-walked-states + m rows == free minimum for
  every (g, r)).

The export now writes part-index columns; the corrected profile is
`fold_lb3.profile`.

### 9.2 The pinned-scaffold DP (new instrument)

`wric_tension_map scaffold pin <global> <scaffold> <ids_prefix> - 24
<profile>`: per (pattern, ref state), the collapsed-scaffold DP with every
part's attachment state FORCED to the profile pin; computes
scaffold_at_profile(pi) exactly.  U(pi) = scaffold_at_profile(pi) +
sum_j inside_j(pi), inside_j = PartOpt_j - UA_j (python from resp+profile).
Validation: pin(fold_lb3) + sum_j M_j(fold_lb3) = 8,348 + 51,378 = 59,726
= the free LB, EXACTLY (the corrected profile is a true per-(g,r) argmin
decomposition of the free DP).

### 9.3 Results at the corrected profile pi-hat = fold_lb3

Every part's witness clamped score equals its conditioned optimum except
four parts whose conditioned optima BEAT their witnesses (02: 1964 < 1965;
11: 336 < 337; 13: 267 < 268; 15: 182 < 183; measured by clamp).

- PartOpt_j(pi-hat) exact for 23 parts (batch, dedup on); crown PartOpt
  29,847 (== its certified witness; the witness-argmin column for the
  crown is IDENTICAL to pi-hat -- no crown lever exists).
- Residuals r_j = PartOpt_j - composite_j: 11 parts at 0; top: part_04
  10, part_03 4, frag_01 4; small-parts sum 35; crown r = 7 (composite
  29,840; CONFIRMED by the completed crown clamp: PartOpt 29,847,
  composite 29,840, validation success).
- **U(pi-hat) = 8,348 + 51,420 = 59,768 -- EXACTLY the old U**: the
  profile-pinned realization TIES the free-label realization.  The old
  "3-unit label-mixing recovery" narrative was a permutation artifact;
  the correct statement is that pi-hat pins are witness-consistent and
  realize the fold optimum.

### 9.4 The branch search and the new best tree

Single-part branches (witness-argmin columns merged into pi-hat,
evaluated exactly via clamp + pin DP; only 8 parts differ from pi-hat at
1-2 pins each): slot 23 -> **59,767**; slots 01/11/13/15 -> 59,768
(neutral); 16/17 -> 59,769; 02 -> 59,770.  Slot 23's witness-argmin
profile drops its inside by 2 (210 -> 208) for +1 scaffold: net -1.

**Realized: the mixed witness set (22 pi-hat clamped witnesses + part 23
at its witness-argmin clamp) scores 59,767 by the scaffold wit-DP -- the
new best tree, one below the campaign's 59,768.**

Pairs with 23: all additive (pin +2, inside stacks), best 59,767 -- no
stacking gain.  Single-pin deviations off pi-hat are barren: at every
(g,r) the walk state is a composite argmin (moving costs scaffold + m >=
0) and inside >= m per site, so no deviation of the remaining parts can
beat U(pi-hat) through single-pin moves; the witness-argmin exhaustion
above covers the remaining cheap class.  Crown: no-op.

### 9.5 Certificate

**[59,726, 59,767]**, gap 41 = 1 (composite deviation at part 23's
swapped pin) + 33 (small-part residuals at pi-hat minus part 23's) + 7
(crown residual, composite 29,840).  Artifacts: npart/fold/fold_lb3.profile
(corrected export), npart/clamp3/ (batch), npart/p3/ (swap profiles,
clamps, mixed realization), /tmp/p3_inside.json.
