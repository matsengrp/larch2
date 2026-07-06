# WRIC chart-SPR benchmark results

Committed Phase-8 sanity tables from the harness in `tools/wric_spr_search_benchmark.sh`. These are CI-scale runs, intended to make the comparison shape reproducible rather than to claim final performance numbers or satisfy a strict final benchmark suite by themselves.

A strict final Phase-8 table should additionally run the exact chart-SPR modes (`sampled_tree_fixed`, `grammar_exact`, and `hybrid_exact`) on the chosen medium and real fixtures in an appropriate benchmark build. The ASAN tables below deliberately keep exact mode to the small fixture so they remain reproducible during development.

## Phase-5 local-commit caveats

`--chart-spr-local-accept-updates` now uses the base-plus-overlay-chain local-commit path: accepted moves append to the overlay chain and update the persistent inside/outside chart caches, avoiding per-accept dense `materialize_overlay_grammar()` plus sidecar rebuild. The per-accept dense-materialization counter is `overlay_materializations_for_accept_materialization`, which should remain zero for local-commit runs; the umbrella `full_overlay_materializations` can still be nonzero because exact candidate verification may materialize candidate overlays -- either via the Phase-9 transient path's tombstone-scope cold fallbacks, or for candidates the transient path does not serve. (The Phase-9 transient path itself does not bump `full_overlay_materializations`; its work is counted under `transient_chain_extensions_for_verification`. See the Phase-9 caveat in `WRIC-SPR-SEARCH.md`: as shipped the transient path lands substrate + oracle, not a wall-clock win.)

Current limitations are explicit rather than silent fallbacks: local commit requires an exact gate (`exact_multisite` or `fixed_topology_exact`), rejects `lower_bound_heuristic`, and rejects `score_ua_edge=true` until the persistent outside-cache path has a documented per-pattern reference-state convention.

Final local-commit compaction is grammar-valued: the chain is densely materialized once, the compacted output DAG preserves accepted overlay production witnesses by taxon-set key, and the reported final parsimony is the output DAG's exact B&B optimum labelled by `final_compaction_exactness_kind`. The final compaction materialization is counted separately as `overlay_materializations_for_final_compaction`; the final safety rebuild remains reported as `final_compaction_rebuilds`/`final_compaction_ms`, not as a per-accepted-move sidecar rebuild.

## Small/medium/repo-DAG lower-bound smoke comparison

Command:

```bash
tools/wric_spr_search_benchmark.sh \
  --dag data/test_5_trees/tree_0.pb.gz \
  --tree data/seedtree/seedtree.pb.gz:data/seedtree/refseq.txt.gz \
  --dag data/testcase/full_dag.pb.gz \
  --modes grammar_lower_bound \
  --max-candidates 1 \
  --top-k-exact 0 \
  --max-moves 1 \
  --iterations 1
```

| fixture | scale | method | status | validation | initial parsimony | final validated parsimony | best reported objective | wall_s | candidates scored | total_ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `data/test_5_trees/tree_0.pb.gz` | small | sample_explore_merge | ok | ok | 174 | 174 | 174 | 0.857819 | NA | NA |
| `data/test_5_trees/tree_0.pb.gz` | small | chart_spr_grammar_lower_bound_heuristic | ok | ok | 174 | 174 | 174 | 0.045441 | 1 | 19.683 |
| `data/seedtree/seedtree.pb.gz` | medium | sample_explore_merge | ok | ok | 1642 | 1633 | 1633 | 46.691196 | NA | NA |
| `data/seedtree/seedtree.pb.gz` | medium | chart_spr_grammar_lower_bound_heuristic | ok | ok | 1642 | 1642 | 1639 | 2.510686 | 1 | 2025.382 |
| `data/testcase/full_dag.pb.gz` | repo multi-tree DAG | sample_explore_merge | ok | ok | 75 | 75 | 75 | 1.443565 | NA | NA |
| `data/testcase/full_dag.pb.gz` | repo multi-tree DAG | chart_spr_grammar_lower_bound_heuristic | ok | ok | 75 | 75 | 71 | 0.397193 | 1 | 84.931 |

`best_reported_objective` is method-specific. For sample--explore--merge, the baseline endpoint is the externally validated output-DAG parsimony; larch2's native iteration summary reports a sampled-tree objective, which can be stale relative to the final merged DAG minimum. In the lower-bound chart-SPR row for `full_dag.pb.gz`, 71 is the composite lower-bound objective; externally validated parsimony remains 75.

## Small exact-mode comparison

Command:

```bash
tools/wric_spr_search_benchmark.sh \
  --dag data/test_5_trees/tree_0.pb.gz \
  --max-candidates 1 \
  --top-k-exact 1 \
  --max-moves 1 \
  --iterations 1
```

| fixture | method | status | validation | initial parsimony | final validated parsimony | best reported objective | wall_s | acceptance | candidate source | candidates scored | exact verifications | total_ms |
|---|---|---:|---:|---:|---:|---:|---:|---|---|---:|---:|---:|
| `data/test_5_trees/tree_0.pb.gz` | sample_explore_merge | ok | ok | 174 | 174 | 174 | 0.856650 | sample_explore_merge | sampled_tree | NA | NA | NA |
| `data/test_5_trees/tree_0.pb.gz` | chart_spr_sampled_tree_fixed_topology | ok | ok | 174 | 174 | 174 | 1.352567 | fixed_topology_exact | sampled_tree | 1 | 1 | 1303.922 |
| `data/test_5_trees/tree_0.pb.gz` | chart_spr_grammar_exact | ok | ok | 174 | 174 | 174 | 3.621185 | exact_multisite | grammar | 1 | 1 | 3554.730 |
| `data/test_5_trees/tree_0.pb.gz` | chart_spr_hybrid_exact | ok | ok | 174 | 174 | 174 | 4.883265 | exact_multisite | hybrid | 1 | 1 | 4805.197 |
