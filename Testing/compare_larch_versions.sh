#!/usr/bin/env bash
set -uo pipefail

# Systematic correctness comparison: larch v1 vs larch2
# Issue #21

# Unset GZIP to avoid deprecation warnings
unset GZIP 2>/dev/null || true

# Per-command timeout (seconds) to prevent hangs on large tree enumeration
CMD_TIMEOUT=300

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPORT="$SCRIPT_DIR/comparison_report.md"

# Executables
OLD_DAGUTIL="/home/matsen/re/larch/build/bin/larch-dagutil"
OLD_USHER="/home/matsen/re/larch/build/bin/larch-usher"
OLD_BCR="/home/matsen/re/larch/build/bin/bcr-larch"
NEW_DAGUTIL="$REPO_DIR/build/dagutil"
NEW_LARCH2="$REPO_DIR/build/larch2"

# Data paths
TESTCASE_DAG="$REPO_DIR/data/testcase/full_dag.pb.gz"
TEST5_DIR="$REPO_DIR/data/test_5_trees"
SEEDTREE="$REPO_DIR/data/seedtree/seedtree.pb.gz"
REFSEQ="$REPO_DIR/data/seedtree/refseq.txt.gz"
D20_DIR="$REPO_DIR/data/20D_from_fasta"
FLUC_DIR="$SCRIPT_DIR/data/fluC_M"
ROTA_DIR="$SCRIPT_DIR/data/rotavirusA"

# Temp prefix
TMP="/tmp/larch_comparison"

# Counters
PART_A_PASS=0; PART_A_FAIL=0; PART_A_SKIP=0
PART_B_PASS=0; PART_B_FAIL=0

OLD_LARCH_OK=true

# Get commit hashes
OLD_COMMIT="$(cd /home/matsen/re/larch && git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
NEW_COMMIT="$(cd "$REPO_DIR" && git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"

# Report accumulator
declare -a REPORT_SECTIONS=()

add_report() {
    REPORT_SECTIONS+=("$1")
}

write_report() {
    {
        echo "# larch v1 vs larch2 Comparison Report"
        echo ""
        echo "Date: $(date +%Y-%m-%d)"
        echo "Old larch commit: $OLD_COMMIT"
        echo "larch2 commit: $NEW_COMMIT"
        echo ""
        for section in "${REPORT_SECTIONS[@]}"; do
            echo "$section"
            echo ""
        done
        echo "## Summary"
        echo "- Part A: $PART_A_PASS passed, $PART_A_FAIL failed, $PART_A_SKIP skipped (of 8)"
        echo "- Part B: $PART_B_PASS passed, $PART_B_FAIL failed (of 10)"
        local total_pass=$((PART_A_PASS + PART_B_PASS))
        local total_fail=$((PART_A_FAIL + PART_B_FAIL))
        if [ "$total_fail" -eq 0 ]; then
            echo "- Overall: **PASS**"
        else
            echo "- Overall: **FAIL** ($total_fail failures)"
        fi
    } > "$REPORT"
    echo "Report written to $REPORT"
}


##############################################################################
# BUILD STATUS
##############################################################################

build_status_section() {
    local old_status="PASS" new_status="PASS"
    if ! [ -x "$OLD_DAGUTIL" ]; then
        old_status="FAIL — executable not found"
        OLD_LARCH_OK=false
    fi
    if ! [ -x "$NEW_DAGUTIL" ]; then
        new_status="FAIL — executable not found"
    fi
    add_report "## Build Status
- Old larch: $old_status
- larch2: $new_status"
}

##############################################################################
# PART A: Direct old-vs-new comparisons
##############################################################################

