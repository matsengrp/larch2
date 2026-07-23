#!/usr/bin/env python3
"""Strict Phase-4 WRIC chart-parallelization acceptance postprocessor.

The Phase-0 harness owns measurement and writes ``raw_trials.tsv``.  This
program deliberately does not modify that harness or aggregate its
``summary.tsv``: phase timing medians must be computed from every raw trial,
and scheduler evidence lives in each trial's ``report_path`` report.

Exit status is zero only when every available gate passes.  An explicitly
deferred Phase-3 baseline is reported as ``deferred_baseline`` and is also a
successful *interim* result; omitting both a baseline and an explicit deferral
is an error.  ``profiling_required`` is a blocking nonzero result.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import stat
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Iterable, Sequence


SCHEMA_VERSION = 1
WORKERS = (1, 2, 4, 8)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
UINT_RE = re.compile(r"^(0|[1-9][0-9]*)$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
CONTENTION_LIMIT = Decimal("0.25")

DEFAULT_LOCAL_PREFIX = "p0-medium-dense64-grammar-lower-bound-heuristic-w"
DEFAULT_CONSTRUCTION_PREFIX = (
    "p0-medium-cache1-grammar-lower-bound-heuristic-w"
)

AXES = (
    "initial_chart",
    "candidate_generation",
    "exact_setup",
    "exact_frontier_clade",
    "exact_candidate",
    "lazy_inside_clade",
    "lazy_outside_clade",
    "inside_cache",
    "outside_cache",
    "fixed_topology_oracle",
    "local_candidate",
    "local_candidate_pattern",
    "other",
)
AXIS_FIELDS = (
    "operations",
    "items",
    "ranges",
    "tasks",
    "active_worker_high_water",
)
AXIS_DIAGNOSTIC_FIELDS = (
    "parallel_operations",
    "minimum_effective_grain",
    "maximum_effective_grain",
)

REQUIRED_COLUMNS = (
    "row_id",
    "fixture",
    "method",
    "status",
    "validation_status",
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
    "user_cpu_s",
    "system_cpu_s",
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
    "local_scoring_ms",
    "initial_chart_construction_ms",
    "report_path",
)

HASH_COLUMNS = (
    "input_sha256",
    "refseq_sha256",
    "search_semantic_sha256",
    "output_semantic_sha256",
    "trial_semantic_sha256",
    "canonical_argv_sha256",
    "canonical_digest",
)


class AcceptanceError(RuntimeError):
    """A malformed artifact or failed acceptance gate."""


class ProfilingRequired(AcceptanceError):
    """System-time contention crossed the mandatory profiling threshold."""


@dataclass(frozen=True)
class RawTrials:
    """One canonical raw-trial table and the rows it owns."""

    path: Path
    header: tuple[str, ...]
    rows: tuple[dict[str, str], ...]


@dataclass(frozen=True)
class Trial:
    row: dict[str, str]
    report: dict[str, str]
    source_path: Path
    report_path: Path
    source_line: int

    @property
    def row_id(self) -> str:
        return self.row["row_id"]

    @property
    def worker(self) -> int:
        return int(self.row["requested_workers"])

    @property
    def trial_index(self) -> int:
        return int(self.row["trial_index"])


def parse_decimal(value: str, label: str, *, positive: bool = False) -> Decimal:
    try:
        result = Decimal(value)
    except (InvalidOperation, ValueError) as error:
        raise AcceptanceError(f"{label}: expected a finite decimal, got {value!r}") from error
    if not result.is_finite() or result < 0 or (positive and result <= 0):
        qualifier = "positive finite" if positive else "nonnegative finite"
        raise AcceptanceError(f"{label}: expected a {qualifier} decimal, got {value!r}")
    return result


def parse_uint(value: str, label: str, *, positive: bool = False) -> int:
    if not UINT_RE.fullmatch(value):
        raise AcceptanceError(f"{label}: expected an unsigned decimal integer, got {value!r}")
    result = int(value)
    if positive and result == 0:
        raise AcceptanceError(f"{label}: expected a positive integer")
    return result


def parse_bool(value: str, label: str) -> bool:
    if value == "true":
        return True
    if value == "false":
        return False
    raise AcceptanceError(f"{label}: expected true or false, got {value!r}")


def median(values: Sequence[Decimal], label: str) -> Decimal:
    if not values:
        raise AcceptanceError(f"{label}: cannot compute an empty median")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / Decimal(2)


def parse_report(path: Path) -> dict[str, str]:
    """Parse unique two-space-indented scalar fields in a dagutil report.

    The report is a deliberately simple YAML-like format.  Restricting this to
    the top-level ``chart_spr_search`` scalars prevents a nested counter with
    the same name from silently winning.
    """

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise AcceptanceError(f"cannot read report_path {path}: {error}") from error

    in_search = False
    values: dict[str, str] = {}
    for line_number, line in enumerate(lines, start=1):
        if line == "chart_spr_search:":
            if in_search:
                raise AcceptanceError(f"{path}:{line_number}: duplicate chart_spr_search section")
            in_search = True
            continue
        if not in_search:
            continue
        if line and not line.startswith(" "):
            break
        match = re.fullmatch(r"  ([A-Za-z0-9_]+):[ \t]*(.*)", line)
        if not match:
            continue
        key, value = match.groups()
        if not value:
            continue
        if key in values:
            raise AcceptanceError(f"{path}:{line_number}: duplicate top-level report key {key}")
        values[key] = value.strip()
    if not in_search:
        raise AcceptanceError(f"{path}: missing chart_spr_search section")
    return values


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
        if not header or any(not field for field in header):
            raise AcceptanceError(f"{path}: empty TSV header field")
        duplicates = sorted({field for field in header if header.count(field) > 1})
        if duplicates:
            raise AcceptanceError(f"{path}: duplicate TSV columns: {', '.join(duplicates)}")
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


def require_canonical_regular_file(path: Path, label: str) -> Path:
    """Require a lexical, non-symlink regular input before opening it."""

    lexical = path.absolute()
    try:
        info = lexical.lstat()
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"{label} is unavailable: {path}: {error}") from error
    if lexical != resolved or stat.S_ISLNK(info.st_mode):
        raise AcceptanceError(
            f"{label} uses a symlink or noncanonical lexical path: {path}"
        )
    if not stat.S_ISREG(info.st_mode):
        raise AcceptanceError(f"{label} is not a regular file: {path}")
    return resolved


def sha256_file(path: Path, label: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while chunk := handle.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise AcceptanceError(f"cannot hash {label} {path}: {error}") from error
    return digest.hexdigest()


def require_object(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise AcceptanceError(f"{label}: expected a JSON object")
    return value


def require_list(value: object, label: str) -> list[object]:
    if not isinstance(value, list):
        raise AcceptanceError(f"{label}: expected a JSON array")
    return value


def require_string(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise AcceptanceError(f"{label}: expected a nonempty JSON string")
    return value


def require_exact_keys(
    value: dict[str, object], expected: set[str], label: str
) -> None:
    missing = sorted(expected - value.keys())
    extra = sorted(value.keys() - expected)
    if missing or extra:
        details: list[str] = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if extra:
            details.append(f"unexpected {', '.join(extra)}")
        raise AcceptanceError(f"{label}: {'; '.join(details)}")


def load_canonical_json(path: Path, label: str) -> dict[str, object]:
    try:
        encoded = path.read_bytes()
        text = encoded.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise AcceptanceError(f"cannot read {label} {path}: {error}") from error

    def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise AcceptanceError(f"{label}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        parsed = json.loads(text, object_pairs_hook=unique_object)
    except (json.JSONDecodeError, AcceptanceError) as error:
        if isinstance(error, AcceptanceError):
            raise
        raise AcceptanceError(f"{label}: malformed JSON: {error}") from error
    result = require_object(parsed, label)
    canonical = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if canonical.encode("utf-8") != encoded:
        raise AcceptanceError(f"{label}: JSON is not in canonical sorted form")
    return result


def receipt_artifact(
    receipt_path: Path,
    reference: object,
    label: str,
    *,
    expected_path: Path | None = None,
) -> Path:
    item = require_object(reference, label)
    require_exact_keys(item, {"path", "sha256"}, label)
    recorded = require_string(item["path"], f"{label}.path")
    expected_hash = require_string(item["sha256"], f"{label}.sha256")
    if not SHA256_RE.fullmatch(expected_hash):
        raise AcceptanceError(f"{label}.sha256 is not a lowercase SHA-256")
    supplied = Path(recorded)
    if not supplied.is_absolute():
        supplied = receipt_path.parent / supplied
    path = require_canonical_regular_file(supplied, label)
    if not Path(recorded).is_absolute():
        try:
            path.relative_to(receipt_path.parent)
        except ValueError as error:
            raise AcceptanceError(f"{label}.path escapes the receipt directory") from error
    if expected_path is not None and path != expected_path:
        raise AcceptanceError(
            f"{label}.path {path} does not match bound input {expected_path}"
        )
    actual_hash = sha256_file(path, label)
    if actual_hash != expected_hash:
        raise AcceptanceError(
            f"{label} SHA-256 {actual_hash} does not match receipt {expected_hash}"
        )
    return path


def load_raw_trials(paths: Sequence[str | os.PathLike[str]]) -> list[RawTrials]:
    """Load one or more tables and establish globally unique row ownership."""

    if not paths:
        raise AcceptanceError("at least one Phase-4 raw trials file is required")
    sources: list[RawTrials] = []
    seen_paths: set[Path] = set()
    owners: dict[tuple[str, str], tuple[Path, int]] = {}
    for supplied in paths:
        path = require_canonical_regular_file(Path(supplied), "raw trials input")
        if path in seen_paths:
            raise AcceptanceError(f"raw trials input is repeated: {path}")
        seen_paths.add(path)
        header, rows = read_tsv(path)
        missing_columns = [column for column in REQUIRED_COLUMNS if column not in header]
        if missing_columns:
            raise AcceptanceError(
                f"{path}: missing required columns: {', '.join(missing_columns)}"
            )
        for row in rows:
            key = (row["row_id"], row["trial_index"])
            line = int(row["__line__"])
            previous = owners.get(key)
            if previous is not None and previous[0] != path:
                previous_path, previous_line = previous
                raise AcceptanceError(
                    "duplicate global row key "
                    f"({key[0]!r}, {key[1]!r}) across raw trials files: "
                    f"{previous_path}:{previous_line} and {path}:{line}"
                )
            owners.setdefault(key, (path, line))
        sources.append(RawTrials(path, tuple(header), tuple(rows)))
    return sources


def resolve_report_path(raw_path: Path, recorded: str) -> Path:
    """Resolve a report only within the raw table's owning directory."""

    root = raw_path.parent
    report = Path(recorded)
    if not report.is_absolute():
        report = root / report
    lexical = report.absolute()
    try:
        info = lexical.lstat()
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"cannot read report_path {report}: {error}") from error
    if lexical != resolved or stat.S_ISLNK(info.st_mode):
        raise AcceptanceError(
            f"report_path uses a symlink or noncanonical lexical path: {report}"
        )
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise AcceptanceError(
            f"report_path escapes owning raw trials directory: {report}"
        ) from error
    if not stat.S_ISREG(info.st_mode):
        raise AcceptanceError(f"report_path is not a regular file: {report}")
    return resolved


