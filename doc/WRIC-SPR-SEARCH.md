# WRIC DAG-native chart-SPR search

This note documents the production chart-SPR search path exposed through
`include/larch/chart_spr_search.hpp` and `dagutil --chart-spr-search`.  It is a
DAG-native SPR optimizer over the collapsed clade grammar, not the legacy
per-candidate helper path.

## Architecture

```text
input DAG
  -> polytomy refinement / binary chart-compatible clade grammar
  -> active site-pattern builder
       invariant sites -> one topology-independent offset
       variable patterns -> chart cache
  -> streaming candidate generator
       bounded lazy upward paths, taxon/sample signatures, path counters
  -> local overlay-delta scorer
       recompute affected rows only, reuse unchanged cached rows
  -> acceptance gate
       lower-bound ranking, then exact/fixed-topology verification as configured
  -> attempted commit
       conservative materialize + one sidecar rebuild + rebuilt-objective gate
       (or optional local accepted-state cache updates)
```

The intended cost model is:

```text
initial full cache build: once per search state
local candidate score: affected clades only
locally rejected candidate: no full rebuild
exact verification: counted overlay materialization/B&B, not an accepted-state rebuild
attempted commit: one conservative rebuild unless local-update mode is enabled
candidate generation: bounded/lazy path expansion, no all-path precompute before caps
```

## Public APIs and helper boundaries

Production search code should use:

- `chart_spr_search_state` and `build_chart_spr_search_state*()` for the
  persistent grammar/pattern/chart cache;
- `for_each_grammar_spr_candidate()` for streaming candidate generation;
- `score_candidate_locally()` / `score_candidates_locally()` for local
  lower-bound scoring;
- `verify_candidate_exact_against_state()` or fixed-topology verification APIs
  for acceptance gates;
- `run_chart_spr_search()` for the accept/reject loop.

The legacy multisite helpers in `chart_spr.hpp` are diagnostic/oracle APIs:

- `score_multisite_spr_candidate_lower_bound()` materializes an overlay grammar
  and runs full composite chart scores on both the base and overlay grammars.
  Its result is a composite lower bound.
- `score_multisite_spr_candidate_exact()` materializes an overlay and rebuilds
  both old and new exact trims.  Production top-K verification instead reuses
  the search state's cached old exact score and computes only candidate-new
  exact scores.

CI lint keeps non-test production code from calling those helpers directly.

## Exactness vs candidate selection

These are separate dimensions and are reported separately by `dagutil`.

Acceptance/objective modes:

- `exact_multisite` (default): grammar-exact multi-site B&B over the modified
  grammar/topology set.  Non-top-K candidates may be unverified, but an
  accepted move has passed exact verification.
- `fixed_topology_exact`: exact score for one complete before/after topology
  certificate or deterministic selector.  This is exact for the selected
  topology, not for every topology in the modified grammar.
- `lower_bound_heuristic`: opt-in mode that accepts a composite lower-bound
  improvement.  It is useful for exploration and benchmarking but is not an
  exact coupled multi-site parsimony proof.

Candidate-selection modes:

- `exhaustive_exact`: exact-verify every locally scored candidate.
- `lower_bound_top_k`: exact-verify only the best local lower-bound candidates.
- `lower_bound_first_improvement`: stop after a locally ranked improving exact
  verification according to the configured policy.
- `sampled_or_randomized`: record the sampling/randomization policy and seed.

## Cache and scoring conventions

Production search caches active/topology-informative patterns only.  Invariant
sites are skipped from the hot cache and kept as
`chart_spr_search_state::invariant_constant_offset` plus
`skipped_invariant_site_count`.

Rules:

1. Internal cache, local-scorer, and exact-trim values are active-only unless a
   field explicitly says `full_with_invariants`.
2. Add `invariant_constant_offset` exactly once at reporting, comparison, and
   commit-gate boundaries.
3. `active_site_pattern_set::assert_no_skipped_invariant_metadata()` enforces
   that active-only helpers never receive skipped-invariant metadata or
   invariant patterns.
4. With `score_ua_edge=true`, a compressed site pattern may include positions
   with different reference states.  Root rows must be scored through
   `chart_spr_weighted_root_score_from_row()`, which applies each
   per-reference-state count to the full root row.  Do not reimplement this
   arithmetic in local scorers.

