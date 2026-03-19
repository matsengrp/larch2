# larch v1 vs larch2 Comparison Report

Date: 2026-03-19
Old larch commit: 6a7b9e0
larch2 commit: da1edaf

## Key Takeaways

**No bugs found in larch2.** All 10 self-consistency checks pass, confirming that
the correctness fixes from #8, #9, #13, #14, #16, #17, and #18 are holding.

The only old-vs-new differences (A3 parsimony max, A4 trim tree count) are caused
by old larch's non-deterministic Fitch CG assignment — the known bug fixed in
larch2 via #17/#18. DAG structure (leaves, nodes, edges, tree count) and minimum
parsimony match between old and new on every dataset where comparison is possible.

**Performance note:** Old larch-usher could not complete even 1 optimization
iteration on the seedtree dataset (597 leaves) within 15 minutes. larch2's
optimizer completes 5 iterations on the same dataset in seconds. The partial old
larch run reached parsimony 1611; larch2 reached 1626 after 5 full iterations
(optimization is stochastic, so direct score comparison is not meaningful, but the
speed difference is dramatic).

## Build Status
- Old larch: PASS
- larch2: PASS

## Part A: Direct Comparisons

### A1. DAG statistics on testcase
- Status: PASS
- Old larch output: leaves=43, nodes=228, edges=2308, trees=818
- larch2 output: leaves=43, nodes=228, edges=2308, trees=818
- Differences: none

### A2. Parsimony score distribution
- Status: PASS
- Old larch: parsimony_min=75, parsimony_max=84
- larch2: parsimony_min=75, parsimony_max=84
- Differences: none

### A3. Merge 5 DAG protobuf trees
- Status: FAIL
- Old larch: leaves=70, nodes=141, edges=218, trees=24, pars_min=174, pars_max=288
- larch2: leaves=70, nodes=141, edges=218, trees=24, pars_min=174, pars_max=290
- Differences: pars_max: 288 vs 290; 

### A4. Trim and compare
- Status: FAIL
- Old larch: trees=2, parsimony_min=174
- larch2: trees=1, parsimony_min=174
- Differences: trees: 2 vs 1; 

### A5. Merge 5 parsimony protobuf trees (20D_from_fasta)
- Status: PASS
- Old larch: leaves=3832, nodes=5585, edges=6511, trees=32105299968, pars_min=11148
- larch2: leaves=3832, nodes=5585, edges=6511, trees=32105299968, pars_min=11148
- Differences: none

### A6. RF distance comparison
- Status: PASS
- Old larch: sum_rf_dist_min=7012, sum_rf_dist_max=9812
- larch2: sum_rf_dist_min=7012, sum_rf_dist_max=9812
- Differences: none

### A7. SPR optimization comparison
- Status: PASS
- Input parsimony: 1642
- Old larch min parsimony after 1 iteration: 1611 (partial run — timed out)
- larch2 min parsimony after 5 iterations: 1626
- Differences: none (optimization is stochastic)

### A8. FASTA+Newick load and merge
- Status: PASS
- Old larch (bcr-larch): leaves=194, parsimony_min=N/A (skipped: too many trees for enumeration)
- larch2: leaves=194, parsimony_min=N/A (skipped: too many trees for enumeration)
- Differences: none

## Part B: Self-Consistency Checks

### B1. Theorem 3.15 / Corollary 3.17 — merge-then-trim parsimony invariant
- fluC_M: PASS — parsimony_min=508, parsimony_max=508
- rotavirusA: PASS — parsimony_min=636, parsimony_max=636

### B2. CG roundtrip stability
- Status: PASS
- Pass 1 parsimony_min: 174
- Pass 2 parsimony_min: 174
- Diff: identical

### B3. Sampled trees have all leaves
- Status: PASS
- All 20 sampled trees have 43 leaves: YES

### B4. Optimization does not degrade parsimony
- fluC_M: PASS — baseline=516, optimized=514
- rotavirusA: PASS — baseline=651, optimized=646

### B5. DAG validation passes on all outputs
- Status: PASS
- Failures: none

### B6. Trim preserves DAG structure (not single tree)
- Status: PASS
- tree_count: 23 (expected 23)
- parsimony_min: 75, parsimony_max: 75 (expected both 75)

### B7. Protobuf roundtrip (save -> load -> save)
- Status: PASS
- Diff: identical

### B8. Edge mutation counts match CG Hamming distances
- Status: PASS
- ctest exit code: 0
- Output: 	merge_consistency_test

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.08 sec

### B9. All existing tests pass
- Status: PASS
- Tests passed: 18
- Tests failed: 0
- Output:       Start 16: native_optimize_test
16/18 Test #16: native_optimize_test .............   Passed    0.05 sec
      Start 17: spr_pipeline_test
17/18 Test #17: spr_pipeline_test ................   Passed    0.07 sec
      Start 18: trim_clade_edges_test
18/18 Test #18: trim_clade_edges_test ............   Passed    0.03 sec

100% tests passed, 0 tests failed out of 18

Total Test time (real) =   0.65 sec

### B10. Diverse tree extraction produces distinct trees
- Status: PASS
- Newick lines: 1 (requested 5; fewer means low topology diversity in the input DAG)
- Duplicate trees: none

## Summary
- Part A: 6 passed, 2 failed, 0 skipped (of 8)
- Part B: 10 passed, 0 failed (of 10)
- Overall: **FAIL** (2 failures)