def require_report_value(report: dict[str, str], key: str, label: str) -> str:
    if key not in report:
        raise AcceptanceError(f"{label}: report lacks {key}")
    return report[key]


def validate_scheduler(trial: Trial) -> None:
    label = f"{trial.row_id} trial {trial.trial_index}"
    report = trial.report
    worker = trial.worker

    requested = parse_uint(
        require_report_value(report, "chart_workers_requested", label),
        f"{label} chart_workers_requested",
        positive=True,
    )
    resolved = parse_uint(
        require_report_value(report, "chart_workers_resolved", label),
        f"{label} chart_workers_resolved",
        positive=True,
    )
    if requested != worker or resolved != int(trial.row["resolved_workers"]):
        raise AcceptanceError(f"{label}: TSV/report worker counts disagree")
    if resolved != worker:
        raise AcceptanceError(f"{label}: explicit worker request resolved to {resolved}")
    for policy_key in ("chart_worker_policy", "chart_worker_resolution_policy"):
        policy = require_report_value(report, policy_key, label)
        if policy != "explicit":
            raise AcceptanceError(f"{label}: {policy_key}={policy!r}, expected 'explicit'")

    global_names = {
        "operations": "chart_scheduler_operations",
        "parallel": "chart_scheduler_parallel_operations",
        "serial": "chart_scheduler_serial_fallbacks",
        "ranges": "chart_scheduler_ranges_created",
        "completed_ranges": "chart_scheduler_ranges_completed",
        "cancelled_ranges": "chart_scheduler_ranges_cancelled",
        "tasks": "chart_scheduler_tasks_submitted",
        "completed_tasks": "chart_scheduler_tasks_completed",
        "joined_tasks": "chart_scheduler_tasks_joined",
        "wait_samples": "chart_scheduler_queue_wait_samples",
        "wait_ns": "chart_scheduler_queue_wait_nanoseconds",
        "wait_ns_max": "chart_scheduler_queue_wait_nanoseconds_max",
        "active_hwm": "chart_workers_actually_active_high_water",
        "last_active": "chart_scheduler_last_active_workers",
        "pending": "chart_scheduler_pending_tasks",
        "pending_shutdown": "chart_scheduler_pending_tasks_at_shutdown",
        "nested": "chart_scheduler_nested_serial_fallbacks",
        "rejected": "chart_scheduler_rejected_concurrent_operations",
        "pools": "chart_scheduler_pool_lifetimes",
        "stopped_pools": "chart_scheduler_pool_lifetimes_stopped",
        "live_threads": "chart_scheduler_live_pool_threads",
        "minimum_grain": "chart_scheduler_minimum_effective_grain",
        "maximum_grain": "chart_scheduler_maximum_effective_grain",
    }
    metrics = {
        name: parse_uint(
            require_report_value(report, report_name, label),
            f"{label} {report_name}",
        )
        for name, report_name in global_names.items()
    }
    shutdown = parse_bool(
        require_report_value(report, "chart_scheduler_shutdown", label),
        f"{label} chart_scheduler_shutdown",
    )

    if metrics["operations"] != metrics["parallel"] + metrics["serial"]:
        raise AcceptanceError(f"{label}: scheduler operation accounting does not reconcile")
    if metrics["ranges"] != metrics["completed_ranges"] + metrics["cancelled_ranges"]:
        raise AcceptanceError(f"{label}: scheduler range accounting does not reconcile")
    if metrics["cancelled_ranges"] != 0:
        raise AcceptanceError(f"{label}: successful trial reports cancelled scheduler ranges")
    if not (
        metrics["tasks"]
        == metrics["completed_tasks"]
        == metrics["joined_tasks"]
        == metrics["wait_samples"]
    ):
        raise AcceptanceError(f"{label}: scheduler task accounting does not reconcile")
    if metrics["pending"] != 0 or metrics["pending_shutdown"] != 0:
        raise AcceptanceError(f"{label}: scheduler left pending tasks")
    if metrics["live_threads"] != 0 or not shutdown:
        raise AcceptanceError(f"{label}: scheduler did not fully shut down")
    if metrics["nested"] != 0 or metrics["rejected"] != 0:
        raise AcceptanceError(f"{label}: unexpected nested/rejected scheduler operation")
    if metrics["pools"] != metrics["stopped_pools"] or metrics["pools"] > 1:
        raise AcceptanceError(f"{label}: scheduler pool lifetime accounting is invalid")
    if metrics["active_hwm"] < 1 or metrics["active_hwm"] > resolved:
        raise AcceptanceError(f"{label}: invalid scheduler active-worker high-water mark")
    if metrics["last_active"] > metrics["active_hwm"]:
        raise AcceptanceError(f"{label}: last active workers exceeds high-water mark")
    if metrics["wait_ns_max"] > metrics["wait_ns"]:
        raise AcceptanceError(f"{label}: queue wait maximum exceeds total")
    if metrics["wait_samples"] == 0 and (
        metrics["wait_ns"] != 0 or metrics["wait_ns_max"] != 0
    ):
        raise AcceptanceError(f"{label}: queue wait time exists without samples")
    if worker == 1:
        if metrics["parallel"] != 0 or metrics["tasks"] != 0 or metrics["pools"] != 0:
            raise AcceptanceError(f"{label}: one-worker run used a parallel pool")
    elif metrics["parallel"] > 0 and metrics["pools"] != 1:
        raise AcceptanceError(f"{label}: parallel operations lack one persistent pool")

    axis_totals = {field: 0 for field in (*AXIS_FIELDS, "parallel_operations")}
    axis_values: dict[str, dict[str, int]] = {}
    for axis in AXES:
        values: dict[str, int] = {}
        for field in (*AXIS_FIELDS, *AXIS_DIAGNOSTIC_FIELDS):
            key = f"chart_axis_{axis}_{field}"
            values[field] = parse_uint(
                require_report_value(report, key, label), f"{label} {key}"
            )
            if field in axis_totals:
                axis_totals[field] += values[field]
        axis_values[axis] = values
        if values["operations"] == 0:
            if any(
                values[field] != 0
                for field in (*AXIS_FIELDS[1:], *AXIS_DIAGNOSTIC_FIELDS)
            ):
                raise AcceptanceError(f"{label}: idle {axis} axis has nonzero accounting")
        else:
            if values["items"] == 0 or values["ranges"] == 0:
                raise AcceptanceError(f"{label}: active {axis} axis lacks items/ranges")
            if values["ranges"] > values["items"]:
                raise AcceptanceError(f"{label}: {axis} ranges exceed items")
            if values["tasks"] > values["ranges"]:
                raise AcceptanceError(f"{label}: {axis} tasks exceed ranges")
            if not 1 <= values["active_worker_high_water"] <= resolved:
                raise AcceptanceError(f"{label}: invalid {axis} active-worker high-water mark")
            if values["parallel_operations"] > values["operations"]:
                raise AcceptanceError(f"{label}: {axis} parallel operations exceed operations")
            if not (
                1
                <= values["minimum_effective_grain"]
                <= values["maximum_effective_grain"]
            ):
                raise AcceptanceError(f"{label}: invalid {axis} effective-grain extrema")

    if any(axis_values["other"][field] != 0 for field in AXIS_FIELDS):
        raise AcceptanceError(f"{label}: uncategorized scheduler work is nonzero")
    if axis_totals["operations"] != metrics["operations"]:
        raise AcceptanceError(f"{label}: axis/global operation totals do not reconcile")
    if axis_totals["parallel_operations"] != metrics["parallel"]:
        raise AcceptanceError(f"{label}: axis/global parallel-operation totals do not reconcile")
    if axis_totals["ranges"] != metrics["ranges"]:
        raise AcceptanceError(f"{label}: axis/global range totals do not reconcile")
    if axis_totals["tasks"] != metrics["tasks"]:
        raise AcceptanceError(f"{label}: axis/global task totals do not reconcile")
    if max(values["active_worker_high_water"] for values in axis_values.values()) != metrics[
        "active_hwm"
    ]:
        raise AcceptanceError(f"{label}: axis/global active-worker high-water marks disagree")
    nonempty_axis_grains = [
        values for values in axis_values.values() if values["ranges"] != 0
    ]
    axis_minimum_grain = min(
        values["minimum_effective_grain"] for values in nonempty_axis_grains
    )
    axis_maximum_grain = max(
        values["maximum_effective_grain"] for values in nonempty_axis_grains
    )
    if (
        axis_minimum_grain != metrics["minimum_grain"]
        or axis_maximum_grain != metrics["maximum_grain"]
    ):
        raise AcceptanceError(f"{label}: axis/global effective-grain extrema disagree")


