#!/usr/bin/env python3
"""Read-only consolidated WRIC chart-parallelization acceptance evaluator.

The benchmark harness remains the owner of measurements and semantic
validation.  This program joins those immutable workload identities across
revisions and applies the timing/RSS gates which cannot be evaluated inside a
single harness invocation.  It writes nothing: the only successful result is
one JSON document on stdout.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import re
import stat
import subprocess
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path, PurePosixPath
from typing import Callable, Mapping, Sequence

import wric_benchmark_capture as capture_contract


SCHEMA = "wric.cross_phase_acceptance"
SCHEMA_VERSION = 5
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
UINT_RE = re.compile(r"^(0|[1-9][0-9]*)$")
UINT64_MAX = (1 << 64) - 1

EVALUATION_MODES = ("pre-default", "final")
PHASE8_CAPTURE_LABELS = (
    "phase8-generation",
    "phase8-generation-retry1",
)
PHASE8_ATTEMPT0_PRODUCT_REVISION = (
    "94a63238d25a8e3262428419d53f8f0986e8879b"
)
PHASE8_ATTEMPT0_CAPTURE_TOOL_REVISION = (
    "b6ae1a968c0a2374d8180e5682ae53775377c31e"
)
PHASE8_ATTEMPT0_RAW_SHA256 = (
    "e3eb3b3d9e77fc4006aa2c701102f39f84244971fae4f794bf66e5ca72cf4a56"
)
PHASE8_ATTEMPT0_LEDGER_SHA256 = (
    "d22f42f31cf4e37bc92abc53e70f938d163f5fc422af68677c0f7dd7244841e1"
)
PHASE8_ATTEMPT0_CAPTURE_WRAPPER_SHA256 = (
    "e374ed726e026ab973ffa9ff385b13cd8c0eea8041d32b861df61fb4fa6c652e"
)
PHASE8_RETRY1_PRODUCT_REVISION = (
    "07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c"
)
PHASE8_AFFINITY = "0,2,4,6,8,10,12,14"
PHASE6_HISTORICAL_RUN_LABEL = "phase6"
PHASE6_ACCEPTANCE_PRODUCT_REVISION = capture_contract.CURRENT_PRODUCT_REVISION
PHASE6_ACCEPTANCE_SOURCES: Mapping[int, str] = {
    1: "final-phase6-exact1",
    4: "final-scaling",
    16: "final-stress",
}
CURRENT_PRODUCT_RETRY_LABELS = (
    "phase3",
    "phase4",
    "phase7-high",
    "phase7-small",
    "phase7-auto",
)
PHASE8_METADATA_NAME = "wric-benchmark-run-metadata.json"
GENERIC_LEDGER_NAME = "wric-evidence-run-ledger.tsv"
GENERIC_LEDGER_SEAL_NAME = GENERIC_LEDGER_NAME + ".sha256"
GENERIC_LEDGER_PREAMBLE = (
    "# schema=wric.evidence_run_ledger",
    "# schema_version=2",
    "kind\tsha256\tbytes\tmode\tpath",
)
SAFE_CAPTURE_COMPONENT_RE = re.compile(r"^[A-Za-z0-9_.-]+$")

RUN_LABELS = (
    "phase1",
    "phase2",
    "phase3",
    "phase4",
    "phase5",
    "phase7-high",
    "phase7-small",
    "phase7-auto",
    "phase8-generation",
    "phase8-generation-retry1",
    "phase8-end-to-end",
    "final-phase6-exact1",
    "final-scaling",
    "final-primary",
    "final-smt",
    "final-small-auto",
    "final-unpinned-auto",
    "final-default-auto",
    "final-stress",
    "final-real",
    "phase9",
)

RAW_COLUMNS = (
    "row_id", "fixture", "method", "status", "validation_status",
    "requested_workers", "resolved_workers", "worker_policy", "trial_index",
    "runner_outcome", "runner_exit_code", "exit_code", "term_signal",
    "core_dumped", "timed_out", "monitor_error", "rss_limit_enabled",
    "rss_limit_observed", "rss_limit_exceeded", "rss_limit_trigger_bytes",
    "rss_limit_term_sent", "rss_limit_kill_sent", "wall_clock_s", "user_cpu_s",
    "system_cpu_s", "max_rss_kb", "peak_sampled_rss_kb", "peak_sampled_swap_kb",
    "process_rss_limit_bytes", "manifest_rss_limit_bytes",
    "configured_chart_memory_budget", "input_sha256", "refseq_sha256",
    "search_semantic_sha256", "output_semantic_sha256",
    "trial_semantic_sha256", "canonical_argv_sha256", "canonical_digest",
    "iterations", "seed", "acceptance", "objective", "candidate_source",
    "candidates_generated", "candidates_scored",
    "exact_verifications", "accepted_moves", "initial_validated_parsimony_min",
    "final_validated_parsimony_min", "best_reported_objective",
    "candidate_generation_ms", "exact_initialization_ms",
    "initial_chart_construction_ms", "local_scoring_ms", "exact_verification_ms",
    "accepted_rebuild_ms", "total_ms", "chart_cache_resident_bytes",
    "peak_concurrent_exact_verifiers",
    "chart_axis_exact_candidate_active_worker_high_water",
    "exact_candidate_admission_batches", "exact_candidate_parallel_batches",
    "exact_candidate_inner_parallel_batches",
    "exact_candidate_memory_limited_batches",
    "exact_candidate_peak_admitted_bytes",
    "exact_candidate_peak_projected_resident_bytes",
    "exact_candidate_queued_for_memory_ms", "exact_candidate_timing_count",
    "exact_candidate_verification_ms_min",
    "exact_candidate_verification_ms_mean",
    "exact_candidate_verification_ms_max", "report_path",
)

PHASE0_CLASSIFICATION_COLUMNS = (
    "method",
    "requested_workers",
    "row_id",
    "measured_trial_target",
    "observed_trial_count",
    "outcome",
    "evidence_kind",
    "characterization_stage",
    "repeat_stage",
    "trial1_native_peer",
)
PHASE0_CAPTURE_SOURCE = "pre_manifest_capture"
PHASE0_REAL_SOURCE = "manifest_strict_real_smoke"

HISTORICAL_PRE_ADMISSION_LABELS = frozenset(
    ("phase0", "phase1", "phase2", "phase5")
)
ADMISSION_FIELD = "exact_candidate_peak_projected_resident_bytes"
ADMISSION_FIELDS = frozenset(
    (
        "peak_concurrent_exact_verifiers",
        "chart_axis_exact_candidate_active_worker_high_water",
        "exact_candidate_admission_batches",
        "exact_candidate_parallel_batches",
        "exact_candidate_inner_parallel_batches",
        "exact_candidate_memory_limited_batches",
        "exact_candidate_peak_admitted_bytes",
        ADMISSION_FIELD,
        "exact_candidate_queued_for_memory_ms",
        "exact_candidate_timing_count",
        "exact_candidate_verification_ms_min",
        "exact_candidate_verification_ms_mean",
        "exact_candidate_verification_ms_max",
    )
)
HISTORICAL_LEGACY_EXACT_TIMING_FIELDS = frozenset(
    (
        "peak_concurrent_exact_verifiers",
        "exact_candidate_timing_count",
        "exact_candidate_verification_ms_min",
        "exact_candidate_verification_ms_mean",
        "exact_candidate_verification_ms_max",
    )
)
# Retain the former tooling name for consumers of this import-only module.
PHASE0_LEGACY_EXACT_TIMING_FIELDS = HISTORICAL_LEGACY_EXACT_TIMING_FIELDS


def required_raw_columns(label: str) -> tuple[str, ...]:
    if label in HISTORICAL_PRE_ADMISSION_LABELS:
        return tuple(field for field in RAW_COLUMNS if field not in ADMISSION_FIELDS)
    return RAW_COLUMNS


MANIFEST_COLUMNS = (
    "row_id", "run_group", "workload_name", "fixture_id", "method",
    "primary_sha256", "refseq_sha256", "requested_workers",
    "expected_resolved_workers", "expected_worker_policy", "rss_limit_bytes",
    "expected_outcome", "expected_timeout_trials", "expected_reason_code",
    "expected_reason_sha256",
    "iterations", "seed", "chart_max_candidates", "chart_top_k_exact",
    "acceptance", "objective", "candidate_source", "memory_budget_bytes",
    "expected_candidates_generated", "expected_candidates_scored",
    "expected_exact_verifications", "expected_accepted_moves",
    "expected_initial_score", "expected_final_score",
    "expected_validated_parsimony",
    "oracle_search_semantic_sha256", "oracle_output_semantic_sha256",
    "oracle_trial_semantic_sha256", "canonical_argv_sha256",
)

METHOD_NATIVE = "sample_explore_merge"
METHOD_LB = "chart_spr_grammar_lower_bound_heuristic"
METHOD_EXACT = "chart_spr_grammar_exact"
METHOD_SAMPLED = "chart_spr_sampled_tree_fixed_topology"
METHOD_HYBRID = "chart_spr_hybrid_exact"

SMALL_DENSE = "p0-small-dense64-grammar-lower-bound-heuristic-w{}"
SMALL_EXACT = "p0-small-exact1-grammar-exact-w{}"
MEDIUM_DENSE = "p0-medium-dense64-grammar-lower-bound-heuristic-w{}"
MEDIUM_CACHE = "p0-medium-cache1-grammar-lower-bound-heuristic-w{}"
MEDIUM_LAZY = "p0-medium-lazy64-grammar-lower-bound-heuristic-w{}"
MEDIUM_EXACT = "p0-medium-exact1-grammar-exact-w{}"

REAL_REFUSAL_REASON_CODE = "high_arity_refinement_refusal"
REAL_REFUSAL_SUFFIX = (
    "error: polytomy refinement: bounded expansion requires arity <= 63\n"
)
REAL_REFUSAL_STDOUT = "leaves: 3832\nnodes: 5436\nedges: 5435\n"
REAL_REFUSAL_STDOUT_SHA256 = (
    "b0174b43d6ddf95ec64e301a7820b4fb19c74c38d9da0873a76787715cfeab36"
)


@dataclass(frozen=True)
class SupplementSpec:
    label: str
    manifest_id: str
    basename: str
    row_ids: frozenset[str]


SUPPLEMENT_SPECS: Mapping[str, SupplementSpec] = {
    "phase7": SupplementSpec(
        "phase7",
        "phase7-lazy",
        "phase7-lazy.tsv",
        frozenset(
            f"phase7-lazy-{fixture}-{policy}-w{worker}"
            for fixture in ("high-compression", "dense-favoring")
            for policy in ("off", "on", "auto")
            for worker in (1, 2, 4, 8)
        ),
    ),
    "phase7-completion": SupplementSpec(
        "phase7-completion",
        "phase7-lazy-completion",
        "phase7-lazy-completion.tsv",
        frozenset(
            (
                *(f"phase7-lazy-completion-tree0-on-w{worker}" for worker in (1, 2, 4, 8)),
                *(f"phase7-lazy-completion-medium-auto-w{worker}" for worker in (1, 2, 4, 8)),
            )
        ),
    ),
    "phase8": SupplementSpec(
        "phase8",
        "phase8-generation",
        "phase8-generation.tsv",
        frozenset(
            f"phase8-generation-tree0-off-w{worker}" for worker in (1, 2, 4, 8)
        ),
    ),
    "phase9": SupplementSpec(
        "phase9",
        "phase9-local-commit",
        "phase9-local-commit.tsv",
        frozenset(
            f"phase9-local-commit-seed{seed}-w{worker}"
            for seed in (1, 7, 19)
            for worker in (1, 2, 4, 8)
        ),
    ),
}


class AcceptanceError(RuntimeError):
    """Malformed evidence or a failed mandatory gate."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def regular(path: Path, label: str) -> Path:
    absolute = path.absolute()
    try:
        info = absolute.lstat()
        resolved = absolute.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"{label} is unavailable: {path}: {error}") from error
    if absolute != resolved or not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise AcceptanceError(f"{label} is not a canonical lexical regular file: {path}")
    return resolved


def decimal(value: str, label: str, *, positive: bool = False) -> Decimal:
    try:
        result = Decimal(value)
    except (InvalidOperation, ValueError) as error:
        raise AcceptanceError(f"{label} is not a finite decimal: {value!r}") from error
    if not result.is_finite() or result < 0 or (positive and result <= 0):
        raise AcceptanceError(f"{label} is not a {'positive' if positive else 'nonnegative'} finite decimal")
    return result


def uint(value: str, label: str, *, positive: bool = False) -> int:
    if UINT_RE.fullmatch(value) is None or (positive and value == "0"):
        raise AcceptanceError(f"{label} is not a {'positive' if positive else 'nonnegative'} integer: {value!r}")
    return int(value)


def checked_uint64(value: str, label: str, *, positive: bool = False) -> int:
    result = uint(value, label, positive=positive)
    if result > UINT64_MAX:
        raise AcceptanceError(f"{label}: unsigned value exceeds uint64")
    return result


def checked_uint64_sum(left: int, right: int, label: str) -> int:
    if left < 0 or right < 0 or left > UINT64_MAX or right > UINT64_MAX:
        raise AcceptanceError(f"{label}: operands are outside uint64")
    if left > UINT64_MAX - right:
        raise AcceptanceError(f"{label}: uint64 addition overflows")
    return left + right


def checked_uint64_product(left: int, right: int, label: str) -> int:
    if left < 0 or right < 0 or left > UINT64_MAX or right > UINT64_MAX:
        raise AcceptanceError(f"{label}: operands are outside uint64")
    if left != 0 and right > UINT64_MAX // left:
        raise AcceptanceError(f"{label}: uint64 multiplication overflows")
    return left * right


def median(values: Sequence[Decimal], label: str) -> Decimal:
    if not values:
        raise AcceptanceError(f"{label} has no finite samples")
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2


def read_tsv_payload(
    payload: bytes, required: Sequence[str], label: str
) -> tuple[tuple[str, ...], list[dict[str, str]]]:
    try:
        text_payload = payload.decode("utf-8")
        with io.StringIO(text_payload, newline="") as handle:
            reader = csv.reader(handle, delimiter="\t", strict=True)
            header = tuple(next(reader))
            if not header or any(not name for name in header):
                raise AcceptanceError(f"{label} has an empty header field")
            duplicates = sorted({name for name in header if header.count(name) > 1})
            if duplicates:
                raise AcceptanceError(f"{label} has duplicate header fields: {', '.join(duplicates)}")
            missing = [name for name in required if name not in header]
            if missing:
                raise AcceptanceError(f"{label} lacks required fields: {', '.join(missing)}")
            rows: list[dict[str, str]] = []
            for number, fields in enumerate(reader, 2):
                if len(fields) != len(header) or any(field == "" for field in fields):
                    raise AcceptanceError(f"{label}:{number} has extra, missing, or empty cells")
                row = dict(zip(header, fields, strict=True))
                row["__line__"] = str(number)
                rows.append(row)
    except StopIteration as error:
        raise AcceptanceError(f"{label} is empty") from error
    except (UnicodeError, csv.Error) as error:
        raise AcceptanceError(f"cannot parse {label}: {error}") from error
    if not rows:
        raise AcceptanceError(f"{label} contains no rows")
    return header, rows


def read_tsv(path: Path, required: Sequence[str], label: str) -> tuple[tuple[str, ...], list[dict[str, str]]]:
    path = regular(path, label)
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise AcceptanceError(f"cannot read {label}: {error}") from error
    return read_tsv_payload(payload, required, label)


@dataclass(frozen=True)
class SealedManifest:
    path: Path
    sha256: str
    preamble: Mapping[str, str]
    rows: Mapping[str, Mapping[str, str]]

    def unchanged(self) -> None:
        if sha256_file(self.path) != self.sha256:
            raise AcceptanceError(f"sealed manifest changed during evaluation: {self.path}")
        seal = self.path.with_name(self.path.name + ".sha256")
        if seal.read_bytes() != f"{self.sha256}  {self.path.name}\n".encode("ascii"):
            raise AcceptanceError(f"manifest seal changed during evaluation: {seal}")


@dataclass(frozen=True)
class ManifestChain:
    base: SealedManifest
    supplements: Mapping[str, SealedManifest]
    rows: Mapping[str, Mapping[str, str]]

    @property
    def path(self) -> Path:
        return self.base.path

    @property
    def sha256(self) -> str:
        return self.base.sha256

    def group(self, name: str, workers: set[str]) -> set[str]:
        selected: set[str] = set()
        # Final Phase-0 groups must be selected from the base itself.  A
        # supplement is not permitted to create a similarly named base row.
        for row_id, row in self.base.rows.items():
            if row["run_group"] != name:
                continue
            token = manifest_worker(row)
            if token == "native" or token in workers:
                selected.add(row_id)
        if not selected:
            raise AcceptanceError(f"sealed base contains no selected rows for group {name!r}")
        return selected


def load_manifest(
    path: Path,
    expected_sha: str,
    *,
    label: str,
    kind: str,
    parent_sha256: str,
    manifest_id: str | None = None,
    basename: str | None = None,
    expected_rows: frozenset[str] | None = None,
) -> SealedManifest:
    path = regular(path, label)
    if HASH_RE.fullmatch(expected_sha) is None:
        raise AcceptanceError(f"{label} expected SHA-256 is not canonical lowercase SHA-256")
    actual = sha256_file(path)
    if actual != expected_sha:
        raise AcceptanceError(f"{label} SHA-256 differs: {actual} != {expected_sha}")
    seal = regular(path.with_name(path.name + ".sha256"), f"{label} detached seal")
    wanted_seal = f"{actual}  {path.name}\n".encode("ascii")
    if seal.read_bytes() != wanted_seal:
        raise AcceptanceError(f"{label} detached seal is not exact GNU sha256sum bytes")
    if basename is not None and path.name != basename:
        raise AcceptanceError(f"{label} basename is {path.name!r}, expected {basename!r}")

    lines = path.read_text(encoding="utf-8").splitlines()
    preamble: dict[str, str] = {}
    data_start = 0
    while data_start < len(lines) and lines[data_start].startswith("# "):
        line = lines[data_start]
        if "=" not in line[2:]:
            raise AcceptanceError(f"invalid {label} preamble line {data_start + 1}")
        key, value = line[2:].split("=", 1)
        if not key or not value or key in preamble:
            raise AcceptanceError(f"invalid/duplicate {label} preamble key {key!r}")
        preamble[key] = value
        data_start += 1
    if preamble.get("schema") != "wric_chart_parallelization_workloads" or preamble.get("schema_version") != "1":
        raise AcceptanceError(f"unsupported {label} workload schema")
    if preamble.get("kind") != kind or preamble.get("parent_sha256") != parent_sha256:
        raise AcceptanceError(f"{label} kind/parent binding is not exact")
    if manifest_id is not None and preamble.get("manifest_id") != manifest_id:
        raise AcceptanceError(f"{label} manifest_id is not {manifest_id!r}")
    if data_start >= len(lines):
        raise AcceptanceError(f"{label} lacks a TSV header")
    with path.open("r", encoding="utf-8", newline="") as handle:
        for _ in range(data_start):
            next(handle)
        reader = csv.reader(handle, delimiter="\t", strict=True)
        header = tuple(next(reader))
        if not header or any(not field for field in header):
            raise AcceptanceError(f"{label} has an empty header field")
        if len(set(header)) != len(header):
            raise AcceptanceError(f"{label} has duplicate header fields")
        missing = [name for name in MANIFEST_COLUMNS if name not in header]
        if missing:
            raise AcceptanceError(f"{label} lacks required fields: {', '.join(missing)}")
        rows: dict[str, Mapping[str, str]] = {}
        for number, fields in enumerate(reader, data_start + 2):
            if len(fields) != len(header) or any(field == "" for field in fields):
                raise AcceptanceError(f"{label} line {number} has malformed cells")
            row = dict(zip(header, fields, strict=True))
            row_id = row["row_id"]
            if not row_id or row_id in rows:
                raise AcceptanceError(f"{label} has empty/duplicate row ID {row_id!r}")
            outcome = row["expected_outcome"]
            if outcome not in ("ok", "timeout", "expected_infeasible", "scale_limit"):
                raise AcceptanceError(f"{label} {row_id}: unsupported expected outcome {outcome!r}")
            timeout_trials = uint(
                row["expected_timeout_trials"],
                f"{label} {row_id} expected_timeout_trials",
            )
            if outcome == "timeout":
                if timeout_trials == 0:
                    raise AcceptanceError(f"{label} {row_id}: timeout has no characterized trials")
            elif timeout_trials != 0:
                raise AcceptanceError(f"{label} {row_id}: non-timeout row claims timeout trials")
            if outcome == "expected_infeasible":
                if row["expected_reason_code"] != REAL_REFUSAL_REASON_CODE:
                    raise AcceptanceError(f"{label} {row_id}: refusal reason code is not exact")
                if HASH_RE.fullmatch(row["expected_reason_sha256"]) is None:
                    raise AcceptanceError(f"{label} {row_id}: refusal reason hash is invalid")
            elif row["expected_reason_code"] != "-" or row["expected_reason_sha256"] != "-":
                raise AcceptanceError(f"{label} {row_id}: non-refusal row claims refusal evidence")
            rows[row_id] = row
    if not rows:
        raise AcceptanceError(f"{label} contains no rows")
    if expected_rows is not None and set(rows) != set(expected_rows):
        raise AcceptanceError(
            f"{label} row set mismatch; missing={sorted(expected_rows - set(rows))}, "
            f"unexpected={sorted(set(rows) - expected_rows)}"
        )
    return SealedManifest(path, actual, preamble, rows)


def load_manifest_chain(
    base_path: Path,
    expected_base_sha: str,
    supplement_paths: Mapping[str, Path],
    supplement_hashes: Mapping[str, str],
) -> ManifestChain:
    base = load_manifest(
        base_path,
        expected_base_sha,
        label="sealed base manifest",
        kind="base",
        parent_sha256="-",
    )
    expected_labels = set(SUPPLEMENT_SPECS)
    if set(supplement_paths) != expected_labels or set(supplement_hashes) != expected_labels:
        raise AcceptanceError(
            "supplement-label set mismatch; "
            f"paths_missing={sorted(expected_labels - set(supplement_paths))}, "
            f"paths_unexpected={sorted(set(supplement_paths) - expected_labels)}, "
            f"anchors_missing={sorted(expected_labels - set(supplement_hashes))}, "
            f"anchors_unexpected={sorted(set(supplement_hashes) - expected_labels)}"
        )
    supplements: dict[str, SealedManifest] = {}
    authoritative: dict[str, Mapping[str, str]] = dict(base.rows)
    for label in SUPPLEMENT_SPECS:
        spec = SUPPLEMENT_SPECS[label]
        supplement = load_manifest(
            supplement_paths[label],
            supplement_hashes[label],
            label=f"sealed {label} supplement",
            kind="supplement",
            parent_sha256=base.sha256,
            manifest_id=spec.manifest_id,
            basename=spec.basename,
            expected_rows=spec.row_ids,
        )
        for inherited in (
            "repo_revision",
            "merge_base",
            "frozen_larch2_sha256",
            "frozen_oracle_dagutil_sha256",
        ):
            if inherited in base.preamble and supplement.preamble.get(inherited) != base.preamble[inherited]:
                raise AcceptanceError(f"sealed {label} supplement changes inherited role {inherited}")
        collisions = set(authoritative) & set(supplement.rows)
        if collisions:
            raise AcceptanceError(
                f"sealed {label} supplement duplicates/overrides row IDs: {sorted(collisions)}"
            )
        authoritative.update(supplement.rows)
        supplements[label] = supplement
    return ManifestChain(base, supplements, authoritative)


