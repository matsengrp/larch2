#!/usr/bin/env python3
"""Strict Phase-9 local-commit acceptance and run-sealing postprocessor.

Final acceptance audits the sealed base/parent/supplement trust chain, derives
all twelve frozen rows from that supplement-owned archive, and audits an exact
artifact ledger for the same-revision current run.  Only three W1 and three W8
timed trials per seed contribute to current timing.  Product reports, full
canonical record streams, compact digests, external DAG digests, scores, and
accepted sequences must all reconcile.

Ad-hoc frozen reports are intentionally available only behind an explicit
``deferred_non_final`` result.  They can support deadline-constrained interim
work but can never produce final ``pass`` status.  Acceptance results use
schema version 2 and split-root current-run archive seals use schema version
3; the frozen canonical evidence retains its independently versioned
historical schemas.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, localcontext
from pathlib import Path, PurePosixPath
from typing import Iterable, Mapping, Sequence, cast


SCHEMA = "wric.phase9.acceptance"
SCHEMA_VERSION = 2
SEEDS = (1, 7, 19)
WORKERS = (1, 2, 4, 8)
MEASURED_WORKERS = (1, 8)
DEFAULT_ROW_ID_TEMPLATE = "phase9-local-commit-seed{seed}-w{worker}"
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
MAX_SYSTEM_USER_RATIO = Decimal("0.25")
MAX_CPU_CAPACITY_SLACK = Decimal("1.25")
MIN_CPU_WALL_RATIO = Decimal("0.50")
TIMER_WALL_RELATIVE_SLACK = Decimal("0.05")
TIMER_WALL_ABSOLUTE_SLACK_MS = Decimal("5")
MAX_WALL_OVER_TIMER_RATIO = Decimal("2")
MAX_WALL_OVER_TIMER_SLACK_MS = Decimal("250")
RSS_LIMIT_BYTES = 16 * 1024**3
MEMORY_BUDGET_BYTES = 12 * 1024**3

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_HARNESS = REPOSITORY_ROOT / "tools" / "wric_spr_search_benchmark.sh"
PRODUCTION_LARCH2 = REPOSITORY_ROOT / "build" / "bin" / "larch2"
PRODUCTION_DAGUTIL = REPOSITORY_ROOT / "build" / "bin" / "dagutil"

RUN_METADATA_NAME = "phase9-run-metadata.json"
RUN_LEDGER_NAME = "phase9-run-artifacts.tsv"
RUN_LEDGER_SEAL_NAME = RUN_LEDGER_NAME + ".sha256"
RUN_SCHEMA = "wric_phase9_benchmark_run"
RUN_LEDGER_SCHEMA = "wric_phase9_benchmark_artifacts"
RUN_SCHEMA_VERSION = 3
RUN_GROUP = "phase9-local-commit"
RUN_METADATA_KEYS = frozenset(
    (
        "schema",
        "schema_version",
        "role",
        "run_group",
        "seeds",
        "workers",
        "repetitions",
        "warmups_per_row",
        "full_canonical",
        "affinity_cpus",
        "base_manifest_sha256",
        "supplement_manifest_sha256",
        "characterization_sha256",
        "fixture_sha256",
        "base_repo_root",
        "base_revision",
        "working_repo_root",
        "working_revision",
        "repository_status",
        "binary_provenance_limit",
        "working_larch2_sha256",
        "working_dagutil_sha256",
        "benchmark_harness_sha256",
        "command_contract_sha256",
        "raw_trials_sha256",
        "summary_sha256",
        "commands_sha256",
        "raw_trial_rows",
        "row_ids",
    )
)
RUN_LEDGER_HEADER = ("sha256", "bytes", "path")
SAFE_LEDGER_COMPONENT = re.compile(r"[A-Za-z0-9_.-]+")

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
    "parallel_operations",
    "minimum_effective_grain",
    "maximum_effective_grain",
)

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
    "best_validated_parsimony_min",
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
    "user_cpu_s",
    "system_cpu_s",
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
class ValidatedIterationContract:
    sequence: tuple[dict[str, object], ...]
    inside_rows: tuple[int, ...] | None = None
    outside_rows: tuple[int, ...] | None = None


@dataclass(frozen=True)
class Trial:
    row: dict[str, str]
    report: ParsedReport
    compact_path: Path
    search_digest: dict[str, object]
    output_digest: dict[str, object]
    full_sidecar_sha256: str
    accepted_sequence: tuple[dict[str, object], ...]
    canonical_record_count: int
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


def parse_int(value: str, label: str) -> int:
    if re.fullmatch(r"0|-?[1-9][0-9]*", value) is None:
        raise AcceptanceError(f"{label}: expected a canonical decimal integer, got {value!r}")
    return int(value)


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
    root = raw_path.parent.absolute()
    path = Path(recorded)
    if not path.is_absolute():
        path = root / path
    lexical = path.absolute()
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"recorded artifact is missing: {path}: {error}") from error
    if lexical != resolved:
        raise AcceptanceError(f"recorded artifact uses a symlink or noncanonical lexical path: {path}")
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise AcceptanceError(f"recorded artifact escapes benchmark directory: {path}") from error
    try:
        info = resolved.stat()
    except OSError as error:
        raise AcceptanceError(f"cannot stat recorded artifact {resolved}: {error}") from error
    if not stat.S_ISREG(info.st_mode):
        raise AcceptanceError(f"recorded artifact is not a regular file: {resolved}")
    return resolved


def require_run_artifact(path: Path, root: Path, label: str) -> Path:
    """Require one canonical, non-symlink regular member of a run archive."""

    lexical = path.absolute()
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"{label} is missing: {path}: {error}") from error
    if lexical != resolved:
        raise AcceptanceError(f"{label} uses a symlink or noncanonical lexical path: {path}")
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise AcceptanceError(f"{label} escapes benchmark directory: {path}") from error
    if not stat.S_ISREG(resolved.stat().st_mode):
        raise AcceptanceError(f"{label} is not a regular file: {path}")
    return resolved


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


def _reject_nonfinite_json_constant(value: str) -> object:
    raise AcceptanceError(f"non-finite JSON constant {value!r}")


def read_json_object(path: Path, label: str) -> dict[str, object]:
    try:
        payload = path.read_text(encoding="utf-8")
    except OSError as error:
        raise AcceptanceError(f"cannot read {label} {path}: {error}") from error
    try:
        value = json.loads(
            payload,
            object_pairs_hook=_reject_duplicate_json_keys,
            parse_constant=_reject_nonfinite_json_constant,
        )
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
    record_count = value["record_count"]
    assert type(record_count) is int
    if record_count <= 0:
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
    clade_count = value["clade_count"]
    production_count = value["production_count"]
    assert type(clade_count) is int and type(production_count) is int
    if clade_count <= 0 or production_count <= 0:
        raise AcceptanceError(f"{label}: clade/production counts must be positive")
    return value


def sanitize_harness_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def expected_phase9_trial_paths(
    raw_path: Path, row: Mapping[str, str], trial_index: int, worker: int
) -> tuple[Path, Path, Path]:
    """Derive the only report/search/output-digest names admitted for a timed row."""

    root = raw_path.parent.absolute()
    fixture = sanitize_harness_name(row["fixture"])
    row_id = sanitize_harness_name(row["row_id"])
    stem = f"{fixture}_{METHOD}_trial{trial_index}_{row_id}_w{worker}"
    report = root / "logs" / f"{stem}.out"
    compact = root / "logs" / f"{stem}.canonical.json"
    score = (
        root
        / "logs"
        / f"{fixture}_{METHOD}_score_trial{trial_index}_{row_id}_w{worker}.canonical-dag.json"
    )
    return report, compact, score


def expected_phase9_warmup_compact_path(
    root: Path, row: Mapping[str, str], worker: int
) -> Path:
    fixture = sanitize_harness_name(row["fixture"])
    row_id = sanitize_harness_name(row["row_id"])
    return (
        root.absolute()
        / "logs"
        / f"{fixture}_{METHOD}_warmup1_{row_id}_w{worker}.canonical.json"
    )


def canonical_paths(raw_path: Path, row: dict[str, str], report_path: Path) -> tuple[Path, Path, Path, Path]:
    root = raw_path.parent
    fixture = sanitize_harness_name(row["fixture"])
    row_id = sanitize_harness_name(row["row_id"])
    full = root / "logs" / f"{fixture}_{row_id}_full_canonical.json"
    sidecar = root / "logs" / f"{fixture}_{row_id}_full_canonical.ndjson"

    needle = f"_{row['method']}_"
    if report_path.name.count(needle) != 1 or not report_path.name.endswith(".out"):
        raise AcceptanceError(
            f"{row['row_id']} trial {row['trial_index']}: cannot derive deferred output digest from report_path"
        )
    compact = report_path.with_name(report_path.name[:-4] + ".canonical.json")
    score_name = report_path.name.replace(needle, f"{needle}score_", 1)
    score_digest = report_path.with_name(score_name[:-4] + ".canonical-dag.json")
    return (
        compact.absolute(),
        full.absolute(),
        sidecar.absolute(),
        score_digest.absolute(),
    )


def sha256_file(path: Path, label: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while chunk := handle.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise AcceptanceError(f"cannot read {label} {path}: {error}") from error
    return digest.hexdigest()


def canonical_json_int(
    record: Mapping[str, object], key: str, label: str, *, minimum: int | None = None
) -> int:
    value = record.get(key)
    if type(value) is not int:
        raise AcceptanceError(f"{label}: {key} is not a JSON integer")
    if minimum is not None and value < minimum:
        raise AcceptanceError(f"{label}: {key}={value}, expected at least {minimum}")
    return value


def canonical_json_bool(record: Mapping[str, object], key: str, label: str) -> bool:
    value = record.get(key)
    if type(value) is not bool:
        raise AcceptanceError(f"{label}: {key} is not a JSON boolean")
    return value


def canonical_json_string(
    record: Mapping[str, object], key: str, label: str, *, nonempty: bool = False
) -> str:
    value = record.get(key)
    if not isinstance(value, str) or (nonempty and not value):
        qualifier = "nonempty " if nonempty else ""
        raise AcceptanceError(f"{label}: {key} is not a {qualifier}JSON string")
    return value


def read_full_canonical_sidecar(
    path: Path, seed: int, label: str
) -> dict[str, object]:
    """Parse and reconcile the full search record stream, not only its digest."""

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise AcceptanceError(f"cannot read full canonical sidecar {path}: {error}") from error
    if not lines or any(not line for line in lines):
        raise AcceptanceError(f"{label}: full canonical sidecar is empty or contains a blank record")
    records: list[dict[str, object]] = []
    for line_number, line in enumerate(lines, 1):
        try:
            value = json.loads(
                line,
                object_pairs_hook=_reject_duplicate_json_keys,
                parse_constant=_reject_nonfinite_json_constant,
            )
        except (json.JSONDecodeError, AcceptanceError) as error:
            raise AcceptanceError(
                f"{label}: malformed full canonical JSON record at line {line_number}: {error}"
            ) from error
        if not isinstance(value, dict):
            raise AcceptanceError(f"{label}: canonical line {line_number} is not a JSON object")
        canonical_json_string(value, "record", f"{label} canonical line {line_number}", nonempty=True)
        records.append(value)

    by_kind: dict[str, list[dict[str, object]]] = {}
    for record in records:
        by_kind.setdefault(str(record["record"]), []).append(record)

    def exactly(kind: str, count: int) -> list[dict[str, object]]:
        selected = by_kind.get(kind, [])
        if len(selected) != count:
            raise AcceptanceError(
                f"{label}: full canonical stream has {len(selected)} {kind} records, expected {count}"
            )
        return selected

    schema = exactly("schema", 1)[0]
    if records[0] is not schema or schema != {
        "record": "schema",
        "schema": "larch.chart_spr.semantic.ndjson",
        "schema_version": 1,
    }:
        raise AcceptanceError(f"{label}: full canonical schema record is absent, displaced, or changed")
    contract = exactly("contract", 1)[0]
    contract_literals: Mapping[str, object] = {
        "acceptance": "exact_multisite",
        "objective": "grammar_exact",
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "topology_selection": "none",
        "commit_mode": "overlay_delta",
        "accepted_state_update": "overlay_chain_local_commit",
        "verification_mode": "transient",
        "chain_per_accept_exactness": "exact_multisite",
        "keep_mask_contract": "exact_required",
        "candidate_cap_semantics": "post_dedup",
        "max_iterations": ITERATIONS,
        "max_candidates": MAX_CANDIDATES,
        "top_k_exact": TOP_K_EXACT,
        "seed": seed,
        "use_bound_pruning": True,
        "require_exact_keep_mask": True,
        "randomize_order": False,
        "reservoir_sample": False,
        "include_immediate_reversals": False,
    }
    for key, expected in contract_literals.items():
        actual = contract.get(key)
        if actual != expected or type(actual) is not type(expected):
            raise AcceptanceError(
                f"{label}: canonical contract {key}={actual!r}, expected {expected!r}"
            )

    initial_record = exactly("initial_state", 1)[0]
    initial_score = canonical_json_int(initial_record, "initial_score", label, minimum=0)
    active_patterns = canonical_json_int(initial_record, "active_patterns", label, minimum=1)
    final_record = exactly("final_state", 1)[0]
    final_score = canonical_json_int(final_record, "final_score", label, minimum=0)
    if canonical_json_int(final_record, "accepted_moves", label) != ITERATIONS:
        raise AcceptanceError(f"{label}: canonical final state does not contain three accepted moves")

    begins = exactly("iteration_begin", ITERATIONS)
    outcomes = exactly("iteration_outcome", ITERATIONS)
    candidates = exactly("candidate", EXPECTED_CANDIDATES)
    lower_bounds = exactly("candidate_lower_bound", EXPECTED_CANDIDATES)
    exact_candidates = exactly("candidate_exact", EXPECTED_EXACT)
    chains = exactly("chain_entry", ITERATIONS)

    def indexed(
        values: Sequence[dict[str, object]], kind: str
    ) -> dict[tuple[int, int], dict[str, object]]:
        result: dict[tuple[int, int], dict[str, object]] = {}
        for value in values:
            iteration = canonical_json_int(value, "iteration", f"{label} {kind}")
            stream = canonical_json_int(value, "stream_index", f"{label} {kind}")
            key = (iteration, stream)
            if key in result:
                raise AcceptanceError(f"{label}: duplicate canonical {kind} key {key}")
            result[key] = value
        return result

    candidates_by_key = indexed(candidates, "candidate")
    lower_by_key = indexed(lower_bounds, "candidate_lower_bound")
    exact_by_key = indexed(exact_candidates, "candidate_exact")
    if set(candidates_by_key) != set(lower_by_key):
        raise AcceptanceError(f"{label}: canonical candidate/lower-bound key sets differ")
    if not set(exact_by_key) <= set(candidates_by_key):
        raise AcceptanceError(f"{label}: canonical exact evidence refers to an absent candidate")

    sequence: list[dict[str, object]] = []
    expected_before = initial_score
    for iteration in range(ITERATIONS):
        iteration_label = f"{label} canonical iteration {iteration}"
        per_iteration_candidates = [key for key in candidates_by_key if key[0] == iteration]
        per_iteration_exact = [key for key in exact_by_key if key[0] == iteration]
        if len(per_iteration_candidates) != MAX_CANDIDATES or len(per_iteration_exact) != TOP_K_EXACT:
            raise AcceptanceError(f"{iteration_label}: canonical 32/4 cardinality changed")
        begin = next(
            (value for value in begins if value.get("iteration") == iteration), None
        )
        outcome = next(
            (value for value in outcomes if value.get("iteration") == iteration), None
        )
        if begin is None or outcome is None:
            raise AcceptanceError(f"{iteration_label}: begin/outcome index is absent or duplicated")
        if canonical_json_int(begin, "seed", iteration_label) != seed + iteration:
            raise AcceptanceError(
                f"{iteration_label}: per-iteration seed is not base seed plus iteration"
            )
        before = canonical_json_int(begin, "state_score_before", iteration_label)
        if before != expected_before:
            raise AcceptanceError(f"{iteration_label}: state-score chain is discontinuous")
        outcome_literals: Mapping[str, object] = {
            "generation_stop_reason": "candidate_cap",
            "candidates_generated": MAX_CANDIDATES,
            "candidates_scored": MAX_CANDIDATES,
            "candidates_exact_verified": TOP_K_EXACT,
            "accepted_move_present": True,
            "accepted_move_committed": True,
            "post_materialization_rejected": False,
        }
        for key, expected in outcome_literals.items():
            actual = outcome.get(key)
            if actual != expected or type(actual) is not type(expected):
                raise AcceptanceError(
                    f"{iteration_label}: outcome {key}={actual!r}, expected {expected!r}"
                )
        selected_stream = canonical_json_int(outcome, "selected_stream_index", iteration_label)
        selected_key = (iteration, selected_stream)
        if selected_key not in exact_by_key:
            raise AcceptanceError(f"{iteration_label}: selected move lacks exact evidence")
        candidate = candidates_by_key[selected_key]
        lower = lower_by_key[selected_key]
        exact = exact_by_key[selected_key]
        signature = canonical_json_string(outcome, "selected_signature", iteration_label, nonempty=True)
        if canonical_json_string(candidate, "signature", iteration_label, nonempty=True) != signature:
            raise AcceptanceError(f"{iteration_label}: selected candidate signature disagrees")
        if canonical_json_bool(candidate, "valid", iteration_label) is not True:
            raise AcceptanceError(f"{iteration_label}: selected canonical candidate is invalid")
        affected = canonical_json_int(candidate, "affected_clade_count", iteration_label, minimum=1)
        after = canonical_json_int(outcome, "state_score_after", iteration_label)
        for score_record, kind, exact_flag in (
            (lower, "composite_lower_bound", False),
            (exact, "grammar_exact", True),
        ):
            if canonical_json_string(score_record, "kind", iteration_label) != kind:
                raise AcceptanceError(f"{iteration_label}: selected {kind} label changed")
            if canonical_json_bool(score_record, "exact_multisite", iteration_label) is not exact_flag:
                raise AcceptanceError(f"{iteration_label}: selected {kind} exactness label changed")
            old = canonical_json_int(score_record, "old_score", iteration_label)
            new = canonical_json_int(score_record, "new_score", iteration_label)
            delta = canonical_json_int(score_record, "delta", iteration_label)
            if old != before or new - old != delta:
                raise AcceptanceError(f"{iteration_label}: selected {kind} score arithmetic disagrees")
            if new >= before or delta >= 0:
                raise AcceptanceError(
                    f"{iteration_label}: selected {kind} did not strictly improve the score"
                )
        if canonical_json_int(exact, "new_score", iteration_label) != after:
            raise AcceptanceError(f"{iteration_label}: selected exact/outcome scores disagree")
        if after >= before:
            raise AcceptanceError(f"{iteration_label}: selected move did not strictly improve the state")
        sequence.append(
            {
                "iteration": iteration,
                "state_score_before": before,
                "state_score_after": after,
                "lower_bound_delta": canonical_json_int(lower, "delta", iteration_label),
                "lower_bound_new_score": canonical_json_int(lower, "new_score", iteration_label),
                "exact_delta": canonical_json_int(exact, "delta", iteration_label),
                "exact_new_score": canonical_json_int(exact, "new_score", iteration_label),
                "affected_clades": affected,
                "topology_selection": "none",
                "selected_signature": signature,
            }
        )
        expected_before = after
    if expected_before != final_score:
        raise AcceptanceError(f"{label}: canonical iteration/final scores disagree")
    chain_positions = [canonical_json_int(value, "position", label) for value in chains]
    if sorted(chain_positions) != list(range(ITERATIONS)):
        raise AcceptanceError(f"{label}: canonical chain positions are not exactly 0..2")
    for value in chains:
        if canonical_json_string(value, "commit_source", label) != "spr_overlay_delta":
            raise AcceptanceError(f"{label}: canonical chain commit source changed")
        for key in ("added_production_keys", "tombstoned_production_keys"):
            productions = value.get(key)
            if not isinstance(productions, list) or not productions or not all(
                isinstance(item, str) and item for item in productions
            ):
                raise AcceptanceError(f"{label}: canonical chain {key} is absent or empty")
    return {
        "record_count": len(records),
        "initial_score": initial_score,
        "final_score": final_score,
        "active_patterns": active_patterns,
        "accepted_sequence": tuple(sequence),
    }


def report_sequence_projection(
    canonical_sequence: Sequence[dict[str, object]],
    *,
    current_trial: bool = False,
) -> tuple[dict[str, object], ...]:
    """Project the full stream to fields emitted by the human product report."""

    return tuple(
        {
            **item,
            "selected_signature": (
                item["selected_signature"]
                if current_trial or index == 0
                else None
            ),
        }
        for index, item in enumerate(canonical_sequence)
    )


def check_literal(actual: str, expected: str, label: str) -> None:
    if actual != expected:
        raise AcceptanceError(f"{label}={actual!r}, expected {expected!r}")


def validate_iteration_contract(
    report: ParsedReport, label: str, *, current_trial: bool = False
) -> ValidatedIterationContract:
    if len(report.iterations) != ITERATIONS:
        raise AcceptanceError(f"{label}: expected {ITERATIONS} iteration reports, found {len(report.iterations)}")
    indexes = [
        parse_uint(require_iteration(item, "iteration", label), f"{label} iteration index")
        for item in report.iterations
    ]
    if indexes != list(range(ITERATIONS)):
        raise AcceptanceError(f"{label}: iteration indexes are not exactly 0..{ITERATIONS - 1}")

    previous_after: int | None = None
    sequence: list[dict[str, object]] = []
    inside_rows: list[int] = []
    outside_rows: list[int] = []
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
        exact_delta = parse_int(
            require_iteration(item, "accepted_exact_delta", iteration_label),
            f"{iteration_label} accepted_exact_delta",
        )
        lower_after = parse_uint(
            require_iteration(item, "accepted_lower_bound_new_score", iteration_label),
            f"{iteration_label} accepted_lower_bound_new_score",
        )
        lower_delta = parse_int(
            require_iteration(item, "accepted_lower_bound_delta", iteration_label),
            f"{iteration_label} accepted_lower_bound_delta",
        )
        rebuilt = parse_uint(require_iteration(item, "post_materialization_rebuilt_score", iteration_label), f"{iteration_label} post_materialization_rebuilt_score")
        if after >= before:
            raise AcceptanceError(
                f"{iteration_label}: committed score did not strictly improve from {before} to {after}"
            )
        if exact_after != after or rebuilt != after:
            raise AcceptanceError(f"{iteration_label}: exact/rebuilt/state-after scores disagree")
        if exact_after - before != exact_delta:
            raise AcceptanceError(f"{iteration_label}: exact delta does not reconcile with scores")
        if lower_after - before != lower_delta:
            raise AcceptanceError(f"{iteration_label}: lower-bound delta does not reconcile with scores")
        if exact_delta >= 0 or lower_delta >= 0 or lower_after >= before:
            raise AcceptanceError(
                f"{iteration_label}: accepted exact/lower-bound scores must both strictly decrease"
            )
        if previous_after is not None and before != previous_after:
            raise AcceptanceError(f"{iteration_label}: state score is discontinuous across commits")
        previous_after = after
        signature = item.get("accepted_candidate_signature", "")
        if (current_trial or index == 0) and not signature:
            qualifier = "current" if current_trial else "first timed"
            raise AcceptanceError(
                f"{iteration_label}: {qualifier} accepted signature is absent"
            )
        if current_trial:
            for key, destination in (
                ("accepted_inside_rows_recomputed", inside_rows),
                ("accepted_outside_rows_recomputed", outside_rows),
            ):
                rows = parse_uint(
                    require_iteration(item, key, iteration_label),
                    f"{iteration_label} {key}",
                )
                if rows < MIN_ROWS_PER_ACCEPT:
                    raise AcceptanceError(
                        f"{iteration_label}: {key}={rows}, expected at least "
                        f"{MIN_ROWS_PER_ACCEPT}"
                    )
                destination.append(rows)
        affected = parse_uint(
            require_iteration(item, "accepted_affected_clades", iteration_label),
            f"{iteration_label} accepted_affected_clades",
            positive=True,
        )
        topology = require_iteration(item, "accepted_topology_selection", iteration_label)
        check_literal(topology, "none", f"{iteration_label} accepted_topology_selection")
        sequence.append(
            {
                "iteration": index,
                "state_score_before": before,
                "state_score_after": after,
                "lower_bound_delta": lower_delta,
                "lower_bound_new_score": lower_after,
                "exact_delta": exact_delta,
                "exact_new_score": exact_after,
                "affected_clades": affected,
                "topology_selection": topology,
                "selected_signature": signature if current_trial or index == 0 else None,
            }
        )
    return ValidatedIterationContract(
        sequence=tuple(sequence),
        inside_rows=tuple(inside_rows) if current_trial else None,
        outside_rows=tuple(outside_rows) if current_trial else None,
    )


def validate_report_characterization(
    report: ParsedReport, seed: int, label: str, *, current_trial: bool = False
) -> dict[str, object]:
    literals = {
        "search_mode": "phase5_accept_reject_overlay_chain_local_commit",
        "accepted_state_update_mode": "overlay_chain_local_cache_commit",
        "accepted_state_materialization": "none_per_accept_overlay_chain_local_cache_update",
        "final_compaction_mode": "grammar_level_exact_multi_tree_compaction",
        "preserves_full_accepted_overlay_dag": "true",
        "actual_dag_mutation": "true",
        "output_dag_mutated": "true",
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
    integer_literals: Mapping[str, int] = {
        "top_k_exact_verify": TOP_K_EXACT,
        "configured_max_candidates": MAX_CANDIDATES,
        "seed": seed,
        "requested_max_iterations": ITERATIONS,
        "iterations": ITERATIONS,
        "accepted_moves": ITERATIONS,
        "local_commit_accepted_moves": ITERATIONS,
        "candidate_accepts_attempted": ITERATIONS,
        "candidates_generated": EXPECTED_CANDIDATES,
        "candidates_scored": EXPECTED_CANDIDATES,
        "exact_verifications": EXPECTED_EXACT,
        "post_materialization_rejections": 0,
        "initial_search_state_rebuilds": 1,
        "sidecar_rebuilds_after_accept": 0,
        "full_search_state_rebuilds": 1,
        "overlay_materializations_for_accept_materialization": 0,
        "overlay_materializations_for_final_compaction": 1,
        "final_compaction_rebuilds": 1,
        "local_commit_tombstone_scope_skips": 0,
    }
    parsed: dict[str, int] = {}
    for numeric_key, numeric_expected in integer_literals.items():
        actual = parse_uint(require_top(report, numeric_key, label), f"{label} {numeric_key}")
        if actual != numeric_expected:
            raise AcceptanceError(
                f"{label}: {numeric_key}={actual}, expected {numeric_expected}"
            )
        parsed[numeric_key] = actual
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
    combined_rows = inside + outside
    validated_iterations = validate_iteration_contract(
        report, label, current_trial=current_trial
    )
    if current_trial:
        assert validated_iterations.inside_rows is not None
        assert validated_iterations.outside_rows is not None
        iteration_inside = sum(validated_iterations.inside_rows)
        iteration_outside = sum(validated_iterations.outside_rows)
        if iteration_inside != inside or iteration_outside != outside:
            raise AcceptanceError(
                f"{label}: per-iteration accepted row deltas {iteration_inside}/{iteration_outside} "
                f"do not equal top-level inside/outside totals {inside}/{outside}"
            )
    else:
        # The sealed historical product report predates per-iteration row
        # deltas.  Preserve that frozen schema while retaining its original
        # aggregate non-vacuity gate.
        minimum_rows = ITERATIONS * MIN_ROWS_PER_ACCEPT
        if combined_rows < minimum_rows:
            raise AcceptanceError(
                f"{label}: combined inside/outside rows recomputed={combined_rows}, "
                f"expected at least {minimum_rows} ({MIN_ROWS_PER_ACCEPT} per accepted move)"
            )
    sequence = validated_iterations.sequence
    initial = parse_uint(require_top(report, "initial_score", label), f"{label} initial_score")
    final = parse_uint(require_top(report, "final_score", label), f"{label} final_score")
    if final >= initial:
        raise AcceptanceError(
            f"{label}: final score {final} did not strictly improve initial score {initial}"
        )
    if sequence[0]["state_score_before"] != initial or sequence[-1]["state_score_after"] != final:
        raise AcceptanceError(f"{label}: top-level and iteration endpoint scores disagree")
    return {
        "active_patterns": active,
        "inside_rows_recomputed_on_commit": inside,
        "outside_rows_recomputed_on_commit": outside,
        "combined_rows_recomputed_on_commit": combined_rows,
        "combined_rows_per_accept": ratio_text(Decimal(combined_rows), Decimal(ITERATIONS)),
        "initial_score": initial,
        "final_score": final,
        "accepted_sequence": sequence,
    }


def validate_scheduler(
    report: ParsedReport, worker: int, active_patterns: int, label: str
) -> None:
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
        "minimum_grain": "chart_scheduler_minimum_effective_grain",
        "maximum_grain": "chart_scheduler_maximum_effective_grain",
        "queue_wait_ns": "chart_scheduler_queue_wait_nanoseconds",
        "queue_wait_max_ns": "chart_scheduler_queue_wait_nanoseconds_max",
        "queue_wait_samples": "chart_scheduler_queue_wait_samples",
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
    if metric["queue_wait_samples"] != metric["tasks"]:
        raise AcceptanceError(f"{label}: scheduler queue-wait samples/tasks do not reconcile")
    if metric["queue_wait_max_ns"] > metric["queue_wait_ns"]:
        raise AcceptanceError(f"{label}: scheduler maximum queue wait exceeds aggregate wait")
    if worker == 1 and (
        metric["parallel"] != 0
        or metric["tasks"] != 0
        or metric["pools"] != 0
        or metric["queue_wait_ns"] != 0
    ):
        raise AcceptanceError(f"{label}: one-worker run used a parallel pool")

    axes: dict[str, dict[str, int]] = {}
    for axis in AXES:
        fields = {
            field: parse_uint(
                require_top(report, f"chart_axis_{axis}_{field}", label),
                f"{label} chart_axis_{axis}_{field}",
            )
            for field in AXIS_FIELDS
        }
        axes[axis] = fields
        operations = fields["operations"]
        if operations == 0:
            if any(fields.values()):
                raise AcceptanceError(f"{label}: zero-operation {axis} axis has nonzero accounting")
            continue
        if fields["parallel_operations"] > operations:
            raise AcceptanceError(f"{label}: {axis} parallel operations exceed operations")
        for field in (
            "items",
            "ranges",
            "active_worker_high_water",
            "minimum_effective_grain",
            "maximum_effective_grain",
        ):
            if fields[field] == 0:
                raise AcceptanceError(f"{label}: active {axis} axis has zero {field}")
        if fields["minimum_effective_grain"] > fields["maximum_effective_grain"]:
            raise AcceptanceError(f"{label}: {axis} grain bounds are reversed")
        if fields["active_worker_high_water"] > worker:
            raise AcceptanceError(f"{label}: {axis} active-worker high-water exceeds worker count")
        if worker == 1 and (
            fields["parallel_operations"] != 0
            or fields["tasks"] != 0
            or fields["active_worker_high_water"] != 1
        ):
            raise AcceptanceError(f"{label}: one-worker {axis} axis used parallel execution")
        if fields["parallel_operations"] and fields["tasks"] == 0:
            raise AcceptanceError(f"{label}: parallel {axis} operations submitted no tasks")

    sums = {
        "operations": sum(value["operations"] for value in axes.values()),
        "parallel": sum(value["parallel_operations"] for value in axes.values()),
        "ranges": sum(value["ranges"] for value in axes.values()),
        "tasks": sum(value["tasks"] for value in axes.values()),
    }
    for key in ("operations", "parallel", "ranges", "tasks"):
        if metric[key] != sums[key]:
            raise AcceptanceError(
                f"{label}: scheduler global {key}={metric[key]} does not equal axis sum {sums[key]}"
            )
    active_axes = [value for value in axes.values() if value["operations"]]
    if not active_axes:
        raise AcceptanceError(f"{label}: scheduler has no active axes")
    if metric["active_hwm"] != max(value["active_worker_high_water"] for value in active_axes):
        raise AcceptanceError(f"{label}: global/axis active-worker high-water marks disagree")
    if metric["minimum_grain"] != min(value["minimum_effective_grain"] for value in active_axes):
        raise AcceptanceError(f"{label}: global/axis minimum effective grains disagree")
    if metric["maximum_grain"] != max(value["maximum_effective_grain"] for value in active_axes):
        raise AcceptanceError(f"{label}: global/axis maximum effective grains disagree")

    cache_operations = ITERATIONS + 1
    expected_grain = (
        active_patterns
        if worker == 1
        else max(1, (active_patterns + 4 * worker - 1) // (4 * worker))
    )
    ranges_per_operation = (active_patterns + expected_grain - 1) // expected_grain
    tasks_per_operation = 0 if worker == 1 else min(worker, ranges_per_operation)
    for axis in ("inside_cache", "outside_cache"):
        fields = axes[axis]
        expected = {
            "operations": cache_operations,
            "items": cache_operations * active_patterns,
            "ranges": cache_operations * ranges_per_operation,
            "tasks": cache_operations * tasks_per_operation,
            "parallel_operations": 0 if worker == 1 else cache_operations,
            "minimum_effective_grain": expected_grain,
            "maximum_effective_grain": expected_grain,
        }
        for field, value in expected.items():
            if fields[field] != value:
                raise AcceptanceError(
                    f"{label}: {axis} {field}={fields[field]}, expected exact "
                    f"initial-plus-three-commit value {value}"
                )
        if worker == 1 and fields["active_worker_high_water"] != 1:
            raise AcceptanceError(
                f"{label}: W1 {axis} active-worker high-water must be exactly one"
            )
        if worker > 1 and not (
            2
            <= fields["active_worker_high_water"]
            <= min(worker, ranges_per_operation)
        ):
            raise AcceptanceError(
                f"{label}: W{worker} {axis} active-worker high-water does not prove "
                "multi-worker execution"
            )


def validate_current_counters(
    report: ParsedReport, seed: int, worker: int, label: str
) -> dict[str, object]:
    characterization = validate_report_characterization(
        report, seed, label, current_trial=True
    )
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
    active_patterns = cast(int, characterization["active_patterns"])
    expected_row_visits = EXPECTED_CANDIDATES * active_patterns
    if row_visits != expected_row_visits:
        raise AcceptanceError(
            f"{label}: local_commit_inside_row_view_pattern_visits={row_visits}, "
            f"expected exactly {EXPECTED_CANDIDATES}*{active_patterns}={expected_row_visits}"
        )
    cache_build_values = {
        "initial_state_inside_charts_built": active_patterns,
        "inside_cache_inside_charts_built": active_patterns,
        "inside_cache_resident_inside_charts_consumed": 0,
        "outside_cache_inside_charts_built": 0,
        "outside_cache_inside_charts_reused": active_patterns,
        "outside_cache_outside_charts_built": active_patterns,
    }
    for key, expected in cache_build_values.items():
        actual = parse_uint(require_top(report, key, label), f"{label} {key}")
        if actual != expected:
            raise AcceptanceError(f"{label}: {key}={actual}, expected {expected}")
    report_memory_budget = parse_uint(
        require_top(report, "memory_budget_bytes", label),
        f"{label} memory_budget_bytes",
        positive=True,
    )
    if report_memory_budget != MEMORY_BUDGET_BYTES:
        raise AcceptanceError(
            f"{label}: report memory_budget_bytes={report_memory_budget}, "
            f"expected {MEMORY_BUDGET_BYTES}"
        )
    validate_scheduler(report, worker, active_patterns, label)
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
    if attempts != ITERATIONS:
        raise AcceptanceError(f"{label}: candidate_accepts_attempted={attempts}, expected exactly {ITERATIONS}")
    score_fields = (
        "initial_validated_parsimony_min",
        "final_validated_parsimony_min",
        "best_reported_objective",
        "best_validated_parsimony_min",
    )
    scores = {key: parse_uint(row[key], f"{label} {key}") for key in score_fields}
    if scores["final_validated_parsimony_min"] >= scores["initial_validated_parsimony_min"]:
        raise AcceptanceError(
            f"{label}: external DAG parsimony did not strictly improve"
        )
    if (
        scores["best_validated_parsimony_min"]
        != scores["final_validated_parsimony_min"]
    ):
        raise AcceptanceError(
            f"{label}: raw external final/best-validated scores disagree"
        )
    wall = parse_decimal(row["wall_clock_s"], f"{label} wall_clock_s", positive=True)
    user_cpu = parse_decimal(row["user_cpu_s"], f"{label} user_cpu_s", positive=True)
    system_cpu = parse_decimal(row["system_cpu_s"], f"{label} system_cpu_s")
    if system_cpu > user_cpu * MAX_SYSTEM_USER_RATIO:
        raise AcceptanceError(
            f"{label}: system/user CPU ratio exceeds {decimal_text(MAX_SYSTEM_USER_RATIO)}"
        )
    cpu_capacity = Decimal(worker) * wall * MAX_CPU_CAPACITY_SLACK + Decimal("0.05")
    if user_cpu + system_cpu > cpu_capacity:
        raise AcceptanceError(
            f"{label}: user+system CPU exceeds worker-count wall-clock capacity"
        )
    accepted_ms = parse_decimal(row["accepted_rebuild_ms"], f"{label} accepted_rebuild_ms", positive=True)
    total_ms = parse_decimal(row["total_ms"], f"{label} total_ms", positive=True)
    if accepted_ms > total_ms:
        raise AcceptanceError(f"{label}: accepted_rebuild_ms exceeds total_ms")
    timer_slack = max(
        TIMER_WALL_ABSOLUTE_SLACK_MS,
        wall * Decimal(1000) * TIMER_WALL_RELATIVE_SLACK,
    )
    if total_ms > wall * Decimal(1000) + timer_slack:
        raise AcceptanceError(f"{label}: product total_ms exceeds runner wall clock plus timer slack")
    wall_ms = wall * Decimal(1000)
    if wall_ms > total_ms * MAX_WALL_OVER_TIMER_RATIO + MAX_WALL_OVER_TIMER_SLACK_MS:
        raise AcceptanceError(
            f"{label}: runner wall clock is implausibly larger than product total_ms"
        )
    if user_cpu + system_cpu < wall * MIN_CPU_WALL_RATIO:
        raise AcceptanceError(
            f"{label}: user+system CPU is below "
            f"{decimal_text(MIN_CPU_WALL_RATIO)}x runner wall clock"
        )
    active = parse_uint(row["active_patterns"], f"{label} active_patterns", positive=True)
    if active < MIN_ACTIVE_PATTERNS:
        raise AcceptanceError(f"{label}: active_patterns={active}, expected at least {MIN_ACTIVE_PATTERNS}")
    rss = parse_uint(row["peak_sampled_rss_kb"], f"{label} peak_sampled_rss_kb", positive=True)
    if parse_uint(row["peak_sampled_swap_kb"], f"{label} peak_sampled_swap_kb") != 0:
        raise AcceptanceError(f"{label}: peak sampled swap is nonzero")
    expected_limits = {
        "process_rss_limit_bytes": RSS_LIMIT_BYTES,
        "configured_chart_memory_budget": MEMORY_BUDGET_BYTES,
        "manifest_rss_limit_bytes": RSS_LIMIT_BYTES,
    }
    for key, expected_limit in expected_limits.items():
        limit = parse_uint(row[key], f"{label} {key}", positive=True)
        if limit != expected_limit:
            raise AcceptanceError(
                f"{label}: {key}={limit}, expected sealed contract value {expected_limit}"
            )
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
    expected_report_path, expected_compact_path, expected_dag_path = (
        expected_phase9_trial_paths(raw_path, row, trial_index, worker)
    )
    if report_path != expected_report_path:
        raise AcceptanceError(
            f"{label}: report_path is not the exact deterministic Phase-9 timed report path"
        )
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
    )
    for report_key in reconciliation:
        if report.top[report_key] != row[report_key]:
            raise AcceptanceError(f"{label}: TSV/report {report_key} values disagree")
    for timing in ("accepted_rebuild_ms", "total_ms"):
        report_value = parse_decimal(require_top(report, timing, label), f"{label} report {timing}")
        row_value = parse_decimal(row[timing], f"{label} TSV {timing}")
        if report_value != row_value:
            raise AcceptanceError(f"{label}: TSV/report {timing} values disagree")
    if report_summary["final_score"] != scores["best_reported_objective"]:
        raise AcceptanceError(
            f"{label}: report search final disagrees with raw best-reported objective"
        )

    compact_path, full_path, sidecar_path, dag_path = canonical_paths(raw_path, row, report_path)
    if compact_path != expected_compact_path:
        raise AcceptanceError(
            f"{label}: compact canonical result is not the exact deterministic Phase-9 timed path"
        )
    if dag_path != expected_dag_path:
        raise AcceptanceError(
            f"{label}: canonical DAG result is not the exact deterministic Phase-9 score path"
        )
    run_root = raw_path.parent.absolute()
    compact_path = require_run_artifact(compact_path, run_root, f"{label} compact canonical result")
    if compact_path.stat().st_nlink != 1:
        raise AcceptanceError(
            f"{label}: compact canonical result is not singly linked"
        )
    full_path = require_run_artifact(full_path, run_root, f"{label} full canonical result")
    sidecar_path = require_run_artifact(sidecar_path, run_root, f"{label} full canonical sidecar")
    dag_path = require_run_artifact(dag_path, run_root, f"{label} canonical DAG result")
    compact = validate_search_digest(compact_path)
    full = validate_search_digest(full_path)
    if compact != full:
        raise AcceptanceError(f"{label}: compact and full search-digest components differ")
    canonical = read_full_canonical_sidecar(sidecar_path, seed, label)
    sidecar_sha = sha256_file(sidecar_path, "full canonical sidecar")
    if sidecar_sha != compact["semantic_sha256"]:
        raise AcceptanceError(f"{label}: full sidecar SHA-256 does not equal compact semantic digest")
    if compact["semantic_sha256"] != row["search_semantic_sha256"]:
        raise AcceptanceError(f"{label}: raw/timed-compact semantic digests differ")
    if compact["candidate_count"] != EXPECTED_CANDIDATES or compact["exact_candidate_count"] != EXPECTED_EXACT or compact["iteration_count"] != ITERATIONS:
        raise AcceptanceError(f"{label}: canonical search record counts do not match the frozen 32/4/3 contract")
    if compact["record_count"] != canonical["record_count"]:
        raise AcceptanceError(f"{label}: compact/full canonical record counts disagree")
    if canonical["initial_score"] != report_summary["initial_score"]:
        raise AcceptanceError(f"{label}: canonical/report search initial scores disagree")
    if canonical["final_score"] != report_summary["final_score"]:
        raise AcceptanceError(f"{label}: canonical/report search final scores disagree")
    if canonical["active_patterns"] != active:
        raise AcceptanceError(f"{label}: canonical/raw active-pattern counts disagree")
    canonical_sequence = cast(
        tuple[dict[str, object], ...], canonical["accepted_sequence"]
    )
    if (
        report_sequence_projection(canonical_sequence, current_trial=True)
        != report_summary["accepted_sequence"]
    ):
        raise AcceptanceError(f"{label}: report/full-canonical timed accepted sequences disagree")

    output = validate_dag_digest(dag_path)
    if output["semantic_sha256"] != row["output_semantic_sha256"]:
        raise AcceptanceError(f"{label}: raw/external canonical DAG semantic digests differ")
    if output["parsimony_min"] != scores["final_validated_parsimony_min"]:
        raise AcceptanceError(
            f"{label}: external canonical DAG parsimony disagrees with raw validation"
        )
    canonical_record_count = canonical["record_count"]
    if type(canonical_record_count) is not int:
        raise AcceptanceError(f"{label}: internal canonical record count is not an integer")
    return Trial(
        row=row,
        report=report,
        compact_path=compact_path,
        search_digest=compact,
        output_digest=output,
        full_sidecar_sha256=sidecar_sha,
        accepted_sequence=canonical_sequence,
        canonical_record_count=canonical_record_count,
        source_line=source_line,
        seed=seed,
        worker=worker,
        trial_index=trial_index,
    )


def trials_for(trials: Iterable[Trial], seed: int, worker: int) -> list[Trial]:
    return [trial for trial in trials if trial.seed == seed and trial.worker == worker]


def validate_phase9_warmup_compacts(
    root: Path,
    trials: Sequence[Trial],
    oracle_digests: Mapping[tuple[int, int], Mapping[str, object]] | None = None,
) -> tuple[Path, ...]:
    """Bind all six commanded warmups to their row, full stream, and oracle."""

    expected_keys = {
        (seed, worker) for seed in SEEDS for worker in MEASURED_WORKERS
    }
    if oracle_digests is not None and set(oracle_digests) != expected_keys:
        raise AcceptanceError(
            "sealed frozen oracle does not provide the exact Phase-9 warmup matrix"
        )

    warmup_paths: list[Path] = []
    measured_paths = [trial.compact_path for trial in trials]
    for seed, worker in sorted(expected_keys):
        row_trials = trials_for(trials, seed, worker)
        if len(row_trials) != 3:
            raise AcceptanceError(
                f"seed {seed} W{worker}: cannot bind warmup without exactly three measured trials"
            )
        representative = min(row_trials, key=lambda trial: trial.trial_index)
        label = f"seed {seed} W{worker} warmup1 compact canonical result"
        path = expected_phase9_warmup_compact_path(
            root, representative.row, worker
        )
        path = require_run_artifact(path, root.absolute(), label)
        if path.stat().st_nlink != 1:
            raise AcceptanceError(f"{label} is not singly linked")
        digest = validate_search_digest(path)
        if digest != representative.search_digest:
            raise AcceptanceError(
                f"{label} differs from the measured row/full canonical search digest"
            )
        if (
            digest["semantic_sha256"]
            != representative.row["search_semantic_sha256"]
        ):
            raise AcceptanceError(f"{label} differs from the raw-row semantic digest")
        if digest["semantic_sha256"] != representative.full_sidecar_sha256:
            raise AcceptanceError(f"{label} differs from the full canonical sidecar")
        if oracle_digests is not None and digest != oracle_digests[(seed, worker)]:
            raise AcceptanceError(f"{label} differs from the sealed frozen oracle")
        warmup_paths.append(path)

    combined_paths = [*measured_paths, *warmup_paths]
    if len(set(combined_paths)) != len(combined_paths):
        raise AcceptanceError(
            "a compact canonical result path is reused across Phase-9 warmups/measured trials"
        )
    identities = {
        (path.stat().st_dev, path.stat().st_ino) for path in combined_paths
    }
    if len(identities) != len(combined_paths):
        raise AcceptanceError(
            "compact canonical results are aliased across Phase-9 warmups/measured trials"
        )
    return tuple(warmup_paths)


def frozen_oracle_search_digests(
    audited: object,
) -> dict[tuple[int, int], dict[str, object]]:
    evidence = getattr(audited, "evidence")
    return {
        (seed, worker): validate_search_digest(
            evidence[(seed, worker)].canonical_result
        )
        for seed in SEEDS
        for worker in MEASURED_WORKERS
    }


def select_matrix(raw_path: Path, rows: Sequence[dict[str, str]]) -> list[Trial]:
    expected_ids = {
        (seed, worker): DEFAULT_ROW_ID_TEMPLATE.format(seed=seed, worker=worker)
        for seed in SEEDS
        for worker in MEASURED_WORKERS
    }
    repetitions = 3
    if len(rows) != len(expected_ids) * repetitions:
        raise AcceptanceError(
            f"current Phase-9 run must contain exactly {len(expected_ids) * repetitions} "
            f"timed W1/W8 rows; found {len(rows)}"
        )

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
    unexpected = sorted({row["row_id"] for row in rows} - selected_ids)
    if unexpected:
        raise AcceptanceError(f"unexpected rows in sealed Phase-9 current run: {', '.join(unexpected)}")

    compact_paths = [trial.compact_path for trial in trials]
    if len(set(compact_paths)) != len(compact_paths):
        raise AcceptanceError(
            "a compact canonical result path is reused across Phase-9 measured trials"
        )
    compact_identities = {
        (path.stat().st_dev, path.stat().st_ino) for path in compact_paths
    }
    if len(compact_identities) != len(compact_paths):
        raise AcceptanceError(
            "compact canonical results are aliased across Phase-9 measured trials"
        )
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
        for worker in MEASURED_WORKERS:
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
        representative_sequence = seed_trials[0].accepted_sequence
        if any(trial.accepted_sequence != representative_sequence for trial in seed_trials[1:]):
            raise AcceptanceError(f"seed {seed}: timed accepted sequence differs across W1/W8 repeats")
        if len({trial.full_sidecar_sha256 for trial in seed_trials}) != 1:
            raise AcceptanceError(f"seed {seed}: full canonical byte stream differs across W1/W8 repeats")
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


def report_phase_share(report: ParsedReport, label: str) -> Decimal:
    """Return the accepted-update share for one fully parsed product report."""

    accepted_ms = parse_decimal(
        require_top(report, "accepted_rebuild_ms", label),
        f"{label} accepted_rebuild_ms",
        positive=True,
    )
    total_ms = parse_decimal(
        require_top(report, "total_ms", label),
        f"{label} total_ms",
        positive=True,
    )
    if accepted_ms > total_ms:
        raise AcceptanceError(f"{label}: accepted_rebuild_ms exceeds total_ms")
    return accepted_ms / total_ms


def frozen_profile_requires_speed_gate(shares: Sequence[Decimal]) -> bool:
    """A phase exemption is valid only when every frozen representative is small."""

    if not shares:
        raise AcceptanceError("frozen profile has no accepted-update phase shares")
    return any(share >= MIN_PHASE_SHARE for share in shares)


def validate_current_score_domain_bindings(
    row: Mapping[str, str], frozen_row: Mapping[str, str], label: str
) -> None:
    """Bind distinct search-objective and external-DAG score domains."""

    bindings = {
        "initial_validated_parsimony_min": "expected_initial_score",
        "final_validated_parsimony_min": "expected_validated_parsimony",
        "best_reported_objective": "expected_final_score",
        "best_validated_parsimony_min": "expected_validated_parsimony",
    }
    for current_field, frozen_field in bindings.items():
        if row[current_field] != frozen_row[frozen_field]:
            raise AcceptanceError(
                f"{label}: current {current_field} does not match sealed supplement "
                f"{frozen_field}"
            )


def audit_sealed_frozen_archive(args: argparse.Namespace, trials: Sequence[Trial]):
    try:
        import wric_phase9_manifest_bootstrap as bootstrap  # noqa: PLC0415
    except (ImportError, OSError) as error:
        raise AcceptanceError(f"cannot load sealed Phase-9 supplement audit: {error}") from error
    try:
        audited = bootstrap.audited_frozen_characterization(
            Path(args.base_manifest),
            args.expected_parent_sha256,
            Path(args.supplement),
            Path(args.base_repo_root),
        )
    except bootstrap.BootstrapError as error:
        raise AcceptanceError(f"sealed Phase-9 supplement audit failed: {error}") from error

    current_reports = {trial.report.path for trial in trials}
    frozen_paths = {
        path
        for evidence in audited.evidence.values()
        for path in (
            evidence.product_report,
            evidence.canonical_sidecar,
            evidence.canonical_result,
            evidence.output_canonical,
        )
    }
    overlap = current_reports & frozen_paths
    if overlap:
        raise AcceptanceError(
            f"same-revision measured reports alias frozen archive evidence: {sorted(map(os.fspath, overlap))}"
        )

    supplement_rows = {
        (int(row["seed"]), int(row["requested_workers"])): row
        for row in audited.supplement.rows
    }
    frozen_result: dict[str, object] = {}
    frozen_sequences: dict[int, tuple[dict[str, object], ...]] = {}
    frozen_phase_shares: list[Decimal] = []
    for seed in SEEDS:
        seed_rows: dict[str, object] = {}
        for worker in WORKERS:
            evidence = audited.evidence[(seed, worker)]
            label = f"sealed frozen seed {seed} W{worker}"
            report = parse_report(evidence.product_report)
            summary = validate_report_characterization(report, seed, label)
            canonical = read_full_canonical_sidecar(evidence.canonical_sidecar, seed, label)
            canonical_sequence = cast(
                tuple[dict[str, object], ...], canonical["accepted_sequence"]
            )
            if report_sequence_projection(canonical_sequence) != summary["accepted_sequence"]:
                raise AcceptanceError(f"{label}: report/full-canonical accepted sequences disagree")
            accepted_share = report_phase_share(report, label)
            frozen_phase_shares.append(accepted_share)
            if seed == 1 and worker == 1:
                accepted_ms = parse_decimal(
                    require_top(report, "accepted_rebuild_ms", label),
                    f"{label} accepted_rebuild_ms",
                    positive=True,
                )
                if accepted_ms < MIN_FROZEN_ACCEPTED_REBUILD_MS:
                    raise AcceptanceError(
                        f"{label}: accepted_rebuild_ms is below {MIN_FROZEN_ACCEPTED_REBUILD_MS} ms"
                    )
            seed_rows[str(worker)] = {
                "row_id": supplement_rows[(seed, worker)]["row_id"],
                "product_report_sha256": evidence.product_report_sha256,
                "canonical_sidecar_sha256": evidence.canonical_sidecar_sha256,
                "canonical_result_sha256": evidence.canonical_result_sha256,
                "output_canonical_sha256": evidence.output_canonical_sha256,
                "active_patterns": summary["active_patterns"],
                "search_initial_score": summary["initial_score"],
                "search_final_score": summary["final_score"],
                "external_initial_parsimony_min": audited.input_evidence.parsimony_min,
                "external_final_parsimony_min": parse_uint(
                    evidence.expected["expected_validated_parsimony"],
                    f"{label} expected_validated_parsimony",
                ),
                "accepted_update_phase_share": decimal_text(accepted_share),
                "combined_rows_recomputed_on_commit": summary[
                    "combined_rows_recomputed_on_commit"
                ],
                "canonical_record_count": canonical["record_count"],
            }
            if worker == 1:
                frozen_sequences[seed] = canonical_sequence
        frozen_result[str(seed)] = seed_rows

    expected_bindings = {
        "candidates_generated": "expected_candidates_generated",
        "candidates_scored": "expected_candidates_scored",
        "exact_verifications": "expected_exact_verifications",
        "accepted_moves": "expected_accepted_moves",
        "iterations": "expected_iterations",
        "active_patterns": "expected_active_patterns",
        "input_sha256": "primary_sha256",
        "search_semantic_sha256": "oracle_search_semantic_sha256",
        "output_semantic_sha256": "oracle_output_semantic_sha256",
        "canonical_argv_sha256": "canonical_argv_sha256",
        "trial_semantic_sha256": "oracle_trial_semantic_sha256",
        "canonical_digest": "oracle_trial_semantic_sha256",
        "process_rss_limit_bytes": "rss_limit_bytes",
        "manifest_rss_limit_bytes": "rss_limit_bytes",
        "configured_chart_memory_budget": "memory_budget_bytes",
    }
    for trial in trials:
        frozen_row = supplement_rows[(trial.seed, trial.worker)]
        expected_row_id = bootstrap.row_id(trial.seed, trial.worker)
        if trial.row_id != expected_row_id or frozen_row["row_id"] != expected_row_id:
            raise AcceptanceError(
                f"seed {trial.seed} W{trial.worker}: current/supplement canonical row IDs disagree"
            )
        if trial.row["fixture"] != bootstrap.workload_name(trial.seed):
            raise AcceptanceError(
                f"{trial.row_id}: current fixture name is not the supplement workload name"
            )
        if trial.row["refseq_sha256"] != "NA":
            raise AcceptanceError(f"{trial.row_id}: Phase-9 DAG fixture must have refseq_sha256=NA")
        validate_current_score_domain_bindings(
            trial.row,
            frozen_row,
            f"{trial.row_id} trial {trial.trial_index}",
        )
        for current_field, frozen_field in expected_bindings.items():
            if trial.row[current_field] != frozen_row[frozen_field]:
                raise AcceptanceError(
                    f"{trial.row_id} trial {trial.trial_index}: current {current_field} "
                    f"does not match sealed supplement {frozen_field}"
                )
        if trial.accepted_sequence != frozen_sequences[trial.seed]:
            raise AcceptanceError(
                f"{trial.row_id} trial {trial.trial_index}: current accepted sequence "
                "does not match sealed frozen evidence"
            )
    return audited, frozen_result, frozen_profile_requires_speed_gate(
        frozen_phase_shares
    )


def require_lexical_directory(path: Path, label: str) -> Path:
    lexical = path.absolute()
    try:
        info = lexical.lstat()
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"{label} is missing: {path}: {error}") from error
    if not stat.S_ISDIR(info.st_mode) or lexical != resolved:
        raise AcceptanceError(f"{label} must be a canonical, non-symlink directory: {path}")
    return resolved


def ledger_relative(path: Path, root: Path, label: str) -> str:
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise AcceptanceError(f"{label} escapes run archive: {path}") from error
    pure = PurePosixPath(relative.as_posix())
    if (
        pure.is_absolute()
        or not pure.parts
        or any(SAFE_LEDGER_COMPONENT.fullmatch(part) is None for part in pure.parts)
        or pure.as_posix() != relative.as_posix()
    ):
        raise AcceptanceError(f"{label} has an unsafe/noncanonical archive path: {relative}")
    return pure.as_posix()


def run_archive_members(root: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    identities: set[tuple[int, int]] = set()
    excluded = {RUN_LEDGER_NAME, RUN_LEDGER_SEAL_NAME}
    for directory, directory_names, file_names in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in tuple(directory_names):
            path = parent / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
                raise AcceptanceError(f"run archive contains a non-directory or symlink directory: {path}")
            ledger_relative(path, root, "run archive directory")
        for name in file_names:
            path = parent / name
            relative = ledger_relative(path, root, "run archive member")
            if relative in excluded:
                continue
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                raise AcceptanceError(f"run archive contains a non-regular or symlink file: {path}")
            identity = (info.st_dev, info.st_ino)
            if info.st_nlink != 1 or identity in identities:
                raise AcceptanceError(f"run archive contains a hard-linked/aliased file: {path}")
            identities.add(identity)
            if relative in result:
                raise AcceptanceError(f"duplicate run archive member: {relative}")
            result[relative] = path
    return result


def fsync_directory(path: Path, label: str) -> None:
    descriptor: int | None = None
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        os.fsync(descriptor)
    except OSError as error:
        raise AcceptanceError(f"cannot durably sync {label} directory {path}: {error}") from error
    finally:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass


def exclusive_write(path: Path, payload: bytes, label: str, mode: int = 0o444) -> None:
    descriptor: int | None = None
    created = False

    def rollback() -> None:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if created:
            try:
                path.chmod(0o644)
                path.unlink()
                fsync_directory(path.parent, f"rolled-back {label}")
            except (OSError, AcceptanceError):
                pass

    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
        created = True
        with os.fdopen(descriptor, "wb") as handle:
            descriptor = None
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        fsync_directory(path.parent, label)
    except AcceptanceError:
        rollback()
        raise
    except OSError as error:
        rollback()
        raise AcceptanceError(f"cannot exclusively write {label} {path}: {error}") from error


def detached_seal_payload(path: Path) -> bytes:
    return f"{sha256_file(path, 'sealed artifact')}  {path.name}\n".encode("ascii")


def render_run_ledger(root: Path, members: Mapping[str, Path]) -> bytes:
    lines = [
        f"# schema={RUN_LEDGER_SCHEMA}",
        f"# schema_version={RUN_SCHEMA_VERSION}",
        "\t".join(RUN_LEDGER_HEADER),
    ]
    for relative, path in sorted(members.items()):
        lines.append(
            "\t".join(
                (
                    sha256_file(path, "run archive member"),
                    str(path.stat().st_size),
                    relative,
                )
            )
        )
    return ("\n".join(lines) + "\n").encode("ascii")


def audited_fixture_sha256(audited) -> str:
    values = {row["primary_sha256"] for row in audited.supplement.rows}
    if len(values) != 1:
        raise AcceptanceError("audited supplement has inconsistent Phase-9 fixture hashes")
    return next(iter(values))


def git_output(root: Path, arguments: Sequence[str], label: str) -> str:
    try:
        completed = subprocess.run(
            ["/usr/bin/git", "-C", os.fspath(root), *arguments],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={
                "LC_ALL": "C",
                "PATH": "/usr/bin:/bin",
                "HOME": "/nonexistent",
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": "/dev/null",
            },
        )
    except OSError as error:
        raise AcceptanceError(f"cannot execute git for {label}: {error}") from error
    if completed.returncode != 0:
        raise AcceptanceError(
            f"git {label} failed: {completed.stderr.strip() or completed.stdout.strip()}"
        )
    return completed.stdout


def validate_git_toplevel(root: Path, label: str) -> Path:
    """Reject aliases and subdirectories; return one exact Git worktree root."""

    root = require_lexical_directory(root.absolute(), label)
    top_text = git_output(root, ("rev-parse", "--show-toplevel"), label).strip()
    if not top_text:
        raise AcceptanceError(f"git returned an empty toplevel for {label}: {root}")
    top = require_lexical_directory(Path(top_text).absolute(), f"Git {label} toplevel")
    if top != root:
        raise AcceptanceError(f"{label} {root} is not exact Git toplevel {top}")
    return root


def validate_git_revision(root: Path, expected_revision: str, label: str) -> str:
    """Bind a canonical worktree to an external full revision."""

    root = validate_git_toplevel(root, label)
    if re.fullmatch(r"[0-9a-f]{40,64}", expected_revision) is None:
        raise AcceptanceError(f"{label} expected revision is not a canonical object ID")
    head = git_output(root, ("rev-parse", "--verify", "HEAD"), "HEAD").strip()
    if re.fullmatch(r"[0-9a-f]{40,64}", head) is None:
        raise AcceptanceError(f"Git HEAD is not a canonical object ID: {head!r}")
    if head != expected_revision:
        raise AcceptanceError(
            f"{label} revision {expected_revision} does not equal live Git HEAD {head}"
        )
    return head


def validate_clean_git_checkout(root: Path, expected_revision: str | None = None) -> str:
    """Return HEAD only for a canonical checkout with no tracked or untracked dirt."""

    root = validate_git_toplevel(root, "working repository root")
    if expected_revision is None:
        head = git_output(root, ("rev-parse", "--verify", "HEAD"), "HEAD").strip()
        if re.fullmatch(r"[0-9a-f]{40,64}", head) is None:
            raise AcceptanceError(f"Git HEAD is not a canonical object ID: {head!r}")
    else:
        head = validate_git_revision(root, expected_revision, "working repository root")
    status = git_output(
        root,
        ("status", "--porcelain=v1", "--untracked-files=all"),
        "clean-status audit",
    )
    if status:
        first = status.splitlines()[0]
        raise AcceptanceError(
            f"repository is not completely clean (first tracked/untracked entry: {first})"
        )
    return head


def validate_base_repo_root(root: Path, expected_revision: str) -> Path:
    """Validate the persistent Phase-0 asset root without requiring it clean."""

    root = validate_git_toplevel(root, "sealed base repository root")
    validate_git_revision(root, expected_revision, "sealed base repository root")
    return root


def validate_working_repo_root(root: Path, expected_revision: str) -> Path:
    """Validate the clean current checkout that owns this acceptance script."""

    root = validate_git_toplevel(root, "working repository root")
    if root != REPOSITORY_ROOT:
        raise AcceptanceError(
            f"working repository root must be this acceptance checkout {REPOSITORY_ROOT}"
        )
    validate_clean_git_checkout(root, expected_revision)
    return root


def production_paths(working_root: Path) -> tuple[Path, Path, Path]:
    """Derive product paths only from the externally supplied working root."""

    return (
        working_root / "build" / "bin" / "larch2",
        working_root / "build" / "bin" / "dagutil",
        working_root / "tools" / "wric_spr_search_benchmark.sh",
    )


def require_exact_production_tool(argument: str, expected: Path, label: str) -> Path:
    path = Path(argument).absolute()
    try:
        resolved = path.resolve(strict=True)
        info = path.lstat()
        expected_resolved = expected.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"{label} is missing: {path}: {error}") from error
    if (
        path != resolved
        or resolved != expected_resolved
        or not stat.S_ISREG(info.st_mode)
        or not os.access(path, os.X_OK)
    ):
        raise AcceptanceError(f"{label} must be exact production executable {expected_resolved}")
    return resolved


def option_tokens(row: Mapping[str, str]) -> list[str]:
    """Reproduce the production harness's Phase-9 manifest option ordering."""

    result = ["--validate"]
    if row["force_no_vcf"] != "false":
        result.append("--force-no-vcf")
    result.extend(
        (
            "--wric-polytomy-mode",
            row["polytomy_mode"],
            "--wric-polytomy-max-exact-arity",
            row["polytomy_max_exact_arity"],
            "--wric-polytomy-max-shapes",
            row["polytomy_max_shapes"],
            "--wric-polytomy-max-productions",
            row["polytomy_max_productions"],
            "--wric-polytomy-max-clades",
            row["polytomy_max_clades"],
            "--wric-lazy-chart",
            row["lazy_policy"],
            "--chart-spr-search",
            "--chart-spr-max-iterations",
            row["iterations"],
            "--chart-spr-max-candidates",
            row["chart_max_candidates"],
            "--chart-spr-top-k-exact",
            row["chart_top_k_exact"],
            "--chart-spr-candidate-selection",
            row["candidate_selection"],
            "--chart-spr-candidate-source",
            row["candidate_source"],
            "--chart-spr-acceptance",
            row["acceptance"],
        )
    )
    if row["topology_selector"] != "none":
        result.extend(("--chart-spr-topology-selector", row["topology_selector"]))
    for field, flag in (
        ("randomize_order", "--chart-spr-randomize-order"),
        ("reservoir_sample", "--chart-spr-reservoir-sample"),
        ("include_immediate_reversals", "--chart-spr-include-immediate-reversals"),
    ):
        if row[field] != "false":
            result.append(flag)
    result.extend(
        (
            "--chart-spr-sampled-tree-count",
            row["sampled_tree_count"],
            "--chart-spr-sampled-tree-radius",
            row["sampled_tree_radius"],
            "--chart-spr-max-upward-path-expansions",
            row["max_upward_path_expansions"],
            "--chart-spr-max-path-pairs",
            row["max_path_pairs"],
            "--chart-spr-min-moved-clade-size",
            row["min_moved_clade_size"],
            "--chart-spr-max-moved-clade-size",
            row["max_moved_clade_size"],
            "--chart-spr-min-target-clade-size",
            row["min_target_clade_size"],
            "--chart-spr-max-target-clade-size",
            row["max_target_clade_size"],
            "--chart-spr-max-cached-patterns",
            row["max_cached_patterns"],
            "--chart-spr-pattern-batch-size",
            row["pattern_batch_size"],
            "--chart-spr-candidate-batch-size",
            row["candidate_batch_size"],
            "--chart-spr-memory-budget",
            row["memory_budget_bytes"],
            "--chart-spr-commit-mode",
            row["commit_mode"],
            "--chart-spr-verification-mode",
            row["verification_mode"],
            "--chart-bnb-dominance",
            row["dominance_mode"],
        )
    )
    if row["max_frontier_entries"] != "0":
        result.extend(("--chart-bnb-max-frontier", row["max_frontier_entries"]))
    if row["local_accept_updates"] != "false":
        result.append("--chart-spr-local-accept-updates")
    if row["bound_pruning"] != "true":
        result.append("--chart-bnb-no-bound-pruning")
    if row["require_exact_keep_mask"] != "true":
        result.append("--chart-bnb-score-only")
    if row["score_ua_edge"] != "false":
        result.append("--chart-score-ua-edge")
    result.extend(("--seed", row["seed"]))
    return result


