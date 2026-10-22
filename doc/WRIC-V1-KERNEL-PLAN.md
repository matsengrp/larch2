# WRIC v1 kernel plan: response rows + coupled scaffold solve

Goal: certify the exact optimum of a partition-fold — all part choices × all
part-internal parses × label freedom × scaffold rearrangement — with memory
that scales with the PARTS, not the whole grammar. This closes the bracket
the greedy chain leaves open (size fold: [59,751, 59,821]) and unfreezes the
scaffold, which every v0/SPR-only variant holds fixed.

This is the realization of the fold-theory construction (paper1's exactness
conditions: connected parts carrying per-site boundary rows + a coupled
scaffold solve; see also `~/tmp/class-scaffold.md`). The response math is the
same (m,S)-row machinery already verified in the context-aware graft scorer.

## 1. The object

Given a partition into parts P1..Pk (connected subtrees, host clades) with
part grammars G_j (their SPR exploration DAGs) and the scaffold S (the
minimal connecting structure + remainder leaves; on startmat's size fold,
37 non-part nodes; on the 5-part SPR-only fold, a handful):

- For each part j, active site s, and state a ∈ {A,C,G,T}:

      m_j(a, s) = min cost inside G_j at site s over parses whose root CG
                  has state a at s.

  This is exactly the root row of the part's class-compressed chart
  downpass — one 4-vector per restriction class, not per site.

- The global optimum is then an exact small coupled problem: optimize the
  scaffold (labels always; topology in M2) with part j replaced by a
  response pseudo-leaf whose per-site state-a cost is m_j(a, s), plus the
  attachment-edge mutation [scaffold state ≠ a] per site. Compute:
  ~15k classes × (37 + k) nodes × 16 transitions ≈ 10⁷ min-plus ops —
  seconds, single-threaded, megabytes.

- Soundness: every global tree decomposes uniquely as (scaffold, part
  parses, root labels); responses capture parts exactly (min over the full
  part span, conditioned per state); the scaffold DP minimizes the rest.
  No independence assumption across sites is made at the coupling — that is
  precisely what the composite LB gets wrong by 41 and this gets right.

## 2. Work items

### M0 — response export (new read-only path)  [DONE 2026-10-22]
- Implemented as `wric_tension_map response <global.pb.gz> <outdir>
  <part1.pb.gz> …` (batch: one global prep, N part preps).  Reuses the
  tension machinery verbatim: `prepare()` + `class_chart_minima()` (the
  unpruned per-class inside DP) — the root row m_j(·) per restriction
  class IS the response.  Output `.resp`: header (part taxa count,
  part composite LB, global invariant offset) + one row per GLOBAL active
  pattern: `gidx weight refA refC refG refT mA mC mG mT`.
