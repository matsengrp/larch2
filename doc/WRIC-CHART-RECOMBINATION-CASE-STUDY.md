# Frozen-medium move-recombination case study

## Status and intended use

This note records the evidence collected on 2026-07-28 for a possible case
study in `higher-rank-larch2-tex/main.tex`. It is an evidence record, not yet
paper prose. In particular, it separates:

1. recombination performed by larch's compact-genome-labelled history-DAG
   merger;
2. topology-certificate validation and final exact verification performed by
   the current chart-SPR additive mode; and
3. the larger recombination space exposed after collapsing the history DAG to
   the paper's clade grammar.

The experiments used repository commit
`6feaba609223cb7457a1ba77a23b66aa5ecec413` (`Usability fixes`) and the
existing RelWithDebInfo build. The relevant executable hashes were:

| Executable | SHA-256 |
|---|---|
| `build/bin/dagutil` | `c8d00fb6d21d962d064ca22f423fc41751af48d90911070621e8d777379798e6` |
| `build/bin/larch2` | `470205ae58f61a3335dafb9b28ece9c204a12571bbbb3f72b6ebb22f8d7d4b2f` |

## Headline result and terminology correction

The frozen-medium result is a clean example of **compatible local SPR
outcomes forming a better global tree through history-DAG union**:

- the source tree scores 1642;
- the best complete one-SPR fragment scores 1632;
- five compatible fragment outcomes, with individual improvements
  `4, 2, 1, 10, 5`, form a 32-history Cartesian product;
- the history containing all five choices scores
  `1642 - (4 + 2 + 1 + 10 + 5) = 1620`; and
- five fragments are cardinality-minimal for reaching 1620 among the captured
  fragments.

This is not, however, evidence that the current chart implementation combines
these moves more powerfully than the native fragment loop. In the
one-iteration, seed-1 comparison on this pinned fixture and toolchain, the
corrected k-ary chart-fronted additive mode and `larch2`'s native
sample--SPR--merge loop produce outputs with byte-identical canonical
collapsed-grammar representations:

| Property | Chart-fronted additive mode | Corrected native loop |
|---|---:|---:|
| Nodes / edges | 1166 / 1415 | 1166 / 1415 |
| Stored history count | 45,084 | 45,084 |
| Minimum score and count | 1620, count 4 | 1620, count 4 |
| Maximum score and count | 1642, count 2 | 1642, count 2 |
| Collapsed semantic SHA-256 | `b39fbf5d36614de8bc0a931cd21144f7c6c78208e9207d2487da0ee1020a72cd` | same |

The complete 23-bin score distributions are also identical. They are retained
in
[`WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv`](WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv),
where each column sums to all 45,084 labelled histories.

The two protobuf byte hashes differ, but the canonical JSON representations
are byte-identical: their canonical clades, productions, score, and semantic
digest are the same. The protobuf difference is therefore non-semantic under
this canonicalization.
The defensible attribution is therefore:

> Chart-projected SPR fragments with complete after-topology certificates
> expose compatible local improvements whose history-DAG union factorizes
> into independent switches, allowing one represented history to realize
> several moves simultaneously. The chart path supplies the arbitrary-arity
> topology certificates and exact-verifies the final witness; the shared
> history-DAG merger performs the observed recombination.

The paper's collapsed chart grammar has a genuinely larger combinatorial
closure, quantified below, but the current 1620 witness does not require that
extra closure.

## Frozen fixture

| Fixture | SHA-256 |
|---|---|
| `data/seedtree/seedtree.pb.gz` | `2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153` |
| `data/seedtree/refseq.txt.gz` | `088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c` |

The loaded source has 597 leaves, 1038 nodes, 1037 edges, and one history. Its
independently computed parsimony score is 1642. Its direct, unrefined grammar
has maximum arity 10, with 105 non-binary productions.

The chart-fronted capture used the maintained usability configuration:

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
  -o /tmp/chart-additive-medium-final.pb.gz