### Fixed-topology local-commit verifier

The local-commit `fixed_topology_exact` verifier is exact for the selected
before/after topology.  It computes the selected before/after root rows from
the persistent inside+outside caches, restricted to the affected rows, with a
production per-pattern gate:

- selected-subtree rows are stored keyed by structural rooted topology and
  active pattern (`fixed_topology_selected_cache_hits` / `_misses` /
  `_rows_computed`);
- the persistent inside cache participates in the delta: every base-clade
  selected row is cross-checked against the grammar-min inside row in `icache`.
  When they agree the selected production is the optimal one at that clade, so
  the persistent-cache row is a valid source for the selected-topology delta
  (an UNAFFECTED row, counted under `fixed_topology_icache_rows_reused`); when
  they disagree, or the clade is a temp ref introduced by the candidate, the
  row is AFFECTED and is recomputed from the selected-production recurrence
  (`fixed_topology_icache_rows_recomputed_affected`).  The selected row's value
  is authoritative either way (the grammar-min cache could be lower), so the
  cross-check is what makes the delta "from persistent inside+outside cache,
  restricted to affected rows" observable rather than asserted.  The outside
  cache supplies the root outside row, keeping the score in the same inside +
  outside scoring convention.

The `fixed_topology_exact` label is granted only when the persistent-cache
score agrees, per pattern, with the independent direct overlay selected-
topology scorer (the Phase-8 production per-pattern gate,
`fixed_topology_persistent_cache_direct_oracle_mismatches`; this scorer does
NOT materialize an overlay grammar, so it does not bump
`full_overlay_materializations`).  On a per-pattern mismatch the direct
oracle's value is the from-scratch authority for the selected topology.

The materialized per-pattern selected-topology oracle (diagnostic/test-only,
enabled by `verify_fixed_topology_materialized_oracle_for_tests`) materializes
the candidate's extended grammar and is the strongest independent check.  When
it is enabled and finds a mismatch, its own scores are used as the authority
(rather than re-running the direct overlay scorer, which shares overlay-space
machinery with the cache path); this is counted under
`fixed_topology_persistent_cache_oracle_mismatches`.