- Findings that shaped the implementation:
  - The GLOBAL grammar must satisfy the arity-≤63 prep bound → the
    polytomic original cannot serve; the binary comb scaffold
    (scaffold_bin.pb.gz, same leaves/reference) is the global.
  - Part-UNIFORM restrictions never appear in part active classes
    (uniform sites live in the part's invariant offset) → handled
    explicitly: m(a) = [a ≠ u].  (First version had an index-space bug
    mixing part-taxonspace ids with global-pattern vectors; one leaf,
    G-vs-T, flagged it.)
  - `Σ_patterns weight·min_a(…)` is the part's composite LB, NOT its
    optimum: per-pattern-independent root states.  Σ part_lb = 59,662 vs
    Σ certified optima 59,751 → Σ internal coupling gaps = 89 across the
    23 size parts (per-part 0–11; four parts have zero gap).
- Acceptance (all 23 parts): (a) certified witness rescore under the same
  machinery reproduces the certified optimum exactly (23/23 OK);
  (b) exported per-part LB == the tension mode's independent composite LB
  (23/23); (c) production `bnb_optimum` spot-check (906/988) confirms the
  LB–optimum gap is real coupling, not an export bug.
- Artifacts: /tmp/v0/resp/part_{00..22}.spr.pb.resp (14,962 rows each),
  plus `wric_leaf_cg` (pb leaf-CG dump tool, used for the data-consistency
  probe that ruled out CG divergence between pbs).

### M1 — scaffold-fixed coupled solve (first certificate)  [DONE 2026-10-22]
- Implemented as `wric_tension_map scaffold <lb|wit> <global> <scaffold_bin>
  <ids_prefix> <rows_prefix> <np> <out.profile> [expected]`:
  collapses each part clade of the binary comb to a response pseudo-leaf
  (any-arity Sankoff over the remaining fixed scaffold, OpenMP over
  patterns; ~37 s).  `wit` scores the realized fold (witness-tree inside
  rows); `lb` scores the composite lower bound (grammar response rows) and
  dumps per-pattern attachment profiles (`fold_lb.profile`).
- **Scope correction (important):** response rows are per-site-class minima
  over part topologies, so the rows-DP is the fold's composite LOWER BOUND
  with exact scaffold coupling — not the exact optimum by itself (the
  89-unit part coupling gap measured at M0 is exactly this relaxation).
  M1 therefore delivers the bracket [LB, U], with exactness iff a single
  per-part topology realizes the LB profile (conditioned materialize =
  next step).
- **Fold discovery:** only **12 of the 23 size parts are clades of the
  comb** (00,01,03,04,05,07,08,09,10,11,12,16); the other 11 were preorder
  windows — their v0 splices were silent no-ops (this is why the v0 chain
  was a 12-switch chain; the 11 parts' isolated optima never entered any
  achieved tree).  M1's fold = scaffold comb + 12 response parts + 11
  frozen original regions.
- **Validation (all exact):** np=0 → 59,887 (comb baseline); np=1 (part
  00) → 59,885 (graft_00's measured −2 reproduced); np=23 wit → **59,820**,
  one BELOW the chain's measured 59,821 — the DP finds a strictly better
  label assignment for the same topology (write_tree's Fitch tie-breaking
  left 1 unit on the table).  Debug archaeology: the +598 initial offset
  was double-weighting — eval_row is already site-count-aware.
- **Result — scaffold-fixed size-fold certificate:**

      [ 59,781 , 59,820 ]   (LB via grammar rows, U via witnesses)

  with the gap fully accounted: Σ live-12 certified optima (19,812) −
  Σ live-12 part_lb (19,773) = **39 = exactly the bracket gap** — the
  scaffold coupling itself costs ZERO beyond per-site independence, and
  all 39 remaining units live inside the 12 parts (topology sharing).
  Closing them needs per-part conditioned materialize at the LB profiles
  (profiles exported); exactness would follow if each part's conditioned
  optimum meets its per-site minima.
- Artifacts: /tmp/v0/fold_lb.profile (14,963 lines), fold totals above.

### M2 — scaffold-free solve (the actual unfreezing)
- Scaffold grammar: megatree over k part-attachments + remainder leaves
  (≤ 60 taxa on startmat). Explore with native SPR / full rSPR enumeration
  at that scale (minutes), or all binary topologies for k ≤ 10 (catalytic
  for validation). Response pseudo-leaves ride along unchanged.
- The coupled DP runs per scaffold production — still tiny; alternatives
  multiply the scaffold side only.
- Deliverable: the first startmat number that is optimal over part spans ×
  scaffold arrangement. Compare against 59,778; the delta prices everything
  the frozen-scaffold variants left on the table.

### M3 — validation campaign (regression-first, per campaign protocol)
- Fixtures where the monolithic chart FITS: (a) seedtree union fold (K=1.1k,
  monolithic seconds): coupled result must equal monolithic optimum exactly;
  (b) startmat 2- and 3-part sub-folds (≤ 5k nodes): same equality; (c)
  degenerate k=1: equals the part's own chart optimum.
- Only then run the startmat folds (size 23-part, tension 5-part), plus HBV
  r5 (monolithic fits → independent end-to-end check at campaign scale).
- `WRIC_VERIFY_EVALS`-style row checks on the DP; rescored-witness equality
  as the outer gate.

## 3. Validation and acceptance

1. Equality with monolithic chart optimum on every fitting fixture (exact
   integer equality, not ±1).
2. Rescored stitched witness == DP optimum (catches convention slips: UA
   edge, invariant offset, class-vs-site mixups).
3. DP optimum ≥ composite LB (59,833 for the SPR grammar) and ≤ best greedy
   tree (59,821 / 59,778) — sanity brackets.
4. Determinism: same inputs → same witness (tie-breaking rule fixed).

## 4. What v1 does NOT cover (honest scope)

- Part boundaries stay inputs. The certificate is over the given partition;
  re-partitioning (tension-guided or otherwise) then re-certifying is the
  outer loop, unchanged.
- Cross-part topology moves beyond scaffold rearrangement (grafting a
  subtree from one part's interior into another's) are outside the space —
  inherent to any partition method; that is v2/paper3 fold territory.
- Part spans are whatever their grammars contain (more SPR → richer
  responses, no code change).

## 5. Costs

- M0: ~half day (mostly plumbing + format). M1: ~1 day including stitching
  and fixtures. M2: ~half–1 day. M3: half a day of runs.
- Compute: responses = one downpass per part (seconds–minutes each);
  coupled DP = seconds; scaffold exploration = minutes. The whole startmat
  v1 certificate ≈ well under an hour of machine time — vs the 100+ GB the
  monolithic fold chart demanded and still OOM'd.

## 6. Order of execution

M0 → M1 fixtures (seedtree, 2-part sub-folds) → M1 startmat certificate
(size fold first: directly comparable to the greedy 59,821) → M2 scaffold
exploration → M2 certificate → doc + commit. Any equality failure in the
fixture chain halts escalation to startmat, per regression-first.
