# WRIC chart-SPR usability

## Outcome

Chart-SPR has a conservative usability mode for the case that motivated this
work: optimize the frozen medium tree directly as an arbitrary-arity DAG and
commit a real parsimony improvement comparable to the pre-WRIC
sample--SPR--merge loop. The mode is:

```text
--chart-spr-search --chart-spr-additive-batch-union
```

It is intentionally a correctness-first materialize/rebuild path. Its
acceptance objective is an independently computed minimum DAG parsimony, not
the composite chart lower bound. It also verifies a concrete arbitrary-arity
topology with the fixed-topology chart and requires that chart score to equal
an independent edge-based score before committing the batch.

The authoritative frozen-medium, eight-worker run reduced the independently
scored DAG from 1642 to 1620 in one accepted and committed compound batch. The
serialized result reloaded, validated, and independently rescored at 1620.
This is better than the frozen pre-WRIC seed-1 result of 1627.

This milestone has no wall-clock acceptance gate. Exact performance
measurement belongs to the later benchmark campaign, when the CPU can be
reserved.

## Why the optimized ordinary chart loop accepted no moves

The zero-accept observation was real and concerning, but comparison with the
frozen pre-parallelization executable did not identify a parallelization
acceptance regression. Baseline and current runs produced byte-identical
candidate/local-score prefixes on the frozen medium fixture, and both accepted
zero moves in the first 32 and first 1024 candidate investigations.

The repository baseline used for that comparison is commit `7ca527b`
(`Baseline measure`); the completed parallelization state under investigation
is commit `93fb663` (`Close WRIC chart parallelization acceptance`). Both
descend from chart source tip `408434e` (`Add chart row fluidity`). Keeping
these identities in the record avoids treating a rebuilt current executable as
the historical control.

Two correctness/semantics issues explain the unusable result.

### A native improvement is an additive union, not one replacement

The ordinary chart loop scores one candidate overlay as a replacement and
accepts at most one independently improving candidate. The native
sample--SPR--merge loop instead retains many ranked moves at each radius,
materializes every complete SPR fragment, and merges all fragments into the
current DAG. Recombination among the fragments can contain a better topology
even when no fragment is better by itself.

The frozen seed-1 native crosswalk made this distinction concrete:

| Evidence | Result |
|---|---:|
| Distinct retained native moves | 53 |
| Individually materialized fragment scores | 1643--1658 |
| Input score | 1642 |
| Full fragment-union score | 1627 |
| Optimal trees in the full union | 28 |
| Moves whose source parent was multifurcating | 8 |
| Those source-parent arities | four arity-3, four arity-6 |
| Full union size | 1164 nodes, 1430 edges |
| Full union tree count | 38,080 |

No individual fragment can pass a strict `candidate < 1642` gate. The 1627
topology exists only in the additive union, so changing the ordinary
single-candidate acceptance mode cannot recover it.

### The generalized Fitch recurrence was wrong for `k > 2`

The compact-genome assignment path used the binary shortcut “intersect all
children if possible, otherwise union and add one.” That is not the Fitch
recurrence for a multifurcation. For child state sets
\(S_1,\ldots,S_k\), let \(m(x)\) be the number of child sets containing state
\(x\). The parent state set is

\[
\{x : m(x)=\max_y m(y)\}
\]

and the local cost increment is

\[
k-\max_y m(y).
\]

For example, children `{A}`, `{A}`, `{C}` select `{A}` with cost one; children
`{A}`, `{C}`, `{G}` select `{A,C,G}` with cost two. The old recurrence selected
`{A,C}` in the first case and added only one in the second. The same binary
assumption appeared in the native move scorer.

This bug explained a pre-fix mismatch in which a multifurcating witness scored
1626 in the chart but 1627 through the assigned compact genomes. Generalizing
both Fitch implementations restores exact arbitrary-arity assignment and
chart/external score parity while preserving binary and unary behavior.

The sampled-tree chart adapter also had binary-only exits: representative
construction rejected a production with more than two children, and
projection returned before its clone/diff fallback could handle a
multifurcating source parent. Those gates are removed for the additive path.

The generalized move scorer is covered end to end by a direct arity-3
regression: moving `G` from source parent `(A,C,G)` beside `T` has independently
re-Fitched score 3 -> 3 and predicted delta zero. The old binary recurrence
predicts `+1` on that fixture and fails the test.

Enabling that fallback exposed a second k-ary projection issue. Removing a
child from a multifurcating parent can leave sparse outgoing clade-index
groups. For example, deleting group 1 from groups `[0,1,2]` leaves `[0,2]`.
The moved topology is valid, but the grammar builder correctly requires each
node's group indices to be dense and rejected the after-tree before it could
be diffed. The fallback now groups edges by their old clade index, preserves
edges that represent alternatives in the same group, and relabels the ordered
groups to `[0,k)` before constructing the after-tree grammar. Both before and
after grammars are explicitly built with polytomies allowed. This is index
normalization only; it neither resolves nor binarizes the topology.

## Conservative additive transaction

One additive iteration performs the following transaction:

1. Independently score the current DAG and sample a stored-parsimony-minimum
   topology.
2. Reassign that topology with generalized k-ary Fitch, recompute edge
   mutations, and require fixed-topology chart/external score parity.
3. Enumerate native ranked SPR moves at doubling radii, retaining at most 50
   moves per radius by default.
4. Project retained moves through the chart candidate adapter and require each
   projected candidate to carry a complete after-topology certificate. Failed
   or incomplete projections are not materialized.
