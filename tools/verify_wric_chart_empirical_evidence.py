#!/usr/bin/env python3
"""Verify tracked WRIC 1614/recombination evidence and optional local artifacts."""

from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "doc"
FULL_PARSE_COUNT = 575_168
REDUCED_PARSE_COUNT = 864

TSV_SCHEMAS = {
    "WRIC-CHART-1614-SCORE-HISTOGRAM.tsv": (
        "score", "reduced_864_count", "full_575168_count",
    ),
    "WRIC-CHART-1614-MINIMA.tsv": (
        "class", "ordinal", "exact_score", "topology_sha256",
        "canonical_dag_semantic_sha256", "canonical_dag_clades_sha256",
        "canonical_dag_productions_sha256", "artifact", "artifact_sha256",
    ),
    "WRIC-CHART-1614-OUTCOME-ABLATIONS.tsv": (
        "removed_outcome_id", "direct_grammar_parses", "labelled_histories",
        "stored_history_minimum", "composite_lower_bound", "pattern_304_min",
        "pattern_312_min", "pattern_1002_min", "pattern_1023_min",
    ),
    "WRIC-CHART-1614-REDUCED-OUTCOME-ABLATIONS.tsv": (
        "removed_outcome_id", "direct_grammar_parses", "labelled_histories",
        "stored_history_minimum", "composite_lower_bound",
    ),
    "WRIC-CHART-1614-PRODUCTION-ABLATIONS.tsv": (
        "clade", "parent_taxa", "unreachable_topology_count", "production",
        "arity", "selected_topology_count", "forced_optimum",
        "forced_optimum_count", "removed_optimum", "removed_optimum_count",
        "global_optimum_selected_count",
    ),
    "WRIC-CHART-1614-PATTERN-PROVENANCE.tsv": (
        "pattern_id", "representative_site", "weight", "reference_state",
        "individual_lower_bound", "score_in_each_1616_topology",
        "regret_in_each_1616_topology",
        "score_in_each_known_labelled_1620_history",
        "regret_in_each_known_labelled_1620_history",
        "captured_provider_productions", "captured_provider_occurrences",
        "byte_distinct_provider_classes",
    ),
    "WRIC-CHART-1614-FOUR-SITE-COMPATIBILITY.tsv": (
        "mask", "patterns_required_at_individual_minimum",
        "direct_grammar_parses", "label_compatible_parses",
        "labelled_histories",
    ),
    "WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv": (
        "site", "captured_occurrences_included",
        "byte_distinct_class_representatives", "best_site_score",
        "captured_provider_productions", "provider_note",
    ),
    "WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv": (
        "score", "chart_fronted_additive_count", "corrected_native_count",
    ),
    "WRIC-CHART-RECOMBINATION-FIVE-SUBSETS.tsv": (
        "mask", "count", "fragments", "min_score", "tree_count", "nodes",
        "edges",
    ),
}

EXPECTED_TRACKED_SHA256 = {
    "WRIC-CHART-1614-SCORE-HISTOGRAM.tsv":
        "323875e95ff13a5b35a746d9a772fd61bfe9fe6536238898475acc9b18d641e2",
    "WRIC-CHART-1614-MINIMA.tsv":
        "a97ede118315884b0d72f8609a3731e1b39b16360b1d4867cb6a7dad9c2fed39",
    "WRIC-CHART-1614-OUTCOME-ABLATIONS.tsv":
        "e348cdf11a52ff6f78d4f58fa24409fac3d303454efee247b130b78aa5598b84",
    "WRIC-CHART-1614-REDUCED-OUTCOME-ABLATIONS.tsv":
        "9c8b56ebe6dcede775cd2371e9e47a03733ad72d1e1ae2c68af5137a43c18d63",
    "WRIC-CHART-1614-PRODUCTION-ABLATIONS.tsv":
        "2d50f316f5b0179a08670b09ae62b832ff3ad0a260e3e0f0b2d3adad573f4d43",
    "WRIC-CHART-1614-PATTERN-PROVENANCE.tsv":
        "7d71cf308710aef36a06df049097388ac6e52961467b02b6ceded3bc82f06e2f",
    "WRIC-CHART-1614-FOUR-SITE-COMPATIBILITY.tsv":
        "6bc662e0263a8eed5dfb855471214be8f8bd970dba768d8a00ba990fc18759d2",
    "WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv":
        "7d17df7e910c70814a30235b4df996af78ce6cb024bda53ba0aed2ad090ccbbb",
    "WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv":
        "9d53650761b9625c04cf3eb3b5ef583101a88d3246b36a839f4c9e2670ba7b5a",
    "WRIC-CHART-RECOMBINATION-FIVE-SUBSETS.tsv":
        "3cac55f15e9609492898c8b80964ac3f6a47015d0aafae19912f7d3bfc39707a",
}