Sequential `fixed_topology_exact` local commits are gated against the recorded
chain objective (the previous accepted after-topology score), not the
candidate's own selected before-topology score, so the chain objective is
non-increasing across accepts.  A candidate whose selected before-topology
score does not equal the chain objective is counted under
`fixed_topology_chain_objective_before_mismatches` as a diagnostic (its before
certificate is not the chain tip's topology); the gate still uses the chain
objective.

### Transient chain extension for `exact_multisite` verification (Phase 9)

The local-commit `exact_multisite` verifier may verify each candidate by
transiently extending the committed overlay chain in **reader-local scratch
storage** (never mutating the shared chain or caches, bypassing the Phase-4
commit barrier), reading the exact frontier on the extended grammar, and
discarding. This is installed automatically when a
local-commit substrate is active (`rebuild_after_accept = false`); conservative
rebuild mode leaves it off and uses the cold from-scratch path
(`verify_candidate_exact_against_state`).  A candidate whose delta cannot be
appended to the scratch chain (tombstone scope: it tombstones a production that
does not resolve to a frozen-base production) falls back to the cold path for
verification, so it can still be accepted and reach the Phase-4 commit-time
tombstone-scope skip.

The transient work is counted under
`transient_chain_extensions_for_verification`, **never** under
`full_overlay_materializations`. The exact B&B still consumes a materialized
extended grammar, but this historical counter distinction keeps transient
verification separate from the explicit cold materialize-per-candidate path.

**Phase 9 caveat -- the persistent rows do not feed B&B.** The production B&B
scorer (`build_multisite_trim_active` ->
`build_multisite_trim(grammar, patterns, ...)`) rebuilds its exact setup from
the materialized extended grammar and takes no persistent-cache argument.
Copying and advancing the inside/outside caches on every production candidate
was therefore dead work and has been removed. Consequences:

- With the two-chart oracle disabled, production transient verification copies
  and advances no inside/outside cache. The separate
  `transient_chain_diagnostic_cache_extensions` counter must remain zero.
- Enabling `verify_transient_chain_extension_oracle_for_tests` constructs the
  reader-local cache copies, advances them with the same paired commit
  primitives as a real commit, and compares both charts with a cold oracle.
  That diagnostic work is reported only by
  `transient_chain_diagnostic_cache_extensions`.
- A forced diagnostic mismatch still discards the transient answer, runs the
  authoritative cold B&B, and preserves the truthful `grammar_exact` label.
- A future warm-started B&B may consume persistent rows directly. The
  diagnostic path preserves the affected-set and two-chart oracle needed to
  validate such work without charging its cache copies to current production
  candidates.

The plan's cross-cutting "dense materialization" warning is the thing to watch:
a full exact-setup rebuild still happens per candidate inside
`build_multisite_trim` on the production path. Removing the unused diagnostic
cache copy avoids compounding that cost, but feeding persistent rows into B&B
remains separate work and the rebuild stays visible in the exact-setup
counters.

**Exit-criterion-3 wording deviation (justified).**  The plan says that on
oracle mismatch the exactness label is "otherwise weakened and the from-
scratch path is used."  This implementation does **not** weaken the label: it
substitutes the cold exact optimum and **keeps** `grammar_exact`.  That is the
right call because the cold B&B is genuinely exact, so weakening would
mislabel a correct value as a mere lower bound.  The substantive requirement --
"do not trust a wrong transient result; use the authoritative cold value" -- is
met; only the literal "weakened" is not honored.  The fallback is still counted
under `transient_chain_extension_fallbacks` /
`transient_chain_extension_oracle_mismatches` so a regression to a wrong
transient result is visible in the counters.

## Polytomy and binary chart compatibility

The chart recurrence requires a binary chart-compatible grammar.  `dagutil` uses
`--wric-polytomy-mode` for chart-SPR grammar construction:

- `reject` (default): fail fast if high-arity productions remain;
- `expand-exact`: run exact soft refinement or fail if caps are exceeded;
- `expand-bounded`: run bounded soft refinement with audit counters;
- `audit-kary`: audit mode for diagnostics, not a valid chart-SPR search input
  if unresolved high-arity productions remain.

Reports print `polytomy_mode` and `binary_chart_compatibility` so the chosen
policy is visible.

## Stable identities

Dense clade/production IDs are only stable inside one grammar build.

- Candidate dedup and reports use stable moved/parent/sibling/target taxon keys
  plus removed/added production taxon keys; `dagutil` prints sample-ID based
  signatures where possible.
- Fixed-topology certificates store overlay production refs for in-candidate
  identity and taxon/sample production signatures so they can survive overlay
  materialization, accepted-state rebuilds, and report round trips.

## Benchmark commands

Smoke comparison:

```bash
tools/wric_spr_search_benchmark.sh --smoke
```

Development comparison:

```bash
tools/wric_spr_search_benchmark.sh \
  --iterations 3 \
  --seed 1 \
  --max-candidates 128 \
  --top-k-exact 16 \
  --include-data-fixtures \
  --include-heuristic
```

See [`WRIC-SPR-SEARCH-BENCHMARK.md`](WRIC-SPR-SEARCH-BENCHMARK.md) for the full
benchmark harness and [`WRIC-SPR-SEARCH-BENCHMARK-RESULTS.md`](WRIC-SPR-SEARCH-BENCHMARK-RESULTS.md)
for committed CI-scale sanity tables.

## Regression checklist

Before changing chart-SPR search internals, verify:

- no hot-loop calls to `build_composite_chart_score()`;
- no hot-loop calls to `materialize_overlay_grammar()` except oracle/debug mode,
  counted exact verification, or counted accepted-state materialization;
- no non-test production optimizer calls to the legacy multisite helper APIs;
- the streaming candidate API is not implemented as a wrapper over eager
  all-path enumeration;
- candidate caps/path budgets stop lazy expansion before path-pair explosion;
- active-only helper inputs have zero skipped-invariant metadata;
- root-row scoring goes through `chart_spr_weighted_root_score_from_row()`;
- candidate/fixed-topology identities use stable taxon/sample signatures, not
  dense-only IDs;
- counters for local scores, composite rebuilds, overlay materializations,
  exact verifications, transient chain extensions for verification, reachability
  traversals, candidate-generation paths, and accepted-state rebuilds remain
  exposed in structured/JSON-style reports.