```

The run enumerated 437 move occurrences and retained and projected 290:

- 40 at radius 2;
- 50 at each of radii 4, 8, 16, 32, and 64;
- 220 projected moves whose source parent was multifurcating; and
- maximum source-parent arity 10.

The 290 retained occurrences reduce to 66 distinct
`(source, destination, LCA, predicted delta)` records and 55 byte-distinct
materialized fragment DAGs. Independently scoring every retained fragment gave
this distribution:

| Exact fragment score | Occurrences |
|---:|---:|
| 1632 | 5 |
| 1633 | 16 |
| 1637 | 23 |
| 1638 | 60 |
| 1639 | 56 |
| 1640 | 89 |
| 1641 | 41 |

Thus every current retained fragment is individually improving, but the best
still remains 12 steps above the union's 1620 result.

## What is being merged

Despite the word "fragment", every retained candidate is materialized as a
complete post-SPR tree from its chart-projected, structurally complete
after-topology certificate
(`src/chart_spr_search.cpp`, around the
`materialize_rank3_tree_from_topology` call). The source DAG, sampled source
tree, and all complete one-move trees are then passed to the ordinary
history-DAG `merge`.

The merger coalesces an internal node when its descendant leaf-set partition
and compact genome agree. It then unions and deduplicates the incident
parent--child edges. At a merged node, history-DAG dynamic programming:

- minimizes over edge alternatives within each child-clade group; and
- sums independently selected contributions between child-clade groups.

Tree counting has the corresponding sum/product form: sum the counts of edge
alternatives within a child clade, then multiply across child clades. Therefore
recursively compatible local alternatives contributed by different complete
input trees can be selected together in a represented output history. The
choices must still form a well-founded history through matching child-clade
incidence; they are not an arbitrary Cartesian product of every edge. No input
fragment needs to contain a compatible global combination.

This mechanism is visible in:

- `include/larch/node_label.hpp`, which defines node identity;
- `include/larch/merge.hpp`, which deduplicates nodes and edges; and
- `include/larch/subtree_weight.hpp`, which implements the per-clade
  min/sum recurrence and the sum/product history count.

## A cardinality-minimum five-fragment witness

One sufficient set can be represented by captured fragment indices
`11, 22, 38, 40, 45`:

| Fragment | First retained radius | Move `(src, dst, LCA)` | Predicted delta | Exact fragment score | Source-parent arity |
|---:|---:|---|---:|---:|---:|
| 11 | 2 | `(118, 119, 2)` | -4 | 1638 | 6 |
| 22 | 2 | `(516, 515, 489)` | -2 | 1640 | 10 |
| 38 | 2 | `(144, 146, 142)` | -1 | 1641 | 2 |
| 40 | 4 | `(473, 482, 467)` | -10 | 1632 | 6 |
| 45 | 4 | `(222, 986, 197)` | -5 | 1637 | 2 |

Indices 5/11, 21/22, and 37/38 are opposite move descriptors that serialize
to the same respective complete fragment DAG. The raw SHA-256 values for the
five representative protobufs are:

| Fragment | SHA-256 |
|---:|---|
| 11 | `cb9de0e3b167a2f44af51730de39788845b5a6012e8fe5662d1b4c47d324cdac` |
| 22 | `dc167a7c276af17965308692f3ee65db71958388041c9d0e9129ef52a79379c3` |
| 38 | `1108c8dd1d37e87bbd0305f63013cf31adf729d774cdb4226c7337ebb38910cc` |
| 40 | `d53992d9bcecd0afd706d1a5d9deabae6c2d12034ceab8f0ec96f176c0ec0bf0` |
| 45 | `5902efd2f51f14d3e7e1225b0082a4d7a7fb26c7fc8a20c29bc4e077bac759ea` |

Fragments 11, 22, and 40 show that the result is not a binary-only case:
their source parents have arities 6, 10, and 6. The resulting 1620 witness
itself has maximum arity 9 and contains 105 multifurcating productions:
72 of arity 3, 24 of arity 4, six of arity 5, two of arity 6, and one of
arity 9.

### Exhaustive factorial check

All 32 subsets of these five byte-distinct fragment outcomes were merged with
the source and independently scored. For every subset \(S\), with no
exceptions:

\[
  \operatorname{trees}(S)=2^{|S|}
\]

and

\[
  \operatorname{minscore}(S)
    = 1642 + \sum_{i \in S}\Delta_i.
\]

The full five-fragment union consequently contains exactly 32 histories and a
unique score-1620 history. Its full score distribution is the subset-sum
distribution of improvements `{1, 2, 4, 5, 10}`:

```text
{1620:1, 1621:1, 1622:1, 1623:1, 1624:1,
 1625:2, 1626:2, 1627:2, 1628:1, 1629:1,
 1630:2, 1631:2, 1632:2, 1633:1, 1634:1,
 1635:2, 1636:2, 1637:2, 1638:1, 1639:1,
 1640:1, 1641:1, 1642:1}
