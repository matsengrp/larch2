# Exact direct-grammar census of the 1614 lower bound

## Result

On the frozen 597-leaf chart artifact, the exact minimum over every complete
parse of the direct, unrefined, arbitrary-arity collapsed grammar is

\[
G^\star = 1616.
\]

There are 575,168 direct grammar parses and 12 distinct minimum topology
classes. Thus the chart's 1614 composite score is a strict per-pattern lower
bound: no direct grammar topology attains it. The exact topology-coupling gap
is two. This conclusion neither inserts binary refinements nor treats a
multifurcation as a soft polytomy; the enumerated productions retain their
original arities, up to arity 10.

Every one of the 12 minima was independently:

1. rescored by the existing four-state Sankoff implementation;
2. materialized as an arbitrary-arity tree;
3. assigned ancestral states and rescored by generalized Fitch;
4. serialized and reloaded;
5. validated as a DAG and a tree; and
6. rescored after reload.

All four score observations were 1616. One- and eight-worker censuses produced
identical score histograms, optimal ordinals, topology digests, and
materialized canonical digests. The 12 serialized W1 and W8 witness pairs are
also byte-identical. Wall-clock measurements were deliberately not used as
acceptance criteria.

The complete score distribution is in
[`WRIC-CHART-1614-SCORE-HISTOGRAM.tsv`](WRIC-CHART-1614-SCORE-HISTOGRAM.tsv),
and the minimum witnesses are indexed in
[`WRIC-CHART-1614-MINIMA.tsv`](WRIC-CHART-1614-MINIMA.tsv).

## Frozen inputs and implementation

| Item | SHA-256 |
|---|---|
| Repository base commit | `6feaba609223cb7457a1ba77a23b66aa5ecec413` |
| `data/seedtree/seedtree.pb.gz` | `2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153` |
| `data/seedtree/refseq.txt.gz` | `088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c` |
| `/tmp/chart-additive-medium-final.pb.gz` | `ae9c0c1b746439e3544eb2e8cef877c404e019d493d173a922717b1772a2b25b` |
| `/tmp/chart-additive-case-five-union.pb.gz` | `b0a35cfb857433c22cf8170b7f305694e389c7eaf5bb266effd241d4e6dd9d4c` |
| `build/wric-1614-experiments/reduced-provider.pb.gz` | `7c292472fae237f6ebb6c15364ea0dc0664b849720a06658bc39c0acb3535f7c` |
| Exact-census `build/bin/dagutil` | `febfa80a7298292f9045622d0afe0b0eb12aadcc92fc98943650c1f689ca6a1c` |
| Final verified `build/bin/dagutil` | `fbb91c1eddd4e55db1af659feff38f6af95e58ac89f72343dd46f2831dd1ebaa` |
| Census/final `grammar_topology_enumerator.hpp` | `64273e6a03b9949b2282e3ccaa7eef6cb7416a78045c7e6d9ab6a8954e4f9a40` |
| Census-run `grammar_topology_fitch_scorer.hpp` | `7254902c8e1e7f8ff5425a339d9076825997fadd0a42d8f3954ebcc92c179f3b` |
| Census-run `grammar_topology_census.hpp` | `fa54ed451d6424ea241038d601644b8abf39e18f152ff082f11429812c2c510e` |
| Final `grammar_topology_fitch_scorer.hpp` | `d47659cd7df89315c2108890e04d15be946a46dc0585b1873fc1e82a36a0902f` |
| Final `grammar_topology_census.hpp` | `05751453e93091e7b3ad48b376a9a2edc4081a50bb708ae0fa20243cb2e0376d` |

The census-run hashes pin the exact uncommitted implementation that generated
the evidence. Before final verification, the two new headers were
clang-formatted and `grammar_topology_census.hpp` gained a direct `<string>`
include; those source-only changes do not alter the algorithm. The final
hashes pin the implementation that passed the test gates.

`grammar_topology_enumerator` computes the exact sum/product parse count with
checked 64-bit arithmetic and deterministically un-ranks each ordinal. It
streams one reusable selected-production topology at a time and divides the
ordinal interval into disjoint contiguous worker ranges.