def command_entries(path: Path) -> list[tuple[str, tuple[str, ...]]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise AcceptanceError(f"cannot read benchmark commands log: {error}") from error
    result: list[tuple[str, tuple[str, ...]]] = []
    for index, line in enumerate(lines):
        if not line.startswith("# "):
            continue
        label = line[2:]
        if index + 1 >= len(lines) or not lines[index + 1].strip():
            # A shebang or documentary comment is not run-capture evidence.
            continue
        try:
            tokens = tuple(shlex.split(lines[index + 1], posix=True))
        except ValueError as error:
            raise AcceptanceError(
                f"benchmark commands log has malformed command after {label!r}: {error}"
            ) from error
        if not tokens:
            raise AcceptanceError(f"benchmark commands log has empty command after {label!r}")
        result.append((label, tokens))
    return result


def validate_phase9_commands(
    root: Path,
    commands: Path,
    audited,
    base_repo_root: Path,
    working_repo_root: Path,
) -> str:
    try:
        import wric_phase9_manifest_bootstrap as bootstrap  # noqa: PLC0415
    except (ImportError, OSError) as error:
        raise AcceptanceError(f"cannot load Phase-9 command contract: {error}") from error
    entries = command_entries(commands)
    observed: dict[str, list[tuple[str, ...]]] = {}
    for label, tokens in entries:
        observed.setdefault(label, []).append(tokens)
    relevant: list[tuple[str, tuple[str, ...]]] = []
    _, working_dagutil, _ = production_paths(working_repo_root)
    dagutil = os.fspath(working_dagutil.resolve(strict=True))
    measured_rows = [
        row
        for row in audited.supplement.rows
        if int(row["seed"]) in SEEDS and int(row["requested_workers"]) in MEASURED_WORKERS
    ]
    if len(measured_rows) != len(SEEDS) * len(MEASURED_WORKERS):
        raise AcceptanceError("sealed supplement does not expose the six measured command rows")

    def take_exact(label: str, expected: tuple[str, ...]) -> None:
        candidates = observed.get(label, [])
        if candidates != [expected]:
            raise AcceptanceError(
                f"benchmark commands log lacks exactly one production command for {label!r}"
            )
        relevant.append((label, expected))

    full_by_label: dict[str, list[tuple[str, ...]]] = {}
    for row in measured_rows:
        worker = int(row["requested_workers"])
        fixture = row["workload_name"]
        row_id = row["row_id"]
        safe = sanitize_harness_name(fixture)
        row_safe = sanitize_harness_name(row_id)
        try:
            primary = bootstrap.resolve_manifest_uri(
                audited.supplement.path, row["primary_uri"], base_repo_root
            )
        except bootstrap.BootstrapError as error:
            raise AcceptanceError(f"cannot resolve command input for {row_id}: {error}") from error
        prefix = [dagutil, "--dag-pb", os.fspath(primary), *option_tokens(row)]
        if row["worker_option"] != "chart_spr_workers":
            raise AcceptanceError(f"{row_id}: measured row does not use chart_spr_workers")
        timed_prefix = [*prefix, "--chart-spr-workers", str(worker)]
        for suffix in ("warmup1", "trial1", "trial2", "trial3"):
            execution_suffix = f"{suffix}_{row_safe}"
            output = root / "outputs" / f"{safe}_{METHOD}_{execution_suffix}_w{worker}.pb.gz"
            compact = root / "logs" / f"{safe}_{METHOD}_{execution_suffix}_w{worker}.canonical.json"
            label = f"{fixture} {METHOD} workers={worker} {execution_suffix}"
            take_exact(
                label,
                tuple(
                    (
                        *timed_prefix,
                        "--chart-spr-canonical-result",
                        os.fspath(compact),
                        "-o",
                        os.fspath(output),
                    )
                ),
            )

        companion_stem = f"{safe}_{row_safe}_canonical_companion"
        companion_json = root / "logs" / f"{companion_stem}.json"
        companion_output = root / "outputs" / f"{companion_stem}.pb.gz"
        companion_label = f"{fixture} {METHOD} deferred semantic companion workers={worker}"
        take_exact(
            companion_label,
            tuple(
                (
                    *timed_prefix,
                    "--chart-spr-canonical-result",
                    os.fspath(companion_json),
                    "-o",
                    os.fspath(companion_output),
                )
            ),
        )

        full_stem = f"{safe}_{row_safe}_full_canonical"
        full_json = root / "logs" / f"{full_stem}.json"
        full_sidecar = root / "logs" / f"{full_stem}.ndjson"
        full_output = root / "outputs" / f"{full_stem}.pb.gz"
        full_label = f"{fixture} {METHOD} deferred explicit-W1 full correctness"
        expected_full = tuple(
            (
                *prefix,
                "--chart-spr-workers",
                "1",
                "--chart-spr-canonical-result",
                os.fspath(full_json),
                "--chart-spr-canonical-sidecar",
                os.fspath(full_sidecar),
                "-o",
                os.fspath(full_output),
            )
        )
        full_by_label.setdefault(full_label, []).append(expected_full)

    for label, expected_commands in full_by_label.items():
        candidates = observed.get(label, [])
        if sorted(candidates) != sorted(expected_commands):
            raise AcceptanceError(
                f"benchmark commands log full-canonical commands for {label!r} are not exact"
            )
        relevant.extend((label, command) for command in expected_commands)

    digest = hashlib.sha256()
    digest.update(b"wric-phase9-production-command-contract-v1\n")
    for label, tokens in sorted(relevant):
        digest.update(f"label={len(label)}:{label}\n".encode("utf-8"))
        digest.update(f"argc={len(tokens)}\n".encode("ascii"))
        for token in tokens:
            digest.update(f"{len(token)}:{token}\n".encode("utf-8"))
    return digest.hexdigest()


def validate_run_configuration(
    root: Path,
    row_ids: Sequence[str],
    audited,
    base_repo_root: Path,
    working_repo_root: Path,
) -> dict[str, str]:
    summary = require_run_artifact(root / "summary.md", root, "benchmark summary")
    commands = require_run_artifact(root / "commands.sh", root, "benchmark commands log")
    try:
        summary_lines = summary.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise AcceptanceError(f"cannot read benchmark configuration evidence: {error}") from error
    configuration = [line for line in summary_lines if line.startswith("Configuration: ")]
    if len(configuration) != 1 or "workers=1 8, warmups=1, repetitions=3," not in configuration[0]:
        raise AcceptanceError(
            "benchmark summary does not document workers=1,8 with exactly one warmup and three repetitions"
        )
    if set(row_ids) != {
        DEFAULT_ROW_ID_TEMPLATE.format(seed=seed, worker=worker)
        for seed in SEEDS
        for worker in MEASURED_WORKERS
    }:
        raise AcceptanceError("run configuration row IDs are not the exact Phase-9 matrix")
    command_contract = validate_phase9_commands(
        root, commands, audited, base_repo_root, working_repo_root
    )
    return {
        "summary_sha256": sha256_file(summary, "benchmark summary"),
        "commands_sha256": sha256_file(commands, "benchmark commands log"),
        "command_contract_sha256": command_contract,
    }


def seal_run(args: argparse.Namespace) -> dict[str, object]:
    root = require_lexical_directory(Path(args.benchmark_dir), "benchmark directory")
    raw_path = require_run_artifact(root / "raw_trials.tsv", root, "raw trials")
    _, rows = read_tsv(raw_path)
    trials = select_matrix(raw_path, rows)
    audited, _, _ = audit_sealed_frozen_archive(args, trials)
    validate_phase9_warmup_compacts(
        root, trials, frozen_oracle_search_digests(audited)
    )
    if args.warmups != 1:
        raise AcceptanceError("final Phase-9 run metadata requires exactly one warmup per row")
    if not args.full_canonical:
        raise AcceptanceError("seal-run requires explicit --full-canonical evidence")
    affinity = args.affinity_cpus
    if affinity != audited.characterization.preamble["affinity_cpus"]:
        raise AcceptanceError("current-run affinity differs from the sealed supplement affinity")
    if re.fullmatch(r"[0-9a-f]{40,64}", args.working_revision) is None:
        raise AcceptanceError("--working-revision is not a canonical Git object ID")
    base_root = validate_base_repo_root(
        Path(args.base_repo_root), audited.base.preamble["repo_revision"]
    )
    working_root = validate_working_repo_root(
        Path(args.working_repo_root), args.working_revision
    )
    head = args.working_revision
    production_larch2, production_dagutil, production_harness = production_paths(
        working_root
    )
    larch2 = require_exact_production_tool(
        args.working_larch2, production_larch2, "working larch2"
    )
    dagutil = require_exact_production_tool(
        args.working_dagutil, production_dagutil, "working dagutil"
    )
    harness = require_exact_production_tool(
        args.benchmark_harness, production_harness, "benchmark harness"
    )
    tool_hashes = {
        "working_larch2_sha256": sha256_file(larch2, "working larch2"),
        "working_dagutil_sha256": sha256_file(dagutil, "working dagutil"),
        "benchmark_harness_sha256": sha256_file(harness, "benchmark harness"),
    }

    metadata_path = root / RUN_METADATA_NAME
    ledger_path = root / RUN_LEDGER_NAME
    seal_path = root / RUN_LEDGER_SEAL_NAME
    for path in (metadata_path, ledger_path, seal_path):
        if path.exists() or path.is_symlink():
            raise AcceptanceError(f"run seal destination is already occupied: {path}")
    row_ids = sorted({trial.row_id for trial in trials})
    configuration_hashes = validate_run_configuration(
        root, row_ids, audited, base_root, working_root
    )
    metadata: dict[str, object] = {
        "schema": RUN_SCHEMA,
        "schema_version": RUN_SCHEMA_VERSION,
        "role": "same_revision_phase9_current_measurement",
        "run_group": RUN_GROUP,
        "seeds": list(SEEDS),
        "workers": list(MEASURED_WORKERS),
        "repetitions": 3,
        "warmups_per_row": 1,
        "full_canonical": True,
        "affinity_cpus": affinity,
        "base_manifest_sha256": audited.base.sha256,
        "supplement_manifest_sha256": audited.supplement.sha256,
        "characterization_sha256": audited.characterization.sha256,
        "fixture_sha256": audited_fixture_sha256(audited),
        "base_repo_root": os.fspath(base_root),
        "base_revision": audited.base.preamble["repo_revision"],
        "working_repo_root": os.fspath(working_root),
        "working_revision": head,
        "repository_status": "clean",
        "binary_provenance_limit": (
            "binary hashes bind the measured executables, but no reproducible-build "
            "attestation proves derivation from HEAD"
        ),
        **tool_hashes,
        "raw_trials_sha256": sha256_file(raw_path, "raw trials"),
        **configuration_hashes,
        "raw_trial_rows": len(rows),
        "row_ids": row_ids,
    }
    created: list[Path] = []
    try:
        exclusive_write(
            metadata_path,
            (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8"),
            "run metadata",
        )
        created.append(metadata_path)
        members = run_archive_members(root)
        if RUN_METADATA_NAME not in members:
            raise AcceptanceError("run artifact closure omits run metadata")
        exclusive_write(ledger_path, render_run_ledger(root, members), "run artifact ledger")
        created.append(ledger_path)
        exclusive_write(seal_path, detached_seal_payload(ledger_path), "run ledger seal")
        created.append(seal_path)
    except BaseException:
        rollback_parents: set[Path] = set()
        for path in reversed(created):
            try:
                path.chmod(0o644)
                path.unlink()
                rollback_parents.add(path.parent)
            except OSError:
                pass
        for parent in rollback_parents:
            try:
                fsync_directory(parent, "rolled-back run seal")
            except AcceptanceError:
                pass
        raise
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": "sealed",
        "benchmark_dir": os.fspath(root),
        "metadata_sha256": sha256_file(metadata_path, "run metadata"),
        "ledger_sha256": sha256_file(ledger_path, "run artifact ledger"),
        "artifact_count": len(members),
    }


def verify_expected_run_ledger_anchor(root: Path, expected_sha256: str) -> str:
    if SHA256_RE.fullmatch(expected_sha256) is None:
        raise AcceptanceError("expected current-run ledger SHA-256 is not canonical")
    root = require_lexical_directory(root, "benchmark directory")
    ledger = require_run_artifact(root / RUN_LEDGER_NAME, root, "run artifact ledger")
    observed = sha256_file(ledger, "run artifact ledger")
    if observed != expected_sha256:
        raise AcceptanceError(
            "current-run ledger SHA-256 does not match the external expected anchor"
        )
    return observed


def audit_run_archive(
    root_text: str,
    audited,
    base_repo_root_text: str,
    working_repo_root_text: str,
    expected_ledger_sha256: str,
) -> tuple[dict[str, object], dict[str, object]]:
    root = require_lexical_directory(Path(root_text), "benchmark directory")
    ledger_path = require_run_artifact(root / RUN_LEDGER_NAME, root, "run artifact ledger")
    seal_path = require_run_artifact(root / RUN_LEDGER_SEAL_NAME, root, "run ledger seal")
    observed_ledger_sha256 = verify_expected_run_ledger_anchor(
        root, expected_ledger_sha256
    )
    metadata_path = require_run_artifact(root / RUN_METADATA_NAME, root, "run metadata")
    if ledger_path.stat().st_nlink != 1 or seal_path.stat().st_nlink != 1:
        raise AcceptanceError("run artifact ledger or detached seal is externally hard-linked")
    metadata = read_json_object(metadata_path, "run metadata")
    require_exact_json_keys(metadata, RUN_METADATA_KEYS, "run metadata")
    base_root = validate_base_repo_root(
        Path(base_repo_root_text), audited.base.preamble["repo_revision"]
    )
    working_revision = metadata.get("working_revision")
    if not isinstance(working_revision, str) or re.fullmatch(
        r"[0-9a-f]{40,64}", working_revision
    ) is None:
        raise AcceptanceError("run metadata working_revision is invalid")
    working_root = validate_working_repo_root(
        Path(working_repo_root_text), working_revision
    )
    literals: Mapping[str, object] = {
        "schema": RUN_SCHEMA,
        "schema_version": RUN_SCHEMA_VERSION,
        "role": "same_revision_phase9_current_measurement",
        "run_group": RUN_GROUP,
        "seeds": list(SEEDS),
        "workers": list(MEASURED_WORKERS),
        "repetitions": 3,
        "warmups_per_row": 1,
        "full_canonical": True,
        "base_manifest_sha256": audited.base.sha256,
        "supplement_manifest_sha256": audited.supplement.sha256,
        "characterization_sha256": audited.characterization.sha256,
        "fixture_sha256": audited_fixture_sha256(audited),
        "base_repo_root": os.fspath(base_root),
        "base_revision": audited.base.preamble["repo_revision"],
        "working_repo_root": os.fspath(working_root),
        "repository_status": "clean",
        "binary_provenance_limit": (
            "binary hashes bind the measured executables, but no reproducible-build "
            "attestation proves derivation from HEAD"
        ),
        "raw_trial_rows": len(SEEDS) * len(MEASURED_WORKERS) * 3,
        "row_ids": sorted(
            DEFAULT_ROW_ID_TEMPLATE.format(seed=seed, worker=worker)
            for seed in SEEDS
            for worker in MEASURED_WORKERS
        ),
    }
    for key, expected in literals.items():
        actual = metadata.get(key)
        if actual != expected or type(actual) is not type(expected):
            raise AcceptanceError(f"run metadata {key}={actual!r}, expected {expected!r}")
    if metadata["affinity_cpus"] != audited.characterization.preamble["affinity_cpus"]:
        raise AcceptanceError("run metadata affinity differs from sealed supplement")
    production_larch2, production_dagutil, production_harness = production_paths(
        working_root
    )
    live_tool_hashes = {
        "working_larch2_sha256": sha256_file(
            require_exact_production_tool(
                os.fspath(production_larch2), production_larch2, "working larch2"
            ),
            "working larch2",
        ),
        "working_dagutil_sha256": sha256_file(
            require_exact_production_tool(
                os.fspath(production_dagutil), production_dagutil, "working dagutil"
            ),
            "working dagutil",
        ),
        "benchmark_harness_sha256": sha256_file(
            require_exact_production_tool(
                os.fspath(production_harness), production_harness, "benchmark harness"
            ),
            "benchmark harness",
        ),
    }
    for key in (
        "working_larch2_sha256",
        "working_dagutil_sha256",
        "benchmark_harness_sha256",
        "raw_trials_sha256",
        "summary_sha256",
        "commands_sha256",
        "command_contract_sha256",
    ):
        value = metadata[key]
        if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
            raise AcceptanceError(f"run metadata {key} is not a lowercase SHA-256")
    for key, expected in live_tool_hashes.items():
        if metadata[key] != expected:
            raise AcceptanceError(f"run metadata {key} differs from the live production tool")
    raw_path = require_run_artifact(root / "raw_trials.tsv", root, "raw trials")
    if sha256_file(raw_path, "raw trials") != metadata["raw_trials_sha256"]:
        raise AcceptanceError("run metadata raw-trials hash mismatch")
    configuration_hashes = validate_run_configuration(
        root,
        cast(list[str], metadata["row_ids"]),
        audited,
        base_root,
        working_root,
    )
    for key, value in configuration_hashes.items():
        if metadata[key] != value:
            raise AcceptanceError(f"run metadata {key} mismatch")

    expected_seal = detached_seal_payload(ledger_path)
    try:
        observed_seal = seal_path.read_bytes()
    except OSError as error:
        raise AcceptanceError(f"cannot read run ledger seal: {error}") from error
    if observed_seal != expected_seal:
        raise AcceptanceError("run artifact ledger detached seal mismatch")
    try:
        lines = ledger_path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise AcceptanceError(f"cannot read run artifact ledger: {error}") from error
    if lines[:3] != [
        f"# schema={RUN_LEDGER_SCHEMA}",
        f"# schema_version={RUN_SCHEMA_VERSION}",
        "\t".join(RUN_LEDGER_HEADER),
    ]:
        raise AcceptanceError("run artifact ledger preamble/header changed")
    listed: dict[str, tuple[str, int]] = {}
    for line_number, line in enumerate(lines[3:], 4):
        fields = line.split("\t")
        if len(fields) != 3:
            raise AcceptanceError(f"run artifact ledger line {line_number} is malformed")
        digest, size_text, relative = fields
        if SHA256_RE.fullmatch(digest) is None:
            raise AcceptanceError(f"run artifact ledger line {line_number} has invalid SHA-256")
        size = parse_uint(size_text, f"run artifact ledger line {line_number} size")
        pure = PurePosixPath(relative)
        if (
            pure.is_absolute()
            or not pure.parts
            or any(SAFE_LEDGER_COMPONENT.fullmatch(part) is None for part in pure.parts)
            or pure.as_posix() != relative
            or relative in (RUN_LEDGER_NAME, RUN_LEDGER_SEAL_NAME)
        ):
            raise AcceptanceError(f"run artifact ledger line {line_number} has unsafe path")
        if relative in listed:
            raise AcceptanceError(f"run artifact ledger duplicates {relative}")
        listed[relative] = (digest, size)
    actual = run_archive_members(root)
    if set(listed) != set(actual):
        missing = sorted(set(actual) - set(listed))
        extra = sorted(set(listed) - set(actual))
        raise AcceptanceError(
            f"run artifact ledger is not the exact archive closure: missing={missing}, extra={extra}"
        )
    for relative, path in actual.items():
        digest, size = listed[relative]
        if path.stat().st_size != size or sha256_file(path, "run archive member") != digest:
            raise AcceptanceError(f"run artifact ledger member changed: {relative}")
    if RUN_METADATA_NAME not in listed or "raw_trials.tsv" not in listed:
        raise AcceptanceError("run artifact ledger omits required metadata/raw evidence")
    return metadata, {
        "metadata_sha256": sha256_file(metadata_path, "run metadata"),
        "ledger_sha256": observed_ledger_sha256,
        "externally_anchored_ledger_sha256": expected_ledger_sha256,
        "artifact_count": len(listed),
    }


def semantic_result(trials: Sequence[Trial], seed: int) -> dict[str, object]:
    representative = trials_for(trials, seed, 1)[0]
    return {
        "score_domains": {
            "search_objective": {
                "initial": parse_uint(
                    require_top(representative.report, "initial_score", representative.row_id),
                    f"{representative.row_id} search initial",
                ),
                "final": parse_uint(
                    require_top(representative.report, "final_score", representative.row_id),
                    f"{representative.row_id} search final",
                ),
            },
            "external_dag_parsimony": {
                "initial": parse_uint(
                    representative.row["initial_validated_parsimony_min"],
                    f"{representative.row_id} external initial",
                ),
                "final": parse_uint(
                    representative.row["final_validated_parsimony_min"],
                    f"{representative.row_id} external final",
                ),
            },
        },
        "search": {key: representative.search_digest[key] for key in (*SEARCH_DIGEST_KEYS, *SEARCH_COUNT_KEYS)},
        "output": {key: representative.output_digest[key] for key in (*DAG_DIGEST_KEYS, *DAG_COUNT_KEYS)},
        "full_sidecar_sha256": representative.full_sidecar_sha256,
        "canonical_record_count": representative.canonical_record_count,
        "accepted_sequence": representative.accepted_sequence,
        "workers_compared": list(MEASURED_WORKERS),
    }


def evaluate(args: argparse.Namespace) -> dict[str, object]:
    if args.benchmark_dir:
        benchmark_dir = Path(args.benchmark_dir).resolve()
        raw_path = benchmark_dir / "raw_trials.tsv"
    else:
        raw_path = Path(args.raw_trials).resolve()
        benchmark_dir = raw_path.parent
    deferred = args.defer_frozen_oracle_characterization is not None
    if not deferred:
        # The external current-run anchor is the trust root for all bytes in
        # the run archive.  Compare it before parsing raw trials or any other
        # evidence owned by that archive.
        verify_expected_run_ledger_anchor(
            benchmark_dir, args.expected_run_ledger_sha256
        )
    _, rows = read_tsv(raw_path)
    trials = select_matrix(raw_path, rows)

    audited = None
    run_metadata: dict[str, object] | None = None
    run_ledger: dict[str, object] | None = None
    frozen_speed_gate_required = False
    if deferred:
        validate_phase9_warmup_compacts(benchmark_dir, trials)
        frozen_result: dict[str, object] = {
            "status": "deferred_non_final",
            "reason": args.defer_frozen_oracle_characterization,
            "role": "historical_fixture_characterization_only",
            "used_for_same_revision_speed_gate": False,
        }
        if args.frozen_oracle_report:
            frozen = parse_frozen_reports(
                args.frozen_oracle_report, {trial.report.path for trial in trials}
            )
            ad_hoc: dict[str, object] = {}
            for seed in SEEDS:
                summary = validate_report_characterization(
                    frozen[seed], seed, f"deferred ad-hoc frozen seed {seed}"
                )
                ad_hoc[str(seed)] = {
                    "report_path": os.fspath(frozen[seed].path),
                    "active_patterns": summary["active_patterns"],
                    "initial_score": summary["initial_score"],
                    "final_score": summary["final_score"],
                }
            frozen_result["ad_hoc_reports_validated"] = ad_hoc
    else:
        audited, frozen_rows, frozen_speed_gate_required = (
            audit_sealed_frozen_archive(args, trials)
        )
        validate_phase9_warmup_compacts(
            benchmark_dir, trials, frozen_oracle_search_digests(audited)
        )
        run_metadata, run_ledger = audit_run_archive(
            os.fspath(benchmark_dir),
            audited,
            args.base_repo_root,
            args.working_repo_root,
            args.expected_run_ledger_sha256,
        )
        frozen_result = {
            "status": "pass",
            "role": "sealed_historical_fixture_characterization_only",
            "used_for_same_revision_speed_gate": True,
            "accepted_update_speed_gate_required": frozen_speed_gate_required,
            "matrix": frozen_rows,
            "all_workers_audited": list(WORKERS),
            "all_seeds_audited": list(SEEDS),
        }

    gates: list[dict[str, object]] = [
        {"name": "measured_trial_health", "status": "pass"},
        {"name": "frozen_32_4_3_contract", "status": "pass"},
        {"name": "three_real_local_commits", "status": "pass"},
        {"name": "incremental_counter_contract", "status": "pass"},
        {"name": "scheduler_quiescence", "status": "pass"},
        {"name": "w8_inside_outside_parallel_high_water", "status": "pass"},
        {"name": "canonical_search_and_output_parity", "status": "pass"},
        {"name": "full_canonical_record_stream", "status": "pass"},
        {"name": "dual_search_and_external_score_domain_reconciliation", "status": "pass"},
        {"name": "timed_accepted_sequence_parity", "status": "pass"},
        {"name": "runner_cpu_and_timer_plausibility", "status": "pass"},
        {"name": "swap_zero", "status": "pass"},
    ]
    if deferred:
        gates.extend(
            (
                {"name": "sealed_base_parent_supplement_provenance", "status": "deferred_non_final"},
                {"name": "sealed_current_run_artifact_closure", "status": "deferred_non_final"},
                {"name": "externally_anchored_current_run_ledger", "status": "deferred_non_final"},
                {"name": "clean_head_and_production_tool_identity", "status": "deferred_non_final"},
            )
        )
    else:
        gates.extend(
            (
                {"name": "sealed_base_parent_supplement_provenance", "status": "pass"},
                {"name": "sealed_current_run_artifact_closure", "status": "pass"},
                {"name": "externally_anchored_current_run_ledger", "status": "pass"},
                {"name": "clean_head_and_production_tool_identity", "status": "pass"},
                {"name": "current_rows_bound_to_supplement", "status": "pass"},
            )
        )

    timing_result: dict[str, object] = {}
    for seed in SEEDS:
        accepted_medians = {
            worker: median(
                [parse_decimal(trial.row["accepted_rebuild_ms"], f"seed {seed} W{worker} accepted_rebuild_ms") for trial in trials_for(trials, seed, worker)],
                f"seed {seed} W{worker} accepted_rebuild_ms median",
            )
            for worker in MEASURED_WORKERS
        }
        total_medians = {
            worker: median(
                [parse_decimal(trial.row["total_ms"], f"seed {seed} W{worker} total_ms") for trial in trials_for(trials, seed, worker)],
                f"seed {seed} W{worker} total_ms median",
            )
            for worker in MEASURED_WORKERS
        }
        share = accepted_medians[1] / total_medians[1]
        current_phase_is_large = share >= MIN_PHASE_SHARE
        mandatory = current_phase_is_large or frozen_speed_gate_required
        speed_pass = accepted_medians[8] * MIN_SPEEDUP <= accepted_medians[1]
        if mandatory and not speed_pass:
            raise AcceptanceError(
                f"seed {seed}: same-revision W8 accepted_rebuild_ms median {decimal_text(accepted_medians[8])} "
                f"is not at least {decimal_text(MIN_SPEEDUP)}x faster than W1 {decimal_text(accepted_medians[1])}"
            )
        timing_result[str(seed)] = {
            "accepted_rebuild_median_ms": {str(worker): decimal_text(accepted_medians[worker]) for worker in MEASURED_WORKERS},
            "total_median_ms": {str(worker): decimal_text(total_medians[worker]) for worker in MEASURED_WORKERS},
            "w1_accepted_share_of_total": ratio_text(accepted_medians[1], total_medians[1]),
            "w8_over_w1_accepted_rebuild": ratio_text(accepted_medians[8], accepted_medians[1]),
            "current_w1_phase_at_least_threshold": current_phase_is_large,
            "frozen_profile_requires_speed_gate": (
                None if deferred else frozen_speed_gate_required
            ),
            "speed_gate": "pass" if mandatory else "exempt_below_10_percent",
        }
    gates.append({"name": "same_revision_accepted_update_speed", "status": "pass", "minimum_speedup": decimal_text(MIN_SPEEDUP), "minimum_phase_share": decimal_text(MIN_PHASE_SHARE)})

    process_result: dict[str, object] = {}
    for seed in SEEDS:
        worker_rows: dict[str, object] = {}
        for worker in MEASURED_WORKERS:
            selected = trials_for(trials, seed, worker)
            wall_median = median(
                [parse_decimal(trial.row["wall_clock_s"], "wall_clock_s") for trial in selected],
                f"seed {seed} W{worker} wall median",
            )
            user_median = median(
                [parse_decimal(trial.row["user_cpu_s"], "user_cpu_s") for trial in selected],
                f"seed {seed} W{worker} user CPU median",
            )
            system_median = median(
                [parse_decimal(trial.row["system_cpu_s"], "system_cpu_s") for trial in selected],
                f"seed {seed} W{worker} system CPU median",
            )
            worker_rows[str(worker)] = {
                "wall_clock_median_s": decimal_text(wall_median),
                "user_cpu_median_s": decimal_text(user_median),
                "system_cpu_median_s": decimal_text(system_median),
                "system_over_user": ratio_text(system_median, user_median),
                "cpu_over_wall": ratio_text(user_median + system_median, wall_median),
            }
        process_result[str(seed)] = worker_rows

    rss_result: dict[str, object] = {}
    for seed in SEEDS:
        w1 = max(parse_uint(trial.row["peak_sampled_rss_kb"], "W1 peak RSS") for trial in trials_for(trials, seed, 1))
        w8 = max(parse_uint(trial.row["peak_sampled_rss_kb"], "W8 peak RSS") for trial in trials_for(trials, seed, 8))
        if Decimal(w8) > MAX_RSS_RATIO * Decimal(w1):
            raise AcceptanceError(f"seed {seed}: W8 peak RSS {w8} KiB exceeds 2x W1 {w1} KiB")
        rss_result[str(seed)] = {"w1_max_kb": w1, "w8_max_kb": w8, "w8_over_w1": ratio_text(Decimal(w8), Decimal(w1))}
    gates.append({"name": "w8_rss_over_w1", "status": "pass", "limit": decimal_text(MAX_RSS_RATIO)})

    gates.append(
        {
            "name": "frozen_oracle_characterization",
            "status": "deferred_non_final" if deferred else "pass",
            "performance_seed": 1,
            "minimum_accepted_rebuild_ms": decimal_text(MIN_FROZEN_ACCEPTED_REBUILD_MS),
        }
    )

    fixture_by_seed = {
        str(seed): trials_for(trials, seed, 1)[0].row["fixture"] for seed in SEEDS
    }
    if deferred:
        provenance_result: dict[str, object] = {
            "status": "deferred_non_final",
            "reason": args.defer_frozen_oracle_characterization,
        }
    else:
        assert audited is not None
        provenance_result = {
            "status": "pass",
            "base_manifest_sha256": audited.base.sha256,
            "supplement_manifest_sha256": audited.supplement.sha256,
            "characterization_sha256": audited.characterization.sha256,
            "run_metadata": run_metadata,
            "run_artifact_ledger": run_ledger,
        }
    verified_evidence = [
        "same-revision W1/W8 exactly three timed repetitions",
        "full canonical search stream, compact digest, report, and external DAG domain bindings",
        "separately strict search-objective and external-DAG parsimony improvements",
        "complete timed accepted sequence and score-chain parity",
        "per-accept inside/outside row deltas each at least 32 and exactly summed",
        "exact initial-plus-three-commit cache scheduler arithmetic",
        "runner wall/user/system CPU, timer, RSS, and swap plausibility",
    ]
    if not deferred:
        verified_evidence.extend(
            (
                "sealed frozen 3-seed by W1/W2/W4/W8 archive",
                "all twelve frozen accepted-update phase shares for exemption eligibility",
                "externally anchored current ledger, clean Git HEAD, and exact production tool paths",
            )
        )
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": "deferred_non_final" if deferred else "pass",
        "benchmark_dir": os.fspath(benchmark_dir),
        "raw_trials": os.fspath(raw_path),
        "matrix": {
            "seeds": list(SEEDS),
            "workers": list(MEASURED_WORKERS),
            "repetitions": 3,
            "row_id_template": DEFAULT_ROW_ID_TEMPLATE,
            "fixture_by_seed": fixture_by_seed,
        },
        "same_revision": {
            "role": "only_source_for_w1_w8_timing_comparison",
            "timings": timing_result,
            "rss": rss_result,
            "process_metrics": process_result,
            "semantics": {str(seed): semantic_result(trials, seed) for seed in SEEDS},
        },
        "frozen_oracle_characterization": frozen_result,
        "provenance": provenance_result,
        "verified_evidence": verified_evidence,
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
            "The frozen oracle characterizes score domains and whether the accepted-update phase is eligible for an exemption. All W1/W8 speed ratios use same-revision measured trials.",
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
    if frozen["status"] == "deferred_non_final":
        lines.extend((f"Reason: {markdown_escape(frozen['reason'])}", ""))
    else:
        lines.extend(
            (
                "| Seed | Frozen workers audited | Active patterns | Search initial/final | External input/output |",
                "|---:|---|---:|---:|---:|",
            )
        )
        matrix = frozen["matrix"]
        assert isinstance(matrix, dict)
        for seed in SEEDS:
            seed_rows = matrix[str(seed)]
            assert isinstance(seed_rows, dict)
            row = seed_rows["1"]
            assert isinstance(row, dict)
            lines.append(
                f"| {seed} | 1, 2, 4, 8 | {row['active_patterns']} | "
                f"{row['search_initial_score']} / {row['search_final_score']} | "
                f"{row['external_initial_parsimony_min']} / "
                f"{row['external_final_parsimony_min']} |"
            )
        lines.append("")
    lines.extend(("## Gates", "", "| Gate | Status |", "|---|---|"))
    gates = result["gates"]
    assert isinstance(gates, list)
    for gate in gates:
        assert isinstance(gate, dict)
        lines.append(f"| {markdown_escape(gate['name'])} | `{markdown_escape(gate['status'])}` |")
    lines.extend(("", "## Verified evidence", ""))
    evidence = result["verified_evidence"]
    assert isinstance(evidence, list)
    lines.extend(f"- {markdown_escape(item)}" for item in evidence)
    lines.append("")
    return "\n".join(lines)


def output_targets(
    json_output: str | None,
    markdown_output: str | None,
    forbidden_roots: Sequence[Path],
) -> list[tuple[Path, str]]:
    requested = [
        (Path(path), label)
        for path, label in (
            (json_output, "JSON output"),
            (markdown_output, "Markdown output"),
        )
        if path is not None
    ]
    result: list[tuple[Path, str]] = []
    for path, label in requested:
        if not path.name:
            raise AcceptanceError(f"{label} has no filename")
        parent = require_lexical_directory(path.parent, f"{label} parent")
        target = parent / path.name
        if target.exists() or target.is_symlink():
            raise AcceptanceError(f"{label} destination already exists: {target}")
        for forbidden in forbidden_roots:
            root = forbidden.absolute().resolve(strict=True)
            try:
                target.relative_to(root)
            except ValueError:
                continue
            raise AcceptanceError(f"{label} destination is inside sealed/input evidence: {target}")
        result.append((target, label))
    if len({path for path, _ in result}) != len(result):
        raise AcceptanceError("JSON and Markdown outputs alias the same destination")
    return result


def publish_outputs_exclusive(payloads: Sequence[tuple[Path, bytes, str]]) -> None:
    temporary: list[tuple[Path, Path, str]] = []
    published: list[Path] = []
    try:
        for target, payload, label in payloads:
            try:
                descriptor, temporary_text = tempfile.mkstemp(
                    prefix=f".{target.name}.tmp.", dir=target.parent
                )
            except OSError as error:
                raise AcceptanceError(
                    f"cannot create temporary {label} beside {target}: {error}"
                ) from error
            temp_path = Path(temporary_text)
            temporary.append((temp_path, target, label))
            try:
                with os.fdopen(descriptor, "wb") as handle:
                    handle.write(payload)
                    handle.flush()
                    os.fsync(handle.fileno())
                temp_path.chmod(0o444)
            except OSError as error:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
                raise AcceptanceError(
                    f"cannot durably stage {label} beside {target}: {error}"
                ) from error
        for temp_path, target, label in temporary:
            try:
                os.link(temp_path, target, follow_symlinks=False)
            except OSError as error:
                raise AcceptanceError(
                    f"cannot exclusively publish {label} {target}: {error}"
                ) from error
            published.append(target)
        for parent in sorted({path.parent for path in published}, key=os.fspath):
            fsync_directory(parent, "published acceptance output")
    except BaseException:
        rollback_parents: set[Path] = set()
        for path in reversed(published):
            try:
                path.chmod(0o644)
                path.unlink()
                rollback_parents.add(path.parent)
            except OSError:
                pass
        for parent in rollback_parents:
            try:
                fsync_directory(parent, "rolled-back acceptance output")
            except AcceptanceError:
                pass
        raise
    finally:
        temporary_parents: set[Path] = set()
        for temp_path, _, _ in temporary:
            try:
                temp_path.chmod(0o644)
                temp_path.unlink()
                temporary_parents.add(temp_path.parent)
            except OSError:
                pass
        for parent in temporary_parents:
            try:
                fsync_directory(parent, "acceptance temporary cleanup")
            except AcceptanceError:
                pass


def emit_result(
    result: dict[str, object], targets: Sequence[tuple[Path, str]] = ()
) -> None:
    rendered_json = json.dumps(result, indent=2, sort_keys=True) + "\n"
    rendered_markdown = render_markdown(result)
    payloads: list[tuple[Path, bytes, str]] = []
    for target, label in targets:
        payload = rendered_json if label == "JSON output" else rendered_markdown
        payloads.append((target, payload.encode("utf-8"), label))
    publish_outputs_exclusive(payloads)
    sys.stdout.write(rendered_json)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="strict Phase-9 local-commit acceptance postprocessor")
    subparsers = parser.add_subparsers(dest="command", required=True)

    def provenance(target: argparse.ArgumentParser) -> None:
        target.add_argument("--base-manifest", required=True)
        target.add_argument("--expected-parent-sha256", required=True)
        target.add_argument("--supplement", required=True)
        target.add_argument(
            "--base-repo-root",
            help="canonical Git worktree containing sealed Phase-0 repo:// assets",
        )
        target.add_argument(
            "--working-repo-root",
            help="clean current checkout containing this tool and build/bin products",
        )
        target.add_argument(
            "--repo-root",
            help="backward-compatible alias setting both repository roots",
        )

    seal = subparsers.add_parser("seal-run", help="validate and exclusively seal current run evidence")
    seal.add_argument("--benchmark-dir", required=True)
    provenance(seal)
    seal.add_argument("--working-revision", required=True)
    seal.add_argument("--working-larch2", required=True)
    seal.add_argument("--working-dagutil", required=True)
    seal.add_argument("--benchmark-harness", required=True)
    seal.add_argument("--affinity-cpus", required=True)
    seal.add_argument("--warmups", type=int, required=True)
    seal.add_argument("--full-canonical", action="store_true")

    evaluate_parser = subparsers.add_parser("evaluate", help="evaluate current and frozen Phase-9 evidence")
    source = evaluate_parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--benchmark-dir", help="benchmark output directory containing raw_trials.tsv")
    source.add_argument("--raw-trials", help="benchmark raw_trials.tsv (its parent is the output directory)")
    evaluate_parser.add_argument("--base-manifest")
    evaluate_parser.add_argument("--expected-parent-sha256")
    evaluate_parser.add_argument("--supplement")
    evaluate_parser.add_argument("--base-repo-root")
    evaluate_parser.add_argument("--working-repo-root")
    evaluate_parser.add_argument(
        "--repo-root", help="backward-compatible alias setting both repository roots"
    )
    evaluate_parser.add_argument(
        "--expected-run-ledger-sha256",
        help="external SHA-256 anchor printed by seal-run",
    )
    evaluate_parser.add_argument(
        "--frozen-oracle-report",
        action="append",
        default=[],
        metavar="SEED=PATH",
        help="ad-hoc W1 report accepted only with explicit non-final deferral",
    )
    evaluate_parser.add_argument(
        "--defer-frozen-oracle-characterization",
        metavar="REASON",
        help="explicitly produce a non-final result without the sealed archive/run chain",
    )
    evaluate_parser.add_argument("--json-output", help="exclusively publish deterministic JSON")
    evaluate_parser.add_argument("--markdown-output", help="exclusively publish deterministic Markdown")
    return parser