def validate_row_common(raw_path: Path, row: dict[str, str], *, scheduler: bool) -> Trial:
    line = int(row["__line__"])
    label = f"{row['row_id']} trial {row['trial_index']}"
    for column in REQUIRED_COLUMNS:
        if column not in row:
            raise AcceptanceError(f"{raw_path}: missing required column {column}")
        if row[column] in ("", "NA", "-"):
            raise AcceptanceError(f"{label}: required column {column} is {row[column]!r}")
    if row["status"] != "ok" or row["validation_status"] != "ok":
        raise AcceptanceError(
            f"{label}: status/validation is {row['status']}/{row['validation_status']}"
        )
    if row["worker_policy"] != "explicit":
        raise AcceptanceError(
            f"{label}: worker_policy={row['worker_policy']!r}, expected 'explicit'"
        )
    expected_literals = {
        "runner_outcome": "exited",
        "runner_exit_code": "0",
        "exit_code": "0",
        "term_signal": "0",
        "core_dumped": "0",
        "timed_out": "0",
        "monitor_error": "0",
        "rss_limit_enabled": "1",
        # ``observed`` means the cap was crossed, not that monitoring ran.
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
    }
    for column, expected in expected_literals.items():
        if row[column] != expected:
            raise AcceptanceError(f"{label}: {column}={row[column]!r}, expected {expected!r}")
    for column in HASH_COLUMNS:
        if not SHA256_RE.fullmatch(row[column]):
            raise AcceptanceError(f"{label}: {column} is not a lowercase SHA-256")
    if row["canonical_digest"] != row["trial_semantic_sha256"]:
        raise AcceptanceError(f"{label}: canonical/trial semantic digests differ")

    parse_uint(row["requested_workers"], f"{label} requested_workers", positive=True)
    parse_uint(row["resolved_workers"], f"{label} resolved_workers", positive=True)
    parse_uint(row["trial_index"], f"{label} trial_index", positive=True)
    parse_decimal(row["wall_clock_s"], f"{label} wall_clock_s", positive=True)
    parse_decimal(row["user_cpu_s"], f"{label} user_cpu_s", positive=True)
    parse_decimal(row["system_cpu_s"], f"{label} system_cpu_s")
    for timing in (
        "local_scoring_ms",
        "initial_chart_construction_ms",
    ):
        parse_decimal(row[timing], f"{label} {timing}")
    peak_rss = parse_uint(row["peak_sampled_rss_kb"], f"{label} peak RSS", positive=True)
    if parse_uint(row["peak_sampled_swap_kb"], f"{label} peak swap") != 0:
        raise AcceptanceError(f"{label}: peak sampled swap is nonzero")
    for limit_name in (
        "process_rss_limit_bytes",
        "configured_chart_memory_budget",
        "manifest_rss_limit_bytes",
    ):
        limit = parse_uint(row[limit_name], f"{label} {limit_name}", positive=True)
        if limit_name != "configured_chart_memory_budget" and peak_rss * 1024 > limit:
            raise AcceptanceError(f"{label}: peak RSS exceeds {limit_name}")

    report_path = resolve_report_path(raw_path, row["report_path"])
    report = parse_report(report_path)
    # The frozen Phase-0 harness exports the historical aggregate and initial
    # timings to TSV.  Phase-4 cache components were added to the product
    # report later, so consume them directly without mutating the frozen
    # measurement harness or silently treating a missing component as zero.
    for timing in ("local_scoring_ms", "initial_chart_construction_ms"):
        report_value = parse_decimal(
            require_report_value(report, timing, label), f"{label} report {timing}"
        )
        raw_value = parse_decimal(row[timing], f"{label} TSV {timing}")
        if report_value != raw_value:
            raise AcceptanceError(f"{label}: TSV/report {timing} values disagree")
    for timing in (
        "local_inside_cache_initialization_ms",
        "local_outside_cache_initialization_ms",
    ):
        parse_decimal(
            require_report_value(report, timing, label), f"{label} report {timing}"
        )
    trial = Trial(
        row=row,
        report=report,
        source_path=raw_path,
        report_path=report_path,
        source_line=line,
    )
    if scheduler:
        validate_scheduler(trial)
    return trial


def select_workload(
    sources: Sequence[RawTrials],
    prefix: str,
    repetitions: int,
    *,
    workers: Sequence[int] = WORKERS,
    scheduler: bool = True,
    allow_other_workers: bool = False,
) -> list[Trial]:
    under_prefix = [
        (source, row)
        for source in sources
        for row in source.rows
        if row["row_id"].startswith(prefix)
    ]
    if not under_prefix:
        paths = ", ".join(os.fspath(source.path) for source in sources)
        raise AcceptanceError(
            f"raw trials inputs ({paths}): no rows match prefix {prefix!r}"
        )

    expected_ids = {f"{prefix}{worker}" for worker in workers}
    unexpected = sorted({row["row_id"] for _, row in under_prefix} - expected_ids)
    if unexpected and not allow_other_workers:
        raise AcceptanceError(
            f"unexpected rows under prefix {prefix!r}: {', '.join(unexpected)}"
        )
    selected = [
        (source, row)
        for source, row in under_prefix
        if row["row_id"] in expected_ids
    ]

    trials: list[Trial] = []
    for worker in workers:
        row_id = f"{prefix}{worker}"
        matching = [
            (source, row) for source, row in selected if row["row_id"] == row_id
        ]
        if len(matching) != repetitions:
            raise AcceptanceError(
                f"{row_id}: expected {repetitions} trials, found {len(matching)}"
            )
        indexes = sorted(
            parse_uint(row["trial_index"], f"{row_id} trial_index")
            for _, row in matching
        )
        if indexes != list(range(1, repetitions + 1)):
            raise AcceptanceError(f"{row_id}: trial indexes are not exactly 1..{repetitions}")
        for source, row in matching:
            if row["requested_workers"] != str(worker):
                raise AcceptanceError(f"{row_id}: requested_workers does not match row ID")
            trials.append(validate_row_common(source.path, row, scheduler=scheduler))

    fixtures = {trial.row["fixture"] for trial in trials}
    methods = {trial.row["method"] for trial in trials}
    if len(fixtures) != 1 or len(methods) != 1:
        raise AcceptanceError(f"{prefix}: fixture/method changed across the worker matrix")
    for hash_name in ("input_sha256", "refseq_sha256"):
        if len({trial.row[hash_name] for trial in trials}) != 1:
            raise AcceptanceError(f"{prefix}: {hash_name} changed across trials")
    semantic_pairs = {
        (trial.row["search_semantic_sha256"], trial.row["output_semantic_sha256"])
        for trial in trials
    }
    if len(semantic_pairs) != 1:
        raise AcceptanceError(f"{prefix}: canonical semantics differ across workers/trials")
    report_paths = [trial.report_path for trial in trials]
    if len(set(report_paths)) != len(report_paths):
        raise AcceptanceError(f"{prefix}: report_path is reused across measured trials")
    for worker in workers:
        worker_trials = by_worker(trials, worker)
        for hash_name in ("canonical_argv_sha256", "trial_semantic_sha256"):
            if len({trial.row[hash_name] for trial in worker_trials}) != 1:
                raise AcceptanceError(
                    f"{prefix}{worker}: {hash_name} changed across repeated trials"
                )
    return trials


def by_worker(trials: Iterable[Trial], worker: int) -> list[Trial]:
    return [trial for trial in trials if trial.worker == worker]


def timing_median(trials: Iterable[Trial], field: str, label: str) -> Decimal:
    return median(
        [parse_decimal(trial.row[field], f"{trial.row_id} {field}") for trial in trials],
        label,
    )


def construction_median(trials: Iterable[Trial], label: str) -> Decimal:
    """Median the disjoint initial-chart plus persistent-cache span."""

    values = [
        parse_decimal(
            trial.row["initial_chart_construction_ms"],
            f"{trial.row_id} initial_chart_construction_ms",
        )
        + parse_decimal(
            trial.report["local_inside_cache_initialization_ms"],
            f"{trial.row_id} report local_inside_cache_initialization_ms",
        )
        + parse_decimal(
            trial.report["local_outside_cache_initialization_ms"],
            f"{trial.row_id} report local_outside_cache_initialization_ms",
        )
        for trial in trials
    ]
    return median(values, label)


def physical_memory_bytes() -> int:
    try:
        with open("/proc/meminfo", "r", encoding="ascii") as handle:
            for line in handle:
                match = re.fullmatch(r"MemTotal:\s+([1-9][0-9]*) kB\s*", line)
                if match:
                    return int(match.group(1)) * 1024
    except OSError as error:
        raise AcceptanceError(f"cannot determine physical memory: {error}") from error
    raise AcceptanceError("cannot determine physical memory from /proc/meminfo")


def gate_ratio(
    gates: list[dict[str, object]],
    name: str,
    numerator: Decimal,
    denominator: Decimal,
    limit: Decimal,
) -> None:
    if denominator <= 0:
        raise AcceptanceError(f"{name}: denominator must be positive")
    ratio = numerator / denominator
    gates.append(
        {
            "name": name,
            "status": "pass" if ratio <= limit else "fail",
            "ratio": str(ratio),
            "limit": str(limit),
            "numerator": str(numerator),
            "denominator": str(denominator),
        }
    )
    if ratio > limit:
        raise AcceptanceError(f"{name}: ratio {ratio} exceeds {limit}")