5. Dense-materialize and validate each chart candidate, map its complete
   certificate into that dense grammar, and build the complete SPR fragment
   from the certified topology. Merge the current DAG, sampled topology, and
   all such fragments.
6. Independently score the tentative DAG, sample its minimum topology,
   reassign it with generalized k-ary Fitch, and verify that topology exactly
   with the fixed-topology chart.
7. Merge that verified witness into the output and validate the DAG. Commit
   the whole transaction only when the exact chart-verified witness is strictly
   better than the independently re-Fitched sampled source topology and both
   the witness and final output are strictly below the prior DAG score. This
   prevents source-tree re-annotation alone from being credited as an SPR
   improvement.
8. Rebuild the accepted grammar with polytomies allowed.

The accepted output deliberately need not be trimmed. Retaining the complete
union is the behavior needed to expose recombined topologies.

## Frozen medium acceptance

The maintained regression is
`test/dagutil_chart_spr_additive_medium_test.sh`. It pins these inputs:

| Fixture | SHA-256 |
|---|---|
| `data/seedtree/seedtree.pb.gz` | `2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153` |
| `data/seedtree/refseq.txt.gz` | `088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c` |

The test has quality and correctness gates, but no elapsed-time comparison:

- independently score the input as exactly 1642;
- use the direct `--wric-polytomy-mode allow` grammar, whose maximum arity is
  greater than two, without a binary surrogate;
- project at least one move from a multifurcating source parent;
- materialize a complete fragment from every projected chart candidate's exact
  after-topology certificate;
- exactly chart-score a witness containing a multifurcating production and
  require equality with its independent external score;
- commit a genuinely improving additive transaction;
- serialize, reload, and validate the output DAG;
- independently rescore the reloaded output at no more than 1627; and
- confirm that the reloaded output grammar still has arity greater than two.

The authoritative eight-worker result is:

| Field | Result |
|---|---:|
| Direct polytomy mode | `allow` |
| Initial -> final independent DAG score | 1642 -> 1620 |
| Accepted/committed updates | 1 compound batch |
| Moves enumerated | 437 |
| Moves retained / projected | 290 / 290 |
| Certified chart candidates materialized | 290 |
| Projected moves from a multifurcating parent | 220 |
| Maximum source-parent arity | 10 |
| Output grammar maximum arity | 10 |
| Exact-witness multifurcating productions | 105 |
| Exact witness chart / external score | 1620 / 1620 |
| Reloaded, validated output size | 1166 nodes / 1415 edges |
| Reloaded output SHA-256 | `ae9c0c1b746439e3544eb2e8cef877c404e019d493d173a922717b1772a2b25b` |
| Independent input distribution | min 1642 (count 1), max 1642 (count 1) |
| Independent reloaded output distribution | min 1620 (count 4), max 1642 (count 2) |

The output is intentionally not required to be trimmed. Its acceptance
contract is a valid arbitrary-arity DAG containing an independently verified
1620 witness and having minimum external score 1620. The independently
reloaded acceptance artifact, `/tmp/chart-additive-medium-final.pb.gz`,
produced the output size and full parsimony extrema above. Its maximum-score
trees demonstrate explicitly that unrelated suboptimal trees may remain in
the additive union.

A generous CTest safety timeout only detects a hang; it is not a benchmark or
a performance acceptance threshold.

Run the maintained gate with:

```bash
cmake --build build --target dagutil
ctest --test-dir build --output-on-failure \
  -R '^dagutil_chart_spr_additive_medium$'
```

The equivalent direct command is:

```bash
build/bin/dagutil \
  --tree-pb data/seedtree/seedtree.pb.gz \
  --refseq data/seedtree/refseq.txt.gz \
  --force-no-vcf --validate \
  --wric-polytomy-mode allow \
  --chart-score-ua-edge \
  --chart-spr-search \
  --chart-spr-additive-batch-union \
  --chart-spr-acceptance fixed-topology-exact \
  --chart-spr-candidate-source sampled-tree \
  --chart-spr-additive-batch-max-moves 50 \
  --chart-spr-sampled-tree-score-threshold -1 \
  --chart-spr-max-iterations 1 \
  --chart-spr-workers 8 \
  --seed 1 \
  -o build/chart-additive-medium.pb.gz
```

Independently reload, validate, and score the artifact with:

```bash
build/bin/dagutil \
  --dag-pb build/chart-additive-medium.pb.gz \
  --force-no-vcf --validate --parsimony \
  --wric-polytomy-mode allow --wric-polytomy-report
```

## Scope and limitations

- This is one additive batch transaction, not attribution of the improvement
  to a single accepted move. The report therefore treats
  `accepted_move_committed` as authoritative even though there may be no
  singular accepted-candidate record.
- Predicted native move deltas rank moves; they are not the acceptance
  objective. Generic per-candidate score counts refer to that ranking work;
  `candidates_exact_verified` and the batch witness fields describe the one
  collective exact acceptance evaluation.
- A k-ary projection's legacy singular `old_sibling` field names one
  deterministic representative cochild. The complete before/after topology
  certificates, not that compatibility field, govern scoring and fragment
  materialization.
- The mode requires UA-edge chart scoring so chart and external objectives use
  the same convention.
- Local overlay commits and canonical semantic capture are intentionally
  unsupported in this conservative path.
- The output is a valid, score-improved, arbitrary-arity DAG but may contain
  many suboptimal trees because trimming is outside this usability milestone.
- Throughput, worker scaling, memory tuning, and comparison with native wall
  time remain follow-up work. They must not weaken the frozen quality gates
  above.
