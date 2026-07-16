#!/usr/bin/env python3
"""Strict Phase-9 local-commit acceptance postprocessor.

The benchmark harness owns measurement.  This program consumes its measured
``raw_trials.tsv`` plus the product reports and deferred canonical companions.
Historical frozen-oracle reports are accepted through a separate input: they
characterize fixture scale, but are never used as the one-worker denominator
for the Phase-9 speed gate.  That denominator and its paired W8 numerator are
always same-revision measured trials.

Exit status is zero only when every available gate passes.  An explicitly
deferred frozen-oracle characterization is a successful interim result, but it
is named ``deferred_frozen_oracle`` and cannot be mistaken for final Phase-9
acceptance.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, localcontext
from pathlib import Path
from typing import Iterable, Sequence


SCHEMA = "wric.phase9.acceptance"
SCHEMA_VERSION = 1
SEEDS = (1, 7, 19)
WORKERS = (1, 2, 4, 8)
DEFAULT_ROW_ID_TEMPLATE = "phase9-local-commit-s{seed}-w{worker}"
METHOD = "chart_spr_grammar_exact"
ITERATIONS = 3
MAX_CANDIDATES = 32
TOP_K_EXACT = 4
EXPECTED_CANDIDATES = ITERATIONS * MAX_CANDIDATES
EXPECTED_EXACT = ITERATIONS * TOP_K_EXACT
MIN_ACTIVE_PATTERNS = 64
MIN_ROWS_PER_ACCEPT = 32
MIN_FROZEN_ACCEPTED_REBUILD_MS = Decimal("100")
MIN_SPEEDUP = Decimal("1.5")
MIN_PHASE_SHARE = Decimal("0.10")
MAX_RSS_RATIO = Decimal("2")

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
UINT_RE = re.compile(r"^(0|[1-9][0-9]*)$")
DECIMAL_RE = re.compile(r"^(0|[1-9][0-9]*)(?:\.[0-9]+)?$")

REQUIRED_COLUMNS = (
    "fixture",
    "method",
    "status",
    "validation_status",
    "initial_validated_parsimony_min",
    "final_validated_parsimony_min",
    "best_reported_objective",
    "iterations",
    "seed",
    "acceptance",
    "candidate_selection",
    "candidate_source",
    "objective",
    "candidates_generated",
    "candidates_scored",
    "exact_verifications",
    "accepted_moves",
    "candidate_accepts_attempted",
    "post_materialization_rejections",
    "accepted_rebuild_ms",
    "total_ms",
    "active_patterns",
    "report_path",
    "row_id",
    "requested_workers",
    "resolved_workers",
    "worker_policy",
    "trial_index",
    "runner_outcome",
    "runner_exit_code",
    "exit_code",
    "term_signal",
    "core_dumped",
    "timed_out",
    "monitor_error",
    "rss_limit_enabled",
    "rss_limit_observed",
    "rss_limit_exceeded",
    "wall_clock_s",
    "peak_sampled_rss_kb",
    "peak_sampled_swap_kb",
    "process_rss_limit_bytes",
    "configured_chart_memory_budget",
    "manifest_rss_limit_bytes",
    "input_sha256",
    "refseq_sha256",
    "search_semantic_sha256",
    "output_semantic_sha256",
    "trial_semantic_sha256",
    "canonical_argv_sha256",
    "canonical_digest",
)

SEARCH_DIGEST_KEYS = (
    "semantic_sha256",
    "contract_sha256",
    "candidates_sha256",
    "exact_sha256",
    "acceptance_sha256",
    "chain_sha256",
    "final_topology_sha256",
)
SEARCH_COUNT_KEYS = (
    "record_count",
    "candidate_count",
    "exact_candidate_count",
    "iteration_count",
)
SEARCH_JSON_KEYS = frozenset(
    (
        "schema",
        "schema_version",
        "digest_algorithm",
        "payload_encoding",
        *SEARCH_DIGEST_KEYS,
        *SEARCH_COUNT_KEYS,
    )
)
DAG_DIGEST_KEYS = ("semantic_sha256", "clades_sha256", "productions_sha256")
DAG_COUNT_KEYS = ("clade_count", "production_count", "parsimony_min")
DAG_JSON_KEYS = frozenset(
    ("schema", "schema_version", "digest_algorithm", *DAG_DIGEST_KEYS, *DAG_COUNT_KEYS)
)


class AcceptanceError(RuntimeError):
    """A malformed artifact or failed Phase-9 gate."""


@dataclass(frozen=True)
class ParsedReport:
    path: Path
    top: dict[str, str]
    iterations: tuple[dict[str, str], ...]


@dataclass(frozen=True)
class Trial:
    row: dict[str, str]
    report: ParsedReport
    search_digest: dict[str, object]
    output_digest: dict[str, object]
    full_sidecar_sha256: str
    source_line: int
    seed: int
    worker: int
    trial_index: int

    @property
    def row_id(self) -> str:
        return self.row["row_id"]


def parse_uint(value: str, label: str, *, positive: bool = False) -> int:
    if not UINT_RE.fullmatch(value):
        raise AcceptanceError(f"{label}: expected an unsigned decimal integer, got {value!r}")
    result = int(value)
    if positive and result == 0:
        raise AcceptanceError(f"{label}: expected a positive integer")
    return result


def parse_decimal(value: str, label: str, *, positive: bool = False) -> Decimal:
    if not DECIMAL_RE.fullmatch(value):
        raise AcceptanceError(f"{label}: expected a nonnegative plain decimal, got {value!r}")
    try:
        result = Decimal(value)
    except InvalidOperation as error:
        raise AcceptanceError(f"{label}: invalid decimal {value!r}") from error
    if not result.is_finite() or (positive and result <= 0):
        qualifier = "positive" if positive else "nonnegative"
        raise AcceptanceError(f"{label}: expected a {qualifier} finite decimal, got {value!r}")
    return result


def parse_bool(value: str, label: str) -> bool:
    if value == "true":
        return True
    if value == "false":
        return False
    raise AcceptanceError(f"{label}: expected true or false, got {value!r}")


def decimal_text(value: Decimal) -> str:
    rendered = format(value, "f")
    if "." in rendered:
        rendered = rendered.rstrip("0").rstrip(".")
    return rendered or "0"


def ratio_text(numerator: Decimal, denominator: Decimal) -> str:
    if denominator <= 0:
        raise AcceptanceError("ratio denominator must be positive")
    with localcontext() as context:
        context.prec = 28
        return format(numerator / denominator, ".6f")


def median(values: Sequence[Decimal], label: str) -> Decimal:
    if not values:
        raise AcceptanceError(f"{label}: cannot compute an empty median")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / Decimal(2)


def read_tsv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    try:
        handle = path.open("r", encoding="utf-8", newline="")
    except OSError as error:
        raise AcceptanceError(f"cannot read raw trials {path}: {error}") from error
    with handle:
        reader = csv.reader(handle, delimiter="\t", strict=True)
        try:
            header = next(reader)
        except StopIteration as error:
            raise AcceptanceError(f"{path}: empty TSV") from error
        except csv.Error as error:
            raise AcceptanceError(f"{path}: malformed TSV header: {error}") from error
        if not header or any(field == "" for field in header):
            raise AcceptanceError(f"{path}: empty TSV header field")
        duplicates = sorted({field for field in header if header.count(field) > 1})
        if duplicates:
            raise AcceptanceError(f"{path}: duplicate TSV columns: {', '.join(duplicates)}")
        missing = [column for column in REQUIRED_COLUMNS if column not in header]
        if missing:
            raise AcceptanceError(f"{path}: missing required columns: {', '.join(missing)}")
        rows: list[dict[str, str]] = []
        try:
            for line_number, fields in enumerate(reader, start=2):
                if len(fields) != len(header):
                    raise AcceptanceError(
                        f"{path}:{line_number}: expected {len(header)} fields, got {len(fields)}"
                    )
                if any(field == "" for field in fields):
                    raise AcceptanceError(f"{path}:{line_number}: empty TSV field")
                row = dict(zip(header, fields, strict=True))
                row["__line__"] = str(line_number)
                rows.append(row)
        except csv.Error as error:
            raise AcceptanceError(f"{path}: malformed TSV: {error}") from error
    return header, rows


def resolve_recorded_path(raw_path: Path, recorded: str) -> Path:
    path = Path(recorded)
    if not path.is_absolute():
        path = raw_path.parent / path
    return path.resolve()


def parse_report(path: Path) -> ParsedReport:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise AcceptanceError(f"cannot read report {path}: {error}") from error
    sections = [index for index, line in enumerate(lines) if line == "chart_spr_search:"]
    if len(sections) != 1:
        raise AcceptanceError(f"{path}: expected exactly one chart_spr_search section, found {len(sections)}")

    top: dict[str, str] = {}
    iterations: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    in_iterations = False
    for line_number, line in enumerate(lines[sections[0] + 1 :], start=sections[0] + 2):
        if line and not line.startswith(" "):
            break
        top_match = re.fullmatch(r"  ([A-Za-z0-9_]+):[ \t]*(.*)", line)
        if top_match:
            key, value = top_match.groups()
            in_iterations = key == "iteration_reports"
            if value:
                if key in top:
                    raise AcceptanceError(f"{path}:{line_number}: duplicate top-level report key {key}")
                top[key] = value.strip()
            continue
        if not in_iterations:
            continue
        item_match = re.fullmatch(r"    - iteration:[ \t]*(.*)", line)
        if item_match:
            if current is not None:
                iterations.append(current)
            value = item_match.group(1).strip()
            if not value:
                raise AcceptanceError(f"{path}:{line_number}: iteration lacks an index")
            current = {"iteration": value}
            continue
        field_match = re.fullmatch(r"      ([A-Za-z0-9_]+):[ \t]*(.*)", line)
        if field_match and current is not None:
            key, value = field_match.groups()
            if not value:
                continue
            if key in current:
                raise AcceptanceError(f"{path}:{line_number}: duplicate iteration key {key}")
            current[key] = value.strip()
    if current is not None:
        iterations.append(current)
    return ParsedReport(path=path, top=top, iterations=tuple(iterations))


def require_top(report: ParsedReport, key: str, label: str) -> str:
    if key not in report.top:
        raise AcceptanceError(f"{label}: report lacks {key}")
    return report.top[key]


def require_iteration(iteration: dict[str, str], key: str, label: str) -> str:
    if key not in iteration:
        raise AcceptanceError(f"{label}: iteration report lacks {key}")
    return iteration[key]


def _reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise AcceptanceError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def read_json_object(path: Path, label: str) -> dict[str, object]:
    try:
        payload = path.read_text(encoding="utf-8")
    except OSError as error:
        raise AcceptanceError(f"cannot read {label} {path}: {error}") from error
    try:
        value = json.loads(payload, object_pairs_hook=_reject_duplicate_json_keys)
    except (json.JSONDecodeError, AcceptanceError) as error:
        raise AcceptanceError(f"{label} {path}: malformed JSON: {error}") from error
    if not isinstance(value, dict):
        raise AcceptanceError(f"{label} {path}: expected a JSON object")
    return value


def require_exact_json_keys(value: dict[str, object], expected: frozenset[str], label: str) -> None:
    missing = sorted(expected - value.keys())
    extra = sorted(value.keys() - expected)
    if missing or extra:
        details: list[str] = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if extra:
            details.append(f"unexpected {', '.join(extra)}")
        raise AcceptanceError(f"{label}: JSON schema mismatch ({'; '.join(details)})")


def validate_search_digest(path: Path) -> dict[str, object]:
    label = f"search digest {path}"
    value = read_json_object(path, "search digest")
    require_exact_json_keys(value, SEARCH_JSON_KEYS, label)
    literals = {
        "schema": "larch.chart_spr.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
        "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
    }
    for key, expected in literals.items():
        if value[key] != expected or type(value[key]) is not type(expected):
            raise AcceptanceError(f"{label}: {key}={value[key]!r}, expected {expected!r}")
    for key in SEARCH_DIGEST_KEYS:
        field = value[key]
        if not isinstance(field, str) or not SHA256_RE.fullmatch(field):
            raise AcceptanceError(f"{label}: {key} is not a lowercase SHA-256")
    for key in SEARCH_COUNT_KEYS:
        field = value[key]
        if type(field) is not int or field < 0:
            raise AcceptanceError(f"{label}: {key} is not an unsigned JSON integer")
    if value["record_count"] <= 0:
        raise AcceptanceError(f"{label}: record_count must be positive")
    return value


def validate_dag_digest(path: Path) -> dict[str, object]:
    label = f"canonical DAG digest {path}"
    value = read_json_object(path, "canonical DAG digest")
    require_exact_json_keys(value, DAG_JSON_KEYS, label)
    literals = {
        "schema": "larch.dag.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
    }
    for key, expected in literals.items():
        if value[key] != expected or type(value[key]) is not type(expected):
            raise AcceptanceError(f"{label}: {key}={value[key]!r}, expected {expected!r}")
    for key in DAG_DIGEST_KEYS:
        field = value[key]
        if not isinstance(field, str) or not SHA256_RE.fullmatch(field):
            raise AcceptanceError(f"{label}: {key} is not a lowercase SHA-256")
    for key in DAG_COUNT_KEYS:
        field = value[key]
        if type(field) is not int or field < 0:
            raise AcceptanceError(f"{label}: {key} is not an unsigned JSON integer")
    if value["clade_count"] <= 0 or value["production_count"] <= 0:
        raise AcceptanceError(f"{label}: clade/production counts must be positive")
    return value


def sanitize_harness_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def canonical_paths(raw_path: Path, row: dict[str, str], report_path: Path) -> tuple[Path, Path, Path, Path]:
    root = raw_path.parent
    fixture = sanitize_harness_name(row["fixture"])
    row_id = sanitize_harness_name(row["row_id"])
    compact = root / "logs" / f"{fixture}_{row_id}_canonical_companion.json"
    full = root / "logs" / f"{fixture}_{row_id}_full_canonical.json"
    sidecar = root / "logs" / f"{fixture}_{row_id}_full_canonical.ndjson"

    needle = f"_{row['method']}_"
    if report_path.name.count(needle) != 1 or not report_path.name.endswith(".out"):
        raise AcceptanceError(
            f"{row['row_id']} trial {row['trial_index']}: cannot derive deferred output digest from report_path"
        )
    score_name = report_path.name.replace(needle, f"{needle}score_", 1)
    score_digest = report_path.with_name(score_name[:-4] + ".canonical-dag.json")
    return compact.resolve(), full.resolve(), sidecar.resolve(), score_digest.resolve()


def sha256_file(path: Path, label: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while chunk := handle.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise AcceptanceError(f"cannot read {label} {path}: {error}") from error
    return digest.hexdigest()


def check_literal(actual: str, expected: str, label: str) -> None:
    if actual != expected:
        raise AcceptanceError(f"{label}={actual!r}, expected {expected!r}")


def validate_iteration_contract(report: ParsedReport, label: str) -> None:
    if len(report.iterations) != ITERATIONS:
        raise AcceptanceError(f"{label}: expected {ITERATIONS} iteration reports, found {len(report.iterations)}")
    indexes = [
        parse_uint(require_iteration(item, "iteration", label), f"{label} iteration index")
        for item in report.iterations
    ]
    if indexes != list(range(ITERATIONS)):
        raise AcceptanceError(f"{label}: iteration indexes are not exactly 0..{ITERATIONS - 1}")

    previous_after: int | None = None
    for index, item in enumerate(report.iterations):
        iteration_label = f"{label} iteration {index}"
        exact_values = {
            "candidates_generated": MAX_CANDIDATES,
            "candidates_scored": MAX_CANDIDATES,
            "candidates_exact_verified": TOP_K_EXACT,
            "candidate_score_failures": 0,
        }
        for key, expected in exact_values.items():
            actual = parse_uint(require_iteration(item, key, iteration_label), f"{iteration_label} {key}")
            if actual != expected:
                raise AcceptanceError(f"{iteration_label}: {key}={actual}, expected {expected}")
        for key, expected in (
            ("accepted_move_present", True),
            ("accepted_move_committed", True),
            ("post_materialization_rejected", False),
            ("reused_patterns_after_accept", True),
        ):
            actual = parse_bool(require_iteration(item, key, iteration_label), f"{iteration_label} {key}")
            if actual is not expected:
                raise AcceptanceError(f"{iteration_label}: {key}={str(actual).lower()}, expected {str(expected).lower()}")
        check_literal(
            require_iteration(item, "accepted_exact_kind", iteration_label),
            "grammar_exact",
            f"{iteration_label} accepted_exact_kind",
        )
        before = parse_uint(require_iteration(item, "state_score_before", iteration_label), f"{iteration_label} state_score_before")
        after = parse_uint(require_iteration(item, "state_score_after", iteration_label), f"{iteration_label} state_score_after")
        exact_after = parse_uint(require_iteration(item, "accepted_exact_new_score", iteration_label), f"{iteration_label} accepted_exact_new_score")
        rebuilt = parse_uint(require_iteration(item, "post_materialization_rebuilt_score", iteration_label), f"{iteration_label} post_materialization_rebuilt_score")
        if after > before:
            raise AcceptanceError(f"{iteration_label}: committed score worsened from {before} to {after}")
        if exact_after != after or rebuilt != after:
            raise AcceptanceError(f"{iteration_label}: exact/rebuilt/state-after scores disagree")
        if previous_after is not None and before != previous_after:
            raise AcceptanceError(f"{iteration_label}: state score is discontinuous across commits")
        previous_after = after


def validate_report_characterization(report: ParsedReport, seed: int, label: str) -> dict[str, int | str]:
    literals = {
        "acceptance": "exact_multisite",
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "candidate_cap_semantics": "post-dedup",
        "randomize_order": "false",
        "reservoir_sample": "false",
        "include_immediate_reversals": "false",
        "objective": "grammar_exact",
        "commit_mode": "overlay_delta",
        "verification_mode": "transient",
        "local_accept_updates": "true",
        "chain_per_accept_exactness_label": "exact_multisite",
        "dominance_mode": "off",
        "bound_pruning": "true",
        "require_exact_keep_mask": "true",
        "lazy_policy": "off",
        "validate": "true",
        "force_no_vcf": "true",
    }
    for key, expected in literals.items():
        check_literal(require_top(report, key, label), expected, f"{label} {key}")
    integer_literals = {
        "top_k_exact_verify": TOP_K_EXACT,
        "configured_max_candidates": MAX_CANDIDATES,
        "seed": seed,
        "requested_max_iterations": ITERATIONS,
        "iterations": ITERATIONS,
        "accepted_moves": ITERATIONS,
        "local_commit_accepted_moves": ITERATIONS,
        "candidates_generated": EXPECTED_CANDIDATES,
        "candidates_scored": EXPECTED_CANDIDATES,
        "exact_verifications": EXPECTED_EXACT,
        "post_materialization_rejections": 0,
        "sidecar_rebuilds_after_accept": 0,
        "overlay_materializations_for_accept_materialization": 0,
        "overlay_materializations_for_final_compaction": 1,
        "final_compaction_rebuilds": 1,
        "local_commit_tombstone_scope_skips": 0,
    }
    parsed: dict[str, int] = {}
    for key, expected in integer_literals.items():
        actual = parse_uint(require_top(report, key, label), f"{label} {key}")
        if actual != expected:
            raise AcceptanceError(f"{label}: {key}={actual}, expected {expected}")
        parsed[key] = actual
    check_literal(
        require_top(report, "final_compaction_exactness_kind", label),
        "exact_optimal_production_union",
        f"{label} final_compaction_exactness_kind",
    )
    active = parse_uint(require_top(report, "active_patterns", label), f"{label} active_patterns", positive=True)
    inside = parse_uint(require_top(report, "inside_rows_recomputed_on_commit", label), f"{label} inside_rows_recomputed_on_commit", positive=True)
    outside = parse_uint(require_top(report, "outside_rows_recomputed_on_commit", label), f"{label} outside_rows_recomputed_on_commit", positive=True)
    if active < MIN_ACTIVE_PATTERNS:
        raise AcceptanceError(f"{label}: active_patterns={active}, expected at least {MIN_ACTIVE_PATTERNS}")
    minimum_rows = ITERATIONS * MIN_ROWS_PER_ACCEPT
    if inside < minimum_rows:
        raise AcceptanceError(f"{label}: inside rows recomputed={inside}, expected at least {minimum_rows}")
    validate_iteration_contract(report, label)
    initial = parse_uint(require_top(report, "initial_score", label), f"{label} initial_score")
    final = parse_uint(require_top(report, "final_score", label), f"{label} final_score")
    if final > initial:
        raise AcceptanceError(f"{label}: final score {final} exceeds initial score {initial}")
    return {
        "active_patterns": active,
        "inside_rows_recomputed_on_commit": inside,
        "outside_rows_recomputed_on_commit": outside,
        "initial_score": initial,
        "final_score": final,
    }


def validate_scheduler(report: ParsedReport, worker: int, label: str) -> None:
    requested = parse_uint(require_top(report, "chart_workers_requested", label), f"{label} chart_workers_requested", positive=True)
    resolved = parse_uint(require_top(report, "chart_workers_resolved", label), f"{label} chart_workers_resolved", positive=True)
    if requested != worker or resolved != worker:
        raise AcceptanceError(f"{label}: report worker request/resolution is {requested}/{resolved}, expected {worker}/{worker}")
    for key in ("chart_worker_policy", "chart_worker_resolution_policy"):
        check_literal(require_top(report, key, label), "explicit", f"{label} {key}")

    names = {
        "operations": "chart_scheduler_operations",
        "parallel": "chart_scheduler_parallel_operations",
        "serial": "chart_scheduler_serial_fallbacks",
        "ranges": "chart_scheduler_ranges_created",
        "completed_ranges": "chart_scheduler_ranges_completed",
        "cancelled_ranges": "chart_scheduler_ranges_cancelled",
        "tasks": "chart_scheduler_tasks_submitted",
        "completed_tasks": "chart_scheduler_tasks_completed",
        "joined_tasks": "chart_scheduler_tasks_joined",
        "pending": "chart_scheduler_pending_tasks",
        "pending_shutdown": "chart_scheduler_pending_tasks_at_shutdown",
        "nested": "chart_scheduler_nested_serial_fallbacks",
        "rejected": "chart_scheduler_rejected_concurrent_operations",
        "pools": "chart_scheduler_pool_lifetimes",
        "stopped_pools": "chart_scheduler_pool_lifetimes_stopped",
        "live_threads": "chart_scheduler_live_pool_threads",
        "active_hwm": "chart_workers_actually_active_high_water",
    }
    metric = {
        key: parse_uint(require_top(report, report_key, label), f"{label} {report_key}")
        for key, report_key in names.items()
    }
    if metric["operations"] != metric["parallel"] + metric["serial"]:
        raise AcceptanceError(f"{label}: scheduler operation accounting does not reconcile")
    if metric["ranges"] != metric["completed_ranges"] + metric["cancelled_ranges"] or metric["cancelled_ranges"] != 0:
        raise AcceptanceError(f"{label}: scheduler range accounting/cancellation is invalid")
    if not (metric["tasks"] == metric["completed_tasks"] == metric["joined_tasks"]):
        raise AcceptanceError(f"{label}: scheduler task accounting does not reconcile")
    if metric["pending"] != 0 or metric["pending_shutdown"] != 0:
        raise AcceptanceError(f"{label}: scheduler left pending tasks")
    if metric["nested"] != 0 or metric["rejected"] != 0:
        raise AcceptanceError(f"{label}: scheduler reports nested/rejected operations")
    if metric["pools"] != metric["stopped_pools"] or metric["pools"] > 1:
        raise AcceptanceError(f"{label}: scheduler pool lifetime accounting is invalid")
    if metric["live_threads"] != 0 or not parse_bool(require_top(report, "chart_scheduler_shutdown", label), f"{label} chart_scheduler_shutdown"):
        raise AcceptanceError(f"{label}: scheduler did not fully shut down")
    if not 1 <= metric["active_hwm"] <= worker:
        raise AcceptanceError(f"{label}: invalid global active-worker high-water mark")
    if worker == 1 and (metric["parallel"] != 0 or metric["tasks"] != 0 or metric["pools"] != 0):
        raise AcceptanceError(f"{label}: one-worker run used a parallel pool")

    for axis in ("inside_cache", "outside_cache"):
        operations = parse_uint(require_top(report, f"chart_axis_{axis}_operations", label), f"{label} {axis} operations")
        parallel = parse_uint(require_top(report, f"chart_axis_{axis}_parallel_operations", label), f"{label} {axis} parallel_operations")
        hwm = parse_uint(require_top(report, f"chart_axis_{axis}_active_worker_high_water", label), f"{label} {axis} active-worker high-water")
        if operations == 0 or parallel > operations or not 1 <= hwm <= worker:
            raise AcceptanceError(f"{label}: invalid {axis} scheduler-axis evidence")
        if worker == 1 and (parallel != 0 or hwm != 1):
            raise AcceptanceError(f"{label}: one-worker {axis} axis used parallel execution")
        if worker == 8 and (parallel == 0 or hwm <= 1):
            raise AcceptanceError(f"{label}: W8 {axis} cache update did not activate multiple workers")


def validate_current_counters(
    report: ParsedReport, seed: int, worker: int, label: str
) -> dict[str, int | str]:
    characterization = validate_report_characterization(report, seed, label)
    exact_values = {
        "accepted_exact_trims_reused": ITERATIONS,
        "accepted_exact_trim_reuse_rejections": 0,
        "local_commit_tip_grammar_refreshes": ITERATIONS,
        "local_leaf_state_owned_copies": 0,
        "pattern_batch_cache_builds": 0,
        "transient_chain_diagnostic_cache_extensions": 0,
        "transient_chain_extension_fallbacks": 0,
        "transient_chain_extension_oracle_mismatches": 0,
    }
    for key, expected in exact_values.items():
        actual = parse_uint(require_top(report, key, label), f"{label} {key}")
        if actual != expected:
            raise AcceptanceError(f"{label}: {key}={actual}, expected {expected}")
    transient_extensions = parse_uint(
        require_top(report, "transient_chain_extensions_for_verification", label),
        f"{label} transient_chain_extensions_for_verification",
        positive=True,
    )
    exact_materializations = parse_uint(
        require_top(report, "overlay_materializations_for_exact_verification", label),
        f"{label} overlay_materializations_for_exact_verification",
    )
    if transient_extensions + exact_materializations != EXPECTED_EXACT:
        raise AcceptanceError(
            f"{label}: transient-extension/exact-materialization accounting "
            f"{transient_extensions}+{exact_materializations} does not equal {EXPECTED_EXACT} exact verifications"
        )
    row_visits = parse_uint(
        require_top(report, "local_commit_inside_row_view_pattern_visits", label),
        f"{label} local_commit_inside_row_view_pattern_visits",
        positive=True,
    )
    validate_scheduler(report, worker, label)
    return {**characterization, "inside_row_view_pattern_visits": row_visits}


def trial_semantic_digest(row: dict[str, str]) -> str:
    payload = (
        "wric-trial-semantic-v1\n"
        f"method={row['method']}\n"
        f"search_semantic_sha256={row['search_semantic_sha256']}\n"
        f"output_semantic_sha256={row['output_semantic_sha256']}\n"
        f"canonical_argv_sha256={row['canonical_argv_sha256']}\n"
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def validate_trial(raw_path: Path, row: dict[str, str], seed: int, worker: int) -> Trial:
    source_line = int(row["__line__"])
    trial_index = parse_uint(row["trial_index"], f"{raw_path}:{source_line} trial_index", positive=True)
    label = f"{row['row_id']} trial {trial_index}"
    literals = {
        "method": METHOD,
        "status": "ok",
        "validation_status": "ok",
        "iterations": str(ITERATIONS),
        "seed": str(seed),
        "acceptance": "exact_multisite",
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "objective": "grammar_exact",
        "candidates_generated": str(EXPECTED_CANDIDATES),
        "candidates_scored": str(EXPECTED_CANDIDATES),
        "exact_verifications": str(EXPECTED_EXACT),
        "accepted_moves": str(ITERATIONS),
        "post_materialization_rejections": "0",
        "requested_workers": str(worker),
        "resolved_workers": str(worker),
        "worker_policy": "explicit",
        "runner_outcome": "exited",
        "runner_exit_code": "0",
        "exit_code": "0",
        "term_signal": "0",
        "core_dumped": "0",
        "timed_out": "0",
        "monitor_error": "0",
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
    }
    for key, expected in literals.items():
        check_literal(row[key], expected, f"{label} {key}")
    attempts = parse_uint(row["candidate_accepts_attempted"], f"{label} candidate_accepts_attempted", positive=True)
    if attempts < ITERATIONS:
        raise AcceptanceError(f"{label}: candidate_accepts_attempted={attempts}, expected at least {ITERATIONS}")
    for key in ("initial_validated_parsimony_min", "final_validated_parsimony_min", "best_reported_objective"):
        parse_uint(row[key], f"{label} {key}")
    if int(row["final_validated_parsimony_min"]) > int(row["initial_validated_parsimony_min"]):
        raise AcceptanceError(f"{label}: externally validated score worsened")
    wall = parse_decimal(row["wall_clock_s"], f"{label} wall_clock_s", positive=True)
    accepted_ms = parse_decimal(row["accepted_rebuild_ms"], f"{label} accepted_rebuild_ms", positive=True)
    total_ms = parse_decimal(row["total_ms"], f"{label} total_ms", positive=True)
    if accepted_ms > total_ms:
        raise AcceptanceError(f"{label}: accepted_rebuild_ms exceeds total_ms")
    active = parse_uint(row["active_patterns"], f"{label} active_patterns", positive=True)
    if active < MIN_ACTIVE_PATTERNS:
        raise AcceptanceError(f"{label}: active_patterns={active}, expected at least {MIN_ACTIVE_PATTERNS}")
    del wall

    rss = parse_uint(row["peak_sampled_rss_kb"], f"{label} peak_sampled_rss_kb", positive=True)
    if parse_uint(row["peak_sampled_swap_kb"], f"{label} peak_sampled_swap_kb") != 0:
        raise AcceptanceError(f"{label}: peak sampled swap is nonzero")
    for key in ("process_rss_limit_bytes", "configured_chart_memory_budget", "manifest_rss_limit_bytes"):
        limit = parse_uint(row[key], f"{label} {key}", positive=True)
        if key != "configured_chart_memory_budget" and rss * 1024 > limit:
            raise AcceptanceError(f"{label}: peak RSS exceeds {key}")
    for key in ("input_sha256", "search_semantic_sha256", "output_semantic_sha256", "trial_semantic_sha256", "canonical_argv_sha256", "canonical_digest"):
        if not SHA256_RE.fullmatch(row[key]):
            raise AcceptanceError(f"{label}: {key} is not a lowercase SHA-256")
    if row["refseq_sha256"] != "NA" and not SHA256_RE.fullmatch(row["refseq_sha256"]):
        raise AcceptanceError(f"{label}: refseq_sha256 is neither NA nor a lowercase SHA-256")
    if row["canonical_digest"] != row["trial_semantic_sha256"]:
        raise AcceptanceError(f"{label}: canonical_digest/trial_semantic_sha256 differ")
    expected_trial_digest = trial_semantic_digest(row)
    if row["trial_semantic_sha256"] != expected_trial_digest:
        raise AcceptanceError(f"{label}: trial semantic digest does not bind method/search/output/argv")

    report_path = resolve_recorded_path(raw_path, row["report_path"])
    report = parse_report(report_path)
    report_summary = validate_current_counters(report, seed, worker, label)
    if int(report.top["seed"]) != seed:
        raise AcceptanceError(f"{label}: report seed does not match matrix seed")
    reconciliation = (
        "iterations",
        "accepted_moves",
        "candidates_generated",
        "candidates_scored",
        "exact_verifications",
        "post_materialization_rejections",
        "active_patterns",
        "initial_score",
        "final_score",
    )
    raw_name = {
        "initial_score": "initial_validated_parsimony_min",
        "final_score": "best_reported_objective",
    }
    for report_key in reconciliation:
        row_key = raw_name.get(report_key, report_key)
        if report.top[report_key] != row[row_key]:
            raise AcceptanceError(f"{label}: TSV/report {report_key} values disagree")
    for timing in ("accepted_rebuild_ms", "total_ms"):
        report_value = parse_decimal(require_top(report, timing, label), f"{label} report {timing}")
        row_value = parse_decimal(row[timing], f"{label} TSV {timing}")
        if report_value != row_value:
            raise AcceptanceError(f"{label}: TSV/report {timing} values disagree")
    if report_summary["initial_score"] != int(row["initial_validated_parsimony_min"]):
        raise AcceptanceError(f"{label}: initial score disagrees with external validation")

    compact_path, full_path, sidecar_path, dag_path = canonical_paths(raw_path, row, report_path)
    compact = validate_search_digest(compact_path)
    full = validate_search_digest(full_path)
    if compact != full:
        raise AcceptanceError(f"{label}: compact and full search-digest components differ")
    sidecar_sha = sha256_file(sidecar_path, "full canonical sidecar")
    if sidecar_sha != compact["semantic_sha256"]:
        raise AcceptanceError(f"{label}: full sidecar SHA-256 does not equal compact semantic digest")
    if compact["semantic_sha256"] != row["search_semantic_sha256"]:
        raise AcceptanceError(f"{label}: raw/search-companion semantic digests differ")
    if compact["candidate_count"] != EXPECTED_CANDIDATES or compact["exact_candidate_count"] != EXPECTED_EXACT or compact["iteration_count"] != ITERATIONS:
        raise AcceptanceError(f"{label}: canonical search record counts do not match the frozen 32/4/3 contract")

    output = validate_dag_digest(dag_path)
    if output["semantic_sha256"] != row["output_semantic_sha256"]:
        raise AcceptanceError(f"{label}: raw/external canonical DAG semantic digests differ")
    if output["parsimony_min"] != int(row["final_validated_parsimony_min"]):
        raise AcceptanceError(f"{label}: canonical DAG parsimony disagrees with raw validation")
    return Trial(
        row=row,
        report=report,
        search_digest=compact,
        output_digest=output,
        full_sidecar_sha256=sidecar_sha,
        source_line=source_line,
        seed=seed,
        worker=worker,
        trial_index=trial_index,
    )


def trials_for(trials: Iterable[Trial], seed: int, worker: int) -> list[Trial]:
    return [trial for trial in trials if trial.seed == seed and trial.worker == worker]


def select_matrix(raw_path: Path, rows: Sequence[dict[str, str]], template: str, repetitions: int) -> list[Trial]:
    expected_ids: dict[tuple[int, int], str] = {}
    try:
        for seed in SEEDS:
            for worker in WORKERS:
                expected_ids[(seed, worker)] = template.format(seed=seed, worker=worker)
    except (KeyError, ValueError) as error:
        raise AcceptanceError(f"invalid --row-id-template: {error}") from error
    if len(set(expected_ids.values())) != len(expected_ids):
        raise AcceptanceError("--row-id-template does not produce unique seed/worker row IDs")

    trials: list[Trial] = []
    for (seed, worker), row_id in expected_ids.items():
        matching = [row for row in rows if row["row_id"] == row_id]
        if len(matching) != repetitions:
            raise AcceptanceError(f"{row_id}: expected {repetitions} measured trials, found {len(matching)}")
        indexes = sorted(parse_uint(row["trial_index"], f"{row_id} trial_index", positive=True) for row in matching)
        if indexes != list(range(1, repetitions + 1)):
            raise AcceptanceError(f"{row_id}: trial indexes are not exactly 1..{repetitions}")
        trials.extend(validate_trial(raw_path, row, seed, worker) for row in matching)

    selected_ids = set(expected_ids.values())
    # A row that appears to be part of this matrix but is not one of the exact
    # template expansions is an error rather than silently ignored evidence.
    fixed_prefix = template.split("{", 1)[0]
    unexpected = sorted({row["row_id"] for row in rows if row["row_id"].startswith(fixed_prefix)} - selected_ids)
    if unexpected:
        raise AcceptanceError(f"unexpected rows under Phase-9 row prefix: {', '.join(unexpected)}")

    report_paths = [trial.report.path for trial in trials]
    if len(set(report_paths)) != len(report_paths):
        raise AcceptanceError("a report_path is reused across Phase-9 measured trials")
    fixtures: dict[int, str] = {}
    for seed in SEEDS:
        seed_trials = [trial for trial in trials if trial.seed == seed]
        seed_fixtures = {trial.row["fixture"] for trial in seed_trials}
        if len(seed_fixtures) != 1:
            raise AcceptanceError(f"seed {seed}: fixture changed across workers/trials")
        fixtures[seed] = next(iter(seed_fixtures))
        for key in ("input_sha256", "refseq_sha256"):
            if len({trial.row[key] for trial in seed_trials}) != 1:
                raise AcceptanceError(f"seed {seed}: {key} changed across workers/trials")
        for worker in WORKERS:
            repeats = trials_for(trials, seed, worker)
            for key in ("canonical_argv_sha256", "trial_semantic_sha256"):
                if len({trial.row[key] for trial in repeats}) != 1:
                    raise AcceptanceError(f"seed {seed} W{worker}: {key} changed across repeats")
        for key in ("search_semantic_sha256", "output_semantic_sha256"):
            if len({trial.row[key] for trial in seed_trials}) != 1:
                raise AcceptanceError(f"seed {seed}: {key} differs across workers/trials")
        for key in (*SEARCH_DIGEST_KEYS, *SEARCH_COUNT_KEYS):
            if len({trial.search_digest[key] for trial in seed_trials}) != 1:
                raise AcceptanceError(f"seed {seed}: canonical search component {key} differs across workers")
        for key in (*DAG_DIGEST_KEYS, *DAG_COUNT_KEYS):
            if len({trial.output_digest[key] for trial in seed_trials}) != 1:
                raise AcceptanceError(f"seed {seed}: canonical output component {key} differs across workers/trials")
    if len(set(fixtures.values())) != len(SEEDS):
        raise AcceptanceError("each seed must use a distinct fixture/workload name for unambiguous harness grouping")
    return trials


def parse_frozen_reports(values: Sequence[str], current_reports: set[Path]) -> dict[int, ParsedReport]:
    result: dict[int, ParsedReport] = {}
    for value in values:
        if "=" not in value:
            raise AcceptanceError(f"--frozen-oracle-report must be SEED=PATH, got {value!r}")
        seed_text, path_text = value.split("=", 1)
        seed = parse_uint(seed_text, "frozen-oracle seed", positive=True)
        if seed not in SEEDS:
            raise AcceptanceError(f"unexpected frozen-oracle seed {seed}; expected one of {SEEDS}")
        if seed in result:
            raise AcceptanceError(f"duplicate frozen-oracle report for seed {seed}")
        if not path_text:
            raise AcceptanceError(f"frozen-oracle seed {seed}: empty report path")
        path = Path(path_text).resolve()
        if path in current_reports:
            raise AcceptanceError(f"frozen-oracle seed {seed}: report is a same-revision measured report")
        result[seed] = parse_report(path)
    missing = [str(seed) for seed in SEEDS if seed not in result]
    if missing:
        raise AcceptanceError(f"missing frozen-oracle reports for seeds: {', '.join(missing)}")
    return result


def semantic_result(trials: Sequence[Trial], seed: int) -> dict[str, object]:
    representative = trials_for(trials, seed, 1)[0]
    return {
        "search": {key: representative.search_digest[key] for key in (*SEARCH_DIGEST_KEYS, *SEARCH_COUNT_KEYS)},
        "output": {key: representative.output_digest[key] for key in (*DAG_DIGEST_KEYS, *DAG_COUNT_KEYS)},
        "full_sidecar_sha256": representative.full_sidecar_sha256,
        "workers_compared": list(WORKERS),
    }


def evaluate(args: argparse.Namespace) -> dict[str, object]:
    if args.benchmark_dir:
        benchmark_dir = Path(args.benchmark_dir).resolve()
        raw_path = benchmark_dir / "raw_trials.tsv"
    else:
        raw_path = Path(args.raw_trials).resolve()
        benchmark_dir = raw_path.parent
    _, rows = read_tsv(raw_path)
    trials = select_matrix(raw_path, rows, args.row_id_template, args.repetitions)

    gates: list[dict[str, object]] = [
        {"name": "measured_trial_health", "status": "pass"},
        {"name": "frozen_32_4_3_contract", "status": "pass"},
        {"name": "three_real_local_commits", "status": "pass"},
        {"name": "incremental_counter_contract", "status": "pass"},
        {"name": "scheduler_quiescence", "status": "pass"},
        {"name": "w8_inside_outside_parallel_high_water", "status": "pass"},
        {"name": "canonical_search_and_output_parity", "status": "pass"},
        {"name": "swap_zero", "status": "pass"},
    ]

    timing_result: dict[str, object] = {}
    for seed in SEEDS:
        accepted_medians = {
            worker: median(
                [parse_decimal(trial.row["accepted_rebuild_ms"], f"seed {seed} W{worker} accepted_rebuild_ms") for trial in trials_for(trials, seed, worker)],
                f"seed {seed} W{worker} accepted_rebuild_ms median",
            )
            for worker in WORKERS
        }
        total_medians = {
            worker: median(
                [parse_decimal(trial.row["total_ms"], f"seed {seed} W{worker} total_ms") for trial in trials_for(trials, seed, worker)],
                f"seed {seed} W{worker} total_ms median",
            )
            for worker in WORKERS
        }
        share = accepted_medians[1] / total_medians[1]
        mandatory = share >= MIN_PHASE_SHARE
        speed_pass = accepted_medians[8] * MIN_SPEEDUP <= accepted_medians[1]
        if mandatory and not speed_pass:
            raise AcceptanceError(
                f"seed {seed}: same-revision W8 accepted_rebuild_ms median {decimal_text(accepted_medians[8])} "
                f"is not at least {decimal_text(MIN_SPEEDUP)}x faster than W1 {decimal_text(accepted_medians[1])}"
            )
        timing_result[str(seed)] = {
            "accepted_rebuild_median_ms": {str(worker): decimal_text(accepted_medians[worker]) for worker in WORKERS},
            "total_median_ms": {str(worker): decimal_text(total_medians[worker]) for worker in WORKERS},
            "w1_accepted_share_of_total": ratio_text(accepted_medians[1], total_medians[1]),
            "w8_over_w1_accepted_rebuild": ratio_text(accepted_medians[8], accepted_medians[1]),
            "speed_gate": "pass" if mandatory else "exempt_below_10_percent",
        }
    gates.append({"name": "same_revision_accepted_update_speed", "status": "pass", "minimum_speedup": decimal_text(MIN_SPEEDUP), "minimum_phase_share": decimal_text(MIN_PHASE_SHARE)})

    rss_result: dict[str, object] = {}
    for seed in SEEDS:
        w1 = max(parse_uint(trial.row["peak_sampled_rss_kb"], "W1 peak RSS") for trial in trials_for(trials, seed, 1))
        w8 = max(parse_uint(trial.row["peak_sampled_rss_kb"], "W8 peak RSS") for trial in trials_for(trials, seed, 8))
        if Decimal(w8) > MAX_RSS_RATIO * Decimal(w1):
            raise AcceptanceError(f"seed {seed}: W8 peak RSS {w8} KiB exceeds 2x W1 {w1} KiB")
        rss_result[str(seed)] = {"w1_max_kb": w1, "w8_max_kb": w8, "w8_over_w1": ratio_text(Decimal(w8), Decimal(w1))}
    gates.append({"name": "w8_rss_over_w1", "status": "pass", "limit": decimal_text(MAX_RSS_RATIO)})

    current_reports = {trial.report.path for trial in trials}
    frozen_result: dict[str, object]
    deferred = args.defer_frozen_oracle_characterization is not None
    if deferred:
        frozen_result = {
            "status": "deferred",
            "reason": args.defer_frozen_oracle_characterization,
            "role": "historical_fixture_characterization_only",
            "used_for_same_revision_speed_gate": False,
        }
        gates.append({"name": "frozen_oracle_characterization", "status": "deferred", "reason": args.defer_frozen_oracle_characterization})
    else:
        frozen = parse_frozen_reports(args.frozen_oracle_report, current_reports)
        reports: dict[str, object] = {}
        for seed in SEEDS:
            report = frozen[seed]
            summary = validate_report_characterization(report, seed, f"frozen-oracle seed {seed}")
            accepted_ms = parse_decimal(require_top(report, "accepted_rebuild_ms", f"frozen-oracle seed {seed}"), f"frozen-oracle seed {seed} accepted_rebuild_ms", positive=True)
            total_ms = parse_decimal(require_top(report, "total_ms", f"frozen-oracle seed {seed}"), f"frozen-oracle seed {seed} total_ms", positive=True)
            if accepted_ms > total_ms:
                raise AcceptanceError(f"frozen-oracle seed {seed}: accepted_rebuild_ms exceeds total_ms")
            if seed == 1 and accepted_ms < MIN_FROZEN_ACCEPTED_REBUILD_MS:
                raise AcceptanceError(
                    f"frozen-oracle seed 1: accepted_rebuild_ms {decimal_text(accepted_ms)} is below {decimal_text(MIN_FROZEN_ACCEPTED_REBUILD_MS)} ms"
                )
            reports[str(seed)] = {
                "report_path": os.fspath(report.path),
                "accepted_rebuild_ms": decimal_text(accepted_ms),
                "total_ms": decimal_text(total_ms),
                **summary,
            }
        frozen_result = {
            "status": "pass",
            "role": "historical_fixture_characterization_only",
            "used_for_same_revision_speed_gate": False,
            "performance_seed": 1,
            "minimum_performance_seed_accepted_rebuild_ms": decimal_text(MIN_FROZEN_ACCEPTED_REBUILD_MS),
            "reports": reports,
        }
        gates.append({"name": "frozen_oracle_characterization", "status": "pass", "performance_seed": 1, "minimum_accepted_rebuild_ms": decimal_text(MIN_FROZEN_ACCEPTED_REBUILD_MS)})

    fixture_by_seed = {
        str(seed): trials_for(trials, seed, 1)[0].row["fixture"] for seed in SEEDS
    }
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": "deferred_frozen_oracle" if deferred else "pass",
        "benchmark_dir": os.fspath(benchmark_dir),
        "raw_trials": os.fspath(raw_path),
        "matrix": {
            "seeds": list(SEEDS),
            "workers": list(WORKERS),
            "repetitions": args.repetitions,
            "row_id_template": args.row_id_template,
            "fixture_by_seed": fixture_by_seed,
        },
        "same_revision": {
            "role": "only_source_for_w1_w8_timing_comparison",
            "timings": timing_result,
            "rss": rss_result,
            "semantics": {str(seed): semantic_result(trials, seed) for seed in SEEDS},
        },
        "frozen_oracle_characterization": frozen_result,
        "artifact_evidence_limitations": [
            "reports expose aggregate recomputed-row counts, not per-commit dependency-plan cardinalities",
            "reports do not expose paired inside/outside cache epoch identities per commit",
            "manifest chart-row execution order is not balanced across workers in raw_trials.tsv",
        ],
        "gates": gates,
    }


def markdown_escape(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_markdown(result: dict[str, object]) -> str:
    lines = ["# WRIC Phase-9 local-commit acceptance", "", f"Status: `{markdown_escape(result['status'])}`", ""]
    if result["status"] == "fail":
        lines.extend((f"Failure: {markdown_escape(result.get('failure', 'unknown failure'))}", ""))
        return "\n".join(lines)
    lines.extend(
        (
            "The frozen oracle is used only to characterize the workload. All W1/W8 speed ratios below use same-revision measured trials.",
            "",
            "## Same-revision timing",
            "",
            "| Seed | W1 accepted ms | W8 accepted ms | W8/W1 | W1 phase share | Speed gate |",
            "|---:|---:|---:|---:|---:|---|",
        )
    )
    same = result["same_revision"]
    assert isinstance(same, dict)
    timings = same["timings"]
    assert isinstance(timings, dict)
    for seed in SEEDS:
        row = timings[str(seed)]
        assert isinstance(row, dict)
        medians = row["accepted_rebuild_median_ms"]
        assert isinstance(medians, dict)
        lines.append(
            f"| {seed} | {medians['1']} | {medians['8']} | {row['w8_over_w1_accepted_rebuild']} | {row['w1_accepted_share_of_total']} | `{row['speed_gate']}` |"
        )
    lines.extend(("", "## RSS", "", "| Seed | W1 max KiB | W8 max KiB | W8/W1 |", "|---:|---:|---:|---:|"))
    rss = same["rss"]
    assert isinstance(rss, dict)
    for seed in SEEDS:
        row = rss[str(seed)]
        assert isinstance(row, dict)
        lines.append(f"| {seed} | {row['w1_max_kb']} | {row['w8_max_kb']} | {row['w8_over_w1']} |")
    frozen = result["frozen_oracle_characterization"]
    assert isinstance(frozen, dict)
    lines.extend(("", "## Frozen-oracle characterization", "", f"Status: `{frozen['status']}`", ""))
    if frozen["status"] == "deferred":
        lines.extend((f"Reason: {markdown_escape(frozen['reason'])}", ""))
    else:
        lines.extend(("| Seed | Accepted rebuild ms | Active patterns | Inside rows | Outside rows |", "|---:|---:|---:|---:|---:|"))
        reports = frozen["reports"]
        assert isinstance(reports, dict)
        for seed in SEEDS:
            row = reports[str(seed)]
            assert isinstance(row, dict)
            lines.append(f"| {seed} | {row['accepted_rebuild_ms']} | {row['active_patterns']} | {row['inside_rows_recomputed_on_commit']} | {row['outside_rows_recomputed_on_commit']} |")
        lines.append("")
    lines.extend(("## Gates", "", "| Gate | Status |", "|---|---|"))
    gates = result["gates"]
    assert isinstance(gates, list)
    for gate in gates:
        assert isinstance(gate, dict)
        lines.append(f"| {markdown_escape(gate['name'])} | `{markdown_escape(gate['status'])}` |")
    lines.extend(("", "## Artifact evidence limitations", ""))
    limitations = result["artifact_evidence_limitations"]
    assert isinstance(limitations, list)
    lines.extend(f"- {markdown_escape(item)}" for item in limitations)
    lines.append("")
    return "\n".join(lines)


def write_atomic(path_text: str, payload: str, label: str) -> None:
    target = Path(path_text)
    temporary = target.with_name(f".{target.name}.tmp.{os.getpid()}")
    try:
        temporary.write_text(payload, encoding="utf-8")
        os.replace(temporary, target)
    except OSError as error:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise AcceptanceError(f"cannot write {label} {target}: {error}") from error


def emit_result(result: dict[str, object], json_output: str | None, markdown_output: str | None) -> None:
    rendered_json = json.dumps(result, indent=2, sort_keys=True) + "\n"
    rendered_markdown = render_markdown(result)
    if json_output:
        write_atomic(json_output, rendered_json, "JSON output")
    if markdown_output:
        write_atomic(markdown_output, rendered_markdown, "Markdown output")
    sys.stdout.write(rendered_json)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="strict Phase-9 local-commit acceptance postprocessor")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--benchmark-dir", help="benchmark output directory containing raw_trials.tsv")
    source.add_argument("--raw-trials", help="benchmark raw_trials.tsv (its parent is the output directory)")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--row-id-template", default=DEFAULT_ROW_ID_TEMPLATE, help="exact matrix row template with {seed} and {worker}")
    frozen = parser.add_mutually_exclusive_group(required=True)
    frozen.add_argument("--frozen-oracle-report", action="append", default=[], metavar="SEED=PATH", help="historical W1 characterization report; repeat for seeds 1,7,19")
    frozen.add_argument("--defer-frozen-oracle-characterization", metavar="REASON", help="explicitly defer only historical fixture characterization")
    parser.add_argument("--json-output", help="atomically write deterministic JSON")
    parser.add_argument("--markdown-output", help="atomically write deterministic Markdown")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.repetitions <= 0:
        parser.error("--repetitions must be positive")
    if args.defer_frozen_oracle_characterization is not None and not args.defer_frozen_oracle_characterization.strip():
        parser.error("--defer-frozen-oracle-characterization requires a nonempty reason")
    try:
        result = evaluate(args)
        emit_result(result, args.json_output, args.markdown_output)
        return 0
    except AcceptanceError as error:
        result = {"schema": SCHEMA, "schema_version": SCHEMA_VERSION, "status": "fail", "failure": str(error)}
    try:
        emit_result(result, args.json_output, args.markdown_output)
    except AcceptanceError as output_error:
        print(f"wric_phase9_acceptance: {output_error}", file=sys.stderr)
        sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
