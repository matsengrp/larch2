# WRIC combined plan: Architecture B (coupled candidate tuples) + free-row placement

Status: planned (2026-10-25).  Follows the Architecture A execution
(WRIC-CONDITIONED-MATERIALIZE.md §11).  Entry state: **U = 59,715**
(unrestricted realization, reproduced exactly at two profiles);
within-26-family bracket **[59,673, 59,715]**; tightest family bracket
[59,694, 59,724] (28-fresh).

## 1. What the 79-unit finding fixed about the landscape

Architecture A measured that every FIXED profile costs >= 59,793 while
the joint per-(g,r) choice realizes 59,715.  Consequences used below:

- any tier that pins profiles is dead (masks, column moves, swaps);
- U is a JOINT object: its evaluation and any improvement of it must be
  free-row (states co-optimized with scaffold edges) -- `scaffold wit`
  is exactly that instrument and is cheap (~1 min);
- the remaining 42 composite units [59,673, 59,715] live in the
  site-coupling of the parts' tree choices -- the axis Architecture B
  must attack with tree-consistent rows, not profile machinery.

Two axes remain: TOPOLOGY (where parts attach -- free-row SPR) and
TUPLE (which tree each part uses -- Architecture B).  Both evaluate
through the same free-row DP; both are realization-side plus
certificate-side in different mixes.

## 2. Shared substrate (B/F common instruments)

1. **Mixed-row `scaffold lb`**: per-part row source = m-rows (resp) OR a
   specific tree's rows (wit file).  Node configurations feed one DP:
   unassigned parts at m-rows (valid relaxation), assigned parts at
   their trees' exact rows.  Implementation: a config file (part ->
   m|wit-path); the DP code already handles both row kinds (resp_rows /
   witness_rows) -- this only mixes them per part.  Fixtures: all-m
   reproduces 59,673; all-wit reproduces 59,715 (assertion targets).
2. **Placement kit**: newick surgery on tc26 (prune pseudo-leaf /
   cherry, regraft on a chosen edge) -- the round-1/2 surgery code
   generalized; validation = wit DP of unchanged surgery == 59,715.
3. **Candidate export**: per part, K diverse materializations from its
   grammar (--diverse-sample machinery; pools = parsimony-optimal
   region) plus the incumbent witness; each candidate's rows via the
   existing witness_rows path.

## 3. Phase F -- free-row placement sweeps (run FIRST; 0.5 day)

Goal: drop U by relocating parts under the TRUE objective; secondarily
harden the claim that tc26's placements are fold-optimal.

EXECUTED 2026-10-25/26 (kit + logs: work/m2-startmat/plF/).  Kit:
tonewick -> python newick surgery (leaf regraft of __PART_j__ leaves,
iterative clone -- deepcopy overflows on ~4k-deep caterpillars) ->
fromnewick (NO trailing newline after ';' -- a phantom-leaf trap) ->
scaffold wit.  Fixture: identity round-trip reproduces 59,715 exactly.
Sweep: radius-6 edges + 12 deterministic far samples per target;
~35 s/eval; two hill-climb iterations.

Results:

- Iter 1 (base tc26, incumbent 59,715): 02/03/cherry(02,03)/11/12/
  cherry(11,12)/09 ALL plateau at 59,715 (totals 59,715-59,719; the
  landscape is flat).  The CROWN (slot 08) moves at distance 1:
  **U = 59,714** (tc27, assertion-checked OK).
- Iter 2 (base tc27): all 8 targets plateau at 59,714 (many exact
  ties) -- hill-climb converged; no further improving move.
- LB(tc27 family) = 59,672 (pin moved -1 with the topology); bracket
  **[59,672, 59,714]**, gap 42 -- the composite slack persists (as
  expected: placement moves realization, not relaxation).

Verdict: the placement axis held exactly one unit, and it was the
crown -- the part with the largest surrogate-vs-fold correction, as
§6 predicted.  tc27 (crown regrafted one edge) is now B's substrate;
the remaining unexplored morsel is a wider-radius crown pass.
Unrestricted LB across families now min(59,672, 59,673, 59,689,
59,694) = 59,672.

- F.1 **Cherry pass**: the two split cherries (02/03 and 11/12) were
  seeded at their parents' positions and only surrogate-searched (the
  megatree SPR scores pseudo-leaves as point taxa -- a surrogate whose
  mis-ranking budget is now measured at ~79 units).  Sweep: each half
  independently + the pair jointly, regraft over (a) all edges within
  radius r of the current spot, (b) the top surrogate-alternative spots
  from the M2.1 descent logs if recoverable.  Rank cheaply with
  mixed-row lb (moved part at wit rows, rest at m-rows -- a valid
  lower bound for that placement), verify the top few with full wit.
- F.2 **Big-inside pass**: slots 09 (inside 5,534) and 08 (crown,
  29,840) -- the parts whose surrogate-vs-fold correction is largest.
  Same protocol.  (No crown clamps are involved anywhere: wit
  evaluation needs only trees.)