def normalize_repository_root_options(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    """Resolve explicit split roots or one legacy same-root alias."""

    legacy = getattr(args, "repo_root", None)
    base = getattr(args, "base_repo_root", None)
    working = getattr(args, "working_repo_root", None)
    if legacy is not None:
        if base is not None or working is not None:
            parser.error(
                "--repo-root cannot be combined with --base-repo-root or "
                "--working-repo-root"
            )
        args.base_repo_root = legacy
        args.working_repo_root = legacy
        return
    if base is None or working is None:
        parser.error(
            "final provenance requires both --base-repo-root and "
            "--working-repo-root"
        )


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    targets: list[tuple[Path, str]] = []
    if args.command == "seal-run":
        normalize_repository_root_options(args, parser)
    if args.command == "evaluate":
        deferred = args.defer_frozen_oracle_characterization is not None
        provenance_values = (
            args.base_manifest,
            args.expected_parent_sha256,
            args.supplement,
            args.base_repo_root,
            args.working_repo_root,
            args.repo_root,
            args.expected_run_ledger_sha256,
        )
        if deferred:
            if not args.defer_frozen_oracle_characterization.strip():
                parser.error("--defer-frozen-oracle-characterization requires a nonempty reason")
            if any(provenance_values):
                parser.error("deferred/non-final evaluation must not mix in partial sealed provenance")
        else:
            non_root_values = (
                args.base_manifest,
                args.expected_parent_sha256,
                args.supplement,
                args.expected_run_ledger_sha256,
            )
            if not all(non_root_values):
                parser.error(
                    "final evaluation requires --base-manifest, --expected-parent-sha256, "
                    "--supplement, split repository roots, and "
                    "--expected-run-ledger-sha256"
                )
            normalize_repository_root_options(args, parser)
            if args.frozen_oracle_report:
                parser.error("ad-hoc --frozen-oracle-report is permitted only in deferred/non-final mode")
        benchmark_root = (
            Path(args.benchmark_dir)
            if args.benchmark_dir
            else Path(args.raw_trials).parent
        )
        forbidden = [benchmark_root]
        if not deferred:
            forbidden.extend((Path(args.base_manifest).parent, Path(args.supplement).parent))
        try:
            targets = output_targets(args.json_output, args.markdown_output, forbidden)
        except AcceptanceError as error:
            parser.error(str(error))
    try:
        if args.command == "seal-run":
            result = seal_run(args)
            sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
            return 0
        result = evaluate(args)
        emit_result(result, targets)
        return 0
    except AcceptanceError as error:
        result = {"schema": SCHEMA, "schema_version": SCHEMA_VERSION, "status": "fail", "failure": str(error)}
    try:
        if args.command == "seal-run":
            sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
        else:
            emit_result(result, targets)
    except AcceptanceError as output_error:
        print(f"wric_phase9_acceptance: {output_error}", file=sys.stderr)
        sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