`grammar_topology_fitch_scorer` is an exact incremental scorer for unordered
unit-cost parsimony. For a production with child optimal-state sets
\(S_1,\ldots,S_k\), it counts how many child sets contain each nucleotide.
The parent optimal states have maximum count, and the local increment is
\(k-\max_x |\{i:x\in S_i\}|\). This is the arbitrary-arity generalized-Fitch
form of the same recurrence computed by the existing four-state Sankoff
scorer. The implementation caches child state sets and local weighted scores;
only selected-production or descendant-state changes are recomputed between
successive parses.

Periodic Sankoff checks supplied an independent algebraic oracle: every one of
the reduced grammar's 864 parses was checked, and 58 regularly spaced full
grammar parses were checked. Every exact minimum received additional Sankoff
and materialized-Fitch checks.

## Progression from source to full grammar

| Artifact | Collapsed clades | Productions | Direct parses | Labelled histories | Stored-history min | Composite lower bound | Exact direct-parse min |
|---|---:|---:|---:|---:|---:|---:|---:|
| Source tree | 1037 | 440 | 1 | 1 | 1642 | 1642 | 1642 |
| Five-outcome union | 1046 | 454 | 32 | 32 | 1620 | 1620 | 1620 |
| Reduced provider union | 1058 | 473 | 864 | 210 | 1620 | 1614 | 1617 |
| Full 55-outcome union plus source | 1095 | 554 | 575,168 | 45,084 | 1620 | 1614 | 1616 |

The full grammar has 133 non-binary productions and maximum arity 10. Its
1,110 exact patterns represent 29,903 sites, of which 28,665 are invariant.
The 864-parse reduced grammar is a deliberately small provider union assembled
from representative outcomes `11,22,38,40,45,31,264,36,84,33,86`. Deleting
any one of those 11 outcomes raises the reduced composite lower bound from
1614 to between 1615 and 1622, so it preserves all lower-bound opportunities
in a compact closure. The exact reduced deletion sweep is in
[`WRIC-CHART-1614-REDUCED-OUTCOME-ABLATIONS.tsv`](WRIC-CHART-1614-REDUCED-OUTCOME-ABLATIONS.tsv).

The reduced census has one score-1617 minimum, at ordinal 813. Its direct
topology SHA-256 is
`827d9c360245f56a2cd585d7401a7913600d3e8a4d42c0b41a05a69ef54f5d9e`;
its materialized canonical semantic, clade, and production digests are
`2dd2cf863d45f54bd5731fb5785649b1387c93405cf3691504cc08947f1d3e98`,
`ce3eb9821b9eff8ad4028048c8ca262e64957ccbaf6fb4ea895b6c6356a38747`,
and
`bb40078b915f487a427cd448b6bb99bbc9f84e6a4f5739c30617dda650c8cd79`.
The one- and eight-worker serialized witnesses are byte-identical, with
SHA-256
`417018217f33ac4cdbab6f3dc8d67da2efd7b337d88ddb10603447a460453948`.
Every one of the 864 reduced scores was cross-checked against the independent
Sankoff scorer in the parity run.

The full labelled DAG has four known score-1620 histories. All four have the
same nonzero regret support relative to the full 1614 bound: pattern 304 pays
1, pattern 312 pays 1, pattern 1002 pays 2, and pattern 1023 pays 2. Every
other exact pattern is at its individual lower bound. Their canonical
semantic digests are:

```text
29b1fb36c7cd7e6bf5a5a834c7102cfdba6377a60a3bb727c07f27ec6ecdc3f3
95fad9958bbd73854c75bc3a0080ee8af82b157a0e7a5dd572b0a4978e150800
c1a9f08330aca7c18786683df472be9e566b01e412517283ccad4bef4b61a2a3
3ec38d22602111ec77ba8ad7624ccd7b9bde758195fe5ea2c0d02765de273f5c
```

## Why 1614 is impossible

Every one of the 12 score-1616 full-grammar minima has the same regret vector
relative to the independently minimized per-pattern chart:

| Exact pattern | Representative site | Pattern lower bound | Score in every 1616 topology | Regret |
|---:|---:|---:|---:|---:|
| 304 | 7716 | 23 | 24 | +1 |
| 1023 | 27563 | 20 | 21 | +1 |
| All other 1,108 patterns | — | their minima | their minima | 0 |

Thus 1616 is not merely the smallest score observed by a heuristic. Exhaustive
enumeration proves that a common topology can simultaneously attain 1,108
pattern minima, but it must pay one step at each of patterns 304 and 1023.

The four-site provider experiment explains the first structural obstruction
at a smaller scale. It studies patterns 304, 312, 1002, and 1023, whose
individual minima sum to 49. All four patterns have multiplicity one, as
recorded with their representative sites, reference states, regrets, and
provider provenance in
[`WRIC-CHART-1614-PATTERN-PROVENANCE.tsv`](WRIC-CHART-1614-PATTERN-PROVENANCE.tsv).
Their direct-grammar Pareto frontier consists of:

| Scores `(304,312,1002,1023)` | Parses | Label-compatible parses | Four-site sum |
|---|---:|---:|---:|
| `(23,3,3,21)` | 4 | 0 | 50 |
| `(23,4,3,20)` | 2 | 0 | 50 |

The all-minimum intersection is empty. In particular, the pattern-312 and
pattern-1023 minimum sets are already disjoint. At reduced clade 1051
(462 taxa), pattern 312 needs production 460 or 462, whereas pattern 1023
needs production 463. Production 460 gives local costs 2 and 5 for those
patterns; production 463 gives 3 and 4. This is an exact one-step topology
tradeoff, not a failure to enumerate enough parses.

Fully scoring representatives of those two four-site Pareto points exposes
their collateral cost on the other patterns. Parse 436, with four-site vector
`(23,3,3,21)`, has full score 1635, 21 above the reduced bound. Parse 466,
with `(23,4,3,20)`, has full score 1637, 23 above it. Their non-four-site
regrets are:

| Parse | Nonzero non-four-site regrets `(pattern:+regret)` |
|---:|---|
| 436 | `56:+2, 125:+1, 199:+1, 506:+1, 543:+1, 717:+2, 808:+1, 812:+1, 847:+2, 936:+2, 1009:+1, 1019:+2, 1036:+1, 1063:+1, 1103:+1` |
| 466 | `56:+2, 125:+1, 199:+1, 393:+2, 506:+1, 543:+1, 717:+2, 808:+1, 812:+1, 847:+2, 936:+2, 1009:+1, 1019:+2, 1036:+1, 1063:+1, 1103:+1` |

Pair and triple intersections are retained in
[`WRIC-CHART-1614-FOUR-SITE-COMPATIBILITY.tsv`](WRIC-CHART-1614-FOUR-SITE-COMPATIBILITY.tsv).
The raw trace has SHA-256
`0074b0c9d231c8da91e42f42b293f0729918f3844dd56e81dad1280086b52c14`.

The minimum opportunities have the following captured outcome providers and
one sufficient production package for each route.  The package is not a claim
that every minimizing direct parse must use exactly those productions (for
example, pattern 312 can use production 460 or 462 at the conflicting clade):

| Site | Captured provider occurrences | Byte-distinct class representatives | Captured production package |
|---:|---|---|---|
| 7716 | 31 or 82 | 31 or 30 | 449 |
| 7899 | 264 | 114 | 352 and 460 |
| 26799 | 36 plus 84 | 35 plus 28 | 465 plus 132 |
| 27563 | 33 plus 86, or 34 plus 86 | 33 plus 39, or 32 plus 39 | 463 plus 470 |

The score progression for each provider combination is preserved in
[`WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv`](WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv).
The distinction matters: byte-identical occurrence aliases were collapsed
before the 55-outcome ablation. In particular, occurrences 82, 264, 36, 84,
34, and 86 map to byte-distinct representatives 30, 114, 35, 28, 32, and 39,
respectively. The two alternatives at 7716 are therefore classes 31/30, and
the two alternatives at 27563 are classes 33/32; they are not duplicate
occurrences masquerading as independent providers.