def manifest_worker(row: Mapping[str, str]) -> str:
    if row["method"] == METHOD_NATIVE:
        return "native"
    requested = row["requested_workers"]
    return "auto" if requested == "0" else requested


@dataclass(frozen=True)
class RawEvidence:
    label: str
    paths: tuple[Path, ...]
    digests: tuple[str, ...]
    rows: tuple[Mapping[str, str], ...]

    def row_ids(self) -> set[str]:
        return {row["row_id"] for row in self.rows}

    def selected(self, row_id: str, repetitions: int) -> list[Mapping[str, str]]:
        result = [row for row in self.rows if row["row_id"] == row_id]
        if len(result) != repetitions:
            raise AcceptanceError(f"{self.label}: {row_id} has {len(result)}/{repetitions} trials")
        indexes = sorted(uint(row["trial_index"], f"{self.label} {row_id} trial_index", positive=True) for row in result)
        if indexes != list(range(1, repetitions + 1)):
            raise AcceptanceError(f"{self.label}: {row_id} trial indexes are not exactly 1..{repetitions}")
        return result

    def required_rows(
        self,
        expected: set[str],
        repetitions: int,
        manifests: ManifestChain,
    ) -> None:
        actual = self.row_ids()
        missing = expected - actual
        if missing:
            raise AcceptanceError(
                f"{self.label}: required row set is incomplete; missing={sorted(missing)}"
            )
        unknown_required = expected - set(manifests.rows)
        if unknown_required:
            raise AcceptanceError(
                f"{self.label}: required rows are absent from sealed manifests: {sorted(unknown_required)}"
            )
        allowed_groups = {manifests.rows[row_id]["run_group"] for row_id in expected}
        disallowed: list[str] = []
        for row_id in sorted(actual - expected):
            manifest = manifests.rows.get(row_id)
            if manifest is None or manifest["run_group"] not in allowed_groups:
                disallowed.append(row_id)
        if disallowed:
            raise AcceptanceError(
                f"{self.label}: rows are outside the explicitly expected sealed run groups "
                f"{sorted(allowed_groups)}: {disallowed}"
            )
        for row_id in sorted(expected):
            self.selected(row_id, repetitions)
        for row_id in sorted(actual - expected):
            extras = [row for row in self.rows if row["row_id"] == row_id]
            indexes = sorted(
                uint(
                    row["trial_index"],
                    f"{self.label} allowed extra {row_id} trial_index",
                    positive=True,
                )
                for row in extras
            )
            if indexes != list(range(1, len(extras) + 1)):
                raise AcceptanceError(
                    f"{self.label}: allowed extra {row_id} trial indexes are not contiguous"
                )

    def unchanged(self) -> None:
        for path, before in zip(self.paths, self.digests, strict=True):
            if sha256_file(path) != before:
                raise AcceptanceError(f"read-only input changed during evaluation: {path}")


@dataclass(frozen=True)
class Phase0RawInput:
    path: Path
    sha256: str
    classification_path: Path | None
    classification_sha256: str | None


@dataclass(frozen=True)
class Phase0ArtifactLedger:
    path: Path
    sha256: str
    seal_path: Path
    members: tuple[tuple[str, Path, str], ...]
    raw_inputs: tuple[Phase0RawInput, ...]

    @property
    def capture_raws(self) -> tuple[tuple[Path, str], ...]:
        return tuple((item.path, item.sha256) for item in self.raw_inputs)

    def unchanged(self) -> None:
        ledger = regular(self.path, "Phase-0 artifact ledger final check")
        if ledger != self.path or sha256_file(ledger) != self.sha256:
            raise AcceptanceError(
                f"Phase-0 artifact ledger changed during evaluation: {self.path}"
            )
        wanted = f"{self.sha256}  {self.path.name}\n".encode("ascii")
        seal = regular(
            self.seal_path,
            "Phase-0 artifact-ledger detached seal final check",
        )
        if seal != self.seal_path or seal.read_bytes() != wanted:
            raise AcceptanceError(
                "Phase-0 artifact-ledger seal changed during evaluation: "
                f"{self.seal_path}"
            )
        for uri, path, expected_sha256 in self.members:
            member = regular(
                path,
                f"Phase-0 artifact-ledger member final check {uri}",
            )
            actual_sha256 = sha256_file(member)
            if member != path or actual_sha256 != expected_sha256:
                raise AcceptanceError(
                    "Phase-0 artifact-ledger member changed during evaluation: "
                    f"{uri}: {actual_sha256} != {expected_sha256}"
                )


def load_phase0_artifact_ledger(
    path: Path,
    expected_sha256: str,
    base_repo_root: Path,
) -> Phase0ArtifactLedger:
    if HASH_RE.fullmatch(expected_sha256) is None:
        raise AcceptanceError(
            "--expected-phase0-artifact-ledger-sha256 is not canonical "
            "lowercase SHA-256"
        )
    ledger = regular(path, "Phase-0 artifact ledger")
    if ledger.name != "phase0-artifacts.tsv":
        raise AcceptanceError(
            "Phase-0 artifact ledger basename is not 'phase0-artifacts.tsv'"
        )
    try:
        ledger.relative_to(base_repo_root)
    except ValueError as error:
        raise AcceptanceError(
            "Phase-0 artifact ledger is outside --base-repo-root"
        ) from error
    actual_sha256 = sha256_file(ledger)
    if actual_sha256 != expected_sha256:
        raise AcceptanceError(
            "Phase-0 artifact ledger differs from external anchor: "
            f"{actual_sha256} != {expected_sha256}"
        )
    seal = regular(
        ledger.with_name(ledger.name + ".sha256"),
        "Phase-0 artifact-ledger detached seal",
    )
    wanted_seal = f"{actual_sha256}  {ledger.name}\n".encode("ascii")
    if seal.read_bytes() != wanted_seal:
        raise AcceptanceError(
            "Phase-0 artifact-ledger detached seal is not exact GNU sha256sum bytes"
        )
    if not ledger.read_bytes().startswith(b"sha256\turi\n"):
        raise AcceptanceError(
            "Phase-0 artifact ledger header bytes are not exactly 'sha256\\turi\\n'"
        )

    header, rows = read_tsv(
        ledger,
        ("sha256", "uri"),
        "Phase-0 artifact ledger",
    )
    if header != ("sha256", "uri"):
        raise AcceptanceError(
            "Phase-0 artifact ledger header is not exactly 'sha256\\turi'"
        )
    seen_uris: set[str] = set()
    seen_paths: set[Path] = set()
    members: list[tuple[str, Path, str]] = []
    forbidden = {ledger, seal}
    for row in rows:
        digest = row["sha256"]
        uri = row["uri"]
        if HASH_RE.fullmatch(digest) is None:
            raise AcceptanceError(
                f"Phase-0 artifact ledger has a noncanonical member hash: {digest!r}"
            )
        if uri in seen_uris:
            raise AcceptanceError(
                f"Phase-0 artifact ledger has a duplicate URI: {uri}"
            )
        seen_uris.add(uri)
        if not uri.startswith("repo://"):
            raise AcceptanceError(
                f"Phase-0 artifact URI is not repository-confined: {uri}"
            )
        relative_text = uri.removeprefix("repo://")
        relative = Path(relative_text)
        if (
            not relative_text
            or "\\" in relative_text
            or relative.is_absolute()
            or relative.as_posix() != relative_text
            or any(part in (".", "..") for part in relative.parts)
        ):
            raise AcceptanceError(f"Phase-0 artifact URI is not normalized: {uri}")
        lexical_target = base_repo_root / relative
        try:
            target = regular(lexical_target, f"Phase-0 artifact-ledger member {uri}")
            target.relative_to(base_repo_root)
        except ValueError as error:
            raise AcceptanceError(
                f"Phase-0 artifact URI escapes --base-repo-root: {uri}"
            ) from error
        if target.relative_to(base_repo_root).as_posix() != relative_text:
            raise AcceptanceError(
                f"Phase-0 artifact URI uses an alias instead of lexical bytes: {uri}"
            )
        if target in seen_paths:
            raise AcceptanceError(
                f"Phase-0 artifact ledger aliases one member more than once: {uri}"
            )
        seen_paths.add(target)
        if target in forbidden:
            raise AcceptanceError(
                "Phase-0 artifact ledger violates its non-circular contract by "
                f"listing {uri}"
            )
        member_sha256 = sha256_file(target)
        if member_sha256 != digest:
            raise AcceptanceError(
                f"Phase-0 artifact-ledger member hash mismatch: {uri}: "
                f"{member_sha256} != {digest}"
            )
        members.append((uri, target, digest))

    uris = [uri for uri, _, _ in members]
    if uris != sorted(uris):
        raise AcceptanceError(
            "Phase-0 artifact ledger members are not in canonical URI order"
        )
    by_baseline_path: dict[str, tuple[Path, str]] = {}
    for _, target, digest in members:
        try:
            relative = target.relative_to(ledger.parent).as_posix()
        except ValueError:
            continue
        by_baseline_path[relative] = (target, digest)

    capture_raws: dict[str, tuple[Path, str]] = {}
    classifications: dict[str, tuple[Path, str]] = {}
    for relative, member in by_baseline_path.items():
        parts = Path(relative).parts
        if (
            len(parts) == 4
            and parts[0] == "bootstrap-phase0"
            and parts[1] == "captures"
            and re.fullmatch(r"[a-z0-9][a-z0-9-]*", parts[2]) is not None
        ):
            if parts[3] == "raw_trials.tsv":
                capture_raws[parts[2]] = member
            elif parts[3] == "trial_classification.tsv":
                classifications[parts[2]] = member
    if not capture_raws:
        raise AcceptanceError(
            "Phase-0 artifact ledger contains no canonical capture raw TSVs"
        )
    if set(capture_raws) != set(classifications):
        raise AcceptanceError(
            "Phase-0 aggregate captures/classifications are not an exact pair; "
            f"raw_only={sorted(set(capture_raws) - set(classifications))}, "
            f"classification_only={sorted(set(classifications) - set(capture_raws))}"
        )
    strict_real_relative = "bootstrap-phase0/strict-real-smoke-final/raw_trials.tsv"
    strict_real = by_baseline_path.get(strict_real_relative)
    if strict_real is None:
        raise AcceptanceError(
            "Phase-0 artifact ledger lacks the final strict real-smoke raw TSV"
        )
    raw_inputs = [
        Phase0RawInput(raw_path, raw_sha, class_path, class_sha)
        for capture_id in sorted(capture_raws)
        for (raw_path, raw_sha), (class_path, class_sha) in (
            (capture_raws[capture_id], classifications[capture_id]),
        )
    ]
    raw_inputs.append(Phase0RawInput(*strict_real, None, None))
    return Phase0ArtifactLedger(
        ledger,
        actual_sha256,
        seal,
        tuple(members),
        tuple(raw_inputs),
    )


def verify_phase0_raw_arguments(
    paths: Sequence[Path],
    expected_sha256: Sequence[str],
    ledger: Phase0ArtifactLedger,
) -> None:
    supplied_paths = tuple(
        regular(path, f"Phase-0 raw TSV argument {index}")
        for index, path in enumerate(paths, 1)
    )
    for index, digest in enumerate(expected_sha256, 1):
        if HASH_RE.fullmatch(digest) is None:
            raise AcceptanceError(
                f"Phase-0 raw SHA-256 anchor {index} is not canonical lowercase SHA-256"
            )
    supplied = tuple(zip(supplied_paths, expected_sha256, strict=False))
    if supplied != ledger.capture_raws:
        raise AcceptanceError(
            "Phase-0 raw arguments/anchors are not the exact ordered complete "
            "capture set derived from the artifact ledger; "
            f"ledger={[(os.fspath(path), digest) for path, digest in ledger.capture_raws]!r}, "
            f"supplied={[(os.fspath(path), digest) for path, digest in supplied]!r}"
        )


def load_phase0_classification(
    item: Phase0RawInput,
) -> dict[tuple[str, str], Mapping[str, str]]:
    if item.classification_path is None or item.classification_sha256 is None:
        raise AssertionError("strict real-smoke input has no capture classification")
    actual = sha256_file(item.classification_path)
    if actual != item.classification_sha256:
        raise AcceptanceError(
            "Phase-0 capture classification differs from the sealed artifact "
            f"ledger: {item.classification_path}: "
            f"{actual} != {item.classification_sha256}"
        )
    header, rows = read_tsv(
        item.classification_path,
        PHASE0_CLASSIFICATION_COLUMNS,
        "Phase-0 capture trial classification",
    )
    if header != PHASE0_CLASSIFICATION_COLUMNS:
        raise AcceptanceError(
            "Phase-0 capture trial-classification header is not exact"
        )
    result: dict[tuple[str, str], Mapping[str, str]] = {}
    row_ids: set[str] = set()
    for row in rows:
        method = row["method"]
        requested = row["requested_workers"]
        key = (method, requested)
        if key in result:
            raise AcceptanceError(
                "Phase-0 capture trial classification has a duplicate "
                f"method/worker key: {method}@{requested}"
            )
        if method == METHOD_NATIVE:
            if requested != "native":
                raise AcceptanceError(
                    "Phase-0 native classification does not use worker token 'native'"
                )
        elif requested not in ("auto", "default"):
            uint(
                requested,
                "Phase-0 capture classification requested_workers",
                positive=True,
            )
        row_id = row["row_id"]
        if (
            re.fullmatch(r"p0-[a-z0-9][a-z0-9-]*", row_id) is None
            or row_id in row_ids
        ):
            raise AcceptanceError(
                "Phase-0 capture trial classification has an invalid/duplicate "
                f"normalized row ID: {row_id!r}"
            )
        row_ids.add(row_id)
        target = uint(
            row["measured_trial_target"],
            f"Phase-0 classification {row_id} measured trial target",
            positive=True,
        )
        observed = uint(
            row["observed_trial_count"],
            f"Phase-0 classification {row_id} observed trial count",
            positive=True,
        )
        if row["characterization_stage"] != "characterization":
            raise AcceptanceError(
                f"Phase-0 classification {row_id} has a noncanonical characterization stage"
            )
        if row["outcome"] == "ok":
            if (
                observed != target
                or row["evidence_kind"] != "finite_performance"
                or row["repeat_stage"] != f"repeats/{method}--{requested}"
            ):
                raise AcceptanceError(
                    f"Phase-0 classification {row_id} has an invalid finite-performance contract"
                )
        elif row["outcome"] == "timeout":
            if (
                observed != 1
                or row["evidence_kind"]
                != "timeout_characterization_non_performance"
                or row["repeat_stage"] != "-"
            ):
                raise AcceptanceError(
                    f"Phase-0 classification {row_id} has an invalid timeout contract"
                )
        else:
            raise AcceptanceError(
                f"Phase-0 classification {row_id} has unsupported outcome {row['outcome']!r}"
            )
        expected_peer = (
            "self"
            if method == METHOD_NATIVE
            else "characterization:sample_explore_merge@native"
        )
        if row["trial1_native_peer"] != expected_peer:
            raise AcceptanceError(
                f"Phase-0 classification {row_id} has a noncanonical trial-1 peer"
            )
        result[key] = row
    return result


def load_raw(
    label: str,
    paths: Sequence[Path],
    expected_sha256: Sequence[str],
    *,
    phase0_inputs: Sequence[Phase0RawInput] | None = None,
    anchored_payloads: Sequence[bytes] | None = None,
    anchored_signatures: Sequence[tuple[int, ...]] | None = None,
) -> RawEvidence:
    if not paths:
        raise AcceptanceError(f"no raw TSV supplied for {label}")
    if len(expected_sha256) != len(paths):
        raise AcceptanceError(
            f"{label}: raw SHA-256 anchor/file multiplicity differs: "
            f"{len(expected_sha256)}/{len(paths)}"
        )
    if (anchored_payloads is None) != (anchored_signatures is None):
        raise AssertionError("anchored raw payload/signature arguments differ")
    if anchored_payloads is not None and (
        len(anchored_payloads) != len(paths)
        or len(anchored_signatures or ()) != len(paths)
    ):
        raise AssertionError("anchored raw payloads do not match raw paths")
    rows: list[Mapping[str, str]] = []
    resolved: list[Path] = []
    digests: list[str] = []
    seen: set[tuple[str, str]] = set()
    header: tuple[str, ...] | None = None
    phase0_by_path: dict[
        Path, tuple[Phase0RawInput, dict[tuple[str, str], Mapping[str, str]] | None]
    ] = {}
    projected_row_origins: dict[str, Path] = {}
    if phase0_inputs is not None:
        if label != "phase0" or len(phase0_inputs) != len(paths):
            raise AssertionError("Phase-0 input descriptions do not match raw loading")
        for item in phase0_inputs:
            projection = (
                load_phase0_classification(item)
                if item.classification_path is not None
                else None
            )
            if projection is not None:
                for classification in projection.values():
                    row_id = classification["row_id"]
                    prior = projected_row_origins.get(row_id)
                    if prior is not None:
                        raise AcceptanceError(
                            "Phase-0 classifications map more than one capture to "
                            f"{row_id}: {prior}, {item.classification_path}"
                        )
                    assert item.classification_path is not None
                    projected_row_origins[row_id] = item.classification_path
            phase0_by_path[item.path] = (item, projection)
        if set(phase0_by_path) != {
            regular(path, f"Phase-0 raw TSV argument {index}")
            for index, path in enumerate(paths, 1)
        }:
            raise AssertionError("Phase-0 input descriptions differ from raw paths")
    for index, (path, expected) in enumerate(
        zip(paths, expected_sha256, strict=True), 1
    ):
        if HASH_RE.fullmatch(expected) is None:
            raise AcceptanceError(
                f"{label}: raw SHA-256 anchor {index} is not canonical lowercase SHA-256"
            )
        canonical = regular(path, f"{label} raw TSV")
        anchored_payload = (
            None if anchored_payloads is None else anchored_payloads[index - 1]
        )
        if anchored_payload is None:
            actual = sha256_file(canonical)
        else:
            current_payload, actual, _, current_signature = _stable_read_bytes(
                canonical,
                f"{label} anchored raw TSV {index}",
                maximum_bytes=128 * 1024 * 1024,
            )
            assert anchored_signatures is not None
            if (
                current_payload != anchored_payload
                or current_signature != anchored_signatures[index - 1]
            ):
                raise AcceptanceError(
                    f"{label}: anchored raw TSV changed before exact-byte parsing"
                )
        if actual != expected:
            raise AcceptanceError(
                f"{label}: raw SHA-256 anchor {index} differs: {actual} != {expected}"
            )
        current_header, current_rows = (
            read_tsv(
                canonical, required_raw_columns(label), f"{label} raw TSV"
            )
            if anchored_payload is None
            else read_tsv_payload(
                anchored_payload,
                required_raw_columns(label),
                f"{label} anchored raw TSV",
            )
        )
        if label in HISTORICAL_PRE_ADMISSION_LABELS:
            # Every historical raw payload reaches schema inspection only
            # after its exact bytes match the external SHA-256 anchor above.
            present_admission = ADMISSION_FIELDS.intersection(current_header)
            historical_legacy_timing = (
                present_admission == HISTORICAL_LEGACY_EXACT_TIMING_FIELDS
            )
            if (
                present_admission
                and present_admission != ADMISSION_FIELDS
                and not historical_legacy_timing
            ):
                missing = sorted(ADMISSION_FIELDS - present_admission)
                raise AcceptanceError(
                    f"{label}: raw TSV has a partial exact-candidate admission "
                    f"schema; missing={missing}"
                )
        if header is None:
            header = current_header
        elif current_header != header:
            historical_compatible = (
                label in HISTORICAL_PRE_ADMISSION_LABELS
                and tuple(
                    field for field in current_header
                    if field not in ADMISSION_FIELDS
                )
                == tuple(
                    field for field in header if field not in ADMISSION_FIELDS
                )
            )
            if not historical_compatible:
                raise AcceptanceError(f"{label}: raw TSV headers differ")
        if canonical in resolved:
            raise AcceptanceError(f"{label}: duplicate raw TSV path {canonical}")
        resolved.append(canonical)
        digests.append(actual)
        phase0_projection = phase0_by_path.get(canonical)
        observed_projection: dict[tuple[str, str], list[Mapping[str, str]]] = {}
        for row in current_rows:
            owned = dict(row)
            if phase0_projection is not None:
                _, projection = phase0_projection
                if projection is None:
                    owned["__phase0_source_kind__"] = PHASE0_REAL_SOURCE
                    if owned["row_id"] in projected_row_origins:
                        raise AcceptanceError(
                            "Phase-0 strict real smoke duplicates a projected capture row: "
                            f"{owned['row_id']}"
                        )
                else:
                    source_row_id = row["row_id"]
                    expected_source_row_id = (
                        f"{row['fixture']}/{row['method']}@{row['requested_workers']}"
                    )
                    if source_row_id != expected_source_row_id:
                        raise AcceptanceError(
                            "Phase-0 aggregate capture row does not retain its "
                            "pre-manifest harness identity: "
                            f"{source_row_id!r} != {expected_source_row_id!r}"
                        )
                    projection_key = (row["method"], row["requested_workers"])
                    classification = projection.get(projection_key)
                    if classification is None:
                        raise AcceptanceError(
                            "Phase-0 aggregate capture row lacks a sealed trial "
                            f"classification: {source_row_id}"
                        )
                    observed_projection.setdefault(projection_key, []).append(row)
                    owned["row_id"] = classification["row_id"]
                    owned["__phase0_source_kind__"] = PHASE0_CAPTURE_SOURCE
                    owned["__phase0_source_row_id__"] = source_row_id
            key = (owned["row_id"], owned["trial_index"])
            if key in seen:
                raise AcceptanceError(f"{label}: duplicate row/trial {key[0]}#{key[1]}")
            seen.add(key)
            owned["__raw_path__"] = os.fspath(canonical)
            recorded_report = Path(row["report_path"])
            if recorded_report.is_absolute():
                normalized_report = recorded_report
            else:
                if recorded_report in (Path("."), Path("")) or any(
                    part in (".", "..") for part in recorded_report.parts
                ):
                    raise AcceptanceError(
                        f"{label}: {key[0]}#{key[1]} has a noncanonical relative report path"
                    )
                normalized_report = canonical.parent / recorded_report
            owned["report_path"] = os.fspath(normalized_report)
            rows.append(owned)
        if phase0_projection is not None and phase0_projection[1] is not None:
            projection = phase0_projection[1]
            assert projection is not None
            if set(observed_projection) != set(projection):
                raise AcceptanceError(
                    "Phase-0 aggregate capture/classification key sets differ; "
                    f"raw_only={sorted(set(observed_projection) - set(projection))}, "
                    f"classification_only={sorted(set(projection) - set(observed_projection))}"
                )
            for projection_key, classification in projection.items():
                selected = observed_projection[projection_key]
                observed = uint(
                    classification["observed_trial_count"],
                    f"Phase-0 classification {classification['row_id']} observed trial count",
                    positive=True,
                )
                if len(selected) != observed:
                    raise AcceptanceError(
                        f"Phase-0 classification {classification['row_id']} records "
                        f"{observed} observations but its aggregate capture has {len(selected)}"
                    )
                statuses = {row["status"] for row in selected}
                if statuses != {classification["outcome"]}:
                    raise AcceptanceError(
                        f"Phase-0 classification {classification['row_id']} outcome "
                        f"{classification['outcome']!r} differs from aggregate raw {sorted(statuses)}"
                    )
    return RawEvidence(label, tuple(resolved), tuple(digests), tuple(rows))


