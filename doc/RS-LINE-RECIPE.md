# RS-LINE-RECIPE.md — the single-recipe loop card

The documented, complete recipe for running the
RS line (response-stratified fold construction)
with this branch's tools.  Every score of record
is free-root.  Campaign evidence:
`~/github/tric-explore/phylo/evidence/rs-bench/`.

## Build

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_COMPILER=/home/ogi/install/gcc-trunk/bin/g++-trunk
    cmake --build build -j8 --target larch larch2 dagutil \
          wric_tension_map wric_context_spr wric_subtree \
          wric_partition wric_pick_clade wric_binarize wric_dag_binarize

(All standalone tools are CMake-registered; the old
direct-g++ card is retired.)

## Stage 0 — intake and comb

    bin-dir/larch2 --tree-pb start.pb.gz --refseq ref.txt -o probe.pb.gz \
        -n 0 --validate
    wric_dag_binarize probe.pb.gz scaffold_bin.pb.gz     # at scale
    # (wric_binarize for small inputs; identical class)

## Stage A — the bounded context search round

For each locus (13–100 leaves, drawn uniformly,
seeded per round; 8 per round, 4 in parallel):

    wric_context_spr search <base.pb.gz> <locus.ids.txt> <outdir>/ \
        engine=inc spr_samples=10 spr_rounds=3 root=ref seed=<S>

Absorption: adopt `outdir/best_graft.pb.gz` iff
its trajectory best beats the search's own
restart-0 baseline (see the engine=inc warning
printed when root=ref is set).

## Stage B — merge, splice, probe, materialize

    dagutil --dag-pb base.pb.gz --dag-pb <grammar...> --dag-pb <grafts...> \
        --force-no-vcf -o union.pb.gz
    # splice unary nodes if present, then:
    bin-dir/larch2 --dag-pb union.pb.gz -o witness.pb.gz \
        --trim-mode chart-bnb -n 0 --chart-bnb-score-only \
        --chart-bnb-upper-bound $((incumbent + 1)) \
        --chart-bnb-class-compress on --chart-bnb-profile-dedup on \
        --chart-bnb-trim-application optimal-topology-materialize \
        --chart-bnb-max-exact-topologies 1

`exact_bnb_objective` is exact.  If below the
incumbent, the emitted witness is adopted (new
base, new incumbent; record its digest).  On a
no-improvement round, probe each single graft
alone (the sweep).  UB-censor retry: on an
unexpected empty-frontier exit, retry once at
witness + 50 to learn the grammar's own optimum;
disclose the retry.

## Typed stops

`neighborhood-terminal` (no round or sweep
improvement), `draw-terminal` (all grafts of a
round identical), `no-loci`, plus round caps and
stage censors.  Each stop is a first-class,
logged outcome.

## Stage C — partitioned family certificate

    wric_tension_map scaffold lb <global.bin.pb.gz> <scaffold.pb.gz> \
        <ids_prefix> <rows_prefix> <np> <out.profile> [expected] <mask>
    wric_tension_map scaffold wit <global.bin.pb.gz> <scaffold.pb.gz> \
        <ids_prefix> <wit_prefix> <np> - <expected_U>
    wric_tension_map megatree build <scaffold.pb.gz> <global.bin.pb.gz> \
        <ids_prefix> <np> <profile> mega.pb.gz
    wric_tension_map megatree expand mega.pb.gz <wit_prefix> <np> full.pb.gz
    wric_tension_map tension full.pb.gz full.pb.gz 0     # == U, rescore

The bracket [LB, U] is the deliverable; the
megatree identity rescore must equal U exactly.

## Determinism battery

Reruns from the same seeds must reproduce
incumbent trajectories and every adoption digest
bit-exactly; see
`phylo/tools/wric_cleanup_battery.py` in the
campaign repo for the four standing checks (WIT
digest, carrier chart, family anchors, family
bracket).