def contention_violations(
    local: Sequence[Trial], construction: Sequence[Trial]
) -> list[dict[str, object]]:
    raw_hashes = {
        trial.source_path: sha256_file(trial.source_path, "Phase-4 raw trials")
        for trial in (*local, *construction)
    }
    violations: list[dict[str, object]] = []
    for role, trials in (("construction", construction), ("local", local)):
        for trial in trials:
            user = parse_decimal(
                trial.row["user_cpu_s"], f"{trial.row_id} user CPU", positive=True
            )
            system = parse_decimal(
                trial.row["system_cpu_s"], f"{trial.row_id} system CPU"
            )
            if system > user * CONTENTION_LIMIT:
                violations.append(
                    {
                        "raw_trials_sha256": raw_hashes[trial.source_path],
                        "role": role,
                        "row_id": trial.row_id,
                        "system_cpu_s": trial.row["system_cpu_s"],
                        "trial_index": trial.trial_index,
                        "user_cpu_s": trial.row["user_cpu_s"],
                        "worker": trial.worker,
                    }
                )
    return violations


def profile_tsv_rows(
    path: Path, expected_fields: tuple[str, ...], label: str
) -> list[dict[str, str]]:
    header, rows = read_tsv(path)
    if tuple(header) != expected_fields:
        raise AcceptanceError(
            f"{label}: header does not match the contention-profile schema"
        )
    return rows


PROCESS_METRICS_FIELDS = {
    "schema_version",
    "outcome",
    "exit_code",
    "term_signal",
    "timed_out",
    "runner_exit_code",
    "wall_seconds",
    "user_seconds",
    "system_seconds",
    "max_rss_kb",
    "peak_sampled_rss_kb",
    "peak_sampled_swap_kb",
    "rss_kb_unit",
    "proc_status_samples",
    "proc_rss_samples",
    "proc_swap_samples",
    "proc_group_samples",
    "peak_sampled_process_count",
    "subreaper_enabled",
    "descendants_reaped",
    "post_leader_descendants",
    "descendant_cleanup_kill_sent",
    "live_descendants_at_return",
    "process_group_alive_at_return",
    "wait4_echild_at_return",
    "monitor_error",
    "monitor_error_count",
    "wait4_collected",
    "wait_errno",
    "child_error_stage",
    "child_error_errno",
    "core_dumped",
    "timeout_term_sent",
    "timeout_kill_sent",
    "rss_limit_bytes",
    "rss_limit_enabled",
    "rss_limit_observed",
    "rss_limit_exceeded",
    "rss_limit_trigger_bytes",
    "rss_limit_term_sent",
    "rss_limit_kill_sent",
}
PROFILE_PROVENANCE_FIELDS = {
    "schema",
    "schema_version",
    "controller_sha256",
    "product_revision",
    "product_tree",
    "dagutil_sha256",
    "input_sha256",
    "refseq_sha256",
    "process_metrics_sha256",
    "valgrind_sha256",
    "callgrind_annotate_sha256",
    "valgrind_version",
    "kernel",
    "affinity",
    "environment",
    "native_repetitions",
    "native_warmups_per_cell",
    "callgrind_interpretation",
}


def read_key_value_file(
    path: Path, label: str, expected_fields: set[str]
) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise AcceptanceError(f"cannot read {label} {path}: {error}") from error
    values: dict[str, str] = {}
    for line_number, line in enumerate(lines, start=1):
        if "=" not in line:
            raise AcceptanceError(f"{label}:{line_number}: malformed metric")
        key, value = line.split("=", 1)
        if not key or not value:
            raise AcceptanceError(f"{label}:{line_number}: empty metric key/value")
        if key in values:
            raise AcceptanceError(f"{label}:{line_number}: duplicate metric {key}")
        values[key] = value
    require_exact_keys(values, expected_fields, label)
    return values


def read_process_metrics(path: Path, label: str) -> dict[str, str]:
    return read_key_value_file(path, label, PROCESS_METRICS_FIELDS)


def validate_profile_process_metrics(
    metrics: dict[str, str], row: dict[str, str], label: str
) -> None:
    for metric, row_field in (
        ("user_seconds", "user_s"),
        ("system_seconds", "system_s"),
        ("wall_seconds", "wall_s"),
        ("peak_sampled_rss_kb", "peak_sampled_rss_kb"),
    ):
        if metrics[metric] != row[row_field]:
            raise AcceptanceError(
                f"{label}: {metric} does not exactly match the timing TSV"
            )
    expected = {
        "schema_version": "2",
        "outcome": "exited",
        "exit_code": "0",
        "term_signal": "0",
        "timed_out": "0",
        "runner_exit_code": "0",
        "peak_sampled_swap_kb": "0",
        "rss_kb_unit": "1024_bytes",
        "peak_sampled_process_count": "1",
        "subreaper_enabled": "1",
        "descendants_reaped": "0",
        "post_leader_descendants": "0",
        "descendant_cleanup_kill_sent": "0",
        "live_descendants_at_return": "0",
        "process_group_alive_at_return": "0",
        "wait4_echild_at_return": "1",
        "monitor_error": "0",
        "monitor_error_count": "0",
        "wait4_collected": "1",
        "wait_errno": "0",
        "child_error_stage": "none",
        "child_error_errno": "0",
        "core_dumped": "0",
        "timeout_term_sent": "0",
        "timeout_kill_sent": "0",
        "rss_limit_bytes": "17179869184",
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
    }
    for metric, expected_value in expected.items():
        if metrics[metric] != expected_value:
            raise AcceptanceError(
                f"{label}: {metric}={metrics[metric]!r}, expected {expected_value!r}"
            )
    parse_uint(metrics["max_rss_kb"], f"{label} max_rss_kb", positive=True)
    parse_uint(
        metrics["peak_sampled_rss_kb"],
        f"{label} peak_sampled_rss_kb",
        positive=True,
    )
    status_samples = parse_uint(
        metrics["proc_status_samples"], f"{label} proc_status_samples", positive=True
    )
    rss_samples = parse_uint(
        metrics["proc_rss_samples"], f"{label} proc_rss_samples", positive=True
    )
    swap_samples = parse_uint(
        metrics["proc_swap_samples"], f"{label} proc_swap_samples", positive=True
    )
    group_samples = parse_uint(
        metrics["proc_group_samples"], f"{label} proc_group_samples", positive=True
    )
    if not (
        rss_samples == swap_samples
        and status_samples - rss_samples in (0, 1)
        and group_samples - status_samples in (0, 1)
    ):
        raise AcceptanceError(f"{label}: process-monitor sample counts do not reconcile")


def profile_group_summary(
    rows: Sequence[dict[str, str]], groups: Sequence[tuple[str, int]], label: str
) -> tuple[list[dict[str, object]], int]:
    expected = set(groups)
    actual: dict[tuple[str, int], list[dict[str, str]]] = {}
    violations = 0
    for row in rows:
        role = row["role"]
        worker = parse_uint(row["worker"], f"{label} worker", positive=True)
        group = (role, worker)
        if group not in expected:
            raise AcceptanceError(f"{label}: unexpected group {role}/W{worker}")
        actual.setdefault(group, []).append(row)
        user = parse_decimal(row["user_s"], f"{label} user_s", positive=True)
        system = parse_decimal(row["system_s"], f"{label} system_s")
        if system > user * CONTENTION_LIMIT:
            violations += 1
    if set(actual) != expected:
        raise AcceptanceError(f"{label}: incomplete workload/worker group matrix")

    summaries: list[dict[str, object]] = []
    for role, worker in groups:
        group_rows = actual[(role, worker)]
        repetitions = sorted(
            parse_uint(row["repetition"], f"{label} repetition", positive=True)
            for row in group_rows
        )
        if repetitions != list(range(1, 11)):
            raise AcceptanceError(
                f"{label}: {role}/W{worker} repetitions are not exactly 1..10"
            )
        group_violations = sum(
            parse_decimal(row["system_s"], f"{label} system_s")
            > parse_decimal(row["user_s"], f"{label} user_s", positive=True)
            * CONTENTION_LIMIT
            for row in group_rows
        )
        summaries.append(
            {
                "role": role,
                "rows": 10,
                "violations": group_violations,
                "worker": worker,
            }
        )
    return summaries, violations