def expected_raw_worker(manifest: Mapping[str, str]) -> str:
    return manifest_worker(manifest)


def is_hash_or_dash(value: str) -> bool:
    # The frozen harness uses NA for the absent raw DAG refseq column while
    # workload manifests use '-'.  Neither spelling is a wildcard.
    return value in ("-", "NA") or HASH_RE.fullmatch(value) is not None


def admission_block_available(
    row: Mapping[str, str], label: str, where: str
) -> bool:
    present = {field for field in ADMISSION_FIELDS if field in row}
    if (
        label in HISTORICAL_PRE_ADMISSION_LABELS
        and present == HISTORICAL_LEGACY_EXACT_TIMING_FIELDS
    ):
        return False
    if label in HISTORICAL_PRE_ADMISSION_LABELS:
        if not present:
            return False
        if present != ADMISSION_FIELDS:
            missing = sorted(ADMISSION_FIELDS - present)
            raise AcceptanceError(
                f"{where}: partial exact-candidate admission block; missing={missing}"
            )
        unavailable = {field for field in ADMISSION_FIELDS if row[field] == "NA"}
        if unavailable == ADMISSION_FIELDS:
            return False
        if unavailable:
            raise AcceptanceError(
                f"{where}: exact-candidate admission block mixes NA and recorded "
                f"values; unavailable={sorted(unavailable)}"
            )
        return True

    unavailable = {
        field
        for field in ADMISSION_FIELDS
        if row.get(field) in (None, "NA")
    }
    if unavailable:
        raise AcceptanceError(
            f"{where}: missing required exact-candidate admission fields: "
            f"{sorted(unavailable)}"
        )
    return True


def validate_exact_candidate_admission(
    row: Mapping[str, str], where: str, budget: int, resident: int
) -> int:
    exact = uint(row["exact_verifications"], f"{where} exact verifications")
    peak = uint(
        row["peak_concurrent_exact_verifiers"],
        f"{where} peak concurrent exact verifiers",
    )
    axis = uint(
        row["chart_axis_exact_candidate_active_worker_high_water"],
        f"{where} exact-candidate active-worker high-water",
    )
    batches = uint(
        row["exact_candidate_admission_batches"],
        f"{where} exact-candidate admission batches",
    )
    parallel = uint(
        row["exact_candidate_parallel_batches"],
        f"{where} exact-candidate parallel batches",
    )
    inner = uint(
        row["exact_candidate_inner_parallel_batches"],
        f"{where} exact-candidate inner-parallel batches",
    )
    limited = uint(
        row["exact_candidate_memory_limited_batches"],
        f"{where} exact-candidate memory-limited batches",
    )
    admitted = checked_uint64(
        row["exact_candidate_peak_admitted_bytes"],
        f"{where} exact-candidate peak admitted bytes",
    )
    projected = checked_uint64(
        row[ADMISSION_FIELD], f"{where} exact-candidate projected resident bytes"
    )
    queued = decimal(
        row["exact_candidate_queued_for_memory_ms"],
        f"{where} exact-candidate memory-queue time",
    )
    timing_count = uint(
        row["exact_candidate_timing_count"],
        f"{where} exact-candidate timing count",
    )
    timing_min = decimal(
        row["exact_candidate_verification_ms_min"],
        f"{where} exact-candidate timing minimum",
    )
    timing_mean = decimal(
        row["exact_candidate_verification_ms_mean"],
        f"{where} exact-candidate timing mean",
    )
    timing_max = decimal(
        row["exact_candidate_verification_ms_max"],
        f"{where} exact-candidate timing maximum",
    )
    resolved = uint(row["resolved_workers"], f"{where} resolved workers", positive=True)

    if (
        parallel > batches
        or inner > batches
        or limited > batches
        or parallel + inner > batches
    ):
        raise AcceptanceError(
            f"{where}: exact-candidate admission batch accounting is inconsistent"
        )
    if admitted > projected:
        raise AcceptanceError(
            f"{where}: exact-candidate admitted bytes exceed projected resident bytes"
        )
    if projected > budget:
        raise AcceptanceError(
            f"{where}: exact-candidate projected resident bytes exceed the memory budget"
        )
    if exact > 0:
        required_projection = checked_uint64_sum(
            resident,
            admitted,
            f"{where} chart resident plus exact-candidate admitted bytes",
        )
        if projected < required_projection:
            raise AcceptanceError(
                f"{where}: exact-candidate projected resident bytes do not cover "
                "chart resident plus admitted bytes"
            )
    if axis > resolved:
        raise AcceptanceError(
            f"{where}: exact-candidate active-worker high-water exceeds resolved workers"
        )
    if timing_count != exact:
        raise AcceptanceError(
            f"{where}: exact-candidate timing count differs from exact verifications"
        )
    if not timing_min <= timing_mean <= timing_max:
        raise AcceptanceError(f"{where}: exact-candidate timing order is invalid")

    if exact == 0:
        if any(
            (
                peak,
                axis,
                batches,
                parallel,
                inner,
                limited,
                admitted,
                projected,
                queued,
                timing_count,
                timing_min,
                timing_mean,
                timing_max,
            )
        ):
            raise AcceptanceError(
                f"{where}: zero exact verifications report nonzero admission evidence"
            )
    else:
        if peak < 1 or peak > exact or peak > resolved:
            raise AcceptanceError(
                f"{where}: peak exact-verifier concurrency contradicts the work/worker count"
            )
        if batches == 0 or admitted == 0 or projected == 0:
            raise AcceptanceError(
                f"{where}: exact verifications lack positive admission evidence"
            )
    if batches == 0 and any(
        (parallel, inner, limited, admitted, projected, queued)
    ):
        raise AcceptanceError(
            f"{where}: zero admission batches report nonzero admission evidence"
        )
    if limited == 0 and queued != 0:
        raise AcceptanceError(
            f"{where}: memory-queue time is nonzero without a memory-limited batch"
        )
    return projected


def validate_success(
    row: Mapping[str, str],
    label: str,
    base: ManifestChain,
    *,
    expected_status: str = "ok",
    pre_manifest_capture: bool = False,
) -> None:
    rid = row["row_id"]
    trial = row["trial_index"]
    where = f"{label} {rid} trial {trial}"
    literals = {
        "status": expected_status, "validation_status": "ok", "runner_outcome": "exited",
        "runner_exit_code": "0", "exit_code": "0", "term_signal": "0",
        "core_dumped": "0", "timed_out": "0", "monitor_error": "0",
        "rss_limit_enabled": "1", "rss_limit_observed": "0",
        "rss_limit_exceeded": "0", "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0", "rss_limit_kill_sent": "0",
    }
    for key, wanted in literals.items():
        if row[key] != wanted:
            raise AcceptanceError(f"{where}: {key}={row[key]!r}, expected {wanted!r}")
    wall = decimal(row["wall_clock_s"], f"{where} wall_clock_s", positive=True)
    decimal(row["user_cpu_s"], f"{where} user_cpu_s")
    decimal(row["system_cpu_s"], f"{where} system_cpu_s")
    del wall
    peak = uint(row["peak_sampled_rss_kb"], f"{where} peak RSS", positive=True)
    uint(row["max_rss_kb"], f"{where} wait4 peak RSS", positive=True)
    if uint(row["peak_sampled_swap_kb"], f"{where} peak swap") != 0:
        raise AcceptanceError(f"{where}: peak sampled swap is nonzero")
    for key in ("process_rss_limit_bytes", "manifest_rss_limit_bytes"):
        limit = uint(row[key], f"{where} {key}", positive=True)
        if peak * 1024 > limit:
            raise AcceptanceError(f"{where}: sampled RSS exceeds {key}")
    if row["canonical_digest"] != row["trial_semantic_sha256"]:
        raise AcceptanceError(f"{where}: canonical/trial digest mismatch")
    for key in ("input_sha256", "output_semantic_sha256", "trial_semantic_sha256", "canonical_argv_sha256", "canonical_digest"):
        if HASH_RE.fullmatch(row[key]) is None:
            raise AcceptanceError(f"{where}: {key} is not a lowercase SHA-256")
    if not is_hash_or_dash(row["refseq_sha256"]):
        raise AcceptanceError(f"{where}: refseq_sha256 is invalid")
    if row["method"] != METHOD_NATIVE and HASH_RE.fullmatch(row["search_semantic_sha256"]) is None:
        raise AcceptanceError(f"{where}: chart search semantic digest is invalid")
    if row["method"] == METHOD_NATIVE and row["search_semantic_sha256"] != "-":
        raise AcceptanceError(f"{where}: native row claims a chart search digest")

    manifest = base.rows.get(rid)
    if manifest is None:
        raise AcceptanceError(f"{where}: row is absent from the sealed manifest chain")
    if manifest["expected_outcome"] == "expected_infeasible":
        raise AcceptanceError(
            f"{where}: successful raw row contradicts sealed outcome {manifest['expected_outcome']!r}"
        )
    pairs = (("method", "method"), ("iterations", "iterations"), ("seed", "seed"))
    if not pre_manifest_capture:
        pairs = (("fixture", "workload_name"), *pairs)
    for raw_key, manifest_key in pairs:
        if row[raw_key] != manifest[manifest_key]:
            raise AcceptanceError(f"{where}: {raw_key} differs from sealed manifest row")
    if row["requested_workers"] != expected_raw_worker(manifest):
        raise AcceptanceError(f"{where}: requested worker differs from sealed manifest row")
    if row["input_sha256"] != manifest["primary_sha256"]:
        raise AcceptanceError(f"{where}: input SHA-256 differs from sealed manifest row")
    frozen_refseq = manifest["refseq_sha256"]
    raw_refseq = "-" if row["refseq_sha256"] == "NA" else row["refseq_sha256"]
    if raw_refseq != frozen_refseq:
        raise AcceptanceError(f"{where}: refseq SHA-256 differs from sealed manifest row")
    for key in ("acceptance", "objective", "candidate_source"):
        if manifest[key] != "-" and row[key] != manifest[key]:
            raise AcceptanceError(f"{where}: {key} differs from sealed manifest row")
    semantic_bindings = [
        ("search_semantic_sha256", "oracle_search_semantic_sha256"),
        ("output_semantic_sha256", "oracle_output_semantic_sha256"),
    ]
    if not pre_manifest_capture or row["method"] == METHOD_NATIVE:
        semantic_bindings.extend(
            (
                ("trial_semantic_sha256", "oracle_trial_semantic_sha256"),
                ("canonical_argv_sha256", "canonical_argv_sha256"),
            )
        )
    for raw_key, manifest_key in semantic_bindings:
        frozen = manifest[manifest_key]
        if frozen != "-" and row[raw_key] != frozen:
            raise AcceptanceError(f"{where}: {raw_key} differs from sealed manifest row")
    if manifest["expected_resolved_workers"] not in ("-", "policy") and row["resolved_workers"] != manifest["expected_resolved_workers"]:
        raise AcceptanceError(f"{where}: resolved workers differ from sealed manifest row")
    if manifest["expected_worker_policy"] not in ("-", "policy") and row["worker_policy"] != manifest["expected_worker_policy"]:
        raise AcceptanceError(f"{where}: worker policy differs from sealed manifest row")
    if manifest["rss_limit_bytes"] != "-" and row["manifest_rss_limit_bytes"] != manifest["rss_limit_bytes"]:
        raise AcceptanceError(f"{where}: RSS limit differs from sealed manifest row")
    if row["method"] != METHOD_NATIVE and manifest["memory_budget_bytes"] != "-" and row["configured_chart_memory_budget"] != manifest["memory_budget_bytes"]:
        raise AcceptanceError(f"{where}: memory budget differs from sealed manifest row")
    for raw_key, manifest_key in (
        ("candidates_generated", "expected_candidates_generated"),
        ("candidates_scored", "expected_candidates_scored"),
        ("exact_verifications", "expected_exact_verifications"),
        ("accepted_moves", "expected_accepted_moves"),
    ):
        frozen = manifest[manifest_key]
        if frozen != "-" and row[raw_key] != frozen:
            raise AcceptanceError(f"{where}: {raw_key} differs from sealed manifest row")

    if row["method"] != METHOD_NATIVE:
        initial = uint(row["initial_validated_parsimony_min"], f"{where} initial score")
        final = uint(row["final_validated_parsimony_min"], f"{where} final score")
        best = uint(row["best_reported_objective"], f"{where} reported objective")
        if final > initial or best > initial:
            raise AcceptanceError(f"{where}: chart search worsened its initial objective")
        for key in (
            "candidate_generation_ms", "exact_initialization_ms",
            "initial_chart_construction_ms", "local_scoring_ms",
            "exact_verification_ms", "accepted_rebuild_ms", "total_ms",
        ):
            decimal(row[key], f"{where} {key}")
        budget = checked_uint64(
            row["configured_chart_memory_budget"],
            f"{where} memory budget",
            positive=True,
        )
        resident = checked_uint64(
            row["chart_cache_resident_bytes"], f"{where} chart resident bytes"
        )
        projected = 0
        if admission_block_available(row, label, where):
            projected = validate_exact_candidate_admission(
                row, where, budget, resident
            )
        if projected > budget or resident > budget:
            raise AcceptanceError(f"{where}: chart/exact resident estimate exceeds memory budget")
    regular(Path(row["report_path"]), f"{where} recorded report")


def path_absent(path: Path, label: str) -> None:
    if os.path.lexists(path):
        raise AcceptanceError(f"{label} unexpectedly exists: {path}")


def validate_refusal(
    row: Mapping[str, str],
    label: str,
    base: ManifestChain,
    *,
    exact_artifacts: bool,
) -> None:
    where = f"{label} {row['row_id']} trial {row['trial_index']}"
    wanted = {
        "status": "expected_infeasible", "validation_status": "not_applicable",
        "runner_outcome": "exited", "runner_exit_code": "1", "exit_code": "1",
        "term_signal": "0", "core_dumped": "0", "timed_out": "0",
        "monitor_error": "0", "rss_limit_enabled": "1",
        "rss_limit_observed": "0", "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0", "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
    }
    for key, value in wanted.items():
        if row[key] != value:
            raise AcceptanceError(f"{where}: refusal {key}={row[key]!r}, expected {value!r}")
    manifest = base.rows.get(row["row_id"])
    if manifest is None or manifest["expected_outcome"] != "expected_infeasible":
        raise AcceptanceError(f"{where}: refusal is not sealed as expected_infeasible")
    if (
        manifest["expected_timeout_trials"] != "0"
        or manifest["expected_reason_code"] != REAL_REFUSAL_REASON_CODE
        or HASH_RE.fullmatch(manifest["expected_reason_sha256"]) is None
    ):
        raise AcceptanceError(f"{where}: sealed refusal reason contract is invalid")
    for raw_key, manifest_key in (
        ("fixture", "workload_name"),
        ("method", "method"),
        ("iterations", "iterations"),
        ("seed", "seed"),
        ("input_sha256", "primary_sha256"),
        ("canonical_argv_sha256", "canonical_argv_sha256"),
        ("manifest_rss_limit_bytes", "rss_limit_bytes"),
    ):
        if row[raw_key] != manifest[manifest_key]:
            raise AcceptanceError(f"{where}: refusal {raw_key} differs from sealed manifest row")
    raw_refseq = "-" if row["refseq_sha256"] == "NA" else row["refseq_sha256"]
    if raw_refseq != manifest["refseq_sha256"]:
        raise AcceptanceError(f"{where}: refusal refseq SHA-256 differs from sealed manifest row")
    if row["requested_workers"] != expected_raw_worker(manifest):
        raise AcceptanceError(f"{where}: refusal worker differs from sealed manifest row")
    if row["resolved_workers"] != "NA" or row["worker_policy"] != "unobserved":
        raise AcceptanceError(f"{where}: refusal claims scheduler observations")
    if row["process_rss_limit_bytes"] != manifest["rss_limit_bytes"]:
        raise AcceptanceError(f"{where}: refusal process RSS cap differs from sealed manifest row")
    if manifest["memory_budget_bytes"] != "-" and row["configured_chart_memory_budget"] != manifest["memory_budget_bytes"]:
        raise AcceptanceError(f"{where}: refusal memory budget differs from sealed manifest row")
    if manifest["expected_initial_score"] != "-" and row["initial_validated_parsimony_min"] != manifest["expected_initial_score"]:
        raise AcceptanceError(f"{where}: refusal initial score differs from sealed manifest row")
    for field in (
        "search_semantic_sha256",
        "output_semantic_sha256",
        "trial_semantic_sha256",
        "canonical_digest",
    ):
        if row[field] != "-":
            raise AcceptanceError(f"{where}: refusal claims unavailable semantic field {field}")
    peak = uint(row["peak_sampled_rss_kb"], f"{where} peak RSS", positive=True)
    if peak * 1024 > uint(manifest["rss_limit_bytes"], f"{where} sealed RSS cap", positive=True):
        raise AcceptanceError(f"{where}: refusal sampled RSS exceeds its sealed cap")

    report = regular(Path(row["report_path"]), f"{where} refusal stdout")
    if not exact_artifacts:
        return
    raw_root = regular(Path(row["__raw_path__"]), f"{where} owning raw TSV").parent
    try:
        report.relative_to(raw_root)
    except ValueError as error:
        raise AcceptanceError(f"{where}: refusal stdout escapes its evidence directory") from error
    stdout_bytes = report.read_bytes()
    if (
        stdout_bytes != REAL_REFUSAL_STDOUT.encode("utf-8")
        or hashlib.sha256(stdout_bytes).hexdigest() != REAL_REFUSAL_STDOUT_SHA256
    ):
        raise AcceptanceError(f"{where}: frozen refusal stdout changed")
    stderr = regular(report.with_suffix(".err"), f"{where} refusal stderr")
    stderr_bytes = stderr.read_bytes()
    if hashlib.sha256(stderr_bytes).hexdigest() != manifest["expected_reason_sha256"]:
        raise AcceptanceError(f"{where}: refusal stderr hash differs from sealed evidence")
    try:
        stderr_text = stderr_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AcceptanceError(f"{where}: refusal stderr is not UTF-8") from error
    if not stderr_text.endswith(REAL_REFUSAL_SUFFIX):
        raise AcceptanceError(f"{where}: refusal stderr does not contain the sealed reason")
    expected_output = raw_root / "outputs" / f"{report.stem}.pb.gz"
    path_absent(expected_output, f"{where} derived output")


def validate_timeout(
    row: Mapping[str, str],
    label: str,
    base: ManifestChain,
    *,
    pre_manifest_capture: bool = False,
) -> None:
    where = f"{label} {row['row_id']} trial {row['trial_index']}"
    manifest = base.rows.get(row["row_id"])
    if manifest is None or manifest["expected_outcome"] != "timeout":
        raise AcceptanceError(f"{where}: timeout is not declared by a sealed manifest row")
    wanted = {
        "status": "timeout",
        "validation_status": "not_run",
        "runner_outcome": "timeout" if pre_manifest_capture else "timed_out",
        "timed_out": "1",
        "monitor_error": "0",
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
    }
    if pre_manifest_capture:
        wanted.update(
            runner_exit_code="124",
            exit_code="-1",
            term_signal="15",
            core_dumped="0",
        )
    for field, value in wanted.items():
        if row[field] != value:
            raise AcceptanceError(f"{where}: timeout {field}={row[field]!r}, expected {value!r}")
    manifest_bindings = [
        ("method", "method"),
        ("iterations", "iterations"),
        ("seed", "seed"),
        ("input_sha256", "primary_sha256"),
        ("manifest_rss_limit_bytes", "rss_limit_bytes"),
        ("process_rss_limit_bytes", "rss_limit_bytes"),
    ]
    if not pre_manifest_capture:
        manifest_bindings.extend(
            (
                ("fixture", "workload_name"),
                ("canonical_argv_sha256", "canonical_argv_sha256"),
            )
        )
    for raw_key, manifest_key in manifest_bindings:
        if row[raw_key] != manifest[manifest_key]:
            raise AcceptanceError(f"{where}: timeout {raw_key} differs from sealed manifest row")
    if row["requested_workers"] != expected_raw_worker(manifest):
        raise AcceptanceError(f"{where}: timeout worker differs from sealed manifest row")
    raw_refseq = "-" if row["refseq_sha256"] == "NA" else row["refseq_sha256"]
    if raw_refseq != manifest["refseq_sha256"]:
        raise AcceptanceError(f"{where}: timeout refseq SHA-256 differs from sealed manifest row")
    if row["resolved_workers"] != "NA" or row["worker_policy"] != "timeout_unobserved":
        raise AcceptanceError(f"{where}: timeout claims scheduler observations")
    if manifest["memory_budget_bytes"] != "-" and row["configured_chart_memory_budget"] != manifest["memory_budget_bytes"]:
        raise AcceptanceError(f"{where}: timeout memory budget differs from sealed manifest row")
    for field in (
        "search_semantic_sha256",
        "output_semantic_sha256",
        "trial_semantic_sha256",
        "canonical_digest",
    ):
        if row[field] != "-":
            raise AcceptanceError(f"{where}: timeout claims unavailable semantic field {field}")
    decimal(row["wall_clock_s"], f"{where} wall_clock_s", positive=True)
    decimal(row["user_cpu_s"], f"{where} user_cpu_s")
    decimal(row["system_cpu_s"], f"{where} system_cpu_s")
    peak = uint(row["peak_sampled_rss_kb"], f"{where} peak RSS", positive=True)
    if peak * 1024 > uint(manifest["rss_limit_bytes"], f"{where} RSS cap", positive=True):
        raise AcceptanceError(f"{where}: timeout sampled RSS exceeds sealed cap")


