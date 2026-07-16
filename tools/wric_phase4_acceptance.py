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
    for identity_field in ("fixture", "method", "input_sha256", "refseq_sha256"):
        local_identity = {trial.row[identity_field] for trial in local}
        construction_identity = {trial.row[identity_field] for trial in construction}
        if local_identity != construction_identity:
            raise AcceptanceError(
                f"local/construction matrices differ in {identity_field}"
            )

    # Contention above this boundary mandates profiling before any timing
    # result may be interpreted, so it deliberately takes precedence over the
    # downstream speed ratios.
    for trial in (*local, *construction):
        user = parse_decimal(trial.row["user_cpu_s"], f"{trial.row_id} user CPU", positive=True)
        system = parse_decimal(trial.row["system_cpu_s"], f"{trial.row_id} system CPU")
        ratio = system / user
        if ratio > Decimal("0.25"):
            raise ProfilingRequired(
                f"{trial.row_id} trial {trial.trial_index}: system/user CPU ratio {ratio} exceeds 0.25"
            )

    gates: list[dict[str, object]] = []
    gates.append({"name": "system_over_user_cpu", "status": "pass", "limit": "0.25"})
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
