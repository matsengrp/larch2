# WRIC chart-SPR search Phase-10 surface (CLI / report / identity)

Phase 10 of the DAG-native SPR & rank-3 rewrite plan is the cross-cutting
"surface" phase: it exposes the commit / rewrite / verification modes and all
counters through `dagutil` and the search report, locks down the counter
contract, and gives chain entries and Option-C rewrite identities a stable
taxon-set-key representation that round-trips through JSON reports.  It adds no
new chart recurrence, no new overlay-vocabulary field, and no new candidate
generation; everything here reports or labels machinery the earlier phases
(1-9) already implement.

This doc is the entry point for the Phase-10 deliverables.  The CI-scale
sanity table lives at `doc/WRIC-CHART-SPR-SEARCH-PHASE10-SANITY.md` (regenerated
by `doc/wric_phase10_sanity_table.sh`); the counter baseline it diffs against
is `doc/WRIC-CHART-SPR-SEARCH-COUNTER-BASELINE.md` (Phase 0).

## What Phase 10 adds

### Named commit / verification modes

Two new enums (`include/larch/chart_spr_search.hpp`) name the accepted-state
commit path and the exact-verification path a run selects, surfaced as labelled
report fields and CLI flags. Their enum defaults are unchanged.

The later Phase-10 worker-policy promotion is deliberately limited to the
`dagutil` CLI: omitting both worker options requests automatic affinity-based
resolution and reports `automatic_default`. Explicit
`--chart-spr-workers 1` continues to select the serial oracle, and direct
library defaults remain unchanged.

- `chart_spr_commit_mode`: `overlay_delta` (default, the Phase-4 local-commit
  SPR path / Option A/B materialize-and-merge) or `option_c` (rank-3 Option-C
  commits, reachable only through the library API `option_c_commit_via_chain`,
  Phase 6/7).  `option_c` is NOT honored by the search loop: the candidate
  generator always produces SPR overlay-delta commits, so selecting it for
  `run_chart_spr_search` throws a labelled error from
  `validate_chart_spr_search_loop_options` rather than silently reporting a
  label the loop did not honor (no silent fallback).
- `chart_spr_verification_mode`: `transient` (default, the Phase-9
  chain-extension exact_multisite verifier, counted under
  `transient_chain_extensions_for_verification`) or `cold` (the from-scratch
  `verify_candidate_exact_against_state` path, counted under
  `overlay_materializations_for_exact_verification`).

`dagutil` flags:

| Flag | Values | Effect |
|------|--------|--------|
| `--chart-spr-local-accept-updates` | (flag) | Local-commit mode (Phase 4). |
| `--chart-spr-commit-mode <M>` | `overlay-delta` (default), `option-c` | Named commit path.  `option-c` is reachable only via the library API (`option_c_commit_via_chain`); selecting it for the search loop throws a labelled error. |
| `--chart-spr-verification-mode <M>` | `transient` (default), `cold` | Named verification path; `cold` skips installing the transient verifier. |
| `--chart-spr-identity-report-json <PATH>` | path | Write the overlay chain's JSON identity report for a local-commit run. |

The report now emits `commit_mode`, `verification_mode`, and
`chain_per_accept_exactness_label` (the chain's per-accept exactness label =
the acceptance mode for exact gates; `none_conservative_materialize_rebuild`
for the conservative path).

### Counter-contract completeness

Every counter named in the plan's cross-cutting counter contract is now present
in the `dagutil` report (both the summary section and the `counters:` dump).
Phase 10 filled in the Phase-8/9 counters that were defined on the structs but
not emitted:

- `fixed_topology_persistent_cache_direct_oracle_mismatches`
- `fixed_topology_icache_rows_reused`
- `fixed_topology_icache_rows_recomputed_affected`
- `fixed_topology_chain_objective_before_mismatches`
- `transient_chain_extensions_for_verification`
- `transient_chain_extension_fallbacks`
- `transient_chain_extension_oracle_mismatches`

A regression to "full rebuild per accept" or "dense materialize per candidate"
is therefore visible in the report, not hidden behind a renamed counter.  The
load-bearing per-accept pair is:

- `sidecar_rebuilds_after_accept == 0` for a local-commit run;
- `overlay_materializations_for_accept_materialization == 0` for a local-commit
  run (the umbrella `full_overlay_materializations` is nonzero on any exact run
  because per-candidate exact verification materializes, and only reaches 0
  once Phase 8/9 eliminate that — see the Phase-4 exit-criterion note in the
  plan for why the per-accept counter, not the umbrella, is the contracted
  one).

### Chain / Option-C identity surface

`include/larch/phase10_report.hpp` is the identity half of Phase 10.  It gives
the overlay chain's entries (one per accepted delta) a stable taxon-set-key
representation and a JSON round trip:

- `build_phase10_chain_identity_report(chain)` builds a per-delta report:
  position (ordinal), `commit_source` label (`spr_overlay_delta` or
  `option_c_chain_commit`), added production taxon-set keys, and tombstoned
  base-production keys.
- `emit_phase10_chain_identity_report_json` / `parse_*_json` round-trip the
  report through JSON.