def validate_empty_refusal_output_namespaces(
    label: str,
    rows: Sequence[Mapping[str, str]],
) -> None:
    output_roots = {
        Path(row["__raw_path__"]).parent / "outputs" for row in rows
    }
    for output_root in sorted(output_roots):
        try:
            info = output_root.lstat()
        except OSError as error:
            raise AcceptanceError(
                f"{label}: refusal output namespace is unavailable: {error}"
            ) from error
        if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
            raise AcceptanceError(
                f"{label}: refusal output namespace is not a real directory"
            )
        with os.scandir(output_root) as entries:
            unexpected = sorted(entry.name for entry in entries)
        if unexpected:
            raise AcceptanceError(
                f"{label}: refusal output namespace is not empty: {unexpected}"
            )


def validate_evidence(
    evidence: RawEvidence,
    base: ManifestChain,
    *,
    required_rows: set[str],
    required_refusal: bool = False,
) -> int:
    validated_required = 0
    for row in evidence.rows:
        required = row["row_id"] in required_rows
        if required_refusal and required:
            validate_refusal(row, evidence.label, base, exact_artifacts=True)
        elif required:
            validate_success(row, evidence.label, base)
        elif row["status"] == "ok":
            validate_success(row, evidence.label, base)
        elif row["status"] == "timeout":
            validate_timeout(row, evidence.label, base)
        elif row["status"] == "scale_limit":
            validate_success(
                row, evidence.label, base, expected_status="scale_limit"
            )
        elif row["status"] == "expected_infeasible":
            validate_refusal(row, evidence.label, base, exact_artifacts=False)
        else:
            raise AcceptanceError(
                f"{evidence.label} {row['row_id']}: unsupported allowed-extra status {row['status']!r}"
            )
        if required:
            validated_required += 1
    for row_id in evidence.row_ids():
        selected = [row for row in evidence.rows if row["row_id"] == row_id]
        stable = (
            "fixture",
            "method",
            "requested_workers",
            "input_sha256",
            "refseq_sha256",
            "search_semantic_sha256",
            "output_semantic_sha256",
            "trial_semantic_sha256",
            "canonical_argv_sha256",
            "canonical_digest",
        )
        for key in stable:
            if len({row[key] for row in selected}) != 1:
                raise AcceptanceError(f"{evidence.label} {row_id}: {key} changes across repetitions")
    if required_refusal:
        validate_empty_refusal_output_namespaces(
            evidence.label,
            [row for row in evidence.rows if row["row_id"] in required_rows],
        )
    return validated_required


def validate_phase0(evidence: RawEvidence, base: ManifestChain) -> None:
    actual_rows = evidence.row_ids()
    sealed_rows = set(base.base.rows)
    if actual_rows != sealed_rows:
        raise AcceptanceError(
            "Phase-0 capture row set is not the entire sealed base row set; "
            f"missing={sorted(sealed_rows - actual_rows)}, "
            f"unexpected={sorted(actual_rows - sealed_rows)}"
        )
    expected_refusals = {
        row_id
        for row_id, row in base.base.rows.items()
        if row["expected_outcome"] == "expected_infeasible"
    }
    strict_real_rows = {
        row["row_id"]
        for row in evidence.rows
        if row.get("__phase0_source_kind__") == PHASE0_REAL_SOURCE
    }
    if strict_real_rows != expected_refusals:
        raise AcceptanceError(
            "Phase-0 strict real-smoke row set differs from the sealed refusal set; "
            f"missing={sorted(expected_refusals - strict_real_rows)}, "
            f"unexpected={sorted(strict_real_rows - expected_refusals)}"
        )
    for row_id in sorted(evidence.row_ids()):
        rows = [row for row in evidence.rows if row["row_id"] == row_id]
        manifest = base.base.rows.get(row_id)
        if manifest is None:
            raise AcceptanceError(f"phase0 {row_id}: row is absent from the sealed base manifest")
        stable = (
            "__phase0_source_kind__",
            "fixture",
            "method",
            "requested_workers",
            "resolved_workers",
            "worker_policy",
            "input_sha256",
            "refseq_sha256",
            "search_semantic_sha256",
            "output_semantic_sha256",
            "trial_semantic_sha256",
            "canonical_argv_sha256",
            "canonical_digest",
        )
        if rows[0].get("__phase0_source_kind__") == PHASE0_CAPTURE_SOURCE:
            stable = ("__phase0_source_row_id__", *stable)
        for field in stable:
            if len({row.get(field) for row in rows}) != 1:
                raise AcceptanceError(
                    f"phase0 {row_id}: {field} changes across sealed observations"
                )
        statuses = {row["status"] for row in rows}
        expected = manifest["expected_outcome"]
        if statuses != {expected}:
            raise AcceptanceError(
                f"phase0 {row_id}: raw outcome {sorted(statuses)} differs from sealed {expected!r}"
            )
        if expected == "expected_infeasible" and (
            len(rows) != 1 or rows[0]["trial_index"] != "1"
        ):
            raise AcceptanceError(
                f"phase0 {row_id}: strict real refusal must have exactly one trial_index=1 observation"
            )
        indexes = sorted(
            uint(row["trial_index"], f"phase0 {row_id} trial_index", positive=True)
            for row in rows
        )
        if indexes != list(range(1, len(rows) + 1)):
            raise AcceptanceError(f"phase0 {row_id}: trial indexes are not exactly 1..{len(rows)}")
        expected_timeouts = uint(
            manifest["expected_timeout_trials"],
            f"phase0 {row_id} expected_timeout_trials",
        )
        if expected == "timeout" and len(rows) != expected_timeouts:
            raise AcceptanceError(
                f"phase0 {row_id}: timeout trial count {len(rows)} differs from sealed {expected_timeouts}"
            )
        for row in rows:
            source_kind = row.get("__phase0_source_kind__")
            if expected == "timeout":
                if source_kind != PHASE0_CAPTURE_SOURCE:
                    raise AcceptanceError(
                        f"phase0 {row_id}: timeout is not owned by a classified aggregate capture"
                    )
                validate_timeout(
                    row,
                    "phase0",
                    base,
                    pre_manifest_capture=True,
                )
            elif expected in ("ok", "scale_limit"):
                if source_kind != PHASE0_CAPTURE_SOURCE:
                    raise AcceptanceError(
                        f"phase0 {row_id}: finite row is not owned by a classified aggregate capture"
                    )
                validate_success(
                    row,
                    "phase0",
                    base,
                    expected_status=expected,
                    pre_manifest_capture=True,
                )
            elif expected == "expected_infeasible":
                if source_kind != PHASE0_REAL_SOURCE:
                    raise AcceptanceError(
                        f"phase0 {row_id}: refusal is not owned by the final strict real smoke"
                    )
                validate_refusal(row, "phase0", base, exact_artifacts=True)
            else:
                raise AcceptanceError(f"phase0 {row_id}: unsupported sealed outcome {expected!r}")
    validate_empty_refusal_output_namespaces(
        "phase0",
        [
            row
            for row in evidence.rows
            if row.get("__phase0_source_kind__") == PHASE0_REAL_SOURCE
        ],
    )


def values(rows: Sequence[Mapping[str, str]], field: str, label: str) -> list[Decimal]:
    return [decimal(row[field], f"{label} {field}") for row in rows]


def med(evidence: RawEvidence, row_id: str, reps: int, field: str) -> Decimal:
    return median(values(evidence.selected(row_id, reps), field, f"{evidence.label} {row_id}"), f"{evidence.label} {row_id} median {field}")


def max_decimal(evidence: RawEvidence, row_id: str, reps: int, field: str) -> Decimal:
    return max(values(evidence.selected(row_id, reps), field, f"{evidence.label} {row_id}"))


def phase_sum(evidence: RawEvidence, row_id: str, reps: int, fields: Sequence[str]) -> Decimal:
    samples = [sum((decimal(row[field], f"{row_id} {field}") for field in fields), Decimal(0)) for row in evidence.selected(row_id, reps)]
    return median(samples, f"{evidence.label} {row_id} phase median")


class Gates:
    def __init__(self) -> None:
        self.items: list[dict[str, object]] = []

    def ratio(self, name: str, numerator: Decimal, denominator: Decimal, limit: Decimal) -> None:
        if denominator <= 0:
            raise AcceptanceError(f"{name}: denominator is not positive")
        ratio = numerator / denominator
        if ratio > limit:
            raise AcceptanceError(f"{name}: ratio {ratio} exceeds {limit}")
        self.items.append({"name": name, "status": "pass", "ratio": str(ratio), "limit": str(limit)})

    def condition(self, name: str, condition: bool, detail: str) -> None:
        if not condition:
            raise AcceptanceError(f"{name}: {detail}")
        self.items.append({"name": name, "status": "pass"})


def baseline_compare(gates: Gates, name: str, phase0: RawEvidence, current: RawEvidence,
                     base: ManifestChain, row_id: str, current_reps: int,
                     field: str, limit: Decimal) -> None:
    frozen = [row for row in phase0.rows if row["row_id"] == row_id]
    if not frozen:
        raise AcceptanceError(f"{name}: frozen Phase-0 row is missing: {row_id}")
    finite = [row for row in frozen if row["status"] == "ok"]
    timed = [row for row in frozen if row["status"] == "timeout"]
    if finite and timed:
        raise AcceptanceError(f"{name}: Phase-0 row mixes finite and timeout outcomes")
    current_value = med(current, row_id, current_reps, field)
    if timed:
        manifest = base.base.rows.get(row_id)
        if manifest is None:
            raise AcceptanceError(f"{name}: timeout comparator is absent from sealed base")
        expected_count = uint(
            manifest["expected_timeout_trials"],
            f"{name} expected timeout trials",
            positive=True,
        )
        if len(timed) != expected_count:
            raise AcceptanceError(
                f"{name}: frozen timeout row has {len(timed)}/{expected_count} characterized trials"
            )
        gates.items.append({"name": name, "status": "pass", "comparison": "finite_completion_after_all_timeout"})
        return
    if not finite:
        raise AcceptanceError(f"{name}: finite Phase-0 row has no captured trials")
    base_value = median(values(finite, field, f"phase0 {row_id}"), f"phase0 {row_id} median")
    gates.ratio(name, current_value, base_value, limit)


def rss_max(evidence: RawEvidence, row_id: str, reps: int) -> int:
    return max(uint(row["peak_sampled_rss_kb"], f"{row_id} RSS", positive=True) for row in evidence.selected(row_id, reps))


def rss_pair(gates: Gates, name: str, evidence: RawEvidence, w1: str, w8: str, reps: int, global_cap: int) -> None:
    one, eight = rss_max(evidence, w1, reps), rss_max(evidence, w8, reps)
    gates.condition(name + "_ratio", eight <= 2 * one, f"W8 RSS {eight} KiB exceeds 2x W1 {one} KiB")
    gates.condition(name + "_global", eight <= global_cap, f"W8 RSS {eight} KiB exceeds global cap {global_cap} KiB")


def semantic_pair(evidence: RawEvidence, row_ids: Sequence[str], reps: int, label: str) -> None:
    pairs = set()
    for row_id in row_ids:
        for row in evidence.selected(row_id, reps):
            pairs.add((row["search_semantic_sha256"], row["output_semantic_sha256"]))
    if len(pairs) != 1:
        raise AcceptanceError(f"{label}: canonical search/output semantics differ")


def top_report_value(path: Path, key: str) -> str:
    path = regular(path, "recorded product report")
    values: list[str] = []
    in_search = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line == "chart_spr_search:":
            if in_search:
                raise AcceptanceError(f"{path}: duplicate chart_spr_search section")
            in_search = True
            continue
        if in_search and line and not line.startswith(" "):
            break
        if in_search:
            match = re.fullmatch(rf"  {re.escape(key)}:[ \t]*(\S+)[ \t]*", line)
            if match:
                values.append(match.group(1))
    if len(values) != 1:
        raise AcceptanceError(f"{path}: expected one top-level {key}, found {len(values)}")
    return values[0]


def lazy_auto_gate(gates: Gates, evidence: RawEvidence, prefix: str, reps: int, name: str, worker: int = 1) -> str:
    off, on, auto = (f"{prefix}-{policy}-w{worker}" for policy in ("off", "on", "auto"))
    off_m, on_m, auto_m = (med(evidence, rid, reps, "wall_clock_s") for rid in (off, on, auto))
    faster = "off" if off_m <= on_m else "on"
    fastest = min(off_m, on_m)
    gates.ratio(name + "_wall", auto_m, fastest, Decimal("1.10"))
    if max(off_m, on_m) / fastest > Decimal("1.10"):
        for row in evidence.selected(auto, reps):
            resolved = top_report_value(Path(row["report_path"]), "lazy_policy_resolved")
            if resolved != faster:
                raise AcceptanceError(f"{name}: auto resolved {resolved!r}, faster forced policy is {faster!r}")
    return faster


def verify_phase9_run_ledger_anchor(
    raw_paths: Sequence[Path], expected_sha256: str
) -> str:
    if len(raw_paths) != 1:
        raise AcceptanceError("phase9 requires exactly one sealed benchmark raw TSV")
    if HASH_RE.fullmatch(expected_sha256) is None:
        raise AcceptanceError(
            "--expected-phase9-run-ledger-sha256 is not canonical lowercase SHA-256"
        )
    raw = regular(raw_paths[0], "phase9 raw TSV")
    ledger = regular(raw.parent / "phase9-run-artifacts.tsv", "Phase-9 run artifact ledger")
    seal = regular(raw.parent / "phase9-run-artifacts.tsv.sha256", "Phase-9 run artifact ledger seal")
    ledger_sha = sha256_file(ledger)
    if ledger_sha != expected_sha256:
        raise AcceptanceError(
            f"Phase-9 run ledger differs from external anchor: {ledger_sha} != {expected_sha256}"
        )
    if seal.read_bytes() != f"{ledger_sha}  {ledger.name}\n".encode("ascii"):
        raise AcceptanceError("Phase-9 run ledger detached seal is invalid")
    return ledger_sha


def canonical_directory(path: Path, label: str) -> Path:
    absolute = path.absolute()
    try:
        info = absolute.lstat()
        resolved = absolute.resolve(strict=True)
    except OSError as error:
        raise AcceptanceError(f"{label} is unavailable: {path}: {error}") from error
    if absolute != resolved or not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise AcceptanceError(f"{label} is not a canonical lexical directory: {path}")
    return resolved


@dataclass(frozen=True)
class CaptureMember:
    relative: str
    kind: str
    sha256: str | None
    size: int | None
    mode: int
    signature: tuple[int, ...]


@dataclass(frozen=True)
class Phase8CaptureProvenance:
    label: str
    capture_directory: Path
    capture_signature: tuple[int, ...]
    raw_path: Path
    raw_sha256: str
    raw_payload: bytes
    raw_signature: tuple[int, ...]
    metadata_path: Path
    metadata_sha256: str
    metadata: Mapping[str, object]
    ledger_path: Path
    ledger_sha256: str
    ledger_signature: tuple[int, ...]
    seal_path: Path
    seal_sha256: str
    seal_signature: tuple[int, ...]
    product_revision: str
    capture_tool_revision: str
    capture_wrapper_sha256: str
    product_repo_root: Path
    capture_tool_repo_root: Path
    members: tuple[CaptureMember, ...]

    def unchanged(self) -> None:
        label = f"{self.label} capture provenance final check"
        root = canonical_directory(self.capture_directory, label)
        if _stable_directory_signature(root.lstat()) != self.capture_signature:
            raise AcceptanceError(
                f"{self.label}: capture directory changed during evaluation"
            )
        ledger_payload, ledger_signature = _read_capture_control(
            self.ledger_path,
            f"{self.label} generic-v2 ledger final check",
            maximum_bytes=64 * 1024 * 1024,
        )
        seal_payload, seal_signature = _read_capture_control(
            self.seal_path,
            f"{self.label} generic-v2 ledger seal final check",
            maximum_bytes=256,
        )
        if (
            sha256_bytes(ledger_payload) != self.ledger_sha256
            or ledger_signature != self.ledger_signature
            or sha256_bytes(seal_payload) != self.seal_sha256
            or seal_signature != self.seal_signature
        ):
            raise AcceptanceError(
                f"{self.label}: capture ledger or seal changed during evaluation"
            )
        current = _scan_capture_members(root)
        if current != self.members:
            raise AcceptanceError(
                f"{self.label}: generic-v2 capture closure changed during evaluation"
            )

    def output(self) -> dict[str, str]:
        return {
            "capture_directory": os.fspath(self.capture_directory),
            "raw_path": os.fspath(self.raw_path),
            "raw_sha256": self.raw_sha256,
            "metadata_path": os.fspath(self.metadata_path),
            "metadata_sha256": self.metadata_sha256,
            "ledger_path": os.fspath(self.ledger_path),
            "ledger_sha256": self.ledger_sha256,
            "seal_path": os.fspath(self.seal_path),
            "seal_sha256": self.seal_sha256,
            "product_revision": self.product_revision,
            "capture_tool_revision": self.capture_tool_revision,
            "capture_wrapper_sha256": self.capture_wrapper_sha256,
        }


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _stable_file_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def _stable_directory_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def _stable_hash(path: Path, label: str) -> tuple[str, int, int, tuple[int, ...]]:
    descriptor: int | None = None
    try:
        before = path.lstat()
    except OSError as error:
        raise AcceptanceError(f"{label} is unavailable: {error}") from error
    if (
        stat.S_ISLNK(before.st_mode)
        or not stat.S_ISREG(before.st_mode)
        or before.st_nlink != 1
    ):
        raise AcceptanceError(
            f"{label} is not a single-link, non-symlink regular file"
        )
    try:
        descriptor = os.open(
            path,
            os.O_RDONLY
            | getattr(os, "O_NOFOLLOW", 0)
            | getattr(os, "O_CLOEXEC", 0),
        )
        opened = os.fstat(descriptor)
        signature = _stable_file_signature(before)
        if _stable_file_signature(opened) != signature:
            raise AcceptanceError(f"{label} changed while opening")
        digest = hashlib.sha256()
        byte_count = 0
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            digest.update(block)
            byte_count += len(block)
        after_open = os.fstat(descriptor)
        after = path.lstat()
    except OSError as error:
        raise AcceptanceError(f"{label} changed while hashing: {error}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if (
        _stable_file_signature(after_open) != signature
        or _stable_file_signature(after) != signature
        or byte_count != after_open.st_size
    ):
        raise AcceptanceError(f"{label} changed while hashing")
    return digest.hexdigest(), byte_count, stat.S_IMODE(after.st_mode), signature


def _stable_read_bytes(
    path: Path, label: str, *, maximum_bytes: int
) -> tuple[bytes, str, int, tuple[int, ...]]:
    descriptor: int | None = None
    try:
        lexical = path.lstat()
        if (
            stat.S_ISLNK(lexical.st_mode)
            or not stat.S_ISREG(lexical.st_mode)
            or lexical.st_nlink != 1
        ):
            raise AcceptanceError(
                f"{label} is not a single-link, non-symlink regular file"
            )
        descriptor = os.open(
            path,
            os.O_RDONLY
            | getattr(os, "O_NOFOLLOW", 0)
            | getattr(os, "O_CLOEXEC", 0),
        )
        opened = os.fstat(descriptor)
        signature = _stable_file_signature(lexical)
        if _stable_file_signature(opened) != signature:
            raise AcceptanceError(f"{label} changed while opening")
        if opened.st_size > maximum_bytes:
            raise AcceptanceError(f"{label} exceeds the maximum supported size")
        chunks: list[bytes] = []
        byte_count = 0
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            byte_count += len(block)
            if byte_count > maximum_bytes:
                raise AcceptanceError(
                    f"{label} exceeds the maximum supported size"
                )
            chunks.append(block)
        after_open = os.fstat(descriptor)
        after_lexical = path.lstat()
    except AcceptanceError:
        raise
    except OSError as error:
        raise AcceptanceError(f"cannot read {label}: {error}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if (
        _stable_file_signature(after_open) != signature
        or _stable_file_signature(after_lexical) != signature
        or byte_count != after_open.st_size
    ):
        raise AcceptanceError(f"{label} changed while reading")
    payload = b"".join(chunks)
    return payload, sha256_bytes(payload), stat.S_IMODE(after_open.st_mode), signature


def _read_capture_control(
    path: Path,
    label: str,
    *,
    maximum_bytes: int,
) -> tuple[bytes, tuple[int, ...]]:
    payload, _, mode, signature = _stable_read_bytes(
        path, label, maximum_bytes=maximum_bytes
    )
    if mode != 0o444:
        raise AcceptanceError(f"{label} is not exact mode 0444")
    return payload, signature


def _validate_capture_relative(relative: str, label: str) -> None:
    pure = PurePosixPath(relative)
    if (
        not relative
        or pure.is_absolute()
        or pure.as_posix() != relative
        or not pure.parts
    ):
        raise AcceptanceError(f"{label} has an unsafe path: {relative!r}")
    for component in pure.parts:
        if (
            SAFE_CAPTURE_COMPONENT_RE.fullmatch(component) is None
            or component in (".", "..", GENERIC_LEDGER_NAME,
                             GENERIC_LEDGER_SEAL_NAME)
            or component.startswith(".wric-evidence-run-ledger.stage-")
        ):
            raise AcceptanceError(f"{label} has an unsafe path: {relative!r}")


def _scan_capture_members(root: Path) -> tuple[CaptureMember, ...]:
    members: dict[str, CaptureMember] = {}
    seen_inodes: set[tuple[int, int]] = set()

    def visit(directory: Path, prefix: str) -> None:
        try:
            before = directory.lstat()
            children = sorted(directory.iterdir(), key=lambda path: path.name)
        except OSError as error:
            raise AcceptanceError(
                f"cannot scan Phase-8 capture {prefix or '.'}: {error}"
            ) from error
        if stat.S_ISLNK(before.st_mode) or not stat.S_ISDIR(before.st_mode):
            raise AcceptanceError(
                f"Phase-8 capture member is not a directory: {prefix or '.'}"
            )
        for child in children:
            relative = child.name if not prefix else f"{prefix}/{child.name}"
            if not prefix and child.name in (
                GENERIC_LEDGER_NAME,
                GENERIC_LEDGER_SEAL_NAME,
            ):
                continue
            _validate_capture_relative(relative, "generic-v2 capture closure")
            try:
                info = child.lstat()
            except OSError as error:
                raise AcceptanceError(
                    f"cannot inspect Phase-8 capture member {relative}: {error}"
                ) from error
            if stat.S_ISLNK(info.st_mode):
                raise AcceptanceError(
                    f"Phase-8 capture contains a symlink: {relative}"
                )
            if stat.S_ISDIR(info.st_mode):
                visit(child, relative)
                try:
                    after = child.lstat()
                except OSError as error:
                    raise AcceptanceError(
                        f"Phase-8 capture directory changed: {relative}: {error}"
                    ) from error
                signature = _stable_directory_signature(after)
                if signature != _stable_directory_signature(info):
                    raise AcceptanceError(
                        f"Phase-8 capture directory changed while scanning: {relative}"
                    )
                members[relative] = CaptureMember(
                    relative,
                    "directory",
                    None,
                    None,
                    stat.S_IMODE(after.st_mode),
                    signature,
                )
                continue
            if not stat.S_ISREG(info.st_mode):
                raise AcceptanceError(
                    f"Phase-8 capture contains a special file: {relative}"
                )
            identity = (info.st_dev, info.st_ino)
            if info.st_nlink != 1 or identity in seen_inodes:
                raise AcceptanceError(
                    f"Phase-8 capture contains a hard-linked/aliased file: {relative}"
                )
            seen_inodes.add(identity)
            digest, size, mode, signature = _stable_hash(
                child, f"Phase-8 capture member {relative}"
            )
            members[relative] = CaptureMember(
                relative, "file", digest, size, mode, signature
            )
        try:
            after = directory.lstat()
        except OSError as error:
            raise AcceptanceError(
                f"Phase-8 capture directory changed: {prefix or '.'}: {error}"
            ) from error
        if _stable_directory_signature(after) != _stable_directory_signature(before):
            raise AcceptanceError(
                f"Phase-8 capture directory changed while scanning: {prefix or '.'}"
            )

    visit(root, "")
    if not any(member.kind == "file" for member in members.values()):
        raise AcceptanceError("Phase-8 capture contains no evidence files")
    return tuple(members[path] for path in sorted(members))