```

The complete subset table is preserved in
`doc/WRIC-CHART-RECOMBINATION-FIVE-SUBSETS.tsv`.

The five-fragment union has 1056 nodes and 1094 edges. Its protobuf SHA-256 is
`b0a35cfb857433c22cf8170b7f305694e389c7eaf5bb266effd241d4e6dd9d4c`;
its canonical semantic SHA-256 is
`b3fef4d2dced4c21399e856df10b7226a4d6bcc6eaada5c7748992b150c92b7e`.

### Production-block attribution

Relative to the source topology, this five-fragment witness adds 14 collapsed
clade-grammar productions and removes 11. The 14 additions partition across
the five fragment outcomes as `2 + 2 + 2 + 4 + 4`. Every added local
production occurs intact in at least one input fragment. No input fragment
contains all 14, and the global five-block combination occurs in none of the
complete input trees.

Equivalently, retaining compact-genome labels, the same witness contains 57
non-source edges and 23 non-source labelled production groups. This
label-sensitive view is the one relevant to the concrete history-DAG merger;
the 14-production view is the topology-only collapsed-grammar view relevant to
the paper. These counts must not be conflated.

Removing fragments 11, 22, 38, 40, or 45 from the five-fragment union raises
the minimum to 1624, 1622, 1621, 1630, or 1625, respectively. The displayed
set is therefore deletion-minimal.

### Proof that five is cardinality-minimum

The stronger cardinality claim was checked against the entire 55-fragment
union, not just the displayed five:

1. The full union has exactly four score-1620 histories.
2. Repeated minimum-weight sampling recovered all four distinct canonical
   semantic digests.
3. Each node was keyed by its sorted descendant-taxon partition plus compact
   genome, and each edge by its parent and child keys. The child descendant
   set implies the clade group within the parent partition.
4. Exact set cover over the non-source labelled edges found one unique
   minimum cover for every witness among the 55 byte-distinct fragment-outcome
   classes:

   | Witness semantic SHA-256 | Unique minimum cover |
   |---|---|
   | `29b1fb36c7cd7e6bf5a5a834c7102cfdba6377a60a3bb727c07f27ec6ecdc3f3` | `{5,21,37,40,94}` |
   | `95fad9958bbd73854c75bc3a0080ee8af82b157a0e7a5dd572b0a4978e150800` | `{5,21,37,40,97}` |
   | `c1a9f08330aca7c18786683df472be9e566b01e412517283ccad4bef4b61a2a3` | `{5,21,37,40,96}` |
   | `3ec38d22602111ec77ba8ad7624ccd7b9bde758195fe5ea2c0d02765de273f5c` | `{5,21,37,40,45}` |

   The minimum covers are not unique over all 290 occurrence IDs because
   several occurrence IDs materialize byte-identical fragment outcomes.
5. Every minimum cover has size five. For these complete full-taxon tree
   inputs, every non-UA witness edge comes from an input; the merger's general
   orphan-fragment connector path is not exercised for internal roots. Every
   history in a subset union is also in the full union. Because the four
   witnesses exhaust the full union's score-1620 histories, no subset of four
   captured fragments can reach 1620 under this stored history-DAG objective.

This proves cardinality minimality among the captured fragment DAGs. It does
not claim that five arbitrary SPR operations are necessary among all possible
search paths, or that another, uncaptured single SPR could not lead to a
different score-1620 tree.

### Seed scope

The exact command used seed 1. For this first iteration on this particular
fixture, the candidate union has a strong seed-independence argument: the
input contains exactly one topology, so minimum-tree sampling cannot choose a
different source, and enumeration, sorting, capping, projection, and merging
are deterministic for that source. The 1620 opportunity should consequently
remain present for any sampling seed on this exact first iteration.

The seed can still select a different one of the four tied 1620 witnesses for
verification or serialization. This argument also does not generalize to an
input DAG with multiple minimum source histories or to later iterations, where
the sampled source topology can change the bounded fragment union.

## Full-union growth trajectory

Accumulating the 66 tuple-distinct representative moves in capture order first
reaches:

| Minimum score | First representative step | Stored histories at that step |
|---:|---:|---:|
| 1633 | 1 | 2 |
| 1629 | 6 | 10 |
| 1627 | 22 | 180 |
| 1626 | 33 | 1056 |
| 1625 | 38 | 3344 |
| 1624 | 41 | 3840 |
| 1620 | 44 | 5600 |

Adding the remaining redundant or alternative outcomes leaves the minimum at
1620 while expanding the output to 45,084 stored histories. This distinguishes
the fragment that first enables the best compatible block combination from
later fragments that enlarge diversity without lowering the minimum.

## Historical pre-WRIC comparison

The frozen historical reference is the one-iteration seed-1 native output
created by the executable frozen from commit `408434e`:

`build/wric-chart-parallelization/phase5-medium-exact1-w8/outputs/seedtree.pb.gz_sample_explore_merge_trial1.pb.gz`

| Property | Historical pre-WRIC | Corrected current union |
|---|---:|---:|
| Nodes / edges | 1164 / 1430 | 1166 / 1415 |
| Stored histories | 38,080 | 45,084 |
| Minimum score and count | 1627, count 28 | 1620, count 4 |
| Maximum score and count | 1661, count 56 | 1642, count 2 |
| Collapsed clades / productions | 1093 / 548 | 1095 / 554 |
| Semantic SHA-256 | `82f38c963833fbfc06fca8260f4f02df584a864523cb5262c2a8d80ae2fb84af` | `b39fbf5d36614de8bc0a931cd21144f7c6c78208e9207d2487da0ee1020a72cd` |

The old capture's stored individual fragment scores, 1643--1658, are not a
valid topology-level comparison. They were computed with the old binary
shortcut for k-ary Fitch assignment. Re-Fitching the 53 byte-distinct
historical fragment topologies with the corrected recurrence gives:

```text
1632:1, 1633:3, 1637:5, 1639:7, 1640:4,
1641:13, 1642:11, 1643:3, 1645:3, 1646:3
```

Merging those corrected, re-Fitched historical topologies produces 6720
stored histories with minimum 1626. Its topology-only collapsed grammar is
unchanged from the historical output—1093 clades, 548 productions, and an
unsaturated parse-count estimate of 43,792—but its compact-genome-labelled
history closure changes substantially.

The comparison supports this causal account:

- the corrected generalized Fitch recurrence changes ancestral labels and
  therefore which history-DAG nodes coalesce;
- it also changes move deltas and the move set retained under the per-radius
  cap;
- corrected labels alone improve the old topology set from 1627 to 1626; and
- in these two captured unions, the corrected historical topology family
  reaches 1626, whereas the current corrected fragment family reaches 1620.

It does not support attributing the seven-point historical gap to chart
recombination: the corrected native loop obtains the same score, history
count, extrema distribution, output size, and canonical collapsed-grammar
representation. Nor does this comparison prove that the current family is
necessary among every possible SPR move or search path.

## History-DAG closure versus collapsed chart-grammar closure

The paper's collapsed grammar intentionally ignores compact-genome labels when
identifying clades and productions. It can therefore combine compatible local
production choices even where labelled history nodes did not coalesce. The
frozen data quantify the distinction:

| DAG | Labelled stored histories | Unsaturated grammar parse-count estimate | Raw parse-estimate / labelled-history-count ratio | Exact stored-history minimum | Per-site composite chart lower bound |
|---|---:|---:|---:|---:|---:|
| Source | 1 | 1 | 1.00 | 1642 | 1642 |
| Historical pre-WRIC | 38,080 | 43,792 | 1.15 | 1627 | 1620 |
| Corrected current/chart | 45,084 | 575,168 | 12.76 | 1620 | 1614 |

The corrected union's grammar parse-count estimate is 13.13 times the
historical grammar estimate, while its labelled history count is only 1.18
times larger. These raw ratios compare different objects: multiple labelled
histories can collapse to one topology, and a complete grammar parse need not
be a source-history witness. They are not a bijective expansion factor.
Nevertheless, they are count evidence that the induced collapsed grammar has
a substantially larger topology-parse closure than the labelled history DAG
represents. Every complete parse remains constrained by parent/child clade
incidence.

The score 1614 remains a lower bound rather than the score of a common
topology. A subsequent exhaustive direct-k-ary census has now closed the old
interval: all 575,168 direct grammar parses were scored, and the exact common
topology optimum is 1616, attained by 12 topology classes. Every class was
independently Sankoff-scored, materialized without binary refinement,
generalized-Fitch-scored, serialized, reloaded, validated, and rescored at
1616. All 12 pay one step at pattern 304/site 7716 and one at pattern
1023/site 27563; they attain the other 1,108 pattern minima. The complete
evidence is in
`doc/WRIC-CHART-1614-EXACT-CENSUS.md`.

This gives the paper two distinct, supportable observations:

1. **Demonstrated concrete synthesis:** history-DAG union combines five
   chart-projected, topology-certified one-SPR outcomes into an explicit
   arbitrary-arity 1620 history.
2. **Demonstrated representational opportunity:** collapsing away ancestral
   labels induces a grammar with 575,168 exhaustively enumerated complete
   parses from a DAG holding 45,084 labelled histories. Twelve newly combined
   topology classes score 1616 after global relabelling, four steps below the
   best stored labelled history. Per-site chart minimization reaches the
   stricter bound 1614, but exact enumeration proves that no common topology
   attains it.

## Relation to the paper

The findings instantiate several claims already made in
`higher-rank-larch2-tex/main.tex`:

- lines 285--305 define the collapsed grammar and state that a complete parse
  need not identify one source history after witnesses have been merged;
- lines 419--456 explain why the per-site composite score is a lower bound,
  not necessarily the score of one topology;
- lines 741--752 contrast one sampled run with chart aggregation over grammar
  runs; and
- lines 781--800 distinguish grammar-topology exactness, source-history
  preservation, fixed-topology verification, and arbitrary-arity limitations.

For a future paper case study, the five-switch factorial is the clean concrete
example. The 45,084-versus-575,168 comparison is the natural bridge to the
higher-rank chart, provided the text retains the multi-site lower-bound caveat.

## Reproduction and retained evidence

The accepted chart output and historical native output can be regenerated by
the maintained commands in `doc/WRIC-CHART-USABILITY.md` and
`build/wric-chart-parallelization/phase5-medium-exact1-w8/commands.sh`.

The individual fragment capture was diagnostic because the production mode
does not persist fragment provenance. GDB stopped immediately after the
parallel fragment materialization loop in `src/chart_spr_search.cpp`, saved
each `fragments[index]`, and printed the aligned retained move record. The
build-specific command file was:

```gdb
set pagination off
set confirm off
break /home/ogi-agent/matsen/larch2-wric/src/chart_spr_search.cpp:8937
commands
silent
set $buf=(char*)malloc(160)
set $sv=(std::string_view*)malloc(sizeof(std::string_view))
set $i=0
set $count=fragments._M_impl._M_finish-fragments._M_impl._M_start
while $i < $count
set $n=(int)snprintf($buf, 160, "/tmp/chart-additive-case-fragments/i%lu.pb.gz", $i)
set $sv->_M_len=$n
set $sv->_M_str=$buf
call larch::save_proto_dag(*(fragments._M_impl._M_start+$i), *$sv)
printf "FRAGMENT index=%zu src=%zu dst=%zu lca=%zu delta=%d\n", $i, (retained_moves._M_impl._M_start+$i)->src, (retained_moves._M_impl._M_start+$i)->dst, (retained_moves._M_impl._M_start+$i)->lca, (retained_moves._M_impl._M_start+$i)->score_change
set $i=$i+1
end
printf "SAVED count=%zu\n", $count
quit
end
run
```

This script depends on the pinned libstdc++ layout and source line and is
evidence-capture scaffolding, not a supported interface. Every fragment was
then independently reloaded and scored with:

```bash
build/bin/dagutil \
  --dag-pb FRAGMENT.pb.gz \
  --force-no-vcf --validate --dag-info --parsimony