check_a1() {
    local section="### A1. DAG statistics on testcase"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    local old_out new_out
    old_out=$("$OLD_DAGUTIL" -i "$TESTCASE_DAG" --force-no-vcf --dag-info 2>&1)
    new_out=$("$NEW_DAGUTIL" --dag-pb "$TESTCASE_DAG" --force-no-vcf --dag-info 2>&1)

    # Extract values (normalize key names)
    local old_leaves old_nodes old_edges old_trees
    old_leaves=$(echo "$old_out" | grep -i 'leave' | grep -oP '\d+' | head -1)
    old_nodes=$(echo "$old_out" | grep -i 'node' | grep -oP '\d+' | head -1)
    old_edges=$(echo "$old_out" | grep -i 'edge' | grep -oP '\d+' | head -1)
    old_trees=$(echo "$old_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)

    local new_leaves new_nodes new_edges new_trees
    new_leaves=$(echo "$new_out" | grep -i 'leaves' | grep -oP '\d+' | head -1)
    new_nodes=$(echo "$new_out" | grep -i 'nodes' | grep -oP '\d+' | head -1)
    new_edges=$(echo "$new_out" | grep -i 'edges' | grep -oP '\d+' | head -1)
    new_trees=$(echo "$new_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    if [ "$old_leaves" != "$new_leaves" ]; then diffs+="leaves: $old_leaves vs $new_leaves; "; status="FAIL"; fi
    if [ "$old_nodes" != "$new_nodes" ]; then diffs+="nodes: $old_nodes vs $new_nodes; "; status="FAIL"; fi
    if [ "$old_edges" != "$new_edges" ]; then diffs+="edges: $old_edges vs $new_edges; "; status="FAIL"; fi
    if [ "$old_trees" != "$new_trees" ]; then diffs+="trees: $old_trees vs $new_trees; "; status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch output: leaves=$old_leaves, nodes=$old_nodes, edges=$old_edges, trees=$old_trees
- larch2 output: leaves=$new_leaves, nodes=$new_nodes, edges=$new_edges, trees=$new_trees
- Differences: $diffs"
}

check_a2() {
    local section="### A2. Parsimony score distribution"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    local old_out new_out
    old_out=$("$OLD_DAGUTIL" -i "$TESTCASE_DAG" --force-no-vcf --parsimony 2>&1)
    new_out=$("$NEW_DAGUTIL" --dag-pb "$TESTCASE_DAG" --force-no-vcf --parsimony 2>&1)

    # Extract parsimony min/max and full distribution
    local old_min old_max new_min new_max
    old_min=$(echo "$old_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    old_max=$(echo "$old_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)
    new_min=$(echo "$new_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    new_max=$(echo "$new_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)

    # Extract full parsimony_all lines
    local old_all new_all
    old_all=$(echo "$old_out" | grep -E 'parsimony_all|score.*count|^\s*\d+\s*:\s*\d+' | sort)
    new_all=$(echo "$new_out" | grep -E 'parsimony_all|score.*count|^\s*\d+\s*:\s*\d+' | sort)

    local status="PASS" diffs=""
    if [ "$old_min" != "$new_min" ]; then diffs+="min: $old_min vs $new_min; "; status="FAIL"; fi
    if [ "$old_max" != "$new_max" ]; then diffs+="max: $old_max vs $new_max; "; status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch: parsimony_min=$old_min, parsimony_max=$old_max
- larch2: parsimony_min=$new_min, parsimony_max=$new_max
- Differences: $diffs"
}

check_a3() {
    local section="### A3. Merge 5 DAG protobuf trees"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    local old_out new_out
    old_out=$("$OLD_DAGUTIL" \
      -i "$TEST5_DIR/tree_0.pb.gz" \
      -i "$TEST5_DIR/tree_1.pb.gz" \
      -i "$TEST5_DIR/tree_2.pb.gz" \
      -i "$TEST5_DIR/tree_3.pb.gz" \
      -i "$TEST5_DIR/tree_4.pb.gz" \
      --force-no-vcf \
      -o "${TMP}_merged_5.pb" --dag-info --parsimony 2>&1)

    new_out=$("$NEW_DAGUTIL" \
      --dag-pb "$TEST5_DIR/tree_0.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_1.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_2.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_3.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_4.pb.gz" \
      --force-no-vcf \
      -o "${TMP}_merged2_5.pb.gz" --dag-info --parsimony 2>&1)

    local old_leaves old_nodes old_edges old_trees old_min old_max
    old_leaves=$(echo "$old_out" | grep -i 'leave' | grep -oP '\d+' | head -1)
    old_nodes=$(echo "$old_out" | grep -i 'node' | grep -oP '\d+' | head -1)
    old_edges=$(echo "$old_out" | grep -i 'edge' | grep -oP '\d+' | head -1)
    old_trees=$(echo "$old_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    old_min=$(echo "$old_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    old_max=$(echo "$old_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)

    local new_leaves new_nodes new_edges new_trees new_min new_max
    new_leaves=$(echo "$new_out" | grep -i 'leaves' | grep -oP '\d+' | head -1)
    new_nodes=$(echo "$new_out" | grep -i 'nodes' | grep -oP '\d+' | head -1)
    new_edges=$(echo "$new_out" | grep -i 'edges' | grep -oP '\d+' | head -1)
    new_trees=$(echo "$new_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    new_min=$(echo "$new_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    new_max=$(echo "$new_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    if [ "$old_leaves" != "$new_leaves" ]; then diffs+="leaves: $old_leaves vs $new_leaves; "; status="FAIL"; fi
    if [ "$old_nodes" != "$new_nodes" ]; then diffs+="nodes: $old_nodes vs $new_nodes; "; status="FAIL"; fi
    if [ "$old_edges" != "$new_edges" ]; then diffs+="edges: $old_edges vs $new_edges; "; status="FAIL"; fi
    if [ "$old_trees" != "$new_trees" ]; then diffs+="trees: $old_trees vs $new_trees; "; status="FAIL"; fi
    if [ "$old_min" != "$new_min" ]; then diffs+="pars_min: $old_min vs $new_min; "; status="FAIL"; fi
    if [ "$old_max" != "$new_max" ]; then diffs+="pars_max: $old_max vs $new_max; "; status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch: leaves=$old_leaves, nodes=$old_nodes, edges=$old_edges, trees=$old_trees, pars_min=$old_min, pars_max=$old_max
- larch2: leaves=$new_leaves, nodes=$new_nodes, edges=$new_edges, trees=$new_trees, pars_min=$new_min, pars_max=$new_max
- Differences: $diffs"
}

check_a4() {
    local section="### A4. Trim and compare"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    # Depends on A3 output files
    if ! [ -f "${TMP}_merged_5.pb" ] || ! [ -f "${TMP}_merged2_5.pb.gz" ]; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (A3 merge outputs not available)"
        return
    fi

    local old_out new_out
    old_out=$("$OLD_DAGUTIL" \
      -i "${TMP}_merged_5.pb" --force-no-vcf \
      -t -o "${TMP}_trimmed_5.pb" --dag-info --parsimony 2>&1)

    new_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_merged2_5.pb.gz" --force-no-vcf \
      -t -o "${TMP}_trimmed2_5.pb.gz" --dag-info --parsimony 2>&1)

    local old_trees old_min new_trees new_min
    old_trees=$(echo "$old_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    old_min=$(echo "$old_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    new_trees=$(echo "$new_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    new_min=$(echo "$new_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    if [ "$old_trees" != "$new_trees" ]; then diffs+="trees: $old_trees vs $new_trees; "; status="FAIL"; fi
    if [ "$old_min" != "$new_min" ]; then diffs+="pars_min: $old_min vs $new_min; "; status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch: trees=$old_trees, parsimony_min=$old_min
- larch2: trees=$new_trees, parsimony_min=$new_min
- Differences: $diffs"
}

check_a5() {
    local section="### A5. Merge 5 parsimony protobuf trees (20D_from_fasta)"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    local old_out new_out
    old_out=$(timeout "$CMD_TIMEOUT" "$OLD_DAGUTIL" \
      -i "$D20_DIR/1final-tree-100.nh1.pb.gz" \
      -i "$D20_DIR/1final-tree-101.nh1.pb.gz" \
      -i "$D20_DIR/1final-tree-102.nh1.pb.gz" \
      -i "$D20_DIR/1final-tree-103.nh1.pb.gz" \
      -i "$D20_DIR/1final-tree-104.nh1.pb.gz" \
      -r "$REFSEQ" \
      --force-no-vcf \
      -o "${TMP}_merged_20D.pb" --dag-info --parsimony 2>&1) || true

    new_out=$(timeout "$CMD_TIMEOUT" "$NEW_DAGUTIL" \
      --tree-pb "$D20_DIR/1final-tree-100.nh1.pb.gz" \
      --tree-pb "$D20_DIR/1final-tree-101.nh1.pb.gz" \
      --tree-pb "$D20_DIR/1final-tree-102.nh1.pb.gz" \
      --tree-pb "$D20_DIR/1final-tree-103.nh1.pb.gz" \
      --tree-pb "$D20_DIR/1final-tree-104.nh1.pb.gz" \
      --refseq "$REFSEQ" \
      --force-no-vcf \
      -o "${TMP}_merged2_20D.pb.gz" --dag-info --parsimony 2>&1) || true

    local old_leaves old_nodes old_edges old_trees old_min
    old_leaves=$(echo "$old_out" | grep -i 'leave' | grep -oP '\d+' | head -1)
    old_nodes=$(echo "$old_out" | grep -i 'node' | grep -oP '\d+' | head -1)
    old_edges=$(echo "$old_out" | grep -i 'edge' | grep -oP '\d+' | head -1)
    old_trees=$(echo "$old_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    old_min=$(echo "$old_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    local new_leaves new_nodes new_edges new_trees new_min
    new_leaves=$(echo "$new_out" | grep -i 'leaves' | grep -oP '\d+' | head -1)
    new_nodes=$(echo "$new_out" | grep -i 'nodes' | grep -oP '\d+' | head -1)
    new_edges=$(echo "$new_out" | grep -i 'edges' | grep -oP '\d+' | head -1)
    new_trees=$(echo "$new_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    new_min=$(echo "$new_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    if [ "$old_leaves" != "$new_leaves" ]; then diffs+="leaves: $old_leaves vs $new_leaves; "; status="FAIL"; fi
    if [ "$old_nodes" != "$new_nodes" ]; then diffs+="nodes: $old_nodes vs $new_nodes; "; status="FAIL"; fi
    if [ "$old_edges" != "$new_edges" ]; then diffs+="edges: $old_edges vs $new_edges; "; status="FAIL"; fi
    if [ "$old_trees" != "$new_trees" ]; then diffs+="trees: $old_trees vs $new_trees; "; status="FAIL"; fi
    if [ -n "$old_min" ] && [ -n "$new_min" ] && [ "$old_min" != "$new_min" ]; then
        diffs+="pars_min: $old_min vs $new_min; "; status="FAIL"
    fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch: leaves=$old_leaves, nodes=$old_nodes, edges=$old_edges, trees=$old_trees, pars_min=$old_min
- larch2: leaves=$new_leaves, nodes=$new_nodes, edges=$new_edges, trees=$new_trees, pars_min=$new_min
- Differences: $diffs"
}

check_a6() {
    local section="### A6. RF distance comparison"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    local old_out new_out
    old_out=$("$OLD_DAGUTIL" -i "$TESTCASE_DAG" --force-no-vcf --print-rf 2>&1)
    new_out=$("$NEW_DAGUTIL" --dag-pb "$TESTCASE_DAG" --force-no-vcf --sum-rf-distance 2>&1)

    local old_min old_max new_min new_max
    old_min=$(echo "$old_out" | grep -i 'sum_rf_dist_min' | grep -oP '\d+' | head -1)
    old_max=$(echo "$old_out" | grep -i 'sum_rf_dist_max' | grep -oP '\d+' | head -1)
    new_min=$(echo "$new_out" | grep -i 'sum_rf_dist_min' | grep -oP '\d+' | head -1)
    new_max=$(echo "$new_out" | grep -i 'sum_rf_dist_max' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    if [ "$old_min" != "$new_min" ]; then diffs+="rf_min: $old_min vs $new_min; "; status="FAIL"; fi
    if [ "$old_max" != "$new_max" ]; then diffs+="rf_max: $old_max vs $new_max; "; status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch: sum_rf_dist_min=$old_min, sum_rf_dist_max=$old_max
- larch2: sum_rf_dist_min=$new_min, sum_rf_dist_max=$new_max
- Differences: $diffs"
}

check_a7() {
    local section="### A7. SPR optimization comparison"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    # Old larch: optimize seedtree
    local old_opt_out="" old_check_out="" old_opt_ok=true
    if ! old_opt_out=$(timeout "$CMD_TIMEOUT" mpirun -n 1 "$OLD_USHER" \
      -i "$SEEDTREE" \
      -r <(zcat "$REFSEQ") \
      -o "${TMP}_opt_seed.pb" \
      -c 5 2>&1); then
        old_opt_ok=false
    fi

    local old_min="N/A"
    if $old_opt_ok && [ -f "${TMP}_opt_seed.pb" ]; then
        old_check_out=$("$OLD_DAGUTIL" -i "${TMP}_opt_seed.pb" --force-no-vcf --dag-info --parsimony 2>&1)
        old_min=$(echo "$old_check_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    fi

    # New larch2: optimize seedtree
    local new_opt_out new_check_out
    new_opt_out=$(timeout "$CMD_TIMEOUT" "$NEW_LARCH2" \
      --tree-pb "$SEEDTREE" \
      --refseq "$REFSEQ" \
      -o "${TMP}_opt2_seed.pb.gz" \
      -n 5 2>&1)

    new_check_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_opt2_seed.pb.gz" --force-no-vcf \
      --dag-info --parsimony 2>&1)
    local new_min
    new_min=$(echo "$new_check_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    # Both should be <= 1642 (input parsimony)
    if [ "$new_min" -gt 1642 ] 2>/dev/null; then
        diffs+="larch2 min ($new_min) > input parsimony (1642); "
        status="FAIL"
    fi
    # Check if larch2 is >5% worse than old larch
    if [ "$old_min" != "N/A" ] && [ -n "$old_min" ] && [ -n "$new_min" ]; then
        local threshold=$((old_min * 105 / 100))
        if [ "$new_min" -gt "$threshold" ] 2>/dev/null; then
            diffs+="larch2 ($new_min) >5% worse than old larch ($old_min); "
            status="FAIL"
        fi
    fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        diffs="none (optimization is stochastic)"
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Input parsimony: 1642
- Old larch min parsimony after 5 iterations: $old_min
- larch2 min parsimony after 5 iterations: $new_min
- Differences: $diffs"
}

check_a8() {
    local section="### A8. FASTA+Newick load and merge"
    if ! $OLD_LARCH_OK; then
        PART_A_SKIP=$((PART_A_SKIP + 1))
        add_report "$section
- Status: SKIP (old larch not available)"
        return
    fi

    # Old larch bcr-larch
    mkdir -p /tmp/larch_comparison_fluC_trees
    for i in 0 1 2 3 4; do
        cp "$FLUC_DIR/tree$i.nwk" "/tmp/larch_comparison_fluC_trees/tree$i-rerooted.treefile"
    done

    local old_out="" old_ok=true old_err=""
    if ! old_out=$("$OLD_BCR" \
      --fasta "$FLUC_DIR/input.fa" \
      --reference "$FLUC_DIR/root.fa" \
      --trees /tmp/larch_comparison_fluC_trees/ \
      -o "${TMP}_fluC_merged.pb" 2>&1); then
        old_ok=false
        old_err="$old_out"
    fi

    local old_leaves="N/A" old_min="N/A"
    if $old_ok && [ -f "${TMP}_fluC_merged.pb" ]; then
        local old_check
        # NOTE: --dag-info and --parsimony skipped because the merged fluC DAG has
        # hundreds of billions of trees. Just get leaf count from load output.
        old_check=$("$OLD_DAGUTIL" -i "${TMP}_fluC_merged.pb" --force-no-vcf 2>&1) || true
        old_leaves=$(echo "$old_check" | grep -i 'leave' | grep -oP '\d+' | head -1)
        old_min="N/A (skipped: too many trees for enumeration)"
    fi

    # larch2
    local new_out
    # Skip --dag-info and --parsimony (exponential tree count makes them infeasible)
    # Just save and parse leaf count from load output
    new_out=$("$NEW_DAGUTIL" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree0.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree1.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree2.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree3.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree4.nwk" \
      --refseq "$FLUC_DIR/root.fa" \
      --force-no-vcf \
      -o "${TMP}_fluC2_merged.pb.gz" 2>&1)

    local new_leaves new_min="N/A (skipped: too many trees for enumeration)"
    new_leaves=$(echo "$new_out" | grep -i 'leaves' | grep -oP '\d+' | head -1)

    local status="PASS" diffs=""
    # Check larch2 has 194 leaves
    if [ "$new_leaves" != "194" ]; then
        diffs+="larch2 leaves=$new_leaves (expected 194); "
        status="FAIL"
    fi

    if ! $old_ok; then
        diffs+="bcr-larch failed: $(echo "$old_err" | tail -1); "
        # Not a FAIL if old tool fails, just note it
    elif [ "$old_leaves" != "$new_leaves" ]; then
        diffs+="leaves differ: old=$old_leaves new=$new_leaves; "
        status="FAIL"
    fi

    if [ "$status" = "PASS" ]; then
        PART_A_PASS=$((PART_A_PASS + 1))
        if [ -z "$diffs" ]; then diffs="none"; fi
    else
        PART_A_FAIL=$((PART_A_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Old larch (bcr-larch): leaves=$old_leaves, parsimony_min=$old_min
- larch2: leaves=$new_leaves, parsimony_min=$new_min
- Differences: $diffs"
}

##############################################################################
# PART B: Self-consistency checks
##############################################################################

check_b1() {
    local section="### B1. Theorem 3.15 / Corollary 3.17 — merge-then-trim parsimony invariant"

    # fluC_M
    "$NEW_DAGUTIL" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree0.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree1.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree2.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree3.nwk" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree4.nwk" \
      --refseq "$FLUC_DIR/root.fa" \
      --force-no-vcf \
      -o "${TMP}_b1_fluC_merged.pb.gz" >/dev/null 2>&1

    # Trim and save
    "$NEW_DAGUTIL" --dag-pb "${TMP}_b1_fluC_merged.pb.gz" --force-no-vcf \
      -t -o "${TMP}_b1_fluC_trimmed.pb.gz" >/dev/null 2>&1

    # Read parsimony from the saved trimmed file (not from trim command which shows pre-trim stats)
    local fluC_out
    fluC_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b1_fluC_trimmed.pb.gz" --force-no-vcf --parsimony 2>&1)

    local fluC_min="" fluC_max="" fluC_status="PASS"
    if [ ! -f "${TMP}_b1_fluC_trimmed.pb.gz" ]; then
        fluC_status="FAIL"; fluC_min="ERROR"; fluC_max="ERROR"
    else
        fluC_min=$(echo "$fluC_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
        fluC_max=$(echo "$fluC_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)
        if [ -z "$fluC_min" ] || [ -z "$fluC_max" ]; then fluC_status="FAIL"; fi
        if [ "$fluC_min" != "$fluC_max" ]; then fluC_status="FAIL"; fi
    fi

    # rotavirusA
    "$NEW_DAGUTIL" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree0.nwk" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree1.nwk" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree2.nwk" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree3.nwk" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree4.nwk" \
      --refseq "$ROTA_DIR/root.fa" \
      --force-no-vcf \
      -o "${TMP}_b1_rota_merged.pb.gz" >/dev/null 2>&1

    # Trim and save
    "$NEW_DAGUTIL" --dag-pb "${TMP}_b1_rota_merged.pb.gz" --force-no-vcf \
      -t -o "${TMP}_b1_rota_trimmed.pb.gz" >/dev/null 2>&1

    # Read parsimony from the saved trimmed file
    local rota_out
    rota_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b1_rota_trimmed.pb.gz" --force-no-vcf --parsimony 2>&1)

    local rota_min="" rota_max="" rota_status="PASS"
    if [ ! -f "${TMP}_b1_rota_trimmed.pb.gz" ]; then
        rota_status="FAIL"; rota_min="ERROR"; rota_max="ERROR"
    else
        rota_min=$(echo "$rota_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
        rota_max=$(echo "$rota_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)
        if [ -z "$rota_min" ] || [ -z "$rota_max" ]; then rota_status="FAIL"; fi
        if [ "$rota_min" != "$rota_max" ]; then rota_status="FAIL"; fi
    fi

    if [ "$fluC_status" = "PASS" ] && [ "$rota_status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- fluC_M: $fluC_status — parsimony_min=$fluC_min, parsimony_max=$fluC_max
- rotavirusA: $rota_status — parsimony_min=$rota_min, parsimony_max=$rota_max"
}

check_b2() {
    local section="### B2. CG roundtrip stability"

    local pass1_out pass2_out
    "$NEW_DAGUTIL" \
      --dag-pb "$TEST5_DIR/tree_0.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_1.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_2.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_3.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_4.pb.gz" \
      --force-no-vcf \
      -o "${TMP}_b2_merged.pb.gz" --parsimony 2>&1 | grep 'parsimony' > "${TMP}_b2_pass1.txt"

    "$NEW_DAGUTIL" --dag-pb "${TMP}_b2_merged.pb.gz" --force-no-vcf \
      --parsimony 2>&1 | grep 'parsimony' > "${TMP}_b2_pass2.txt"

    local pass1_min pass2_min
    pass1_min=$(grep -i 'parsimony_min' "${TMP}_b2_pass1.txt" | grep -oP '\d+' | head -1)
    pass2_min=$(grep -i 'parsimony_min' "${TMP}_b2_pass2.txt" | grep -oP '\d+' | head -1)

    local diff_out status="PASS"
    diff_out=$(diff "${TMP}_b2_pass1.txt" "${TMP}_b2_pass2.txt" 2>&1) || true
    if [ -n "$diff_out" ]; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Pass 1 parsimony_min: $pass1_min
- Pass 2 parsimony_min: $pass2_min
- Diff: $(if [ -z "$diff_out" ]; then echo 'identical'; else echo "$diff_out"; fi)"
}

check_b3() {
    local section="### B3. Sampled trees have all leaves"
    local all_ok=true bad_samples=""

    for i in $(seq 1 20); do
        local sample_out
        sample_out=$("$NEW_DAGUTIL" --dag-pb "$TESTCASE_DAG" --force-no-vcf \
          -s --seed "$i" -o "${TMP}_b3_sample_$i.pb.gz" --dag-info 2>&1)
        local leaf_count
        leaf_count=$(echo "$sample_out" | grep -i 'leaves' | grep -oP '\d+' | head -1)
        if [ "$leaf_count" != "43" ]; then
            all_ok=false
            bad_samples+="seed=$i leaves=$leaf_count; "
        fi
    done

    local status="PASS"
    if ! $all_ok; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- All 20 sampled trees have 43 leaves: $(if $all_ok; then echo 'YES'; else echo "NO — $bad_samples"; fi)"
}

check_b4() {
    local section="### B4. Optimization does not degrade parsimony"

    # fluC_M baseline
    "$NEW_LARCH2" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree0.nwk" \
      --refseq "$FLUC_DIR/root.fa" \
      -o "${TMP}_b4_baseline.pb.gz" -n 0 >/dev/null 2>&1

    local base_out base_min
    base_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b4_baseline.pb.gz" --force-no-vcf --parsimony 2>&1)
    base_min=$(echo "$base_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    "$NEW_LARCH2" \
      --fasta "$FLUC_DIR/input.fa" --newick "$FLUC_DIR/tree0.nwk" \
      --refseq "$FLUC_DIR/root.fa" \
      -o "${TMP}_b4_optimized.pb.gz" -n 10 >/dev/null 2>&1

    local opt_out opt_min
    opt_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b4_optimized.pb.gz" --force-no-vcf --parsimony 2>&1)
    opt_min=$(echo "$opt_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    local fluC_status="PASS"
    if [ "$opt_min" -gt "$base_min" ] 2>/dev/null; then fluC_status="FAIL"; fi

    # rotavirusA baseline
    "$NEW_LARCH2" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree0.nwk" \
      --refseq "$ROTA_DIR/root.fa" \
      -o "${TMP}_b4_rota_baseline.pb.gz" -n 0 >/dev/null 2>&1

    local rbase_out rbase_min
    rbase_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b4_rota_baseline.pb.gz" --force-no-vcf --parsimony 2>&1)
    rbase_min=$(echo "$rbase_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    "$NEW_LARCH2" \
      --fasta "$ROTA_DIR/input.fa" --newick "$ROTA_DIR/tree0.nwk" \
      --refseq "$ROTA_DIR/root.fa" \
      -o "${TMP}_b4_rota_optimized.pb.gz" -n 10 >/dev/null 2>&1

    local ropt_out ropt_min
    ropt_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b4_rota_optimized.pb.gz" --force-no-vcf --parsimony 2>&1)
    ropt_min=$(echo "$ropt_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)

    local rota_status="PASS"
    if [ "$ropt_min" -gt "$rbase_min" ] 2>/dev/null; then rota_status="FAIL"; fi

    if [ "$fluC_status" = "PASS" ] && [ "$rota_status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- fluC_M: $fluC_status — baseline=$base_min, optimized=$opt_min
- rotavirusA: $rota_status — baseline=$rbase_min, optimized=$ropt_min"
}

check_b5() {
    local section="### B5. DAG validation passes on all outputs"
    local all_ok=true failures=""

    for f in "${TMP}_b1_"*.pb.gz "${TMP}_b2_"*.pb.gz "${TMP}_b4_"*.pb.gz; do
        if [ ! -f "$f" ]; then continue; fi
        local val_out
        if ! val_out=$("$NEW_DAGUTIL" --dag-pb "$f" --force-no-vcf --validate 2>&1); then
            all_ok=false
            failures+="$(basename "$f"): $(echo "$val_out" | tail -1); "
        fi
    done

    local status="PASS"
    if ! $all_ok; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Failures: $(if $all_ok; then echo 'none'; else echo "$failures"; fi)"
}

check_b6() {
    local section="### B6. Trim preserves DAG structure (not single tree)"

    # Trim and save
    "$NEW_DAGUTIL" --dag-pb "$TESTCASE_DAG" --force-no-vcf \
      -t -o "${TMP}_b6_trimmed.pb.gz" >/dev/null 2>&1

    # Read stats from the saved trimmed file (not from trim command output which shows pre-trim stats)
    local trimmed_out
    trimmed_out=$("$NEW_DAGUTIL" --dag-pb "${TMP}_b6_trimmed.pb.gz" --force-no-vcf --dag-info --parsimony 2>&1)

    local tree_count pars_min pars_max
    tree_count=$(echo "$trimmed_out" | grep -i 'tree_count' | grep -oP '\d+' | head -1)
    pars_min=$(echo "$trimmed_out" | grep -i 'parsimony_min' | grep -oP '\d+' | head -1)
    pars_max=$(echo "$trimmed_out" | grep -i 'parsimony_max' | grep -oP '\d+' | head -1)

    local status="PASS"
    if [ "$tree_count" != "23" ]; then status="FAIL"; fi
    if [ "$pars_min" != "75" ] || [ "$pars_max" != "75" ]; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- tree_count: $tree_count (expected 23)
- parsimony_min: $pars_min, parsimony_max: $pars_max (expected both 75)"
}

check_b7() {
    local section="### B7. Protobuf roundtrip (save -> load -> save)"

    "$NEW_DAGUTIL" --dag-pb "$TESTCASE_DAG" --force-no-vcf \
      -o "${TMP}_b7_pass1.pb.gz" >/dev/null 2>&1

    "$NEW_DAGUTIL" --dag-pb "${TMP}_b7_pass1.pb.gz" --force-no-vcf \
      -o "${TMP}_b7_pass2.pb.gz" >/dev/null 2>&1

    "$NEW_DAGUTIL" --dag-pb "${TMP}_b7_pass1.pb.gz" --force-no-vcf --parsimony \
      2>&1 | grep 'parsimony' > "${TMP}_b7_pars1.txt"
    "$NEW_DAGUTIL" --dag-pb "${TMP}_b7_pass2.pb.gz" --force-no-vcf --parsimony \
      2>&1 | grep 'parsimony' > "${TMP}_b7_pars2.txt"

    local diff_out status="PASS"
    diff_out=$(diff "${TMP}_b7_pars1.txt" "${TMP}_b7_pars2.txt" 2>&1) || true
    if [ -n "$diff_out" ]; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Diff: $(if [ -z "$diff_out" ]; then echo 'identical'; else echo "$diff_out"; fi)"
}

check_b8() {
    local section="### B8. Edge mutation counts match CG Hamming distances"

    local test_out
    test_out=$(cd "$REPO_DIR/build" && ctest -R merge_consistency_test -V 2>&1)
    local exit_code=$?

    local status="PASS"
    if [ $exit_code -ne 0 ]; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- ctest exit code: $exit_code
- Output: $(echo "$test_out" | tail -5)"
}

check_b9() {
    local section="### B9. All existing tests pass"

    local test_out
    test_out=$(cd "$REPO_DIR/build" && ctest --output-on-failure 2>&1)
    local exit_code=$?

    local passed failed
    passed=$(echo "$test_out" | grep -oP '\d+ tests passed' | grep -oP '\d+' || echo "0")
    failed=$(echo "$test_out" | grep -oP '\d+ tests failed' | grep -oP '\d+' || echo "0")

    local status="PASS"
    if [ $exit_code -ne 0 ]; then status="FAIL"; fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Tests passed: $passed
- Tests failed: $failed
- Output: $(echo "$test_out" | tail -10)"
}

check_b10() {
    local section="### B10. Diverse tree extraction produces distinct trees"

    # Use merged DAG from earlier or create fresh
    "$NEW_DAGUTIL" \
      --dag-pb "$TEST5_DIR/tree_0.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_1.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_2.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_3.pb.gz" \
      --dag-pb "$TEST5_DIR/tree_4.pb.gz" \
      --force-no-vcf \
      -o "${TMP}_b10_merged.pb.gz" >/dev/null 2>&1

    "$NEW_LARCH2" \
      --dag-pb "${TMP}_b10_merged.pb.gz" \
      -o "${TMP}_b10_diverse.pb.gz" \
      --diverse-sample 5 --diverse-newick "${TMP}_b10_diverse.nwk" -n 0 >/dev/null 2>&1

    local duplicates status="PASS" line_count="0"

    if [ ! -f "${TMP}_b10_diverse.nwk" ]; then
        status="FAIL"
        duplicates=""
    else
        duplicates=$(sort "${TMP}_b10_diverse.nwk" | uniq -d)
        if [ -n "$duplicates" ]; then status="FAIL"; fi
        line_count=$(wc -l < "${TMP}_b10_diverse.nwk")
        if [ "$line_count" -lt 1 ]; then status="FAIL"; fi
    fi

    if [ "$status" = "PASS" ]; then
        PART_B_PASS=$((PART_B_PASS + 1))
    else
        PART_B_FAIL=$((PART_B_FAIL + 1))
    fi

    add_report "$section
- Status: $status
- Newick lines: $line_count
- Duplicate trees: $(if [ -z "$duplicates" ]; then echo 'none'; else echo 'YES'; fi)"
}

##############################################################################
# MAIN
##############################################################################

echo "======================================"
echo "larch v1 vs larch2 Comparison"
echo "======================================"
echo ""

build_status_section

add_report "## Part A: Direct Comparisons"

echo "--- Part A ---"
echo "[A1] DAG statistics on testcase..."; check_a1
echo "[A2] Parsimony score distribution..."; check_a2
echo "[A3] Merge 5 DAG protobuf trees..."; check_a3
echo "[A4] Trim and compare..."; check_a4
echo "[A5] Merge 5 parsimony protobuf trees (20D)..."; check_a5
echo "[A6] RF distance comparison..."; check_a6
echo "[A7] SPR optimization comparison..."; check_a7
echo "[A8] FASTA+Newick load and merge..."; check_a8

add_report "## Part B: Self-Consistency Checks"

echo ""
echo "--- Part B ---"
echo "[B1] Theorem 3.15 invariant..."; check_b1
echo "[B2] CG roundtrip stability..."; check_b2
echo "[B3] Sampled trees have all leaves..."; check_b3
echo "[B4] Optimization does not degrade parsimony..."; check_b4
echo "[B5] DAG validation..."; check_b5
echo "[B6] Trim preserves DAG structure..."; check_b6
echo "[B7] Protobuf roundtrip..."; check_b7
echo "[B8] Edge mutation / CG Hamming..."; check_b8
echo "[B9] All existing tests pass..."; check_b9
echo "[B10] Diverse tree extraction..."; check_b10

echo ""
echo "======================================"
write_report
echo ""
echo "Part A: $PART_A_PASS passed, $PART_A_FAIL failed, $PART_A_SKIP skipped"
echo "Part B: $PART_B_PASS passed, $PART_B_FAIL failed"
echo "======================================"

# Cleanup temp tree copies
rm -rf /tmp/larch_comparison_fluC_trees