The four-site trace was collected with a session-local experimental
diagnostic rather than a supported `dagutil` mode. Its current source and
binary SHA-256 values are
`3ee6ba63cbc678cbe96b3dc17842c353e58f5f409a1ef8e2e7dc6534e5c8e7c7`
and
`798f6134e3c1a78b7582070584a17cb32314ed0035e50333f62c7ec87992c490`.
It can be rebuilt and rerun in this worktree as:

```bash
/home/ogi-agent/install/gcc-trunk/bin/g++-trunk \
  -O2 -g -DNDEBUG -std=c++26 -freflection \
  -Iinclude -Ibuild/generated -static-libstdc++ -static-libgcc \
  build/four_site_census.cpp -o build/four_site_census \
  build/liblarch.a /usr/lib/libz.so

build/four_site_census \
  build/wric-1614-experiments/reduced-provider.pb.gz \
  i31=/tmp/chart-additive-case-fragments/i31.pb.gz \
  i82=/tmp/chart-additive-case-fragments/i82.pb.gz \
  i264=/tmp/chart-additive-case-fragments/i264.pb.gz \
  i36=/tmp/chart-additive-case-fragments/i36.pb.gz \
  i84=/tmp/chart-additive-case-fragments/i84.pb.gz \
  i33=/tmp/chart-additive-case-fragments/i33.pb.gz \
  i34=/tmp/chart-additive-case-fragments/i34.pb.gz \
  i86=/tmp/chart-additive-case-fragments/i86.pb.gz
```

The source remains under ignored experimental `build/` state; it is not
presented as a maintained production interface. The durable, normalized
compatibility and provider tables are tracked beside this report, including
the tied choices and structural/label conclusions. The original complete
trace is retained at
`build/wric-1614-experiments/four-site-census.txt`.

## Collapsing ancestral labels: opportunity and limit

The labelled full artifact stores 45,084 histories, while its collapsed
grammar has 575,168 direct parses. The reduced artifact sharpens the
comparison: it has 210 labelled histories but 864 direct parses. Collapsing
compact-genome labels permits a parse to combine clade productions whose
original ancestral labels did not coexist in one stored history. Materializing
a parse and re-running Fitch then supplies globally consistent ancestral
labels. This is how the exact grammar search finds real score-1616 trees below
the labelled history DAG's score 1620.

The four-site Pareto parses are an explicit boundary case: none has a
label-compatible stored-history witness. At clade 1052, production 464's
stored edge expects a `TGTT` child label, while the selected clade-1051
witnesses have `TGTC`. At clade 1056, production 470's stored edge expects
clade-727 witness `TGTC`, while the pattern-1002-minimizing production 132
uses `TGCC`. The topology remains valid after labels are forgotten, but its
new ancestral labels must be recomputed globally.

The label-compatible four-site Pareto frontier pays two additional steps:
all five of its vectors sum to 52. Therefore:

- preserving stored label incidence is too restrictive to expose the whole
  grammar opportunity;
- ignoring labels does expose valid new topologies, including the 1616
  minima;
- independently materializing and relabelling an entire selected topology is
  the correctness gate; and
- a future partially uncollapsed or label-aware chart would need to propagate
  label-incidence state through the grammar. No such hybrid experiment is
  claimed here.

## Ablations

The source plus all 55 byte-distinct captured outcomes was used for
source-inclusive leave-one-out reconstruction. The full table is
[`WRIC-CHART-1614-OUTCOME-ABLATIONS.tsv`](WRIC-CHART-1614-OUTCOME-ABLATIONS.tsv).
Only outcomes 35 and 28 uniquely preserve the pattern-1002 bound, outcome 39
the pattern-1023 bound, and outcome 114 the pattern-312 bound. No individual
outcome uniquely preserves the pattern-304 bound because its relevant
production has alternative providers. Dropping outcomes 137, 101, 140, or 123
raises the best stored-history score from 1620 to 1621 without necessarily
raising the composite bound.