```

Subset unions were built with the same `larch::merge` and
`subtree_weight<parsimony_score_ops>` APIs used by the program. The 32-subset
table has SHA-256
`ce8e6add9a952f53fde30f9f62cd5aeb93c34650ea02c53a0ea99314b2cc754f`
in its original capture form. The tracked copy contains the same data with a
documentation comment header.

Primary temporary artifacts from the collection session were:

| Artifact | Purpose | SHA-256 |
|---|---|---|
| `/tmp/chart-additive-medium-final.pb.gz` | authoritative chart-fronted output | `ae9c0c1b746439e3544eb2e8cef877c404e019d493d173a922717b1772a2b25b` |
| `/tmp/case-study-current-native-n1.pb.gz` | corrected native confounder run | `7b67a125dec0fa930ce92a96220b56655b63f1018c75babd395360a2400820b5` |
| `/tmp/chart-additive-case-five-union.pb.gz` | five-switch union | `b0a35cfb857433c22cf8170b7f305694e389c7eaf5bb266effd241d4e6dd9d4c` |
| `/tmp/chart-additive-case-five-witness.pb.gz` | one concrete 1620 witness | `58a26eba19b50aaa9c4333dcb09c31e85f84fd2ea9910d7af9ccf312540b81f8` |
| `/tmp/chart-additive-case-moves.tsv` | all 290 aligned retained moves | `1789f551d7b4eb1969f78c5eca6219e923b984d9bc8f0a862afc0ca0c7a29aec` |
| `/tmp/chart-additive-case-fragment-scores.tsv` | all 290 independent fragment scores | `cd8af156dbaccd076f22282427890c58951d35f983f0679ed62882c8a88ca72f` |
| `/tmp/chart-additive-case-five-factorial.tsv` | independent 32-subset factorial check | `023558b416d7f3bb996761252a66a3537006965123eef34fb1ccf3280e4db3b7` |
| `/tmp/chart-fragment-attribution-summary.tsv` | four-witness minimum-cover summary | `a7f43042bbb5d84cf305a7840b30e0d03d535069eaa66614b6094a795d82240a` |
| `/tmp/chart-additive-case-production-origins.out` | topology-only 14-production attribution | `1bdf5a193b28ecbef07cf2797a54fd835a6b4091f3ad27cb562ab023f34f405b` |

The permanent code currently does not retain input fragment IDs through
`merge`. A publication-grade rerun should either:

- record `fragment ID -> move certificate -> canonical edge/production set`
  before merging; or
- propagate compact origin bitsets through the merger.

That provenance addition would make the attribution a maintained diagnostic
rather than a debugger-assisted capture. It is not needed to validate the
current score or factorial result.

## Claims to avoid

The collected evidence does **not** support any of these statements:

- "The chart mode reaches 1620 while the fragment loop reaches only 1627."
  That compares corrected and pre-fix algorithms; corrected native also reaches
  1620.
- "No individual SPR fragment improves on 1642." That was an artifact of
  historically stored, incorrectly k-ary-assigned compact genomes.
- "The chart found a score-1614 tree." It found a composite per-site lower
  bound of 1614.
- "The five moves are universally necessary." They are cardinality-minimum
  within this captured one-iteration fragment family.
- "The result is binary." Three of the five displayed fragment outcomes touch
  source-parent arities 6, 10, and 6, and the witness retains productions up to
  arity 9.
