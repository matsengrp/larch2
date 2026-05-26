# WRIC chart-SPR benchmark results

Committed Phase-8 sanity tables from the harness in `tools/wric_spr_search_benchmark.sh`. These are CI-scale runs, intended to make the comparison shape reproducible rather than to claim final performance numbers or satisfy a strict final benchmark suite by themselves.

A strict final Phase-8 table should additionally run the exact chart-SPR modes (`sampled_tree_fixed`, `grammar_exact`, and `hybrid_exact`) on the chosen medium and real fixtures in an appropriate benchmark build. The ASAN tables below deliberately keep exact mode to the small fixture so they remain reproducible during development.

## Phase-9 local accept-update caveats

`--chart-spr-local-accept-updates` is currently a safe local-cache-update mode, not the full base-plus-overlay-chain design described in the planning notes. Accepted moves avoid full search-state rebuilds, but each accepted candidate is still committed by dense `materialize_overlay_grammar()` materialization into the next search grammar.

Final local-update compaction is also tree-valued: the code selects one concrete topology, materializes that tree, rebuilds the search state from the output DAG, and checks that the rebuilt objective equals the local sidecar objective. This is score-safe, but it does not preserve every accepted overlay production/topology in the output DAG. The final safety rebuild is reported separately as `final_compaction_rebuilds`/`final_compaction_ms`, not as a per-accepted-move sidecar rebuild. Use conservative rebuild mode when output DAG structural diversity must be preserved beyond the selected score-equivalent tree.

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