EXPECTED_RECOMBINATION_HISTOGRAM = {
    1620: 4, 1621: 56, 1622: 245, 1623: 530, 1624: 895,
    1625: 1405, 1626: 1816, 1627: 2020, 1628: 2164, 1629: 2342,
    1630: 2737, 1631: 3396, 1632: 4261, 1633: 5030, 1634: 5214,
    1635: 4717, 1636: 3718, 1637: 2475, 1638: 1330, 1639: 544,
    1640: 156, 1641: 27, 1642: 2,
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_tsv(name: str) -> list[dict[str, str]]:
    path = DOC / name
    try:
        expected_hash = EXPECTED_TRACKED_SHA256.get(name)
        if expected_hash is None:
            fail(f"{name}: verifier has no pinned SHA-256")
        actual_hash = sha256(path)
        if actual_hash != expected_hash:
            fail(
                f"{name}: SHA-256 differs: expected {expected_hash}, "
                f"got {actual_hash}"
            )
        with path.open(newline="", encoding="utf-8") as handle:
            lines = [line for line in handle if not line.startswith("#")]
        reader = csv.DictReader(lines, delimiter="\t")
        if reader.fieldnames is None:
            fail(f"{name}: missing header")
        expected_schema = TSV_SCHEMAS.get(name)
        if expected_schema is None:
            fail(f"{name}: verifier has no declared schema")
        if tuple(reader.fieldnames) != expected_schema:
            fail(
                f"{name}: schema differs: expected {expected_schema}, "
                f"got {tuple(reader.fieldnames)}"
            )
        rows = list(reader)
    except (OSError, UnicodeError, csv.Error) as error:
        fail(f"{name}: cannot read TSV: {error}")
    if not rows:
        fail(f"{name}: no rows")
    for row_number, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            fail(f"{name}: malformed row {row_number}")
    return rows


def as_int(row: dict[str, str], field: str, source: str) -> int:
    try:
        return int(row[field])
    except (KeyError, TypeError, ValueError) as error:
        fail(f"{source}: invalid integer {field!r}: {error}")


def require_unique_int_field(
    rows: list[dict[str, str]], field: str, source: str
) -> list[int]:
    values = [as_int(row, field, source) for row in rows]
    if len(values) != len(set(values)):
        fail(f"{source}: duplicate {field} value")
    return values


def require_nonnegative_fields(
    rows: list[dict[str, str]], fields: tuple[str, ...], source: str
) -> None:
    for row in rows:
        for field in fields:
            if as_int(row, field, source) < 0:
                fail(f"{source}: negative {field}")


def check_direct_census() -> None:
    source = "WRIC-CHART-1614-SCORE-HISTOGRAM.tsv"
    rows = read_tsv(source)
    scores = require_unique_int_field(rows, "score", source)
    if set(scores) != set(range(1616, 1643)):
        fail(f"{source}: scores are not exactly 1616..1642")
    require_nonnegative_fields(
        rows, ("reduced_864_count", "full_575168_count"), source
    )
    reduced = sum(as_int(row, "reduced_864_count", source) for row in rows)
    full = sum(as_int(row, "full_575168_count", source) for row in rows)
    if (reduced, full) != (REDUCED_PARSE_COUNT, FULL_PARSE_COUNT):
        fail(f"{source}: histogram totals are {(reduced, full)}")
    by_score = {as_int(row, "score", source): row for row in rows}
    if min(score for score, row in by_score.items()
           if as_int(row, "full_575168_count", source)) != 1616:
        fail(f"{source}: full minimum is not 1616")
    if as_int(by_score[1616], "full_575168_count", source) != 12:
        fail(f"{source}: score-1616 count is not 12")
    if min(score for score, row in by_score.items()
           if as_int(row, "reduced_864_count", source)) != 1617:
        fail(f"{source}: reduced minimum is not 1617")
    if as_int(by_score[1617], "reduced_864_count", source) != 1:
        fail(f"{source}: reduced score-1617 count is not 1")

    source = "WRIC-CHART-1614-MINIMA.tsv"
    minima = read_tsv(source)
    if len(minima) != 12:
        fail(f"{source}: expected 12 rows, got {len(minima)}")
    for field in (
        "class",
        "ordinal",
        "topology_sha256",
        "canonical_dag_semantic_sha256",
        "canonical_dag_clades_sha256",
        "canonical_dag_productions_sha256",
        "artifact_sha256",
    ):
        values = [row[field] for row in minima]
        if len(set(values)) != 12:
            fail(f"{source}: {field} is not unique")
    if {as_int(row, "class", source) for row in minima} != set(range(12)):
        fail(f"{source}: classes are not exactly 0..11")
    if any(as_int(row, "exact_score", source) != 1616 for row in minima):
        fail(f"{source}: a minimum score differs from 1616")

    source = "WRIC-CHART-1614-OUTCOME-ABLATIONS.tsv"
    outcomes = read_tsv(source)
    if len(outcomes) != 55:
        fail(f"{source}: expected 55 outcome rows")
    require_unique_int_field(outcomes, "removed_outcome_id", source)
    require_nonnegative_fields(
        outcomes,
        (
            "removed_outcome_id", "direct_grammar_parses", "labelled_histories",
            "stored_history_minimum", "composite_lower_bound",
            "pattern_304_min", "pattern_312_min", "pattern_1002_min",
            "pattern_1023_min",
        ),
        source,
    )
    source = "WRIC-CHART-1614-REDUCED-OUTCOME-ABLATIONS.tsv"
    reduced_outcomes = read_tsv(source)
    if len(reduced_outcomes) != 11:
        fail(f"{source}: expected 11 outcome rows")
    require_unique_int_field(reduced_outcomes, "removed_outcome_id", source)
    require_nonnegative_fields(
        reduced_outcomes,
        (
            "removed_outcome_id", "direct_grammar_parses", "labelled_histories",
            "stored_history_minimum", "composite_lower_bound",
        ),
        source,
    )

    source = "WRIC-CHART-1614-PRODUCTION-ABLATIONS.tsv"
    productions = read_tsv(source)
    if len(productions) != 79:
        fail(f"{source}: expected 79 production rows")
    require_unique_int_field(productions, "production", source)
    require_nonnegative_fields(
        productions,
        (
            "unreachable_topology_count", "arity", "selected_topology_count",
            "forced_optimum_count", "removed_optimum_count",
            "global_optimum_selected_count",
        ),
        source,
    )
    by_clade: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in productions:
        by_clade[as_int(row, "clade", source)].append(row)
    for clade, alternatives in by_clade.items():
        unreachable_values = {
            as_int(row, "unreachable_topology_count", source)
            for row in alternatives
        }
        if len(unreachable_values) != 1:
            fail(f"{source}: clade {clade} has inconsistent unreachable counts")
        selected = sum(
            as_int(row, "selected_topology_count", source)
            for row in alternatives
        )
        if selected + next(iter(unreachable_values)) != FULL_PARSE_COUNT:
            fail(f"{source}: clade {clade} does not reconcile to {FULL_PARSE_COUNT}")
    indispensable = {
        as_int(row, "production", source)
        for row in productions
        if as_int(row, "global_optimum_selected_count", source) == 12
        and as_int(row, "removed_optimum", source) > 1616
    }
    expected_indispensable = {
        132, 134, 290, 325, 348, 406, 425,
        503, 506, 524, 527, 545, 551,
    }
    if indispensable != expected_indispensable:
        fail(f"{source}: indispensable production set differs: {indispensable}")


def check_pattern_provenance() -> None:
    source = "WRIC-CHART-1614-PATTERN-PROVENANCE.tsv"
    rows = read_tsv(source)
    pattern_ids = require_unique_int_field(rows, "pattern_id", source)
    by_pattern = {as_int(row, "pattern_id", source): row for row in rows}
    expected = {
        304: (7716, 1, "1", 23, 24, 1, 24, 1, "449", "31 OR 82", "31 OR 30"),
        312: (7899, 1, "2", 3, 3, 0, 4, 1, "352 AND 460", "264", "114"),
        1002: (26799, 1, "1", 3, 3, 0, 5, 2, "132 AND 465", "36 AND 84", "35 AND 28"),
        1023: (27563, 1, "1", 20, 21, 1, 22, 2, "463 AND 470", "(33 OR 34) AND 86", "(33 OR 32) AND 39"),
    }
    if len(rows) != len(expected) or set(pattern_ids) != set(expected):
        fail(f"{source}: pattern IDs differ from {sorted(expected)}")
    fields = (
        "representative_site",
        "weight",
        "reference_state",
        "individual_lower_bound",
        "score_in_each_1616_topology",
        "regret_in_each_1616_topology",
        "score_in_each_known_labelled_1620_history",
        "regret_in_each_known_labelled_1620_history",
        "captured_provider_productions",
        "captured_provider_occurrences",
        "byte_distinct_provider_classes",
    )
    for pattern, values in expected.items():
        row = by_pattern[pattern]
        actual = tuple(
            row[field] if field in {
                "reference_state",
                "captured_provider_productions",
                "captured_provider_occurrences",
                "byte_distinct_provider_classes",
            } else as_int(row, field, source)
            for field in fields
        )
        if actual != values:
            fail(f"{source}: pattern {pattern} differs: {actual}")
        lower = as_int(row, "individual_lower_bound", source)
        full_score = as_int(row, "score_in_each_1616_topology", source)
        full_regret = as_int(row, "regret_in_each_1616_topology", source)
        labelled_score = as_int(
            row, "score_in_each_known_labelled_1620_history", source
        )
        labelled_regret = as_int(
            row, "regret_in_each_known_labelled_1620_history", source
        )
        if full_score - lower != full_regret or labelled_score - lower != labelled_regret:
            fail(f"{source}: pattern {pattern} regret arithmetic does not reconcile")
    if sum(as_int(row, "individual_lower_bound", source) for row in rows) != 49:
        fail(f"{source}: four-pattern individual minima do not sum to 49")

    source = "WRIC-CHART-1614-FOUR-SITE-COMPATIBILITY.tsv"
    compatibility = read_tsv(source)
    masks = require_unique_int_field(compatibility, "mask", source)
    if len(compatibility) != 15 or set(masks) != set(range(1, 16)):
        fail(f"{source}: masks are not exactly 1..15")
    require_nonnegative_fields(
        compatibility,
        ("direct_grammar_parses", "label_compatible_parses", "labelled_histories"),
        source,
    )
    bits = ((1, "304"), (2, "312"), (4, "1002"), (8, "1023"))
    by_mask = {as_int(row, "mask", source): row for row in compatibility}
    for mask, row in by_mask.items():
        expected_patterns = ",".join(name for bit, name in bits if mask & bit)
        if row["patterns_required_at_individual_minimum"] != expected_patterns:
            fail(f"{source}: mask {mask} pattern list is inconsistent")
    for mask in (10, 15):
        if any(as_int(by_mask[mask], field, source) != 0 for field in (
            "direct_grammar_parses", "label_compatible_parses", "labelled_histories"
        )):
            fail(f"{source}: mask {mask} is not empty")

    source = "WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv"
    providers = read_tsv(source)
    provider_keys = [
        (as_int(row, "site", source), row["captured_occurrences_included"])
        for row in providers
    ]
    if len(provider_keys) != len(set(provider_keys)):
        fail(f"{source}: duplicate site/provider row")
    require_nonnegative_fields(providers, ("best_site_score",), source)
    minimum_rows = defaultdict(set)
    lower_by_site = {values[0]: values[3] for values in expected.values()}
    for row in providers:
        site = as_int(row, "site", source)
        score = as_int(row, "best_site_score", source)
        if score == lower_by_site[site]:
            minimum_rows[site].add((
                row["captured_occurrences_included"],
                row["byte_distinct_class_representatives"],
                row["captured_provider_productions"],
            ))
    expected_minimum_rows = {
        7716: {("31", "31", "449"), ("82", "30", "449")},
        7899: {("264", "114", "352,460")},
        26799: {("36,84", "35,28", "132,465")},
        27563: {
            ("33,86", "33,39", "463,470"),
            ("34,86", "32,39", "463,470"),
        },
    }
    if dict(minimum_rows) != expected_minimum_rows:
        fail(f"{source}: minimum provider rows differ: {dict(minimum_rows)}")


def tracked_recombination_histogram() -> dict[int, int]:
    source = "WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv"
    rows = read_tsv(source)
    scores = require_unique_int_field(rows, "score", source)
    if len(rows) != 23 or set(scores) != set(range(1620, 1643)):
        fail(f"{source}: scores are not exactly 1620..1642")
    require_nonnegative_fields(
        rows, ("chart_fronted_additive_count", "corrected_native_count"), source
    )
    chart = {
        as_int(row, "score", source):
        as_int(row, "chart_fronted_additive_count", source)
        for row in rows
    }
    native = {
        as_int(row, "score", source): as_int(row, "corrected_native_count", source)
        for row in rows
    }
    if chart != native:
        fail(f"{source}: chart and native histograms differ")
    if chart != EXPECTED_RECOMBINATION_HISTOGRAM:
        fail(f"{source}: histogram differs from the independently captured bins")
    return chart


def check_five_subset_factorization() -> None:
    source = "WRIC-CHART-RECOMBINATION-FIVE-SUBSETS.tsv"
    rows = read_tsv(source)
    masks = require_unique_int_field(rows, "mask", source)
    if len(rows) != 32 or set(masks) != set(range(32)):
        fail(f"{source}: masks are not exactly 0..31")
    require_nonnegative_fields(rows, ("count", "tree_count", "nodes", "edges"), source)
    deltas = (4, 2, 1, 10, 5)
    for row in rows:
        mask = as_int(row, "mask", source)
        count = as_int(row, "count", source)
        if count != mask.bit_count():
            fail(f"{source}: mask {mask} has wrong count")
        if as_int(row, "tree_count", source) != 2 ** count:
            fail(f"{source}: mask {mask} violates the tree-count factorization")
        expected_score = 1642 - sum(
            delta for bit, delta in enumerate(deltas) if mask & (1 << bit)
        )
        if as_int(row, "min_score", source) != expected_score:
            fail(f"{source}: mask {mask} violates the additive score identity")


def parse_parsimony_histogram(stdout: str, source: str) -> dict[int, int]:
    line = next((line for line in stdout.splitlines() if line.startswith("{")), None)
    if line is None:
        fail(f"{source}: dagutil output has no histogram")
    try:
        value = ast.literal_eval(line)
    except (SyntaxError, ValueError) as error:
        fail(f"{source}: invalid histogram: {error}")
    if not isinstance(value, dict) or any(
        not isinstance(key, int) or not isinstance(count, int)
        for key, count in value.items()
    ):
        fail(f"{source}: histogram is not an integer mapping")
    return value


def run_parsimony(dagutil: Path, dag: Path) -> tuple[dict[int, int], bytes]:
    completed = subprocess.run(
        [
            str(dagutil), "--dag-pb", str(dag), "--force-no-vcf",
            "--validate", "--parsimony",
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        fail(
            f"dagutil failed for {dag} with {completed.returncode}: "
            f"{completed.stderr.decode(errors='replace')}"
        )
    stdout = completed.stdout.decode("utf-8")
    return parse_parsimony_histogram(stdout, str(dag)), completed.stdout


def check_local_artifacts(tracked_histogram: dict[int, int]) -> None:
    trace = ROOT / "build/wric-1614-experiments/four-site-census.txt"
    expected_trace_hash = "0074b0c9d231c8da91e42f42b293f0729918f3844dd56e81dad1280086b52c14"
    if not trace.is_file() or sha256(trace) != expected_trace_hash:
        fail(f"local four-site trace is missing or differs from {expected_trace_hash}")
    pattern_re = re.compile(
        r"^pattern\t(304|312|1002|1023)\tposition\t(\d+)\tweight\t(\d+)\treference\t(\d+)$"
    )
    trace_patterns = {}
    for line in trace.read_text(encoding="utf-8").splitlines():
        match = pattern_re.match(line)
        if match:
            trace_patterns[int(match.group(1))] = tuple(map(int, match.groups()[1:]))
    expected_trace_patterns = {
        304: (7716, 1, 1),
        312: (7899, 1, 2),
        1002: (26799, 1, 1),
        1023: (27563, 1, 1),
    }
    if trace_patterns != expected_trace_patterns:
        fail(f"local four-site trace pattern metadata differs: {trace_patterns}")

    pattern_scores = Path("/tmp/case-study-current-pattern-scores.tsv")
    expected_pattern_scores_hash = "0485fe11d58f79d0f7a94017d3ce39ca77bcc3337a72654d2c83e6db3c7d5991"
    if (
        not pattern_scores.is_file()
        or sha256(pattern_scores) != expected_pattern_scores_hash
    ):
        fail(
            "local pattern-score capture is missing or differs from "
            f"{expected_pattern_scores_hash}"
        )
    score_rows = []
    for line_number, line in enumerate(
        pattern_scores.read_text(encoding="utf-8").splitlines(), start=1
    ):
        fields = line.split("\t")
        if len(fields) != 3:
            fail(f"local pattern-score capture line {line_number} is malformed")
        try:
            score_rows.append(tuple(map(int, fields)))
        except ValueError as error:
            fail(f"local pattern-score capture line {line_number}: {error}")
    if len(score_rows) != 1110 or [row[0] for row in score_rows] != list(range(1110)):
        fail("local pattern-score capture does not contain exactly patterns 0..1109")
    if sum(row[1] for row in score_rows) != 29_903:
        fail("local pattern-score weights do not sum to 29903")
    if sum(row[1] for row in score_rows if row[2] == 0) != 28_665:
        fail("local zero-score pattern weight does not equal 28665")
    expected_selected_scores = {
        304: (1, 23), 312: (1, 3), 1002: (1, 3), 1023: (1, 20)
    }
    selected_scores = {
        pattern: (weight, score)
        for pattern, weight, score in score_rows
        if pattern in expected_selected_scores
    }
    if selected_scores != expected_selected_scores:
        fail(f"local selected pattern scores differ: {selected_scores}")

    dagutil = ROOT / "build/bin/dagutil"
    if not dagutil.is_file():
        fail(f"local dagutil is missing: {dagutil}")
    dags = (
        (
            Path("/tmp/chart-additive-medium-final.pb.gz"),
            "ae9c0c1b746439e3544eb2e8cef877c404e019d493d173a922717b1772a2b25b",
        ),
        (
            Path("/tmp/case-study-current-native-n1.pb.gz"),
            "7b67a125dec0fa930ce92a96220b56655b63f1018c75babd395360a2400820b5",
        ),
    )
    stdout_values = []
    for dag, expected_hash in dags:
        if not dag.is_file() or sha256(dag) != expected_hash:
            fail(f"local DAG is missing or differs from {expected_hash}: {dag}")
        histogram, stdout = run_parsimony(dagutil, dag)
        if histogram != tracked_histogram:
            fail(f"local DAG histogram differs from tracked evidence: {dag}")
        stdout_values.append(stdout)
    if stdout_values[0] != stdout_values[1]:
        fail("local chart-fronted and corrected-native parsimony stdout differs")
    stdout_hash = hashlib.sha256(stdout_values[0]).hexdigest()
    expected_stdout_hash = "d3958c9a717632d7fc74ccce73c46c69fae1f9656322a15783a6b4cdbd02e26a"
    if stdout_hash != expected_stdout_hash:
        fail(f"local parsimony stdout differs from {expected_stdout_hash}")

    for row in read_tsv("WRIC-CHART-1614-MINIMA.tsv"):
        artifact = ROOT / row["artifact"]
        if not artifact.is_file() or sha256(artifact) != row["artifact_sha256"]:
            fail(f"retained census minimum is missing or hash-mismatched: {artifact}")
        histogram, _ = run_parsimony(dagutil, artifact)
        if histogram != {1616: 1}:
            fail(f"retained census minimum does not rescore to one 1616 history: {artifact}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--local-artifacts",
        action="store_true",
        help="also require and independently check the ignored trace and retained DAGs",
    )
    args = parser.parse_args()

    try:
        check_direct_census()
        check_pattern_provenance()
        tracked_histogram = tracked_recombination_histogram()
        check_five_subset_factorization()
        if args.local_artifacts:
            check_local_artifacts(tracked_histogram)
    except RuntimeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    scope = "tracked evidence plus local artifacts" if args.local_artifacts else "tracked evidence"
    print(f"OK: verified {scope}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