def _parse_generic_v2_ledger(
    payload: bytes, label: str
) -> tuple[tuple[str, str, str, str, str], ...]:
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        raise AcceptanceError(f"{label} is not canonical newline-delimited ASCII")
    try:
        lines = payload[:-1].decode("ascii").split("\n")
    except UnicodeDecodeError as error:
        raise AcceptanceError(f"{label} is not ASCII") from error
    if tuple(lines[: len(GENERIC_LEDGER_PREAMBLE)]) != GENERIC_LEDGER_PREAMBLE:
        raise AcceptanceError(f"{label} is not the exact generic-v2 schema")
    result: list[tuple[str, str, str, str, str]] = []
    seen: set[str] = set()
    for number, line in enumerate(
        lines[len(GENERIC_LEDGER_PREAMBLE):],
        len(GENERIC_LEDGER_PREAMBLE) + 1,
    ):
        fields = line.split("\t")
        if len(fields) != 5:
            raise AcceptanceError(f"{label} line {number} is malformed")
        kind, digest, size, mode, relative = fields
        if kind == "file":
            if HASH_RE.fullmatch(digest) is None or UINT_RE.fullmatch(size) is None:
                raise AcceptanceError(
                    f"{label} line {number} has invalid file fields"
                )
        elif kind == "directory":
            if digest != "-" or size != "-":
                raise AcceptanceError(
                    f"{label} line {number} has invalid directory fields"
                )
        else:
            raise AcceptanceError(f"{label} line {number} has unknown kind")
        if re.fullmatch(r"[0-7]{4}", mode) is None:
            raise AcceptanceError(f"{label} line {number} has invalid mode")
        _validate_capture_relative(relative, label)
        if relative in seen:
            raise AcceptanceError(f"{label} duplicates path {relative}")
        seen.add(relative)
        result.append((kind, digest, size, mode, relative))
    if not result or [record[4] for record in result] != sorted(seen):
        raise AcceptanceError(f"{label} member order/closure is not canonical")
    for _, _, _, _, relative in result:
        parts = PurePosixPath(relative).parts
        for length in range(1, len(parts)):
            parent = "/".join(parts[:length])
            parent_record = next(
                (record for record in result if record[4] == parent), None
            )
            if parent_record is None or parent_record[0] != "directory":
                raise AcceptanceError(
                    f"{label} omits typed parent {parent} for {relative}"
                )
    return tuple(result)


