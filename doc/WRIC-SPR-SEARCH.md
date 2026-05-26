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
  exact verifications, reachability traversals, candidate-generation paths, and
  accepted-state rebuilds remain exposed in structured/JSON-style reports.