- `phase10_chain_net_production_keys` gives the net production key set the
  chain represents (every base key minus every tombstoned key plus every
  added key).  It is exercised by the round-trip tests as a faithful superset
  of the materialized chain grammar's keys (it records every production added
  or tombstoned, including those later pruned by reachability).

Identity follows Work item 1's convention: **chain position is an ordinal**
(ordering only); **taxon-set key is the identity** (survives materialize /
rebuild / report); **the tombstone rule governs re-acceptance** (enforced by
`overlay_chain::append`, not by this report).  The `commit_source` label is
carried through `overlay_chain::append`'s rebase so Option-C entries stay
labelled after they are folded onto the chain.

A `commit_source` field was added to `spr_overlay_delta` (default empty =
`spr_overlay_delta`; `option_c_as_overlay_delta` sets it to
`option_c_chain_commit`, kept in sync with
`option_c_chain_commit_result::commit_label`).

## Round-trip property (exit criterion 2)

"A JSON report's chain entries and Option-C rewrite identities survive
materialize -> rebuild -> report without loss (keys preserved)."  This is
checked in `test/chart_spr_phase10_test.cpp`:

- The Option-C library round trip builds a chain with a rank-3 child-set
  rewrite, emits the JSON identity report, parses it back, materializes the
  chain to a grammar, compacts to an output DAG, rebuilds the grammar, and
  asserts the materialized and rebuilt grammars carry the **same** production
  taxon-set keys, and that the report's added keys are present / tombstoned
  keys absent.
- The search-loop round trip runs a local-commit SPR search, parses the emitted
  `chain_identity_report_json`, and asserts the report's added productions are
  present in the rebuilt output grammar and its tombstoned productions absent.

Note: the materialized/rebuilt grammar prunes productions that became
unreachable after a tombstone (e.g. an Option-C rewrite that removes the last
production referencing a clade).  The round trip therefore checks set equality
between the materialized and rebuilt grammars (which prune identically), and
membership for the report's per-delta added/tombstoned keys against the
materialized grammar.  The report's net-key set is a faithful superset (it
records every production the chain added or tombstoned, including those later
pruned by reachability).

## CI-scale sanity table

`doc/wric_phase10_sanity_table.sh` runs the search on a fixture **known to
accept an improving move** (`test/wric_four_taxon_misplaced.{fa,nwk,ref}`, the
same four-taxon "misplaced" tree the library-level Phase-10 tests use) in
conservative and local-commit modes (plus a cold-verification run unless
`--smoke`), emits a markdown table diffing the contracted counters, and exits
non-zero on a contract violation so it can gate CI.  The fixture is chosen so
the per-accept contract is **non-vacuous**: the single generated candidate is
an improving move that is accepted exactly once, so `local_commit_accepted_moves
> 0`, the conservative per-accept counters (`sidecar_rebuilds_after_accept`,
`overlay_materializations_for_accept_materialization`) are `> 0`, and the
local-commit per-accept counters are `0` -- the load-bearing contrast a
vacuous all-zero table (the original `data/test_5_trees/tree_0` fixture under
`--chart-spr-max-candidates 1`, which accepts 0 moves) could not show.  The
cold-verification column reads its counters from the cold run's report (not the
transient run's).  Smoke mode is the ctest (`wric_phase10_sanity_table_smoke`);
the cold-verification row is separately covered by
`dagutil_chart_spr_search_verification_cold`.

```sh
cmake --build build -j
./doc/wric_phase10_sanity_table.sh            # full table
./doc/wric_phase10_sanity_table.sh --smoke    # CI-fast (ctest path)
```

## Tests

- `test/chart_spr_phase10_test.cpp` (library-level): counter-contract field
  presence, local-commit contracted values + mode labels, conservative-mode
  unchanged structure, cold verification-mode toggle, `commit_mode == option_c`
  labelled-throw enforcement, JSON identity round trip (Option-C library +
  search-loop SPR), and a **2-delta** Option-C chain identity report
  (exercises `build_phase10_chain_identity_report`'s per-delta resolution
  against a shared tip, plus `phase10_chain_net_production_keys`).
- `dagutil_chart_spr_search_local_commit_report`: the report carries every
  contracted counter with the contracted values + mode labels, AND the fixture
  accepts a move (`local_commit_accepted_moves > 0`, non-vacuous).
- `dagutil_chart_spr_search_verification_cold`: `cold` mode toggles the path
  (`verification_mode: cold`, `transient_chain_extensions_for_verification: 0`)
  and still accepts a move (`local_commit_accepted_moves > 0`).
- `dagutil_chart_spr_search_identity_report_json` +
  `dagutil_chart_spr_identity_report_json_valid`: the identity JSON is emitted
  and valid.
- `wric_phase10_sanity_table_smoke`: the sanity table passes its contract.

These dagutil ctests and the sanity table use the `test/wric_four_taxon_misplaced`
fixture (accepts exactly one improving move), so the per-accept counter contract
is exercised for real -- the original `data/test_5_trees/tree_0` fixture under
`--chart-spr-max-candidates 1` accepts 0 moves and made the contract vacuous.

ASAN cleanness on cache-mutating paths and TSAN cleanness on multi-worker runs
are checked by the ASAN / TSAN builds of the whole suite (the epoch/snapshot
barrier from Phase 4 is load-bearing for the TSAN check).