def validate_contention_profile(
    receipt_path: Path, receipt: dict[str, object]
) -> None:
    profile = require_object(receipt["profile"], "contention receipt profile")
    require_exact_keys(
        profile,
        {
            "affinity",
            "callgrind",
            "completion_marker",
            "devnull_and_fixed_controls",
            "environment",
            "host_preflight",
            "native_rusage",
            "process_metrics_sha256",
            "provenance",
            "runner",
        },
        "contention receipt profile",
    )
    if profile["affinity"] != "0,2,4,6,8,10,12,14":
        raise AcceptanceError(
            "contention receipt profile.affinity does not match the frozen CPU set"
        )
    environment = require_list(
        profile["environment"], "contention receipt profile.environment"
    )
    if environment != [
        "HOME=/nonexistent",
        "LANG=C",
        "LC_ALL=C",
        "PATH=/usr/bin:/bin",
        "TMPDIR=/tmp",
        "TZ=Europe/Sofia",
    ]:
        raise AcceptanceError(
            "contention receipt profile.environment does not match the frozen environment"
        )
    process_metrics_hash = require_string(
        profile["process_metrics_sha256"],
        "contention receipt profile.process_metrics_sha256",
    )
    if not SHA256_RE.fullmatch(process_metrics_hash):
        raise AcceptanceError(
            "contention receipt profile.process_metrics_sha256 is not a lowercase SHA-256"
        )

    completion_marker = receipt_artifact(
        receipt_path,
        profile["completion_marker"],
        "contention receipt profile.completion_marker",
    )
    try:
        marker_bytes = completion_marker.read_bytes()
    except OSError as error:
        raise AcceptanceError(
            f"cannot read contention profile completion marker: {error}"
        ) from error
    if marker_bytes != b"complete\n":
        raise AcceptanceError(
            "contention receipt profile.completion_marker is not exactly 'complete\\n'"
        )
    provenance_path = receipt_artifact(
        receipt_path,
        profile["provenance"],
        "contention receipt profile.provenance",
    )
    runner = require_object(profile["runner"], "contention receipt profile.runner")
    require_exact_keys(
        runner, {"kind", "path", "sha256"}, "contention receipt profile.runner"
    )
    if runner["kind"] != "preserved_script":
        raise AcceptanceError(
            "contention receipt profile.runner.kind must be 'preserved_script'"
        )
    receipt_artifact(
        receipt_path,
        {"path": runner["path"], "sha256": runner["sha256"]},
        "contention receipt profile.runner",
    )
    provenance = read_key_value_file(
        provenance_path,
        "contention receipt profile.provenance",
        PROFILE_PROVENANCE_FIELDS,
    )
    product = require_object(receipt["product"], "contention receipt product")
    callgrind = require_object(
        profile["callgrind"], "contention receipt profile.callgrind"
    )
    profiler = require_object(
        callgrind.get("profiler"), "contention receipt profile.callgrind.profiler"
    )
    expected_provenance = {
        "schema": "wric.phase4.contention_profile_provenance",
        "schema_version": "1",
        "controller_sha256": require_string(
            runner.get("sha256"), "contention receipt profile.runner.sha256"
        ),
        "product_revision": require_string(
            product.get("revision"), "contention receipt product.revision"
        ),
        "product_tree": require_string(
            product.get("tree"), "contention receipt product.tree"
        ),
        "dagutil_sha256": require_string(
            require_object(
                product.get("dagutil"), "contention receipt product.dagutil"
            ).get("sha256"),
            "contention receipt product.dagutil.sha256",
        ),
        "input_sha256": require_string(
            require_object(
                product.get("input"), "contention receipt product.input"
            ).get("sha256"),
            "contention receipt product.input.sha256",
        ),
        "refseq_sha256": require_string(
            require_object(
                product.get("refseq"), "contention receipt product.refseq"
            ).get("sha256"),
            "contention receipt product.refseq.sha256",
        ),
        "process_metrics_sha256": process_metrics_hash,
        "valgrind_sha256": require_string(
            profiler.get("binary_sha256"),
            "contention receipt profile.callgrind.profiler.binary_sha256",
        ),
        "callgrind_annotate_sha256": "8716f89225e5c49615788f9d04779a92fb20928626ba13cf25045697d40e5e4d",
        "valgrind_version": require_string(
            profiler.get("version"),
            "contention receipt profile.callgrind.profiler.version",
        ),
        "affinity": require_string(
            profile.get("affinity"), "contention receipt profile.affinity"
        ),
        "environment": " ".join(
            require_string(value, "contention receipt profile.environment value")
            for value in environment
        ),
        "native_repetitions": "10",
        "native_warmups_per_cell": "1",
        "callgrind_interpretation": require_string(
            callgrind.get("interpretation"),
            "contention receipt profile.callgrind.interpretation",
        ),
    }
    for key, expected_value in expected_provenance.items():
        if provenance[key] != expected_value:
            raise AcceptanceError(
                f"contention receipt profile.provenance {key} does not match receipt"
            )
    require_string(
        provenance["kernel"], "contention receipt profile.provenance kernel"
    )

    host = require_object(
        profile["host_preflight"], "contention receipt profile.host_preflight"
    )
    require_exact_keys(
        host,
        {"processes_after", "processes_before", "samples", "times"},
        "contention receipt profile.host_preflight",
    )
    if host["samples"] != 15:
        raise AcceptanceError(
            "contention receipt profile.host_preflight.samples must equal 15"
        )
    for field in ("processes_after", "processes_before"):
        receipt_artifact(
            receipt_path,
            host[field],
            f"contention receipt profile.host_preflight.{field}",
        )
    preflight_path = receipt_artifact(
        receipt_path,
        host["times"],
        "contention receipt profile.host_preflight.times",
    )
    preflight_rows = profile_tsv_rows(
        preflight_path,
        (
            "sample",
            "epoch",
            "load1",
            "runnable_processes",
            "total_processes",
        ),
        "contention receipt profile.host_preflight.times",
    )
    if len(preflight_rows) != 15:
        raise AcceptanceError(
            "contention receipt profile.host_preflight.times must contain 15 rows"
        )
    samples = [
        parse_uint(
            row["sample"], "contention receipt host preflight sample", positive=True
        )
        for row in preflight_rows
    ]
    if samples != list(range(1, 16)):
        raise AcceptanceError(
            "contention receipt host preflight samples are not exactly 1..15"
        )
    epochs = [
        parse_decimal(
            row["epoch"], "contention receipt host preflight epoch", positive=True
        )
        for row in preflight_rows
    ]
    for previous, current in zip(epochs, epochs[1:]):
        interval = current - previous
        if interval < Decimal("0.9") or interval > Decimal("1.1"):
            raise AcceptanceError(
                "contention receipt host preflight intervals are not one-second samples"
            )
    for row in preflight_rows:
        load = parse_decimal(
            row["load1"], "contention receipt host preflight load1"
        )
        runnable = parse_uint(
            row["runnable_processes"],
            "contention receipt host preflight runnable_processes",
            positive=True,
        )
        total = parse_uint(
            row["total_processes"],
            "contention receipt host preflight total_processes",
            positive=True,
        )
        if load > Decimal("1.0") or runnable > 2 or total < runnable:
            raise AcceptanceError(
                "contention receipt host preflight is not quiescent"
            )

    timing_fields = (
        "role",
        "worker",
        "repetition",
        "user_s",
        "system_s",
        "wall_s",
        "peak_sampled_rss_kb",
        "semantic_sha256",
        "canonical_sha256",
        "metrics_sha256",
        "stdout_sha256",
        "stderr_sha256",
        "output_mode",
        "output_sha256",
    )
    timing_specs = (
        (
            "native_rusage",
            80,
            (
                ("cache", 1),
                ("cache", 2),
                ("cache", 4),
                ("cache", 8),
                ("dense", 1),
                ("dense", 2),
                ("dense", 4),
                ("dense", 8),
            ),
        ),
        (
            "devnull_and_fixed_controls",
            50,
            (
                ("cache", 1),
                ("cache", 8),
                ("dense", 1),
                ("dense", 8),
                ("fixed", 1),
            ),
        ),
    )
    workloads = {
        require_string(
            require_object(item, "contention receipt workload").get("role"),
            "contention receipt workload.role",
        ): require_object(item, "contention receipt workload")
        for item in require_list(receipt["workloads"], "contention receipt workloads")
    }
    if set(workloads) != {"construction", "local"}:
        raise AcceptanceError("contention receipt workload roles are invalid")
    output_semantics = {
        require_string(
            workload.get("output_semantic_sha256"),
            "contention receipt workload.output_semantic_sha256",
        )
        for workload in workloads.values()
    }
    if len(output_semantics) != 1:
        raise AcceptanceError(
            "contention receipt workload output semantics disagree"
        )
    profile_semantics = {
        "cache": require_string(
            workloads["construction"].get("search_semantic_sha256"),
            "construction search semantic",
        ),
        "dense": require_string(
            workloads["local"].get("search_semantic_sha256"),
            "local search semantic",
        ),
        "fixed": next(iter(output_semantics)),
    }
    callgrind = require_object(
        profile["callgrind"], "contention receipt profile.callgrind"
    )
    receipt_runs = require_list(
        callgrind.get("runs"), "contention receipt profile.callgrind.runs"
    )
    profile_canonicals: dict[str, str] = {}
    for index, value in enumerate(receipt_runs):
        run = require_object(
            value, f"contention receipt profile.callgrind.runs[{index}]"
        )
        role = require_string(
            run.get("role"), f"contention receipt profile.callgrind.runs[{index}].role"
        )
        canonical = require_string(
            run.get("canonical_sha256"),
            f"contention receipt profile.callgrind.runs[{index}].canonical_sha256",
        )
        if (
            role not in profile_semantics
            or role in profile_canonicals
            or not SHA256_RE.fullmatch(canonical)
            or run.get("semantic_sha256") != profile_semantics[role]
        ):
            raise AcceptanceError(
                "contention receipt callgrind run identities do not match workloads"
            )
        profile_canonicals[role] = canonical
    if set(profile_canonicals) != {"cache", "dense", "fixed"}:
        raise AcceptanceError(
            "contention receipt callgrind run identities are incomplete"
        )

    for field, expected_rows, groups in timing_specs:
        section_label = f"contention receipt profile.{field}"
        section = require_object(profile[field], section_label)
        require_exact_keys(
            section,
            {
                "artifact",
                "group_violations",
                "measured_repetitions_per_cell",
                "rows",
                "violations",
                "warmups_per_cell",
            },
            section_label,
        )
        if (
            section["rows"] != expected_rows
            or section["measured_repetitions_per_cell"] != 10
            or section["warmups_per_cell"] != 1
        ):
            raise AcceptanceError(
                f"{section_label}: expected {expected_rows} rows, 10 measured repetitions, and 1 warmup"
            )
        artifact_path = receipt_artifact(
            receipt_path, section["artifact"], f"{section_label}.artifact"
        )
        rows = profile_tsv_rows(artifact_path, timing_fields, f"{section_label}.artifact")
        if len(rows) != expected_rows:
            raise AcceptanceError(
                f"{section_label}.artifact: expected {expected_rows} rows, found {len(rows)}"
            )
        artifact_root = artifact_path.parent
        for row in rows:
            role = row["role"]
            if role not in profile_semantics:
                raise AcceptanceError(
                    f"{section_label}.artifact: unexpected role {role!r}"
                )
            worker = parse_uint(
                row["worker"], f"{section_label}.artifact worker", positive=True
            )
            repetition = parse_uint(
                row["repetition"],
                f"{section_label}.artifact repetition",
                positive=True,
            )
            parse_decimal(
                row["wall_s"], f"{section_label}.artifact wall_s", positive=True
            )
            parse_uint(
                row["peak_sampled_rss_kb"],
                f"{section_label}.artifact peak_sampled_rss_kb",
                positive=True,
            )
            for hash_field in (
                "semantic_sha256",
                "canonical_sha256",
                "metrics_sha256",
                "stdout_sha256",
                "stderr_sha256",
            ):
                if not SHA256_RE.fullmatch(row[hash_field]):
                    raise AcceptanceError(
                        f"{section_label}.artifact: {hash_field} is not a lowercase SHA-256"
                    )
            if (
                row["semantic_sha256"] != profile_semantics[role]
                or row["canonical_sha256"] != profile_canonicals[role]
            ):
                raise AcceptanceError(
                    f"{section_label}.artifact: {role}/W{worker} semantic or canonical identity is stale"
                )
            stem = f"{role}-w{worker}-r{repetition}"
            per_run_paths: dict[str, Path] = {}
            for suffix, hash_field_name in (
                ("canonical.json", "canonical_sha256"),
                ("metrics", "metrics_sha256"),
                ("stdout", "stdout_sha256"),
                ("stderr", "stderr_sha256"),
            ):
                per_run_path = require_canonical_regular_file(
                    artifact_root / f"{stem}.{suffix}",
                    f"{section_label}.artifact {stem}.{suffix}",
                )
                if sha256_file(per_run_path, f"{section_label} per-run artifact") != row[
                    hash_field_name
                ]:
                    raise AcceptanceError(
                        f"{section_label}.artifact: {stem}.{suffix} SHA-256 mismatch"
                    )
                per_run_paths[suffix] = per_run_path
            metrics = read_process_metrics(
                per_run_paths["metrics"],
                f"{section_label}.artifact {stem}.metrics",
            )
            validate_profile_process_metrics(
                metrics,
                row,
                f"{section_label}.artifact {stem}.metrics",
            )
            if field == "native_rusage":
                if (
                    row["output_mode"] != "file"
                    or not SHA256_RE.fullmatch(row["output_sha256"])
                ):
                    raise AcceptanceError(
                        f"{section_label}.artifact: {stem} output identity is invalid"
                    )
                output_path = require_canonical_regular_file(
                    artifact_root / f"{stem}.pb.gz",
                    f"{section_label}.artifact {stem}.pb.gz",
                )
                if sha256_file(output_path, f"{section_label} output") != row[
                    "output_sha256"
                ]:
                    raise AcceptanceError(
                        f"{section_label}.artifact: {stem}.pb.gz SHA-256 mismatch"
                    )
            elif (
                row["output_mode"]
                != ("no_output" if role == "fixed" else "devnull")
                or row["output_sha256"] != "-"
            ):
                raise AcceptanceError(
                    f"{section_label}.artifact: {stem} output mode is invalid"
                )
        summaries, violations = profile_group_summary(rows, groups, section_label)
        if section["group_violations"] != summaries:
            raise AcceptanceError(
                f"{section_label}.group_violations does not match the bound artifact"
            )
        if section["violations"] != violations:
            raise AcceptanceError(
                f"{section_label}.violations does not match the bound artifact"
            )
        if field == "devnull_and_fixed_controls":
            fixed = next(
                summary
                for summary in summaries
                if summary["role"] == "fixed" and summary["worker"] == 1
            )
            if fixed["violations"] != 8:
                raise AcceptanceError(
                    f"{section_label}: fixed W1 control must breach in exactly 8/10 trials"
                )
        exact_violations = 38 if field == "native_rusage" else 25
        if violations != exact_violations:
            raise AcceptanceError(
                f"{section_label}: expected exactly {exact_violations} threshold violations"
            )

    callgrind_label = "contention receipt profile.callgrind"
    require_exact_keys(
        callgrind,
        {"artifact", "interpretation", "profiler", "runs"},
        callgrind_label,
    )
    if callgrind["interpretation"] != "qualitative_only":
        raise AcceptanceError(
            f"{callgrind_label}.interpretation must be 'qualitative_only'"
        )
    profiler = require_object(callgrind["profiler"], f"{callgrind_label}.profiler")
    require_exact_keys(
        profiler,
        {"binary_sha256", "kind", "version"},
        f"{callgrind_label}.profiler",
    )
    if profiler != {
        "binary_sha256": "9e8422466bd87902983118bb7bc51e67679b0bbaa09788f7d6b2aa0af40406b3",
        "kind": "callgrind_collect_systime_nsec",
        "version": "valgrind-3.27.1",
    }:
        raise AcceptanceError(f"{callgrind_label}.profiler identity is invalid")
    summary_path = receipt_artifact(
        receipt_path, callgrind["artifact"], f"{callgrind_label}.artifact"
    )
    summary_fields = (
        "role",
        "worker",
        "semantic_sha256",
        "canonical_sha256",
        "output_sha256",
        "stdout_sha256",
        "stderr_sha256",
        "profile_sha256",
        "log_sha256",
        "annotation_sha256",
        "instructions",
        "syscall_count",
        "system_time_ns",
        "system_cpu_time_ns",
    )
    summary = profile_tsv_rows(summary_path, summary_fields, f"{callgrind_label}.artifact")
    if len(summary) != 3:
        raise AcceptanceError(f"{callgrind_label}.artifact: expected 3 rows")
    expected_groups = {("dense", 8), ("cache", 8), ("fixed", 1)}
    if {
        (row["role"], parse_uint(row["worker"], f"{callgrind_label} worker", positive=True))
        for row in summary
    } != expected_groups:
        raise AcceptanceError(f"{callgrind_label}.artifact: invalid run matrix")
    expected_runs = []
    for row in summary:
        role = row["role"]
        worker = parse_uint(row["worker"], f"{callgrind_label} worker", positive=True)
        for hash_field in ("semantic_sha256", "canonical_sha256", "profile_sha256"):
            if not SHA256_RE.fullmatch(row[hash_field]):
                raise AcceptanceError(
                    f"{callgrind_label}.artifact: {hash_field} is not a lowercase SHA-256"
                )
        profile_path = require_canonical_regular_file(
            receipt_path.parent / "callgrind" / f"{role}-w{worker}.callgrind",
            f"{callgrind_label} {role}/W{worker} profile",
        )
        if sha256_file(profile_path, f"{callgrind_label} profile") != row[
            "profile_sha256"
        ]:
            raise AcceptanceError(
                f"{callgrind_label}: {role}/W{worker} profile SHA-256 mismatch"
            )
        expected_runs.append(
            {
                "canonical_sha256": row["canonical_sha256"],
                "profile_sha256": row["profile_sha256"],
                "role": role,
                "semantic_sha256": row["semantic_sha256"],
                "syscall_count": parse_uint(
                    row["syscall_count"], f"{callgrind_label} syscall_count"
                ),
                "system_cpu_time_ns": parse_uint(
                    row["system_cpu_time_ns"], f"{callgrind_label} system_cpu_time_ns"
                ),
                "system_time_ns": parse_uint(
                    row["system_time_ns"], f"{callgrind_label} system_time_ns"
                ),
                "worker": worker,
            }
        )
    if callgrind["runs"] != expected_runs:
        raise AcceptanceError(f"{callgrind_label}.runs does not match the bound artifact")


