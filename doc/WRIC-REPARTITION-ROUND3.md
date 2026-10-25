# WRIC re-partition round 3 — the grammar-richness tool

Status: planned (2026-10-25).  Follows WRIC-M2-SCAFFOLD-FREE.md §11-12
(rounds 1-2).  Entry state: **U = 59,715** (26-part realization),
strongest single-family bracket **[59,694, 59,724]** at the 28-family.

## 1. The wall, precisely

Round 2 proved the tau-steering works: the split children's residuals
decomposed 16 -> 4 exactly at the diagnosed clades.  It also proved the
blocker: children built by the current path (comb-scaffold extract, or
witness extract, + chained SPR) are STRUCTURALLY poorer than the parents'
fold-era merged grammars -- clamped PartOpts +18, free optima plateaued at
zero further SPR gains.  Splits convert residual structure into score
only if the children's spans contain what the parents' spans contained.

## 2. The tool: DAG restriction (`grammar restrict`)

Semantics.  Given a parent grammar DAG G over taxa set P and a subset
A ⊆ P, the restriction G|A is the sub-DAG of all nodes whose clade
taxa ⊆ A, with boundary nodes (parents have taxa ⊄ A) re-attached under
a fresh UA.  Two properties make it exact for our use:

- **Span inclusion**: for every tree T in span(G), the restriction T|A
  (A's subtree of T) is in span(G|A).  (T|A is composed of G-nodes whose
  clades are ⊆ A, all present in the sub-DAG.)
- **Row domination**: m_{G|A}[g][s] + m_{G|B}[g][s] <= m_G[g][s] per
  (pattern, state) for any split P = A ∪ B -- the children's response
  rows are at least as good as the parent's decomposition, so composite
  LBs, clamped PartOpts, and the fold certificate all weakly improve
  relative to the split parents.

Implementation: ~100 lines in the wsub/tension-map toolset (iterate
nodes, test clade taxa against the ids set, copy nodes/edges, rewire
boundary roots, build_clade_offsets + validate_dag).  No new math.

Validation battery (all must pass before any use):
1. Self-restriction: G|P == G (same node/edge counts after UA rebuild).
2. Restriction to a WITNESS clade: score of the witness's subtree ==
   free optimum of G|A (exact containment both ways for that clade).
3. Row domination on real splits: m_A + m_B <= m_parent for all (g,s)
   (checked from the exported responses).
4. Round-2 re-run: restricting round-1's part_02/part_03 grammars to the
   four round-2 children must give free optima <= the fresh grammars'
   (571/448/148/803) -- strictly better expected.

## 3. Stages

### R3.0 — build + validate the tool (0.5 d)

As §2.  Also add `grammar merge`-lite if trivially available (merge the
restricted child with its fresh-SPR version so enrichment can still run
on top); if merging is not cheap, skip -- restriction output is already
the enriched-parent superset, and further SPR can seed FROM it.

### R3.1 — redo round-2's splits with rich children (0.5 d)

Restrict round-1's g2 grammars (parts 02, 03) to the four children's ids;
responses -> lb (28) -> clamps -> trims -> certificate.  Expected:
- clamped PartOpts fall from 1,980 toward <= 1,962 (parents' level);
- U(28) from 59,724 toward <= 59,715, possibly below (the residual
  decomposition 16 -> 4 becomes realizable: headroom ~5-10 units);
- LB: the richer children LOWER the 28-family LB from 59,694 (rows get
  better) -- the bracket presentation must re-derive (see §5).

Decision gate: if U(28-rich) >= 59,715, the split axis is dead even with
rich grammars -- stop, document, and the certificate stays as is.

### R3.2 — round-3 splits proper (0.5-1 d, only if R3.1 pays)

Tau-diagnose the current top carriers with rich grammars in place, then:
- part_04's deep tau=2 @ 5 taxa + tau=1 @ 75 (r=3; deferred from round 1);
- the six r=2 slots (they were no-ops under fresh grammars; re-price
  under restriction);
- possibly a further bisection of whichever half still carries residual.
Each split: restrict -> responses -> lb -> clamps -> wit.  Enrichment
loop and swap sweep as standard post-passes (both cheap).

### R3.3 — crown (optional, explicitly gated)

The crown's 7 units remain quarantined (diffuse tau, 6.4 h clamps).  With
the restriction tool, a crown SPLIT could be re-priced WITHOUT running
crown clamps: restrict the crown grammar to the halves, compute the LB
movement and the free-optima decomposition first; only if the projected
gain clears ~5 units does a crown clamp become worth its 6.4 h.  Default:
skip.

## 4. Risks

- Restriction output can be large (the sub-DAG of a rich parent may
  carry many boundary roots); cap by node count and validate arity <= 63
  before any chart use (the usual polytomy prep).
- The cherry-restructure cost (~9 units at fixed witnesses, round 2) is
  TOPOLOGY-side and does not vanish with rich grammars; the PartOpt gains
  must clear it.  If they clear it only partially, keep the better of the
  two family realizations (the certificate takes the union min for U).
- Multi-family bookkeeping: every lb/pi/resp set must be tagged with its
  family; no cross-family bracket mixing (round 2's §12 rule).

## 5. Certificate convention (final)

- **U** = best realized tree over ALL families tried (unrestricted-valid).
- **Family bracket** = [free DP on that family's rows, best realization
  in that family]; quote the tightest family bracket alongside U.
- Any global-sounding claim must carry the within-family caveat (the
  standing convention since M1).

## 6. Costs and order

R3.0 (0.5 d) -> R3.1 (0.5 d, decision gate) -> R3.2 (0.5-1 d) -> R3.3
(gated).  Compute-light throughout; no crown clamps by default.