- F.3 **Hill-climb** (only if F.1/F.2 hit): greedy SPR on tc26 under
  the wit objective until a 0-move plateau; each accepted move updates
  the incumbent scaffold (tc26 -> tc26').
- Certificates: any hit lowers U (unrestricted-valid, re-assert with
  `scaffold wit ... - <new U>`); a full miss tightens the
  placement-optimality claim to "free-row SPR plateau at fixed trees".
- Budget: 30-60 wit evaluations targeted; hard timebox 4 h.  If no hit,
  F still pays: tc26 confirmed as B's substrate.

## 4. Phase B -- Architecture B as candidate-tuple B&B (1-1.5 day)

EXECUTED 2026-10-26 (driver + pools + logs: work/m2-startmat/archB/).

- **B.0 mixed-row lb**: `WRIC_MIXED=<cfg>` config ("j m" | "j w <pb>")
  on `scaffold lb`.  Fixtures PASS: all-m = 59,672 (composite); all-wit
  = **59,714 = scaffold wit exactly** (full-assignment leaf = wit DP).
- **B.1 pools**: diverse-optimal materializations per grammar (K=16,
  `--diverse-sample --diverse-newick` -> fromnewick with the witness as
  cg source).  Rich (16): slots 01,02,06,07,09,15,18,20; medium: 04(8),
  25(10), 03/11/16/21/22(3-5); degenerate: 00,05,10,12,13,14,17,19,23,24;
  crown sampler killed at 4 h (pool = witness only).  Candidates can be
  BETTER free than the incumbent witness (slot 01: 965 vs 966 -- the
  g2-era grammars were enriched past the witnesses).
- **B.2 B&B**: best-first over assignments; STOPPED at opened=40,
  heap=625, evals=665, min open bound 59,692 -- the front grows +16 per
  expansion (the tuple tier's relaxation wall); that 59,692 subtree's
  promise was REALIZED by the sweeps below.
- **One-away sweeps** (full-assignment leaves = exact wit values):
  iter1 (witness base): ALL 16 slot-09 candidates -> **59,706** (-8!);
  iter2 (base 9:1): 22 leaves 59,705; iter3 (+5:1): 20 leaves 59,704;
  iter4 (+6:1): 4 leaves 59,703 (slot 21); iter5 (+21:1): **ZERO below
  59,703** -> converged (156 ties, 24 at 59,704).

**Final: U = 59,703 assertion-checked OK** (tc27 scaffold, tuple =
witnesses with slots 05/06/09/21 -> diverse candidates; tree files
work/m2-startmat/archB/witB/).  Session total 59,715 -> 59,703
(-12: -1 Phase F placement, -11 Phase B tuples; 184 below comb,
143 below SPR-only).  Bracket **[59,672, 59,703]**, gap 31.

Certificate semantics (honest):

1. One-away optimality WITHIN the pools: no single-candidate swap
   improves 59,703; any further gain needs >= 2 simultaneous swaps
   within pools, or trees outside the pools (9 parts degenerate; crown
   pool absent).
2. **The resp m-rows are NOT pointwise envelopes of the grammar spans**
   (measured: assigning the four winners to wit rows LOWERS the mixed
   bound to 59,667 < composite 59,672).  Partial mixed bounds are
   diagnostics, not LBs; lower-bound claims stay on the all-m composite
   (59,672, machinery-validated).
3. Full-assignment leaves are exact realizations -- every reported U is
   a real tree tuple, assertion-checked.

The coupled-chart optimum (within-family) = min over TUPLES (one tree
per part) of the free-row DP value.  Exact enumeration is impossible;
the composite lb is the k=0 relaxation.  B = B&B over per-part
CANDIDATE SETS with exact tree rows:

- Node = partial assignment (some parts -> candidate trees); rows =
  mixed (assigned: exact tree rows; unassigned: m-rows).  Bound = mixed
  DP; incumbent = U (post-F).
- Branch on the part with the largest live gap (bound contribution of
  its m-rows vs its best candidate's rows); children = its candidates.
- Prune at bound >= incumbent.  Termination = open-front empty
  => **candidate-family optimum certified exactly**; report family
  coverage honestly (K per part; pool sizes vs grammar spans).
- Expected dynamics: the witnesses already form the tuple realizing U;
  the B&B's question is whether any OTHER tuple beats it -- exactly the
  multi-part, tree-consistent tier that profiles could not express.
- The crown: free diverse materializations only (its historical 6.4 h
  clamps are NOT needed -- rows come from materialized trees, and the
  crown's r=7 enters as its candidate row gap; if the crown's pool is
  degenerate (1-3 trees), say so and carry its m-rows with the 7-unit
  caveat).

Stages:
- B.0 mixed-row lb + fixtures (0.25 d);
- B.1 candidate export + pools per part (0.25 d; measure pool richness
  -- the megatree probe's 3-region degeneracy is the risk precedent);
- B.2 the B&B loop, best-first, memoized wit runs (~1 min/node; a few
  hundred nodes feasible overnight) (0.5 d);
- B.3 (gated) root-entry-class branching / frontier-row export toward
  full-family certification -- only if B.2's bound demonstrably moves
  and the open front stalls below U with meaningful candidate gaps.

Success criteria (any of):
- U drops (new realization from a pruned-in tuple);
- LB rises above 59,673 (certified candidate-family optimum > old LB);
- or the honest negative: candidate families too weak (pools
  degenerate) -> documents that the k-relaxation ladder needs a
  different candidate generator (e.g. SPR-neighborhood trees per part).

## 5. Sequencing and certificate semantics

F before B: F is cheaper, tightens the incumbent B must beat, and may
change the substrate (tc26').  B then certifies on the best scaffold.

Reporting: U always unrestricted-realized; family brackets always
within-family and now also within-candidate-family (state K and pool
sizes).  No profile-tier claims anywhere (retired by §1).

## 6. Costs and risks

- F: 0.5 d, no new math; risk = all-miss (likely for most parts; the
  cherries are the genuine unknown).
- B: 1-1.5 d; main risk = degenerate diverse pools (precedent: the
  megatree probe's 3 distinct regions); mitigation = SPR-neighborhood
  candidates per part grammar (cheap, richer), and B.3 escalation.
- Shared risk: wit-DP cost creep on moved crowns (29,840-row parts) --
  keep the crown last in every loop.