def validate_contention_receipt(
    args: argparse.Namespace,
    local: Sequence[Trial],
    construction: Sequence[Trial],
    violations: list[dict[str, object]],
) -> tuple[Path, str]:
    receipt_path = require_canonical_regular_file(
        Path(args.contention_investigation_receipt),
        "contention investigation receipt",
    )
    expected_receipt_hash = args.expected_contention_investigation_receipt_sha256
    if not SHA256_RE.fullmatch(expected_receipt_hash):
        raise AcceptanceError(
            "expected contention investigation receipt SHA-256 is invalid"
        )
    actual_receipt_hash = sha256_file(
        receipt_path, "contention investigation receipt"
    )
    if actual_receipt_hash != expected_receipt_hash:
        raise AcceptanceError(
            "contention investigation receipt SHA-256 "
            f"{actual_receipt_hash} does not match expected {expected_receipt_hash}"
        )
    receipt = load_canonical_json(receipt_path, "contention investigation receipt")
    require_exact_keys(
        receipt,
        {
            "capture",
            "disposition",
            "finding",
            "product",
            "profile",
            "receipt_builder",
            "schema",
            "schema_version",
            "status",
            "threshold",
            "violations",
            "workloads",
        },
        "contention investigation receipt",
    )
    if (
        receipt["schema"] != "wric.phase4.contention_investigation"
        or receipt["schema_version"] != 1
        or receipt["status"] != "complete"
    ):
        raise AcceptanceError("contention investigation receipt schema/status is invalid")
    threshold = require_object(receipt["threshold"], "contention receipt threshold")
    require_exact_keys(
        threshold, {"comparison", "limit", "metric"}, "contention receipt threshold"
    )
    if threshold != {
        "comparison": "greater_than",
        "limit": "0.25",
        "metric": "system_cpu_s_over_user_cpu_s",
    }:
        raise AcceptanceError("contention investigation receipt threshold is invalid")
    if receipt["violations"] != violations:
        raise AcceptanceError(
            "contention investigation receipt violation set does not exactly match raw trials"
        )

    capture = require_object(receipt["capture"], "contention receipt capture")
    require_exact_keys(
        capture,
        {"phase3_baseline", "raw_trials", "repetitions", "row_prefixes"},
        "contention receipt capture",
    )
    if capture["repetitions"] != args.repetitions or capture["row_prefixes"] != {
        "construction": args.construction_row_prefix,
        "local": args.local_row_prefix,
    }:
        raise AcceptanceError(
            "contention investigation receipt capture matrix does not match invocation"
        )
    raw_bindings = require_list(
        capture["raw_trials"], "contention receipt capture.raw_trials"
    )
    if len(raw_bindings) != 2:
        raise AcceptanceError(
            "contention receipt capture.raw_trials must bind local and construction"
        )
    expected_sources = {
        "local": {trial.source_path for trial in local},
        "construction": {trial.source_path for trial in construction},
    }
    if any(len(paths) != 1 for paths in expected_sources.values()):
        raise AcceptanceError(
            "contention receipt requires one raw-trials owner per workload matrix"
        )
    seen_roles: set[str] = set()
    for index, value in enumerate(raw_bindings):
        label = f"contention receipt capture.raw_trials[{index}]"
        binding = require_object(value, label)
        require_exact_keys(
            binding,
            {"metadata", "path", "role", "run_ledger", "sha256"},
            label,
        )
        role = require_string(binding["role"], f"{label}.role")
        if role not in expected_sources or role in seen_roles:
            raise AcceptanceError(f"{label}.role is invalid or repeated")
        seen_roles.add(role)
        expected_path = next(iter(expected_sources[role]))
        receipt_artifact(
            receipt_path,
            {"path": binding["path"], "sha256": binding["sha256"]},
            label,
            expected_path=expected_path,
        )
        receipt_artifact(receipt_path, binding["metadata"], f"{label}.metadata")
        receipt_artifact(receipt_path, binding["run_ledger"], f"{label}.run_ledger")
    if seen_roles != {"local", "construction"}:
        raise AcceptanceError("contention receipt raw-trials roles are incomplete")

    if not args.phase3_raw_trials:
        raise AcceptanceError(
            "contention investigation receipt requires a bound Phase-3 baseline"
        )
    phase3_path = require_canonical_regular_file(
        Path(args.phase3_raw_trials), "Phase-3 raw trials input"
    )
    phase3 = require_object(
        capture["phase3_baseline"], "contention receipt capture.phase3_baseline"
    )
    require_exact_keys(
        phase3,
        {"metadata", "path", "run_ledger", "sha256"},
        "contention receipt capture.phase3_baseline",
    )
    receipt_artifact(
        receipt_path,
        {"path": phase3["path"], "sha256": phase3["sha256"]},
        "contention receipt capture.phase3_baseline",
        expected_path=phase3_path,
    )
    receipt_artifact(
        receipt_path,
        phase3["metadata"],
        "contention receipt capture.phase3_baseline.metadata",
    )
    receipt_artifact(
        receipt_path,
        phase3["run_ledger"],
        "contention receipt capture.phase3_baseline.run_ledger",
    )

    expected_workloads: list[dict[str, object]] = []
    for role, matrix in (("construction", construction), ("local", local)):
        expected_workloads.append(
            {
                "fixture": matrix[0].row["fixture"],
                "input_sha256": matrix[0].row["input_sha256"],
                "method": matrix[0].row["method"],
                "output_semantic_sha256": matrix[0].row["output_semantic_sha256"],
                "refseq_sha256": matrix[0].row["refseq_sha256"],
                "role": role,
                "search_semantic_sha256": matrix[0].row["search_semantic_sha256"],
                "workers": [
                    {
                        "canonical_argv_sha256": by_worker(matrix, worker)[0].row[
                            "canonical_argv_sha256"
                        ],
                        "row_id": f"{args.construction_row_prefix if role == 'construction' else args.local_row_prefix}{worker}",
                        "worker": worker,
                    }
                    for worker in WORKERS
                ],
            }
        )
    if receipt["workloads"] != expected_workloads:
        raise AcceptanceError(
            "contention investigation receipt workload/argv identity does not match raw trials"
        )

    product = require_object(receipt["product"], "contention receipt product")
    require_exact_keys(
        product,
        {"dagutil", "input", "process_metrics", "refseq", "revision", "tree"},
        "contention receipt product",
    )
    if product["revision"] != args.expected_contention_product_revision:
        raise AcceptanceError(
            "contention investigation receipt product revision does not match expected"
        )
    if not REVISION_RE.fullmatch(require_string(product["tree"], "product.tree")):
        raise AcceptanceError("contention investigation receipt product tree is invalid")
    dagutil = require_object(product["dagutil"], "contention receipt product.dagutil")
    if dagutil.get("sha256") != args.expected_contention_dagutil_sha256:
        raise AcceptanceError(
            "contention investigation receipt dagutil SHA-256 does not match expected"
        )
    receipt_artifact(
        receipt_path, dagutil, "contention receipt product.dagutil"
    )
    receipt_artifact(receipt_path, product["input"], "contention receipt product.input")
    receipt_artifact(receipt_path, product["refseq"], "contention receipt product.refseq")
    receipt_artifact(
        receipt_path,
        product["process_metrics"],
        "contention receipt product.process_metrics",
    )
    input_hash = local[0].row["input_sha256"]
    refseq_hash = local[0].row["refseq_sha256"]
    if (
        require_object(product["input"], "product.input").get("sha256") != input_hash
        or require_object(product["refseq"], "product.refseq").get("sha256")
        != refseq_hash
    ):
        raise AcceptanceError(
            "contention investigation receipt product input/refseq identity is stale"
        )
    process_metrics = require_object(
        product["process_metrics"], "contention receipt product.process_metrics"
    )
    profile = require_object(receipt["profile"], "contention receipt profile")
    if profile.get("process_metrics_sha256") != process_metrics.get("sha256"):
        raise AcceptanceError(
            "contention investigation receipt process-metrics identities disagree"
        )

    finding = require_object(receipt["finding"], "contention receipt finding")
    require_exact_keys(
        finding,
        {
            "acceptance_timing_source",
            "alternate_timing_waiver",
            "classification",
            "diagnostic_timings_used_for_acceptance",
            "rationale",
            "rollback_review",
        },
        "contention receipt finding",
    )
    if (
        finding["acceptance_timing_source"] != "bound_raw_trials_only"
        or finding["alternate_timing_waiver"] is not False
        or finding["diagnostic_timings_used_for_acceptance"] is not False
        or finding["rollback_review"] != "completed"
        or finding["classification"]
        != "short_run_fixed_overhead_and_parallel_contention_not_scaling_blocker"
    ):
        raise AcceptanceError("contention investigation receipt finding is invalid")
    receipt_artifact(
        receipt_path, finding["rationale"], "contention receipt finding.rationale"
    )
    receipt_artifact(
        receipt_path, receipt["receipt_builder"], "contention receipt receipt_builder"
    )
    validate_contention_profile(receipt_path, receipt)

    disposition = require_string(
        receipt["disposition"], "contention receipt disposition"
    )
    if disposition in ("inconclusive", "rollback_required"):
        raise ProfilingRequired(
            f"contention investigation disposition is {disposition}"
        )
    if disposition != "no_rollback_required_for_bound_capture":
        raise AcceptanceError(
            f"contention investigation receipt disposition {disposition!r} is invalid"
        )
    return receipt_path, actual_receipt_hash