An exact production force/remove census was derived from the 575,168 scored
topologies without re-enumeration. For every alternative at each
multi-production clade, it records selection count, forced optimum, removed
optimum, and how many of the 12 global minima select it. The complete result is
[`WRIC-CHART-1614-PRODUCTION-ABLATIONS.tsv`](WRIC-CHART-1614-PRODUCTION-ABLATIONS.tsv).
These rows distinguish productions merely present in the grammar from those
required by every 1616 class, and identify alternatives that force one or more
extra steps. Thirteen production selections are shared by all 12 minima and
indispensable to score 1616: removing productions 132, 134, 290, 325, 348,
406, 425, 503, 506, 524, 527, 545, or 551 raises the exact optimum.

Loading 54 outcome protobufs in one `dagutil` invocation exposed an existing
loader/merge failure (signal 11). To avoid weakening the biological
leave-one-out definition, each ablation was reconstructed as a balanced tree
of disjoint intermediate unions and then merged with the source. The
source-inclusive results above are exact for the intended outcome set; the
loader failure is recorded as an implementation finding, not silently
discarded.

## UA-edge control

Repeating all 575,168 parses without the UA/reference edge produced the same
12 optimal topology classes. Every score and the lower bound decreased by
exactly one: \(G^\star=1615\), lower bound 1613. The UA edge is therefore a
topology-independent constant on this fixture and is not the source of the
two-step coupling gap.

## Reproduction

The supported full census command is:

```bash
build/bin/dagutil \
  --dag-pb /tmp/chart-additive-medium-final.pb.gz \
  --refseq data/seedtree/refseq.txt.gz \
  --force-no-vcf --validate \
  --wric-polytomy-mode allow \
  --chart-score-ua-edge \
  --chart-kary-census \
  --chart-kary-census-workers 8 \
  --chart-kary-census-sankoff-stride 10000 \
  --chart-kary-census-output-prefix \
    build/wric-1614-experiments/full-census-w8-min
```

Use one worker to obtain the deterministic serial control. Worker count may
change recomputation counters because each partition starts with a cold cache;
it must not change counts, scores, ordinals, or digests.

## Verification gates

The focused final-source test commands are:

```bash
ctest --test-dir build \
  -R '^(chart_bnb_trim_apply_test|multifurcation_chart_oracle_test)$' \
  --output-on-failure

ctest --test-dir build-asan \
  -R '^(chart_bnb_trim_apply_test|multifurcation_chart_oracle_test)$' \
  --output-on-failure
```

Both suites pass. The arbitrary-arity regression covers arities 3, 4, 5, and
10 through direct scoring, materialization, generalized Fitch scoring,
protobuf reload, grammar rebuild, and both UA objectives. The streaming
regression has a mixed binary/ternary grammar with four exact parses and known
scores `{6,4,5,3}`; it checks the exact histogram and minimum, full Sankoff
parity, production-conditional histograms, early/empty ranges, and identical
coverage under 1, 2, 3, and 7 worker partitions. The ASan configuration
supplies the proportionate memory/concurrency check for those same paths.

The final executable was also rerun over the full grammar with eight workers
and Sankoff stride 10,000. It again reconciled 575,168 audit, enumerator, and
scored parses; performed 58 independent Sankoff checks; and reported
`optimum=1616`, 12 optimal parses, and 12 distinct minimum classes.

The evidence tables have additional mechanical reconciliation gates:

- reduced and full histogram columns sum to 864 and 575,168;
- the outcome deletion table has 55 rows;
- the production table has 79 rows, and at each clade its alternative
  selection counts plus its unreachable count sum to 575,168;
- the minimum table has 12 rows, and every retained protobuf hash matches its
  indexed artifact; and
- `git diff --check` reports no whitespace errors.

## Conclusion

For the frozen full collapsed grammar, the exact category is:

> **The 1614 lower bound is unattainable in the direct arbitrary-arity
> grammar. The exact common-topology optimum is 1616, attained by 12 topology
> classes.**

The chart's main additional opportunity over the labelled fragment union is
real: collapsing ancestral labels expands 45,084 stored histories to a
575,168-parse topology grammar and exposes validated score-1616 trees below
the stored-history minimum of 1620. The residual two steps are a genuine
cross-pattern topology conflict, not a mode-selection problem, a binary
refinement artifact, an incomplete search, or the UA edge.
