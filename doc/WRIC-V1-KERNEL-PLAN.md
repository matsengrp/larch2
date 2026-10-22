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

### M1 — scaffold-fixed coupled solve (first certificate)
- `wric_scaffold <scaffold.pb.gz> <resp1> … <respk>`: bespoke min-plus DP
  over the fixed scaffold comb. Part attachments consume their response
  rows; scaffold nodes free labels; UA/reference conventions matched to the
  scoring used everywhere else (tension-tool cross-checks: rescore the
  stitched witness, must equal the DP total exactly).
- Witness stitching: for each part, materialize its optimal parse GIVEN the
  scaffold-chosen root CG (conditioned materialize = down-pass with the
  root row clamped; small addition to M0's export tool — the row is already
  in hand). Splice-graft the witnesses into the scaffold (existing any-arity
  graft), rescore: exact equality with the DP optimum is the acceptance
  test.
- Deliverable: for the size fold, the exact optimum over all 2²³ part
  combinations × label freedom, replacing the greedy 59,821 (expect equal
  or a few units lower — the chain was near-additive, so the certificate is
  the point more than the score).
- Deliverable: for the SPR-only 5-part fold, the same certificate over its
  (richer) part grammars.

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