def evaluate(args: argparse.Namespace) -> dict[str, object]:
    supplied_raw_trials = args.raw_trials
    if isinstance(supplied_raw_trials, (str, os.PathLike)):
        supplied_raw_trials = [supplied_raw_trials]
    raw_sources = load_raw_trials(supplied_raw_trials)
    local = select_workload(
        raw_sources, args.local_row_prefix, args.repetitions
    )
    construction = select_workload(
        raw_sources, args.construction_row_prefix, args.repetitions
    )

    # Ensure the two matrices did not accidentally select the same rows.
    local_keys = {(trial.row_id, trial.trial_index) for trial in local}
    construction_keys = {(trial.row_id, trial.trial_index) for trial in construction}
    if local_keys & construction_keys:
        raise AcceptanceError("local and construction workload selectors overlap")
    # `fixture` is the frozen workload label, so the local-scoring and
    # construction variants intentionally use different values even though
    # they consume the same underlying fixture.  Bind that shared identity by
    # method and immutable input/refseq hashes; select_workload has already
    # required one internally consistent fixture label within each matrix.
    for identity_field in ("method", "input_sha256", "refseq_sha256"):
        local_identity = {trial.row[identity_field] for trial in local}
        construction_identity = {trial.row[identity_field] for trial in construction}
        if local_identity != construction_identity:
            raise AcceptanceError(
                f"local/construction matrices differ in {identity_field}"
            )

    violations = contention_violations(local, construction)
    receipt_supplied = args.contention_investigation_receipt is not None
    if not violations and receipt_supplied:
        raise AcceptanceError(
            "contention investigation receipt is forbidden when no system/user CPU violations exist"
        )
    if violations and not receipt_supplied:
        first = violations[0]
        user = parse_decimal(str(first["user_cpu_s"]), "violating user CPU", positive=True)
        system = parse_decimal(str(first["system_cpu_s"]), "violating system CPU")
        raise ProfilingRequired(
            f"{first['row_id']} trial {first['trial_index']}: system/user CPU ratio "
            f"{system / user} exceeds 0.25"
        )

    contention_gate: dict[str, object]
    if violations:
        receipt_path, receipt_hash = validate_contention_receipt(
            args, local, construction, violations
        )
        contention_gate = {
            "name": "system_over_user_cpu",
            "status": "investigated",
            "limit": "0.25",
            "violations": len(violations),
            "disposition": "no_rollback_required_for_bound_capture",
            "receipt": os.fspath(receipt_path),
            "receipt_sha256": receipt_hash,
        }
    else:
        contention_gate = {
            "name": "system_over_user_cpu",
            "status": "pass",
            "limit": "0.25",
            "violations": 0,
        }
    gates: list[dict[str, object]] = []
    gates.append(contention_gate)
    local_medians = {
        worker: timing_median(
            by_worker(local, worker), "local_scoring_ms", f"local W{worker} median"
        )
        for worker in WORKERS
    }
    construction_medians = {
        worker: construction_median(
            by_worker(construction, worker), f"construction W{worker} median"
        )
        for worker in WORKERS
    }
    gate_ratio(
        gates,
        "local_w8_over_w1",
        local_medians[8],
        local_medians[1],
        Decimal("0.50"),
    )
    gate_ratio(
        gates,
        "local_w8_over_w4",
        local_medians[8],
        local_medians[4],
        Decimal("1.10"),
    )
    gate_ratio(
        gates,
        "construction_w8_over_w1",
        construction_medians[8],
        construction_medians[1],
        Decimal("0.50"),
    )
    gate_ratio(
        gates,
        "construction_w8_over_w4",
        construction_medians[8],
        construction_medians[4],
        Decimal("1.10"),
    )

    for name, matrix, relevant_axes in (
        ("local", local, ("local_candidate", "local_candidate_pattern")),
        ("construction", construction, ("initial_chart", "inside_cache", "outside_cache")),
    ):
        for trial in by_worker(matrix, 8):
            global_hwm = parse_uint(
                trial.report["chart_workers_actually_active_high_water"],
                f"{trial.row_id} global high-water",
            )
            axis_hwm = max(
                parse_uint(
                    trial.report[f"chart_axis_{axis}_active_worker_high_water"],
                    f"{trial.row_id} {axis} high-water",
                )
                for axis in relevant_axes
            )
            if global_hwm <= 1 or axis_hwm <= 1:
                raise AcceptanceError(
                    f"{name} W8 trial {trial.trial_index}: expected real multi-worker execution"
                )
    gates.append({"name": "parallel_high_water", "status": "pass"})

    memory = args.physical_memory_bytes or physical_memory_bytes()
    if memory <= 0:
        raise AcceptanceError("physical memory must be positive")
    global_rss_cap_kb = min(16 * 1024 * 1024, memory // (4 * 1024))
    rss_details: dict[str, object] = {"global_cap_kb": global_rss_cap_kb}
    for name, matrix in (("local", local), ("construction", construction)):
        w1_max = max(int(trial.row["peak_sampled_rss_kb"]) for trial in by_worker(matrix, 1))
        w8_max = max(int(trial.row["peak_sampled_rss_kb"]) for trial in by_worker(matrix, 8))
        if w8_max > 2 * w1_max:
            raise AcceptanceError(f"{name} W8 peak RSS {w8_max} KiB exceeds 2x W1 {w1_max} KiB")
        if w8_max > global_rss_cap_kb:
            raise AcceptanceError(
                f"{name} W8 peak RSS {w8_max} KiB exceeds global cap {global_rss_cap_kb} KiB"
            )
        rss_details[name] = {"w1_max_kb": w1_max, "w8_max_kb": w8_max}
    gates.append({"name": "rss", "status": "pass", **rss_details})
    gates.append({"name": "swap_zero", "status": "pass"})

    deferred = False
    if args.phase3_raw_trials:
        phase3_sources = load_raw_trials([args.phase3_raw_trials])
        phase3 = select_workload(
            phase3_sources,
            args.local_row_prefix,
            args.repetitions,
            workers=(1,),
            scheduler=False,
            allow_other_workers=True,
        )
        current_identity = {
            (
                trial.row["input_sha256"],
                trial.row["refseq_sha256"],
                trial.row["search_semantic_sha256"],
                trial.row["output_semantic_sha256"],
            )
            for trial in by_worker(local, 1)
        }
        baseline_identity = {
            (
                trial.row["input_sha256"],
                trial.row["refseq_sha256"],
                trial.row["search_semantic_sha256"],
                trial.row["output_semantic_sha256"],
            )
            for trial in phase3
        }
        if current_identity != baseline_identity:
            raise AcceptanceError("Phase-3/current local W1 workload semantics or inputs differ")
        phase3_median = timing_median(
            phase3, "local_scoring_ms", "Phase-3 local W1 median"
        )
        gate_ratio(
            gates,
            "local_w1_over_phase3_w1",
            local_medians[1],
            phase3_median,
            Decimal("1.05"),
        )
        baseline_result: dict[str, object] = {
            "status": "pass",
            "phase3_median_ms": str(phase3_median),
        }
    else:
        deferred = True
        baseline_result = {
            "status": "deferred",
            "reason": args.defer_phase3_baseline,
        }
        gates.append(
            {
                "name": "local_w1_over_phase3_w1",
                "status": "deferred",
                "limit": "1.05",
                "reason": args.defer_phase3_baseline,
            }
        )

    raw_paths = [os.fspath(source.path) for source in raw_sources]
    return {
        "schema_version": SCHEMA_VERSION,
        "status": "deferred_baseline" if deferred else "pass",
        # Retain the historical scalar for one-file consumers.  The new list
        # is authoritative and records every independently owned input.
        "raw_trials": raw_paths[0],
        "raw_trial_inputs": raw_paths,
        "repetitions": args.repetitions,
        "workloads": {
            "local": {
                "row_prefix": args.local_row_prefix,
                "source_raw_trials": sorted(
                    {os.fspath(trial.source_path) for trial in local}
                ),
                "median_ms": {str(key): str(value) for key, value in local_medians.items()},
            },
            "construction": {
                "row_prefix": args.construction_row_prefix,
                "source_raw_trials": sorted(
                    {os.fspath(trial.source_path) for trial in construction}
                ),
                "component_fields": [
                    "initial_chart_construction_ms",
                    "local_inside_cache_initialization_ms",
                    "local_outside_cache_initialization_ms",
                ],
                "median_ms": {
                    str(key): str(value) for key, value in construction_medians.items()
                },
            },
        },
        "phase3_baseline": baseline_result,
        "gates": gates,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="strict Phase-4 WRIC chart-parallelization acceptance postprocessor"
    )
    parser.add_argument(
        "--raw-trials",
        required=True,
        action="append",
        metavar="PATH",
        help="Phase-4 raw_trials.tsv; repeat for independently captured matrices",
    )
    parser.add_argument("--repetitions", required=True, type=int)
    parser.add_argument("--local-row-prefix", default=DEFAULT_LOCAL_PREFIX)
    parser.add_argument("--construction-row-prefix", default=DEFAULT_CONSTRUCTION_PREFIX)
    baseline = parser.add_mutually_exclusive_group(required=True)
    baseline.add_argument("--phase3-raw-trials", help="same-workload Phase-3 raw_trials.tsv")
    baseline.add_argument(
        "--defer-phase3-baseline",
        metavar="REASON",
        help="explicitly defer only the Phase-3 serial-regression comparison",
    )
    parser.add_argument(
        "--physical-memory-bytes",
        type=int,
        help="override /proc/meminfo for reproducible/off-host validation",
    )
    parser.add_argument(
        "--contention-investigation-receipt",
        metavar="PATH",
        help="canonical Phase-4 contention-investigation JSON receipt",
    )
    parser.add_argument(
        "--expected-contention-investigation-receipt-sha256",
        metavar="HEX",
        help="expected SHA-256 of the contention-investigation receipt",
    )
    parser.add_argument(
        "--expected-contention-product-revision",
        metavar="HEX",
        help="expected product revision bound by the contention receipt",
    )
    parser.add_argument(
        "--expected-contention-dagutil-sha256",
        metavar="HEX",
        help="expected dagutil SHA-256 bound by the contention receipt",
    )
    parser.add_argument("--json-output", help="also atomically write the JSON result")
    return parser


def emit_result(result: dict[str, object], output: str | None) -> None:
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if output:
        target = Path(output)
        temporary = target.with_name(f".{target.name}.tmp.{os.getpid()}")
        try:
            temporary.write_text(rendered, encoding="utf-8")
            os.replace(temporary, target)
        except OSError as error:
            try:
                temporary.unlink()
            except OSError:
                pass
            raise AcceptanceError(f"cannot write JSON output {target}: {error}") from error
    sys.stdout.write(rendered)


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.repetitions <= 0:
        parser.error("--repetitions must be positive")
    if args.physical_memory_bytes is not None and args.physical_memory_bytes <= 0:
        parser.error("--physical-memory-bytes must be positive")
    if args.defer_phase3_baseline is not None and not args.defer_phase3_baseline.strip():
        parser.error("--defer-phase3-baseline requires a nonempty reason")
    contention_arguments = (
        args.contention_investigation_receipt,
        args.expected_contention_investigation_receipt_sha256,
        args.expected_contention_product_revision,
        args.expected_contention_dagutil_sha256,
    )
    if any(value is not None for value in contention_arguments) and not all(
        value is not None for value in contention_arguments
    ):
        parser.error(
            "all four contention-investigation receipt/product arguments are required together"
        )
    if (
        args.expected_contention_investigation_receipt_sha256 is not None
        and not SHA256_RE.fullmatch(
            args.expected_contention_investigation_receipt_sha256
        )
    ):
        parser.error(
            "--expected-contention-investigation-receipt-sha256 must be a lowercase SHA-256"
        )
    if (
        args.expected_contention_product_revision is not None
        and not REVISION_RE.fullmatch(args.expected_contention_product_revision)
    ):
        parser.error("--expected-contention-product-revision must be a lowercase revision")
    if (
        args.expected_contention_dagutil_sha256 is not None
        and not SHA256_RE.fullmatch(args.expected_contention_dagutil_sha256)
    ):
        parser.error(
            "--expected-contention-dagutil-sha256 must be a lowercase SHA-256"
        )
    try:
        result = evaluate(args)
        emit_result(result, args.json_output)
        return 0
    except ProfilingRequired as error:
        result = {
            "schema_version": SCHEMA_VERSION,
            "status": "profiling_required",
            "failure": str(error),
        }
    except AcceptanceError as error:
        result = {
            "schema_version": SCHEMA_VERSION,
            "status": "fail",
            "failure": str(error),
        }
    try:
        emit_result(result, args.json_output)
    except AcceptanceError as output_error:
        print(f"wric_phase4_acceptance: {output_error}", file=sys.stderr)
        emit_result(result, None)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