def _strict_canonical_json(path: Path, label: str) -> tuple[dict[str, object], str]:
    payload, digest, mode, signature = _stable_read_bytes(
        path, label, maximum_bytes=64 * 1024 * 1024
    )
    if mode != 0o444:
        raise AcceptanceError(f"{label} is not exact mode 0444")

    def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in items:
            if key in result:
                raise AcceptanceError(f"{label} duplicates JSON key {key!r}")
            result[key] = value
        return result

    def constant(value: str) -> None:
        raise AcceptanceError(f"{label} contains non-finite number {value}")

    try:
        value = json.loads(
            payload, object_pairs_hook=pairs, parse_constant=constant
        )
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise AcceptanceError(f"{label} is invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise AcceptanceError(f"{label} root is not an object")
    canonical = (json.dumps(value, sort_keys=True, indent=2) + "\n").encode()
    if canonical != payload:
        raise AcceptanceError(f"{label} is not exact canonical JSON")
    if _stable_file_signature(path.lstat()) != signature:
        raise AcceptanceError(f"{label} changed while parsing")
    return value, digest


def _metadata_mapping(
    value: object, label: str
) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise AcceptanceError(f"{label} is not an object")
    return value


def _metadata_revision(
    metadata: Mapping[str, object],
    role: str,
    expected: str,
    expected_root: Path,
    label: str,
) -> None:
    repositories = _metadata_mapping(metadata.get("repositories"), f"{label} repositories")
    record = _metadata_mapping(repositories.get(role), f"{label} repository {role}")
    if (
        record.get("expected_revision") != expected
        or record.get("path") != os.fspath(expected_root)
    ):
        raise AcceptanceError(
            f"{label} {role} root/revision differs from its external anchors"
        )
    before = record.get("pre")
    after = record.get("post")
    if before != after:
        raise AcceptanceError(f"{label} repository {role} changed during capture")
    for position in ("pre", "post"):
        state = _metadata_mapping(
            record.get(position), f"{label} repository {role} {position}"
        )
        if (
            state.get("head") != expected
            or state.get("toplevel") != os.fspath(expected_root)
            or state.get("porcelain_v2_z_base64") != ""
            or state.get("porcelain_v2_z_bytes") != 0
            or state.get("porcelain_v2_z_sha256")
            != hashlib.sha256(b"").hexdigest()
        ):
            raise AcceptanceError(
                f"{label} repository {role} {position} is not the exact clean anchored state"
            )


def _require_metadata_snapshot_member(
    snapshot_value: object,
    path: Path,
    member: CaptureMember,
    label: str,
) -> None:
    try:
        snapshot = capture_contract.require_snapshot_shape(snapshot_value, label)
    except capture_contract.CaptureError as error:
        raise AcceptanceError(f"{label}: {error}") from error
    signature = member.signature
    expected = {
        "bytes": member.size,
        "ctime_ns": signature[6],
        "device": signature[0],
        "inode": signature[1],
        "mode": member.mode,
        "mtime_ns": signature[5],
        "nlink": signature[3],
        "path": os.fspath(path),
        "sha256": member.sha256,
    }
    if dict(snapshot) != expected:
        raise AcceptanceError(
            f"{label}: metadata snapshot differs from the exact ledger member"
        )


def _require_live_metadata_snapshot(
    snapshot_value: object,
    path: Path,
    label: str,
    *,
    executable: bool,
) -> None:
    try:
        snapshot = capture_contract.require_snapshot_shape(snapshot_value, label)
    except capture_contract.CaptureError as error:
        raise AcceptanceError(f"{label}: {error}") from error
    digest, size, mode, signature = _stable_hash(path, label)
    expected = {
        "bytes": size,
        "ctime_ns": signature[6],
        "device": signature[0],
        "inode": signature[1],
        "mode": mode,
        "mtime_ns": signature[5],
        "nlink": signature[3],
        "path": os.fspath(path),
        "sha256": digest,
    }
    if dict(snapshot) != expected or (executable and mode & 0o111 == 0):
        raise AcceptanceError(
            f"{label}: metadata snapshot differs from the live exact file"
        )


def load_phase8_capture_provenance(
    label: str,
    capture_directory: Path,
    raw_path_argument: Path,
    expected_raw_sha256: str,
    expected_ledger_sha256: str,
    expected_product_revision: str,
    expected_capture_tool_revision: str,
    expected_capture_wrapper_sha256: str,
    product_repo_root_argument: Path,
    capture_tool_repo_root_argument: Path,
    base: ManifestChain,
    phase0_artifact_ledger: Phase0ArtifactLedger,
    base_repo_root: Path,
) -> Phase8CaptureProvenance:
    if label not in PHASE8_CAPTURE_LABELS:
        raise AssertionError(f"unsupported Phase-8 capture label {label}")
    for value, description, pattern in (
        (expected_raw_sha256, "raw SHA-256", HASH_RE),
        (expected_ledger_sha256, "ledger SHA-256", HASH_RE),
        (expected_capture_wrapper_sha256, "capture-wrapper SHA-256", HASH_RE),
        (expected_product_revision, "product revision", re.compile(r"^[0-9a-f]{40}$")),
        (expected_capture_tool_revision, "capture-tool revision", re.compile(r"^[0-9a-f]{40}$")),
    ):
        if pattern.fullmatch(value) is None:
            raise AcceptanceError(
                f"{label}: external {description} is not canonical"
            )
    required_product = (
        PHASE8_ATTEMPT0_PRODUCT_REVISION
        if label == "phase8-generation"
        else PHASE8_RETRY1_PRODUCT_REVISION
    )
    if expected_product_revision != required_product:
        raise AcceptanceError(
            f"{label}: product revision must be exact immutable revision "
            f"{required_product}, not {expected_product_revision}"
        )
    if label == "phase8-generation":
        for actual, required, description in (
            (
                expected_raw_sha256,
                PHASE8_ATTEMPT0_RAW_SHA256,
                "raw SHA-256",
            ),
            (
                expected_ledger_sha256,
                PHASE8_ATTEMPT0_LEDGER_SHA256,
                "generic-v2 ledger SHA-256",
            ),
            (
                expected_capture_wrapper_sha256,
                PHASE8_ATTEMPT0_CAPTURE_WRAPPER_SHA256,
                "capture-wrapper SHA-256",
            ),
        ):
            if actual != required:
                raise AcceptanceError(
                    f"{label}: immutable attempt0 {description} must be "
                    f"{required}, not {actual}"
                )
        if expected_capture_tool_revision != PHASE8_ATTEMPT0_CAPTURE_TOOL_REVISION:
            raise AcceptanceError(
                f"{label}: capture-tool revision must be exact immutable revision "
                f"{PHASE8_ATTEMPT0_CAPTURE_TOOL_REVISION}"
            )
    elif expected_capture_tool_revision == PHASE8_ATTEMPT0_CAPTURE_TOOL_REVISION:
        raise AcceptanceError(
            f"{label}: retry capture-tool revision C must be distinct from attempt 0"
        )
    root = canonical_directory(
        capture_directory, f"{label} capture directory"
    )
    product_repo_root = canonical_directory(
        product_repo_root_argument, f"{label} product repository root"
    )
    capture_tool_repo_root = canonical_directory(
        capture_tool_repo_root_argument,
        f"{label} capture-tool repository root",
    )
    root_signature = _stable_directory_signature(root.lstat())
    raw_path = regular(raw_path_argument, f"{label} raw TSV")
    if raw_path != root / "raw_trials.tsv":
        raise AcceptanceError(
            f"{label}: raw TSV is not the canonical capture raw_trials.tsv"
        )
    ledger_path = root / GENERIC_LEDGER_NAME
    seal_path = root / GENERIC_LEDGER_SEAL_NAME
    ledger_payload, ledger_signature = _read_capture_control(
        ledger_path,
        f"{label} generic-v2 ledger",
        maximum_bytes=64 * 1024 * 1024,
    )
    actual_ledger_sha256 = sha256_bytes(ledger_payload)
    if actual_ledger_sha256 != expected_ledger_sha256:
        raise AcceptanceError(
            f"{label}: generic-v2 ledger differs from external anchor: "
            f"{actual_ledger_sha256} != {expected_ledger_sha256}"
        )
    seal_payload, seal_signature = _read_capture_control(
        seal_path,
        f"{label} generic-v2 ledger seal",
        maximum_bytes=256,
    )
    if seal_payload != (
        f"{actual_ledger_sha256}  {GENERIC_LEDGER_NAME}\n".encode("ascii")
    ):
        raise AcceptanceError(
            f"{label}: generic-v2 ledger detached seal is not exact"
        )
    ledger_records = _parse_generic_v2_ledger(
        ledger_payload, f"{label} generic-v2 ledger"
    )
    members = _scan_capture_members(root)
    actual_records = tuple(
        (
            member.kind,
            member.sha256 if member.sha256 is not None else "-",
            str(member.size) if member.size is not None else "-",
            f"{member.mode:04o}",
            member.relative,
        )
        for member in members
    )
    if ledger_records != actual_records:
        raise AcceptanceError(
            f"{label}: generic-v2 ledger does not match the capture closure"
        )
    by_relative = {member.relative: member for member in members}
    metadata_member = by_relative.get(PHASE8_METADATA_NAME)
    raw_member = by_relative.get("raw_trials.tsv")
    if (
        metadata_member is None
        or metadata_member.kind != "file"
        or raw_member is None
        or raw_member.kind != "file"
    ):
        raise AcceptanceError(
            f"{label}: generic-v2 ledger omits metadata or raw_trials.tsv"
        )
    raw_payload, raw_sha256, _, raw_signature = _stable_read_bytes(
        raw_path,
        f"{label} exact anchored raw TSV",
        maximum_bytes=128 * 1024 * 1024,
    )
    if (
        raw_sha256 != expected_raw_sha256
        or raw_member.sha256 != expected_raw_sha256
        or raw_member.signature != raw_signature
    ):
        raise AcceptanceError(
            f"{label}: raw TSV hash differs between external anchor, ledger, and evidence"
        )
    metadata_path = root / PHASE8_METADATA_NAME
    metadata, metadata_sha256 = _strict_canonical_json(
        metadata_path, f"{label} canonical v2 capture metadata"
    )
    if metadata_sha256 != metadata_member.sha256:
        raise AcceptanceError(f"{label}: metadata differs from generic-v2 ledger")
    try:
        capture_contract.validate_metadata_shape(metadata)
    except capture_contract.CaptureError as error:
        raise AcceptanceError(
            f"{label}: invalid full schema-v2 capture metadata: {error}"
        ) from error
    if (
        metadata.get("schema") != "wric.benchmark_capture"
        or metadata.get("schema_version") != 2
        or metadata.get("run_label") != label
        or metadata.get("capture_dir") != os.fspath(root)
    ):
        raise AcceptanceError(
            f"{label}: capture metadata schema/label/directory binding is not exact"
        )
    outputs = _metadata_mapping(
        metadata.get("capture_outputs"), f"{label} capture outputs"
    )
    if set(outputs) != {
        "commands", "raw_trial_rows", "raw_trials", "row_ids", "summary"
    }:
        raise AcceptanceError(f"{label}: capture-output key set is not exact")
    raw_snapshot = _metadata_mapping(
        outputs.get("raw_trials"), f"{label} raw snapshot"
    )
    if (
        raw_snapshot.get("path") != os.fspath(raw_path)
        or raw_snapshot.get("sha256") != expected_raw_sha256
        or raw_snapshot.get("bytes") != raw_member.size
        or raw_snapshot.get("mode") != raw_member.mode
        or raw_snapshot.get("nlink") != 1
    ):
        raise AcceptanceError(
            f"{label}: metadata does not bind the exact raw path/hash/snapshot"
        )
    _require_metadata_snapshot_member(
        raw_snapshot,
        raw_path,
        raw_member,
        f"{label} raw snapshot",
    )
    expected_ids = sorted(
        f"phase8-generation-tree0-off-w{worker}" for worker in (1, 8)
    )
    if (
        outputs.get("raw_trial_rows") != 10
        or outputs.get("row_ids") != expected_ids
    ):
        raise AcceptanceError(
            f"{label}: metadata raw row/cardinality identity is not exact"
        )
    configuration = _metadata_mapping(
        metadata.get("harness_configuration"),
        f"{label} harness configuration",
    )
    if set(configuration) != {
        "affinity_class",
        "base_manifest",
        "frozen_larch2",
        "frozen_process_metrics",
        "full_canonical_correctness",
        "out_dir",
        "product_dagutil",
        "repetitions",
        "run_label",
        "run_manifest_group",
        "supplement_manifest_ids",
        "supplement_manifests",
        "warmups",
        "workers_list",
    }:
        raise AcceptanceError(f"{label}: harness-configuration key set is not exact")
    if (
        configuration.get("run_label") != label
        or configuration.get("run_manifest_group") != "phase8-generation"
        or configuration.get("out_dir") != os.fspath(root)
        or configuration.get("workers_list") != "1,8"
        or configuration.get("repetitions") != "5"
        or configuration.get("warmups") != "1"
        or configuration.get("full_canonical_correctness") is not True
        or configuration.get("affinity_class") != "P"
        or configuration.get("base_manifest") != os.fspath(base.path)
        or configuration.get("supplement_manifest_ids")
        != ["phase8-generation"]
        or configuration.get("supplement_manifests")
        != [os.fspath(base.supplements["phase8"].path)]
    ):
        raise AcceptanceError(
            f"{label}: metadata harness route is not the exact Phase-8 capture"
        )
    if (
        metadata.get("working_directory") != os.fspath(base_repo_root)
        or metadata.get("environment")
        != capture_contract.harness_environment(base_repo_root)
        or metadata.get("umask") != "0022"
        or metadata.get("affinity")
        != {
            "pre": PHASE8_AFFINITY,
            "post": PHASE8_AFFINITY,
            "requested": PHASE8_AFFINITY,
        }
    ):
        raise AcceptanceError(
            f"{label}: environment/affinity/working-directory contract is not exact"
        )
    _metadata_revision(
        metadata,
        "product",
        expected_product_revision,
        product_repo_root,
        label,
    )
    _metadata_revision(
        metadata,
        "capture_tool",
        expected_capture_tool_revision,
        capture_tool_repo_root,
        label,
    )
    tools = _metadata_mapping(metadata.get("tools"), f"{label} tools")
    wrapper = _metadata_mapping(
        tools.get("capture_wrapper"), f"{label} capture-wrapper tool"
    )
    tracked = _metadata_mapping(
        metadata.get("tracked_git_blobs"), f"{label} tracked Git blobs"
    )
    tracked_wrapper = _metadata_mapping(
        tracked.get("capture_wrapper"), f"{label} tracked capture wrapper"
    )
    tracked_working = _metadata_mapping(
        tracked_wrapper.get("working_file"),
        f"{label} tracked capture-wrapper working file",
    )
    if (
        wrapper.get("sha256") != expected_capture_wrapper_sha256
        or tracked_wrapper.get("blob_sha256") != expected_capture_wrapper_sha256
        or tracked_working.get("sha256") != expected_capture_wrapper_sha256
        or tracked_wrapper.get("working_file") != wrapper
        or tracked_wrapper.get("repository")
        != os.fspath(capture_tool_repo_root)
        or tracked_wrapper.get("relative")
        != "tools/wric_benchmark_capture.py"
    ):
        raise AcceptanceError(
            f"{label}: capture-wrapper hash/provenance differs from its external anchor"
        )
    wrapper_path = regular(
        capture_tool_repo_root / "tools/wric_benchmark_capture.py",
        f"{label} externally routed capture wrapper",
    )
    wrapper_digest, _, wrapper_mode, _ = _stable_hash(
        wrapper_path, f"{label} externally routed capture wrapper"
    )
    if (
        wrapper_digest != expected_capture_wrapper_sha256
        or wrapper_mode & 0o111 == 0
        or wrapper.get("path") != os.fspath(wrapper_path)
    ):
        raise AcceptanceError(
            f"{label}: externally routed capture-wrapper root/path/hash is not exact"
        )
    _require_live_metadata_snapshot(
        wrapper,
        wrapper_path,
        f"{label} externally routed capture wrapper",
        executable=True,
    )
    generic_ledger = _metadata_mapping(
        tools.get("generic_ledger"), f"{label} generic-ledger tool"
    )
    generic_ledger_path = regular(
        capture_tool_repo_root / "tools/wric_evidence_run_ledger.py",
        f"{label} externally routed generic ledger",
    )
    if generic_ledger.get("path") != os.fspath(generic_ledger_path):
        raise AcceptanceError(
            f"{label}: generic-ledger tool is not routed through its capture-tool root"
        )
    _require_live_metadata_snapshot(
        generic_ledger,
        generic_ledger_path,
        f"{label} externally routed generic ledger",
        executable=True,
    )
    tracked_ledger = _metadata_mapping(
        tracked.get("generic_ledger"), f"{label} tracked generic ledger"
    )
    if (
        tracked_ledger.get("working_file") != generic_ledger
        or tracked_ledger.get("repository")
        != os.fspath(capture_tool_repo_root)
        or tracked_ledger.get("relative")
        != "tools/wric_evidence_run_ledger.py"
    ):
        raise AcceptanceError(
            f"{label}: generic-ledger HEAD/tool provenance is not exact"
        )
    harness_provenance = _metadata_mapping(
        metadata.get("harness_provenance"), f"{label} harness provenance"
    )
    benchmark_harness = _metadata_mapping(
        tools.get("benchmark_harness"), f"{label} benchmark harness"
    )
    product_harness = _metadata_mapping(
        _metadata_mapping(
            metadata.get("tracked_git_blobs"), f"{label} tracked Git blobs"
        ).get("product_harness"),
        f"{label} product harness Git blob",
    )
    if (
        harness_provenance.get("kind") != "exact_product_tracked"
        or harness_provenance.get("expected_metadata_sha256") != "-"
        or harness_provenance.get("expected_harness_sha256")
        != benchmark_harness.get("sha256")
        or harness_provenance.get("product_git_blob") != product_harness
        or product_harness.get("working_file") != benchmark_harness
        or product_harness.get("repository") != os.fspath(product_repo_root)
        or product_harness.get("relative")
        != "tools/wric_spr_search_benchmark.sh"
        or benchmark_harness.get("path")
        != os.fspath(product_repo_root / "tools/wric_spr_search_benchmark.sh")
    ):
        raise AcceptanceError(
            f"{label}: exact product-harness provenance is not fully bound"
        )
    harness_path = regular(
        product_repo_root / "tools/wric_spr_search_benchmark.sh",
        f"{label} exact product benchmark harness",
    )
    _require_live_metadata_snapshot(
        benchmark_harness,
        harness_path,
        f"{label} exact product benchmark harness",
        executable=True,
    )
    harness_argv = metadata.get("harness_argv")
    if (
        not isinstance(harness_argv, list)
        or not harness_argv
        or not all(isinstance(token, str) for token in harness_argv)
        or harness_argv[0] != benchmark_harness.get("path")
        or metadata.get("harness_argv_sha256")
        != capture_contract.argv_digest(harness_argv)
    ):
        raise AcceptanceError(f"{label}: harness argv/digest binding is invalid")
    chain = _metadata_mapping(
        metadata.get("phase0_chain"), f"{label} Phase-0 chain"
    )
    chain_base = _metadata_mapping(
        chain.get("base_manifest"), f"{label} Phase-0 base manifest"
    )
    chain_artifacts = _metadata_mapping(
        chain.get("artifact_ledger"), f"{label} Phase-0 artifact ledger"
    )
    chain_root = _metadata_mapping(
        chain.get("phase0_root"), f"{label} Phase-0 root"
    )
    chain_supplements = chain.get("supplement_manifests")
    if chain_root.get("path") != os.fspath(base_repo_root):
        raise AcceptanceError(f"{label}: Phase-0 root binding is invalid")
    if (
        chain_base.get("path") != os.fspath(base.path)
        or chain_base.get("expected_sha256") != base.sha256
        or chain_artifacts.get("path")
        != os.fspath(phase0_artifact_ledger.path)
        or chain_artifacts.get("expected_sha256")
        != phase0_artifact_ledger.sha256
        or not isinstance(chain_supplements, list)
        or len(chain_supplements) != 1
        or not isinstance(chain_supplements[0], Mapping)
        or chain_supplements[0].get("path")
        != os.fspath(base.supplements["phase8"].path)
        or chain_supplements[0].get("expected_sha256")
        != base.supplements["phase8"].sha256
    ):
        raise AcceptanceError(
            f"{label}: Phase-0 manifest/supplement/artifact chain is not exact"
        )
    execution = _metadata_mapping(
        metadata.get("harness_execution"), f"{label} harness execution"
    )
    if set(execution) != {
        "returncode", "stderr", "stderr_name", "stdout", "stdout_name"
    } or (
        execution.get("returncode") != 0
        or execution.get("stdout_name") != "wric-benchmark-harness.stdout"
        or execution.get("stderr_name") != "wric-benchmark-harness.stderr"
    ):
        raise AcceptanceError(
            f"{label}: harness execution is not one exact successful run"
        )
    for stream, basename in (
        ("stdout", "wric-benchmark-harness.stdout"),
        ("stderr", "wric-benchmark-harness.stderr"),
    ):
        member = by_relative.get(basename)
        if member is None or member.kind != "file":
            raise AcceptanceError(
                f"{label}: harness {stream} is absent from the ledger closure"
            )
        _require_metadata_snapshot_member(
            execution.get(stream), root / basename, member,
            f"{label} harness {stream}",
        )
    for role, basename in (("commands", "commands.sh"), ("summary", "summary.md")):
        member = by_relative.get(basename)
        if member is None or member.kind != "file":
            raise AcceptanceError(
                f"{label}: capture output {role} is absent from the ledger closure"
            )
        _require_metadata_snapshot_member(
            outputs.get(role), root / basename, member,
            f"{label} capture output {role}",
        )
    if _stable_directory_signature(root.lstat()) != root_signature:
        raise AcceptanceError(f"{label}: capture changed during provenance loading")
    return Phase8CaptureProvenance(
        label=label,
        capture_directory=root,
        capture_signature=root_signature,
        raw_path=raw_path,
        raw_sha256=expected_raw_sha256,
        raw_payload=raw_payload,
        raw_signature=raw_signature,
        metadata_path=metadata_path,
        metadata_sha256=metadata_sha256,
        metadata=metadata,
        ledger_path=ledger_path,
        ledger_sha256=actual_ledger_sha256,
        ledger_signature=ledger_signature,
        seal_path=seal_path,
        seal_sha256=sha256_bytes(seal_payload),
        seal_signature=seal_signature,
        product_revision=expected_product_revision,
        capture_tool_revision=expected_capture_tool_revision,
        capture_wrapper_sha256=expected_capture_wrapper_sha256,
        product_repo_root=product_repo_root,
        capture_tool_repo_root=capture_tool_repo_root,
        members=members,
    )


def single_row_source(
    evidence: RawEvidence, row_id: str, repetitions: int
) -> Path:
    owners = {
        regular(Path(row["__raw_path__"]), f"{evidence.label} owning raw TSV")
        for row in evidence.selected(row_id, repetitions)
    }
    if len(owners) != 1:
        raise AcceptanceError(
            f"{evidence.label}: {row_id} repetitions span multiple raw TSV owners"
        )
    return next(iter(owners))


def deep_phase4_validate(
    raw: RawEvidence,
    phase3_raw_path: Path,
    working_repo_root: Path,
    physical_memory: int,
) -> Mapping[str, object]:
    tool = regular(
        Path(__file__).with_name("wric_phase4_acceptance.py"),
        "Phase-4 deep acceptance evaluator",
    )
    command = [sys.executable, os.fspath(tool)]
    for path in raw.paths:
        command.extend(("--raw-trials", os.fspath(path)))
    command.extend(
        (
            "--repetitions",
            "5",
            "--phase3-raw-trials",
            os.fspath(phase3_raw_path),
            "--physical-memory-bytes",
            str(physical_memory),
        )
    )
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        command,
        cwd=working_repo_root,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise AcceptanceError(f"deep Phase-4 acceptance failed: {detail}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise AcceptanceError(
            f"deep Phase-4 acceptance emitted invalid JSON: {error}"
        ) from error
    if not isinstance(payload, dict) or payload.get("status") != "pass":
        raise AcceptanceError(
            "deep Phase-4 acceptance did not return final status=pass"
        )
    return payload


Phase4Validator = Callable[
    [RawEvidence, Path, Path, int], Mapping[str, object]
]


def deep_phase8_capture_audit(
    provenance: Phase8CaptureProvenance,
    manifests: ManifestChain,
    phase0_artifact_ledger: Phase0ArtifactLedger,
    base_repo_root: Path,
) -> Mapping[str, object]:
    """Audit one capture with the exact externally anchored capture wrapper."""
    wrapper = regular(
        provenance.capture_tool_repo_root
        / "tools/wric_benchmark_capture.py",
        f"{provenance.label} exact capture-wrapper audit route",
    )
    wrapper_digest, _, wrapper_mode, _ = _stable_hash(
        wrapper, f"{provenance.label} exact capture-wrapper audit route"
    )
    if (
        wrapper_digest != provenance.capture_wrapper_sha256
        or wrapper_mode & 0o111 == 0
    ):
        raise AcceptanceError(
            f"{provenance.label}: exact capture-wrapper audit route changed"
        )
    tools = _metadata_mapping(
        provenance.metadata.get("tools"), f"{provenance.label} tools"
    )
    harness = _metadata_mapping(
        tools.get("benchmark_harness"),
        f"{provenance.label} benchmark harness",
    )
    harness_provenance = _metadata_mapping(
        provenance.metadata.get("harness_provenance"),
        f"{provenance.label} harness provenance",
    )
    harness_path = harness.get("path")
    harness_sha256 = harness_provenance.get("expected_harness_sha256")
    harness_metadata_sha256 = harness_provenance.get(
        "expected_metadata_sha256"
    )
    if not all(
        isinstance(value, str)
        for value in (
            harness_path,
            harness_sha256,
            harness_metadata_sha256,
        )
    ):
        raise AcceptanceError(
            f"{provenance.label}: harness audit anchors are not strings"
        )
    taskset = regular(Path("/usr/bin/taskset"), "taskset audit launcher")
    supplement = manifests.supplements["phase8"]
    command = [
        os.fspath(taskset),
        "-c",
        PHASE8_AFFINITY,
        os.fspath(wrapper),
        "audit",
        "--capture-dir",
        os.fspath(provenance.capture_directory),
        "--expected-ledger-sha256",
        provenance.ledger_sha256,
        "--expected-run-label",
        provenance.label,
        "--expected-affinity-cpus",
        PHASE8_AFFINITY,
        "--product-repo-root",
        os.fspath(provenance.product_repo_root),
        "--expected-product-revision",
        provenance.product_revision,
        "--capture-tool-repo-root",
        os.fspath(provenance.capture_tool_repo_root),
        "--expected-capture-tool-revision",
        provenance.capture_tool_revision,
        "--benchmark-harness",
        harness_path,
        "--expected-harness-sha256",
        harness_sha256,
        "--expected-harness-metadata-sha256",
        harness_metadata_sha256,
        "--phase0-base-root",
        os.fspath(base_repo_root),
        "--base-manifest",
        os.fspath(manifests.base.path),
        "--expected-base-manifest-sha256",
        manifests.base.sha256,
        "--supplement-manifest",
        os.fspath(supplement.path),
        "--expected-supplement-manifest-sha256",
        supplement.sha256,
        "--expected-phase0-artifact-ledger-sha256",
        phase0_artifact_ledger.sha256,
    ]
    environment = {
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "PYTHONDONTWRITEBYTECODE": "1",
        "TMPDIR": "/tmp",
        "TZ": "Europe/Sofia",
    }
    result = subprocess.run(
        command,
        cwd=base_repo_root,
        env=environment,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise AcceptanceError(
            f"{provenance.label}: exact capture-wrapper audit failed: {detail}"
        )
    if result.stderr:
        raise AcceptanceError(
            f"{provenance.label}: exact capture-wrapper audit emitted stderr"
        )
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise AcceptanceError(
            f"{provenance.label}: exact capture-wrapper audit emitted invalid JSON: "
            f"{error}"
        ) from error
    if (
        not isinstance(payload, dict)
        or set(payload)
        != {"capture_dir", "ledger_sha256", "member_count", "run_label", "status"}
        or payload.get("capture_dir")
        != os.fspath(provenance.capture_directory)
        or payload.get("ledger_sha256") != provenance.ledger_sha256
        or payload.get("run_label") != provenance.label
        or payload.get("status") != "audited"
        or isinstance(payload.get("member_count"), bool)
        or not isinstance(payload.get("member_count"), int)
        or payload["member_count"] <= 0
    ):
        raise AcceptanceError(
            f"{provenance.label}: exact capture-wrapper audit result is not exact"
        )
    return payload


Phase8CaptureAuditor = Callable[
    [Phase8CaptureProvenance, ManifestChain, Phase0ArtifactLedger, Path],
    Mapping[str, object],
]


def deep_phase9_validate(
    manifests: ManifestChain,
    raw: RawEvidence,
    base_repo_root: Path,
    working_repo_root: Path,
    expected_run_ledger_sha256: str,
) -> Mapping[str, object]:
    if len(raw.paths) != 1:
        raise AcceptanceError("phase9 requires exactly one sealed benchmark raw TSV")
    tool = regular(
        Path(__file__).with_name("wric_phase9_acceptance.py"),
        "Phase-9 deep acceptance evaluator",
    )
    command = [
        sys.executable,
        os.fspath(tool),
        "evaluate",
        "--raw-trials",
        os.fspath(raw.paths[0]),
        "--base-manifest",
        os.fspath(manifests.base.path),
        "--expected-parent-sha256",
        manifests.base.sha256,
        "--supplement",
        os.fspath(manifests.supplements["phase9"].path),
        "--base-repo-root",
        os.fspath(base_repo_root),
        "--working-repo-root",
        os.fspath(working_repo_root),
        "--expected-run-ledger-sha256",
        expected_run_ledger_sha256,
    ]
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        command,
        cwd=working_repo_root,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise AcceptanceError(f"deep Phase-9 acceptance failed: {detail}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise AcceptanceError(f"deep Phase-9 acceptance emitted invalid JSON: {error}") from error
    if not isinstance(payload, dict) or payload.get("status") != "pass":
        raise AcceptanceError("deep Phase-9 acceptance did not return final status=pass")
    return payload


Phase9Validator = Callable[
    [ManifestChain, RawEvidence, Path, Path, str], Mapping[str, object]
]


def exact_worker_matrix(
    manifests: ManifestChain,
    group: str,
    max_candidates: int,
    top_k: int,
    methods: Sequence[str],
) -> dict[tuple[str, int], str]:
    result: dict[tuple[str, int], str] = {}
    for method in methods:
        for worker in (1, 2, 4, 8):
            matches = [
                row_id
                for row_id, row in manifests.base.rows.items()
                if row["run_group"] == group
                and row["method"] == method
                and manifest_worker(row) == str(worker)
                and row["chart_max_candidates"] == str(max_candidates)
                and row["chart_top_k_exact"] == str(top_k)
            ]
            if len(matches) != 1:
                raise AcceptanceError(
                    "sealed base does not define exactly one Phase-6 row for "
                    f"{group}/{method}/{max_candidates}c/TopK{top_k}/W{worker}: "
                    f"{sorted(matches)}"
                )
            result[(method, worker)] = matches[0]
    return result


def require_manifest_work_contract(
    manifests: ManifestChain,
    row_ids: set[str],
    max_candidates: int,
    top_k: int,
    label: str,
) -> None:
    chart_rows = 0
    for row_id in sorted(row_ids):
        row = manifests.rows[row_id]
        if row["method"] == METHOD_NATIVE:
            continue
        chart_rows += 1
        if (
            row["chart_max_candidates"] != str(max_candidates)
            or row["chart_top_k_exact"] != str(top_k)
        ):
            raise AcceptanceError(
                f"{label}: sealed row {row_id} work contract is "
                f"{row['chart_max_candidates']}/{row['chart_top_k_exact']}, "
                f"expected {max_candidates}/{top_k}"
            )
    if chart_rows == 0:
        raise AcceptanceError(f"{label}: sealed selection contains no chart rows")


def validate_selected_work(
    evidence: RawEvidence,
    row_ids: set[str],
    manifests: ManifestChain,
) -> None:
    for row in evidence.rows:
        row_id = row["row_id"]
        if row_id not in row_ids or row["method"] == METHOD_NATIVE:
            continue
        manifest = manifests.rows[row_id]
        where = f"{evidence.label} {row_id} trial {row['trial_index']}"
        iterations = checked_uint64(
            manifest["iterations"], f"{where} sealed iterations", positive=True
        )
        max_candidates = checked_uint64(
            manifest["chart_max_candidates"],
            f"{where} sealed chart_max_candidates",
            positive=True,
        )
        top_k = checked_uint64(
            manifest["chart_top_k_exact"],
            f"{where} sealed chart_top_k_exact",
            positive=True,
        )
        if top_k > max_candidates:
            raise AcceptanceError(
                f"{where}: sealed exact Top-K exceeds the candidate budget"
            )
        expected_candidates = checked_uint64_product(
            iterations,
            max_candidates,
            f"{where} expected candidate-score work",
        )
        expected_exact = checked_uint64_product(
            iterations,
            top_k,
            f"{where} expected exact-verification work",
        )
        actual_candidates = checked_uint64(
            row["candidates_scored"], f"{where} candidates_scored"
        )
        actual_exact = checked_uint64(
            row["exact_verifications"], f"{where} exact_verifications"
        )
        if actual_candidates != expected_candidates:
            raise AcceptanceError(
                f"{where}: candidates_scored={actual_candidates}, expected "
                f"iterations*chart_max_candidates={expected_candidates}"
            )
        if actual_exact != expected_exact:
            raise AcceptanceError(
                f"{where}: exact_verifications={actual_exact}, expected "
                f"iterations*chart_top_k_exact={expected_exact}"
            )


def evaluate(
    base: ManifestChain,
    phase0: RawEvidence,
    phase0_artifact_ledger: Phase0ArtifactLedger,
    runs: Mapping[str, RawEvidence],
    phase8_provenance: Mapping[str, Phase8CaptureProvenance],
    *,
    evaluation_mode: str,
    base_repo_root: Path,
    working_repo_root: Path,
    phase9_run_ledger_sha256: str,
    phase8_capture_auditor: Phase8CaptureAuditor = deep_phase8_capture_audit,
    phase4_validator: Phase4Validator = deep_phase4_validate,
    phase9_validator: Phase9Validator = deep_phase9_validate,
) -> dict[str, object]:
    if evaluation_mode not in EVALUATION_MODES:
        raise AcceptanceError(f"unsupported evaluation mode {evaluation_mode!r}")
    current_retry_bindings = {
        label: capture_contract.CURRENT_PRODUCT_RUN_REVISIONS.get(label)
        for label in CURRENT_PRODUCT_RETRY_LABELS
    }
    if current_retry_bindings != {
        label: PHASE6_ACCEPTANCE_PRODUCT_REVISION
        for label in CURRENT_PRODUCT_RETRY_LABELS
    }:
        raise AcceptanceError(
            "internal Phase-3/4/7 retries are not bound to the immutable "
            "current product"
        )
    phase6_source_labels = set(PHASE6_ACCEPTANCE_SOURCES.values())
    if (
        set(PHASE6_ACCEPTANCE_SOURCES) != {1, 4, 16}
        or len(phase6_source_labels) != len(PHASE6_ACCEPTANCE_SOURCES)
        or {
            label: capture_contract.CURRENT_PRODUCT_RUN_REVISIONS.get(label)
            for label in phase6_source_labels
        }
        != {
            label: PHASE6_ACCEPTANCE_PRODUCT_REVISION
            for label in phase6_source_labels
        }
    ):
        raise AcceptanceError(
            "internal Phase-6 acceptance sources are not uniquely bound to "
            "the immutable current product"
        )
    scoped_run_labels = tuple(
        label
        for label in RUN_LABELS
        if evaluation_mode == "final" or label != "final-default-auto"
    )
    if set(runs) != set(scoped_run_labels):
        raise AcceptanceError("internal run scope differs from evaluation mode")
    if set(phase8_provenance) != set(PHASE8_CAPTURE_LABELS):
        raise AcceptanceError("Phase-8 capture provenance label set is not exact")
    validate_phase0(phase0, base)

    # Required row/cardinality contracts.  A direct harness output is
    # group-complete, so sealed extras from these groups remain admissible.
    phase1_ids = {MEDIUM_DENSE.format(1), SMALL_DENSE.format(1), SMALL_EXACT.format(1), MEDIUM_CACHE.format(1), MEDIUM_LAZY.format(1)}
    phase2_ids = {MEDIUM_DENSE.format(1), SMALL_EXACT.format(1)}
    phase3_ids = {
        SMALL_DENSE.format(1),
        SMALL_DENSE.format(8),
        MEDIUM_DENSE.format(1),
    }
    phase4_ids = {
        template.format(worker)
        for template in (MEDIUM_DENSE, MEDIUM_CACHE)
        for worker in (1, 2, 4, 8)
    }
    phase5_ids = {MEDIUM_EXACT.format(1), MEDIUM_EXACT.format(8)}
    phase6_matrices = {
        1: exact_worker_matrix(
            base,
            "p0-medium-exact1-physical",
            1,
            1,
            (METHOD_EXACT,),
        ),
        4: exact_worker_matrix(
            base,
            "p0-primary-physical",
            32,
            4,
            (METHOD_SAMPLED, METHOD_EXACT, METHOD_HYBRID),
        ),
        16: exact_worker_matrix(
            base,
            "p0-stress-physical",
            128,
            16,
            (METHOD_SAMPLED, METHOD_EXACT, METHOD_HYBRID),
        ),
    }
    phase6_ids_by_top_k = {
        top_k: set(matrix.values()) for top_k, matrix in phase6_matrices.items()
    }
    runs["phase1"].required_rows(phase1_ids, 5, base)
    runs["phase2"].required_rows(phase2_ids, 5, base)
    runs["phase3"].required_rows(phase3_ids, 5, base)
    runs["phase4"].required_rows(phase4_ids, 5, base)
    runs["phase5"].required_rows(phase5_ids, 5, base)

    high_prefix = "phase7-lazy-high-compression"
    dense_prefix = "phase7-lazy-dense-favoring"
    high_ids = {f"{high_prefix}-{policy}-w{worker}" for policy in ("off", "on", "auto") for worker in (1, 8)}
    runs["phase7-high"].required_rows(high_ids, 5, base)
    small_ids = {SMALL_DENSE.format(worker) for worker in (1, 8)} | {f"phase7-lazy-completion-tree0-on-w{worker}" for worker in (1, 8)}
    runs["phase7-small"].required_rows(small_ids, 5, base)
    medium_ids = {MEDIUM_DENSE.format(worker) for worker in (1, 8)} | {MEDIUM_LAZY.format(worker) for worker in (1, 8)} | {f"phase7-lazy-completion-medium-auto-w{worker}" for worker in (1, 8)}
    dense_ids = {f"{dense_prefix}-{policy}-w{worker}" for policy in ("off", "on", "auto") for worker in (1, 8)}
    runs["phase7-auto"].required_rows(medium_ids | dense_ids, 5, base)
    phase8_ids = {f"phase8-generation-tree0-off-w{worker}" for worker in (1, 8)}
    runs["phase8-generation"].required_rows(phase8_ids, 5, base)
    runs["phase8-generation-retry1"].required_rows(phase8_ids, 5, base)
    runs["phase8-end-to-end"].required_rows(phase8_ids, 5, base)

    final_phase6_exact1 = phase6_ids_by_top_k[1]
    runs["final-phase6-exact1"].required_rows(
        final_phase6_exact1, 3, base
    )
    final_scaling = base.group("p0-primary-physical", {"1", "2", "4", "8"})
    runs["final-scaling"].required_rows(final_scaling, 3, base)
    final_primary = base.group("p0-primary-physical", {"1", "8"})
    runs["final-primary"].required_rows(final_primary, 5, base)
    final_smt = base.group("p0-primary-smt", {"1", "2", "4", "8", "16", "auto"})
    runs["final-smt"].required_rows(final_smt, 3, base)
    final_small = base.group("p0-small-auto", {"1", "auto"})
    runs["final-small-auto"].required_rows(final_small, 5, base)
    unpinned_group = "p0-primary-unpinned" if any(row["run_group"] == "p0-primary-unpinned" for row in base.base.rows.values()) else "p0-primary-smt"
    final_unpinned = base.group(unpinned_group, {"auto"})
    runs["final-unpinned-auto"].required_rows(final_unpinned, 5, base)
    final_default = base.group(unpinned_group, {"auto", "default"})
    if evaluation_mode == "final":
        runs["final-default-auto"].required_rows(final_default, 5, base)
    final_stress = base.group(
        "p0-stress-physical", {"1", "2", "4", "8"}
    )
    runs["final-stress"].required_rows(final_stress, 3, base)
    final_real = base.group("real-bounded", {"1", "8"})
    runs["final-real"].required_rows(final_real, 3, base)
    phase9_ids = {f"phase9-local-commit-seed{seed}-w{worker}" for seed in (1, 7, 19) for worker in (1, 8)}
    runs["phase9"].required_rows(phase9_ids, 3, base)
    for top_k, max_candidates in ((1, 1), (4, 32), (16, 128)):
        require_manifest_work_contract(
            base,
            set(phase6_matrices[top_k].values()),
            max_candidates,
            top_k,
            f"phase6 TopK{top_k}",
        )
    strict_work_by_label = {
        "final-phase6-exact1": final_phase6_exact1,
        "final-scaling": final_scaling,
        "final-primary": final_primary,
        "final-smt": final_smt,
        "final-unpinned-auto": final_unpinned,
        "final-default-auto": final_default,
        "final-stress": final_stress,
    }
    require_manifest_work_contract(
        base,
        final_phase6_exact1,
        1,
        1,
        "final-phase6-exact1",
    )
    for label in (
        "final-scaling",
        "final-primary",
        "final-smt",
        "final-unpinned-auto",
        "final-default-auto",
    ):
        require_manifest_work_contract(
            base, strict_work_by_label[label], 32, 4, label
        )
    require_manifest_work_contract(
        base, final_stress, 128, 16, "final-stress"
    )
    required_by_label = {
        "phase1": phase1_ids,
        "phase2": phase2_ids,
        "phase3": phase3_ids,
        "phase4": phase4_ids,
        "phase5": phase5_ids,
        "phase7-high": high_ids,
        "phase7-small": small_ids,
        "phase7-auto": medium_ids | dense_ids,
        "phase8-generation": phase8_ids,
        "phase8-generation-retry1": phase8_ids,
        "phase8-end-to-end": phase8_ids,
        "final-phase6-exact1": final_phase6_exact1,
        "final-scaling": final_scaling,
        "final-primary": final_primary,
        "final-smt": final_smt,
        "final-small-auto": final_small,
        "final-unpinned-auto": final_unpinned,
        "final-default-auto": final_default,
        "final-stress": final_stress,
        "final-real": final_real,
        "phase9": phase9_ids,
    }
    if evaluation_mode == "pre-default":
        del strict_work_by_label["final-default-auto"]
        del required_by_label["final-default-auto"]
    validated_required: dict[str, int] = {}
    for label in scoped_run_labels:
        validated_required[label] = validate_evidence(
            runs[label],
            base,
            required_rows=required_by_label[label],
            required_refusal=(label == "final-real"),
        )
        if label in strict_work_by_label:
            validate_selected_work(
                runs[label], strict_work_by_label[label], base
            )
    phase8_audit_results: dict[str, Mapping[str, object]] = {}
    for label in PHASE8_CAPTURE_LABELS:
        provenance = phase8_provenance[label]
        audit_result = phase8_capture_auditor(
            provenance,
            base,
            phase0_artifact_ledger,
            base_repo_root,
        )
        if (
            not isinstance(audit_result, Mapping)
            or audit_result.get("status") != "audited"
            or audit_result.get("run_label") != label
            or audit_result.get("capture_dir")
            != os.fspath(provenance.capture_directory)
            or audit_result.get("ledger_sha256") != provenance.ledger_sha256
        ):
            raise AcceptanceError(
                f"{label}: exact capture-wrapper auditor did not return the "
                "anchored audited capture"
            )
        phase8_audit_results[label] = audit_result
    memory_bytes = physical_memory_bytes()
    phase3_raw_path = single_row_source(
        runs["phase3"], MEDIUM_DENSE.format(1), 5
    )
    phase4_result = phase4_validator(
        runs["phase4"],
        phase3_raw_path,
        working_repo_root,
        memory_bytes,
    )
    if not isinstance(phase4_result, Mapping) or phase4_result.get("status") != "pass":
        raise AcceptanceError("deep Phase-4 validator did not return final status=pass")
    phase9_result = phase9_validator(
        base,
        runs["phase9"],
        base_repo_root,
        working_repo_root,
        phase9_run_ledger_sha256,
    )
    if not isinstance(phase9_result, Mapping) or phase9_result.get("status") != "pass":
        raise AcceptanceError("deep Phase-9 validator did not return final status=pass")

    global_rss_cap_kb = min(16 * 1024 * 1024, memory_bytes // (4 * 1024))
    gates = Gates()

    # Phases 1-3.
    for row_id, field in (
        (MEDIUM_DENSE.format(1), "local_scoring_ms"),
        (SMALL_DENSE.format(1), "wall_clock_s"),
        (SMALL_EXACT.format(1), "wall_clock_s"),
        (MEDIUM_CACHE.format(1), "wall_clock_s"),
        (MEDIUM_LAZY.format(1), "wall_clock_s"),
    ):
        baseline_compare(gates, f"phase1_{row_id}", phase0, runs["phase1"], base, row_id, 5, field, Decimal("1.05"))
    baseline_compare(gates, "phase2_medium_local_vs_phase0", phase0, runs["phase2"], base, MEDIUM_DENSE.format(1), 5, "local_scoring_ms", Decimal("0.70"))
    gates.ratio("phase2_medium_local_vs_phase1", med(runs["phase2"], MEDIUM_DENSE.format(1), 5, "local_scoring_ms"), med(runs["phase1"], MEDIUM_DENSE.format(1), 5, "local_scoring_ms"), Decimal("1.05"))
    gates.ratio("phase2_small_exact_vs_phase1", med(runs["phase2"], SMALL_EXACT.format(1), 5, "wall_clock_s"), med(runs["phase1"], SMALL_EXACT.format(1), 5, "wall_clock_s"), Decimal("0.75"))
    for row_id in (MEDIUM_DENSE.format(1), SMALL_EXACT.format(1)):
        frozen = [row for row in phase0.rows if row["row_id"] == row_id and row["status"] == "ok"]
        if not frozen:
            raise AcceptanceError(
                f"phase2 RSS comparator {row_id} lacks finite Phase-0 trials"
            )
        current_rss = rss_max(runs["phase2"], row_id, 5)
        frozen_rss = max(uint(row["peak_sampled_rss_kb"], f"phase0 {row_id} RSS") for row in frozen)
        gates.condition(f"phase2_{row_id}_rss", Decimal(current_rss) <= Decimal("1.25") * frozen_rss, "RSS exceeds 1.25x Phase 0")
    gates.ratio("phase3_small_w8_over_w1", med(runs["phase3"], SMALL_DENSE.format(8), 5, "wall_clock_s"), med(runs["phase3"], SMALL_DENSE.format(1), 5, "wall_clock_s"), Decimal("1.05"))

    # Phase 5 exact span and timeout bounds.
    p5_w1, p5_w8 = MEDIUM_EXACT.format(1), MEDIUM_EXACT.format(8)
    gates.ratio("phase5_exact_span_w8_over_w1", phase_sum(runs["phase5"], p5_w8, 5, ("exact_initialization_ms", "exact_verification_ms")), phase_sum(runs["phase5"], p5_w1, 5, ("exact_initialization_ms", "exact_verification_ms")), Decimal("0.50"))
    gates.condition("phase5_w1_under_600s", max_decimal(runs["phase5"], p5_w1, 5, "wall_clock_s") < 600, "a W1 trial reached the frozen timeout")
    gates.condition("phase5_w8_under_180s", max_decimal(runs["phase5"], p5_w8, 5, "wall_clock_s") < 180, "a W8 trial reached 180 seconds")
    rss_pair(gates, "phase5", runs["phase5"], p5_w1, p5_w8, 5, global_rss_cap_kb)

    # Phase 6 is accepted only from the immutable current product.  The 870c
    # historical label is a retained diagnostic and is intentionally absent
    # from accepting evaluator inputs.
    phase6_sources = {
        top_k: runs[label]
        for top_k, label in PHASE6_ACCEPTANCE_SOURCES.items()
    }
    for top_k, matrix in phase6_matrices.items():
        phase6 = phase6_sources[top_k]
        for method in sorted({key[0] for key in matrix}):
            method_name = method.removeprefix("chart_spr_")
            matrix_ids = tuple(matrix[(method, worker)] for worker in (1, 2, 4, 8))
            semantic_pair(
                phase6,
                matrix_ids,
                3,
                f"phase6 TopK{top_k} {method_name} worker matrix",
            )
            gates.condition(
                f"phase6_topk{top_k}_{method_name}_semantics",
                True,
                "worker-count semantics differ",
            )
            w8 = matrix[(method, 8)]
            repeated = phase6.selected(w8, 3)
            gates.condition(
                f"phase6_topk{top_k}_{method_name}_w8_repeat_identity",
                len(
                    {
                        (
                            row["trial_semantic_sha256"],
                            row["canonical_digest"],
                        )
                        for row in repeated
                    }
                )
                == 1,
                "repeated W8 canonical results are not byte-identical",
            )
            rss_pair(
                gates,
                f"phase6_topk{top_k}_{method_name}",
                phase6,
                matrix[(method, 1)],
                w8,
                3,
                global_rss_cap_kb,
            )
            for row_id in matrix_ids:
                for row in phase6.selected(row_id, 3):
                    exact = uint(
                        row["exact_verifications"],
                        f"phase6 {row_id} exact verifications",
                        positive=True,
                    )
                    if top_k == 1 and exact != 1:
                        raise AcceptanceError(
                            f"phase6 {row_id}: TopK1 did not perform exactly one verification"
                        )

    k4_grammar = phase6_matrices[4]
    phase6_topk4 = phase6_sources[4]
    k4_w1 = k4_grammar[(METHOD_EXACT, 1)]
    k4_w8 = k4_grammar[(METHOD_EXACT, 8)]
    for worker, row_id in ((1, k4_w1), (8, k4_w8)):
        for row in phase6_topk4.selected(row_id, 3):
            where = f"phase6 TopK4 W{worker} trial {row['trial_index']}"
            if uint(row["exact_verifications"], f"{where} exact verifications") != 4:
                raise AcceptanceError(f"{where}: exact verification count is not 4")
            exact_ms = decimal(
                row["exact_verification_ms"],
                f"{where} exact verification time",
                positive=True,
            )
            timing_max = decimal(
                row["exact_candidate_verification_ms_max"],
                f"{where} exact-candidate timing maximum",
            )
            if timing_max > exact_ms + Decimal("0.001"):
                raise AcceptanceError(
                    f"{where}: candidate timing maximum exceeds aggregate exact time"
                )
            parallel = uint(
                row["exact_candidate_parallel_batches"],
                f"{where} parallel batches",
            )
            peak = uint(
                row["peak_concurrent_exact_verifiers"],
                f"{where} peak exact verifiers",
            )
            axis = uint(
                row["chart_axis_exact_candidate_active_worker_high_water"],
                f"{where} exact-candidate high-water",
            )
            if worker == 8 and (parallel < 1 or peak < 2 or axis < 2):
                raise AcceptanceError(
                    f"{where}: candidate-parallel path was not activated"
                )
            if worker == 1 and (parallel != 0 or peak != 1 or axis > 1):
                raise AcceptanceError(
                    f"{where}: one-worker candidate scheduling evidence is inconsistent"
                )
    gates.ratio(
        "phase6_topk4_exact_verification_w8_over_w1",
        med(phase6_topk4, k4_w8, 3, "exact_verification_ms"),
        med(phase6_topk4, k4_w1, 3, "exact_verification_ms"),
        Decimal("0.50"),
    )
    gates.condition(
        "phase6_topk4_candidate_parallel_activation",
        True,
        "candidate-parallel scheduler evidence is incomplete",
    )

    phase6_topk16 = phase6_sources[16]
    for row_id in phase6_matrices[16].values():
        for row in phase6_topk16.selected(row_id, 3):
            where = f"phase6 TopK16 {row_id} trial {row['trial_index']}"
            uint(row["exact_verifications"], f"{where} exact verifications", positive=True)
            uint(
                row["exact_candidate_admission_batches"],
                f"{where} admission batches",
                positive=True,
            )
            admitted = uint(
                row["exact_candidate_peak_admitted_bytes"],
                f"{where} admitted bytes",
                positive=True,
            )
            projected = uint(
                row[ADMISSION_FIELD], f"{where} projected resident bytes", positive=True
            )
            budget = uint(
                row["configured_chart_memory_budget"],
                f"{where} memory budget",
                positive=True,
            )
            if admitted > projected or projected > budget:
                raise AcceptanceError(
                    f"{where}: concurrent-memory admission bound was exceeded"
                )
    gates.condition(
        "phase6_topk16_concurrent_memory_admission",
        True,
        "TopK16 admission evidence is incomplete",
    )

    # Phase 7 forced scaling, automatic policy, and both named branches.
    gates.ratio("phase7_high_forced_lazy_w8_over_w1", med(runs["phase7-high"], f"{high_prefix}-on-w8", 5, "wall_clock_s"), med(runs["phase7-high"], f"{high_prefix}-on-w1", 5, "wall_clock_s"), Decimal(2) / 3)
    small_on1, small_on8 = "phase7-lazy-completion-tree0-on-w1", "phase7-lazy-completion-tree0-on-w8"
    gates.ratio("phase7_small_forced_lazy_w8_over_w1", med(runs["phase7-small"], small_on8, 5, "wall_clock_s"), med(runs["phase7-small"], small_on1, 5, "wall_clock_s"), Decimal("1.10"))
    semantic_pair(runs["phase7-high"], tuple(sorted(high_ids)), 5, "phase7 high-compression forced/auto")
    semantic_pair(runs["phase7-small"], tuple(sorted(small_ids)), 5, "phase7 named small dense/lazy")
    semantic_pair(runs["phase7-auto"], tuple(sorted(dense_ids)), 5, "phase7 dense-favoring forced/auto")
    high_branch = lazy_auto_gate(gates, runs["phase7-high"], high_prefix, 5, "phase7_high_auto_w1", 1)
    lazy_auto_gate(gates, runs["phase7-high"], high_prefix, 5, "phase7_high_auto_w8", 8)
    dense_branch = lazy_auto_gate(gates, runs["phase7-auto"], dense_prefix, 5, "phase7_dense_auto_w1", 1)
    lazy_auto_gate(gates, runs["phase7-auto"], dense_prefix, 5, "phase7_dense_auto_w8", 8)
    medium_off, medium_on, medium_auto = MEDIUM_DENSE.format(1), MEDIUM_LAZY.format(1), "phase7-lazy-completion-medium-auto-w1"
    medium_medians = {"off": med(runs["phase7-auto"], medium_off, 5, "wall_clock_s"), "on": med(runs["phase7-auto"], medium_on, 5, "wall_clock_s")}
    faster_medium = min(medium_medians, key=medium_medians.get)  # type: ignore[arg-type]
    gates.ratio("phase7_medium_auto_wall", med(runs["phase7-auto"], medium_auto, 5, "wall_clock_s"), min(medium_medians.values()), Decimal("1.10"))
    if max(medium_medians.values()) / min(medium_medians.values()) > Decimal("1.10"):
        for row in runs["phase7-auto"].selected(medium_auto, 5):
            if top_report_value(Path(row["report_path"]), "lazy_policy_resolved") != faster_medium:
                raise AcceptanceError("phase7 medium auto did not select the faster forced representation")
    medium_off8, medium_on8, medium_auto8 = MEDIUM_DENSE.format(8), MEDIUM_LAZY.format(8), "phase7-lazy-completion-medium-auto-w8"
    medium8 = {
        "off": med(runs["phase7-auto"], medium_off8, 5, "wall_clock_s"),
        "on": med(runs["phase7-auto"], medium_on8, 5, "wall_clock_s"),
    }
    gates.ratio("phase7_medium_auto_wall_w8", med(runs["phase7-auto"], medium_auto8, 5, "wall_clock_s"), min(medium8.values()), Decimal("1.10"))
    if max(medium8.values()) / min(medium8.values()) > Decimal("1.10"):
        faster8 = min(medium8, key=medium8.get)  # type: ignore[arg-type]
        for row in runs["phase7-auto"].selected(medium_auto8, 5):
            if top_report_value(Path(row["report_path"]), "lazy_policy_resolved") != faster8:
                raise AcceptanceError("phase7 medium W8 auto did not select the faster forced representation")
    semantic_pair(
        runs["phase7-auto"],
        (medium_off, medium_on, medium_auto, medium_off8, medium_on8, medium_auto8),
        5,
        "phase7 medium forced/auto",
    )
    gates.condition("phase7_named_auto_branches", {high_branch, dense_branch} == {"on", "off"}, "named fixtures did not exercise both lazy and dense automatic branches")
    for evidence, one, eight, name in (
        (runs["phase7-high"], f"{high_prefix}-on-w1", f"{high_prefix}-on-w8", "phase7_high"),
        (runs["phase7-small"], small_on1, small_on8, "phase7_small"),
    ):
        rss_pair(gates, name, evidence, one, eight, 5, global_rss_cap_kb)

    # Phase 8 attempt 0 is immutable required failure evidence.  Retry 1 is
    # the only observation allowed to discharge the speed/non-regression/RSS
    # decision; preserving attempt 0 must never turn it into a passing run.
    p8_w1, p8_w8 = "phase8-generation-tree0-off-w1", "phase8-generation-tree0-off-w8"
    semantic_pair(runs["phase8-generation"], (p8_w1, p8_w8), 5, "phase8")
    semantic_pair(
        runs["phase8-generation-retry1"],
        (p8_w1, p8_w8),
        5,
        "phase8 retry1",
    )
    attempt0_semantics = {
        (row["search_semantic_sha256"], row["output_semantic_sha256"])
        for row in runs["phase8-generation"].rows
    }
    retry1_semantics = {
        (row["search_semantic_sha256"], row["output_semantic_sha256"])
        for row in runs["phase8-generation-retry1"].rows
    }
    prior_semantics = {
        (row["search_semantic_sha256"], row["output_semantic_sha256"])
        for row in runs["phase8-end-to-end"].rows
    }
    if attempt0_semantics != retry1_semantics or retry1_semantics != prior_semantics:
        raise AcceptanceError(
            "Phase-8 attempt0/retry1 and same-workload Phase-7 semantics differ"
        )
    attempt0_w1_generation = med(
        runs["phase8-generation"], p8_w1, 5, "candidate_generation_ms"
    )
    attempt0_w8_generation = med(
        runs["phase8-generation"], p8_w8, 5, "candidate_generation_ms"
    )
    if attempt0_w1_generation <= 0:
        raise AcceptanceError(
            "phase8 attempt0 generation-speed denominator is not positive"
        )
    attempt0_ratio = attempt0_w8_generation / attempt0_w1_generation
    if attempt0_ratio <= Decimal("0.50"):
        raise AcceptanceError(
            "immutable Phase-8 attempt 0 is not the retained failed observation: "
            f"generation ratio {attempt0_ratio} is at or below 0.50"
        )
    retry1_w1_generation = med(
        runs["phase8-generation-retry1"],
        p8_w1,
        5,
        "candidate_generation_ms",
    )
    retry1_w8_generation = med(
        runs["phase8-generation-retry1"],
        p8_w8,
        5,
        "candidate_generation_ms",
    )
    if retry1_w1_generation <= 0:
        raise AcceptanceError(
            "phase8 retry1 generation-speed denominator is not positive"
        )
    retry1_ratio = retry1_w8_generation / retry1_w1_generation
    gates.ratio(
        "phase8_generation_retry1_w8_over_w1",
        retry1_w8_generation,
        retry1_w1_generation,
        Decimal("0.50"),
    )
    for worker in (1, 8):
        rid = f"phase8-generation-tree0-off-w{worker}"
        gates.ratio(
            f"phase8_retry1_end_to_end_w{worker}_vs_phase7",
            med(
                runs["phase8-generation-retry1"], rid, 5, "wall_clock_s"
            ),
            med(runs["phase8-end-to-end"], rid, 5, "wall_clock_s"),
            Decimal("1.00"),
        )
    attempt0_w1_rss = rss_max(
        runs["phase8-generation"], p8_w1, 5
    )
    attempt0_w8_rss = rss_max(
        runs["phase8-generation"], p8_w8, 5
    )
    rss_pair(
        gates,
        "phase8_retry1",
        runs["phase8-generation-retry1"],
        p8_w1,
        p8_w8,
        5,
        global_rss_cap_kb,
    )
    phase8_generation_attempts: dict[str, dict[str, object]] = {}
    for (
        label,
        disposition,
        w1_generation,
        w8_generation,
        ratio,
        status,
    ) in (
        (
            "phase8-generation",
            "retained_failed_observation",
            attempt0_w1_generation,
            attempt0_w8_generation,
            attempt0_ratio,
            "fail",
        ),
        (
            "phase8-generation-retry1",
            "accepted_retry",
            retry1_w1_generation,
            retry1_w8_generation,
            retry1_ratio,
            "pass",
        ),
    ):
        provenance = phase8_provenance[label]
        phase8_generation_attempts[label] = {
            "disposition": disposition,
            "candidate_generation_w1_median_ms": str(w1_generation),
            "candidate_generation_w8_median_ms": str(w8_generation),
            "measured_ratio": str(ratio),
            "limit": "0.50",
            "status": status,
            **provenance.output(),
            "product_repo_root": os.fspath(provenance.product_repo_root),
            "capture_tool_repo_root": os.fspath(
                provenance.capture_tool_repo_root
            ),
            "capture_audit_status": phase8_audit_results[label]["status"],
            "historical_health_status": (
                "observed" if label == "phase8-generation" else "not_applicable"
            ),
        }
    phase8_generation_attempts["phase8-generation"].update(
        {
            "historical_w1_peak_rss_kb": attempt0_w1_rss,
            "historical_w8_peak_rss_kb": attempt0_w8_rss,
            "historical_rss_status": "observed_not_gated",
        }
    )

    # Mandatory same-affinity physical W1/2/4/8 matrix.  This is deliberately
    # distinct from the five-repetition paired native-parity run below.
    scaling = runs["final-scaling"]
    scaling_by_method: dict[str, tuple[str, ...]] = {}
    for method in (METHOD_SAMPLED, METHOD_EXACT, METHOD_HYBRID):
        ids = tuple(
            unique_method_id(scaling, method, str(worker), 3)
            for worker in (1, 2, 4, 8)
        )
        scaling_by_method[method] = ids
        semantic_pair(
            scaling,
            ids,
            3,
            f"final physical scaling {method}",
        )
    scaling_grammar1 = scaling_by_method[METHOD_EXACT][0]
    scaling_grammar8 = scaling_by_method[METHOD_EXACT][3]
    gates.ratio(
        "final_scaling_grammar_w8_over_w1",
        med(scaling, scaling_grammar8, 3, "wall_clock_s"),
        med(scaling, scaling_grammar1, 3, "wall_clock_s"),
        Decimal("0.50"),
    )
    for field in (
        "initial_chart_construction_ms",
        "local_scoring_ms",
        "exact_verification_ms",
    ):
        phase_one = med(scaling, scaling_grammar1, 3, field)
        wall_ms = med(scaling, scaling_grammar1, 3, "wall_clock_s") * 1000
        if phase_one >= wall_ms * Decimal("0.10"):
            gates.ratio(
                f"final_scaling_{field}_w8_over_w1",
                med(scaling, scaling_grammar8, 3, field),
                phase_one,
                Decimal(2) / 3,
            )
        else:
            gates.items.append(
                {
                    "name": f"final_scaling_{field}_w8_over_w1",
                    "status": "pass",
                    "comparison": "profile_exempt_below_10_percent",
                }
            )
    rss_pair(
        gates,
        "final_scaling_grammar",
        scaling,
        scaling_grammar1,
        scaling_grammar8,
        3,
        global_rss_cap_kb,
    )

    # Final primary parity and its existing same-revision scaling gates.
    primary = runs["final-primary"]
    native_id = unique_method_id(primary, METHOD_NATIVE, "native", 5)
    grammar1 = unique_method_id(primary, METHOD_EXACT, "1", 5)
    grammar8 = unique_method_id(primary, METHOD_EXACT, "8", 5)
    native_trials = {row["trial_index"]: row for row in primary.selected(native_id, 5)}
    grammar_trials = {row["trial_index"]: row for row in primary.selected(grammar8, 5)}
    pair_ratios = [decimal(grammar_trials[index]["wall_clock_s"], "chart wall") / decimal(native_trials[index]["wall_clock_s"], "native wall", positive=True) for index in sorted(native_trials)]
    gates.ratio("final_primary_grammar_native_median", med(primary, grammar8, 5, "wall_clock_s"), med(primary, native_id, 5, "wall_clock_s"), Decimal("1.00"))
    gates.condition("final_primary_paired_majority", sum(ratio <= 1 for ratio in pair_ratios) >= 3, "fewer than 3/5 chart trials beat their native peer")
    gates.condition("final_primary_paired_outlier", max(pair_ratios) <= Decimal("1.15"), "a paired chart/native ratio exceeds 1.15")
    gates.ratio("final_primary_w8_over_w1", med(primary, grammar8, 5, "wall_clock_s"), med(primary, grammar1, 5, "wall_clock_s"), Decimal("0.50"))
    semantic_pair(primary, (grammar1, grammar8), 5, "final primary grammar")
    for field in ("initial_chart_construction_ms", "local_scoring_ms", "exact_verification_ms"):
        phase_one = med(primary, grammar1, 5, field)
        wall_ms = med(primary, grammar1, 5, "wall_clock_s") * 1000
        if phase_one >= wall_ms * Decimal("0.10"):
            gates.ratio(f"final_primary_{field}_w8_over_w1", med(primary, grammar8, 5, field), phase_one, Decimal(2) / 3)
        else:
            gates.items.append({"name": f"final_primary_{field}_w8_over_w1", "status": "pass", "comparison": "profile_exempt_below_10_percent"})
    rss_pair(gates, "final_primary", primary, grammar1, grammar8, 5, global_rss_cap_kb)
    for method in (METHOD_SAMPLED, METHOD_HYBRID):
        current = unique_method_id(primary, method, "8", 5)
        baseline_compare(gates, f"final_{method}_phase0_nonregression", phase0, primary, base, current, 5, "wall_clock_s", Decimal("1.00"))

    # Same-affinity automatic policy, small guard, unpinned parity, and default.
    smt = runs["final-smt"]
    smt_auto = unique_method_id(smt, METHOD_EXACT, "auto", 3)
    explicit = [unique_method_id(smt, METHOD_EXACT, str(worker), 3) for worker in (1, 2, 4, 8, 16)]
    best = min(med(smt, rid, 3, "wall_clock_s") for rid in explicit)
    gates.ratio("final_smt_auto_vs_best_explicit", med(smt, smt_auto, 3, "wall_clock_s"), best, Decimal("1.10"))
    semantic_pair(smt, (*explicit, smt_auto), 3, "final SMT automatic policy")
    small = runs["final-small-auto"]
    small1 = unique_method_id(small, METHOD_LB, "1", 5)
    small_auto = unique_method_id(small, METHOD_LB, "auto", 5)
    gates.ratio("final_small_auto_over_w1", med(small, small_auto, 5, "wall_clock_s"), med(small, small1, 5, "wall_clock_s"), Decimal("1.10"))
    unpinned = runs["final-unpinned-auto"]
    unpinned_native = unique_method_id(unpinned, METHOD_NATIVE, "native", 5)
    unpinned_auto = unique_method_id(unpinned, METHOD_EXACT, "auto", 5)
    gates.ratio("final_unpinned_auto_native_parity", med(unpinned, unpinned_auto, 5, "wall_clock_s"), med(unpinned, unpinned_native, 5, "wall_clock_s"), Decimal("1.00"))
    if evaluation_mode == "final":
        default = runs["final-default-auto"]
        default_native = unique_method_id(default, METHOD_NATIVE, "native", 5)
        default_id = unique_method_id(default, METHOD_EXACT, "default", 5)
        default_auto = unique_method_id(default, METHOD_EXACT, "auto", 5)
        d_trials = {
            row["trial_index"]: row for row in default.selected(default_id, 5)
        }
        a_trials = {
            row["trial_index"]: row for row in default.selected(default_auto, 5)
        }
        for index in sorted(d_trials):
            if (
                d_trials[index]["worker_policy"] != "automatic_default"
                or a_trials[index]["worker_policy"] != "automatic"
            ):
                raise AcceptanceError(
                    "final default/auto worker-policy labels are not exact"
                )
            if d_trials[index]["resolved_workers"] != a_trials[index]["resolved_workers"]:
                raise AcceptanceError(
                    "final default/auto resolved worker counts differ"
                )
            if decimal(
                d_trials[index]["wall_clock_s"], "default wall"
            ) > Decimal("1.10") * decimal(
                a_trials[index]["wall_clock_s"], "auto wall"
            ):
                raise AcceptanceError(
                    "final default is more than 10% slower than auto in a paired trial"
                )
        semantic_pair(default, (default_id, default_auto), 5, "final default/auto")
        gates.ratio(
            "final_default_over_auto_median",
            med(default, default_id, 5, "wall_clock_s"),
            med(default, default_auto, 5, "wall_clock_s"),
            Decimal("1.10"),
        )
        gates.ratio(
            "final_default_native_parity",
            med(default, default_id, 5, "wall_clock_s"),
            med(default, default_native, 5, "wall_clock_s"),
            Decimal("1.00"),
        )

    # Stress.  Phase 9 is accepted only by the delegated deep evaluator.
    stress = runs["final-stress"]
    for row_id in sorted(stress.row_ids()):
        row = stress.selected(row_id, 3)[0]
        if row["method"] == METHOD_NATIVE or row["requested_workers"] != "1":
            continue
        peer = next((rid for rid in stress.row_ids() if stress.selected(rid, 3)[0]["method"] == row["method"] and stress.selected(rid, 3)[0]["fixture"] == row["fixture"] and stress.selected(rid, 3)[0]["requested_workers"] == "8"), None)
        if peer is None:
            raise AcceptanceError(f"stress row {row_id} lacks its W8 peer")
        threshold = Decimal("1.00") if med(stress, row_id, 3, "total_ms") >= 100 else Decimal("1.10")
        gates.ratio(f"final_stress_{row_id}_w8_over_w1", med(stress, peer, 3, "wall_clock_s"), med(stress, row_id, 3, "wall_clock_s"), threshold)
        rss_pair(gates, f"final_stress_{row_id}", stress, row_id, peer, 3, global_rss_cap_kb)

    expected_refusal_trials = len(final_real) * 3
    gates.condition(
        "final_real_bounded_contract",
        validated_required["final-real"] == expected_refusal_trials,
        "validated refusal trial count differs from exact required matrix: "
        f"{validated_required['final-real']}/{expected_refusal_trials}",
    )

    phase0.unchanged()
    phase0_artifact_ledger.unchanged()
    for evidence in runs.values():
        evidence.unchanged()
    for provenance in phase8_provenance.values():
        provenance.unchanged()
    base.base.unchanged()
    for supplement in base.supplements.values():
        supplement.unchanged()
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": "pass",
        "evaluation_mode": evaluation_mode,
        "completion_eligible": evaluation_mode == "final",
        "deferred_run_labels": (
            [] if evaluation_mode == "final" else ["final-default-auto"]
        ),
        "base_manifest": os.fspath(base.path),
        "base_manifest_sha256": base.sha256,
        "phase0_artifact_ledger": os.fspath(phase0_artifact_ledger.path),
        "phase0_artifact_ledger_sha256": phase0_artifact_ledger.sha256,
        "base_repo_root": os.fspath(base_repo_root),
        "working_repo_root": os.fspath(working_repo_root),
        "supplement_manifests": {
            label: {
                "path": os.fspath(base.supplements[label].path),
                "sha256": base.supplements[label].sha256,
            }
            for label in SUPPLEMENT_SPECS
        },
        "run_labels": list(scoped_run_labels),
        "all_run_labels": list(RUN_LABELS),
        "global_rss_cap_kb": global_rss_cap_kb,
        "current_product_retries": {
            "required_product_revision": PHASE6_ACCEPTANCE_PRODUCT_REVISION,
            "run_labels": list(CURRENT_PRODUCT_RETRY_LABELS),
        },
        "phase6_acceptance_sources": {
            "historical_run_label": PHASE6_HISTORICAL_RUN_LABEL,
            "historical_disposition": "diagnostic_not_an_acceptance_input",
            "required_product_revision": PHASE6_ACCEPTANCE_PRODUCT_REVISION,
            "top_k": {
                str(top_k): PHASE6_ACCEPTANCE_SOURCES[top_k]
                for top_k in sorted(PHASE6_ACCEPTANCE_SOURCES)
            },
        },
        "gates": gates.items,
        "phase8_generation_attempts": phase8_generation_attempts,
        "phase8_capture_audits": {
            label: dict(phase8_audit_results[label])
            for label in PHASE8_CAPTURE_LABELS
        },
        "phase4_acceptance": dict(phase4_result),
        "phase9_acceptance": dict(phase9_result),
    }


def unique_method_id(evidence: RawEvidence, method: str, worker: str, reps: int) -> str:
    ids = {row["row_id"] for row in evidence.rows if row["method"] == method and row["requested_workers"] == worker}
    if len(ids) != 1:
        raise AcceptanceError(f"{evidence.label}: expected one {method}@{worker} row, found {sorted(ids)}")
    row_id = next(iter(ids))
    evidence.selected(row_id, reps)
    return row_id


def physical_memory_bytes() -> int:
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            match = re.fullmatch(r"MemTotal:\s+([1-9][0-9]*) kB", line)
            if match:
                return int(match.group(1)) * 1024
    except OSError as error:
        raise AcceptanceError(f"cannot read physical memory: {error}") from error
    raise AcceptanceError("cannot determine physical memory")


def run_labels_for_mode(evaluation_mode: str) -> tuple[str, ...]:
    if evaluation_mode not in EVALUATION_MODES:
        raise AcceptanceError(f"unsupported evaluation mode {evaluation_mode!r}")
    return tuple(
        label
        for label in RUN_LABELS
        if evaluation_mode == "final" or label != "final-default-auto"
    )


def run_assignment(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--run must be LABEL=raw.tsv")
    label, path = value.split("=", 1)
    if label not in RUN_LABELS or not path:
        raise argparse.ArgumentTypeError(f"unknown/empty --run assignment {value!r}")
    return label, Path(path)


def supplement_assignment(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--supplement must be LABEL=manifest.tsv")
    label, path = value.split("=", 1)
    if label not in SUPPLEMENT_SPECS or not path:
        raise argparse.ArgumentTypeError(f"unknown/empty --supplement assignment {value!r}")
    return label, Path(path)


def supplement_hash_assignment(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            "--expected-supplement-sha256 must be LABEL=HASH"
        )
    label, digest = value.split("=", 1)
    if label not in SUPPLEMENT_SPECS or not digest:
        raise argparse.ArgumentTypeError(
            f"unknown/empty --expected-supplement-sha256 assignment {value!r}"
        )
    return label, digest


def raw_hash_assignment(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--expected-raw-sha256 must be LABEL=HASH")
    label, digest = value.split("=", 1)
    if label not in ("phase0", *RUN_LABELS) or not digest:
        raise argparse.ArgumentTypeError(
            f"unknown/empty --expected-raw-sha256 assignment {value!r}"
        )
    return label, digest


def phase8_path_assignment(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            "Phase-8 capture assignment must be LABEL=PATH"
        )
    label, path = value.split("=", 1)
    if label not in PHASE8_CAPTURE_LABELS or not path:
        raise argparse.ArgumentTypeError(
            f"unknown/empty Phase-8 capture assignment {value!r}"
        )
    return label, Path(path)


def phase8_value_assignment(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            "Phase-8 provenance assignment must be LABEL=VALUE"
        )
    label, assigned = value.split("=", 1)
    if label not in PHASE8_CAPTURE_LABELS or not assigned:
        raise argparse.ArgumentTypeError(
            f"unknown/empty Phase-8 provenance assignment {value!r}"
        )
    return label, assigned


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    evaluate_parser = commands.add_parser("evaluate", help="perform every cross-phase gate read-only")
    evaluate_parser.add_argument(
        "--evaluation-mode", choices=EVALUATION_MODES, required=True
    )
    evaluate_parser.add_argument("--base-manifest", type=Path, required=True)
    evaluate_parser.add_argument("--expected-base-sha256", required=True)
    evaluate_parser.add_argument("--base-repo-root", type=Path, required=True)
    evaluate_parser.add_argument("--working-repo-root", type=Path, required=True)
    evaluate_parser.add_argument(
        "--phase0-artifact-ledger", type=Path, required=True
    )
    evaluate_parser.add_argument(
        "--expected-phase0-artifact-ledger-sha256", required=True
    )
    evaluate_parser.add_argument(
        "--supplement", type=supplement_assignment, action="append",
        required=True, metavar="LABEL=manifest.tsv",
    )
    evaluate_parser.add_argument(
        "--expected-supplement-sha256", type=supplement_hash_assignment,
        action="append", required=True, metavar="LABEL=HASH",
    )
    evaluate_parser.add_argument("--phase0-raw", type=Path, action="append", required=True)
    evaluate_parser.add_argument("--run", type=run_assignment, action="append", required=True, metavar="LABEL=raw.tsv")
    evaluate_parser.add_argument(
        "--expected-raw-sha256", type=raw_hash_assignment, action="append",
        required=True, metavar="LABEL=HASH",
    )
    evaluate_parser.add_argument(
        "--expected-phase9-run-ledger-sha256", required=True
    )
    evaluate_parser.add_argument(
        "--phase8-capture-dir", type=phase8_path_assignment,
        action="append", required=True, metavar="LABEL=DIR",
    )
    evaluate_parser.add_argument(
        "--phase8-product-repo-root", type=phase8_path_assignment,
        action="append", required=True, metavar="LABEL=ROOT",
    )
    evaluate_parser.add_argument(
        "--phase8-capture-tool-repo-root", type=phase8_path_assignment,
        action="append", required=True, metavar="LABEL=ROOT",
    )
    evaluate_parser.add_argument(
        "--expected-phase8-run-ledger-sha256", type=phase8_value_assignment,
        action="append", required=True, metavar="LABEL=HASH",
    )
    evaluate_parser.add_argument(
        "--expected-phase8-product-revision", type=phase8_value_assignment,
        action="append", required=True, metavar="LABEL=REVISION",
    )
    evaluate_parser.add_argument(
        "--expected-phase8-capture-tool-revision",
        type=phase8_value_assignment, action="append", required=True,
        metavar="LABEL=REVISION",
    )
    evaluate_parser.add_argument(
        "--expected-phase8-capture-wrapper-sha256",
        type=phase8_value_assignment, action="append", required=True,
        metavar="LABEL=HASH",
    )
    return result


def main(
    argv: Sequence[str] | None = None,
    *,
    phase8_capture_auditor: Phase8CaptureAuditor = deep_phase8_capture_audit,
    phase4_validator: Phase4Validator = deep_phase4_validate,
    phase9_validator: Phase9Validator = deep_phase9_validate,
) -> int:
    args = parser().parse_args(argv)
    try:
        assignments: dict[str, list[Path]] = {}
        raw_hashes: dict[str, list[str]] = {}
        supplement_paths: dict[str, Path] = {}
        supplement_hashes: dict[str, str] = {}
        all_paths: set[Path] = set()

        def exact_phase8_assignments(
            values: Sequence[tuple[str, object]], description: str
        ) -> dict[str, object]:
            result: dict[str, object] = {}
            for label, value in values:
                if label in result:
                    raise AcceptanceError(
                        f"duplicate {description} assignment for {label}"
                    )
                result[label] = value
            expected_labels = set(PHASE8_CAPTURE_LABELS)
            if set(result) != expected_labels:
                raise AcceptanceError(
                    f"{description} label set mismatch; "
                    f"missing={sorted(expected_labels - set(result))}, "
                    f"unexpected={sorted(set(result) - expected_labels)}"
                )
            return result

        phase8_capture_dirs = exact_phase8_assignments(
            args.phase8_capture_dir, "Phase-8 capture directory"
        )
        phase8_product_repo_roots = exact_phase8_assignments(
            args.phase8_product_repo_root, "Phase-8 product repository root"
        )
        phase8_capture_tool_repo_roots = exact_phase8_assignments(
            args.phase8_capture_tool_repo_root,
            "Phase-8 capture-tool repository root",
        )
        phase8_ledger_hashes = exact_phase8_assignments(
            args.expected_phase8_run_ledger_sha256,
            "Phase-8 run-ledger anchor",
        )
        phase8_product_revisions = exact_phase8_assignments(
            args.expected_phase8_product_revision,
            "Phase-8 product revision",
        )
        phase8_capture_tool_revisions = exact_phase8_assignments(
            args.expected_phase8_capture_tool_revision,
            "Phase-8 capture-tool revision",
        )
        phase8_wrapper_hashes = exact_phase8_assignments(
            args.expected_phase8_capture_wrapper_sha256,
            "Phase-8 capture-wrapper anchor",
        )
        for label, path in args.run:
            assignments.setdefault(label, []).append(path)
        for label, digest in args.expected_raw_sha256:
            raw_hashes.setdefault(label, []).append(digest)
        for label, path in args.supplement:
            if label in supplement_paths:
                raise AcceptanceError(f"duplicate supplement assignment for {label}")
            supplement_paths[label] = path
        for label, digest in args.expected_supplement_sha256:
            if label in supplement_hashes:
                raise AcceptanceError(f"duplicate supplement SHA-256 anchor for {label}")
            supplement_hashes[label] = digest
        labels = set(assignments)
        scoped_run_labels = run_labels_for_mode(args.evaluation_mode)
        expected = set(scoped_run_labels)
        if labels != expected:
            raise AcceptanceError(f"run-label set mismatch; missing={sorted(expected-labels)}, unexpected={sorted(labels-expected)}")
        expected_raw_labels = {"phase0", *scoped_run_labels}
        if set(raw_hashes) != expected_raw_labels:
            raise AcceptanceError(
                f"raw-anchor label set mismatch; missing={sorted(expected_raw_labels-set(raw_hashes))}, "
                f"unexpected={sorted(set(raw_hashes)-expected_raw_labels)}"
            )
        for label, paths in assignments.items():
            for path in paths:
                canonical = regular(path, f"{label} raw TSV")
                if canonical in all_paths:
                    raise AcceptanceError(f"raw TSV reused across run labels: {canonical}")
                all_paths.add(canonical)
        phase9_run_ledger_sha256 = verify_phase9_run_ledger_anchor(
            assignments["phase9"], args.expected_phase9_run_ledger_sha256
        )
        base_repo_root = canonical_directory(
            args.base_repo_root, "sealed base repository root"
        )
        working_repo_root = canonical_directory(
            args.working_repo_root, "working product repository root"
        )
        phase0_artifact_ledger = load_phase0_artifact_ledger(
            args.phase0_artifact_ledger,
            args.expected_phase0_artifact_ledger_sha256,
            base_repo_root,
        )
        verify_phase0_raw_arguments(
            args.phase0_raw,
            raw_hashes["phase0"],
            phase0_artifact_ledger,
        )
        base = load_manifest_chain(
            args.base_manifest,
            args.expected_base_sha256,
            supplement_paths,
            supplement_hashes,
        )
        phase0 = load_raw(
            "phase0",
            args.phase0_raw,
            raw_hashes["phase0"],
            phase0_inputs=phase0_artifact_ledger.raw_inputs,
        )
        if any(path in all_paths for path in phase0.paths):
            raise AcceptanceError("Phase-0 raw TSV is reused as a later run")
        for label in PHASE8_CAPTURE_LABELS:
            if len(assignments[label]) != 1 or len(raw_hashes[label]) != 1:
                raise AcceptanceError(
                    f"{label}: Phase-8 capture requires exactly one raw TSV "
                    "and one external raw anchor"
                )
        phase8_provenance = {
            label: load_phase8_capture_provenance(
                label,
                phase8_capture_dirs[label],  # type: ignore[arg-type]
                assignments[label][0],
                raw_hashes[label][0],
                phase8_ledger_hashes[label],  # type: ignore[arg-type]
                phase8_product_revisions[label],  # type: ignore[arg-type]
                phase8_capture_tool_revisions[label],  # type: ignore[arg-type]
                phase8_wrapper_hashes[label],  # type: ignore[arg-type]
                phase8_product_repo_roots[label],  # type: ignore[arg-type]
                phase8_capture_tool_repo_roots[label],  # type: ignore[arg-type]
                base,
                phase0_artifact_ledger,
                base_repo_root,
            )
            for label in PHASE8_CAPTURE_LABELS
        }
        runs: dict[str, RawEvidence] = {}
        for label, paths in assignments.items():
            provenance = phase8_provenance.get(label)
            runs[label] = load_raw(
                label,
                paths,
                raw_hashes[label],
                anchored_payloads=(
                    None if provenance is None else [provenance.raw_payload]
                ),
                anchored_signatures=(
                    None if provenance is None else [provenance.raw_signature]
                ),
            )
        attempt0 = phase8_provenance["phase8-generation"]
        retry1 = phase8_provenance["phase8-generation-retry1"]
        if os.path.samefile(
            attempt0.capture_directory, retry1.capture_directory
        ):
            raise AcceptanceError(
                "Phase-8 attempt 0 and retry1 reuse the same capture directory"
            )
        if os.path.samefile(attempt0.raw_path, retry1.raw_path):
            raise AcceptanceError(
                "Phase-8 attempt 0 and retry1 reuse the same raw TSV path"
            )
        if attempt0.raw_sha256 == retry1.raw_sha256:
            raise AcceptanceError(
                "Phase-8 attempt 0 and retry1 reuse the same raw TSV bytes"
            )
        if (
            attempt0.capture_tool_revision
            == retry1.capture_tool_revision
        ):
            raise AcceptanceError(
                "Phase-8 retry1 capture-tool revision C is not distinct"
            )
        if os.path.samefile(
            attempt0.product_repo_root, retry1.product_repo_root
        ):
            raise AcceptanceError(
                "Phase-8 attempt 0 and retry1 reuse the same product repository root"
            )
        if os.path.samefile(
            attempt0.capture_tool_repo_root, retry1.capture_tool_repo_root
        ):
            raise AcceptanceError(
                "Phase-8 attempt 0 and retry1 reuse the same capture-tool repository root"
            )
        result = evaluate(
            base,
            phase0,
            phase0_artifact_ledger,
            runs,
            phase8_provenance,
            evaluation_mode=args.evaluation_mode,
            base_repo_root=base_repo_root,
            working_repo_root=working_repo_root,
            phase9_run_ledger_sha256=phase9_run_ledger_sha256,
            phase8_capture_auditor=phase8_capture_auditor,
            phase4_validator=phase4_validator,
            phase9_validator=phase9_validator,
        )
        sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return 0
    except (AcceptanceError, OSError, UnicodeError, ValueError, csv.Error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
