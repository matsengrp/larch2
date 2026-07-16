#!/usr/bin/env python3
"""Non-destructive bootstrap for the frozen Phase-0 WRIC workload manifest.

The benchmark harness intentionally accepts only a sealed, fully populated
manifest.  This tool bridges that bootstrap cycle without weakening the
harness: it captures the immutable matrix in ordinary (unsealed) mode, derives
the path-independent manifest-mode argv/trial digests, writes a *pending*
sealed manifest, and asks the benchmark harness itself to audit it before an
optional explicit promotion.

Long medium captures are never started without --confirm-long-medium.  Every
created path is exclusive; the tool has no overwrite mode.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
from datetime import datetime, timezone
from decimal import Decimal, ROUND_HALF_EVEN
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys
import tempfile
import time
from typing import Iterable, Mapping, Sequence


FROZEN_REVISION = "408434ecfd096af484ecbbd3deeb67511151cd76"
MERGE_BASE = "fee366e71439d7b8c8eda588cc72cde8534f102f"
FROZEN_LARCH2_SHA256 = (
    "ee160aa4fdecce5660f1bf0737289fc32fd33b1965e660f0ec92072805a4de9b"
)
FROZEN_ORACLE_SHA256 = (
    "7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c"
)
BASELINE_RELATIVE = Path(
    "build/wric-chart-parallelization/baseline-408434e"
)
PHYSICAL_AFFINITY = "0,2,4,6,8,10,12,14"
SMT_AFFINITY = "0-15"
MEMORY_BUDGET_BYTES = 12 * 1024**3
RSS_LIMIT_BYTES = 16 * 1024**3
REAL_PREFLIGHT_BYTES = 6 * 1024**3
TIMEOUT_SECONDS = 600
WRAPPER_CALIBRATION_SCHEMA = "wric_process_wrapper_calibration"
WRAPPER_CALIBRATION_SCHEMA_VERSION = 3
WRAPPER_CALIBRATION_CONTROLLER_SCHEMA = "wric_wrapper_calibration_controller"
WRAPPER_CALIBRATION_CONTROLLER_VERSION = 3
WRAPPER_CALIBRATION_WORKLOAD_SCHEMA = "wric_wrapper_calibration_cpu_workload"
WRAPPER_CALIBRATION_WORKLOAD_VERSION = 1
WRAPPER_CALIBRATION_WORKLOAD_VERSION_TEXT = (
    "wric-wrapper-calibration-workload-v1"
)
WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256 = (
    "66244128374664bdc6868df078410608fbc5f9a0dd47c71bdc7f7534b81da8d2"
)
WRAPPER_CALIBRATION_WORKLOAD_STDOUT_TEXT = (
    "schema=wric-wrapper-calibration-workload-v1\n"
    "threads=8\n"
    "iterations_per_thread=2250000000\n"
    "affinity_cpu_count=8\n"
    "checksum=8745913267985538626\n"
)
WRAPPER_CALIBRATION_WORKLOAD_STDOUT_BYTES = 137
WRAPPER_CALIBRATION_WORKLOAD_STDOUT_SHA256 = (
    "6c20ef62a67ad417071deb005e0a49bc4e7e806a18dd9578a1bc8c4ac32480b6"
)
WRAPPER_CALIBRATION_THRESHOLD = Decimal("1.02")
WRAPPER_CALIBRATION_SATURATION_MIN = Decimal("7.2")
WRAPPER_CALIBRATION_SATURATION_MAX = Decimal("8.8")
WRAPPER_CALIBRATION_MIN_ARM_WALL_NS = 5_000_000_000
WRAPPER_CALIBRATION_MIN_ARM_CPU_US = 36_000_000
WRAPPER_CALIBRATION_WALL_EARLY_TOLERANCE_NS = 5_000_000
WRAPPER_CALIBRATION_WALL_MAX_DELTA_NS = 250_000_000
WRAPPER_CALIBRATION_CPU_EARLY_TOLERANCE_US = 10_000
WRAPPER_CALIBRATION_CPU_MAX_DELTA_US = 500_000
WRAPPER_CALIBRATION_RSS_MAX_DELTA_KB = 64 * 1024
WRAPPER_CALIBRATION_RATIO_PLACES = Decimal("0.000000000001")
WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM = 200_000
WRAPPER_CALIBRATION_LIVE_POLL_MS = 250
WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS = 1_000_000_000
WRAPPER_CALIBRATION_MIN_PREFLIGHT_NS = 5_000_000_000
WRAPPER_CALIBRATION_ENVIRONMENT = {
    "LC_ALL": "C",
    "PATH": "/usr/bin:/bin",
    "TZ": "Europe/Sofia",
}
WRAPPER_CALIBRATION_ARGV = ["@workload"]
WRAPPER_CALIBRATION_REDIRECTIONS = {
    "stderr": "@stderr",
    "stdout": "@stdout",
}
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
MANIFEST_SENTINEL_GROUP = "__wric_phase0_manifest_audit_missing_group__"
BOOTSTRAP_VERSION = 3
SMALL_PRIMARY_SHA256 = "e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6"
MEDIUM_PRIMARY_SHA256 = "2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153"
MEDIUM_REFSEQ_SHA256 = "088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c"
REAL20D_PRIMARY_SHA256 = "a65f300916f158ea4c8de8bc49f5a94379905a4c3b7fba337ce6773d65ecfce4"
REAL20D_REFSEQ_SHA256 = "82c11885688b9a67b72ec4d2dd5913571a1723039ca501dc63bafb2cb5ea13eb"
REAL20D_INITIAL_SCORE = "11155"
REAL20D_INITIAL_SEMANTIC_SHA256 = (
    "b4e8c4ea029f2222a789026ebc56f096e37bebeb3fef9474ed333aa4c157ad2f"
)
REAL20D_REFUSAL_SHA256 = (
    "84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35"
)
REAL20D_REFUSAL_TEXT = (
    "error: polytomy refinement: bounded expansion requires arity <= 63\n"
)
REAL20D_STDOUT_TEXT = "leaves: 3832\nnodes: 5436\nedges: 5435\n"
REAL20D_STDOUT_SHA256 = (
    "b0174b43d6ddf95ec64e301a7820b4fb19c74c38d9da0873a76787715cfeab36"
)
REAL20D_STDOUT_SIZE = 37
REAL20D_STDERR_SIZE = 159
REAL20D_REFUSAL_INTRO_REVISION = "c171278faab65839415bf4b447ab8a5684c6e36d"
REAL20D_CANONICAL_ARGV_SHA256 = {
    "1": "518726d8e4df68ffa926b998e7f4b6853eccc0769ae0d88427137e1a535c3dc2",
    "8": "82e697d208998f4a619e7135d004f245806f725c18644e2828fd885dd31decb6",
}


class BootstrapError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class CaptureSpec:
    capture_id: str
    affinity_kind: str
    affinity_cpus: str
    fixture: str
    iterations: int
    native_max_moves: int
    chart_max_candidates: int
    chart_top_k_exact: int
    modes: tuple[str, ...]
    workers: tuple[str, ...]
    lazy_policy: str = "off"
    memory_budget_bytes: int = MEMORY_BUDGET_BYTES
    rss_limit_bytes: int = RSS_LIMIT_BYTES
    capture_role: str = "standard"

    @property
    def long_medium(self) -> bool:
        return self.fixture == "medium"

    @property
    def long_real(self) -> bool:
        return self.fixture == "real20d"


@dataclasses.dataclass(frozen=True)
class RowSpec:
    row_id: str
    run_group: str
    workload_name: str
    capture_id: str
    method: str
    requested_workers: str
    timeout_policy: str = "forbid"
    real_role: str = "standard"
    measured_trial_target: int = 3


@dataclasses.dataclass(frozen=True)
class StructuralEvidence:
    refinement_exactness: str
    cache_strategy: str
    effective_pattern_batch_size: str
    source_row_id: str
    source_report: Path
    source_report_sha256: str
    evidence_key_sha256: str


MODE_FIELDS: dict[str, tuple[str, str, str, str, str]] = {
    "sampled_tree_fixed": (
        "chart_spr_sampled_tree_fixed_topology",
        "fixed_topology_exact",
        "fixed_topology_exact",
        "sampled_tree",
        "first_reachable_overlay_topology",
    ),
    "grammar_exact": (
        "chart_spr_grammar_exact",
        "exact_multisite",
        "grammar_exact",
        "grammar",
        "none",
    ),
    "hybrid_exact": (
        "chart_spr_hybrid_exact",
        "exact_multisite",
        "grammar_exact",
        "hybrid",
        "none",
    ),
    "grammar_lower_bound": (
        "chart_spr_grammar_lower_bound_heuristic",
        "lower_bound_heuristic",
        "composite_lower_bound_heuristic",
        "grammar",
        "none",
    ),
}


RESOLUTION_FIELDS = (
    "method requested_workers worker_option expected_worker_policy binary_role input_kind primary_sha256 "
    "refseq_sha256 affinity_cpus iterations seed native_max_moves "
    "chart_max_candidates chart_top_k_exact timeout_seconds "
    "candidate_cap_semantics acceptance objective candidate_selection "
    "candidate_source topology_selector randomize_order reservoir_sample "
    "include_immediate_reversals sampled_tree_count sampled_tree_radius "
    "sampled_tree_score_threshold max_upward_path_expansions max_path_pairs "
    "min_moved_clade_size max_moved_clade_size min_target_clade_size "
    "max_target_clade_size max_affected_clades polytomy_mode "
    "polytomy_max_exact_arity polytomy_max_shapes polytomy_max_productions "
    "polytomy_max_clades lazy_policy max_cached_patterns pattern_batch_size "
    "candidate_batch_size memory_budget_bytes commit_mode verification_mode "
    "local_accept_updates dominance_mode bound_pruning require_exact_keep_mask "
    "max_frontier_entries score_ua_edge validate force_no_vcf"
).split()

# These are the exact defining fields that manifest-mode execution asks the
# product report to repeat.  Keeping the map here makes bootstrap capture fail
# immediately if an instrumented oracle omits a line; waiting until a later
# acceptance run would make an expensive medium capture unusable.
DEFINING_REPORT_FIELDS = {
    "acceptance": "acceptance",
    "objective": "objective",
    "candidate_selection": "candidate_selection",
    "candidate_source": "candidate_source",
    "candidate_cap_semantics": "candidate_cap_semantics",
    "topology_selector": "topology_selector",
    "iterations": "requested_max_iterations",
    "seed": "seed",
    "chart_max_candidates": "configured_max_candidates",
    "chart_top_k_exact": "top_k_exact_verify",
    "randomize_order": "randomize_order",
    "reservoir_sample": "reservoir_sample",
    "include_immediate_reversals": "include_immediate_reversals",
    "sampled_tree_count": "sampled_tree_count",
    "sampled_tree_radius": "sampled_tree_radius",
    "sampled_tree_score_threshold": "sampled_tree_score_threshold",
    "max_upward_path_expansions": "max_upward_path_expansions",
    "max_path_pairs": "max_path_pairs",
    "min_moved_clade_size": "min_moved_clade_size",
    "max_moved_clade_size": "max_moved_clade_size",
    "min_target_clade_size": "min_target_clade_size",
    "max_target_clade_size": "max_target_clade_size",
    "max_affected_clades": "max_affected_clades",
    "polytomy_mode": "polytomy_mode",
    "polytomy_max_exact_arity": "polytomy_max_exact_arity",
    "polytomy_max_shapes": "polytomy_max_shapes",
    "polytomy_max_productions": "polytomy_max_productions",
    "polytomy_max_clades": "polytomy_max_clades",
    "lazy_policy": "lazy_policy",
    "max_cached_patterns": "max_cached_patterns",
    "pattern_batch_size": "configured_pattern_batch_size",
    "candidate_batch_size": "configured_candidate_batch_size",
    "memory_budget_bytes": "memory_budget_bytes",
    "commit_mode": "commit_mode",
    "verification_mode": "verification_mode",
    "local_accept_updates": "local_accept_updates",
    "dominance_mode": "dominance_mode",
    "bound_pruning": "bound_pruning",
    "require_exact_keep_mask": "require_exact_keep_mask",
    "max_frontier_entries": "max_frontier_entries",
    "score_ua_edge": "score_ua_edge",
    "validate": "validate",
    "force_no_vcf": "force_no_vcf",
}


EXACT_CANDIDATE_ADMISSION_FIELDS = (
    "exact_candidate_admission_batches",
    "exact_candidate_parallel_batches",
    "exact_candidate_inner_parallel_batches",
    "exact_candidate_memory_limited_batches",
    "exact_candidate_peak_admitted_bytes",
    "exact_candidate_peak_projected_resident_bytes",
    "exact_candidate_queued_for_memory_ms",
)


# These immutable inputs determine the three chart-report fields that the
# strict manifest schema requires even when the timed row itself expires
# before it can write a report.  Method, candidate shape, worker count, and
# affinity are intentionally absent: they do not determine refinement or
# cache construction.  All successful reports sharing a key must nevertheless
# agree, so the bootstrap fails instead of silently relying on that premise.
STRUCTURAL_EVIDENCE_FIELDS = (
    "input_kind",
    "primary_sha256",
    "refseq_sha256",
    "polytomy_mode",
    "polytomy_max_exact_arity",
    "polytomy_max_shapes",
    "polytomy_max_productions",
    "polytomy_max_clades",
    "lazy_policy",
    "max_cached_patterns",
    "pattern_batch_size",
    "memory_budget_bytes",
    "score_ua_edge",
    "force_no_vcf",
)


def fail(message: str) -> "NoReturn":
    raise BootstrapError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def path_occupied(path: Path) -> bool:
    """Treat dangling symlinks as occupied for every exclusive destination."""

    return os.path.lexists(path)


def require_absent_output(path: Path, context: str) -> None:
    """Require lexical absence; a dangling output symlink is still an artifact."""

    if path_occupied(path):
        fail(f"{context} created an output artifact: {path}")


def require_exact_regular_file(
    path: Path, label: str, *, executable: bool = False, immutable: bool = False
) -> None:
    """Reject aliases and writable frozen executables before trusting bytes."""

    try:
        info = path.lstat()
    except FileNotFoundError:
        fail(f"{label} is missing: {path}")
    if not stat.S_ISREG(info.st_mode) or path.is_symlink():
        fail(f"{label} must be a lexical regular file, not an alias: {path}")
    if executable and not (info.st_mode & 0o111):
        fail(f"{label} is not executable: {path}")
    if immutable and info.st_mode & 0o222:
        fail(f"{label} must not have writable mode bits: {path}")


def write_bytes_exclusive(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, mode)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        raise


def write_text_exclusive(path: Path, text: str, mode: int = 0o644) -> None:
    write_bytes_exclusive(path, text.encode("utf-8"), mode)


def normalized_repo_path(repo_root: Path, path: Path) -> str:
    root = repo_root.resolve(strict=True)
    resolved = path.resolve(strict=True)
    try:
        relative = resolved.relative_to(root)
    except ValueError:
        fail(f"path is outside the repository and cannot be manifested: {path}")
    if any(part in ("", ".", "..") for part in relative.parts):
        fail(f"path is not normalized: {path}")
    return relative.as_posix()


def fixed_repo_root(baseline_dir: Path) -> Path:
    """Derive the repository without consulting mutable bootstrap metadata."""

    baseline = baseline_dir.absolute()
    relative_parts = BASELINE_RELATIVE.parts
    if baseline.parts[-len(relative_parts) :] != relative_parts:
        fail(
            "Phase-0 bootstrap artifacts must use the fixed lexical baseline "
            f"path {BASELINE_RELATIVE.as_posix()}: {baseline}"
        )
    repo_root = baseline.parents[len(relative_parts) - 1]
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel", "HEAD"],
        cwd=repo_root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    values = result.stdout.splitlines()
    if (
        result.returncode != 0
        or len(values) != 2
        or Path(values[0]).absolute() != repo_root
    ):
        fail(
            "cannot prove the fixed Phase-0 repository root independently of "
            f"metadata: {repo_root}: {result.stderr.strip()}"
        )
    if values[1] != FROZEN_REVISION:
        fail(
            f"Phase-0 repository revision changed: expected {FROZEN_REVISION}, "
            f"observed {values[1]}"
        )
    if baseline != (repo_root / BASELINE_RELATIVE):
        fail("fixed Phase-0 baseline path is not lexically canonical")
    return repo_root


def observed_affinity() -> str:
    result = subprocess.run(
        ["taskset", "-pc", str(os.getpid())],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    value = result.stdout.rsplit(":", 1)[-1].strip()
    if not re.fullmatch(r"[0-9]+(?:[,-][0-9]+)*", value):
        fail(f"taskset returned a noncanonical CPU list: {value!r}")
    return value


def validate_affinity(value: str, label: str) -> None:
    if not re.fullmatch(r"[0-9]+(?:[,-][0-9]+)*", value):
        fail(f"{label} affinity is not a canonical taskset CPU list: {value!r}")
    result = subprocess.run(
        ["taskset", "-c", value, "true"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        fail(f"{label} affinity cannot execute on this host: {value}: {result.stderr.strip()}")


def capture_map(captures: Iterable[CaptureSpec]) -> dict[str, CaptureSpec]:
    result: dict[str, CaptureSpec] = {}
    for capture in captures:
        if capture.capture_id in result:
            fail(f"duplicate capture ID: {capture.capture_id}")
        result[capture.capture_id] = capture
    return result


def mode_method(mode: str) -> str:
    return MODE_FIELDS[mode][0]


def method_mode(method: str) -> str:
    matches = [mode for mode, values in MODE_FIELDS.items() if values[0] == method]
    if len(matches) != 1:
        fail(f"chart method has no unique capture mode: {method!r}")
    return matches[0]


def raw_key_slug(key: tuple[str, str]) -> str:
    method, worker = key
    if not re.fullmatch(r"[a-z0-9_]+", method) or not re.fullmatch(
        r"(?:[1-9][0-9]*|auto|default|native)", worker
    ):
        fail(f"raw capture key cannot form a safe repeat path: {key!r}")
    return f"{method}--{worker}"


def build_matrix(
    physical_affinity: str, smt_affinity: str, unpinned_affinity: str
) -> tuple[list[CaptureSpec], list[RowSpec]]:
    """Build the immutable base matrix, deduplicating resolver-equivalent rows."""

    captures: list[CaptureSpec] = []

    def capture(
        capture_id: str,
        affinity_kind: str,
        affinity_cpus: str,
        fixture: str,
        iterations: int,
        native_moves: int,
        candidates: int,
        top_k: int,
        modes: Sequence[str],
        workers: Sequence[str],
        lazy: str = "off",
        memory_budget: int = MEMORY_BUDGET_BYTES,
        rss_limit: int = RSS_LIMIT_BYTES,
        role: str = "standard",
    ) -> None:
        captures.append(
            CaptureSpec(
                capture_id,
                affinity_kind,
                affinity_cpus,
                fixture,
                iterations,
                native_moves,
                candidates,
                top_k,
                tuple(modes),
                tuple(workers),
                lazy,
                memory_budget,
                rss_limit,
                role,
            )
        )

    w1248 = ("1", "2", "4", "8")
    exact_modes = ("sampled_tree_fixed", "grammar_exact", "hybrid_exact")
    capture("small-dense64-physical", "physical", physical_affinity, "small", 1, 50, 64, 0, ("grammar_lower_bound",), w1248)
    capture("small-exact1-physical", "physical", physical_affinity, "small", 1, 50, 1, 1, ("grammar_exact",), w1248)
    capture("small-primary32k4-physical", "physical", physical_affinity, "small", 1, 50, 32, 4, exact_modes, w1248)
    # Action 6 requires the stress shape on the small fixture as well as the
    # medium seedtree.  Keep the same three-iteration contract as medium.
    capture("small-stress128k16-physical", "physical", physical_affinity, "small", 3, 50, 128, 16, exact_modes, w1248)
    capture("medium-dense64-physical", "physical", physical_affinity, "medium", 1, 50, 64, 0, ("grammar_lower_bound",), w1248)
    capture("medium-cache1-physical", "physical", physical_affinity, "medium", 1, 50, 1, 0, ("grammar_lower_bound",), w1248)
    capture("medium-exact1-physical", "physical", physical_affinity, "medium", 1, 50, 1, 1, ("grammar_exact",), w1248)
    capture("medium-primary32k4-physical", "physical", physical_affinity, "medium", 1, 50, 32, 4, exact_modes, w1248)
    # This is the final contracted stress shape: three iterations and all exact
    # sources.  It deliberately does not redefine the primary 32/4 gate.
    capture("medium-stress128k16-physical", "physical", physical_affinity, "medium", 3, 50, 128, 16, exact_modes, w1248)
    capture("medium-lazy64-physical", "physical", physical_affinity, "medium", 1, 50, 64, 0, ("grammar_lower_bound",), w1248, "on")
    capture("medium-primary32k4-smt", "smt", smt_affinity, "medium", 1, 50, 32, 4, ("grammar_exact",), ("1", "2", "4", "8", "16", "auto", "default"))
    capture("small-auto-unpinned", "unpinned", unpinned_affinity, "small", 1, 1, 64, 0, ("grammar_lower_bound",), ("1", "auto"))
    capture(
        "real20d-exact1-preflight-physical",
        "physical",
        physical_affinity,
        "real20d",
        1,
        0,
        1,
        1,
        ("grammar_exact",),
        ("1", "8"),
        rss_limit=REAL_PREFLIGHT_BYTES,
        role="real_preflight",
    )
    if unpinned_affinity != smt_affinity:
        capture("medium-primary32k4-unpinned", "unpinned", unpinned_affinity, "medium", 1, 50, 32, 4, ("grammar_exact",), ("auto", "default"))

    by_id = capture_map(captures)
    rows: list[RowSpec] = []
    resolution_seen: dict[tuple[str, ...], RowSpec] = {}

    # Hash placeholders are enough to exercise the exact resolver key here.
    fixture_hashes = {
        "small": ("small-primary", "-"),
        "medium": ("medium-primary", "medium-refseq"),
        "real20d": ("real20d-primary", "real20d-refseq"),
    }

    def row_contract(spec: RowSpec) -> dict[str, str]:
        capture_spec = by_id[spec.capture_id]
        return base_manifest_contract(
            capture_spec,
            spec,
            fixture_hashes[capture_spec.fixture][0],
            fixture_hashes[capture_spec.fixture][1],
        )

    def add(spec: RowSpec) -> None:
        contract = row_contract(spec)
        key = tuple(contract[field] for field in RESOLUTION_FIELDS)
        if key in resolution_seen:
            # One manifest row intentionally serves both commands whenever the
            # observed unpinned affinity is already the frozen SMT/physical set.
            previous = resolution_seen[key]
            if previous.measured_trial_target != spec.measured_trial_target:
                fail(
                    "resolver-equivalent Phase-0 rows disagree on measured "
                    f"trial target: {previous.row_id}={previous.measured_trial_target}, "
                    f"{spec.row_id}={spec.measured_trial_target}"
                )
            return
        resolution_seen[key] = spec
        rows.append(spec)

    def native(
        row_id: str, group: str, name: str, source: str, target: int = 5
    ) -> None:
        add(
            RowSpec(
                row_id,
                group,
                name,
                source,
                "sample_explore_merge",
                "native",
                measured_trial_target=target,
            )
        )

    def charts(
        prefix: str,
        group: str,
        name: str,
        source: str,
        target: int = 5,
        target_by_worker: Mapping[str, int] | None = None,
    ) -> None:
        capture_spec = by_id[source]
        for mode in capture_spec.modes:
            method = mode_method(mode)
            short_method = method.removeprefix("chart_spr_").replace("_", "-")
            for worker in capture_spec.workers:
                suffix = "default" if worker == "default" else ("auto" if worker == "auto" else f"w{worker}")
                timeout_policy = (
                    "allow"
                    if capture_spec.fixture == "medium"
                    and method != "chart_spr_grammar_lower_bound_heuristic"
                    else "forbid"
                )
                add(
                    RowSpec(
                        f"{prefix}-{short_method}-{suffix}",
                        group,
                        name,
                        source,
                        method,
                        worker,
                        timeout_policy,
                        measured_trial_target=(
                            target
                            if target_by_worker is None
                            else target_by_worker[worker]
                        ),
                    )
                )

    native("p0-native-small-physical-i1-m50", "p0-small-dense-physical", "dense-local-small-64", "small-dense64-physical")
    charts("p0-small-dense64", "p0-small-dense-physical", "dense-local-small-64", "small-dense64-physical")
    charts("p0-small-exact1", "p0-small-exact1-physical", "exact-small-one", "small-exact1-physical")
    charts(
        "p0-small-primary32k4",
        "p0-small-primary-physical",
        "exact-small-topk4",
        "small-primary32k4-physical",
        target=3,
    )
    native(
        "p0-native-small-physical-i3-m50",
        "p0-small-stress-physical",
        "stress-small-128-16",
        "small-stress128k16-physical",
        target=3,
    )
    charts(
        "p0-small-stress128k16",
        "p0-small-stress-physical",
        "stress-small-128-16",
        "small-stress128k16-physical",
        target=3,
    )

    native("p0-native-medium-physical-i1-m50", "p0-primary-physical", "exact-medium-topk4", "medium-primary32k4-physical")
    charts("p0-medium-dense64", "p0-medium-dense-physical", "dense-local-medium-64", "medium-dense64-physical")
    charts("p0-medium-cache1", "p0-medium-cache-physical", "cache-medium", "medium-cache1-physical")
    charts("p0-medium-exact1", "p0-medium-exact1-physical", "exact-medium-one", "medium-exact1-physical")
    charts("p0-medium-primary32k4", "p0-primary-physical", "exact-medium-topk4", "medium-primary32k4-physical")
    native("p0-native-medium-physical-i3-m50", "p0-stress-physical", "stress-medium-128-16", "medium-stress128k16-physical", target=3)
    charts("p0-medium-stress128k16", "p0-stress-physical", "stress-medium-128-16", "medium-stress128k16-physical", target=3)
    charts("p0-medium-lazy64", "p0-medium-lazy-physical", "lazy-compression-medium", "medium-lazy64-physical")

    native("p0-native-medium-smt-i1-m50", "p0-primary-smt", "exact-medium-topk4-smt", "medium-primary32k4-smt")
    charts(
        "p0-medium-primary32k4-smt",
        "p0-primary-smt",
        "exact-medium-topk4-smt",
        "medium-primary32k4-smt",
        target_by_worker={
            "1": 3,
            "2": 3,
            "4": 3,
            "8": 3,
            "16": 3,
            "auto": 5,
            "default": 5,
        },
    )

    native("p0-native-small-unpinned-i1-m1", "p0-small-auto", "small-auto-overhead", "small-auto-unpinned")
    charts("p0-small-auto", "p0-small-auto", "small-auto-overhead", "small-auto-unpinned")
    if unpinned_affinity != smt_affinity:
        native("p0-native-medium-unpinned-i1-m50", "p0-primary-unpinned", "exact-medium-topk4-unpinned", "medium-primary32k4-unpinned")
        charts(
            "p0-medium-primary32k4-unpinned",
            "p0-primary-unpinned",
            "exact-medium-topk4-unpinned",
            "medium-primary32k4-unpinned",
            target=5,
        )

    for worker in ("1", "8"):
        add(
            RowSpec(
                f"p0-real20d-preflight-grammar-exact-w{worker}",
                "real-bounded",
                "real20d-exact-one",
                "real20d-exact1-preflight-physical",
                "chart_spr_grammar_exact",
                worker,
                "classify",
                "preflight",
                measured_trial_target=1,
            )
        )

    assert_unique_resolution(rows, by_id, fixture_hashes)
    assert_wall_gate_pairs(rows, by_id)
    return captures, rows


def base_manifest_contract(
    capture: CaptureSpec,
    spec: RowSpec,
    primary_sha: str,
    refseq_sha: str,
) -> dict[str, str]:
    native = spec.method == "sample_explore_merge"
    row = {field: "-" for field in RESOLUTION_FIELDS}
    row.update(
        {
            "method": spec.method,
            "binary_role": "frozen_native" if native else "working_chart",
            "input_kind": "dag_pb" if capture.fixture == "small" else "tree_pb_refseq",
            "primary_sha256": primary_sha,
            "refseq_sha256": refseq_sha,
            "affinity_cpus": capture.affinity_cpus,
            "iterations": str(capture.iterations),
            "seed": "1",
            "timeout_seconds": str(TIMEOUT_SECONDS),
            "validate": "true",
        }
    )
    if native:
        row.update(
            {
                "requested_workers": "-",
                "worker_option": "none",
                "expected_worker_policy": "-",
                "native_max_moves": str(capture.native_max_moves),
            }
        )
        return row

    mode_tuple = next(value for value in MODE_FIELDS.values() if value[0] == spec.method)
    _, acceptance, objective, source, topology = mode_tuple
    requested = "0" if spec.requested_workers == "auto" else spec.requested_workers
    row.update(
        {
            "requested_workers": requested,
            "worker_option": "none" if requested == "default" else "chart_spr_workers",
            "expected_worker_policy": (
                "policy"
                if requested == "default"
                else ("automatic" if requested == "0" else "explicit")
            ),
            "native_max_moves": "-",
            "chart_max_candidates": str(capture.chart_max_candidates),
            "chart_top_k_exact": str(capture.chart_top_k_exact),
            "candidate_cap_semantics": "post-dedup",
            "acceptance": acceptance,
            "objective": objective,
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": source,
            "topology_selector": topology,
            "randomize_order": "false",
            "reservoir_sample": "false",
            "include_immediate_reversals": "false",
            "sampled_tree_count": "1",
            "sampled_tree_radius": "0",
            "sampled_tree_score_threshold": "2147483647",
            "max_upward_path_expansions": "0",
            "max_path_pairs": "0",
            "min_moved_clade_size": "1",
            "max_moved_clade_size": "0",
            "min_target_clade_size": "1",
            "max_target_clade_size": "0",
            "max_affected_clades": "0",
            "polytomy_mode": "expand-bounded",
            "polytomy_max_exact_arity": "6",
            "polytomy_max_shapes": "1",
            "polytomy_max_productions": "1024",
            "polytomy_max_clades": "256",
            "lazy_policy": capture.lazy_policy,
            "max_cached_patterns": "0",
            "pattern_batch_size": "0",
            "candidate_batch_size": "0",
            "memory_budget_bytes": str(capture.memory_budget_bytes),
            "commit_mode": "overlay_delta",
            "verification_mode": "transient",
            "local_accept_updates": "false",
            "dominance_mode": "off",
            "bound_pruning": "true",
            "require_exact_keep_mask": "true",
            "max_frontier_entries": "0",
            "score_ua_edge": "false",
            "force_no_vcf": "true",
        }
    )
    return row


def assert_unique_resolution(
    rows: Sequence[RowSpec],
    captures: Mapping[str, CaptureSpec],
    fixture_hashes: Mapping[str, tuple[str, str]],
) -> None:
    seen: dict[tuple[str, ...], str] = {}
    for spec in rows:
        capture = captures[spec.capture_id]
        primary, refseq = fixture_hashes[capture.fixture]
        contract = base_manifest_contract(capture, spec, primary, refseq)
        key = tuple(contract[field] for field in RESOLUTION_FIELDS)
        previous = seen.get(key)
        if previous is not None:
            fail(
                "non-group manifest resolution would be ambiguous for rows "
                f"{previous!r} and {spec.row_id!r}"
            )
        seen[key] = spec.row_id


def assert_wall_gate_pairs(
    rows: Sequence[RowSpec], captures: Mapping[str, CaptureSpec]
) -> None:
    """Prove each planned wall-gate chart row has one raw fixture join peer."""

    gate_groups = {
        "p0-primary-physical",
        "p0-stress-physical",
        "p0-primary-smt",
        "p0-small-auto",
        "p0-primary-unpinned",
    }
    natives = [row for row in rows if row.method == "sample_explore_merge"]
    checked = 0
    for chart in rows:
        if chart.method == "sample_explore_merge" or chart.run_group not in gate_groups:
            continue
        capture = captures[chart.capture_id]
        peers = [
            native
            for native in natives
            if native.workload_name == chart.workload_name
            and captures[native.capture_id].affinity_cpus == capture.affinity_cpus
            and captures[native.capture_id].fixture == capture.fixture
            and captures[native.capture_id].iterations == capture.iterations
        ]
        if len(peers) != 1:
            fail(
                f"planned wall gate row {chart.row_id!r} has {len(peers)} "
                "native rows with the exact fixture/workload_name pairing key"
            )
        if peers[0].run_group != chart.run_group:
            fail(
                f"manifest group {chart.run_group!r} would omit native wall-ratio "
                f"peer {peers[0].row_id!r} for {chart.row_id!r}"
            )
        checked += 1
    if checked == 0:
        fail("wall-gate pair audit checked no chart rows")


def fixture_paths(repo_root: Path, fixture: str) -> tuple[Path, Path | None]:
    if fixture == "small":
        return repo_root / "data/test_5_trees/tree_0.pb.gz", None
    if fixture == "medium":
        return (
            repo_root / "data/seedtree/seedtree.pb.gz",
            repo_root / "data/seedtree/refseq.txt.gz",
        )
    if fixture == "real20d":
        return (
            repo_root / "data/20D_from_fasta/1final-tree-1.nh1.pb.gz",
            repo_root / "data/20D_from_fasta/refseq.txt",
        )
    fail(f"unknown fixture: {fixture}")


def metadata_paths(repo_root: Path, baseline_dir: Path) -> dict[str, Path]:
    return {
        "repo_root": repo_root,
        "baseline_dir": baseline_dir,
        "bootstrap_dir": baseline_dir / "bootstrap-phase0",
        "metadata": baseline_dir / "bootstrap-phase0/metadata.json",
        "capture_plan": baseline_dir / "bootstrap-phase0/capture-plan.tsv",
        "row_plan": baseline_dir / "bootstrap-phase0/row-plan.tsv",
        "timeout_plan": baseline_dir / "bootstrap-phase0/timeout-eligibility.tsv",
        "field_evidence": baseline_dir / "bootstrap-phase0/timeout-field-evidence.tsv",
        "frozen_helper": baseline_dir / "bootstrap-phase0/bootstrap-helper.py",
        "prepared_contract": baseline_dir / "bootstrap-phase0/prepared-contract.tsv",
        "prepared_contract_seal": baseline_dir / "bootstrap-phase0/prepared-contract.tsv.sha256",
        "captures": baseline_dir / "bootstrap-phase0/captures",
        "commands": baseline_dir / "commands.phase0.sh",
        "frozen_larch2": baseline_dir / "bin/larch2",
        "frozen_oracle": baseline_dir / "bin/dagutil",
        "frozen_harness": baseline_dir / "bin/wric-spr-search-benchmark.sh",
        "frozen_process_metrics": baseline_dir / "bin/wric-process-metrics",
        "frozen_wrapper_calibration": baseline_dir / "bootstrap-phase0/wrapper-calibration.json",
        "frozen_calibration_controller": baseline_dir / "bootstrap-phase0/wric-wrapper-calibration.py",
        "frozen_calibration_workload_source": baseline_dir / "bootstrap-phase0/wric-wrapper-calibration-workload.cpp",
        "frozen_calibration_workload": baseline_dir / "bin/wric-wrapper-calibration-workload",
        "pending_manifest": baseline_dir / "workloads.pending.tsv",
        "pending_seal": baseline_dir / "workloads.pending.tsv.sha256",
        "artifact_manifest": baseline_dir / "phase0-artifacts.pending.tsv",
        "artifact_manifest_seal": baseline_dir / "phase0-artifacts.pending.tsv.sha256",
        "final_manifest": baseline_dir / "workloads.tsv",
        "final_seal": baseline_dir / "workloads.tsv.sha256",
        "final_artifact_manifest": baseline_dir / "phase0-artifacts.tsv",
        "final_artifact_manifest_seal": baseline_dir / "phase0-artifacts.tsv.sha256",
        "pending_smoke": baseline_dir / "bootstrap-phase0/strict-smoke-pending",
        "pending_smoke_status": baseline_dir / "bootstrap-phase0/strict-smoke-pending.status.json",
        "final_smoke": baseline_dir / "bootstrap-phase0/strict-smoke-final",
        "final_smoke_status": baseline_dir / "bootstrap-phase0/strict-smoke-final.status.json",
        "pending_real_smoke": baseline_dir / "bootstrap-phase0/strict-real-smoke-pending",
        "pending_real_smoke_status": baseline_dir / "bootstrap-phase0/strict-real-smoke-pending.status.json",
        "final_real_smoke": baseline_dir / "bootstrap-phase0/strict-real-smoke-final",
        "final_real_smoke_status": baseline_dir / "bootstrap-phase0/strict-real-smoke-final.status.json",
        "seal_transaction": baseline_dir / "bootstrap-phase0/seal-transaction.json",
        "real_refusal_proof": baseline_dir / "bootstrap-phase0/real20d-refusal-source-proof.txt",
        "real_outcome_approval": baseline_dir / "bootstrap-phase0/real20d-outcome-approval.json",
        "capture_metadata": baseline_dir / "capture-metadata.txt",
        "native_loop_proof": baseline_dir / "native-loop-proof.txt",
        "provenance_commands": baseline_dir / "provenance-commands.sh",
        "unsealed_inputs": baseline_dir / "unsealed-sha256-inputs.txt",
    }


def assert_no_sealed_base(paths: Mapping[str, Path]) -> None:
    if (
        path_occupied(paths["final_manifest"])
        or path_occupied(paths["final_seal"])
        or path_occupied(paths["seal_transaction"])
    ):
        fail(
            "the Phase-0 base is already present/sealed or promotion is in "
            "progress; bootstrap mutation is forbidden"
        )


def serialize_capture(capture: CaptureSpec) -> dict[str, object]:
    value = dataclasses.asdict(capture)
    value["modes"] = list(capture.modes)
    value["workers"] = list(capture.workers)
    return value


def serialize_row(row: RowSpec) -> dict[str, object]:
    return dataclasses.asdict(row)


def deserialize_capture(value: Mapping[str, object]) -> CaptureSpec:
    return CaptureSpec(
        capture_id=str(value["capture_id"]),
        affinity_kind=str(value["affinity_kind"]),
        affinity_cpus=str(value["affinity_cpus"]),
        fixture=str(value["fixture"]),
        iterations=int(value["iterations"]),
        native_max_moves=int(value["native_max_moves"]),
        chart_max_candidates=int(value["chart_max_candidates"]),
        chart_top_k_exact=int(value["chart_top_k_exact"]),
        modes=tuple(str(item) for item in value["modes"]),  # type: ignore[index]
        workers=tuple(str(item) for item in value["workers"]),  # type: ignore[index]
        lazy_policy=str(value["lazy_policy"]),
        memory_budget_bytes=int(value["memory_budget_bytes"]),
        rss_limit_bytes=int(value["rss_limit_bytes"]),
        capture_role=str(value["capture_role"]),
    )


def deserialize_row(value: Mapping[str, object]) -> RowSpec:
    text_fields = {
        field.name: str(value[field.name])
        for field in dataclasses.fields(RowSpec)
        if field.name != "measured_trial_target"
    }
    return RowSpec(
        **text_fields,
        measured_trial_target=int(value["measured_trial_target"]),
    )


def render_capture_plan(captures: Sequence[CaptureSpec]) -> bytes:
    fields = [field.name for field in dataclasses.fields(CaptureSpec)] + [
        "long_medium",
        "long_real",
    ]
    lines = ["\t".join(fields)]
    for item in captures:
        values = dataclasses.asdict(item)
        values["modes"] = " ".join(item.modes)
        values["workers"] = ",".join(item.workers)
        values["long_medium"] = "true" if item.long_medium else "false"
        values["long_real"] = "true" if item.long_real else "false"
        lines.append("\t".join(str(values[field]) for field in fields))
    return ("\n".join(lines) + "\n").encode("utf-8")


def render_row_plan(rows: Sequence[RowSpec]) -> bytes:
    fields = [field.name for field in dataclasses.fields(RowSpec)]
    lines = ["\t".join(fields)]
    for item in rows:
        values = dataclasses.asdict(item)
        lines.append("\t".join(str(values[field]) for field in fields))
    return ("\n".join(lines) + "\n").encode("utf-8")


def render_timeout_plan(rows: Sequence[RowSpec]) -> bytes:
    fields = (
        "row_id",
        "capture_id",
        "method",
        "requested_workers",
        "timeout_policy",
        "finite_success_trial_target",
        "timeout_characterization_trials",
        "timeout_evidence_kind",
        "required_confirmation",
    )
    lines = ["\t".join(fields)]
    for item in sorted(
        (row for row in rows if row.timeout_policy == "allow"),
        key=lambda row: row.row_id,
    ):
        lines.append(
            "\t".join(
                (
                    item.row_id,
                    item.capture_id,
                    item.method,
                    item.requested_workers,
                    item.timeout_policy,
                    str(item.measured_trial_target),
                    "1",
                    "non_performance",
                    "post-capture explicit approval only",
                )
            )
        )
    return ("\n".join(lines) + "\n").encode("utf-8")


def canonical_json_fragment(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def load_metadata(baseline_dir: Path) -> tuple[dict[str, object], dict[str, Path]]:
    baseline_dir = baseline_dir.resolve()
    repo_root = fixed_repo_root(baseline_dir)
    paths = metadata_paths(repo_root, baseline_dir)
    # The detached prepared-contract root is checked before metadata supplies
    # even one path, hash, affinity, row, or capture value.
    verify_prepared_contract_artifacts(repo_root, paths)
    metadata_path = paths["metadata"]
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"bootstrap metadata is missing/invalid; run prepare first: {error}")
    if not isinstance(metadata, dict):
        fail("bootstrap metadata root is not an object")
    verify_prepared_metadata(metadata, repo_root, paths)
    return metadata, paths


def verify_metadata_inputs(metadata: Mapping[str, object]) -> None:
    repo_root = Path(str(metadata["repo_root"]))
    baseline_dir = Path(str(metadata["baseline_dir"]))
    paths = metadata_paths(repo_root, baseline_dir)
    checks = (
        ("frozen_larch2", "frozen_larch2_sha256", paths["frozen_larch2"]),
        ("frozen_oracle", "frozen_oracle_sha256", paths["frozen_oracle"]),
        ("harness", "harness_sha256", paths["frozen_harness"]),
        ("process_metrics", "process_metrics_sha256", paths["frozen_process_metrics"]),
        (
            "calibration_workload",
            "calibration_workload_sha256",
            paths["frozen_calibration_workload"],
        ),
    )
    identities: set[tuple[int, int]] = set()
    for path_key, hash_key, expected_path in checks:
        if str(metadata[path_key]) != str(expected_path):
            fail(
                f"{path_key} is not the exact frozen baseline role path: "
                f"{metadata[path_key]!r} != {str(expected_path)!r}"
            )
        path = expected_path
        require_exact_regular_file(
            path, f"frozen {path_key}", executable=True, immutable=True
        )
        info = path.stat(follow_symlinks=False)
        identity = (info.st_dev, info.st_ino)
        if identity in identities:
            fail("frozen executable roles must have distinct file identities")
        identities.add(identity)
        actual = sha256_file(path)
        if actual != metadata[hash_key]:
            fail(
                f"{path_key} changed after prepare: expected {metadata[hash_key]}, "
                f"observed {actual}"
            )
    frozen_evidence = (
        (
            "wrapper_calibration",
            "wrapper_calibration_sha256",
            paths["frozen_wrapper_calibration"],
            False,
        ),
        (
            "calibration_controller",
            "calibration_controller_sha256",
            paths["frozen_calibration_controller"],
            True,
        ),
        (
            "calibration_workload_source",
            "calibration_workload_source_sha256",
            paths["frozen_calibration_workload_source"],
            False,
        ),
    )
    for path_key, hash_key, expected_path, executable in frozen_evidence:
        if metadata[path_key] != str(expected_path):
            fail(f"{path_key} is not the exact frozen calibration role path")
        require_exact_regular_file(
            expected_path,
            f"frozen {path_key}",
            executable=executable,
            immutable=True,
        )
        if sha256_file(expected_path) != metadata[hash_key]:
            fail(f"{path_key} changed after prepare")
    load_wrapper_calibration(
        paths["frozen_wrapper_calibration"],
        expected_runner_sha256=sha256_file(paths["frozen_process_metrics"]),
        expected_controller_sha256=sha256_file(
            paths["frozen_calibration_controller"]
        ),
        expected_workload_sha256=sha256_file(
            paths["frozen_calibration_workload"]
        ),
        expected_workload_source_sha256=sha256_file(
            paths["frozen_calibration_workload_source"]
        ),
        label="frozen wrapper calibration",
    )


def frozen_harness_command(metadata: Mapping[str, object]) -> list[str]:
    # Do not let an interactive shell, compiler setup, or Codex process leak
    # incidental variables into a timing run.  This exact map is embedded in
    # every completion record through its argv and repeated in live evidence.
    return [
        "env",
        "-i",
        *(f"{key}={value}" for key, value in CAPTURE_ENVIRONMENT.items()),
        f"WRIC_REPO_ROOT={metadata['repo_root']}",
        str(metadata["harness"]),
    ]


def capture_command(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    *,
    out_dir: Path | None = None,
    modes: Sequence[str] | None = None,
    workers: Sequence[str] | None = None,
    warmups: int = 0,
    repetitions: int = 1,
    native_only: bool = False,
    full_canonical: bool = True,
) -> list[str]:
    repo_root = Path(str(metadata["repo_root"]))
    selected_modes = tuple(capture.modes if modes is None else modes)
    selected_workers = tuple(capture.workers if workers is None else workers)
    if warmups < 0 or repetitions <= 0:
        fail("capture command requires nonnegative warmups and positive repetitions")
    if not selected_modes:
        fail("capture command requires at least one chart mode")
    if not selected_workers:
        fail("capture command requires at least one worker selection")
    command = frozen_harness_command(metadata)
    command += ["--dagutil", str(metadata["frozen_oracle"])]
    command += ["--larch2", str(metadata["frozen_larch2"])]
    command += ["--process-metrics", str(metadata["process_metrics"])]
    command += [
        "--out-dir",
        str(out_dir or (paths["captures"] / capture.capture_id / "characterization")),
    ]
    command += ["--iterations", str(capture.iterations), "--seed", "1"]
    command += ["--max-moves", str(capture.native_max_moves)]
    command += ["--max-candidates", str(capture.chart_max_candidates)]
    command += ["--top-k-exact", str(capture.chart_top_k_exact)]
    command += ["--modes", " ".join(selected_modes)]
    command += ["--polytomy-mode", "expand-bounded", "--polytomy-shapes", "1"]
    command += ["--warmups", str(warmups), "--repetitions", str(repetitions)]
    command += ["--timeout-seconds", str(TIMEOUT_SECONDS)]
    command += ["--capture-rss-limit-bytes", str(capture.rss_limit_bytes)]
    command += ["--chart-memory-budget", str(capture.memory_budget_bytes)]
    command += ["--chart-lazy-policy", capture.lazy_policy]
    if full_canonical:
        command += ["--full-canonical-correctness"]
    if native_only:
        command += ["--native-only"]
    elif selected_workers != ("default",):
        command += ["--workers-list", ",".join(selected_workers)]
    primary, refseq = fixture_paths(repo_root, capture.fixture)
    if refseq is None:
        command += ["--dag", str(primary)]
    else:
        command += ["--tree", f"{primary}:{refseq}"]
    if capture.affinity_kind != "unpinned":
        command = ["taskset", "-c", capture.affinity_cpus, *command]
    return command


def render_capture_commands(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    captures: Sequence[CaptureSpec],
    rows: Sequence[RowSpec],
) -> bytes:
    helper_sha = str(metadata["helper_sha256"])
    baseline_dir = paths["baseline_dir"]
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        'here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)',
        f"expected_helper_sha={shlex.quote(helper_sha)}",
        'actual_helper_sha=$(sha256sum "$here/bootstrap-phase0/bootstrap-helper.py" | awk \'{print $1}\')',
        '[[ "$actual_helper_sha" == "$expected_helper_sha" ]] || { echo \'frozen bootstrap helper hash mismatch\' >&2; exit 1; }',
        "[[ ${WRIC_CONFIRM_LONG_PHASE0_CAPTURES:-} == YES ]] || { echo 'set WRIC_CONFIRM_LONG_PHASE0_CAPTURES=YES to run the complete long matrix' >&2; exit 1; }",
        "",
    ]
    for item in captures:
        direct = (
            "guarded direct frozen-oracle/process-metrics chart-only "
            "preflight; exact argv is capture-generated and hash-bound"
            if item.capture_role == "real_preflight"
            else shlex.join(capture_command(metadata, paths, item))
        )
        lines += [
            f"# exact characterization invocation: {direct}",
            shlex.join(
                [
                    "python3",
                    str(paths["frozen_helper"]),
                    "capture",
                    "--baseline-dir",
                    str(baseline_dir),
                    "--capture-id",
                    item.capture_id,
                    *(["--confirm-long-medium"] if item.long_medium else []),
                    *(["--confirm-real-preflight"] if item.long_real else []),
                ]
            ),
            "",
        ]
        if item.capture_role != "real_preflight":
            specs = capture_row_specs(item, rows)
            for key in [
                key
                for key in [
                    ("sample_explore_merge", "native"),
                    *ordered_capture_chart_keys(item),
                ]
                if key in specs
            ]:
                stage, command = repeat_stage_command(
                    metadata,
                    paths,
                    item,
                    key,
                    specs[key].measured_trial_target,
                )
                lines += [
                    f"# success-only repeat row: {specs[key].row_id}",
                    f"# target measured trials: {specs[key].measured_trial_target}",
                    f"# repeat stage: {stage}",
                    f"# exact repeat invocation: {shlex.join(command)}",
                    "",
                ]
    return "\n".join(lines).encode("utf-8")


def real_refusal_source_proof(repo_root: Path) -> str:
    source_path = "include/larch/polytomy_refinement.hpp"
    ancestor = subprocess.run(
        [
            "git",
            "merge-base",
            "--is-ancestor",
            REAL20D_REFUSAL_INTRO_REVISION,
            FROZEN_REVISION,
        ],
        cwd=repo_root,
        check=False,
    )
    if ancestor.returncode != 0:
        fail(
            "the pre-existing real-20D refusal revision is not an ancestor of "
            "the frozen Phase-0 revision"
        )
    source = subprocess.run(
        ["git", "show", f"{FROZEN_REVISION}:{source_path}"],
        cwd=repo_root,
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    diagnostic = REAL20D_REFUSAL_TEXT.removeprefix("error: ").rstrip("\n")
    if diagnostic.encode("utf-8") not in source:
        fail(
            "the exact expected-infeasible diagnostic is absent from the "
            "frozen source"
        )
    blob = subprocess.run(
        ["git", "rev-parse", f"{FROZEN_REVISION}:{source_path}"],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()
    return (
        "schema=wric-real20d-refusal-source-proof-v1\n"
        f"frozen_revision={FROZEN_REVISION}\n"
        f"introducing_revision={REAL20D_REFUSAL_INTRO_REVISION}\n"
        f"source_path={source_path}\n"
        f"source_blob={blob}\n"
        f"diagnostic_sha256={sha256_bytes(REAL20D_REFUSAL_TEXT.encode('utf-8'))}\n"
        f"diagnostic={REAL20D_REFUSAL_TEXT.rstrip()}\n"
    )


PREPARED_METADATA_KEYS = {
    "bootstrap_version",
    "repo_root",
    "baseline_dir",
    "repo_revision",
    "merge_base",
    "physical_affinity",
    "smt_affinity",
    "unpinned_affinity",
    "frozen_larch2",
    "frozen_larch2_sha256",
    "frozen_oracle",
    "frozen_oracle_sha256",
    "harness",
    "harness_sha256",
    "harness_source_uri",
    "process_metrics",
    "process_metrics_sha256",
    "wrapper_calibration",
    "wrapper_calibration_sha256",
    "calibration_controller",
    "calibration_controller_sha256",
    "calibration_workload",
    "calibration_workload_sha256",
    "calibration_workload_source",
    "calibration_workload_source_sha256",
    "helper_sha256",
    "real20d_refusal_source_proof_sha256",
    "captures",
    "rows",
}


def prepared_contract_members(
    repo_root: Path, paths: Mapping[str, Path]
) -> set[Path]:
    members = {
        paths["metadata"],
        paths["capture_plan"],
        paths["row_plan"],
        paths["timeout_plan"],
        paths["commands"],
        paths["frozen_helper"],
        paths["real_refusal_proof"],
        paths["frozen_larch2"],
        paths["frozen_oracle"],
        paths["frozen_harness"],
        paths["frozen_process_metrics"],
        paths["frozen_wrapper_calibration"],
        paths["frozen_calibration_controller"],
        paths["frozen_calibration_workload_source"],
        paths["frozen_calibration_workload"],
        paths["capture_metadata"],
        paths["native_loop_proof"],
        paths["provenance_commands"],
        paths["unsealed_inputs"],
    }
    for fixture in ("small", "medium", "real20d"):
        members.update(
            path for path in fixture_paths(repo_root, fixture) if path is not None
        )
    return members


def verify_prepared_contract_artifacts(
    repo_root: Path, paths: Mapping[str, Path]
) -> None:
    require_exact_regular_file(
        paths["prepared_contract"], "prepared-contract ledger", immutable=True
    )
    require_exact_regular_file(
        paths["prepared_contract_seal"],
        "prepared-contract detached seal",
        immutable=True,
    )
    expected = prepared_contract_members(repo_root, paths)
    executable_members = {
        paths["commands"],
        paths["frozen_helper"],
        paths["frozen_larch2"],
        paths["frozen_oracle"],
        paths["frozen_harness"],
        paths["frozen_process_metrics"],
        paths["frozen_calibration_controller"],
        paths["frozen_calibration_workload"],
    }
    for path in expected:
        require_exact_regular_file(
            path,
            "prepared-contract member",
            executable=path in executable_members,
            immutable=path in executable_members,
        )
    audit_artifacts(
        repo_root,
        paths["prepared_contract"],
        paths["prepared_contract_seal"],
        expected_paths=expected,
        required_paths=tuple(expected),
    )


def read_unsealed_input_hashes(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\t\r\n]+)", line)
        if match is None:
            fail(f"invalid unsealed SHA-256 input line {number}: {line!r}")
        digest, relative = match.groups()
        value = Path(relative)
        if value.is_absolute() or ".." in value.parts or value.as_posix() != relative:
            fail(f"unsealed SHA-256 input path is not normalized: {relative!r}")
        if relative in result:
            fail(f"duplicate unsealed SHA-256 input path: {relative}")
        result[relative] = digest
    return result


def verify_fixture_contract(repo_root: Path, paths: Mapping[str, Path]) -> None:
    expected = {
        fixture_paths(repo_root, "small")[0]: SMALL_PRIMARY_SHA256,
        fixture_paths(repo_root, "medium")[0]: MEDIUM_PRIMARY_SHA256,
        fixture_paths(repo_root, "medium")[1]: MEDIUM_REFSEQ_SHA256,
        fixture_paths(repo_root, "real20d")[0]: REAL20D_PRIMARY_SHA256,
        fixture_paths(repo_root, "real20d")[1]: REAL20D_REFSEQ_SHA256,
        paths["frozen_larch2"]: FROZEN_LARCH2_SHA256,
    }
    recorded = read_unsealed_input_hashes(paths["unsealed_inputs"])
    for maybe_path, digest in expected.items():
        if maybe_path is None:
            raise AssertionError("fixture contract unexpectedly lacks a path")
        path = maybe_path
        relative = normalized_repo_path(repo_root, path)
        if recorded.get(relative) != digest:
            fail(
                "unsealed input evidence differs from the fixed fixture "
                f"contract: {relative}: {recorded.get(relative)!r} != {digest}"
            )
        if sha256_file(path) != digest:
            fail(f"fixed Phase-0 fixture bytes changed: {relative}")


def verify_bootstrap_namespace(
    metadata: Mapping[str, object], paths: Mapping[str, Path]
) -> None:
    """Reject files that no frozen bootstrap phase is allowed to create."""

    captures = [
        deserialize_capture(value) for value in metadata["captures"]  # type: ignore[index]
    ]
    fixed_files = {
        paths["metadata"],
        paths["capture_plan"],
        paths["row_plan"],
        paths["timeout_plan"],
        paths["field_evidence"],
        paths["frozen_helper"],
        paths["frozen_wrapper_calibration"],
        paths["frozen_calibration_controller"],
        paths["frozen_calibration_workload_source"],
        paths["prepared_contract"],
        paths["prepared_contract_seal"],
        paths["real_refusal_proof"],
        paths["real_outcome_approval"],
        paths["pending_smoke_status"],
        *smoke_stream_paths(paths["pending_smoke_status"]),
        paths["final_smoke_status"],
        *smoke_stream_paths(paths["final_smoke_status"]),
        paths["pending_real_smoke_status"],
        *smoke_stream_paths(paths["pending_real_smoke_status"]),
        paths["final_real_smoke_status"],
        *smoke_stream_paths(paths["final_real_smoke_status"]),
        paths["seal_transaction"],
    }
    for capture in captures:
        prefix = paths["bootstrap_dir"] / f"capture-{capture.capture_id}"
        fixed_files.update(
            {
                prefix.with_suffix(".stdout"),
                prefix.with_suffix(".stderr"),
                prefix.with_suffix(".status.json"),
                prefix.with_suffix(".timeout-approval.json"),
            }
        )
    fixed_directories = {
        paths["captures"],
        paths["pending_smoke"],
        paths["final_smoke"],
        paths["pending_real_smoke"],
        paths["final_real_smoke"],
    }
    for entry in paths["bootstrap_dir"].iterdir():
        if entry in fixed_files:
            if entry.is_symlink() or not entry.is_file():
                fail(f"bootstrap namespace file is not lexical regular: {entry}")
        elif entry in fixed_directories:
            if entry.is_symlink() or not entry.is_dir():
                fail(f"bootstrap namespace directory is not lexical: {entry}")
        else:
            fail(f"unexpected file/directory in bootstrap namespace: {entry}")
    if paths["captures"].is_dir():
        expected_capture_dirs = {
            paths["captures"] / capture.capture_id for capture in captures
        }
        for entry in paths["captures"].iterdir():
            if (
                entry not in expected_capture_dirs
                or entry.is_symlink()
                or not entry.is_dir()
            ):
                fail(f"unexpected/aliased capture directory: {entry}")


def verify_prepared_metadata(
    metadata: Mapping[str, object],
    repo_root: Path,
    paths: Mapping[str, Path],
) -> None:
    if set(metadata) != PREPARED_METADATA_KEYS:
        fail(
            "prepared metadata key set changed; "
            f"missing={sorted(PREPARED_METADATA_KEYS - set(metadata))}, "
            f"unexpected={sorted(set(metadata) - PREPARED_METADATA_KEYS)}"
        )
    canonical_metadata = (
        json.dumps(metadata, sort_keys=True, indent=2) + "\n"
    ).encode("utf-8")
    if paths["metadata"].read_bytes() != canonical_metadata:
        fail("prepared metadata is not the exact canonical JSON serialization")
    exact_scalars: dict[str, object] = {
        "bootstrap_version": BOOTSTRAP_VERSION,
        "repo_root": str(repo_root),
        "baseline_dir": str(paths["baseline_dir"]),
        "repo_revision": FROZEN_REVISION,
        "merge_base": MERGE_BASE,
        "frozen_larch2": str(paths["frozen_larch2"]),
        "frozen_larch2_sha256": FROZEN_LARCH2_SHA256,
        "frozen_oracle": str(paths["frozen_oracle"]),
        "frozen_oracle_sha256": FROZEN_ORACLE_SHA256,
        "harness": str(paths["frozen_harness"]),
        "harness_source_uri": "repo://tools/wric_spr_search_benchmark.sh",
        "process_metrics": str(paths["frozen_process_metrics"]),
        "wrapper_calibration": str(paths["frozen_wrapper_calibration"]),
        "calibration_controller": str(paths["frozen_calibration_controller"]),
        "calibration_workload": str(paths["frozen_calibration_workload"]),
        "calibration_workload_source": str(
            paths["frozen_calibration_workload_source"]
        ),
    }
    for key, expected in exact_scalars.items():
        if metadata[key] != expected:
            fail(
                f"prepared metadata {key} changed: {metadata[key]!r} != "
                f"{expected!r}"
            )
    affinity_keys = ("physical_affinity", "smt_affinity", "unpinned_affinity")
    if any(not isinstance(metadata[key], str) for key in affinity_keys):
        fail("prepared affinity values must be strings")
    affinities = tuple(str(metadata[key]) for key in affinity_keys)
    for label, affinity in zip(("physical", "SMT", "unpinned"), affinities):
        if not re.fullmatch(r"[0-9]+(?:[,-][0-9]+)*", affinity):
            fail(f"recorded {label} affinity is not canonical: {affinity!r}")
    captures, rows = build_matrix(*affinities)
    if not isinstance(metadata["captures"], list) or not isinstance(
        metadata["rows"], list
    ):
        fail("prepared captures/rows must be JSON arrays")
    expected_captures = [serialize_capture(item) for item in captures]
    expected_rows = [serialize_row(item) for item in rows]
    if canonical_json_fragment(metadata["captures"]) != canonical_json_fragment(
        expected_captures
    ):
        fail("prepared capture serialization differs from the rebuilt matrix")
    if canonical_json_fragment(metadata["rows"]) != canonical_json_fragment(
        expected_rows
    ):
        fail("prepared row serialization differs from the rebuilt matrix")
    expected_files = {
        paths["capture_plan"]: render_capture_plan(captures),
        paths["row_plan"]: render_row_plan(rows),
        paths["timeout_plan"]: render_timeout_plan(rows),
        paths["commands"]: render_capture_commands(metadata, paths, captures, rows),
    }
    for path, expected in expected_files.items():
        if path.read_bytes() != expected:
            fail(f"prepared derived artifact differs from rebuilt bytes: {path}")
    expected_proof = real_refusal_source_proof(repo_root).encode("utf-8")
    if paths["real_refusal_proof"].read_bytes() != expected_proof:
        fail("real-20D refusal proof differs from the frozen git object")
    if metadata["real20d_refusal_source_proof_sha256"] != sha256_bytes(
        expected_proof
    ):
        fail("prepared refusal-proof metadata digest changed")
    frozen_hashes = {
        "helper_sha256": paths["frozen_helper"],
        "harness_sha256": paths["frozen_harness"],
        "process_metrics_sha256": paths["frozen_process_metrics"],
        "wrapper_calibration_sha256": paths["frozen_wrapper_calibration"],
        "calibration_controller_sha256": paths["frozen_calibration_controller"],
        "calibration_workload_sha256": paths["frozen_calibration_workload"],
        "calibration_workload_source_sha256": paths[
            "frozen_calibration_workload_source"
        ],
    }
    for key, path in frozen_hashes.items():
        if metadata[key] != sha256_file(path):
            fail(f"prepared metadata {key} differs from the frozen role bytes")
    running_helper = Path(os.path.abspath(__file__))
    allowed_helpers = {
        repo_root / "tools/wric_phase0_manifest_bootstrap.py",
        paths["frozen_helper"],
    }
    if running_helper not in allowed_helpers:
        fail(f"bootstrap helper is running from an uncontracted path: {running_helper}")
    require_exact_regular_file(
        running_helper, "running bootstrap helper", executable=True
    )
    if sha256_file(running_helper) != metadata["helper_sha256"]:
        fail(
            "bootstrap helper bytes changed since prepare; run the frozen helper "
            f"instead: {paths['frozen_helper']}"
        )
    verify_metadata_inputs(metadata)
    helper_info = paths["frozen_helper"].stat(follow_symlinks=False)
    helper_identity = (helper_info.st_dev, helper_info.st_ino)
    for role in (
        "frozen_larch2",
        "frozen_oracle",
        "frozen_harness",
        "frozen_process_metrics",
        "frozen_calibration_workload",
    ):
        info = paths[role].stat(follow_symlinks=False)
        if (info.st_dev, info.st_ino) == helper_identity:
            fail("frozen helper and executable roles must be distinct files")
    verify_fixture_contract(repo_root, paths)
    verify_bootstrap_namespace(metadata, paths)


def prepare(args: argparse.Namespace) -> None:
    baseline_dir = Path(args.baseline_dir).resolve()
    repo_root = fixed_repo_root(baseline_dir)
    if Path(__file__).resolve() != (
        repo_root / "tools/wric_phase0_manifest_bootstrap.py"
    ):
        fail("prepare must run from the lexical live helper in the fixed repository")
    paths = metadata_paths(repo_root, baseline_dir)
    assert_no_sealed_base(paths)
    if not baseline_dir.is_dir():
        fail(f"baseline directory does not exist: {baseline_dir}")
    if path_occupied(paths["bootstrap_dir"]) or path_occupied(paths["commands"]):
        fail("bootstrap paths already exist; refusing to overwrite or resume implicitly")

    unpinned = args.unpinned_affinity or observed_affinity()
    validate_affinity(args.physical_affinity, "physical")
    validate_affinity(args.smt_affinity, "SMT")
    validate_affinity(unpinned, "unpinned")
    captures, rows = build_matrix(args.physical_affinity, args.smt_affinity, unpinned)
    frozen_larch2 = paths["frozen_larch2"]
    frozen_oracle = paths["frozen_oracle"]
    harness_source = repo_root / "tools/wric_spr_search_benchmark.sh"
    frozen_harness = paths["frozen_harness"]
    process_metrics_source = repo_root / "build/bin/wric-process-metrics"
    frozen_process_metrics = paths["frozen_process_metrics"]
    calibration_input = Path(args.wrapper_calibration).absolute()
    calibration_controller_source = repo_root / "tools/wric_wrapper_calibration.py"
    calibration_workload_source = (
        repo_root / "tools/wric_wrapper_calibration_workload.cpp"
    )
    calibration_workload_binary = (
        repo_root / "build/bin/wric-wrapper-calibration-workload"
    )
    required = (
        frozen_larch2,
        frozen_oracle,
        harness_source,
        process_metrics_source,
        calibration_input,
        calibration_controller_source,
        calibration_workload_source,
        calibration_workload_binary,
        paths["capture_metadata"],
        paths["native_loop_proof"],
        paths["provenance_commands"],
        paths["unsealed_inputs"],
    )
    for path in required:
        require_exact_regular_file(
            path,
            "required Phase-0 input",
            executable=path in (
                frozen_larch2,
                frozen_oracle,
                harness_source,
                process_metrics_source,
                calibration_controller_source,
                calibration_workload_binary,
            ),
            immutable=path in (frozen_larch2, frozen_oracle, calibration_input),
        )
    load_wrapper_calibration(
        calibration_input,
        expected_runner_sha256=sha256_file(process_metrics_source),
        expected_controller_sha256=sha256_file(calibration_controller_source),
        expected_workload_sha256=sha256_file(calibration_workload_binary),
        expected_workload_source_sha256=sha256_file(
            calibration_workload_source
        ),
        label="prepare wrapper calibration input",
    )
    if sha256_file(frozen_larch2) != FROZEN_LARCH2_SHA256:
        fail("frozen larch2 hash differs from the Phase-0 native oracle")
    if args.expected_oracle_sha256 != FROZEN_ORACLE_SHA256:
        fail(
            "--expected-oracle-sha256 must equal the independently settled "
            f"Phase-0 digest {FROZEN_ORACLE_SHA256}"
        )
    if sha256_file(frozen_oracle) != FROZEN_ORACLE_SHA256:
        fail(
            "frozen dagutil differs from the hard-bound corrected Phase-0 "
            "semantic oracle"
        )
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=repo_root, check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()
    if revision != FROZEN_REVISION:
        fail(f"prepare must run at the frozen research revision {FROZEN_REVISION}, got {revision}")
    for fixture in ("small", "medium", "real20d"):
        for path in fixture_paths(repo_root, fixture):
            if path is not None:
                require_exact_regular_file(path, "required Phase-0 fixture")
    verify_fixture_contract(repo_root, paths)

    source_bytes = Path(__file__).resolve().read_bytes()
    helper_sha = sha256_bytes(source_bytes)
    refusal_proof = real_refusal_source_proof(repo_root)
    metadata = {
        "bootstrap_version": BOOTSTRAP_VERSION,
        "repo_root": str(repo_root.resolve()),
        "baseline_dir": str(baseline_dir),
        "repo_revision": revision,
        "merge_base": MERGE_BASE,
        "physical_affinity": args.physical_affinity,
        "smt_affinity": args.smt_affinity,
        "unpinned_affinity": unpinned,
        "frozen_larch2": str(frozen_larch2),
        "frozen_larch2_sha256": FROZEN_LARCH2_SHA256,
        "frozen_oracle": str(frozen_oracle),
        "frozen_oracle_sha256": FROZEN_ORACLE_SHA256,
        "harness": str(frozen_harness),
        "harness_sha256": sha256_file(harness_source),
        "harness_source_uri": (
            f"repo://{normalized_repo_path(repo_root, harness_source)}"
        ),
        "process_metrics": str(frozen_process_metrics),
        "process_metrics_sha256": sha256_file(process_metrics_source),
        "wrapper_calibration": str(paths["frozen_wrapper_calibration"]),
        "wrapper_calibration_sha256": sha256_file(calibration_input),
        "calibration_controller": str(paths["frozen_calibration_controller"]),
        "calibration_controller_sha256": sha256_file(
            calibration_controller_source
        ),
        "calibration_workload": str(paths["frozen_calibration_workload"]),
        "calibration_workload_sha256": sha256_file(
            calibration_workload_binary
        ),
        "calibration_workload_source": str(
            paths["frozen_calibration_workload_source"]
        ),
        "calibration_workload_source_sha256": sha256_file(
            calibration_workload_source
        ),
        "helper_sha256": helper_sha,
        "real20d_refusal_source_proof_sha256": sha256_bytes(
            refusal_proof.encode("utf-8")
        ),
        "captures": [serialize_capture(item) for item in captures],
        "rows": [serialize_row(item) for item in rows],
    }

    paths["bootstrap_dir"].mkdir(mode=0o755)
    paths["captures"].mkdir(mode=0o755)
    try:
        if path_occupied(frozen_process_metrics):
            if (
                frozen_process_metrics.is_symlink()
                or not frozen_process_metrics.is_file()
                or sha256_file(frozen_process_metrics)
                != sha256_file(process_metrics_source)
            ):
                fail(
                    "existing frozen process-metrics helper differs from the "
                    f"current Phase-0 helper: {frozen_process_metrics}"
                )
        else:
            write_bytes_exclusive(
                frozen_process_metrics, process_metrics_source.read_bytes(), 0o555
            )
        if path_occupied(frozen_harness):
            if (
                frozen_harness.is_symlink()
                or not frozen_harness.is_file()
                or sha256_file(frozen_harness) != sha256_file(harness_source)
            ):
                fail(
                    "existing frozen benchmark harness differs from the "
                    f"current Phase-0 source: {frozen_harness}"
                )
        else:
            write_bytes_exclusive(
                frozen_harness, harness_source.read_bytes(), 0o555
            )
        if path_occupied(paths["frozen_calibration_workload"]):
            if (
                paths["frozen_calibration_workload"].is_symlink()
                or not paths["frozen_calibration_workload"].is_file()
                or sha256_file(paths["frozen_calibration_workload"])
                != sha256_file(calibration_workload_binary)
            ):
                fail(
                    "existing frozen calibration workload differs from the "
                    "validated build/bin helper"
                )
        else:
            write_bytes_exclusive(
                paths["frozen_calibration_workload"],
                calibration_workload_binary.read_bytes(),
                0o555,
            )
        write_bytes_exclusive(
            paths["frozen_wrapper_calibration"],
            calibration_input.read_bytes(),
            0o444,
        )
        write_bytes_exclusive(
            paths["frozen_calibration_controller"],
            calibration_controller_source.read_bytes(),
            0o555,
        )
        write_bytes_exclusive(
            paths["frozen_calibration_workload_source"],
            calibration_workload_source.read_bytes(),
            0o444,
        )
        verify_metadata_inputs(metadata)
        write_bytes_exclusive(paths["frozen_helper"], source_bytes, 0o555)
        write_text_exclusive(paths["real_refusal_proof"], refusal_proof, 0o444)
        write_text_exclusive(
            paths["metadata"],
            json.dumps(metadata, sort_keys=True, indent=2) + "\n",
            0o444,
        )
        write_bytes_exclusive(paths["capture_plan"], render_capture_plan(captures), 0o444)
        write_bytes_exclusive(paths["row_plan"], render_row_plan(rows), 0o444)
        allowed_timeout_rows = sorted(
            (row for row in rows if row.timeout_policy == "allow"),
            key=lambda row: row.row_id,
        )
        write_bytes_exclusive(
            paths["timeout_plan"], render_timeout_plan(rows), 0o444
        )
        write_bytes_exclusive(
            paths["commands"],
            render_capture_commands(metadata, paths, captures, rows),
            0o555,
        )
        verify_fixture_contract(repo_root, paths)
        if path_occupied(paths["prepared_contract"]) or path_occupied(
            paths["prepared_contract_seal"]
        ):
            fail("prepared-contract ledger or detached seal destination is occupied")
        write_artifact_ledger(
            repo_root,
            paths["prepared_contract"],
            paths["prepared_contract_seal"],
            prepared_contract_members(repo_root, paths),
        )
        # Re-enter through the same trust boundary used by every later action.
        load_metadata(baseline_dir)
    except BaseException:
        # Exclusive creation ensures no user data can be overwritten.  Leave
        # partial evidence in place for diagnosis instead of deleting it.
        raise

    print(f"prepared {len(captures)} capture commands and {len(rows)} unique manifest rows")
    print(f"capture plan: {paths['capture_plan']}")
    print(f"row plan: {paths['row_plan']}")
    print(
        f"timeout-eligibility plan: {paths['timeout_plan']} "
        f"({len(allowed_timeout_rows)} rows; observed timeouts require a "
        "separate exact post-capture approval)"
    )
    print("No benchmark capture was launched.")


def expected_capture_keys(capture: CaptureSpec) -> set[tuple[str, str]]:
    keys = {("sample_explore_merge", "native")}
    for mode in capture.modes:
        for worker in capture.workers:
            keys.add((mode_method(mode), worker))
    return keys


def capture_row_specs(
    capture: CaptureSpec, rows: Sequence[RowSpec]
) -> dict[tuple[str, str], RowSpec]:
    result: dict[tuple[str, str], RowSpec] = {}
    for spec in rows:
        if spec.capture_id != capture.capture_id:
            continue
        key = timeout_raw_key(spec)
        if key in result:
            fail(
                f"capture {capture.capture_id} has duplicate raw row specs for {key}"
            )
        if spec.real_role != "preflight" and spec.measured_trial_target < 3:
            fail(
                f"finite Phase-0 row target is below three: {spec.row_id}="
                f"{spec.measured_trial_target}"
            )
        result[key] = spec
    expected_charts = expected_capture_keys(capture) - {
        ("sample_explore_merge", "native")
    }
    missing = expected_charts - result.keys()
    unexpected = set(result) - expected_capture_keys(capture)
    if missing or unexpected:
        fail(
            f"capture/row plan mismatch for {capture.capture_id}: "
            f"missing={sorted(missing)}, unexpected={sorted(unexpected)}"
        )
    return result


def timeout_rows_for_capture(
    rows: Sequence[RowSpec], capture_id: str
) -> list[RowSpec]:
    result = sorted(
        (
            row
            for row in rows
            if row.capture_id == capture_id and row.timeout_policy == "allow"
        ),
        key=lambda row: row.row_id,
    )
    for row in result:
        if row.method == "sample_explore_merge":
            fail(f"native row may not allow a timeout: {row.row_id}")
    return result


def timeout_raw_key(spec: RowSpec) -> tuple[str, str]:
    return spec.method, (
        "native" if spec.method == "sample_explore_merge" else spec.requested_workers
    )


def timeout_key_map(specs: Sequence[RowSpec]) -> dict[tuple[str, str], str]:
    result: dict[tuple[str, str], str] = {}
    for spec in specs:
        key = timeout_raw_key(spec)
        if key in result:
            fail(
                "expected-timeout plan has duplicate raw resolver keys for "
                f"{result[key]!r} and {spec.row_id!r}"
            )
        result[key] = spec.row_id
    return result


def read_raw_trials(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream, dialect="excel-tab")
            fields = reader.fieldnames
            if (
                fields is None
                or any(not field for field in fields)
                or len(fields) != len(set(fields))
            ):
                fail(f"capture raw_trials.tsv has an invalid header: {path}")
            rows = list(reader)
    except FileNotFoundError:
        fail(f"capture omitted raw_trials.tsv: {path}")
    if not rows:
        fail(f"capture has no raw trials: {path}")
    for row in rows:
        if None in row or set(row) != set(fields) or any(
            value is None for value in row.values()
        ):
            fail(f"capture raw_trials.tsv has extra/missing cells: {path}")
    return rows


def raw_key(row: Mapping[str, str]) -> tuple[str, str]:
    worker = row["requested_workers"]
    return row["method"], worker


def validate_stage_artifact_paths(
    capture_root: Path,
    stage_dir: Path,
    rows: Sequence[Mapping[str, str]],
    expected_fixture: str,
) -> None:
    """Bind each timed row to its deterministic stage-local report and curve."""

    capture_resolved = capture_root.resolve(strict=True)
    stage_resolved = stage_dir.resolve(strict=True)
    report_parent = (stage_resolved / "logs").resolve(strict=True)
    curve_parent = (stage_resolved / "curves").resolve(strict=True)
    seen_reports: set[Path] = set()
    seen_curves: set[Path] = set()
    for row in rows:
        key = raw_key(row)
        trial = require_unsigned_text(
            row.get("trial_index", ""), f"{key} trial index", positive=True
        )
        report_text = row.get("report_path", "")
        curve_text = row.get("curve_path", "")
        report = Path(report_text).resolve(strict=True)
        curve = Path(curve_text).resolve(strict=True)
        for path, text, parent, label in (
            (report, report_text, report_parent, "report"),
            (curve, curve_text, curve_parent, "curve"),
        ):
            require_exact_regular_file(path, f"stage-local {label}")
            if str(path) != text or path.parent != parent:
                fail(
                    f"{key} {label} is not its lexical stage-local artifact: "
                    f"{text}: {stage_dir}"
                )
            try:
                path.relative_to(capture_resolved)
            except ValueError:
                fail(f"{key} {label} escapes its capture: {path}")
        method, worker = key
        fixture = row.get("fixture", "")
        expected_row_id = f"{expected_fixture}/{method}@{worker}"
        if fixture != expected_fixture or row.get("row_id") != expected_row_id:
            fail(
                f"{key} trial {trial} fixture/row identity changed: "
                f"{fixture!r}/{row.get('row_id')!r} != "
                f"{expected_fixture!r}/{expected_row_id!r}"
            )
        safe_fixture = re.sub(r"[^A-Za-z0-9_.-]", "_", expected_fixture)
        if method == "sample_explore_merge":
            # larch2's native iteration/progress report is written to stderr;
            # stdout is not the authoritative native report artifact.
            expected_report = f"{safe_fixture}_{method}_trial{trial}.err"
            expected_curve = f"{safe_fixture}_{method}_trial{trial}.tsv"
        else:
            safe_worker = re.sub(r"[^A-Za-z0-9_.-]", "_", worker)
            expected_report = (
                f"{safe_fixture}_{method}_trial{trial}_w{safe_worker}.out"
            )
            expected_curve = (
                f"{safe_fixture}_{method}_trial{trial}_w{safe_worker}.tsv"
            )
        if report.name != expected_report or curve.name != expected_curve:
            fail(
                f"{key} trial {trial} report/curve names do not match their "
                "exact deterministic stage identity: "
                f"{report.name}/{curve.name} != "
                f"{expected_report}/{expected_curve}"
            )
        if report in seen_reports or curve in seen_curves:
            fail(f"capture stage reuses a report/curve artifact: {stage_dir}: {key}")
        seen_reports.add(report)
        seen_curves.add(curve)


def validate_defining_report(
    capture_dir: Path,
    capture: CaptureSpec,
    method: str,
    requested_workers: str,
    raw: Mapping[str, str],
) -> Path:
    report = Path(raw.get("report_path", "")).resolve(strict=True)
    capture_root = capture_dir.resolve(strict=True)
    try:
        report.relative_to(capture_root)
    except ValueError:
        fail(f"raw report path escapes its capture: {report}")
    spec = RowSpec(
        row_id="capture-validation",
        run_group="capture-validation",
        workload_name="capture-validation",
        capture_id=capture.capture_id,
        method=method,
        requested_workers=requested_workers,
    )
    contract = base_manifest_contract(capture, spec, "primary", "refseq")
    for manifest_field, report_field in DEFINING_REPORT_FIELDS.items():
        expected = contract[manifest_field]
        actual = report_value(report, report_field)
        if actual != expected:
            fail(
                f"instrumented report mismatch for {method}@{requested_workers}: "
                f"{report_field}={actual!r}, expected {expected!r}: {report}"
            )
    expected_worker_policy = contract["expected_worker_policy"]
    actual_worker_policy = report_value(report, "chart_worker_policy")
    if expected_worker_policy == "policy":
        if actual_worker_policy not in ("default_serial", "automatic_default"):
            fail(
                "instrumented report has an unrecognized omitted/default "
                f"worker policy {actual_worker_policy!r}: {report}"
            )
    elif actual_worker_policy != expected_worker_policy:
        fail(
            f"instrumented report worker policy {actual_worker_policy!r} "
            f"differs from expected {expected_worker_policy!r}: {report}"
        )
    if report_value(report, "keep_mask_contract") != "exact_required":
        fail(f"instrumented report lacks the frozen exact keep-mask contract: {report}")
    refinement = report_value(report, "refinement_exactness")
    if refinement not in ("EXACT", "BOUNDED_REFINED_GRAMMAR"):
        fail(f"instrumented report has invalid refinement exactness {refinement!r}: {report}")
    return report


def validate_observed_rss_within_cap(
    row: Mapping[str, str], cap_bytes: int, label: str
) -> None:
    """Require both wait4 and sampled RSS observations to stay under the cap."""

    if cap_bytes <= 0:
        fail(f"{label} has a nonpositive RSS cap: {cap_bytes}")
    for field in ("max_rss_kb", "peak_sampled_rss_kb"):
        measured_kb = require_unsigned_text(
            row.get(field, ""), f"{label} {field}"
        )
        if measured_kb * 1024 > cap_bytes:
            fail(
                f"{label} exceeds its frozen RSS cap via {field}: "
                f"{measured_kb * 1024} > {cap_bytes}"
            )


def validate_standard_process_contract(
    capture: CaptureSpec,
    row: Mapping[str, str],
    status: str,
    key: tuple[str, str],
) -> None:
    """Reject any standard-capture row that did not run under its frozen cap."""
    expected = {
        "monitor_error": "0",
        "core_dumped": "0",
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
        "process_rss_limit_bytes": str(capture.rss_limit_bytes),
        "manifest_rss_limit_bytes": str(capture.rss_limit_bytes),
    }
    if status == "ok":
        expected |= {
            "runner_outcome": "exited",
            "runner_exit_code": "0",
            "exit_code": "0",
            "term_signal": "0",
            "timed_out": "0",
        }
    elif status == "timeout":
        expected |= {
            "runner_outcome": "timeout",
            "runner_exit_code": "124",
            "timed_out": "1",
        }
    else:
        fail(f"internal unsupported standard-capture status {status!r}: {key}")
    for field, expected_value in expected.items():
        if row.get(field) != expected_value:
            fail(
                f"{status} row has inconsistent capped-process {field}: "
                f"{key}: {row.get(field)!r} != {expected_value!r}"
            )
    exit_text = row.get("exit_code", "")
    signal_text = row.get("term_signal", "")
    if not re.fullmatch(r"-1|0|[1-9][0-9]*", exit_text):
        fail(f"{status} row has an invalid process exit code: {key}: {exit_text!r}")
    if not re.fullmatch(r"0|[1-9][0-9]*", signal_text):
        fail(
            f"{status} row has an invalid terminating signal: "
            f"{key}: {signal_text!r}"
        )
    exit_code = int(exit_text)
    term_signal = int(signal_text)
    if exit_code > 255 or term_signal > 64 or (
        (exit_code == -1) != (term_signal != 0)
    ):
        fail(
            f"{status} row has an inconsistent exit/signal representation: "
            f"{key}: exit_code={exit_code}, term_signal={term_signal}"
        )
    validate_observed_rss_within_cap(
        row, capture.rss_limit_bytes, f"{status} row {key}"
    )


RAW_FIXTURE_HASHES = {
    "small": (SMALL_PRIMARY_SHA256, "NA"),
    "medium": (MEDIUM_PRIMARY_SHA256, MEDIUM_REFSEQ_SHA256),
    "real20d": (REAL20D_PRIMARY_SHA256, REAL20D_REFSEQ_SHA256),
}

RAW_FIXTURE_LABELS = {
    "small": "tree_0.pb.gz",
    "medium": "seedtree.pb.gz",
    "real20d": "1final-tree-1.nh1.pb.gz",
}


def require_unsigned_text(value: str, label: str, *, positive: bool = False) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        fail(f"{label} is not an unsigned integer: {value!r}")
    parsed = int(value)
    if positive and parsed == 0:
        fail(f"{label} must be positive")
    return parsed


def validate_initial_score_unanimity(
    rows: Sequence[Mapping[str, str]], label: str
) -> str:
    """Require one numeric externally validated starting score per fixture."""

    scores = {
        str(
            require_unsigned_text(
                row.get("initial_validated_parsimony_min", ""),
                f"{label} initial validated parsimony",
            )
        )
        for row in rows
    }
    if len(scores) != 1:
        fail(f"{label} has non-unanimous initial validated parsimony: {sorted(scores)}")
    return next(iter(scores))


def require_decimal_text(value: str, label: str, *, positive: bool = False) -> Decimal:
    parsed = decimal_value(value, label)
    if positive and parsed == 0:
        fail(f"{label} must be positive")
    return parsed


def capture_row_spec(capture: CaptureSpec, key: tuple[str, str]) -> RowSpec:
    method, worker = key
    return RowSpec(
        row_id="capture-domain-validation",
        run_group="capture-domain-validation",
        workload_name="capture-domain-validation",
        capture_id=capture.capture_id,
        method=method,
        requested_workers=("native" if method == "sample_explore_merge" else worker),
    )


def expected_native_argv_sha256(capture: CaptureSpec) -> str:
    primary_sha, raw_refseq_sha = RAW_FIXTURE_HASHES[capture.fixture]
    if raw_refseq_sha == "NA":
        inputs = ("--dag-pb", f"@primary:{primary_sha}")
    else:
        inputs = (
            "--tree-pb",
            f"@primary:{primary_sha}",
            "--refseq",
            f"@refseq:{raw_refseq_sha}",
        )
    return canonical_argv_digest(
        (
            "@binary:frozen_native",
            *inputs,
            "-o",
            "@output",
            "-n",
            str(capture.iterations),
            "--max-moves",
            str(capture.native_max_moves),
            "--seed",
            "1",
            "--validate",
        )
    )


def expected_chart_argv(
    capture: CaptureSpec, key: tuple[str, str]
) -> list[str]:
    primary_sha, raw_refseq_sha = RAW_FIXTURE_HASHES[capture.fixture]
    if raw_refseq_sha == "NA":
        argv = ["@binary:working_chart", "--dag-pb", f"@primary:{primary_sha}"]
    else:
        argv = [
            "@binary:working_chart",
            "--tree-pb",
            f"@primary:{primary_sha}",
            "--refseq",
            f"@refseq:{raw_refseq_sha}",
        ]
    method, worker = key
    mode = method_mode(method)
    acceptance = {
        "sampled_tree_fixed": "fixed-topology",
        "grammar_exact": "exact",
        "hybrid_exact": "exact",
        "grammar_lower_bound": "lower-bound",
    }[mode]
    source = MODE_FIELDS[mode][3]
    argv += [
        "--force-no-vcf",
        "--validate",
        "--wric-polytomy-mode",
        "expand-bounded",
        "--wric-polytomy-max-shapes",
        "1",
        "--wric-lazy-chart",
        capture.lazy_policy,
        "--chart-spr-search",
        "--chart-spr-max-iterations",
        str(capture.iterations),
        "--chart-spr-max-candidates",
        str(capture.chart_max_candidates),
        "--chart-spr-top-k-exact",
        str(capture.chart_top_k_exact),
        "--chart-spr-candidate-selection",
        "lower-bound-top-k",
        "--chart-spr-candidate-source",
        source,
        "--chart-spr-acceptance",
        acceptance,
    ]
    if worker != "default":
        argv += ["--chart-spr-workers", "0" if worker == "auto" else worker]
    argv += [
        "--chart-spr-memory-budget",
        str(capture.memory_budget_bytes),
        "--seed",
        "1",
        "--chart-spr-canonical-result",
        "@search-canonical-result",
        "-o",
        "@output",
    ]
    return argv


def expected_chart_argv_sha256(
    capture: CaptureSpec, key: tuple[str, str]
) -> str:
    return canonical_argv_digest(expected_chart_argv(capture, key))


def validate_raw_trial_binding(
    capture: CaptureSpec,
    row: Mapping[str, str],
    key: tuple[str, str],
    status: str,
) -> None:
    """Validate command-owned and wrapper-owned fields for every timed row."""

    method, worker = key
    if row.get("benchmark_scope") != "CHART_SPR_PHASE0_SEARCH_COMPARISON":
        fail(f"raw trial has an invalid benchmark scope: {key}")
    if row.get("method") != method or row.get("requested_workers") != worker:
        fail(f"raw trial identity differs from its planned key: {key}")
    expected_fixture = RAW_FIXTURE_LABELS[capture.fixture]
    expected_row_id = f"{expected_fixture}/{method}@{worker}"
    if row.get("fixture") != expected_fixture or row.get("row_id") != expected_row_id:
        fail(
            f"raw trial fixture/row identity differs from its capture: {key}: "
            f"{row.get('fixture')!r}/{row.get('row_id')!r} != "
            f"{expected_fixture!r}/{expected_row_id!r}"
        )
    expected_input, expected_refseq = RAW_FIXTURE_HASHES[capture.fixture]
    if row.get("input_sha256") != expected_input or row.get(
        "refseq_sha256"
    ) != expected_refseq:
        fail(
            f"raw trial fixture hashes differ from {capture.fixture}: {key}: "
            f"{row.get('input_sha256')}/{row.get('refseq_sha256')}"
        )
    if row.get("manifest_rss_limit_bytes") != str(capture.rss_limit_bytes):
        fail(f"raw trial RSS contract differs from its capture: {key}")
    if method == "sample_explore_merge":
        if row.get("configured_chart_memory_budget") != "NA":
            fail(f"native raw trial claims a chart memory budget: {key}")
    elif row.get("configured_chart_memory_budget") != str(
        capture.memory_budget_bytes
    ):
        fail(f"raw chart trial memory budget differs from its capture: {key}")

    require_decimal_text(row.get("wall_clock_s", ""), f"{key} wall", positive=True)
    require_decimal_text(row.get("user_cpu_s", ""), f"{key} user CPU")
    require_decimal_text(row.get("system_cpu_s", ""), f"{key} system CPU")
    for field in ("max_rss_kb", "peak_sampled_rss_kb", "peak_sampled_swap_kb"):
        require_unsigned_text(row.get(field, ""), f"{key} {field}")
    if row.get("execution_order") not in ("baseline-first", "chart-first"):
        fail(f"raw trial has an invalid execution-order label: {key}")
    require_unsigned_text(row.get("trial_index", ""), f"{key} trial index", positive=True)

    expected_argv = (
        expected_native_argv_sha256(capture)
        if method == "sample_explore_merge"
        else expected_chart_argv_sha256(capture, key)
    )
    if row.get("canonical_argv_sha256") != expected_argv:
        fail(f"raw trial canonical argv is not bound to its capture: {key}")

    require_unsigned_text(
        row.get("initial_validated_parsimony_min", ""),
        f"{key} initial validated parsimony",
    )
    expected_command_fields = {
        "iterations": str(capture.iterations),
        "seed": "1",
    }
    if method == "sample_explore_merge":
        expected_command_fields |= {
            "acceptance": "sample_explore_merge",
            "objective": "parsimony_sampling",
            "candidate_selection": "native_best_moves",
            "candidate_source": "sampled_tree",
        }
    else:
        contract = base_manifest_contract(
            capture,
            capture_row_spec(capture, key),
            expected_input,
            "-" if expected_refseq == "NA" else expected_refseq,
        )
        expected_command_fields |= {
            field: contract[field]
            for field in (
                "acceptance",
                "objective",
                "candidate_selection",
                "candidate_source",
            )
        }
    for field, expected_value in expected_command_fields.items():
        if row.get(field) != expected_value:
            fail(
                f"raw trial command-owned {field} differs from its capture: "
                f"{key}: {row.get(field)!r} != {expected_value!r}"
            )

    if method == "sample_explore_merge":
        if row.get("resolved_workers") != "native" or row.get(
            "worker_policy"
        ) != "native":
            fail(f"native row has chart worker evidence: {key}")
    elif status == "timeout":
        if row.get("resolved_workers") != "NA" or row.get(
            "worker_policy"
        ) != "timeout_unobserved":
            fail(
                "timeout row claims a product worker resolution/policy that "
                f"was not observed: {key}"
            )


def validate_successful_raw_domains(
    capture_root: Path,
    capture: CaptureSpec,
    row: Mapping[str, str],
    key: tuple[str, str],
) -> None:
    """Validate all row-explicit Phase-0 values used as recorded evidence."""

    method, worker = key
    for field in (
        "initial_validated_parsimony_min",
        "final_validated_parsimony_min",
        "best_reported_objective",
        "best_validated_parsimony_min",
    ):
        require_unsigned_text(row.get(field, ""), f"{key} {field}")
    expected_best_validated = str(
        min(
            int(row["initial_validated_parsimony_min"]),
            int(row["final_validated_parsimony_min"]),
        )
    )
    if row["best_validated_parsimony_min"] != expected_best_validated:
        fail(f"successful row best validated parsimony is not recomputable: {key}")
    if row.get("iterations") != str(capture.iterations) or row.get("seed") != "1":
        fail(f"successful row work count/seed differs from its capture: {key}")

    search_sha = row.get("search_semantic_sha256", "")
    if method == "sample_explore_merge":
        if search_sha != "-":
            fail(f"native row claims a chart search digest: {key}")
    else:
        if not re.fullmatch(r"[0-9a-f]{64}", search_sha):
            fail(f"successful chart row lacks a search digest: {key}")
    output_sha = row.get("output_semantic_sha256", "")
    argv_sha = row.get("canonical_argv_sha256", "")
    wanted_trial = trial_digest(method, search_sha, output_sha, argv_sha)
    if row.get("trial_semantic_sha256") != wanted_trial or row.get(
        "canonical_digest"
    ) != wanted_trial:
        fail(f"successful row trial/canonical digest is not recomputable: {key}")

    if method == "sample_explore_merge":
        if row["best_reported_objective"] != row["final_validated_parsimony_min"]:
            fail(f"native reported endpoint is not the validated merged DAG: {key}")
        return

    spec = capture_row_spec(capture, key)
    primary_sha, raw_refseq_sha = RAW_FIXTURE_HASHES[capture.fixture]
    contract = base_manifest_contract(
        capture,
        spec,
        primary_sha,
        "-" if raw_refseq_sha == "NA" else raw_refseq_sha,
    )
    raw_contract_fields = {
        "acceptance": "acceptance",
        "objective": "objective",
        "candidate_selection": "candidate_selection",
        "candidate_source": "candidate_source",
    }
    for raw_field, contract_field in raw_contract_fields.items():
        if row.get(raw_field) != contract[contract_field]:
            fail(f"successful row {raw_field} differs from its capture: {key}")

    report = Path(row.get("report_path", "")).resolve(strict=True)
    try:
        report.relative_to(capture_root.resolve(strict=True))
    except ValueError:
        fail(f"successful raw-domain report escapes capture root: {report}")

    # Worker request, resolution, and policy are product observations.  Bind
    # all four redundant report/TSV surfaces so a report from another worker
    # run cannot be spliced into this trial.
    resolved_workers = require_unsigned_text(
        row.get("resolved_workers", ""), f"{key} resolved workers", positive=True
    )
    report_requested = require_unsigned_text(
        report_value(report, "chart_workers_requested"),
        f"{key} report requested workers",
    )
    report_resolved = require_unsigned_text(
        report_value(report, "chart_workers_resolved"),
        f"{key} report resolved workers",
        positive=True,
    )
    report_local = require_unsigned_text(
        report_value(report, "local_score_workers"),
        f"{key} report local score workers",
        positive=True,
    )
    report_policy = report_value(report, "chart_worker_policy")
    if report_resolved != resolved_workers or report_local != resolved_workers:
        fail(f"successful chart worker counts disagree across raw/report: {key}")
    if row.get("worker_policy") != report_policy:
        fail(f"successful chart worker policy differs from its report: {key}")
    if worker == "auto":
        if report_requested != 0 or report_policy != "automatic":
            fail(f"automatic chart worker observation is inconsistent: {key}")
    elif worker == "default":
        if report_policy == "default_serial":
            if report_requested != 1 or resolved_workers != 1:
                fail(f"serial default worker observation is inconsistent: {key}")
        elif report_policy == "automatic_default":
            if report_requested != 0:
                fail(f"automatic default worker request is not zero: {key}")
        else:
            fail(f"successful default row has an invalid worker policy: {key}")
    elif (
        report_requested != int(worker)
        or resolved_workers != int(worker)
        or report_policy != "explicit"
    ):
        fail(f"explicit chart worker observation is inconsistent: {key}")

    direct_report_fields = {
        "iterations": "iterations",
        "seed": "seed",
        "acceptance": "acceptance",
        "objective": "objective",
        "candidate_selection": "candidate_selection",
        "candidate_source": "candidate_source",
        "configured_chart_memory_budget": "memory_budget_bytes",
        "best_reported_objective": "final_score",
        "final_compaction_exactness_kind": "final_compaction_exactness_kind",
    }
    for raw_field, report_field in direct_report_fields.items():
        if row.get(raw_field) != report_value(report, report_field):
            fail(
                f"successful raw/report field differs for {key}: "
                f"{raw_field}/{report_field}"
            )
    report_initial_score = require_unsigned_text(
        report_value(report, "initial_score"), f"{key} report initial score"
    )
    report_final_score = require_unsigned_text(
        report_value(report, "final_score"), f"{key} report final score"
    )
    if report_final_score > report_initial_score:
        fail(f"successful chart objective worsened: {key}")
    # Grammar-exact is the one report convention that is itself the complete
    # DAG parsimony minimum.  Fixed-topology and lower-bound objectives are
    # intentionally kept separate from externally validated parsimony.
    if row.get("objective") == "grammar_exact" and (
        report_initial_score != int(row["initial_validated_parsimony_min"])
        or report_final_score != int(row["final_validated_parsimony_min"])
    ):
        fail(f"grammar-exact report/validated parsimony differs: {key}")

    count_fields = (
        "candidates_generated",
        "candidates_scored",
        "exact_verifications",
        "accepted_moves",
        "candidate_accepts_attempted",
        "post_materialization_rejections",
        "active_patterns",
        "grammar_clades",
        "grammar_productions",
        "chart_cache_resident_bytes",
        "final_dag_nodes",
        "final_dag_edges",
        "full_search_state_rebuilds",
        "final_compaction_rebuilds",
        "initial_search_state_rebuilds",
        "sidecar_rebuilds_after_accept",
        "overlay_materializations_for_exact_verification",
        "overlay_materializations_for_accept_materialization",
        "overlay_materializations_for_final_compaction",
        "overlay_materializations_for_oracle",
        "full_overlay_materializations",
        "upward_path_iterator_steps",
        "path_pairs_considered",
        "candidates_pruned_before_construction",
        "candidates_pruned_after_construction",
        "candidates_generated_after_dedup",
        "candidate_cap_cutoffs",
        "path_budget_cutoffs",
        "reachable_clades_traversed",
        "reachable_productions_traversed",
        "reachability_full_grammar_like_passes",
        "peak_concurrent_exact_verifiers",
        *EXACT_CANDIDATE_ADMISSION_FIELDS[:-1],
        "exact_candidate_timing_count",
    )
    counts = {
        field: require_unsigned_text(row.get(field, ""), f"{key} {field}")
        for field in count_fields
    }
    if not (
        counts["candidates_generated"] >= counts["candidates_scored"]
        >= counts["exact_verifications"]
    ):
        fail(f"successful row candidate/exact counts are inconsistent: {key}")
    if counts["candidates_generated_after_dedup"] != counts[
        "candidates_generated"
    ]:
        fail(f"successful row generated-candidate counters disagree: {key}")
    iterations = int(row["iterations"])
    if counts["candidates_generated"] > capture.chart_max_candidates * iterations:
        fail(f"successful row exceeds its candidate budget: {key}")
    if counts["exact_verifications"] > capture.chart_top_k_exact * iterations:
        fail(f"successful row exceeds its exact-verification budget: {key}")
    if counts["accepted_moves"] > iterations:
        fail(f"successful row accepts more than one move per iteration: {key}")
    if counts["candidate_accepts_attempted"] != (
        counts["accepted_moves"] + counts["post_materialization_rejections"]
    ):
        fail(f"successful row attempted/committed/rejected counts disagree: {key}")
    if counts["exact_candidate_timing_count"] != counts["exact_verifications"]:
        fail(f"successful row exact timing count differs from verifications: {key}")
    if counts["initial_search_state_rebuilds"] != 1 or counts[
        "full_search_state_rebuilds"
    ] != (
        counts["initial_search_state_rebuilds"]
        + counts["sidecar_rebuilds_after_accept"]
    ):
        fail(f"successful row search-state rebuild accounting differs: {key}")
    if counts["final_compaction_rebuilds"] not in (0, 1):
        fail(f"successful row final compaction count is not boolean: {key}")
    exactness = row.get("final_compaction_exactness_kind")
    if (counts["final_compaction_rebuilds"] == 0) != (exactness == "none"):
        fail(f"successful row final compaction/exactness differs: {key}")
    reason_materializations = sum(
        counts[field]
        for field in (
            "overlay_materializations_for_exact_verification",
            "overlay_materializations_for_accept_materialization",
            "overlay_materializations_for_final_compaction",
            "overlay_materializations_for_oracle",
        )
    )
    if counts["full_overlay_materializations"] != reason_materializations:
        fail(f"successful row overlay materialization accounting differs: {key}")

    timer_fields = (
        "candidate_generation_ms",
        "cache_build_ms",
        "initial_chart_construction_ms",
        "local_scoring_ms",
        "local_candidates_per_second",
        "exact_initialization_ms",
        "exact_verification_ms",
        "exact_candidate_verification_ms_min",
        "exact_candidate_verification_ms_mean",
        "exact_candidate_verification_ms_max",
        "accepted_rebuild_ms",
        "final_compaction_ms",
        "post_materialization_check_ms",
        "materialization_ms",
        "materialization_exact_verification_ms",
        "materialization_accepted_update_ms",
        "materialization_final_compaction_ms",
        EXACT_CANDIDATE_ADMISSION_FIELDS[-1],
        "total_ms",
    )
    timers = {
        field: require_decimal_text(row.get(field, ""), f"{key} {field}")
        for field in timer_fields
    }
    if not (
        timers["exact_candidate_verification_ms_min"]
        <= timers["exact_candidate_verification_ms_mean"]
        <= timers["exact_candidate_verification_ms_max"]
    ):
        fail(f"successful row exact-candidate timing order is invalid: {key}")
    if row.get("final_compaction_exactness_kind") not in {
        "none",
        "exact_optimal_production_union",
        "score_only_not_exact",
    }:
        fail(f"successful chart row has an invalid final exactness label: {key}")
    raw_to_report = {
        "active_patterns": "active_patterns",
        "grammar_clades": "final_grammar_clades",
        "grammar_productions": "final_grammar_productions",
        "candidates_generated": "candidates_generated",
        "candidates_scored": "candidates_scored",
        "exact_verifications": "exact_verifications",
        "accepted_moves": "accepted_moves",
        "candidate_accepts_attempted": "candidate_accepts_attempted",
        "post_materialization_rejections": "post_materialization_rejections",
        "chart_cache_resident_bytes": "chart_cache_resident_bytes",
        "full_search_state_rebuilds": "full_search_state_rebuilds",
        "final_compaction_rebuilds": "final_compaction_rebuilds",
        "initial_search_state_rebuilds": "initial_search_state_rebuilds",
        "sidecar_rebuilds_after_accept": "sidecar_rebuilds_after_accept",
        "overlay_materializations_for_exact_verification": "overlay_materializations_for_exact_verification",
        "overlay_materializations_for_accept_materialization": "overlay_materializations_for_accept_materialization",
        "overlay_materializations_for_final_compaction": "overlay_materializations_for_final_compaction",
        "candidate_generation_ms": "candidate_generation_ms",
        "cache_build_ms": "cache_build_ms",
        "initial_chart_construction_ms": "initial_chart_construction_ms",
        "local_scoring_ms": "local_scoring_ms",
        "local_candidates_per_second": "local_candidates_per_second",
        "exact_initialization_ms": "exact_initialization_ms",
        "exact_verification_ms": "exact_verification_ms",
        "exact_candidate_timing_count": "exact_candidate_timing_count",
        "exact_candidate_verification_ms_min": "exact_candidate_verification_ms_min",
        "exact_candidate_verification_ms_mean": "exact_candidate_verification_ms_mean",
        "exact_candidate_verification_ms_max": "exact_candidate_verification_ms_max",
        "accepted_rebuild_ms": "accepted_rebuild_ms",
        "final_compaction_ms": "final_compaction_ms",
        "post_materialization_check_ms": "post_materialization_check_ms",
        "materialization_ms": "materialization_ms",
        "materialization_exact_verification_ms": "materialization_exact_verification_ms",
        "materialization_accepted_update_ms": "materialization_accepted_update_ms",
        "materialization_final_compaction_ms": "materialization_final_compaction_ms",
        "peak_concurrent_exact_verifiers": "peak_concurrent_exact_verifiers",
        "total_ms": "total_ms",
    }
    raw_to_report.update(
        {field: field for field in EXACT_CANDIDATE_ADMISSION_FIELDS}
    )
    for raw_field, report_field in raw_to_report.items():
        if row.get(raw_field) != report_value(report, report_field):
            fail(
                f"successful raw/report field differs for {key}: "
                f"{raw_field}/{report_field}"
            )

    raw_to_counter = {
        "upward_path_iterator_steps": "upward_path_iterator_steps",
        "path_pairs_considered": "path_pairs_considered",
        "candidates_pruned_before_construction": "candidates_pruned_before_construction",
        "candidates_pruned_after_construction": "candidates_pruned_after_construction",
        "candidates_generated_after_dedup": "candidates_generated_after_dedup",
        "candidate_cap_cutoffs": "candidate_cap_cutoffs",
        "path_budget_cutoffs": "path_budget_cutoffs",
        "overlay_materializations_for_oracle": "overlay_materializations_for_oracle",
        "full_overlay_materializations": "full_overlay_materializations",
        "reachable_clades_traversed": "reachable_clades_traversed",
        "reachable_productions_traversed": "reachable_productions_traversed",
        "reachability_full_grammar_like_passes": "reachability_full_grammar_like_passes",
    }
    for raw_field, report_field in raw_to_counter.items():
        if row.get(raw_field) != report_counter_value(report, report_field):
            fail(
                f"successful raw/report counter differs for {key}: "
                f"{raw_field}/{report_field}"
            )
    for field in (
        "exact_verifications",
        "accepted_moves",
        "candidate_accepts_attempted",
        "post_materialization_rejections",
        "sidecar_rebuilds_after_accept",
        "overlay_materializations_for_exact_verification",
        "overlay_materializations_for_accept_materialization",
        "overlay_materializations_for_final_compaction",
    ):
        if row.get(field) != report_counter_value(report, field):
            fail(f"successful report summary/counter differs for {key}: {field}")

    for raw_field, section, report_field in (
        ("final_dag_nodes", "final_dag", "nodes"),
        ("final_dag_edges", "final_dag", "edges"),
        ("affected_mean", "affected_clade_count_distribution", "mean"),
        ("affected_p50", "affected_clade_count_distribution", "p50"),
        ("affected_p95", "affected_clade_count_distribution", "p95"),
        ("affected_max", "affected_clade_count_distribution", "max"),
    ):
        if row.get(raw_field) != report_section_value(report, section, report_field):
            fail(
                f"successful raw/report section field differs for {key}: "
                f"{raw_field}/{section}.{report_field}"
            )
    require_decimal_text(row.get("affected_mean", ""), f"{key} affected_mean")
    affected_quantiles = [
        require_unsigned_text(row.get(field, ""), f"{key} {field}")
        for field in ("affected_p50", "affected_p95", "affected_max")
    ]
    if affected_quantiles != sorted(affected_quantiles):
        fail(f"successful row affected-clade quantiles are inconsistent: {key}")

    if row.get("exact_ms_per_candidate") != row.get(
        "exact_candidate_verification_ms_mean"
    ):
        fail(f"successful row exact per-candidate timing is not recomputable: {key}")
    attempted = counts["candidate_accepts_attempted"]
    expected_commit_ratio = (
        "NA"
        if attempted == 0
        else f"{Decimal(counts['accepted_moves']) / Decimal(attempted):.6f}"
    )
    if row.get("committed_attempt_ratio") != expected_commit_ratio:
        fail(f"successful row committed-attempt ratio is not recomputable: {key}")
    scored = counts["candidates_scored"]
    expected_local_ratio = (
        "NA"
        if scored == 0
        else f"{timers['local_scoring_ms'] / Decimal(scored):.6f}"
    )
    if row.get("local_ms_per_candidate") != expected_local_ratio:
        fail(f"successful row local per-candidate timing is not recomputable: {key}")
    if report_value(report, "chain_per_accept_exactness_label") != (
        "none_conservative_materialize_rebuild"
    ):
        fail(f"Phase-0 report used an unexpected per-accept chain mode: {key}")


REPEATED_UNANIMOUS_RAW_FIELDS = (
    "benchmark_scope",
    "fixture",
    "method",
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
    "final_compaction_exactness_kind",
    "full_search_state_rebuilds",
    "final_compaction_rebuilds",
    "initial_search_state_rebuilds",
    "sidecar_rebuilds_after_accept",
    "overlay_materializations_for_exact_verification",
    "overlay_materializations_for_accept_materialization",
    "overlay_materializations_for_final_compaction",
    *EXACT_CANDIDATE_ADMISSION_FIELDS[:-1],
    "grammar_clades",
    "grammar_productions",
    "active_patterns",
    "final_dag_nodes",
    "final_dag_edges",
    "requested_workers",
    "resolved_workers",
    "worker_policy",
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

REPEATED_UNANIMOUS_REPORT_FIELDS = tuple(
    sorted(
        set(DEFINING_REPORT_FIELDS.values())
        | {
            "accepted_moves",
            "active_patterns",
            "cache_strategy",
            "candidates_generated",
            "candidates_scored",
            "chain_per_accept_exactness_label",
            "effective_pattern_batch_size",
            "exact_verifications",
            *EXACT_CANDIDATE_ADMISSION_FIELDS[:-1],
            "final_compaction_exactness_kind",
            "final_score",
            "initial_grammar_clades",
            "initial_grammar_productions",
            "initial_score",
            "iterations",
            "refinement_exactness",
        }
    )
)


def expected_execution_order(warmups: int, trial_index: int) -> str:
    return (
        "baseline-first"
        if (warmups + trial_index) % 2 == 1
        else "chart-first"
    )


PHASE0_REPORT_ROUNDING_TOLERANCE_MS = Decimal("0.0021")


def validate_chart_phase0_instrumentation(
    row: Mapping[str, str], key: tuple[str, str]
) -> None:
    durations = {
        field: decimal_value(row.get(field, ""), f"{key} {field}")
        for field in (
            "cache_build_ms",
            "total_ms",
            "initial_chart_construction_ms",
            "materialization_ms",
            "materialization_exact_verification_ms",
            "materialization_accepted_update_ms",
            "materialization_final_compaction_ms",
        )
    }
    materialization_reason_sum = sum(
        (
            durations["materialization_exact_verification_ms"],
            durations["materialization_accepted_update_ms"],
            durations["materialization_final_compaction_ms"],
        ),
        Decimal(0),
    )
    if (
        abs(durations["materialization_ms"] - materialization_reason_sum)
        > PHASE0_REPORT_ROUNDING_TOLERANCE_MS
    ):
        fail(
            "materialization total is not the rounded sum of its three "
            f"disjoint reason buckets: {key}"
        )
    if (
        durations["initial_chart_construction_ms"]
        > durations["cache_build_ms"] + PHASE0_REPORT_ROUNDING_TOLERANCE_MS
    ):
        fail(f"initial chart construction exceeds cache build time: {key}")
    if (
        durations["materialization_ms"]
        > durations["total_ms"] + PHASE0_REPORT_ROUNDING_TOLERANCE_MS
    ):
        fail(f"materialization time exceeds total run time: {key}")

    exact_text = row.get("exact_verifications", "")
    peak_text = row.get("peak_concurrent_exact_verifiers", "")
    if not re.fullmatch(r"[0-9]+", exact_text) or not re.fullmatch(
        r"[0-9]+", peak_text
    ):
        fail(f"exact-verifier concurrency fields are not unsigned integers: {key}")
    exact_verifications = int(exact_text)
    peak = int(peak_text)
    if exact_verifications == 0:
        if peak != 0:
            fail(f"zero exact verifications reported a nonzero peak: {key}")
    elif peak < 1 or peak > exact_verifications:
        fail(
            "peak exact-verifier concurrency contradicts the verification "
            f"count: {key}: peak={peak}, exact={exact_verifications}"
        )

    admission = {
        field: require_unsigned_text(row.get(field, ""), f"{key} {field}")
        for field in EXACT_CANDIDATE_ADMISSION_FIELDS[:-1]
    }
    queued_for_memory = decimal_value(
        row.get(EXACT_CANDIDATE_ADMISSION_FIELDS[-1], ""),
        f"{key} {EXACT_CANDIDATE_ADMISSION_FIELDS[-1]}",
    )
    budget = require_unsigned_text(
        row.get("configured_chart_memory_budget", ""),
        f"{key} configured_chart_memory_budget",
    )
    batches = admission["exact_candidate_admission_batches"]
    parallel = admission["exact_candidate_parallel_batches"]
    inner = admission["exact_candidate_inner_parallel_batches"]
    memory_limited = admission["exact_candidate_memory_limited_batches"]
    admitted = admission["exact_candidate_peak_admitted_bytes"]
    projected = admission["exact_candidate_peak_projected_resident_bytes"]
    if (
        parallel > batches
        or inner > batches
        or memory_limited > batches
        or parallel + inner > batches
    ):
        fail(f"exact-candidate admission batch accounting is inconsistent: {key}")
    if admitted > projected:
        fail(f"exact-candidate admitted bytes exceed projected resident bytes: {key}")
    if batches == 0 and any(
        (parallel, inner, memory_limited, admitted, projected, queued_for_memory)
    ):
        fail(f"zero exact-candidate batches reported nonzero admission evidence: {key}")
    if batches > 0 and (admitted == 0 or projected == 0):
        fail(f"exact-candidate batches lack peak byte evidence: {key}")
    if exact_verifications > 0 and batches == 0:
        fail(f"exact verifications lack an admission batch: {key}")
    if memory_limited == 0 and queued_for_memory != 0:
        fail(f"unlimited exact-candidate batches report memory-queue time: {key}")
    if budget != 0 and projected > budget:
        fail(
            "exact-candidate projected resident bytes exceed the configured "
            f"memory budget: {key}: projected={projected}, budget={budget}"
        )


def validate_successful_row(
    capture_root: Path,
    stage_dir: Path,
    capture: CaptureSpec,
    row: Mapping[str, str],
    key: tuple[str, str],
) -> None:
    if row.get("validation_status") != "ok":
        fail(f"successful row did not validate: {capture.capture_id}: {key}")
    validate_standard_process_contract(capture, row, "ok", key)
    validate_successful_raw_domains(capture_root, capture, row, key)
    for field in (
        "output_semantic_sha256",
        "trial_semantic_sha256",
        "canonical_argv_sha256",
    ):
        if not re.fullmatch(r"[0-9a-f]{64}", row.get(field, "")):
            fail(f"successful row lacks {field}: {capture.capture_id}: {key}")
    if key[0] != "sample_explore_merge":
        validate_chart_phase0_instrumentation(row, key)
        search = row.get("search_semantic_sha256", "")
        if not re.fullmatch(r"[0-9a-f]{64}", search):
            fail(f"successful chart row lacks semantic digest: {key}")
        validate_defining_report(stage_dir, capture, key[0], key[1], row)


def validate_repeated_unanimity(
    capture_root: Path,
    key: tuple[str, str],
    rows: Sequence[Mapping[str, str]],
    *,
    stage_dir: Path | None = None,
) -> None:
    if len(rows) < 2:
        return
    first = rows[0]
    for field in REPEATED_UNANIMOUS_RAW_FIELDS:
        expected = first.get(field)
        for row in rows[1:]:
            if row.get(field) != expected:
                fail(
                    f"repeated successful key {key} disagrees on {field}: "
                    f"{expected!r} != {row.get(field)!r}"
                )
    if key[0] == "sample_explore_merge":
        return
    reports: list[Path] = []
    for row in rows:
        report = Path(row.get("report_path", "")).resolve(strict=True)
        try:
            report.relative_to(capture_root.resolve(strict=True))
        except ValueError:
            fail(f"repeated report escapes capture root: {report}")
        if stage_dir is not None and report.parent != (
            stage_dir.resolve(strict=True) / "logs"
        ):
            fail(f"repeated report escapes its exact stage: {report}: {stage_dir}")
        reports.append(report)
    for field in REPEATED_UNANIMOUS_REPORT_FIELDS:
        expected = report_value(reports[0], field)
        for report in reports[1:]:
            actual = report_value(report, field)
            if actual != expected:
                fail(
                    f"repeated successful key {key} report contract disagrees "
                    f"on {field}: {expected!r} != {actual!r}: {report}"
                )
    expected_stop_reason = report_stop_reason(reports[0])
    for report in reports[1:]:
        actual_stop_reason = report_stop_reason(report)
        if actual_stop_reason != expected_stop_reason:
            fail(
                f"repeated successful key {key} report contract disagrees on "
                f"stop_reason: {expected_stop_reason!r} != "
                f"{actual_stop_reason!r}: {report}"
            )


UNAVAILABLE_SEMANTIC_FIELDS = (
    "search_semantic_sha256",
    "output_semantic_sha256",
    "trial_semantic_sha256",
    "canonical_digest",
)


def validate_unavailable_semantic_sentinels(
    row: Mapping[str, str], label: str
) -> None:
    for field in UNAVAILABLE_SEMANTIC_FIELDS:
        if row.get(field) != "-":
            fail(f"{label} claims unavailable semantic field {field}")


RAW_PROCESS_METRIC_BINDINGS = (
    ("runner_outcome", "outcome"),
    ("runner_exit_code", "runner_exit_code"),
    ("exit_code", "exit_code"),
    ("term_signal", "term_signal"),
    ("core_dumped", "core_dumped"),
    ("timed_out", "timed_out"),
    ("monitor_error", "monitor_error"),
    ("wall_clock_s", "wall_seconds"),
    ("user_cpu_s", "user_seconds"),
    ("system_cpu_s", "system_seconds"),
    ("max_rss_kb", "max_rss_kb"),
    ("peak_sampled_rss_kb", "peak_sampled_rss_kb"),
    ("peak_sampled_swap_kb", "peak_sampled_swap_kb"),
    ("process_rss_limit_bytes", "rss_limit_bytes"),
    ("rss_limit_enabled", "rss_limit_enabled"),
    ("rss_limit_observed", "rss_limit_observed"),
    ("rss_limit_exceeded", "rss_limit_exceeded"),
    ("rss_limit_trigger_bytes", "rss_limit_trigger_bytes"),
    ("rss_limit_term_sent", "rss_limit_term_sent"),
    ("rss_limit_kill_sent", "rss_limit_kill_sent"),
)


def validate_raw_process_metric_bindings(
    row: Mapping[str, str], metrics: Mapping[str, str], label: str
) -> None:
    for raw_field, metric_field in RAW_PROCESS_METRIC_BINDINGS:
        if row.get(raw_field) != metrics.get(metric_field):
            fail(
                f"{label} raw/process-metrics {raw_field}/{metric_field} "
                f"disagree: {row.get(raw_field)!r} != "
                f"{metrics.get(metric_field)!r}"
            )


def require_stage_log_file(path: Path, stage_dir: Path, label: str) -> Path:
    require_exact_regular_file(path, label)
    resolved = path.resolve(strict=True)
    logs = (stage_dir.resolve(strict=True) / "logs").resolve(strict=True)
    if not path.is_absolute() or resolved != path or path.parent != logs:
        fail(f"{label} is not its exact lexical stage-local file: {path}")
    return path


def timed_process_metrics_path(
    stage_dir: Path, row: Mapping[str, str], key: tuple[str, str]
) -> Path:
    report = Path(row.get("report_path", ""))
    if key[0] == "sample_explore_merge":
        metrics = report.with_suffix(".out.process_metrics")
    else:
        metrics = Path(str(report) + ".process_metrics")
    return require_stage_log_file(
        metrics, stage_dir, f"timed process metrics for {key}"
    )


def require_successful_auxiliary_process(
    path: Path, rss_limit_bytes: int, label: str
) -> dict[str, str]:
    metrics = read_process_metrics(path, 0, rss_limit_bytes)
    if (
        metrics["outcome"] != "exited"
        or metrics["runner_exit_code"] != "0"
        or metrics["exit_code"] != "0"
        or metrics["term_signal"] != "0"
        or metrics["core_dumped"] != "0"
        or metrics["timed_out"] != "0"
        or metrics["timeout_term_sent"] != "0"
        or metrics["timeout_kill_sent"] != "0"
        or metrics["monitor_error"] != "0"
        or metrics["rss_limit_observed"] != "0"
        or metrics["rss_limit_exceeded"] != "0"
        or metrics["rss_limit_trigger_bytes"] != "0"
        or metrics["rss_limit_term_sent"] != "0"
        or metrics["rss_limit_kill_sent"] != "0"
    ):
        fail(f"{label} did not complete as a clean capped process: {path}")
    validate_observed_rss_within_cap(metrics, rss_limit_bytes, label)
    return metrics


def dag_info_unsigned_value(path: Path, field: str) -> str:
    pattern = re.compile(rf"^{re.escape(field)}:\s*([0-9]+)\s*$")
    values = [
        match.group(1)
        for line in path.read_text(encoding="utf-8").splitlines()
        if (match := pattern.fullmatch(line)) is not None
    ]
    if len(values) != 1:
        fail(f"DAG-info field {field!r} occurs {len(values)} times: {path}")
    return values[0]


def dag_info_parsimony_min(path: Path) -> str:
    pattern = re.compile(r"^parsimony_min:\s*score:([0-9]+),\s*count:([0-9]+)\s*$")
    values = [
        (match.group(1), match.group(2))
        for line in path.read_text(encoding="utf-8").splitlines()
        if (match := pattern.fullmatch(line)) is not None
    ]
    if len(values) != 1 or int(values[0][1]) <= 0:
        fail(f"DAG-info parsimony minimum is not singular/nonempty: {path}")
    return values[0][0]


CANONICAL_DAG_RESULT_FIELDS = {
    "schema",
    "schema_version",
    "digest_algorithm",
    "semantic_sha256",
    "clades_sha256",
    "productions_sha256",
    "clade_count",
    "production_count",
    "parsimony_min",
}


def read_canonical_dag_result(path: Path, label: str) -> dict[str, object]:
    require_exact_regular_file(path, label)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{label} is not parseable: {path}: {error}")
    if not isinstance(value, dict) or set(value) != CANONICAL_DAG_RESULT_FIELDS:
        fail(f"{label} has an open or incomplete key set: {path}")
    if (
        value.get("schema") != "larch.dag.semantic_digest"
        or value.get("schema_version") != 1
        or value.get("digest_algorithm") != "sha256"
    ):
        fail(f"{label} schema changed: {path}")
    for field in ("semantic_sha256", "clades_sha256", "productions_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", str(value.get(field, ""))):
            fail(f"{label} has an invalid {field}: {path}")
    for field in ("clade_count", "production_count", "parsimony_min"):
        field_value = value.get(field)
        if (
            isinstance(field_value, bool)
            or not isinstance(field_value, int)
            or field_value < 0
        ):
            fail(f"{label} has an invalid {field}: {path}")
    return value


def score_artifact_prefix(
    stage_dir: Path,
    capture: CaptureSpec,
    row: Mapping[str, str],
    key: tuple[str, str],
) -> Path:
    fixture = re.sub(r"[^A-Za-z0-9_.-]", "_", RAW_FIXTURE_LABELS[capture.fixture])
    trial = require_unsigned_text(
        row.get("trial_index", ""), f"{key} score trial index", positive=True
    )
    suffix = f"{fixture}_{key[0]}_score_trial{trial}"
    if key[0] != "sample_explore_merge":
        worker = re.sub(r"[^A-Za-z0-9_.-]", "_", key[1])
        suffix += f"_w{worker}"
    return stage_dir.resolve(strict=True) / "logs" / suffix


def validate_score_artifacts(
    stage_dir: Path,
    capture: CaptureSpec,
    row: Mapping[str, str],
    key: tuple[str, str],
    *,
    prefix: Path | None = None,
    bind_shape: bool = True,
) -> None:
    prefix = score_artifact_prefix(stage_dir, capture, row, key) if prefix is None else prefix
    score_out = require_stage_log_file(
        Path(str(prefix) + ".out"), stage_dir, f"score stdout for {key}"
    )
    require_stage_log_file(
        Path(str(prefix) + ".err"), stage_dir, f"score stderr for {key}"
    )
    score_metrics = require_stage_log_file(
        Path(str(prefix) + ".out.process_metrics"),
        stage_dir,
        f"score process metrics for {key}",
    )
    require_successful_auxiliary_process(
        score_metrics, capture.rss_limit_bytes, f"score oracle for {key}"
    )
    canonical = read_canonical_dag_result(
        require_stage_log_file(
            Path(str(prefix) + ".canonical-dag.json"),
            stage_dir,
            f"score canonical DAG for {key}",
        ),
        f"score canonical DAG for {key}",
    )
    score = dag_info_parsimony_min(score_out)
    nodes = dag_info_unsigned_value(score_out, "nodes")
    edges = dag_info_unsigned_value(score_out, "edges")
    if (
        str(canonical["semantic_sha256"])
        != row.get("output_semantic_sha256")
        or str(canonical["parsimony_min"])
        != row.get("final_validated_parsimony_min")
        or score != row.get("final_validated_parsimony_min")
    ):
        fail(f"score oracle semantic/parsimony differs from raw row {key}")
    if bind_shape and (
        nodes != row.get("final_dag_nodes")
        or edges != row.get("final_dag_edges")
    ):
        fail(f"score oracle nodes/edges differ from raw row {key}")


def companion_stem(capture: CaptureSpec, key: tuple[str, str]) -> str:
    fixture = RAW_FIXTURE_LABELS[capture.fixture]
    safe_fixture = re.sub(r"[^A-Za-z0-9_.-]", "_", fixture)
    raw_row_id = f"{fixture}/{key[0]}@{key[1]}"
    safe_row = re.sub(r"[^A-Za-z0-9_.-]", "_", raw_row_id)
    return f"{safe_fixture}_{safe_row}"


def exact_canonical_asset_pair(
    capture_root: Path, capture: CaptureSpec, key: tuple[str, str]
) -> tuple[Path, Path]:
    logs = capture_root.resolve(strict=True) / "characterization" / "logs"
    prefix = logs / f"{companion_stem(capture, key)}_full_canonical"
    sidecar = require_stage_log_file(
        Path(str(prefix) + ".ndjson"),
        capture_root / "characterization",
        f"full canonical sidecar for {key}",
    )
    report = require_stage_log_file(
        Path(str(prefix) + ".json"),
        capture_root / "characterization",
        f"full canonical report for {key}",
    )
    return sidecar, report


CHART_SEARCH_DIGEST_FIELDS = frozenset(
    {
        "schema",
        "schema_version",
        "digest_algorithm",
        "payload_encoding",
        "semantic_sha256",
        "contract_sha256",
        "candidates_sha256",
        "exact_sha256",
        "acceptance_sha256",
        "chain_sha256",
        "final_topology_sha256",
        "record_count",
        "candidate_count",
        "exact_candidate_count",
        "iteration_count",
    }
)
CHART_SEARCH_DIGEST_SHA_FIELDS = (
    "semantic_sha256",
    "contract_sha256",
    "candidates_sha256",
    "exact_sha256",
    "acceptance_sha256",
    "chain_sha256",
    "final_topology_sha256",
)
CHART_SEARCH_DIGEST_COUNT_FIELDS = (
    "record_count",
    "candidate_count",
    "exact_candidate_count",
    "iteration_count",
)
MAX_CHART_SEARCH_DIGEST_BYTES = 64 * 1024


def parse_chart_search_digest_bytes(
    data: bytes, path: Path, label: str
) -> dict[str, object]:
    def closed_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                fail(f"{label} has duplicate JSON key {key!r}: {path}")
            result[key] = value
        return result

    def reject_nonfinite(value: str) -> object:
        raise BootstrapError(
            f"{label} has non-finite JSON constant {value!r}: {path}"
        )

    try:
        value = json.loads(
            data.decode("utf-8"),
            object_pairs_hook=closed_object,
            parse_constant=reject_nonfinite,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not strict UTF-8 JSON: {path}: {error}")
    if not isinstance(value, dict) or set(value) != CHART_SEARCH_DIGEST_FIELDS:
        fail(f"{label} has an open or incomplete key set: {path}")
    literals = {
        "schema": "larch.chart_spr.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
        "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
    }
    for field, expected in literals.items():
        if value[field] != expected or type(value[field]) is not type(expected):
            fail(f"{label} schema changed at {field}: {path}")
    for field in CHART_SEARCH_DIGEST_SHA_FIELDS:
        digest = value[field]
        if (
            not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            fail(f"{label} has an invalid {field}: {path}")
    for field in CHART_SEARCH_DIGEST_COUNT_FIELDS:
        count = value[field]
        if type(count) is not int or count < 0:
            fail(f"{label} has an invalid {field}: {path}")
    if value["record_count"] == 0:
        fail(f"{label} has an empty canonical record stream: {path}")
    return {str(field): item for field, item in value.items()}


def compact_file_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def compact_directory_identity(info: os.stat_result) -> tuple[int, int, int]:
    return info.st_dev, info.st_ino, info.st_mode


def read_timed_chart_compact(
    path: Path, stage_dir: Path, label: str
) -> tuple[dict[str, object], bytes, tuple[int, int]]:
    """Read an exact stage-local compact through a stable no-follow descriptor."""

    stage = stage_dir.resolve(strict=True)
    logs = stage / "logs"
    try:
        logs_info = logs.lstat()
        logs_resolved = logs.resolve(strict=True)
    except OSError as error:
        fail(f"{label} logs directory is unavailable: {logs}: {error}")
    if not stat.S_ISDIR(logs_info.st_mode) or logs_resolved != logs:
        fail(f"{label} logs directory is not a canonical directory: {logs}")
    if not path.is_absolute() or path.absolute() != path or path.parent != logs:
        fail(f"{label} is not its exact lexical stage-local file: {path}")

    directory_descriptor = -1
    descriptor = -1
    try:
        directory_descriptor = os.open(
            logs,
            os.O_RDONLY
            | os.O_DIRECTORY
            | os.O_NOFOLLOW
            | getattr(os, "O_CLOEXEC", 0),
        )
        opened_logs = os.fstat(directory_descriptor)
        if compact_directory_identity(opened_logs) != compact_directory_identity(
            logs_info
        ):
            fail(f"{label} logs directory changed while it was opened: {logs}")
        try:
            before = os.stat(
                path.name, dir_fd=directory_descriptor, follow_symlinks=False
            )
        except FileNotFoundError:
            fail(f"{label} is missing: {path}")
        if not stat.S_ISREG(before.st_mode):
            fail(f"{label} must be a lexical regular file, not an alias: {path}")
        if before.st_nlink != 1:
            fail(f"{label} is not singly linked: {path}")
        if before.st_size > MAX_CHART_SEARCH_DIGEST_BYTES:
            fail(
                f"{label} exceeds {MAX_CHART_SEARCH_DIGEST_BYTES} bytes: {path}"
            )

        descriptor = os.open(
            path.name,
            os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0),
            dir_fd=directory_descriptor,
        )
        opened = os.fstat(descriptor)
        if compact_file_signature(opened) != compact_file_signature(before):
            fail(f"{label} changed while its descriptor was opened: {path}")
        chunks: list[bytes] = []
        byte_count = 0
        while True:
            chunk = os.read(descriptor, 64 * 1024)
            if not chunk:
                break
            byte_count += len(chunk)
            if byte_count > MAX_CHART_SEARCH_DIGEST_BYTES:
                fail(
                    f"{label} exceeds {MAX_CHART_SEARCH_DIGEST_BYTES} bytes: {path}"
                )
            chunks.append(chunk)

        opened_after = os.fstat(descriptor)
        after = os.stat(
            path.name, dir_fd=directory_descriptor, follow_symlinks=False
        )
        lexical_logs_after = logs.lstat()
        lexical_logs_resolved = logs.resolve(strict=True)
        if (
            compact_directory_identity(opened_logs)
            != compact_directory_identity(lexical_logs_after)
            or lexical_logs_resolved != logs
        ):
            fail(f"{label} logs directory changed while it was read: {logs}")
        if (
            compact_file_signature(opened_after)
            != compact_file_signature(opened)
            or compact_file_signature(after) != compact_file_signature(opened)
        ):
            fail(f"{label} changed while it was read: {path}")
        data = b"".join(chunks)
        if len(data) != opened.st_size:
            fail(f"{label} byte count changed while it was read: {path}")
    except OSError as error:
        fail(f"cannot descriptor-read {label}: {path}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if directory_descriptor >= 0:
            os.close(directory_descriptor)
    mapping = parse_chart_search_digest_bytes(data, path, label)
    return mapping, data, (opened.st_dev, opened.st_ino)


def register_unique_chart_compact(
    path: Path,
    identity: tuple[int, int],
    paths: set[Path],
    identities: dict[tuple[int, int], Path],
    label: str,
) -> None:
    if path in paths:
        fail(f"capture stage reuses a chart compact path for {label}: {path}")
    if identity in identities:
        fail(
            f"capture stage aliases chart compacts for {label}: "
            f"{path} and {identities[identity]}"
        )
    paths.add(path)
    identities[identity] = path


def measured_chart_compact_path(row: Mapping[str, str]) -> Path:
    return Path(row.get("report_path", "")).with_suffix(".canonical.json")


def warmup_chart_compact_path(
    stage_dir: Path,
    capture: CaptureSpec,
    key: tuple[str, str],
    warmup_index: int,
) -> Path:
    if warmup_index <= 0:
        fail("chart compact warmup index must be positive")
    fixture = re.sub(
        r"[^A-Za-z0-9_.-]", "_", RAW_FIXTURE_LABELS[capture.fixture]
    )
    worker = re.sub(r"[^A-Za-z0-9_.-]", "_", key[1])
    return (
        stage_dir.resolve(strict=True)
        / "logs"
        / f"{fixture}_{key[0]}_warmup{warmup_index}_w{worker}.canonical.json"
    )


def require_chart_compact_match(
    mapping: Mapping[str, object],
    data: bytes,
    expected_mapping: Mapping[str, object],
    expected_data: bytes,
    label: str,
) -> None:
    if mapping != expected_mapping or data != expected_data:
        fail(f"{label} differs from its row/full-sidecar canonical oracle")


def validate_chart_semantic_artifacts(
    capture_root: Path,
    stage_dir: Path,
    capture: CaptureSpec,
    row: Mapping[str, str],
    key: tuple[str, str],
    timed_mapping: Mapping[str, object],
    timed_data: bytes,
) -> tuple[dict[str, object], bytes]:
    stem = companion_stem(capture, key)
    compact_prefix = stage_dir.resolve(strict=True) / "logs" / f"{stem}_canonical_companion"
    compact = require_stage_log_file(
        Path(str(compact_prefix) + ".json"),
        stage_dir,
        f"compact semantic report for {key}",
    )
    compact_out = require_stage_log_file(
        Path(str(compact_prefix) + ".out"),
        stage_dir,
        f"compact semantic stdout for {key}",
    )
    require_stage_log_file(
        Path(str(compact_prefix) + ".err"),
        stage_dir,
        f"compact semantic stderr for {key}",
    )
    compact_metrics = require_stage_log_file(
        Path(str(compact_prefix) + ".out.process_metrics"),
        stage_dir,
        f"compact semantic process metrics for {key}",
    )
    require_successful_auxiliary_process(
        compact_metrics,
        capture.rss_limit_bytes,
        f"compact semantic companion for {key}",
    )
    sidecar, full_report = exact_canonical_asset_pair(capture_root, capture, key)
    compact_mapping, compact_data, _ = read_timed_chart_compact(
        compact,
        stage_dir,
        f"compact semantic report for {key}",
    )
    full_mapping, full_data, _ = read_timed_chart_compact(
        full_report,
        capture_root / "characterization",
        f"full canonical report for {key}",
    )
    if compact_mapping != full_mapping or compact_data != full_data:
        fail(f"compact/full canonical digest bytes differ for {key}")
    validate_canonical_sidecar_source(
        capture_root,
        (
            sidecar.relative_to(capture_root).as_posix(),
            full_report.relative_to(capture_root).as_posix(),
        ),
        f"{key[0]}@{key[1]}",
        stable_digest_mapping=full_mapping,
    )
    require_chart_compact_match(
        timed_mapping,
        timed_data,
        compact_mapping,
        compact_data,
        f"timed compact semantic report for {key}",
    )
    if compact_mapping.get("semantic_sha256") != row.get(
        "search_semantic_sha256"
    ):
        fail(f"row-local compact semantic digest differs from raw row {key}")

    full_prefix = full_report.with_suffix("")
    require_stage_log_file(
        Path(str(full_prefix) + ".out"),
        capture_root / "characterization",
        f"full canonical stdout for {key}",
    )
    require_stage_log_file(
        Path(str(full_prefix) + ".err"),
        capture_root / "characterization",
        f"full canonical stderr for {key}",
    )
    full_metrics = require_stage_log_file(
        Path(str(full_prefix) + ".out.process_metrics"),
        capture_root / "characterization",
        f"full canonical process metrics for {key}",
    )
    require_successful_auxiliary_process(
        full_metrics,
        capture.rss_limit_bytes,
        f"full canonical companion for {key}",
    )
    companion_score_prefix = Path(str(compact_out.with_suffix("")) + ".score")
    validate_score_artifacts(
        stage_dir,
        capture,
        row,
        key,
        prefix=companion_score_prefix,
        bind_shape=False,
    )
    return compact_mapping, compact_data


def validate_stage_artifact_bindings(
    capture_root: Path,
    stage_dir: Path,
    capture: CaptureSpec,
    rows: Sequence[Mapping[str, str]],
    *,
    warmups: int,
) -> None:
    safe_fixture = re.sub(
        r"[^A-Za-z0-9_.-]", "_", RAW_FIXTURE_LABELS[capture.fixture]
    )
    initial_out = require_stage_log_file(
        stage_dir.resolve(strict=True) / "logs" / f"{safe_fixture}_initial_score.out",
        stage_dir,
        "stage initial-score stdout",
    )
    require_stage_log_file(
        stage_dir.resolve(strict=True) / "logs" / f"{safe_fixture}_initial_score.err",
        stage_dir,
        "stage initial-score stderr",
    )
    initial_metrics = require_stage_log_file(
        stage_dir.resolve(strict=True)
        / "logs"
        / f"{safe_fixture}_initial_score.out.process_metrics",
        stage_dir,
        "stage initial-score process metrics",
    )
    require_successful_auxiliary_process(
        initial_metrics, capture.rss_limit_bytes, "stage initial-score oracle"
    )
    initial_score = validate_initial_score_unanimity(
        rows, f"stage artifact binding {stage_dir}"
    )
    if dag_info_parsimony_min(initial_out) != initial_score:
        fail(f"stage initial-score stdout disagrees with raw rows: {stage_dir}")

    seen_metrics: set[Path] = set()
    seen_compact_paths: set[Path] = set()
    seen_compact_identities: dict[tuple[int, int], Path] = {}
    chart_oracles: dict[
        tuple[str, str], tuple[dict[str, object], bytes, Mapping[str, str]]
    ] = {}
    for row in rows:
        key = raw_key(row)
        metrics_path = timed_process_metrics_path(stage_dir, row, key)
        if metrics_path in seen_metrics:
            fail(f"capture stage reuses timed process metrics: {stage_dir}: {key}")
        seen_metrics.add(metrics_path)
        runner_status = require_unsigned_text(
            row.get("runner_exit_code", ""), f"{key} runner exit code"
        )
        metrics = read_process_metrics(
            metrics_path, runner_status, capture.rss_limit_bytes
        )
        validate_raw_process_metric_bindings(row, metrics, f"timed row {key}")
        if row.get("status") != "ok":
            continue
        validate_score_artifacts(stage_dir, capture, row, key)
        if key[0] != "sample_explore_merge":
            timed_path = measured_chart_compact_path(row)
            timed_mapping, timed_data, timed_identity = read_timed_chart_compact(
                timed_path,
                stage_dir,
                f"timed chart compact for {key} trial {row.get('trial_index', '')}",
            )
            register_unique_chart_compact(
                timed_path,
                timed_identity,
                seen_compact_paths,
                seen_compact_identities,
                f"{key} trial {row.get('trial_index', '')}",
            )
            oracle_mapping, oracle_data = validate_chart_semantic_artifacts(
                capture_root,
                stage_dir,
                capture,
                row,
                key,
                timed_mapping,
                timed_data,
            )
            prior = chart_oracles.get(key)
            if prior is not None:
                require_chart_compact_match(
                    oracle_mapping,
                    oracle_data,
                    prior[0],
                    prior[1],
                    f"repeated chart compact oracle for {key}",
                )
            else:
                chart_oracles[key] = (oracle_mapping, oracle_data, row)

    for key, (oracle_mapping, oracle_data, row) in chart_oracles.items():
        for warmup_index in range(1, warmups + 1):
            warmup_path = warmup_chart_compact_path(
                stage_dir, capture, key, warmup_index
            )
            mapping, data, identity = read_timed_chart_compact(
                warmup_path,
                stage_dir,
                f"chart warmup compact for {key} warmup {warmup_index}",
            )
            register_unique_chart_compact(
                warmup_path,
                identity,
                seen_compact_paths,
                seen_compact_identities,
                f"{key} warmup {warmup_index}",
            )
            require_chart_compact_match(
                mapping,
                data,
                oracle_mapping,
                oracle_data,
                f"chart warmup compact for {key} warmup {warmup_index}",
            )
            if mapping["semantic_sha256"] != row.get("search_semantic_sha256"):
                fail(
                    "chart warmup compact semantic digest differs from raw row "
                    f"{key}"
                )


def validate_stage(
    capture_root: Path,
    stage_dir: Path,
    capture: CaptureSpec,
    ordered_chart_keys: Sequence[tuple[str, str]],
    *,
    include_native: bool,
    warmups: int,
    repetitions: int,
    allowed_timeout_keys: Mapping[tuple[str, str], str],
) -> tuple[list[dict[str, str]], bool]:
    rows = read_raw_trials(stage_dir / "raw_trials.tsv")
    validate_initial_score_unanimity(rows, f"capture stage {stage_dir}")
    validate_stage_artifact_paths(
        capture_root, stage_dir, rows, RAW_FIXTURE_LABELS[capture.fixture]
    )
    native_key = ("sample_explore_merge", "native")
    ordered_keys = ([native_key] if include_native else []) + list(
        ordered_chart_keys
    )
    expected_keys = set(ordered_keys)
    if len(expected_keys) != len(ordered_keys):
        fail(f"capture stage has duplicate planned keys: {stage_dir}")
    expected_stream: list[tuple[tuple[str, str], str, str]] = []
    for trial in range(1, repetitions + 1):
        order = expected_execution_order(warmups, trial)
        trial_keys = (
            ordered_keys
            if order == "baseline-first"
            else [*ordered_chart_keys, *([native_key] if include_native else [])]
        )
        expected_stream.extend((key, str(trial), order) for key in trial_keys)
    observed_stream = [
        (raw_key(row), row.get("trial_index", ""), row.get("execution_order", ""))
        for row in rows
    ]
    if observed_stream != expected_stream:
        fail(
            f"capture stage trial/order stream changed: {stage_dir}: "
            f"observed={observed_stream}, expected={expected_stream}"
        )
    grouped: dict[tuple[str, str], list[dict[str, str]]] = {
        key: [] for key in expected_keys
    }
    any_timeout = False
    for row in rows:
        key = raw_key(row)
        grouped[key].append(row)
        status = row.get("status", "")
        validate_raw_trial_binding(capture, row, key, status)
        if status == "timeout":
            if warmups != 0 or repetitions != 1:
                fail(
                    "expected timeout is permitted only in the single recorded "
                    f"characterization trial: {stage_dir}: {key}"
                )
            if allowed_timeout_keys.get(key) is None:
                fail(
                    "capture produced an unplanned timeout without an exact "
                    f"row confirmation: {capture.capture_id}: {key}"
                )
            any_timeout = True
            if row.get("validation_status") != "not_run":
                fail(f"timeout row has unexpected validation state: {key}")
            validate_standard_process_contract(capture, row, "timeout", key)
            validate_unavailable_semantic_sentinels(
                row, f"timeout row {key}"
            )
        elif status == "ok":
            validate_successful_row(capture_root, stage_dir, capture, row, key)
        else:
            fail(
                f"capture row is neither ok nor timeout ({status!r}): "
                f"{stage_dir}: {key}"
            )
    for key, key_rows in grouped.items():
        if len(key_rows) != repetitions:
            fail(
                f"capture stage key {key} has {len(key_rows)}/{repetitions} rows: "
                f"{stage_dir}"
            )
        successes = [row for row in key_rows if row["status"] == "ok"]
        if successes:
            if len(successes) != len(key_rows):
                fail(f"capture key mixes success and timeout: {stage_dir}: {key}")
            validate_repeated_unanimity(
                capture_root, key, successes, stage_dir=stage_dir
            )
    validate_stage_artifact_bindings(
        capture_root, stage_dir, capture, rows, warmups=warmups
    )
    return rows, any_timeout


def write_tsv_exclusive(
    path: Path, fields: Sequence[str], rows: Sequence[Mapping[str, str]]
) -> None:
    if not fields or len(fields) != len(set(fields)):
        fail(f"cannot write TSV with invalid fields: {path}")
    lines = ["\t".join(fields)]
    for row in rows:
        if set(row) != set(fields):
            fail(f"cannot write TSV row with a different schema: {path}")
        values = [row[field] for field in fields]
        if any("\t" in value or "\n" in value or "\r" in value for value in values):
            fail(f"cannot write TSV row containing a control delimiter: {path}")
        lines.append("\t".join(values))
    write_text_exclusive(path, "\n".join(lines) + "\n", 0o444)


def decimal_value(value: str, label: str) -> Decimal:
    try:
        parsed = Decimal(value)
    except Exception as error:
        fail(f"{label} is not a decimal: {value!r}: {error}")
    if not parsed.is_finite() or parsed < 0:
        fail(f"{label} is not a finite nonnegative decimal: {value!r}")
    return parsed


def decimal_median(values: Sequence[Decimal]) -> Decimal:
    if not values:
        fail("cannot compute a median of an empty sequence")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / Decimal(2)


def fixed_decimal(value: Decimal, places: str) -> str:
    quantum = Decimal(places)
    return format(value.quantize(quantum, rounding=ROUND_HALF_EVEN), "f")


SUMMARY_FIELDS = (
    "method",
    "requested_workers",
    "row_id",
    "evidence_kind",
    "measured_trial_count",
    "wall_clock_median_s",
    "wall_clock_max_s",
    "user_cpu_median_s",
    "system_cpu_median_s",
    "max_rss_max_kb",
    "peak_sampled_rss_max_kb",
    "paired_ratio_median",
)


def render_capture_summary(
    merged_rows: Sequence[Mapping[str, str]],
    spec_by_key: Mapping[tuple[str, str], RowSpec],
    paired_ratios: Mapping[tuple[str, str], Sequence[Decimal]],
) -> bytes:
    grouped: dict[tuple[str, str], list[Mapping[str, str]]] = {}
    for row in merged_rows:
        grouped.setdefault(raw_key(row), []).append(row)
    lines = ["\t".join(SUMMARY_FIELDS)]
    for key in sorted(grouped):
        key_rows = sorted(grouped[key], key=lambda row: int(row["trial_index"]))
        spec = spec_by_key.get(key)
        if spec is None:
            fail(f"canonical merged rows contain an unmanifested key: {key}")
        statuses = {row["status"] for row in key_rows}
        if statuses == {"timeout"}:
            if len(key_rows) != 1:
                fail(f"timeout characterization has multiple rows: {key}")
            values = (
                key[0],
                key[1],
                spec.row_id,
                "timeout_characterization_non_performance",
                "1",
                "-",
                "-",
                "-",
                "-",
                key_rows[0]["max_rss_kb"],
                key_rows[0]["peak_sampled_rss_kb"],
                "-",
            )
        elif statuses == {"ok"}:
            if len(key_rows) != spec.measured_trial_target:
                fail(
                    f"finite row {spec.row_id} has {len(key_rows)}/"
                    f"{spec.measured_trial_target} measured trials"
                )
            if len(key_rows) < 3:
                fail(f"finite row cannot claim an N=1/N=2 median: {spec.row_id}")
            walls = [
                decimal_value(row["wall_clock_s"], f"{key} wall")
                for row in key_rows
            ]
            users = [
                decimal_value(row["user_cpu_s"], f"{key} user CPU")
                for row in key_rows
            ]
            systems = [
                decimal_value(row["system_cpu_s"], f"{key} system CPU")
                for row in key_rows
            ]
            rss = [int(row["max_rss_kb"]) for row in key_rows]
            peak_rss = [int(row["peak_sampled_rss_kb"]) for row in key_rows]
            ratios = list(paired_ratios.get(key, ()))
            if key[0] != "sample_explore_merge" and len(ratios) != len(key_rows):
                fail(f"finite chart row lacks exact paired ratios: {key}")
            values = (
                key[0],
                key[1],
                spec.row_id,
                "finite_performance",
                str(len(key_rows)),
                fixed_decimal(decimal_median(walls), "0.000001"),
                fixed_decimal(max(walls), "0.000001"),
                fixed_decimal(decimal_median(users), "0.000001"),
                fixed_decimal(decimal_median(systems), "0.000001"),
                str(max(rss)),
                str(max(peak_rss)),
                (
                    "-"
                    if key[0] == "sample_explore_merge"
                    else fixed_decimal(decimal_median(ratios), "0.000000001")
                ),
            )
        else:
            fail(f"canonical row key mixes statuses: {key}: {sorted(statuses)}")
        lines.append("\t".join(values))
    return ("\n".join(lines) + "\n").encode("utf-8")


def parse_cpu_set(value: str) -> list[int]:
    result: set[int] = set()
    for piece in value.split(","):
        match = re.fullmatch(r"([0-9]+)(?:-([0-9]+))?", piece)
        if match is None:
            fail(f"invalid CPU-set syntax in frozen capture plan: {value!r}")
        first = int(match.group(1))
        last = first if match.group(2) is None else int(match.group(2))
        if last < first:
            fail(f"descending CPU range in frozen capture plan: {value!r}")
        result.update(range(first, last + 1))
    if not result:
        fail("frozen capture CPU set is empty")
    return sorted(result)


CAPTURE_ENVIRONMENT = {
    "HOME": "/nonexistent",
    "LANG": "C",
    "LC_ALL": "C",
    "PATH": "/usr/bin:/bin",
    "TMPDIR": "/tmp",
    "TZ": "Europe/Sofia",
}

LIVE_CPUFREQ_FIELDS = (
    "scaling_governor",
    "scaling_cur_freq",
    "scaling_min_freq",
    "scaling_max_freq",
    "energy_performance_preference",
    "boost",
)


def read_required_text(path: Path, label: str) -> str:
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        fail(f"cannot read required live evidence {label}: {path}: {error}")
    if not value:
        fail(f"required live evidence is empty for {label}: {path}")
    return value


def write_live_evidence(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    capture_root: Path,
    position: str,
) -> tuple[Path, Path]:
    if position not in ("pre", "post"):
        fail(f"invalid live-evidence position: {position}")
    raw_porcelain = subprocess.run(
        [
            "git",
            "status",
            "--porcelain=v2",
            "-z",
            "--untracked-files=all",
        ],
        cwd=metadata["repo_root"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    porcelain_path = capture_root / f"live-{position}.git-porcelain-v2-z"
    write_bytes_exclusive(porcelain_path, raw_porcelain, 0o444)
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=metadata["repo_root"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()
    if revision != FROZEN_REVISION:
        fail(f"live capture revision changed: {revision} != {FROZEN_REVISION}")
    loadavg = read_required_text(Path("/proc/loadavg"), "loadavg")
    meminfo: dict[str, str] = {}
    for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"(MemAvailable|SwapFree):\s+([0-9]+) kB", line)
        if match:
            meminfo[match.group(1)] = match.group(2)
    if set(meminfo) != {"MemAvailable", "SwapFree"}:
        fail(f"live memory evidence is incomplete: {meminfo}")
    target_cpus = parse_cpu_set(capture.affinity_cpus)
    online = set(parse_cpu_set(read_required_text(Path("/sys/devices/system/cpu/online"), "online CPUs")))
    if not set(target_cpus) <= online:
        fail(f"capture affinity contains offline CPUs: {capture.affinity_cpus}")
    cpufreq: dict[str, dict[str, str]] = {}
    for cpu in target_cpus:
        base = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
        values = {
            field: read_required_text(base / field, f"CPU {cpu} {field}")
            for field in LIVE_CPUFREQ_FIELDS
        }
        for field in ("scaling_cur_freq", "scaling_min_freq", "scaling_max_freq"):
            if not re.fullmatch(r"[1-9][0-9]*", values[field]):
                fail(f"live CPU frequency is invalid: cpu{cpu} {field}={values[field]!r}")
        if int(values["scaling_min_freq"]) > int(values["scaling_max_freq"]):
            fail(f"live CPU frequency bounds are inverted: cpu{cpu}")
        for field in ("scaling_governor", "energy_performance_preference"):
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", values[field]):
                fail(f"live CPU policy field is invalid: cpu{cpu} {field}")
        if values["boost"] not in ("0", "1"):
            fail(f"live per-CPU boost state is invalid: cpu{cpu}")
        cpufreq[str(cpu)] = values
    global_boost = read_required_text(
        Path("/sys/devices/system/cpu/cpufreq/boost"), "global CPU boost"
    )
    if global_boost not in ("0", "1"):
        fail(f"live global boost state is invalid: {global_boost!r}")
    # Bind all three clock representations to one instant.  JSON timestamps
    # are microsecond-precision, so truncate the nanosecond clock rather than
    # retaining sub-microsecond bits that cannot be recomputed from the text.
    sampled_time_ns = time.time_ns()
    sampled_seconds, sampled_remainder_ns = divmod(sampled_time_ns, 1_000_000_000)
    sampled_microseconds = sampled_remainder_ns // 1_000
    unix_time_ns = sampled_seconds * 1_000_000_000 + sampled_microseconds * 1_000
    now_utc = datetime.fromtimestamp(sampled_seconds, tz=timezone.utc).replace(
        microsecond=sampled_microseconds
    )
    now_local = now_utc.astimezone()
    evidence = {
        "schema": "wric_phase0_live_capture_environment",
        "schema_version": 1,
        "capture_id": capture.capture_id,
        "position": position,
        "timestamp_utc": now_utc.isoformat(timespec="microseconds"),
        "timestamp_local": now_local.isoformat(timespec="microseconds"),
        "unix_time_ns": unix_time_ns,
        "repo_revision": revision,
        "git_porcelain_format": "porcelain-v2-z",
        "git_porcelain_path": porcelain_path.name,
        "git_porcelain_sha256": sha256_bytes(raw_porcelain),
        "git_dirty": bool(raw_porcelain),
        "bootstrap_affinity_cpus": sorted(os.sched_getaffinity(0)),
        "target_affinity_kind": capture.affinity_kind,
        "target_affinity_cpus": capture.affinity_cpus,
        "target_affinity_cpu_list": target_cpus,
        "online_cpus": sorted(online),
        "loadavg_raw": loadavg,
        "mem_available_kb": int(meminfo["MemAvailable"]),
        "swap_free_kb": int(meminfo["SwapFree"]),
        "global_boost": global_boost,
        "cpufreq": cpufreq,
        "environment": CAPTURE_ENVIRONMENT
        | {"WRIC_REPO_ROOT": str(metadata["repo_root"])},
        "static_bindings": {
            "capture_metadata_sha256": sha256_file(paths["capture_metadata"]),
            "frozen_larch2_sha256": str(metadata["frozen_larch2_sha256"]),
            "frozen_oracle_sha256": str(metadata["frozen_oracle_sha256"]),
            "harness_sha256": str(metadata["harness_sha256"]),
            "process_metrics_sha256": str(metadata["process_metrics_sha256"]),
            "wrapper_calibration_sha256": str(
                metadata["wrapper_calibration_sha256"]
            ),
        },
    }
    evidence_path = capture_root / f"live-{position}.json"
    write_text_exclusive(
        evidence_path,
        json.dumps(evidence, sort_keys=True, indent=2) + "\n",
        0o444,
    )
    return evidence_path, porcelain_path


def ordered_capture_chart_keys(capture: CaptureSpec) -> list[tuple[str, str]]:
    return [
        (mode_method(mode), worker)
        for mode in capture.modes
        for worker in capture.workers
    ]


def run_stage_command(
    command: Sequence[str], stdout_path: Path, stderr_path: Path
) -> int:
    for path in (stdout_path, stderr_path):
        if path_occupied(path):
            fail(f"capture stage stream already exists: {path}")
    stdout_fd = os.open(stdout_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
    stderr_fd = os.open(stderr_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
    with os.fdopen(stdout_fd, "wb") as stdout_stream, os.fdopen(
        stderr_fd, "wb"
    ) as stderr_stream:
        result = subprocess.run(
            command, stdout=stdout_stream, stderr=stderr_stream, check=False
        )
    return result.returncode


def repeat_stage_command(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    key: tuple[str, str],
    target: int,
) -> tuple[Path, list[str]]:
    if target < 3:
        fail(f"finite repeat target is below three: {key}: {target}")
    stage_dir = (
        paths["captures"]
        / capture.capture_id
        / "repeats"
        / raw_key_slug(key)
    )
    if key == ("sample_explore_merge", "native"):
        command = capture_command(
            metadata,
            paths,
            capture,
            out_dir=stage_dir,
            warmups=1,
            repetitions=target - 1,
            native_only=True,
            full_canonical=False,
        )
    else:
        command = capture_command(
            metadata,
            paths,
            capture,
            out_dir=stage_dir,
            modes=(method_mode(key[0]),),
            workers=(key[1],),
            warmups=1,
            repetitions=target - 1,
            full_canonical=False,
        )
    return stage_dir, command


PAIR_PEER_FIELDS = (
    "paired_method",
    "paired_requested_workers",
    "peer_role",
    "stage_id",
    "logical_trial_index",
)

PAIRED_RATIO_FIELDS = (
    "method",
    "requested_workers",
    "row_id",
    "trial_index",
    "chart_wall_s",
    "native_wall_s",
    "chart_over_native",
    "execution_order",
    "chart_stage",
    "native_peer_source",
)

TRIAL_CLASSIFICATION_FIELDS = (
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

PLAN_FIELD_COVERAGE_TEMPLATE_FIELDS = (
    "plan_field",
    "applies_to",
    "source_kind",
    "artifact_path",
    "source_field",
    "phase0_state",
    "overlap_note",
    "note",
)

PLAN_FIELD_COVERAGE_FIELDS = (
    "plan_field",
    "applies_to",
    "row_id",
    "required_for_status",
    "source_kind",
    "artifact_path",
    "source_field",
    "phase0_state",
    "overlap_note",
    "note",
)

# This is a closed map of the evidence promised by "Evidence recorded after
# every phase" in the implementation plan.  A field absent before its owning
# optimization phase is named as such; it is never synthesized from a nearby
# aggregate timer or counter.
PLAN_FIELD_COVERAGE_ROWS = (
    ("git_revision_and_dirty", "capture", "artifact_field", "live-pre.json;live-post.json;live-pre.git-porcelain-v2-z;live-post.git-porcelain-v2-z", "repo_revision,git_dirty,git_porcelain_sha256", "recorded", "-", "raw porcelain-v2-z bytes and their hashes are both retained"),
    ("exact_command", "capture", "command_log", "../../capture-STATUS.status.json", "characterization_command,repeat_stages", "recorded", "-", "repeat_stages is a closed array and may be empty only when no successful row requires repeats"),
    ("exact_environment", "capture", "artifact_field", "live-pre.json;live-post.json", "environment", "recorded", "-", "child command is launched by env -i with this exact map"),
    ("compiler_build_flags", "capture", "capture_metadata", "../../../capture-metadata.txt", "compiler,build_type,flags", "recorded", "-", "static build binding is also hashed by both live snapshots"),
    ("cpu_model_topology", "capture", "capture_metadata", "../../../capture-metadata.txt", "cpu_model,topology", "recorded", "-", "static machine identity"),
    ("live_affinity", "capture", "artifact_field", "live-pre.json;live-post.json", "bootstrap_affinity_cpus,target_affinity_cpus", "recorded", "-", "timed child affinity is additionally fixed by command/taskset"),
    ("live_cpufreq_governor_min_max_boost", "capture", "artifact_field", "live-pre.json;live-post.json", "cpufreq,global_boost", "recorded", "-", "all target CPUs, before and after"),
    ("live_load_memory_swap", "capture", "artifact_field", "live-pre.json;live-post.json", "loadavg_raw,mem_available_kb,swap_free_kb", "recorded", "-", "before and after"),
    ("worker_request_policy", "timed_chart_trial", "raw_tsv", "raw_trials.tsv", "requested_workers,worker_policy", "recorded", "-", "timeouts use the explicit timeout_unobserved policy label"),
    ("worker_resolution", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "resolved_workers", "recorded", "-", "product-derived resolved count is evidence only after a complete successful report"),
    ("fixture_hashes", "timed_trial", "raw_tsv", "raw_trials.tsv", "input_sha256,refseq_sha256", "recorded", "-", "also fixed in workload manifest"),
    ("active_patterns", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "active_patterns", "recorded", "-", "-"),
    ("grammar_clades_productions", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "grammar_clades,grammar_productions", "recorded", "-", "-"),
    ("candidate_source_count", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "candidate_source,candidates_generated,candidates_scored", "recorded", "-", "-"),
    ("exact_verification_count", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "exact_verifications", "recorded", "-", "-"),
    ("seed_exactness_labels", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "seed,acceptance,objective,final_compaction_exactness_kind", "recorded", "-", "additional labels are checked directly in each report"),
    ("wall_user_system_time", "timed_trial", "raw_tsv", "raw_trials.tsv", "wall_clock_s,user_cpu_s,system_cpu_s", "recorded", "-", "wall median is recomputed only for N>=3 finite rows"),
    ("peak_rss", "timed_trial", "raw_tsv", "raw_trials.tsv", "max_rss_kb,peak_sampled_rss_kb", "recorded", "-", "aggregate sampled descendant RSS is authoritative"),
    ("resident_chart_bytes", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "chart_cache_resident_bytes", "recorded", "-", "-"),
    ("configured_memory_budget", "timed_chart_trial", "raw_tsv", "raw_trials.tsv", "configured_chart_memory_budget,manifest_rss_limit_bytes", "recorded", "-", "-"),
    ("peak_concurrent_exact_verifiers", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "peak_concurrent_exact_verifiers", "recorded", "-", "required Phase-0 instrumentation; no unavailable waiver"),
    (
        "exact_candidate_admission",
        "successful_chart_trial",
        "raw_tsv",
        "raw_trials.tsv",
        ",".join(EXACT_CANDIDATE_ADMISSION_FIELDS),
        "recorded",
        "-",
        "required Phase-6 batch-axis, memory-admission, and queue-time instrumentation; no unavailable waiver",
    ),
    ("candidate_generation_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "candidate_generation_ms", "recorded", "-", "-"),
    ("chart_plan_time", "successful_chart_trial", "unavailable", "-", "-", "explicitly_unavailable", "-", "Phase-0 product has no distinct chart-plan timer"),
    ("initial_chart_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "initial_chart_construction_ms", "recorded", "-", "required Phase-0 instrumentation; distinct from cache build"),
    ("cache_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "cache_build_ms", "recorded", "combined initial chart/cache region; not additive with initial_chart_ms", "legacy aggregate retained for continuity"),
    ("local_scoring_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "local_scoring_ms", "recorded", "-", "-"),
    ("exact_setup_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "exact_initialization_ms", "recorded", "-", "-"),
    ("exact_candidate_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "exact_candidate_verification_ms_min,exact_candidate_verification_ms_mean,exact_candidate_verification_ms_max", "recorded", "-", "-"),
    ("accepted_update_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "accepted_rebuild_ms", "recorded", "contains update/rebuild work; not treated as materialization", "legacy aggregate"),
    ("materialization_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "materialization_ms,materialization_exact_verification_ms,materialization_accepted_update_ms,materialization_final_compaction_ms", "recorded", "materialization_ms is exactly the sum of three disjoint reason buckets", "local-score-only diagnostic materializations are excluded by definition"),
    ("total_time", "successful_chart_trial", "raw_tsv", "raw_trials.tsv", "total_ms", "recorded", "-", "-"),
    ("scheduler_task_count", "successful_chart_trial", "report", "raw_trials.tsv:report_path", "local_score_worker_tasks", "recorded", "-", "existing local-score scheduler counter"),
    ("scheduler_grain", "successful_chart_trial", "unavailable", "-", "-", "explicitly_unavailable", "-", "not applicable before Phase 3 persistent scheduler"),
    ("scheduler_axis", "successful_chart_trial", "unavailable", "-", "-", "explicitly_unavailable", "-", "not applicable before Phase 3 persistent scheduler"),
    ("scheduler_utilization", "successful_chart_trial", "unavailable", "-", "-", "explicitly_unavailable", "-", "not applicable before Phase 3 persistent scheduler"),
    ("final_objective_parsimony", "successful_trial", "raw_tsv", "raw_trials.tsv", "final_validated_parsimony_min,best_reported_objective", "recorded", "-", "externally validated output"),
    ("candidate_and_accept_semantics", "full_canonical_companion", "canonical_sidecar", "characterization/logs/*_full_canonical.ndjson;characterization/logs/*_full_canonical.json", "candidate.signature,iteration_outcome.selected_signature,iteration_outcome.accepted_move_committed,exact_evidence.evidence_kind,exact_evidence.keep_mask_kind,exact_evidence.keep_production_exact,exact_root_provenance_class|fixed_topology_certificate,report.candidates_sha256,report.exact_sha256,report.acceptance_sha256,report.chain_sha256", "recorded", "-", "explicit one-worker full companion content-addressed by search_semantic_sha256; grammar evidence has root provenance and fixed-topology evidence has its certificate"),
    ("final_topology_digest", "successful_trial", "canonical_digest", "raw_trials.tsv", "output_semantic_sha256,trial_semantic_sha256,canonical_argv_sha256", "recorded", "-", "all repeated values are required unanimous"),
)


def expected_plan_field_coverage_rows(
    capture: CaptureSpec,
    capture_root: Path,
    merged_rows: Sequence[Mapping[str, str]],
    spec_by_key: Mapping[tuple[str, str], RowSpec],
) -> list[dict[str, str]]:
    if not merged_rows:
        fail(f"cannot cover plan fields without capture rows: {capture.capture_id}")
    validate_initial_score_unanimity(
        merged_rows, f"plan-field coverage {capture.capture_id}"
    )
    plan_fields = [row[0] for row in PLAN_FIELD_COVERAGE_ROWS]
    if len(plan_fields) != len(set(plan_fields)):
        fail("plan-field coverage map has duplicate plan_field values")
    allowed_sources = {
        "artifact_field",
        "canonical_digest",
        "canonical_sidecar",
        "capture_metadata",
        "command_log",
        "manifest",
        "process_metrics",
        "raw_tsv",
        "report",
        "unavailable",
    }
    raw_fields = set(merged_rows[0])
    rows_by_id: dict[str, list[Mapping[str, str]]] = {}
    for trial in merged_rows:
        key = raw_key(trial)
        spec = spec_by_key.get(key)
        if spec is None:
            fail(f"coverage has no planned row for {capture.capture_id}: {key}")
        row_id = spec.row_id
        siblings = rows_by_id.setdefault(row_id, [])
        if siblings:
            for field in ("method", "requested_workers", "status"):
                if siblings[0].get(field) != trial.get(field):
                    fail(
                        f"coverage row identity changes across trials: "
                        f"{row_id}: {field}"
                    )
        siblings.append(trial)
        status = trial.get("status", "")
        validate_raw_trial_binding(capture, trial, key, status)
        if status == "ok":
            validate_successful_raw_domains(
                capture_root, capture, trial, key
            )
        elif status != "timeout":
            fail(f"coverage row has an invalid status: {spec.row_id}: {status}")

    required_status = {
        "capture": "all",
        "timed_trial": "all",
        "timed_chart_trial": "all",
        "successful_trial": "ok",
        "successful_chart_trial": "ok",
        "full_canonical_companion": "ok",
    }

    def applicability(
        applies_to: str, trial: Mapping[str, str]
    ) -> tuple[bool, str]:
        is_chart = trial["method"] != "sample_explore_merge"
        succeeded = trial["status"] == "ok"
        if applies_to == "timed_trial":
            return True, "-"
        if applies_to == "timed_chart_trial":
            return (
                (True, "-")
                if is_chart
                else (False, "not applicable to the native row")
            )
        if applies_to == "successful_trial":
            return (
                (True, "-")
                if succeeded
                else (False, "not applicable to an observed timeout row")
            )
        if applies_to in (
            "successful_chart_trial",
            "full_canonical_companion",
        ):
            if not is_chart:
                return False, "not applicable to the native row"
            if not succeeded:
                return False, "not applicable to an observed timeout row"
            return True, "-"
        fail(f"plan-field coverage has invalid applies_to value: {applies_to}")

    rendered: list[dict[str, str]] = []
    for values in PLAN_FIELD_COVERAGE_ROWS:
        template = dict(
            zip(PLAN_FIELD_COVERAGE_TEMPLATE_FIELDS, values, strict=True)
        )
        applies_to = template["applies_to"]
        if applies_to not in required_status:
            fail(f"plan-field coverage has invalid applies_to value: {template}")
        if template["plan_field"] == "exact_command":
            template["artifact_path"] = (
                f"../../capture-{capture.capture_id}.status.json"
            )
        elif template["source_kind"] == "capture_metadata":
            template["artifact_path"] = "../../../capture-metadata.txt"

        target_rows: list[tuple[str, Mapping[str, str] | None]]
        if applies_to == "capture":
            target_rows = [(f"capture:{capture.capture_id}", None)]
        else:
            target_rows = [
                (row_id, rows_by_id[row_id][0]) for row_id in sorted(rows_by_id)
            ]

        for row_id, representative in target_rows:
            row = {
                "plan_field": template["plan_field"],
                "applies_to": applies_to,
                "row_id": row_id,
                "required_for_status": required_status[applies_to],
                "source_kind": template["source_kind"],
                "artifact_path": template["artifact_path"],
                "source_field": template["source_field"],
                "phase0_state": template["phase0_state"],
                "overlap_note": template["overlap_note"],
                "note": template["note"],
            }
            if representative is not None:
                applies, reason = applicability(applies_to, representative)
                if not applies:
                    row.update(
                        {
                            "source_kind": "unavailable",
                            "artifact_path": "-",
                            "source_field": "-",
                            "phase0_state": "not_applicable",
                            "overlap_note": "-",
                            "note": reason,
                        }
                    )
                elif row["source_kind"] == "canonical_sidecar":
                    sidecar, report = exact_canonical_asset_pair(
                        capture_root, capture, raw_key(representative)
                    )
                    row["artifact_path"] = ";".join(
                        (
                            sidecar.relative_to(capture_root).as_posix(),
                            report.relative_to(capture_root).as_posix(),
                        )
                    )

            source = row["source_kind"]
            state = row["phase0_state"]
            if source not in allowed_sources:
                fail(f"plan-field coverage has invalid source kind: {row}")
            if source == "unavailable":
                if state not in ("explicitly_unavailable", "not_applicable"):
                    fail(f"plan-field unavailable source/state disagree: {row}")
            elif state != "recorded":
                fail(f"recorded plan-field source/state disagree: {row}")
            if source in ("raw_tsv", "canonical_digest"):
                missing = set(row["source_field"].split(",")) - raw_fields
                if missing:
                    fail(
                        f"plan-field {row['plan_field']} names missing raw "
                        f"fields: {sorted(missing)}"
                    )
            elif source == "report":
                assert representative is not None
                for trial in rows_by_id[row_id]:
                    report = Path(trial["report_path"]).resolve(strict=True)
                    try:
                        report.relative_to(capture_root.resolve(strict=True))
                    except ValueError:
                        fail(f"coverage report escapes capture: {report}")
                    for report_field in row["source_field"].split(","):
                        if report_field == "local_score_worker_tasks":
                            require_unsigned_text(
                                report_counter_value(report, report_field),
                                f"{row_id} report counter {report_field}",
                            )
                        else:
                            report_value(report, report_field)
            rendered.append(row)
    return rendered


def write_plan_field_coverage(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    capture_root: Path,
    merged_rows: Sequence[Mapping[str, str]],
    spec_by_key: Mapping[tuple[str, str], RowSpec],
) -> Path:
    del metadata, paths
    rendered = expected_plan_field_coverage_rows(
        capture, capture_root, merged_rows, spec_by_key
    )
    coverage_path = capture_root / "plan-field-coverage.tsv"
    write_tsv_exclusive(
        coverage_path, PLAN_FIELD_COVERAGE_FIELDS, rendered
    )
    return coverage_path


def paired_ratio_row(
    key: tuple[str, str],
    spec: RowSpec,
    trial: int,
    chart: Mapping[str, str],
    native: Mapping[str, str],
    chart_stage: str,
    peer_source: str,
) -> tuple[dict[str, str], Decimal]:
    if chart["execution_order"] != native["execution_order"]:
        fail(f"paired execution order differs for {key} trial {trial}")
    chart_wall = decimal_value(chart["wall_clock_s"], f"{key} chart wall")
    native_wall = decimal_value(native["wall_clock_s"], f"{key} native wall")
    if native_wall <= 0:
        fail(f"paired native wall is not positive for {key} trial {trial}")
    ratio = chart_wall / native_wall
    return (
        {
            "method": key[0],
            "requested_workers": key[1],
            "row_id": spec.row_id,
            "trial_index": str(trial),
            "chart_wall_s": chart["wall_clock_s"],
            "native_wall_s": native["wall_clock_s"],
            "chart_over_native": fixed_decimal(ratio, "0.000000001"),
            "execution_order": chart["execution_order"],
            "chart_stage": chart_stage,
            "native_peer_source": peer_source,
        },
        ratio,
    )


def capture_one(args: argparse.Namespace) -> None:
    baseline_dir = Path(args.baseline_dir).resolve()
    metadata, paths = load_metadata(baseline_dir)
    assert_no_sealed_base(paths)
    verify_metadata_inputs(metadata)
    captures = {
        item.capture_id: item
        for item in (
            deserialize_capture(value)
            for value in metadata["captures"]  # type: ignore[index]
        )
    }
    rows = [
        deserialize_row(value) for value in metadata["rows"]  # type: ignore[index]
    ]
    capture = captures.get(args.capture_id)
    if capture is None:
        fail(f"unknown capture ID: {args.capture_id}")
    planned_timeout_specs = timeout_rows_for_capture(rows, capture.capture_id)
    allowed_timeout_keys = timeout_key_map(planned_timeout_specs)
    if capture.long_medium and not args.confirm_long_medium:
        fail(
            "medium capture requires the explicit --confirm-long-medium guard; "
            "no process was started"
        )
    if capture.long_real and not args.confirm_real_preflight:
        fail(
            "real-scale preflight requires the explicit "
            "--confirm-real-preflight guard; no process was started"
        )
    if capture.capture_role == "real_preflight":
        capture_real_preflight(baseline_dir, metadata, paths, capture, rows)
        return
    if capture.affinity_kind == "unpinned":
        current_affinity = observed_affinity()
        if current_affinity != capture.affinity_cpus:
            fail(
                "the unpinned affinity changed after prepare: expected "
                f"{capture.affinity_cpus}, observed {current_affinity}"
            )

    capture_root = paths["captures"] / capture.capture_id
    stdout_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.stdout"
    stderr_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.stderr"
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    approval_path = timeout_approval_path(paths, capture.capture_id)
    for path in (capture_root, stdout_path, stderr_path, status_path, approval_path):
        if path_occupied(path):
            fail(f"capture destination already exists; refusing overwrite: {path}")
    capture_root.mkdir(mode=0o755)
    pre_path, pre_porcelain = write_live_evidence(
        metadata, paths, capture, capture_root, "pre"
    )

    chart_keys = ordered_capture_chart_keys(capture)
    spec_by_key = capture_row_specs(capture, rows)
    characterization_dir = capture_root / "characterization"
    characterization_command = capture_command(
        metadata, paths, capture, out_dir=characterization_dir
    )
    characterization_exit = run_stage_command(
        characterization_command, stdout_path, stderr_path
    )
    characterization_rows, any_timeout = validate_stage(
        capture_root,
        characterization_dir,
        capture,
        chart_keys,
        include_native=True,
        warmups=0,
        repetitions=1,
        allowed_timeout_keys=allowed_timeout_keys,
    )
    expected_characterization_exit = 1 if any_timeout else 0
    if characterization_exit != expected_characterization_exit:
        fail(
            "characterization harness exit is inconsistent with its exact "
            f"rows: {characterization_exit} != {expected_characterization_exit}"
        )
    characterization_by_key = {
        raw_key(row): row for row in characterization_rows
    }
    native_key = ("sample_explore_merge", "native")
    native_trial1 = characterization_by_key[native_key]
    if native_trial1["status"] != "ok":
        fail("the shared characterization native peer did not succeed")

    raw_fields = list(characterization_rows[0])
    merged_rows: list[dict[str, str]] = []
    pair_peer_rows: list[dict[str, str]] = []
    ratio_rows: list[dict[str, str]] = []
    ratios_by_key: dict[tuple[str, str], list[Decimal]] = {}
    classification_rows: list[dict[str, str]] = []
    repeat_records: list[dict[str, object]] = []
    repeats_root = capture_root / "repeats"
    repeats_root.mkdir(mode=0o755)

    # Only the capture that owns the unique native manifest row contributes to
    # the canonical native sample.  Native executions in chart subgroups are
    # retained separately as exact pairing peers and cannot inflate that sample.
    native_spec = spec_by_key.get(native_key)
    if native_spec is not None:
        merged_rows.append(dict(native_trial1))
        native_stage, native_command = repeat_stage_command(
            metadata,
            paths,
            capture,
            native_key,
            native_spec.measured_trial_target,
        )
        native_stdout = capture_root / "repeats" / "native.stdout"
        native_stderr = capture_root / "repeats" / "native.stderr"
        native_exit = run_stage_command(native_command, native_stdout, native_stderr)
        native_repeat_rows, native_timeout = validate_stage(
            capture_root,
            native_stage,
            capture,
            (),
            include_native=True,
            warmups=1,
            repetitions=native_spec.measured_trial_target - 1,
            allowed_timeout_keys={},
        )
        if native_exit != 0 or native_timeout:
            fail(f"canonical native repeat subgroup failed: {native_stage}")
        for row in native_repeat_rows:
            normalized = dict(row)
            normalized["trial_index"] = str(int(row["trial_index"]) + 1)
            merged_rows.append(normalized)
        repeat_records.append(
            {
                "key": list(native_key),
                "row_id": native_spec.row_id,
                "stage": native_stage.relative_to(capture_root).as_posix(),
                "command": native_command,
                "harness_exit_code": native_exit,
                "raw_trials_sha256": sha256_file(native_stage / "raw_trials.tsv"),
                "summary_sha256": sha256_file(native_stage / "summary.tsv"),
                "stdout_sha256": sha256_file(native_stdout),
                "stderr_sha256": sha256_file(native_stderr),
                "canonical_rows": len(native_repeat_rows),
                "pair_peer_rows": 0,
            }
        )
        classification_rows.append(
            {
                "method": native_key[0],
                "requested_workers": native_key[1],
                "row_id": native_spec.row_id,
                "measured_trial_target": str(native_spec.measured_trial_target),
                "observed_trial_count": str(native_spec.measured_trial_target),
                "outcome": "ok",
                "evidence_kind": "finite_performance",
                "characterization_stage": "characterization",
                "repeat_stage": native_stage.relative_to(capture_root).as_posix(),
                "trial1_native_peer": "self",
            }
        )

    for key in chart_keys:
        spec = spec_by_key[key]
        chart_trial1 = characterization_by_key[key]
        peer_base = dict(native_trial1)
        peer_base.update(
            {
                "paired_method": key[0],
                "paired_requested_workers": key[1],
                "peer_role": "shared_characterization_trial1",
                "stage_id": "characterization",
                "logical_trial_index": "1",
            }
        )
        pair_peer_rows.append(peer_base)
        merged_rows.append(dict(chart_trial1))
        if chart_trial1["status"] == "timeout":
            classification_rows.append(
                {
                    "method": key[0],
                    "requested_workers": key[1],
                    "row_id": spec.row_id,
                    "measured_trial_target": str(spec.measured_trial_target),
                    "observed_trial_count": "1",
                    "outcome": "timeout",
                    "evidence_kind": "timeout_characterization_non_performance",
                    "characterization_stage": "characterization",
                    "repeat_stage": "-",
                    "trial1_native_peer": "characterization:sample_explore_merge@native",
                }
            )
            continue

        repeat_stage, repeat_command = repeat_stage_command(
            metadata, paths, capture, key, spec.measured_trial_target
        )
        repeat_slug = raw_key_slug(key)
        repeat_stdout = capture_root / "repeats" / f"{repeat_slug}.stdout"
        repeat_stderr = capture_root / "repeats" / f"{repeat_slug}.stderr"
        repeat_exit = run_stage_command(
            repeat_command, repeat_stdout, repeat_stderr
        )
        repeat_rows, repeat_timeout = validate_stage(
            capture_root,
            repeat_stage,
            capture,
            (key,),
            include_native=True,
            warmups=1,
            repetitions=spec.measured_trial_target - 1,
            allowed_timeout_keys={},
        )
        if repeat_exit != 0 or repeat_timeout:
            fail(f"finite chart repeat subgroup failed: {repeat_stage}")
        repeat_chart = {
            int(row["trial_index"]): row
            for row in repeat_rows
            if raw_key(row) == key
        }
        repeat_native = {
            int(row["trial_index"]): row
            for row in repeat_rows
            if raw_key(row) == native_key
        }
        if set(repeat_chart) != set(repeat_native) or set(repeat_chart) != set(
            range(1, spec.measured_trial_target)
        ):
            fail(f"finite repeat subgroup has an incomplete paired join: {key}")
        ratio_row, ratio = paired_ratio_row(
            key,
            spec,
            1,
            chart_trial1,
            native_trial1,
            "characterization",
            "shared_characterization_trial1",
        )
        ratio_rows.append(ratio_row)
        ratios_by_key.setdefault(key, []).append(ratio)
        for stage_trial in range(1, spec.measured_trial_target):
            logical_trial = stage_trial + 1
            chart_row = dict(repeat_chart[stage_trial])
            native_row = dict(repeat_native[stage_trial])
            chart_row["trial_index"] = str(logical_trial)
            native_row["trial_index"] = str(logical_trial)
            merged_rows.append(chart_row)
            peer = dict(native_row)
            peer.update(
                {
                    "paired_method": key[0],
                    "paired_requested_workers": key[1],
                    "peer_role": "isolated_repeat_peer",
                    "stage_id": repeat_stage.relative_to(capture_root).as_posix(),
                    "logical_trial_index": str(logical_trial),
                }
            )
            pair_peer_rows.append(peer)
            ratio_row, ratio = paired_ratio_row(
                key,
                spec,
                logical_trial,
                chart_row,
                native_row,
                repeat_stage.relative_to(capture_root).as_posix(),
                "isolated_repeat_peer",
            )
            ratio_rows.append(ratio_row)
            ratios_by_key[key].append(ratio)
        repeat_records.append(
            {
                "key": list(key),
                "row_id": spec.row_id,
                "stage": repeat_stage.relative_to(capture_root).as_posix(),
                "command": repeat_command,
                "harness_exit_code": repeat_exit,
                "raw_trials_sha256": sha256_file(
                    repeat_stage / "raw_trials.tsv"
                ),
                "summary_sha256": sha256_file(repeat_stage / "summary.tsv"),
                "stdout_sha256": sha256_file(repeat_stdout),
                "stderr_sha256": sha256_file(repeat_stderr),
                "canonical_rows": len(repeat_chart),
                "pair_peer_rows": len(repeat_native),
            }
        )
        classification_rows.append(
            {
                "method": key[0],
                "requested_workers": key[1],
                "row_id": spec.row_id,
                "measured_trial_target": str(spec.measured_trial_target),
                "observed_trial_count": str(spec.measured_trial_target),
                "outcome": "ok",
                "evidence_kind": "finite_performance",
                "characterization_stage": "characterization",
                "repeat_stage": repeat_stage.relative_to(capture_root).as_posix(),
                "trial1_native_peer": "characterization:sample_explore_merge@native",
            }
        )

    merged_rows.sort(key=lambda row: (*raw_key(row), int(row["trial_index"])))
    for key in {raw_key(row) for row in merged_rows}:
        key_rows = [row for row in merged_rows if raw_key(row) == key]
        successes = [row for row in key_rows if row["status"] == "ok"]
        if successes:
            validate_repeated_unanimity(capture_root, key, successes)
    validate_repeated_unanimity(
        capture_root, native_key, (native_trial1, *pair_peer_rows)
    )
    validate_cross_worker_semantics(merged_rows)
    pair_peer_rows.sort(
        key=lambda row: (
            row["paired_method"],
            row["paired_requested_workers"],
            int(row["logical_trial_index"]),
        )
    )
    ratio_rows.sort(
        key=lambda row: (
            row["method"], row["requested_workers"], int(row["trial_index"])
        )
    )
    classification_rows.sort(
        key=lambda row: (row["method"], row["requested_workers"])
    )
    merged_path = capture_root / "raw_trials.tsv"
    peer_path = capture_root / "pair_peer_trials.tsv"
    ratio_path = capture_root / "paired_ratios.tsv"
    classification_path = capture_root / "trial_classification.tsv"
    summary_path = capture_root / "summary.tsv"
    write_tsv_exclusive(merged_path, raw_fields, merged_rows)
    write_tsv_exclusive(
        peer_path, (*raw_fields, *PAIR_PEER_FIELDS), pair_peer_rows
    )
    write_tsv_exclusive(ratio_path, PAIRED_RATIO_FIELDS, ratio_rows)
    write_tsv_exclusive(
        classification_path, TRIAL_CLASSIFICATION_FIELDS, classification_rows
    )
    summary_bytes = render_capture_summary(
        merged_rows, spec_by_key, ratios_by_key
    )
    write_bytes_exclusive(summary_path, summary_bytes, 0o444)
    if render_capture_summary(
        read_raw_trials(merged_path), spec_by_key, ratios_by_key
    ) != summary_path.read_bytes():
        fail("capture summary is not exactly recomputable from merged raw trials")

    coverage_path = write_plan_field_coverage(
        metadata, paths, capture, capture_root, merged_rows, spec_by_key
    )
    post_path, post_porcelain = write_live_evidence(
        metadata, paths, capture, capture_root, "post"
    )
    observed_timeout_ids = sorted(
        allowed_timeout_keys[raw_key(row)]
        for row in merged_rows
        if row["status"] == "timeout"
    )
    status = {
        "capture_id": capture.capture_id,
        "characterization_command": characterization_command,
        "characterization_harness_exit_code": characterization_exit,
        "characterization_raw_trials_sha256": sha256_file(
            characterization_dir / "raw_trials.tsv"
        ),
        "characterization_summary_sha256": sha256_file(
            characterization_dir / "summary.tsv"
        ),
        "characterization_stdout_sha256": sha256_file(stdout_path),
        "characterization_stderr_sha256": sha256_file(stderr_path),
        "timeout_eligible_row_ids": sorted(
            row.row_id for row in planned_timeout_specs
        ),
        "observed_timeout_row_ids": observed_timeout_ids,
        "repeat_stages": repeat_records,
        "row_count": len(merged_rows),
        "timeout_rows": len(observed_timeout_ids),
        "raw_trials_sha256": sha256_file(merged_path),
        "summary_sha256": sha256_file(summary_path),
        "pair_peer_trials_sha256": sha256_file(peer_path),
        "paired_ratios_sha256": sha256_file(ratio_path),
        "trial_classification_sha256": sha256_file(classification_path),
        "plan_field_coverage_sha256": sha256_file(coverage_path),
        "live_pre_sha256": sha256_file(pre_path),
        "live_pre_porcelain_sha256": sha256_file(pre_porcelain),
        "live_post_sha256": sha256_file(post_path),
        "live_post_porcelain_sha256": sha256_file(post_porcelain),
    }
    write_text_exclusive(
        status_path, json.dumps(status, sort_keys=True, indent=2) + "\n", 0o444
    )
    readback_rows, readback_timeouts, expected_status = validate_capture_evidence(
        metadata, paths, capture, rows
    )
    if (
        readback_rows != merged_rows
        or readback_timeouts != observed_timeout_ids
        or expected_status != status
    ):
        fail(
            "completed capture did not survive exact on-disk readback: "
            f"{capture.capture_id}"
        )
    print(
        f"captured {capture.capture_id}: canonical_rows={len(merged_rows)}, "
        f"timeouts={len(observed_timeout_ids)}, repeats={len(repeat_records)}"
    )
    if observed_timeout_ids:
        flags = " ".join(
            f"--allow-expected-timeout {shlex.quote(row_id)}"
            for row_id in observed_timeout_ids
        )
        print(
            "Observed eligible timeouts remain unapproved. Review the capture, "
            "then explicitly run:\n"
            f"  python3 {shlex.quote(str(paths['frozen_helper']))} "
            f"approve-timeouts --baseline-dir {shlex.quote(str(baseline_dir))} "
            f"--capture-id {shlex.quote(capture.capture_id)} {flags}"
        )


def timeout_approval_path(paths: Mapping[str, Path], capture_id: str) -> Path:
    return paths["bootstrap_dir"] / f"capture-{capture_id}.timeout-approval.json"


def read_exact_tsv(
    path: Path, expected_fields: Sequence[str], *, allow_empty: bool = False
) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream, dialect="excel-tab")
            if reader.fieldnames != list(expected_fields):
                fail(
                    f"TSV header changed for {path}: {reader.fieldnames!r} != "
                    f"{list(expected_fields)!r}"
                )
            rows = list(reader)
    except FileNotFoundError:
        fail(f"required capture TSV is missing: {path}")
    if not rows and not allow_empty:
        fail(f"required capture TSV has no data rows: {path}")
    for row in rows:
        if None in row or set(row) != set(expected_fields) or any(
            value is None for value in row.values()
        ):
            fail(f"capture TSV has extra/missing cells: {path}")
    return rows


def validate_sorted_cpu_list(value: object, label: str) -> list[int]:
    if (
        not isinstance(value, list)
        or not value
        or any(isinstance(cpu, bool) or not isinstance(cpu, int) or cpu < 0 for cpu in value)
        or value != sorted(set(value))
    ):
        fail(f"live evidence {label} is not a sorted unique CPU list")
    return value


def validate_live_affinity_lists(
    capture: CaptureSpec,
    target: Sequence[int],
    bootstrap: Sequence[int],
    online: Sequence[int],
    label: str,
) -> None:
    """Bind target and inherited affinity, including the unpinned control."""

    if list(target) != parse_cpu_set(capture.affinity_cpus):
        fail(f"live target affinity differs from its capture: {label}")
    if not set(target) <= set(online) or not set(bootstrap) <= set(online):
        fail(f"live affinity contains an offline CPU: {label}")
    if capture.affinity_kind == "unpinned" and list(bootstrap) != list(target):
        fail(
            "unpinned live target is not the exact inherited bootstrap "
            f"affinity: {label}: target={list(target)}, "
            f"bootstrap={list(bootstrap)}"
        )


def validate_live_cpufreq(value: object, expected_cpus: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected_cpus:
        fail(f"live cpufreq CPU set changed: {label}")
    for cpu, raw_values in value.items():
        if not isinstance(raw_values, dict) or set(raw_values) != set(
            LIVE_CPUFREQ_FIELDS
        ):
            fail(f"live cpufreq field set changed for cpu{cpu}: {label}")
        values = raw_values
        for field in ("scaling_governor", "energy_performance_preference"):
            if not isinstance(values[field], str) or not re.fullmatch(
                r"[A-Za-z0-9_.-]+", values[field]
            ):
                fail(f"live cpufreq {field} is invalid for cpu{cpu}: {label}")
        frequencies: dict[str, int] = {}
        for field in ("scaling_cur_freq", "scaling_min_freq", "scaling_max_freq"):
            text = values[field]
            if not isinstance(text, str) or not re.fullmatch(r"[1-9][0-9]*", text):
                fail(f"live cpufreq {field} is invalid for cpu{cpu}: {label}")
            frequencies[field] = int(text)
        if frequencies["scaling_min_freq"] > frequencies["scaling_max_freq"]:
            fail(f"live cpufreq bounds are inverted for cpu{cpu}: {label}")
        if values["boost"] not in ("0", "1"):
            fail(f"live cpufreq boost is invalid for cpu{cpu}: {label}")


LOADAVG_PATTERN = re.compile(
    r"[0-9]+(?:[.][0-9]+)? [0-9]+(?:[.][0-9]+)? "
    r"[0-9]+(?:[.][0-9]+)? [0-9]+/[0-9]+ [0-9]+"
)


def validate_live_evidence_file(
    metadata: Mapping[str, object],
    capture: CaptureSpec,
    capture_root: Path,
    position: str,
) -> tuple[Path, Path, dict[str, object]]:
    evidence_path = capture_root / f"live-{position}.json"
    porcelain_path = capture_root / f"live-{position}.git-porcelain-v2-z"
    try:
        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"invalid live capture evidence {position}: {error}")
    if not isinstance(evidence, dict):
        fail(f"live capture evidence is not an object: {evidence_path}")
    required = {
        "schema",
        "schema_version",
        "capture_id",
        "position",
        "timestamp_utc",
        "timestamp_local",
        "unix_time_ns",
        "repo_revision",
        "git_porcelain_format",
        "git_porcelain_path",
        "git_porcelain_sha256",
        "git_dirty",
        "bootstrap_affinity_cpus",
        "target_affinity_kind",
        "target_affinity_cpus",
        "target_affinity_cpu_list",
        "online_cpus",
        "loadavg_raw",
        "mem_available_kb",
        "swap_free_kb",
        "global_boost",
        "cpufreq",
        "environment",
        "static_bindings",
    }
    if set(evidence) != required:
        fail(
            f"live capture evidence key set changed: {evidence_path}: "
            f"missing={sorted(required - set(evidence))}, "
            f"unexpected={sorted(set(evidence) - required)}"
        )
    if (
        isinstance(evidence.get("schema_version"), bool)
        or evidence.get("schema_version") != 1
        or not isinstance(evidence.get("git_dirty"), bool)
    ):
        fail(f"live capture evidence has invalid scalar types: {evidence_path}")
    exact = {
        "schema": "wric_phase0_live_capture_environment",
        "schema_version": 1,
        "capture_id": capture.capture_id,
        "position": position,
        "repo_revision": FROZEN_REVISION,
        "git_porcelain_format": "porcelain-v2-z",
        "git_porcelain_path": porcelain_path.name,
        "target_affinity_kind": capture.affinity_kind,
        "target_affinity_cpus": capture.affinity_cpus,
        "target_affinity_cpu_list": parse_cpu_set(capture.affinity_cpus),
        "environment": CAPTURE_ENVIRONMENT
        | {"WRIC_REPO_ROOT": str(metadata["repo_root"])},
        "static_bindings": {
            "capture_metadata_sha256": sha256_file(
                Path(str(metadata["baseline_dir"])) / "capture-metadata.txt"
            ),
            "frozen_larch2_sha256": str(metadata["frozen_larch2_sha256"]),
            "frozen_oracle_sha256": str(metadata["frozen_oracle_sha256"]),
            "harness_sha256": str(metadata["harness_sha256"]),
            "process_metrics_sha256": str(metadata["process_metrics_sha256"]),
            "wrapper_calibration_sha256": str(
                metadata["wrapper_calibration_sha256"]
            ),
        },
    }
    for field, expected in exact.items():
        if evidence.get(field) != expected:
            fail(
                f"live capture evidence {field} changed: {evidence.get(field)!r} "
                f"!= {expected!r}: {evidence_path}"
            )
    raw = porcelain_path.read_bytes()
    if evidence["git_porcelain_sha256"] != sha256_bytes(raw) or evidence[
        "git_dirty"
    ] != bool(raw):
        fail(f"live git porcelain/hash state disagrees: {evidence_path}")
    timestamps: dict[str, datetime] = {}
    for timestamp_field in ("timestamp_utc", "timestamp_local"):
        try:
            parsed = datetime.fromisoformat(str(evidence[timestamp_field]))
        except ValueError:
            fail(f"live evidence timestamp is invalid: {evidence_path}")
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            fail(f"live evidence timestamp lacks a timezone: {evidence_path}")
        timestamps[timestamp_field] = parsed
    if timestamps["timestamp_utc"].utcoffset() != timezone.utc.utcoffset(None):
        fail(f"live UTC timestamp has a non-UTC offset: {evidence_path}")
    if timestamps["timestamp_utc"] != timestamps["timestamp_local"]:
        fail(f"live UTC/local timestamps name different instants: {evidence_path}")
    for integer_field in (
        "unix_time_ns",
        "mem_available_kb",
        "swap_free_kb",
    ):
        if (
            isinstance(evidence[integer_field], bool)
            or not isinstance(evidence[integer_field], int)
            or evidence[integer_field] < 0
        ):
            fail(f"live evidence integer is invalid: {integer_field}: {evidence_path}")
    utc_delta = timestamps["timestamp_utc"] - datetime(
        1970, 1, 1, tzinfo=timezone.utc
    )
    timestamp_ns = (
        (utc_delta.days * 86_400 + utc_delta.seconds) * 1_000_000_000
        + utc_delta.microseconds * 1_000
    )
    if evidence["unix_time_ns"] != timestamp_ns:
        fail(f"live textual/unix timestamps disagree: {evidence_path}")
    if not isinstance(evidence["loadavg_raw"], str) or LOADAVG_PATTERN.fullmatch(
        evidence["loadavg_raw"]
    ) is None:
        fail(f"live loadavg grammar is invalid: {evidence_path}")
    if evidence["global_boost"] not in ("0", "1"):
        fail(f"live boost evidence is invalid: {evidence_path}")
    target = validate_sorted_cpu_list(
        evidence["target_affinity_cpu_list"], "target_affinity_cpu_list"
    )
    bootstrap = validate_sorted_cpu_list(
        evidence["bootstrap_affinity_cpus"], "bootstrap_affinity_cpus"
    )
    online = validate_sorted_cpu_list(evidence["online_cpus"], "online_cpus")
    validate_live_affinity_lists(
        capture, target, bootstrap, online, str(evidence_path)
    )
    expected_cpus = {str(cpu) for cpu in target}
    validate_live_cpufreq(evidence["cpufreq"], expected_cpus, str(evidence_path))
    return evidence_path, porcelain_path, evidence


def json_path_exists(value: object, path: str) -> bool:
    parts = path.split(".")
    current = [value]
    for part in parts:
        following: list[object] = []
        for item in current:
            if isinstance(item, dict) and part in item:
                following.append(item[part])
            elif isinstance(item, list):
                following.extend(
                    member[part]
                    for member in item
                    if isinstance(member, dict) and part in member
                )
        if not following:
            return False
        current = following
    return True


REPEAT_STATUS_FIELDS = {
    "key",
    "row_id",
    "stage",
    "command",
    "harness_exit_code",
    "raw_trials_sha256",
    "summary_sha256",
    "stdout_sha256",
    "stderr_sha256",
    "canonical_rows",
    "pair_peer_rows",
}


def validate_command_log_mapping(value: object, label: str) -> None:
    if not isinstance(value, dict):
        fail(f"command-log source is not an object: {label}")
    characterization = value.get("characterization_command")
    if not isinstance(characterization, list) or not characterization or any(
        not isinstance(token, str)
        or not token
        or "\n" in token
        or "\r" in token
        for token in characterization
    ):
        fail(f"command-log characterization argv is invalid: {label}")
    repeats = value.get("repeat_stages")
    if not isinstance(repeats, list):
        fail(f"command-log repeat_stages is not an array: {label}")
    identities: set[tuple[str, str]] = set()
    for index, stage in enumerate(repeats):
        if not isinstance(stage, dict) or set(stage) != REPEAT_STATUS_FIELDS:
            fail(f"command-log repeat stage {index} has an open schema: {label}")
        key = stage["key"]
        command = stage["command"]
        stage_path = stage["stage"]
        parsed_stage_path = Path(stage_path) if isinstance(stage_path, str) else None
        if (
            not isinstance(key, list)
            or len(key) != 2
            or any(not isinstance(part, str) or not part for part in key)
            or not isinstance(stage["row_id"], str)
            or not stage["row_id"]
            or not isinstance(stage_path, str)
            or not stage_path.startswith("repeats/")
            or parsed_stage_path is None
            or parsed_stage_path.is_absolute()
            or ".." in parsed_stage_path.parts
            or len(parsed_stage_path.parts) < 2
            or parsed_stage_path.as_posix() != stage_path
            or not isinstance(command, list)
            or not command
            or any(
                not isinstance(token, str)
                or not token
                or "\n" in token
                or "\r" in token
                for token in command
            )
            or isinstance(stage["harness_exit_code"], bool)
            or not isinstance(stage["harness_exit_code"], int)
            or stage["harness_exit_code"] != 0
        ):
            fail(f"command-log repeat stage {index} is invalid: {label}")
        identity = (key[0], key[1])
        chart_methods = {mode_method(mode) for mode in MODE_FIELDS}
        if not (
            identity == ("sample_explore_merge", "native")
            or (
                identity[0] in chart_methods
                and (
                    identity[1] in ("auto", "default")
                    or re.fullmatch(r"[1-9][0-9]*", identity[1]) is not None
                )
            )
        ):
            fail(f"command-log repeat key is outside the closed domain: {label}")
        if identity in identities:
            fail(f"command-log has duplicate repeat key {identity}: {label}")
        identities.add(identity)
        for field in (
            "raw_trials_sha256",
            "summary_sha256",
            "stdout_sha256",
            "stderr_sha256",
        ):
            if not isinstance(stage[field], str) or not re.fullmatch(
                r"[0-9a-f]{64}", stage[field]
            ):
                fail(f"command-log repeat stage has invalid {field}: {label}")
        for field in ("canonical_rows", "pair_peer_rows"):
            if (
                isinstance(stage[field], bool)
                or not isinstance(stage[field], int)
                or stage[field] < 0
            ):
                fail(f"command-log repeat stage has invalid {field}: {label}")
        if stage["canonical_rows"] == 0:
            fail(f"command-log repeat stage has no canonical rows: {label}")
        if identity == ("sample_explore_merge", "native"):
            if stage["pair_peer_rows"] != 0:
                fail(f"native command-log repeat stage claims pair peers: {label}")
        elif stage["pair_peer_rows"] != stage["canonical_rows"]:
            fail(f"chart command-log repeat stage has incomplete peers: {label}")


CAPTURE_METADATA_FIELD_PATTERNS = {
    "compiler": re.compile(r"compiler", re.IGNORECASE),
    "build_type": re.compile(
        r"build[ _-]*type|cmake_build_type|RelWithDebInfo", re.IGNORECASE
    ),
    "flags": re.compile(r"flags|(?:^|\s)-O[0-3s](?:\s|$)", re.IGNORECASE),
    "cpu_model": re.compile(r"cpu[ _-]*model|host cpu|model name", re.IGNORECASE),
    "topology": re.compile(
        r"topology|physical cores|threads(?:\s*|[-_/])per(?:\s*|[-_/])core",
        re.IGNORECASE,
    ),
}


def validate_capture_metadata_source(path: Path, source_fields: str) -> None:
    text = path.read_text(encoding="utf-8")
    for field in source_fields.split(","):
        pattern = CAPTURE_METADATA_FIELD_PATTERNS.get(field)
        if pattern is None or pattern.search(text) is None:
            fail(f"capture metadata lacks declared field {field}: {path}")


def validate_canonical_sidecar_source(
    capture_root: Path,
    artifact_paths: Sequence[str],
    row_id: str,
    *,
    stable_digest_mapping: Mapping[str, object] | None = None,
) -> None:
    if len(artifact_paths) != 2:
        fail(f"canonical companion does not name sidecar plus digest: {row_id}")
    sidecar = capture_root / artifact_paths[0]
    report_path = capture_root / artifact_paths[1]
    if sidecar.suffix != ".ndjson" or report_path.suffix != ".json":
        fail(f"canonical companion artifacts have invalid types: {row_id}")
    try:
        sidecar_bytes = sidecar.read_bytes()
        raw_lines = sidecar_bytes.splitlines(keepends=True)
        if (
            not raw_lines
            or any(not line.endswith(b"\n") or b"\r" in line for line in raw_lines)
            or b"" in raw_lines
        ):
            fail(f"canonical companion is not canonical newline-delimited JSON: {row_id}")
        records = [json.loads(line.decode("utf-8")) for line in raw_lines]
        digest = (
            json.loads(report_path.read_text(encoding="utf-8"))
            if stable_digest_mapping is None
            else dict(stable_digest_mapping)
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"canonical companion is not parseable for {row_id}: {error}")
    if not records or any(not isinstance(record, dict) for record in records):
        fail(f"canonical companion has no closed record stream: {row_id}")
    if not isinstance(digest, dict):
        fail(f"canonical companion digest is not an object: {row_id}")
    digest_fields = (
        "semantic_sha256",
        "contract_sha256",
        "candidates_sha256",
        "exact_sha256",
        "acceptance_sha256",
        "chain_sha256",
        "final_topology_sha256",
    )
    count_fields = (
        "record_count",
        "candidate_count",
        "exact_candidate_count",
        "iteration_count",
    )
    expected_digest_keys = {
        "schema",
        "schema_version",
        "digest_algorithm",
        "payload_encoding",
        *digest_fields,
        *count_fields,
    }
    if set(digest) != expected_digest_keys:
        fail(
            f"canonical companion digest has an open key set for {row_id}: "
            f"missing={sorted(expected_digest_keys - set(digest))}, "
            f"unexpected={sorted(set(digest) - expected_digest_keys)}"
        )
    if (
        digest.get("schema") != "larch.chart_spr.semantic_digest"
        or digest.get("schema_version") != 1
        or digest.get("digest_algorithm") != "sha256"
        or digest.get("payload_encoding")
        != "larch.chart_spr.semantic.ndjson.v1"
    ):
        fail(f"canonical companion digest schema changed: {row_id}")
    for field in digest_fields:
        if not re.fullmatch(r"[0-9a-f]{64}", str(digest.get(field, ""))):
            fail(f"canonical companion digest lacks {field}: {row_id}")
    if hashlib.sha256(sidecar_bytes).hexdigest() != digest["semantic_sha256"]:
        fail(f"canonical companion sidecar/digest disagree: {row_id}")

    record_sections = {
        "schema": "contract",
        "contract": "contract",
        "initial_state": "contract",
        "candidate": "candidates",
        "candidate_lower_bound": "candidates",
        "candidate_rank": "candidates",
        "candidate_exact_verification_rank": "exact",
        "candidate_exact": "exact",
        "exact_evidence": "exact",
        "exact_keep_production": "exact",
        "exact_frontier_size": "exact",
        "exact_root_provenance_class": "exact",
        "fixed_topology_before_production": "exact",
        "fixed_topology_after_production": "exact",
        "iteration_begin": "acceptance",
        "iteration_outcome": "acceptance",
        "chain_base_production": "chain",
        "chain_entry": "chain",
        "final_state": "final_topology",
        "final_clade": "final_topology",
        "final_production": "final_topology",
    }
    section_bytes: dict[str, list[bytes]] = {
        name: []
        for name in (
            "contract",
            "candidates",
            "exact",
            "acceptance",
            "chain",
            "final_topology",
        )
    }

    by_record: dict[str, list[dict[str, object]]] = {}
    for record, raw_line in zip(records, raw_lines, strict=True):
        kind = record.get("record")
        if not isinstance(kind, str) or kind not in record_sections:
            fail(f"canonical companion record has an unknown kind {kind!r}: {row_id}")
        by_record.setdefault(kind, []).append(record)
        section_bytes[record_sections[kind]].append(raw_line)
    for section, chunks in section_bytes.items():
        field = f"{section}_sha256"
        if hashlib.sha256(b"".join(chunks)).hexdigest() != digest[field]:
            fail(
                f"canonical companion {section} component digest disagrees: "
                f"{row_id}"
            )
    schema_records = by_record.get("schema", [])
    if schema_records != [
        {
            "record": "schema",
            "schema": "larch.chart_spr.semantic.ndjson",
            "schema_version": 1,
        }
    ]:
        fail(f"canonical companion NDJSON schema record changed: {row_id}")
    if len(by_record.get("contract", [])) != 1 or len(
        by_record.get("initial_state", [])
    ) != 1:
        fail(f"canonical companion contract/initial-state cardinality changed: {row_id}")

    for field in count_fields:
        value = digest.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            fail(f"canonical companion digest count {field} is invalid: {row_id}")
    if digest["record_count"] != len(records):
        fail(
            f"canonical companion record count disagrees: "
            f"{digest['record_count']} != {len(records)}: {row_id}"
        )
    candidate_count = digest.get("candidate_count")
    exact_count = digest.get("exact_candidate_count")
    iteration_count = digest.get("iteration_count")
    candidates = by_record.get("candidate", [])
    if len(candidates) != candidate_count or any(
        not isinstance(record.get("signature"), str) or not record["signature"]
        for record in candidates
    ):
        fail(f"canonical companion candidate count/signatures disagree: {row_id}")
    if len(by_record.get("candidate_exact", [])) != exact_count:
        fail(f"canonical companion exact-candidate count disagrees: {row_id}")
    outcomes = by_record.get("iteration_outcome", [])
    begins = by_record.get("iteration_begin", [])
    if (
        len(outcomes) != iteration_count
        or len(begins) != iteration_count
        or any(
            "selected_signature" not in record
            or "accepted_move_committed" not in record
            for record in outcomes
        )
    ):
        fail(f"canonical companion iteration count/sequence disagrees: {row_id}")
    if exact_count:
        exact_evidence = by_record.get("exact_evidence", [])
        if not exact_evidence or any(
            "evidence_kind" not in record
            or
            "keep_mask_kind" not in record
            or "keep_production_exact" not in record
            for record in exact_evidence
        ):
            fail(f"canonical companion lacks exact-mask evidence: {row_id}")
        grammar_evidence = any(
            str(record["evidence_kind"]).startswith("grammar_exact")
            for record in exact_evidence
        )
        if grammar_evidence and not by_record.get(
            "exact_root_provenance_class", []
        ):
            fail(f"grammar companion lacks root provenance: {row_id}")
        fixed_evidence = any(
            record["evidence_kind"] == "fixed_topology_certificate"
            for record in exact_evidence
        )
        if fixed_evidence and not (
            by_record.get("fixed_topology_before_production", [])
            and by_record.get("fixed_topology_after_production", [])
        ):
            fail(f"fixed-topology companion lacks its certificate: {row_id}")


def validate_plan_field_coverage(
    capture: CaptureSpec,
    capture_root: Path,
    merged_rows: Sequence[Mapping[str, str]],
    spec_by_key: Mapping[tuple[str, str], RowSpec],
) -> Path:
    path = capture_root / "plan-field-coverage.tsv"
    rows = read_exact_tsv(path, PLAN_FIELD_COVERAGE_FIELDS)
    expected_rows = expected_plan_field_coverage_rows(
        capture, capture_root, merged_rows, spec_by_key
    )
    if rows != expected_rows:
        fail(f"plan-field coverage is not the exact closed ordered map: {path}")
    identities = [(row["plan_field"], row["row_id"]) for row in rows]
    if len(identities) != len(set(identities)):
        fail(f"plan-field coverage has duplicate field/row identities: {path}")
    rows_by_id: dict[str, list[Mapping[str, str]]] = {}
    for trial in merged_rows:
        rows_by_id.setdefault(spec_by_key[raw_key(trial)].row_id, []).append(trial)
    for row in rows:
        source = row["source_kind"]
        artifact_paths: list[str] = []
        if source in ("artifact_field", "canonical_sidecar"):
            artifact_paths = row["artifact_path"].split(";")
        elif source in (
            "canonical_digest",
            "capture_metadata",
            "command_log",
            "raw_tsv",
        ):
            artifact_paths = [row["artifact_path"]]
        elif source == "report":
            artifact_paths = ["raw_trials.tsv"]
            for trial in rows_by_id[row["row_id"]]:
                report = Path(trial["report_path"]).resolve(strict=True)
                try:
                    report.relative_to(capture_root.resolve(strict=True))
                except ValueError:
                    fail(f"plan-field report escapes capture root: {report}")
                if report.is_symlink() or not report.is_file():
                    fail(f"plan-field report is not an exact file: {report}")
        for artifact_path in artifact_paths:
            artifact = capture_root / artifact_path
            if artifact.is_symlink() or not artifact.is_file():
                fail(f"plan-field source artifact is not exact: {artifact}")

        if source == "artifact_field":
            json_artifacts = [
                capture_root / artifact_path
                for artifact_path in artifact_paths
                if artifact_path.endswith(".json")
            ]
            if not json_artifacts:
                fail(f"artifact-field coverage has no JSON source: {row}")
            for artifact in json_artifacts:
                try:
                    value = json.loads(artifact.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as error:
                    fail(f"artifact-field source is not parseable: {artifact}: {error}")
                for field in row["source_field"].split(","):
                    if not json_path_exists(value, field):
                        fail(f"artifact-field source lacks {field}: {artifact}")
        elif source == "command_log":
            artifact = capture_root / artifact_paths[0]
            try:
                value = json.loads(artifact.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                fail(f"command-log source is not parseable: {artifact}: {error}")
            validate_command_log_mapping(value, str(artifact))
            for field in row["source_field"].split(","):
                if not json_path_exists(value, field):
                    fail(f"command-log source lacks {field}: {artifact}")
        elif source == "capture_metadata":
            validate_capture_metadata_source(
                capture_root / artifact_paths[0], row["source_field"]
            )
        elif source == "canonical_digest":
            for trial in rows_by_id[row["row_id"]]:
                for field in row["source_field"].split(","):
                    if not re.fullmatch(r"[0-9a-f]{64}", trial.get(field, "")):
                        fail(
                            f"canonical-digest source lacks {field}: "
                            f"{row['row_id']}"
                        )
        elif source == "canonical_sidecar":
            validate_canonical_sidecar_source(
                capture_root, artifact_paths, row["row_id"]
            )
    return path


def validate_cross_worker_semantics(
    merged_rows: Sequence[Mapping[str, str]],
) -> None:
    by_method: dict[str, tuple[str, str]] = {}
    for row in merged_rows:
        if row["method"] == "sample_explore_merge" or row["status"] != "ok":
            continue
        value = row["search_semantic_sha256"], row["output_semantic_sha256"]
        previous = by_method.setdefault(row["method"], value)
        if previous != value:
            fail(
                "successful workers disagree semantically for "
                f"{row['method']}: {previous} != {value}"
            )


def validate_capture_evidence(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    rows: Sequence[RowSpec],
) -> tuple[list[dict[str, str]], list[str], dict[str, object]]:
    capture_root = paths["captures"] / capture.capture_id
    chart_keys = ordered_capture_chart_keys(capture)
    spec_by_key = capture_row_specs(capture, rows)
    timeout_specs = timeout_rows_for_capture(rows, capture.capture_id)
    allowed_timeout_keys = timeout_key_map(timeout_specs)
    characterization_dir = capture_root / "characterization"
    characterization_rows, any_timeout = validate_stage(
        capture_root,
        characterization_dir,
        capture,
        chart_keys,
        include_native=True,
        warmups=0,
        repetitions=1,
        allowed_timeout_keys=allowed_timeout_keys,
    )
    characterization_by_key = {
        raw_key(row): row for row in characterization_rows
    }
    native_key = ("sample_explore_merge", "native")
    native_trial1 = characterization_by_key[native_key]
    if native_trial1["status"] != "ok":
        fail(f"capture shared native peer did not succeed: {capture.capture_id}")
    raw_fields = list(characterization_rows[0])
    merged_path = capture_root / "raw_trials.tsv"
    merged_rows = read_raw_trials(merged_path)
    if list(merged_rows[0]) != raw_fields:
        fail(f"merged raw header differs from characterization: {capture.capture_id}")
    canonical_keys = set(chart_keys)
    native_spec = spec_by_key.get(native_key)
    if native_spec is not None:
        canonical_keys.add(native_key)
    if {raw_key(row) for row in merged_rows} != canonical_keys:
        fail(
            f"merged canonical key set changed for {capture.capture_id}: "
            f"{sorted({raw_key(row) for row in merged_rows})} != "
            f"{sorted(canonical_keys)}"
        )

    expected_merged: list[dict[str, str]] = []
    expected_peers: list[dict[str, str]] = []
    expected_ratios: list[dict[str, str]] = []
    ratios_by_key: dict[tuple[str, str], list[Decimal]] = {}
    expected_classifications: list[dict[str, str]] = []
    expected_repeat_records: list[dict[str, object]] = []
    if native_spec is not None:
        expected_merged.append(dict(native_trial1))
        native_stage, native_command = repeat_stage_command(
            metadata,
            paths,
            capture,
            native_key,
            native_spec.measured_trial_target,
        )
        native_repeat, native_timeout = validate_stage(
            capture_root,
            native_stage,
            capture,
            (),
            include_native=True,
            warmups=1,
            repetitions=native_spec.measured_trial_target - 1,
            allowed_timeout_keys={},
        )
        if native_timeout:
            fail(f"native finite repeat unexpectedly timed out: {native_stage}")
        for row in native_repeat:
            normalized = dict(row)
            normalized["trial_index"] = str(int(row["trial_index"]) + 1)
            expected_merged.append(normalized)
        native_stdout = capture_root / "repeats" / "native.stdout"
        native_stderr = capture_root / "repeats" / "native.stderr"
        expected_repeat_records.append(
            {
                "key": list(native_key),
                "row_id": native_spec.row_id,
                "stage": native_stage.relative_to(capture_root).as_posix(),
                "command": native_command,
                "harness_exit_code": 0,
                "raw_trials_sha256": sha256_file(native_stage / "raw_trials.tsv"),
                "summary_sha256": sha256_file(native_stage / "summary.tsv"),
                "stdout_sha256": sha256_file(native_stdout),
                "stderr_sha256": sha256_file(native_stderr),
                "canonical_rows": len(native_repeat),
                "pair_peer_rows": 0,
            }
        )
        expected_classifications.append(
            {
                "method": native_key[0],
                "requested_workers": native_key[1],
                "row_id": native_spec.row_id,
                "measured_trial_target": str(native_spec.measured_trial_target),
                "observed_trial_count": str(native_spec.measured_trial_target),
                "outcome": "ok",
                "evidence_kind": "finite_performance",
                "characterization_stage": "characterization",
                "repeat_stage": native_stage.relative_to(capture_root).as_posix(),
                "trial1_native_peer": "self",
            }
        )

    for key in chart_keys:
        spec = spec_by_key[key]
        trial1 = characterization_by_key[key]
        expected_merged.append(dict(trial1))
        peer = dict(native_trial1)
        peer.update(
            {
                "paired_method": key[0],
                "paired_requested_workers": key[1],
                "peer_role": "shared_characterization_trial1",
                "stage_id": "characterization",
                "logical_trial_index": "1",
            }
        )
        expected_peers.append(peer)
        if trial1["status"] == "timeout":
            expected_classifications.append(
                {
                    "method": key[0],
                    "requested_workers": key[1],
                    "row_id": spec.row_id,
                    "measured_trial_target": str(spec.measured_trial_target),
                    "observed_trial_count": "1",
                    "outcome": "timeout",
                    "evidence_kind": "timeout_characterization_non_performance",
                    "characterization_stage": "characterization",
                    "repeat_stage": "-",
                    "trial1_native_peer": "characterization:sample_explore_merge@native",
                }
            )
            repeat_path = capture_root / "repeats" / raw_key_slug(key)
            if path_occupied(repeat_path):
                fail(f"timeout characterization has a repeat stage: {repeat_path}")
            continue
        repeat_stage, repeat_command = repeat_stage_command(
            metadata, paths, capture, key, spec.measured_trial_target
        )
        repeat_rows, repeat_timeout = validate_stage(
            capture_root,
            repeat_stage,
            capture,
            (key,),
            include_native=True,
            warmups=1,
            repetitions=spec.measured_trial_target - 1,
            allowed_timeout_keys={},
        )
        if repeat_timeout:
            fail(f"finite repeat stage claims timeout: {repeat_stage}")
        chart_repeat = {
            int(row["trial_index"]): row
            for row in repeat_rows
            if raw_key(row) == key
        }
        native_repeat = {
            int(row["trial_index"]): row
            for row in repeat_rows
            if raw_key(row) == native_key
        }
        ratio_row, ratio = paired_ratio_row(
            key,
            spec,
            1,
            trial1,
            native_trial1,
            "characterization",
            "shared_characterization_trial1",
        )
        expected_ratios.append(ratio_row)
        ratios_by_key.setdefault(key, []).append(ratio)
        for stage_trial in range(1, spec.measured_trial_target):
            logical = stage_trial + 1
            chart_row = dict(chart_repeat[stage_trial])
            native_row = dict(native_repeat[stage_trial])
            chart_row["trial_index"] = str(logical)
            native_row["trial_index"] = str(logical)
            expected_merged.append(chart_row)
            peer = dict(native_row)
            peer.update(
                {
                    "paired_method": key[0],
                    "paired_requested_workers": key[1],
                    "peer_role": "isolated_repeat_peer",
                    "stage_id": repeat_stage.relative_to(capture_root).as_posix(),
                    "logical_trial_index": str(logical),
                }
            )
            expected_peers.append(peer)
            ratio_row, ratio = paired_ratio_row(
                key,
                spec,
                logical,
                chart_row,
                native_row,
                repeat_stage.relative_to(capture_root).as_posix(),
                "isolated_repeat_peer",
            )
            expected_ratios.append(ratio_row)
            ratios_by_key[key].append(ratio)
        slug = raw_key_slug(key)
        repeat_stdout = capture_root / "repeats" / f"{slug}.stdout"
        repeat_stderr = capture_root / "repeats" / f"{slug}.stderr"
        expected_repeat_records.append(
            {
                "key": list(key),
                "row_id": spec.row_id,
                "stage": repeat_stage.relative_to(capture_root).as_posix(),
                "command": repeat_command,
                "harness_exit_code": 0,
                "raw_trials_sha256": sha256_file(repeat_stage / "raw_trials.tsv"),
                "summary_sha256": sha256_file(repeat_stage / "summary.tsv"),
                "stdout_sha256": sha256_file(repeat_stdout),
                "stderr_sha256": sha256_file(repeat_stderr),
                "canonical_rows": len(chart_repeat),
                "pair_peer_rows": len(native_repeat),
            }
        )
        expected_classifications.append(
            {
                "method": key[0],
                "requested_workers": key[1],
                "row_id": spec.row_id,
                "measured_trial_target": str(spec.measured_trial_target),
                "observed_trial_count": str(spec.measured_trial_target),
                "outcome": "ok",
                "evidence_kind": "finite_performance",
                "characterization_stage": "characterization",
                "repeat_stage": repeat_stage.relative_to(capture_root).as_posix(),
                "trial1_native_peer": "characterization:sample_explore_merge@native",
            }
        )

    key_order = lambda row: (*raw_key(row), int(row["trial_index"]))
    expected_merged.sort(key=key_order)
    if merged_rows != expected_merged:
        fail(
            f"merged canonical raw trials are not an exact source-stage projection: "
            f"{capture.capture_id}"
        )
    for key in canonical_keys:
        key_rows = [row for row in merged_rows if raw_key(row) == key]
        trial_indices = [int(row["trial_index"]) for row in key_rows]
        spec = spec_by_key[key]
        if key_rows[0]["status"] == "timeout":
            if trial_indices != [1]:
                fail(f"timeout characterization trial set changed: {key}")
        elif trial_indices != list(range(1, spec.measured_trial_target + 1)):
            fail(f"finite logical trial indices are not exact: {key}: {trial_indices}")
        for row in key_rows:
            if row["execution_order"] != expected_execution_order(
                0, int(row["trial_index"])
            ):
                fail(f"logical paired order is not alternating: {key}: {row}")
        successes = [row for row in key_rows if row["status"] == "ok"]
        if successes:
            validate_repeated_unanimity(capture_root, key, successes)
    validate_repeated_unanimity(
        capture_root, native_key, (native_trial1, *expected_peers)
    )
    validate_cross_worker_semantics(merged_rows)

    expected_peers.sort(
        key=lambda row: (
            row["paired_method"],
            row["paired_requested_workers"],
            int(row["logical_trial_index"]),
        )
    )
    actual_peers = read_exact_tsv(
        capture_root / "pair_peer_trials.tsv",
        (*raw_fields, *PAIR_PEER_FIELDS),
    )
    if actual_peers != expected_peers:
        fail(f"pair-peer evidence is not an exact source projection: {capture.capture_id}")
    expected_ratios.sort(
        key=lambda row: (
            row["method"], row["requested_workers"], int(row["trial_index"])
        )
    )
    actual_ratios = read_exact_tsv(
        capture_root / "paired_ratios.tsv",
        PAIRED_RATIO_FIELDS,
        allow_empty=True,
    )
    if actual_ratios != expected_ratios:
        fail(f"paired-ratio evidence is not exactly recomputable: {capture.capture_id}")
    expected_classifications.sort(
        key=lambda row: (row["method"], row["requested_workers"])
    )
    actual_classifications = read_exact_tsv(
        capture_root / "trial_classification.tsv",
        TRIAL_CLASSIFICATION_FIELDS,
    )
    if actual_classifications != expected_classifications:
        fail(f"trial classification is not exact: {capture.capture_id}")
    summary_path = capture_root / "summary.tsv"
    if summary_path.read_bytes() != render_capture_summary(
        merged_rows, spec_by_key, ratios_by_key
    ):
        fail(f"capture summary is not exactly recomputable: {capture.capture_id}")
    coverage_path = validate_plan_field_coverage(
        capture, capture_root, merged_rows, spec_by_key
    )
    pre_path, pre_porcelain, pre = validate_live_evidence_file(
        metadata, capture, capture_root, "pre"
    )
    post_path, post_porcelain, post = validate_live_evidence_file(
        metadata, capture, capture_root, "post"
    )
    if int(post["unix_time_ns"]) < int(pre["unix_time_ns"]):
        fail(f"capture live pre/post chronology is inverted: {capture.capture_id}")
    for field in ("timestamp_utc", "timestamp_local"):
        if datetime.fromisoformat(str(post[field])) < datetime.fromisoformat(
            str(pre[field])
        ):
            fail(
                f"capture live pre/post {field} chronology is inverted: "
                f"{capture.capture_id}"
            )
    if (
        pre["git_porcelain_sha256"] != post["git_porcelain_sha256"]
        or pre["git_dirty"] != post["git_dirty"]
        or pre_porcelain.read_bytes() != post_porcelain.read_bytes()
    ):
        fail(f"capture Git state changed between live snapshots: {capture.capture_id}")
    if pre["static_bindings"] != post["static_bindings"]:
        fail(f"capture static bindings changed between snapshots: {capture.capture_id}")
    observed_ids = sorted(
        allowed_timeout_keys[raw_key(row)]
        for row in merged_rows
        if row["status"] == "timeout"
    )
    characterization_command = capture_command(
        metadata, paths, capture, out_dir=characterization_dir
    )
    stdout_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.stdout"
    stderr_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.stderr"
    expected_status: dict[str, object] = {
        "capture_id": capture.capture_id,
        "characterization_command": characterization_command,
        "characterization_harness_exit_code": 1 if any_timeout else 0,
        "characterization_raw_trials_sha256": sha256_file(
            characterization_dir / "raw_trials.tsv"
        ),
        "characterization_summary_sha256": sha256_file(
            characterization_dir / "summary.tsv"
        ),
        "characterization_stdout_sha256": sha256_file(stdout_path),
        "characterization_stderr_sha256": sha256_file(stderr_path),
        "timeout_eligible_row_ids": sorted(row.row_id for row in timeout_specs),
        "observed_timeout_row_ids": observed_ids,
        "repeat_stages": expected_repeat_records,
        "row_count": len(merged_rows),
        "timeout_rows": len(observed_ids),
        "raw_trials_sha256": sha256_file(merged_path),
        "summary_sha256": sha256_file(summary_path),
        "pair_peer_trials_sha256": sha256_file(
            capture_root / "pair_peer_trials.tsv"
        ),
        "paired_ratios_sha256": sha256_file(capture_root / "paired_ratios.tsv"),
        "trial_classification_sha256": sha256_file(
            capture_root / "trial_classification.tsv"
        ),
        "plan_field_coverage_sha256": sha256_file(coverage_path),
        "live_pre_sha256": sha256_file(pre_path),
        "live_pre_porcelain_sha256": sha256_file(pre_porcelain),
        "live_post_sha256": sha256_file(post_path),
        "live_post_porcelain_sha256": sha256_file(post_porcelain),
    }
    return merged_rows, observed_ids, expected_status


def approve_timeouts(args: argparse.Namespace) -> None:
    baseline_dir = Path(args.baseline_dir).resolve()
    metadata, paths = load_metadata(baseline_dir)
    assert_no_sealed_base(paths)
    verify_metadata_inputs(metadata)
    captures = {
        item.capture_id: item
        for item in (deserialize_capture(value) for value in metadata["captures"])  # type: ignore[index]
    }
    rows = [deserialize_row(value) for value in metadata["rows"]]  # type: ignore[index]
    capture = captures.get(args.capture_id)
    if capture is None:
        fail(f"unknown capture ID: {args.capture_id}")
    raw_rows, observed, expected_status = validate_capture_evidence(
        metadata, paths, capture, rows
    )
    supplied = list(args.allow_expected_timeout)
    if len(supplied) != len(set(supplied)):
        fail("duplicate --allow-expected-timeout approval")
    if sorted(supplied) != observed:
        fail(
            "post-capture timeout approval must name every and only observed "
            f"eligible timeout; observed={observed}, supplied={sorted(supplied)}"
        )
    if not observed:
        fail("capture has no observed timeout; no approval artifact is permitted")
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    if not status_path.is_file():
        fail(f"capture completion record is missing: {status_path}")
    try:
        capture_status = json.loads(status_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        fail(f"capture completion record is invalid: {status_path}: {error}")
    if capture_status != expected_status:
        fail(
            "capture completion record is not the exact command/outcome "
            f"contract for timeout approval: {capture.capture_id}"
        )
    approval_path = timeout_approval_path(paths, capture.capture_id)
    if path_occupied(approval_path):
        fail(f"timeout approval already exists; refusing overwrite: {approval_path}")
    approval = {
        "capture_id": capture.capture_id,
        "approved_observed_timeout_row_ids": observed,
        "capture_status_sha256": sha256_file(status_path),
        "raw_trials_sha256": sha256_file(
            paths["captures"] / capture.capture_id / "raw_trials.tsv"
        ),
    }
    write_text_exclusive(
        approval_path,
        json.dumps(approval, sort_keys=True, indent=2) + "\n",
        0o444,
    )
    print(f"approved exactly {len(observed)} observed timeout rows: {approval_path}")


PROCESS_METRICS_V2_FIELDS = {
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


def wrapper_calibration_median(values: Sequence[Decimal]) -> Decimal:
    if not values:
        fail("wrapper calibration has no values to summarize")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def wrapper_calibration_decimal_text(value: Decimal) -> str:
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text or "0"


def wrapper_calibration_ratio_text(value: Decimal) -> str:
    return format(
        value.quantize(
            WRAPPER_CALIBRATION_RATIO_PLACES, rounding=ROUND_HALF_EVEN
        ),
        ".12f",
    )


def wrapper_calibration_physical_cores() -> list[dict[str, int]]:
    cpus = (0, 2, 4, 6, 8, 10, 12, 14)
    result: list[dict[str, int]] = []
    identities: set[tuple[int, int]] = set()
    for cpu in cpus:
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        try:
            package_id = int(
                (topology / "physical_package_id")
                .read_text(encoding="ascii")
                .strip()
            )
            core_id = int(
                (topology / "core_id").read_text(encoding="ascii").strip()
            )
        except (FileNotFoundError, PermissionError, ValueError) as error:
            fail(
                f"cannot validate wrapper-calibration topology for CPU {cpu}: "
                f"{error}"
            )
        identity = (package_id, core_id)
        if identity in identities:
            fail(
                "wrapper-calibration physical cpuset aliases an SMT core: "
                f"{identity}"
            )
        identities.add(identity)
        result.append(
            {"core_id": core_id, "cpu": cpu, "package_id": package_id}
        )
    if len(identities) != 8:
        fail("wrapper calibration does not use eight distinct physical cores")
    return result


def require_exact_json_mapping(
    value: object, expected_keys: set[str], label: str
) -> dict[str, object]:
    if type(value) is not dict:
        fail(f"{label} is not a JSON object")
    result = value
    if set(result) != expected_keys:
        fail(
            f"{label} key set changed; "
            f"missing={sorted(expected_keys - set(result))}, "
            f"unexpected={sorted(set(result) - expected_keys)}"
        )
    return result


def validate_wrapper_calibration_stream(
    value: object, label: str
) -> dict[str, object]:
    result = require_exact_json_mapping(value, {"bytes", "sha256"}, label)
    if type(result["bytes"]) is not int or int(result["bytes"]) < 0:
        fail(f"{label} byte count is not a nonnegative JSON integer")
    if type(result["sha256"]) is not str or not re.fullmatch(
        r"[0-9a-f]{64}", str(result["sha256"])
    ):
        fail(f"{label} SHA-256 is not canonical lowercase hex")
    return result


def validate_wrapper_calibration_file_identity(
    value: object,
    label: str,
    *,
    expected_sha256: str,
    expected_mode: int | None = None,
) -> dict[str, object]:
    identity = require_exact_json_mapping(
        value,
        {
            "ctime_ns",
            "device",
            "inode",
            "mode",
            "mtime_ns",
            "sha256",
            "size_bytes",
        },
        label,
    )
    for key in ("ctime_ns", "device", "inode", "mode", "mtime_ns", "size_bytes"):
        if type(identity[key]) is not int:
            fail(f"{label} {key} is not an exact JSON integer")
    if (
        int(identity["device"]) < 0
        or int(identity["inode"]) <= 0
        or int(identity["size_bytes"]) <= 0
        or int(identity["mtime_ns"]) <= 0
        or int(identity["ctime_ns"]) <= 0
        or not 0 <= int(identity["mode"]) <= 0o7777
    ):
        fail(f"{label} has an invalid live filesystem identity")
    if (
        type(identity["sha256"]) is not str
        or identity["sha256"] != expected_sha256
        or not re.fullmatch(r"[0-9a-f]{64}", str(identity["sha256"]))
    ):
        fail(f"{label} SHA-256 differs from its bound calibration input")
    if expected_mode is not None and identity["mode"] != expected_mode:
        fail(f"{label} does not have exact read-only snapshot mode {expected_mode:o}")
    return identity


def validate_wrapper_calibration_file_closure(
    value: object,
    label: str,
    *,
    expected_sha256: str,
    expected_mode: int | None = None,
) -> dict[str, object]:
    closure = require_exact_json_mapping(value, {"end", "start"}, label)
    start = validate_wrapper_calibration_file_identity(
        closure["start"],
        f"{label} start",
        expected_sha256=expected_sha256,
        expected_mode=expected_mode,
    )
    end = validate_wrapper_calibration_file_identity(
        closure["end"],
        f"{label} end",
        expected_sha256=expected_sha256,
        expected_mode=expected_mode,
    )
    if start != end:
        fail(f"{label} identity/hash changed between calibration start and end")
    return closure


def validate_wrapper_calibration_directory_identity(
    value: object, label: str, *, expected_mode: int | None = None
) -> dict[str, object]:
    identity = require_exact_json_mapping(
        value,
        {"ctime_ns", "device", "inode", "mode", "mtime_ns"},
        label,
    )
    for key in ("ctime_ns", "device", "inode", "mode", "mtime_ns"):
        if type(identity[key]) is not int:
            fail(f"{label} {key} is not an exact JSON integer")
    if (
        int(identity["device"]) < 0
        or int(identity["inode"]) <= 0
        or int(identity["ctime_ns"]) <= 0
        or int(identity["mtime_ns"]) <= 0
        or not 0 <= int(identity["mode"]) <= 0o7777
        or (expected_mode is not None and identity["mode"] != expected_mode)
    ):
        fail(f"{label} has an invalid sealed directory identity/mode")
    return identity


def validate_wrapper_calibration_directory_closure(
    value: object, label: str, *, expected_mode: int
) -> dict[str, object]:
    closure = require_exact_json_mapping(value, {"end", "start"}, label)
    start = validate_wrapper_calibration_directory_identity(
        closure["start"], f"{label} start", expected_mode=expected_mode
    )
    end = validate_wrapper_calibration_directory_identity(
        closure["end"], f"{label} end", expected_mode=expected_mode
    )
    if start != end:
        fail(f"{label} identity/mode changed between calibration start and end")
    return closure


def validate_wrapper_calibration_inode_directory_closure(
    value: object, label: str
) -> dict[str, object]:
    closure = require_exact_json_mapping(value, {"end", "start"}, label)
    identities: list[dict[str, object]] = []
    for boundary in ("start", "end"):
        identity = require_exact_json_mapping(
            closure[boundary], {"device", "inode", "mode"},
            f"{label} {boundary}",
        )
        if any(type(identity[key]) is not int for key in identity) or (
            int(identity["device"]) < 0
            or int(identity["inode"]) <= 0
            or not 0 <= int(identity["mode"]) <= 0o7777
        ):
            fail(f"{label} {boundary} has an invalid directory inode binding")
        identities.append(identity)
    if identities[0] != identities[1]:
        fail(f"{label} inode/mode changed between calibration start and end")
    return closure


def validate_wrapper_calibration_input_provenance(
    value: object,
    *,
    expected_controller_sha256: str,
    expected_runner_sha256: str,
    expected_workload_sha256: str,
    expected_workload_source_sha256: str,
    label: str,
) -> dict[str, object]:
    roles = {
        "controller": (expected_controller_sha256, 0o555),
        "runner": (expected_runner_sha256, 0o555),
        "workload": (expected_workload_sha256, 0o555),
        "workload_source": (expected_workload_source_sha256, 0o444),
    }
    expected_keys = {
        "controller_execution",
        "input_snapshot_directory",
        "launch_state",
        "output_parent_directory",
        "runner_execution",
        "stage_directory",
        "workload_execution",
    }
    for role in roles:
        expected_keys.update({role, f"{role}_snapshot"})
    provenance = require_exact_json_mapping(value, expected_keys, label)
    identities: set[tuple[int, int]] = set()
    snapshot_closures: dict[str, dict[str, object]] = {}
    for role, (expected_sha256, snapshot_mode) in roles.items():
        live_closure = validate_wrapper_calibration_file_closure(
            provenance[role],
            f"{label} live {role}",
            expected_sha256=expected_sha256,
        )
        snapshot_closure = validate_wrapper_calibration_file_closure(
            provenance[f"{role}_snapshot"],
            f"{label} private {role} snapshot",
            expected_sha256=expected_sha256,
            expected_mode=snapshot_mode,
        )
        snapshot_closures[role] = snapshot_closure
        if snapshot_closure["start"]["size_bytes"] != live_closure["start"]["size_bytes"]:  # type: ignore[index]
            fail(f"{label} private {role} snapshot size differs from its live input")
        for identity in (live_closure["start"], snapshot_closure["start"]):  # type: ignore[index]
            key = (int(identity["device"]), int(identity["inode"]))  # type: ignore[index]
            if key in identities:
                fail(f"{label} aliases two input/snapshot roles to one inode")
            identities.add(key)
    for role in ("controller", "runner", "workload"):
        execution_closure = validate_wrapper_calibration_file_closure(
            provenance[f"{role}_execution"],
            f"{label} descriptor-bound {role} execution",
            expected_sha256=roles[role][0],
            expected_mode=roles[role][1],
        )
        if execution_closure != snapshot_closures[role]:
            fail(f"{label} {role} execution descriptor is not its snapshot inode")
    launch_mapping = require_exact_json_mapping(
        provenance["launch_state"], {"end", "start"},
        f"{label} sealed launch-state closure",
    )
    launch_start_raw = require_exact_json_mapping(
        launch_mapping["start"],
        {"ctime_ns", "device", "inode", "mode", "mtime_ns", "sha256", "size_bytes"},
        f"{label} sealed launch-state start",
    )
    launch_sha256 = launch_start_raw["sha256"]
    if type(launch_sha256) is not str or not re.fullmatch(
        r"[0-9a-f]{64}", launch_sha256
    ):
        fail(f"{label} sealed launch-state SHA-256 is invalid")
    launch_closure = validate_wrapper_calibration_file_closure(
        provenance["launch_state"],
        f"{label} sealed launch-state closure",
        expected_sha256=launch_sha256,
        expected_mode=0o444,
    )
    launch_inode = (
        int(launch_closure["start"]["device"]),  # type: ignore[index]
        int(launch_closure["start"]["inode"]),  # type: ignore[index]
    )
    if launch_inode in identities:
        fail(f"{label} sealed launch state aliases an input inode")
    identities.add(launch_inode)
    directory_closures = {
        name: validate_wrapper_calibration_directory_closure(
            provenance[name], f"{label} {name}", expected_mode=0o700
        )
        for name in ("stage_directory", "input_snapshot_directory")
    }
    output_parent = validate_wrapper_calibration_inode_directory_closure(
        provenance["output_parent_directory"],
        f"{label} output parent directory",
    )
    directory_inodes = {
        (
            int(closure["start"]["device"]),  # type: ignore[index]
            int(closure["start"]["inode"]),  # type: ignore[index]
        )
        for closure in directory_closures.values()
    }
    directory_inodes.add(
        (
            int(output_parent["start"]["device"]),  # type: ignore[index]
            int(output_parent["start"]["inode"]),  # type: ignore[index]
        )
    )
    if len(directory_inodes) != 3 or directory_inodes & identities:
        fail(f"{label} aliases sealed directories/filesystem input roles")
    return provenance


def validate_wrapper_calibration_wait4(
    value: object, label: str
) -> dict[str, object]:
    result = require_exact_json_mapping(
        value,
        {
            "core_dumped",
            "exit_code",
            "max_rss_kb",
            "system_us",
            "term_signal",
            "user_us",
        },
        label,
    )
    if type(result["core_dumped"]) is not bool or result["core_dumped"]:
        fail(f"{label} claims a core dump or has a non-boolean flag")
    exact_zero = ("exit_code", "term_signal")
    if any(type(result[key]) is not int or result[key] != 0 for key in exact_zero):
        fail(f"{label} is not a successful ordinary exit")
    for key in ("max_rss_kb", "system_us", "user_us"):
        if type(result[key]) is not int or int(result[key]) < 0:
            fail(f"{label} {key} is not a nonnegative JSON integer")
    if int(result["max_rss_kb"]) <= 0:
        fail(f"{label} has no wait4 max-RSS observation")
    return result


def validate_wrapper_calibration_metrics(
    value: object, label: str
) -> dict[str, str]:
    if type(value) is not dict or set(value) != PROCESS_METRICS_V2_FIELDS:
        observed = set(value) if type(value) is dict else set()
        fail(
            f"{label} is not the exact 41-key process-metrics mapping; "
            f"missing={sorted(PROCESS_METRICS_V2_FIELDS - observed)}, "
            f"unexpected={sorted(observed - PROCESS_METRICS_V2_FIELDS)}"
        )
    if any(type(item) is not str for item in value.values()):
        fail(f"{label} process-metrics values must all be JSON strings")
    metrics = dict(value)
    exact = {
        "schema_version": "2",
        "outcome": "exited",
        "exit_code": "0",
        "term_signal": "0",
        "timed_out": "0",
        "runner_exit_code": "0",
        "rss_kb_unit": "1024_bytes",
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
        "rss_limit_bytes": str(RSS_LIMIT_BYTES),
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
        "peak_sampled_process_count": "1",
    }
    changed = sorted(key for key, expected in exact.items() if metrics[key] != expected)
    if changed:
        fail(
            f"{label} reports a cap, timeout, failure, or lifecycle error: "
            f"{changed}"
        )
    duration_pattern = re.compile(r"(?:0|[1-9][0-9]*)\.[0-9]+")
    for key in ("wall_seconds", "user_seconds", "system_seconds"):
        if not duration_pattern.fullmatch(metrics[key]):
            fail(f"{label} has a noncanonical duration: {key}")
    if Decimal(metrics["wall_seconds"]) <= 0 or Decimal(
        metrics["user_seconds"]
    ) <= 0:
        fail(f"{label} has no positive wall/user duration")
    non_integer = {
        "outcome",
        "wall_seconds",
        "user_seconds",
        "system_seconds",
        "rss_kb_unit",
        "child_error_stage",
    }
    for key in PROCESS_METRICS_V2_FIELDS - non_integer:
        if not re.fullmatch(r"0|[1-9][0-9]*", metrics[key]):
            fail(f"{label} has a noncanonical unsigned metric: {key}")
    for key in (
        "max_rss_kb",
        "peak_sampled_rss_kb",
        "proc_status_samples",
        "proc_rss_samples",
        "proc_swap_samples",
        "proc_group_samples",
    ):
        if int(metrics[key]) <= 0:
            fail(f"{label} has no positive {key} observation")
    return metrics


def validate_wrapper_calibration_arm(
    value: object,
    label: str,
    *,
    wrapped: bool,
    expected_stdout: Mapping[str, object],
) -> dict[str, object]:
    keys = {
        "argv",
        "outer_clock",
        "outer_ended_ns",
        "outer_started_ns",
        "outer_wall_ns",
        "redirections",
        "stderr",
        "stdout",
        "wait4",
    }
    if wrapped:
        keys.update({"driver_stderr", "driver_stdout", "process_metrics"})
    arm = require_exact_json_mapping(value, keys, label)
    if arm["argv"] != WRAPPER_CALIBRATION_ARGV or arm[
        "redirections"
    ] != WRAPPER_CALIBRATION_REDIRECTIONS:
        fail(f"{label} workload argv/redirection contract changed")
    if arm["outer_clock"] != "CLOCK_MONOTONIC":
        fail(f"{label} did not use the exact outer monotonic clock")
    if any(
        type(arm[key]) is not int
        for key in ("outer_started_ns", "outer_ended_ns", "outer_wall_ns")
    ):
        fail(f"{label} monotonic interval fields are not exact JSON integers")
    if (
        int(arm["outer_started_ns"]) <= 0
        or int(arm["outer_ended_ns"]) <= int(arm["outer_started_ns"])
        or int(arm["outer_wall_ns"])
        != int(arm["outer_ended_ns"]) - int(arm["outer_started_ns"])
        or int(arm["outer_wall_ns"]) < WRAPPER_CALIBRATION_MIN_ARM_WALL_NS
    ):
        fail(f"{label} is shorter than the fixed five-second wall minimum")
    stdout = validate_wrapper_calibration_stream(arm["stdout"], f"{label} stdout")
    stderr = validate_wrapper_calibration_stream(arm["stderr"], f"{label} stderr")
    if stdout != expected_stdout:
        fail(f"{label} deterministic workload stdout changed")
    if stderr != {"bytes": 0, "sha256": EMPTY_SHA256}:
        fail(f"{label} workload stderr is not empty")
    wait4 = validate_wrapper_calibration_wait4(arm["wait4"], f"{label} wait4")
    wall_ns = Decimal(int(arm["outer_wall_ns"]))
    if wrapped:
        for stream_name in ("driver_stdout", "driver_stderr"):
            stream = validate_wrapper_calibration_stream(
                arm[stream_name], f"{label} {stream_name}"
            )
            if stream != {"bytes": 0, "sha256": EMPTY_SHA256}:
                fail(f"{label} process-wrapper driver output is not empty")
        metrics = validate_wrapper_calibration_metrics(
            arm["process_metrics"], f"{label} process metrics"
        )
        inner_wall_ns = Decimal(metrics["wall_seconds"]) * Decimal(
            1_000_000_000
        )
        inner_cpu_us = (
            Decimal(metrics["user_seconds"])
            + Decimal(metrics["system_seconds"])
        ) * Decimal(1_000_000)
        outer_cpu_us = Decimal(
            int(wait4["user_us"]) + int(wait4["system_us"])
        )
        wall_delta_ns = wall_ns - inner_wall_ns
        cpu_delta_us = outer_cpu_us - inner_cpu_us
        inner_rss_kb = int(metrics["max_rss_kb"])
        outer_rss_kb = int(wait4["max_rss_kb"])
        cpu_per_wall = (inner_cpu_us * 1000) / wall_ns
        if (
            inner_wall_ns < WRAPPER_CALIBRATION_MIN_ARM_WALL_NS
            or inner_cpu_us < WRAPPER_CALIBRATION_MIN_ARM_CPU_US
            or cpu_per_wall < WRAPPER_CALIBRATION_SATURATION_MIN
            or cpu_per_wall > WRAPPER_CALIBRATION_SATURATION_MAX
        ):
            fail(
                f"{label} lacks plausible eight-core saturation: "
                f"cpu/wall={cpu_per_wall}"
            )
        if not (
            -WRAPPER_CALIBRATION_WALL_EARLY_TOLERANCE_NS
            <= wall_delta_ns
            <= WRAPPER_CALIBRATION_WALL_MAX_DELTA_NS
        ):
            fail(
                f"{label} inner/outer wall clocks are not bound: "
                f"delta_ns={wall_delta_ns}"
            )
        if not (
            -WRAPPER_CALIBRATION_CPU_EARLY_TOLERANCE_US
            <= cpu_delta_us
            <= WRAPPER_CALIBRATION_CPU_MAX_DELTA_US
        ):
            fail(
                f"{label} inner/outer CPU usage is not bound: "
                f"delta_us={cpu_delta_us}"
            )
        if not (
            inner_rss_kb
            <= outer_rss_kb
            <= inner_rss_kb + WRAPPER_CALIBRATION_RSS_MAX_DELTA_KB
        ):
            fail(
                f"{label} inner/outer max RSS is not bound: "
                f"inner={inner_rss_kb}, outer={outer_rss_kb}"
            )
    else:
        cpu_us = Decimal(int(wait4["user_us"]) + int(wait4["system_us"]))
        cpu_per_wall = (cpu_us * 1000) / wall_ns
        if (
            cpu_us < WRAPPER_CALIBRATION_MIN_ARM_CPU_US
            or cpu_per_wall < WRAPPER_CALIBRATION_SATURATION_MIN
            or cpu_per_wall > WRAPPER_CALIBRATION_SATURATION_MAX
        ):
            fail(
                f"{label} lacks plausible eight-core saturation: "
                f"cpu/wall={cpu_per_wall}"
            )
    return arm


def validate_wrapper_calibration_mapping(
    value: object,
    *,
    expected_runner_sha256: str,
    expected_controller_sha256: str,
    expected_workload_sha256: str,
    expected_workload_source_sha256: str,
    label: str,
) -> dict[str, object]:
    top_keys = {
        "affinity_cpus",
        "clock",
        "controller",
        "environment",
        "input_provenance",
        "live_guard",
        "measured_pairs",
        "physical_cores",
        "preflight",
        "repo_revision",
        "rss_limit_bytes",
        "runner",
        "schema",
        "schema_version",
        "summary",
        "timeout_seconds",
        "warmup_pairs",
        "workload",
    }
    document = require_exact_json_mapping(value, top_keys, label)
    exact_top: dict[str, object] = {
        "affinity_cpus": PHYSICAL_AFFINITY,
        "clock": "CLOCK_MONOTONIC",
        "environment": WRAPPER_CALIBRATION_ENVIRONMENT,
        "repo_revision": FROZEN_REVISION,
        "rss_limit_bytes": RSS_LIMIT_BYTES,
        "schema": WRAPPER_CALIBRATION_SCHEMA,
        "schema_version": WRAPPER_CALIBRATION_SCHEMA_VERSION,
        "timeout_seconds": TIMEOUT_SECONDS,
    }
    for key, expected in exact_top.items():
        if document[key] != expected or type(document[key]) is not type(expected):
            fail(f"{label} {key} changed: {document[key]!r} != {expected!r}")
    expected_physical_cores = wrapper_calibration_physical_cores()
    physical_cores = document["physical_cores"]
    physical_types_exact = (
        type(physical_cores) is list
        and all(
            type(record) is dict
            and set(record) == {"core_id", "cpu", "package_id"}
            and all(type(record[key]) is int for key in record)
            for record in physical_cores
        )
    )
    if not physical_types_exact or physical_cores != expected_physical_cores:
        fail(
            f"{label} cpuset does not map to the exact eight live physical "
            "core identities"
        )

    controller = require_exact_json_mapping(
        document["controller"], {"schema", "sha256", "uri", "version"},
        f"{label} controller",
    )
    expected_controller: dict[str, object] = {
        "schema": WRAPPER_CALIBRATION_CONTROLLER_SCHEMA,
        "sha256": expected_controller_sha256,
        "uri": "repo://tools/wric_wrapper_calibration.py",
        "version": WRAPPER_CALIBRATION_CONTROLLER_VERSION,
    }
    for key, expected in expected_controller.items():
        if controller[key] != expected or type(controller[key]) is not type(expected):
            fail(
                f"{label} controller provenance differs from the exact repo source"
            )
    runner = require_exact_json_mapping(
        document["runner"],
        {"process_metrics_schema_version", "sha256", "uri"},
        f"{label} runner",
    )
    expected_runner: dict[str, object] = {
        "process_metrics_schema_version": 2,
        "sha256": expected_runner_sha256,
        "uri": "repo://build/bin/wric-process-metrics",
    }
    for key, expected in expected_runner.items():
        if runner[key] != expected or type(runner[key]) is not type(expected):
            fail(f"{label} is not bound to the exact build/bin process wrapper")
    if (
        expected_workload_source_sha256
        != WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256
    ):
        fail(
            f"{label} live workload source is not the hard-bound v1 source"
        )
    workload = require_exact_json_mapping(
        document["workload"],
        {
            "argv",
            "schema",
            "sha256",
            "source_sha256",
            "source_uri",
            "stderr_bytes",
            "stderr_sha256",
            "stdout_bytes",
            "stdout_sha256",
            "stdout_text",
            "uri",
            "version",
            "version_text",
        },
        f"{label} workload",
    )
    expected_workload_scalars: dict[str, object] = {
        "argv": WRAPPER_CALIBRATION_ARGV,
        "schema": WRAPPER_CALIBRATION_WORKLOAD_SCHEMA,
        "sha256": expected_workload_sha256,
        "source_sha256": WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256,
        "source_uri": "repo://tools/wric_wrapper_calibration_workload.cpp",
        "stderr_bytes": 0,
        "stderr_sha256": EMPTY_SHA256,
        "stdout_bytes": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_BYTES,
        "stdout_sha256": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_SHA256,
        "stdout_text": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_TEXT,
        "uri": "repo://build/bin/wric-wrapper-calibration-workload",
        "version": WRAPPER_CALIBRATION_WORKLOAD_VERSION,
        "version_text": WRAPPER_CALIBRATION_WORKLOAD_VERSION_TEXT,
    }
    for key, expected in expected_workload_scalars.items():
        if workload[key] != expected or type(workload[key]) is not type(expected):
            fail(f"{label} workload {key} differs from the fixed contract")
    validate_wrapper_calibration_input_provenance(
        document["input_provenance"],
        expected_controller_sha256=expected_controller_sha256,
        expected_runner_sha256=expected_runner_sha256,
        expected_workload_sha256=expected_workload_sha256,
        expected_workload_source_sha256=expected_workload_source_sha256,
        label=f"{label} input provenance",
    )
    expected_stdout = validate_wrapper_calibration_stream(
        {"bytes": workload["stdout_bytes"], "sha256": workload["stdout_sha256"]},
        f"{label} expected workload stdout",
    )
    if expected_stdout != {
        "bytes": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_BYTES,
        "sha256": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_SHA256,
    }:
        fail(f"{label} expected workload stdout bytes/hash changed")

    preflight = require_exact_json_mapping(
        document["preflight"],
        {"duration_ns", "max_logical_cpu_busy_ppm", "threshold_ppm"},
        f"{label} preflight",
    )
    if (
        type(preflight["duration_ns"]) is not int
        or int(preflight["duration_ns"]) < WRAPPER_CALIBRATION_MIN_PREFLIGHT_NS
        or type(preflight["max_logical_cpu_busy_ppm"]) is not int
        or not 0 <= int(preflight["max_logical_cpu_busy_ppm"]) <= WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM
        or preflight["threshold_ppm"] != WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM
        or type(preflight["threshold_ppm"]) is not int
    ):
        fail(f"{label} did not pass the exact five-second quiet preflight")
    live_guard = require_exact_json_mapping(
        document["live_guard"],
        {
            "coverage_duration_ns",
            "coverage_ended_ns",
            "coverage_started_ns",
            "final_heartbeat_ns",
            "first_heartbeat_ns",
            "forbidden_process_matches",
            "max_consecutive_heartbeat_gap_ns",
            "max_consecutive_heartbeat_gap_threshold_ns",
            "max_unselected_smt_busy_ppm",
            "minimum_scan_count",
            "poll_interval_ms",
            "scan_count",
            "scan_failures",
            "started_ns",
            "stopped_ns",
            "threshold_ppm",
        },
        f"{label} live guard",
    )
    expected_live = {
        "forbidden_process_matches": 0,
        "max_consecutive_heartbeat_gap_threshold_ns": (
            WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        ),
        "poll_interval_ms": WRAPPER_CALIBRATION_LIVE_POLL_MS,
        "scan_failures": 0,
        "threshold_ppm": WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM,
    }
    for key, expected in expected_live.items():
        if live_guard[key] != expected or type(live_guard[key]) is not int:
            fail(f"{label} live guard {key} changed")
    integer_fields = {
        "coverage_duration_ns",
        "coverage_ended_ns",
        "coverage_started_ns",
        "final_heartbeat_ns",
        "first_heartbeat_ns",
        "max_consecutive_heartbeat_gap_ns",
        "max_unselected_smt_busy_ppm",
        "minimum_scan_count",
        "scan_count",
        "started_ns",
        "stopped_ns",
    }
    if any(type(live_guard[key]) is not int for key in integer_fields):
        fail(f"{label} live guard timing/count evidence is not exact JSON integers")
    coverage_duration_ns = int(live_guard["coverage_duration_ns"])
    coverage_started_ns = int(live_guard["coverage_started_ns"])
    coverage_ended_ns = int(live_guard["coverage_ended_ns"])
    scan_count = int(live_guard["scan_count"])
    minimum_scan_count = max(
        2,
        (
            coverage_duration_ns
            + WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
            - 1
        )
        // WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        + 1,
    )
    if (
        coverage_duration_ns <= 0
        or coverage_ended_ns - coverage_started_ns != coverage_duration_ns
        or int(live_guard["started_ns"]) <= 0
        or not int(live_guard["started_ns"])
        <= int(live_guard["first_heartbeat_ns"])
        <= coverage_started_ns
        < coverage_ended_ns
        <= int(live_guard["final_heartbeat_ns"])
        <= int(live_guard["stopped_ns"])
        or int(live_guard["first_heartbeat_ns"])
        - int(live_guard["started_ns"])
        > WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        or coverage_started_ns - int(live_guard["first_heartbeat_ns"])
        > WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        or int(live_guard["final_heartbeat_ns"]) - coverage_ended_ns
        > WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        or int(live_guard["stopped_ns"])
        - int(live_guard["final_heartbeat_ns"])
        > WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        or int(live_guard["max_consecutive_heartbeat_gap_ns"]) <= 0
        or int(live_guard["max_consecutive_heartbeat_gap_ns"])
        > WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
        or scan_count < minimum_scan_count
        or live_guard["minimum_scan_count"] != minimum_scan_count
        or coverage_duration_ns
        > (scan_count - 1)
        * int(live_guard["max_consecutive_heartbeat_gap_ns"])
        or not 0
        <= int(live_guard["max_unselected_smt_busy_ppm"])
        <= WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM
    ):
        fail(f"{label} live unselected-SMT interference gate failed")

    warmups = document["warmup_pairs"]
    measured = document["measured_pairs"]
    if type(warmups) is not list or len(warmups) < 2:
        fail(f"{label} has fewer than two warmup pairs")
    if type(measured) is not list or len(measured) < 11:
        fail(f"{label} has fewer than eleven measured pairs")

    arm_sequence: list[dict[str, object]] = []

    def validate_pairs(pairs: list[object], kind: str) -> list[dict[str, object]]:
        result: list[dict[str, object]] = []
        for position, value_pair in enumerate(pairs, 1):
            pair = require_exact_json_mapping(
                value_pair, {"direct", "index", "order", "wrapped"},
                f"{label} {kind} pair {position}",
            )
            wanted_order = "direct_then_wrapped" if position % 2 else "wrapped_then_direct"
            if (
                type(pair["index"]) is not int
                or pair["index"] != position
                or pair["order"] != wanted_order
                or type(pair["order"]) is not str
            ):
                fail(f"{label} {kind} pairs are not consecutively alternating")
            direct = validate_wrapper_calibration_arm(
                pair["direct"], f"{label} {kind} pair {position} direct",
                wrapped=False, expected_stdout=expected_stdout,
            )
            wrapped_arm = validate_wrapper_calibration_arm(
                pair["wrapped"], f"{label} {kind} pair {position} wrapped",
                wrapped=True, expected_stdout=expected_stdout,
            )
            if direct["argv"] != wrapped_arm["argv"] or direct[
                "redirections"
            ] != wrapped_arm["redirections"]:
                fail(f"{label} {kind} pair {position} workload setup differs")
            first, second = (
                (direct, wrapped_arm)
                if wanted_order == "direct_then_wrapped"
                else (wrapped_arm, direct)
            )
            if int(first["outer_ended_ns"]) >= int(second["outer_started_ns"]):
                fail(f"{label} {kind} pair {position} arm order/interval is spliced")
            arm_sequence.extend((first, second))
            result.append(pair)
        return result

    validate_pairs(warmups, "warmup")
    measured_pairs = validate_pairs(measured, "measured")
    for previous, current in zip(arm_sequence[1::2], arm_sequence[2::2]):
        if int(previous["outer_ended_ns"]) >= int(current["outer_started_ns"]):
            fail(f"{label} arm intervals overlap, repeat, or violate global order")
    arm_wall_sum = sum(int(arm["outer_wall_ns"]) for arm in arm_sequence)
    if (
        coverage_started_ns > int(arm_sequence[0]["outer_started_ns"])
        or coverage_ended_ns < int(arm_sequence[-1]["outer_ended_ns"])
        or coverage_duration_ns < arm_wall_sum
    ):
        fail(f"{label} live watcher coverage does not contain every arm")
    direct_walls = [
        Decimal(int(pair["direct"]["outer_wall_ns"]))  # type: ignore[index]
        for pair in measured_pairs
    ]
    wrapped_walls = [
        Decimal(int(pair["wrapped"]["outer_wall_ns"]))  # type: ignore[index]
        for pair in measured_pairs
    ]
    paired_ratios = [
        wrapped_wall / direct_wall
        for direct_wall, wrapped_wall in zip(direct_walls, wrapped_walls)
    ]
    direct_median = wrapper_calibration_median(direct_walls)
    wrapped_median = wrapper_calibration_median(wrapped_walls)
    paired_median = wrapper_calibration_median(paired_ratios)
    ratio_of_medians = wrapped_median / direct_median
    if (
        paired_median > WRAPPER_CALIBRATION_THRESHOLD
        or ratio_of_medians > WRAPPER_CALIBRATION_THRESHOLD
    ):
        fail(
            f"{label} exceeds the strict 2% wrapper-overhead gate: "
            f"paired={paired_median}, medians={ratio_of_medians}"
        )
    summary = require_exact_json_mapping(
        document["summary"],
        {
            "decision",
            "direct_median_ns",
            "median_paired_ratio",
            "ratio_of_medians",
            "threshold",
            "wrapped_median_ns",
        },
        f"{label} summary",
    )
    expected_summary = {
        "decision": "PASS",
        "direct_median_ns": wrapper_calibration_decimal_text(direct_median),
        "median_paired_ratio": wrapper_calibration_ratio_text(paired_median),
        "ratio_of_medians": wrapper_calibration_ratio_text(ratio_of_medians),
        "threshold": wrapper_calibration_ratio_text(
            WRAPPER_CALIBRATION_THRESHOLD
        ),
        "wrapped_median_ns": wrapper_calibration_decimal_text(wrapped_median),
    }
    if summary != expected_summary:
        fail(f"{label} summary does not exactly recompute from measured pairs")
    return document


def load_wrapper_calibration(
    path: Path,
    *,
    expected_runner_sha256: str,
    expected_controller_sha256: str,
    expected_workload_sha256: str,
    expected_workload_source_sha256: str,
    label: str,
) -> dict[str, object]:
    if path.name != "wrapper-calibration.json":
        fail(f"{label} basename must be exactly wrapper-calibration.json")
    require_exact_regular_file(path, label, immutable=True)
    if path.absolute() != path.resolve(strict=True):
        fail(f"{label} path is not canonical/lexical: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not valid UTF-8 JSON: {error}")
    if path.read_bytes() != (
        json.dumps(value, sort_keys=True, indent=2) + "\n"
    ).encode("utf-8"):
        fail(f"{label} is not the exact canonical JSON serialization")
    return validate_wrapper_calibration_mapping(
        value,
        expected_runner_sha256=expected_runner_sha256,
        expected_controller_sha256=expected_controller_sha256,
        expected_workload_sha256=expected_workload_sha256,
        expected_workload_source_sha256=expected_workload_source_sha256,
        label=label,
    )


def read_process_metrics(
    path: Path,
    runner_status: int,
    expected_rss_limit_bytes: int | None = None,
) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        fail(f"direct real preflight omitted process metrics: {path}")
    values: dict[str, str] = {}
    for line in lines:
        if "=" not in line:
            fail(f"invalid process-metrics line: {line!r}: {path}")
        key, value = line.split("=", 1)
        if key in values:
            fail(f"duplicate process-metrics key {key!r}: {path}")
        values[key] = value
    if set(values) != PROCESS_METRICS_V2_FIELDS:
        fail(
            "process-metrics schema-v2 key set is not exact; "
            f"missing={sorted(PROCESS_METRICS_V2_FIELDS - set(values))}, "
            f"unexpected={sorted(set(values) - PROCESS_METRICS_V2_FIELDS)}: "
            f"{path}"
        )
    if values["schema_version"] != "2":
        fail(f"process-metrics schema_version is not exactly 2: {path}")
    unsigned = (
        "term_signal",
        "runner_exit_code",
        "max_rss_kb",
        "peak_sampled_rss_kb",
        "peak_sampled_swap_kb",
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
    )
    if not re.fullmatch(r"-?(0|[1-9][0-9]*)", values["exit_code"]):
        fail(f"invalid process exit code: {path}")
    for field in unsigned:
        if not re.fullmatch(r"0|[1-9][0-9]*", values[field]):
            fail(f"invalid unsigned process metric {field}: {path}")
    boolean = (
        "timed_out",
        "subreaper_enabled",
        "descendant_cleanup_kill_sent",
        "process_group_alive_at_return",
        "wait4_echild_at_return",
        "monitor_error",
        "wait4_collected",
        "core_dumped",
        "timeout_term_sent",
        "timeout_kill_sent",
        "rss_limit_enabled",
        "rss_limit_observed",
        "rss_limit_exceeded",
        "rss_limit_term_sent",
        "rss_limit_kill_sent",
    )
    for field in boolean:
        if values[field] not in ("0", "1"):
            fail(f"invalid boolean process metric {field}: {path}")
    for field in ("wall_seconds", "user_seconds", "system_seconds"):
        if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.[0-9]+", values[field]):
            fail(f"invalid duration process metric {field}: {path}")
    if values["runner_exit_code"] != str(runner_status):
        fail(f"process runner status disagrees with metrics: {path}")
    if values["rss_kb_unit"] != "1024_bytes":
        fail(f"process metrics lack exact RSS units: {path}")
    outcome = values["outcome"]
    outcomes = {
        "exited",
        "signaled",
        "timeout",
        "rss_limit",
        "exec_error",
        "setup_error",
        "descendant_leak",
        "monitor_error",
        "wait_error",
    }
    if outcome not in outcomes:
        fail(f"process outcome is outside the closed schema-v2 enum: {path}")
    stage = values["child_error_stage"]
    stages = {
        "none",
        "restore_sigchld",
        "set_process_group",
        "redirect_stdout",
        "redirect_stderr",
        "exec",
    }
    if stage not in stages:
        fail(f"process child-error stage is outside the closed enum: {path}")
    if (
        values["subreaper_enabled"] != "1"
        or values["live_descendants_at_return"] != "0"
        or values["process_group_alive_at_return"] != "0"
        or values["wait4_echild_at_return"] != "1"
    ):
        fail(f"process runner did not close its descendant lifecycle: {path}")
    if outcome != "wait_error" and values["wait4_collected"] != "1":
        fail(f"process runner did not collect the leader: {path}")
    if (outcome == "wait_error") != (int(values["wait_errno"]) > 0):
        fail(f"process wait-error outcome/errno disagree: {path}")
    if (outcome == "monitor_error") != (values["monitor_error"] == "1"):
        fail(f"process monitor-error outcome/flag disagree: {path}")
    if (values["monitor_error"] == "1") != (
        int(values["monitor_error_count"]) > 0
    ):
        fail(f"process monitor-error flag/count disagree: {path}")
    if (outcome == "timeout") != (values["timed_out"] == "1"):
        fail(f"process timeout outcome/flag disagree: {path}")
    rss_limit = int(values["rss_limit_bytes"])
    expected_enabled = "1" if rss_limit > 0 else "0"
    if values["rss_limit_enabled"] != expected_enabled:
        fail(f"process RSS-limit bytes/enabled flag disagree: {path}")
    if (
        expected_rss_limit_bytes is not None
        and rss_limit != expected_rss_limit_bytes
    ):
        fail(
            "process RSS limit differs from the direct preflight contract: "
            f"{rss_limit} != {expected_rss_limit_bytes}: {path}"
        )
    if values["rss_limit_observed"] == "1":
        if (
            values["rss_limit_enabled"] != "1"
            or int(values["rss_limit_trigger_bytes"]) <= rss_limit
        ):
            fail(f"process RSS observation lacks an over-limit trigger: {path}")
        if int(values["rss_limit_trigger_bytes"]) > int(
            values["peak_sampled_rss_kb"]
        ) * 1024:
            fail(f"process RSS trigger exceeds sampled peak RSS: {path}")
    elif values["rss_limit_trigger_bytes"] != "0":
        fail(f"unobserved process RSS limit has a trigger: {path}")
    if (outcome == "rss_limit") != (values["rss_limit_exceeded"] == "1"):
        fail(f"process RSS-limit outcome/flag disagree: {path}")
    if outcome == "timeout":
        if (
            runner_status != 124
            or values["rss_limit_exceeded"] != "0"
            or values["timeout_term_sent"] != "1"
            or values["rss_limit_term_sent"] != "0"
            or values["rss_limit_kill_sent"] != "0"
        ):
            fail(f"process timeout enforcement state is inconsistent: {path}")
    if outcome == "rss_limit":
        if (
            runner_status != 123
            or values["timed_out"] != "0"
            or values["rss_limit_enabled"] != "1"
            or values["rss_limit_observed"] != "1"
            or values["rss_limit_term_sent"] != "0"
            or values["rss_limit_kill_sent"] != "1"
            or values["timeout_term_sent"] != "0"
            or values["timeout_kill_sent"] != "0"
        ):
            fail(f"process RSS-limit enforcement state is inconsistent: {path}")
    elif (
        values["rss_limit_term_sent"] != "0"
        or values["rss_limit_kill_sent"] != "0"
    ):
        fail(f"non-RSS process outcome claims RSS enforcement: {path}")
    if outcome == "descendant_leak" and (
        runner_status != 125
        or int(values["post_leader_descendants"]) == 0
        or values["timed_out"] != "0"
    ):
        fail(f"process descendant-leak outcome is inconsistent: {path}")
    if outcome in ("monitor_error", "wait_error") and runner_status != 125:
        fail(f"process runner-error outcome status is not 125: {path}")
    if outcome == "exited" and (
        int(values["exit_code"]) < 0 or values["term_signal"] != "0"
    ):
        fail(f"exited process outcome has inconsistent child status: {path}")
    if outcome == "signaled" and (
        values["exit_code"] != "-1" or int(values["term_signal"]) == 0
    ):
        fail(f"signaled process outcome has inconsistent child status: {path}")
    if outcome == "exec_error" and stage != "exec":
        fail(f"process exec-error outcome lacks exec stage: {path}")
    if outcome == "setup_error" and stage in ("none", "exec"):
        fail(f"process setup-error outcome lacks setup stage: {path}")
    if outcome not in ("exec_error", "setup_error") and stage != "none":
        fail(f"ordinary process outcome claims a child-error stage: {path}")
    return values


def run_direct_measured(
    metadata: Mapping[str, object],
    capture: CaptureSpec,
    stdout_path: Path,
    stderr_path: Path,
    metrics_path: Path,
    command: Sequence[str],
) -> tuple[int, dict[str, str]]:
    runner = [
        "taskset",
        "-c",
        capture.affinity_cpus,
        str(metadata["process_metrics"]),
        "--timeout-seconds",
        str(TIMEOUT_SECONDS),
        "--rss-limit-bytes",
        str(capture.rss_limit_bytes),
        "--stdout",
        str(stdout_path),
        "--stderr",
        str(stderr_path),
        "--metrics",
        str(metrics_path),
        "--",
        *command,
    ]
    result = subprocess.run(
        runner, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if result.stdout:
        fail(f"process-metrics wrote unexpected wrapper stdout: {result.stdout!r}")
    if result.stderr:
        fail(f"process-metrics wrote unexpected wrapper stderr: {result.stderr!r}")
    return result.returncode, read_process_metrics(
        metrics_path, result.returncode, capture.rss_limit_bytes
    )


def real_chart_argv(
    metadata: Mapping[str, object],
    repo_root: Path,
    capture_dir: Path,
    capture: CaptureSpec,
    spec: RowSpec,
) -> tuple[list[str], str, Path]:
    primary, refseq = fixture_paths(repo_root, capture.fixture)
    assert refseq is not None
    primary_sha = sha256_file(primary)
    refseq_sha = sha256_file(refseq)
    canonical = manifest_chart_argv(capture, spec, primary_sha, refseq_sha)
    output = capture_dir / f"worker-{spec.requested_workers}.pb.gz"
    replacements = {
        "@binary:working_chart": str(metadata["frozen_oracle"]),
        f"@primary:{primary_sha}": str(primary),
        f"@refseq:{refseq_sha}": str(refseq),
        "@search-canonical-result": str(
            capture_dir / f"worker-{spec.requested_workers}.canonical.json"
        ),
        "@output": str(output),
    }
    actual = [replacements.get(token, token) for token in canonical]
    return actual, canonical_argv_digest(canonical), output


def capture_real_preflight(
    baseline_dir: Path,
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    rows: Sequence[RowSpec],
) -> None:
    if capture.capture_role != "real_preflight":
        fail(f"unsupported direct real capture role: {capture.capture_role}")
    capture_dir = paths["captures"] / capture.capture_id
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    for path in (capture_dir, status_path, paths["real_outcome_approval"]):
        if path_occupied(path):
            fail(f"real preflight destination already exists: {path}")
    capture_dir.mkdir(mode=0o755)
    repo_root = Path(str(metadata["repo_root"]))
    primary, refseq = fixture_paths(repo_root, "real20d")
    assert refseq is not None
    if (
        sha256_file(primary) != REAL20D_PRIMARY_SHA256
        or sha256_file(refseq) != REAL20D_REFSEQ_SHA256
    ):
        fail("real-20D fixture bytes differ from the frozen preflight contract")

    initial_stdout = capture_dir / "initial.stdout"
    initial_stderr = capture_dir / "initial.stderr"
    initial_metrics = capture_dir / "initial.process-metrics"
    initial_canonical = capture_dir / "initial.canonical-dag.json"
    initial_command = [
        str(metadata["frozen_oracle"]),
        "--tree-pb",
        str(primary),
        "--refseq",
        str(refseq),
        "--force-no-vcf",
        "--validate",
        "--dag-info",
        "--canonical-dag-result",
        str(initial_canonical),
    ]
    initial_status, initial_values = run_direct_measured(
        metadata,
        capture,
        initial_stdout,
        initial_stderr,
        initial_metrics,
        initial_command,
    )
    if (
        initial_status != 0
        or initial_values["exit_code"] != "0"
        or initial_values["term_signal"] != "0"
        or initial_values["timed_out"] != "0"
        or initial_values["rss_limit_exceeded"] != "0"
    ):
        fail("frozen initial real-20D scorer did not complete within both caps")
    try:
        initial_json = json.loads(initial_canonical.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"frozen initial real-20D semantic result is invalid: {error}")
    if (
        str(initial_json.get("parsimony_min")) != REAL20D_INITIAL_SCORE
        or initial_json.get("semantic_sha256") != REAL20D_INITIAL_SEMANTIC_SHA256
    ):
        fail(
            "frozen initial real-20D score/semantic digest changed: "
            f"{initial_json.get('parsimony_min')}/"
            f"{initial_json.get('semantic_sha256')}"
        )

    row_specs = sorted(
        (
            row
            for row in rows
            if row.capture_id == capture.capture_id and row.real_role == "preflight"
        ),
        key=lambda row: row.row_id,
    )
    if not row_specs:
        fail("real preflight has no frozen row specifications")
    trial_records: list[dict[str, object]] = []
    command_lines = ["#!/usr/bin/env bash", "set -euo pipefail"]
    for spec in row_specs:
        command, argv_sha, output = real_chart_argv(
            metadata, repo_root, capture_dir, capture, spec
        )
        if argv_sha != REAL20D_CANONICAL_ARGV_SHA256.get(spec.requested_workers):
            fail(
                "real preflight canonical argv changed from the independently "
                f"diagnosed contract: {spec.row_id}: {argv_sha}"
            )
        stdout_path = capture_dir / f"worker-{spec.requested_workers}.stdout"
        stderr_path = capture_dir / f"worker-{spec.requested_workers}.stderr"
        metrics_path = capture_dir / f"worker-{spec.requested_workers}.process-metrics"
        command_lines.append(shlex.join(command))
        runner_status, values = run_direct_measured(
            metadata,
            capture,
            stdout_path,
            stderr_path,
            metrics_path,
            command,
        )
        trial_records.append(
            {
                "row_id": spec.row_id,
                "requested_workers": spec.requested_workers,
                "command": command,
                "canonical_argv_sha256": argv_sha,
                "runner_exit_code": runner_status,
                "metrics": values,
                "stdout_sha256": sha256_file(stdout_path),
                "stderr_sha256": sha256_file(stderr_path),
                "metrics_sha256": sha256_file(metrics_path),
                "output_exists": path_occupied(output),
                "output_sha256": (
                    sha256_file(output)
                    if output.is_file() and not output.is_symlink()
                    else "-"
                ),
            }
        )
    write_text_exclusive(
        capture_dir / "commands.sh", "\n".join(command_lines) + "\n", 0o444
    )
    status = {
        "capture_id": capture.capture_id,
        "capture_role": capture.capture_role,
        "fixture_primary_sha256": REAL20D_PRIMARY_SHA256,
        "fixture_refseq_sha256": REAL20D_REFSEQ_SHA256,
        "initial_score": REAL20D_INITIAL_SCORE,
        "initial_semantic_sha256": REAL20D_INITIAL_SEMANTIC_SHA256,
        "initial_command": initial_command,
        "initial_runner_exit_code": initial_status,
        "initial_stdout_sha256": sha256_file(initial_stdout),
        "initial_stderr_sha256": sha256_file(initial_stderr),
        "initial_metrics_sha256": sha256_file(initial_metrics),
        "initial_canonical_sha256": sha256_file(initial_canonical),
        "rss_limit_bytes": capture.rss_limit_bytes,
        "memory_budget_bytes": capture.memory_budget_bytes,
        "trials": trial_records,
    }
    write_text_exclusive(
        status_path, json.dumps(status, sort_keys=True, indent=2) + "\n", 0o444
    )
    print(
        f"captured guarded direct real-20D preflight ({len(trial_records)} row); "
        "no outcome is approved or manifestable yet"
    )


def validate_real_preflight_status(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    rows: Sequence[RowSpec],
) -> dict[str, object]:
    capture_dir = paths["captures"] / capture.capture_id
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"real preflight completion record is invalid: {error}")
    exact_top = {
        "capture_id": capture.capture_id,
        "capture_role": "real_preflight",
        "fixture_primary_sha256": REAL20D_PRIMARY_SHA256,
        "fixture_refseq_sha256": REAL20D_REFSEQ_SHA256,
        "initial_score": REAL20D_INITIAL_SCORE,
        "initial_semantic_sha256": REAL20D_INITIAL_SEMANTIC_SHA256,
        "rss_limit_bytes": REAL_PREFLIGHT_BYTES,
        "memory_budget_bytes": MEMORY_BUDGET_BYTES,
    }
    for field, expected in exact_top.items():
        if status.get(field) != expected:
            fail(
                f"real preflight completion mismatch: {field}="
                f"{status.get(field)!r}, expected {expected!r}"
            )
    file_hashes = {
        "initial_stdout_sha256": capture_dir / "initial.stdout",
        "initial_stderr_sha256": capture_dir / "initial.stderr",
        "initial_metrics_sha256": capture_dir / "initial.process-metrics",
        "initial_canonical_sha256": capture_dir / "initial.canonical-dag.json",
    }
    for field, path in file_hashes.items():
        if not path.is_file() or status.get(field) != sha256_file(path):
            fail(f"real preflight initial artifact changed: {field}: {path}")
    primary, refseq = fixture_paths(Path(str(metadata["repo_root"])), "real20d")
    assert refseq is not None
    initial_command = [
        str(metadata["frozen_oracle"]),
        "--tree-pb",
        str(primary),
        "--refseq",
        str(refseq),
        "--force-no-vcf",
        "--validate",
        "--dag-info",
        "--canonical-dag-result",
        str(capture_dir / "initial.canonical-dag.json"),
    ]
    if status.get("initial_command") != initial_command:
        fail("real preflight initial scorer command changed")
    if status.get("initial_runner_exit_code") != 0:
        fail("real preflight initial scorer runner status changed")
    initial_metrics = read_process_metrics(
        capture_dir / "initial.process-metrics", 0, capture.rss_limit_bytes
    )
    for field, expected in {
        "outcome": "exited",
        "exit_code": "0",
        "term_signal": "0",
        "timed_out": "0",
        "subreaper_enabled": "1",
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
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
    }.items():
        if initial_metrics.get(field) != expected:
            fail(
                f"real preflight initial scorer process {field} changed: "
                f"{initial_metrics.get(field)!r} != {expected!r}"
            )
    try:
        initial_semantic = json.loads(
            (capture_dir / "initial.canonical-dag.json").read_text(encoding="utf-8")
        )
    except json.JSONDecodeError as error:
        fail(f"real preflight initial canonical result is invalid: {error}")
    if (
        str(initial_semantic.get("parsimony_min")) != REAL20D_INITIAL_SCORE
        or initial_semantic.get("semantic_sha256")
        != REAL20D_INITIAL_SEMANTIC_SHA256
    ):
        fail("real preflight initial canonical score/semantic bytes changed")
    trial_values = status.get("trials")
    if not isinstance(trial_values, list):
        fail("real preflight completion record lacks trials")
    expected_specs = sorted(
        (row for row in rows if row.capture_id == capture.capture_id),
        key=lambda row: row.row_id,
    )
    if len(trial_values) != len(expected_specs):
        fail("real preflight completion record has the wrong trial count")
    repo_root = Path(str(metadata["repo_root"]))
    for value, spec in zip(trial_values, expected_specs, strict=True):
        if not isinstance(value, dict) or value.get("row_id") != spec.row_id:
            fail("real preflight completion record has a wrong trial identity")
        command, argv_sha, output = real_chart_argv(
            metadata, repo_root, capture_dir, capture, spec
        )
        worker = spec.requested_workers
        stdout_path = capture_dir / f"worker-{worker}.stdout"
        stderr_path = capture_dir / f"worker-{worker}.stderr"
        metrics_path = capture_dir / f"worker-{worker}.process-metrics"
        runner_status = int(value.get("runner_exit_code", -999))
        metrics = read_process_metrics(
            metrics_path, runner_status, capture.rss_limit_bytes
        )
        exact_trial: dict[str, object] = {
            "row_id": spec.row_id,
            "requested_workers": worker,
            "command": command,
            "canonical_argv_sha256": argv_sha,
            "runner_exit_code": 1,
            "metrics": metrics,
            "stdout_sha256": REAL20D_STDOUT_SHA256,
            "stderr_sha256": REAL20D_REFUSAL_SHA256,
            "metrics_sha256": sha256_file(metrics_path),
            "output_exists": False,
            "output_sha256": "-",
        }
        if set(value) != set(exact_trial):
            fail(
                f"real preflight trial key set changed for {spec.row_id}: "
                f"missing={sorted(set(exact_trial) - set(value))}, "
                f"unexpected={sorted(set(value) - set(exact_trial))}"
            )
        for field, expected in exact_trial.items():
            if value.get(field) != expected:
                fail(
                    f"real preflight trial {spec.row_id} changed: {field}="
                    f"{value.get(field)!r}, expected {expected!r}"
                )
        require_absent_output(output, "expected-infeasible real preflight")
        if (
            stdout_path.stat().st_size != REAL20D_STDOUT_SIZE
            or sha256_file(stdout_path) != REAL20D_STDOUT_SHA256
            or stdout_path.read_text(encoding="utf-8") != REAL20D_STDOUT_TEXT
        ):
            fail(f"real preflight stdout differs from the frozen bytes: {spec.row_id}")
        if (
            stderr_path.stat().st_size != REAL20D_STDERR_SIZE
            or sha256_file(stderr_path) != REAL20D_REFUSAL_SHA256
        ):
            fail(f"real preflight stderr differs from the frozen bytes: {spec.row_id}")
        if (
            metrics["outcome"] != "exited"
            or metrics["exit_code"] != "1"
            or metrics["term_signal"] != "0"
            or metrics["timed_out"] != "0"
            or metrics["post_leader_descendants"] != "0"
            or metrics["descendant_cleanup_kill_sent"] != "0"
            or metrics["live_descendants_at_return"] != "0"
            or metrics["process_group_alive_at_return"] != "0"
            or metrics["wait4_echild_at_return"] != "1"
            or metrics["monitor_error"] != "0"
            or metrics["monitor_error_count"] != "0"
            or metrics["wait4_collected"] != "1"
            or metrics["wait_errno"] != "0"
            or metrics["child_error_stage"] != "none"
            or metrics["child_error_errno"] != "0"
            or metrics["core_dumped"] != "0"
            or metrics["timeout_term_sent"] != "0"
            or metrics["timeout_kill_sent"] != "0"
            or metrics["rss_limit_enabled"] != "1"
            or metrics["rss_limit_observed"] != "0"
            or metrics["rss_limit_exceeded"] != "0"
            or metrics["rss_limit_trigger_bytes"] != "0"
            or metrics["rss_limit_term_sent"] != "0"
            or metrics["rss_limit_kill_sent"] != "0"
            or int(metrics["peak_sampled_rss_kb"]) * 1024
            > REAL_PREFLIGHT_BYTES
        ):
            fail(
                "real preflight is not the exact ordinary exit-1, signal-0, "
                f"within-both-caps refusal: {spec.row_id}: {metrics}"
            )
        if not stderr_path.read_text(encoding="utf-8").endswith(
            REAL20D_REFUSAL_TEXT
        ):
            fail(f"real preflight stderr lacks the exact refusal suffix: {spec.row_id}")
        classification = classify_real_preflight_observation(
            exit_code=int(metrics["exit_code"]),
            term_signal=int(metrics["term_signal"]),
            timed_out=metrics["timed_out"] == "1",
            rss_limit_exceeded=metrics["rss_limit_exceeded"] == "1",
            core_dumped=metrics["core_dumped"] == "1",
            output_exists=path_occupied(output),
            stderr_sha256=sha256_file(stderr_path),
        )
        if classification != "expected_infeasible":
            fail(
                f"observed real preflight branch is not the frozen refusal: "
                f"{spec.row_id}: {classification}"
            )
    expected_status_keys = set(exact_top) | {
        "initial_command",
        "initial_runner_exit_code",
        *file_hashes,
        "trials",
    }
    if set(status) != expected_status_keys:
        fail(
            "real preflight completion key set changed; "
            f"missing={sorted(expected_status_keys - set(status))}, "
            f"unexpected={sorted(set(status) - expected_status_keys)}"
        )
    return status


def classify_real_preflight_observation(
    *,
    exit_code: int,
    term_signal: int,
    timed_out: bool,
    rss_limit_exceeded: bool,
    core_dumped: bool,
    output_exists: bool,
    stderr_sha256: str,
) -> str:
    """Classify a preflight without turning an arbitrary failure into a waiver."""

    if term_signal != 0 or core_dumped:
        return "blocked"
    if rss_limit_exceeded:
        return "scale_limit:rss_limit_bytes"
    if timed_out:
        return "scale_limit:timeout_seconds"
    if exit_code == 0 and output_exists:
        return "ok"
    if (
        exit_code == 1
        and not output_exists
        and stderr_sha256 == REAL20D_REFUSAL_SHA256
    ):
        return "expected_infeasible"
    return "blocked"


def approve_real_outcome(args: argparse.Namespace) -> None:
    if not args.confirm_real_outcome:
        fail("real outcome approval requires --confirm-real-outcome")
    baseline_dir = Path(args.baseline_dir).resolve()
    metadata, paths = load_metadata(baseline_dir)
    assert_no_sealed_base(paths)
    verify_metadata_inputs(metadata)
    captures = {
        item.capture_id: item
        for item in (deserialize_capture(value) for value in metadata["captures"])  # type: ignore[index]
    }
    rows = [deserialize_row(value) for value in metadata["rows"]]  # type: ignore[index]
    capture = captures.get(args.capture_id)
    if capture is None or capture.capture_role != "real_preflight":
        fail(f"capture is not the frozen real preflight: {args.capture_id}")
    expected_ids = sorted(
        row.row_id for row in rows if row.capture_id == capture.capture_id
    )
    supplied_ids = list(args.expected_infeasible)
    if len(supplied_ids) != len(set(supplied_ids)):
        fail("duplicate --expected-infeasible row approval")
    if sorted(supplied_ids) != expected_ids:
        fail(
            "real outcome approval must name every and only frozen refusal row; "
            f"expected={expected_ids}, supplied={sorted(supplied_ids)}"
        )
    if args.expected_reason_sha256 != REAL20D_REFUSAL_SHA256:
        fail(
            "--expected-reason-sha256 differs from the independently repeated "
            "real-20D refusal digest"
        )
    status = validate_real_preflight_status(metadata, paths, capture, rows)
    proof_sha = sha256_file(paths["real_refusal_proof"])
    if proof_sha != metadata["real20d_refusal_source_proof_sha256"]:
        fail("frozen real-20D refusal source proof changed after prepare")
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    if path_occupied(paths["real_outcome_approval"]):
        fail(
            "real outcome approval exists; refusing overwrite: "
            f"{paths['real_outcome_approval']}"
        )
    approval = {
        "capture_id": capture.capture_id,
        "approved_row_ids": expected_ids,
        "expected_outcome": "expected_infeasible",
        "expected_reason_code": "high_arity_refinement_refusal",
        "expected_reason_sha256": REAL20D_REFUSAL_SHA256,
        "capture_status_sha256": sha256_file(status_path),
        "capture_status": status,
        "refusal_source_proof_sha256": proof_sha,
        "frozen_oracle_sha256": metadata["frozen_oracle_sha256"],
        "fixture_primary_sha256": REAL20D_PRIMARY_SHA256,
        "fixture_refseq_sha256": REAL20D_REFSEQ_SHA256,
    }
    write_text_exclusive(
        paths["real_outcome_approval"],
        json.dumps(approval, sort_keys=True, indent=2) + "\n",
        0o444,
    )
    print(
        "approved the exact pre-existing real-20D high-arity refusal for "
        f"{len(expected_ids)} frozen row"
    )


def load_real_outcome_approval(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    rows: Sequence[RowSpec],
) -> dict[str, object]:
    status = validate_real_preflight_status(metadata, paths, capture, rows)
    try:
        approval = json.loads(
            paths["real_outcome_approval"].read_text(encoding="utf-8")
        )
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"real-20D outcome lacks valid explicit approval: {error}")
    expected_ids = sorted(
        row.row_id for row in rows if row.capture_id == capture.capture_id
    )
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    checks = {
        "capture_id": capture.capture_id,
        "approved_row_ids": expected_ids,
        "expected_outcome": "expected_infeasible",
        "expected_reason_code": "high_arity_refinement_refusal",
        "expected_reason_sha256": REAL20D_REFUSAL_SHA256,
        "capture_status_sha256": sha256_file(status_path),
        "capture_status": status,
        "refusal_source_proof_sha256": metadata[
            "real20d_refusal_source_proof_sha256"
        ],
        "frozen_oracle_sha256": metadata["frozen_oracle_sha256"],
        "fixture_primary_sha256": REAL20D_PRIMARY_SHA256,
        "fixture_refseq_sha256": REAL20D_REFSEQ_SHA256,
    }
    assert_exact_approval_mapping(approval, checks, "real-20D")
    return approval


def assert_exact_approval_mapping(
    approval: Mapping[str, object],
    expected_fields: Mapping[str, object],
    label: str,
) -> None:
    if set(approval) != set(expected_fields):
        fail(
            f"{label} approval key set changed; "
            f"missing={sorted(set(expected_fields) - set(approval))}, "
            f"unexpected={sorted(set(approval) - set(expected_fields))}"
        )
    for field, expected in expected_fields.items():
        if approval.get(field) != expected:
            fail(
                f"{label} approval changed: {field}={approval.get(field)!r}, "
                f"expected {expected!r}"
            )


def json_string(path: Path, key: str) -> str:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))[key]
    except (FileNotFoundError, json.JSONDecodeError, KeyError) as error:
        fail(f"cannot read JSON field {key!r} from {path}: {error}")
    return str(value)


def report_indented_value(path: Path, key: str, spaces: int, label: str) -> str:
    """Return one scalar at an exact indentation, rejecting ambiguity."""

    prefix = " " * spaces
    pattern = re.compile(rf"^{re.escape(prefix + key)}:\s*(.*?)\s*$")
    values = [
        match.group(1)
        for line in path.read_text(encoding="utf-8").splitlines()
        if (match := pattern.fullmatch(line)) is not None
    ]
    if len(values) != 1 or not values[0]:
        fail(
            f"chart report {label} {key!r} occurs {len(values)} times "
            f"at indentation {spaces}: {path}"
        )
    return values[0]


def report_value(path: Path, key: str) -> str:
    """Return one chart-search summary scalar (exactly two leading spaces)."""

    return report_indented_value(path, key, 2, "top-level scalar")


def report_counter_value(path: Path, key: str) -> str:
    """Return one scalar from the single chart-search counters mapping."""

    lines = path.read_text(encoding="utf-8").splitlines()
    headers = [index for index, line in enumerate(lines) if line == "  counters:"]
    if len(headers) != 1:
        fail(f"chart report counters section occurs {len(headers)} times: {path}")
    start = headers[0] + 1
    end = len(lines)
    for index in range(start, len(lines)):
        line = lines[index]
        if line.startswith("  ") and not line.startswith("    "):
            end = index
            break
    pattern = re.compile(rf"^    {re.escape(key)}:\s*(.*?)\s*$")
    values = [
        match.group(1)
        for line in lines[start:end]
        if (match := pattern.fullmatch(line)) is not None
    ]
    if len(values) != 1 or not values[0]:
        fail(
            f"chart report counter {key!r} occurs {len(values)} times in "
            f"the counters section: {path}"
        )
    return values[0]


def report_section_value(path: Path, section: str, key: str) -> str:
    """Return one four-space scalar from one named two-space mapping."""

    lines = path.read_text(encoding="utf-8").splitlines()
    headers = [
        index for index, line in enumerate(lines) if line == f"  {section}:"
    ]
    if len(headers) != 1:
        fail(
            f"chart report section {section!r} occurs {len(headers)} times: "
            f"{path}"
        )
    start = headers[0] + 1
    end = len(lines)
    for index in range(start, len(lines)):
        line = lines[index]
        if line.startswith("  ") and not line.startswith("    "):
            end = index
            break
    pattern = re.compile(rf"^    {re.escape(key)}:\s*(.*?)\s*$")
    values = [
        match.group(1)
        for line in lines[start:end]
        if (match := pattern.fullmatch(line)) is not None
    ]
    if len(values) != 1 or not values[0]:
        fail(
            f"chart report section {section!r} scalar {key!r} occurs "
            f"{len(values)} times: {path}"
        )
    return values[0]


def report_stop_reason(path: Path) -> str:
    """Collapse the manifest's singular stop reason only when every iteration agrees."""

    iterations = require_unsigned_text(
        report_value(path, "iterations"), f"report iterations {path}"
    )
    lines = path.read_text(encoding="utf-8").splitlines()
    headers = [
        index for index, line in enumerate(lines) if line == "  iteration_reports:"
    ]
    if len(headers) != 1:
        fail(
            f"chart report iteration_reports section occurs {len(headers)} "
            f"times: {path}"
        )
    start = headers[0] + 1
    end = len(lines)
    for index in range(start, len(lines)):
        line = lines[index]
        if line.startswith("  ") and not line.startswith("    "):
            end = index
            break
    section_lines = lines[start:end]
    item_pattern = re.compile(r"^    - iteration:\s*([0-9]+)\s*$")
    iteration_ids = [
        int(match.group(1))
        for line in section_lines
        if (match := item_pattern.fullmatch(line)) is not None
    ]
    if iteration_ids != list(range(iterations)):
        fail(
            "chart report iteration_reports identities are not the exact "
            f"0..{iterations - 1} sequence: {iteration_ids}: {path}"
        )
    item_indices = [
        index
        for index, line in enumerate(section_lines)
        if item_pattern.fullmatch(line) is not None
    ]
    stop_pattern = re.compile(r"^        stop_reason:\s*(.*?)\s*$")
    preamble_lines = section_lines[: item_indices[0]] if item_indices else section_lines
    if any(
        line == "      candidate_generation:"
        or stop_pattern.fullmatch(line) is not None
        for line in preamble_lines
    ):
        fail(
            "chart report iteration_reports section contains candidate-generation "
            f"evidence outside an iteration item: {path}"
        )
    reasons: list[str] = []
    for item_offset, item_start in enumerate(item_indices):
        item_end = (
            item_indices[item_offset + 1]
            if item_offset + 1 < len(item_indices)
            else len(section_lines)
        )
        item_lines = section_lines[item_start + 1 : item_end]
        generation_indices = [
            index
            for index, line in enumerate(item_lines)
            if line == "      candidate_generation:"
        ]
        if len(generation_indices) != 1:
            fail(
                "chart report iteration does not contain exactly one "
                "candidate_generation mapping: "
                f"iteration={iteration_ids[item_offset]}, "
                f"count={len(generation_indices)}: {path}"
            )
        generation_start = generation_indices[0] + 1
        generation_end = len(item_lines)
        for index in range(generation_start, len(item_lines)):
            line = item_lines[index]
            if line.startswith("      ") and not line.startswith("        "):
                generation_end = index
                break
        scoped_reasons = [
            match.group(1)
            for line in item_lines[generation_start:generation_end]
            if (match := stop_pattern.fullmatch(line)) is not None
        ]
        all_item_reasons = [
            match.group(1)
            for line in item_lines
            if (match := stop_pattern.fullmatch(line)) is not None
        ]
        if (
            len(scoped_reasons) != 1
            or len(all_item_reasons) != 1
            or not scoped_reasons[0]
        ):
            fail(
                "chart report iteration does not contain exactly one scoped "
                "candidate-generation stop reason: "
                f"iteration={iteration_ids[item_offset]}, "
                f"scoped={len(scoped_reasons)}, total={len(all_item_reasons)}: "
                f"{path}"
            )
        reasons.append(scoped_reasons[0])
    if len(set(reasons)) != 1:
        fail(
            "manifest has one stop-reason field but report iterations disagree: "
            f"{reasons}: {path}"
        )
    reason = reasons[0]
    if reason not in {"exhausted", "candidate_cap", "path_budget", "callback_stop"}:
        fail(f"chart report has an invalid stop reason {reason!r}: {path}")
    return reason


def structural_evidence_key(
    capture: CaptureSpec,
    spec: RowSpec,
    primary_sha: str,
    refseq_sha: str,
) -> tuple[str, ...]:
    contract = base_manifest_contract(capture, spec, primary_sha, refseq_sha)
    return tuple(contract[field] for field in STRUCTURAL_EVIDENCE_FIELDS)


def structural_evidence_key_sha256(key: Sequence[str]) -> str:
    data = "wric-timeout-structural-evidence-v1\n" + "".join(
        f"{field}={value}\n"
        for field, value in zip(STRUCTURAL_EVIDENCE_FIELDS, key, strict=True)
    )
    return sha256_bytes(data.encode("utf-8"))


def report_structural_values(report: Path, row_id: str) -> tuple[str, str, str]:
    refinement = report_value(report, "refinement_exactness")
    if refinement not in ("EXACT", "BOUNDED_REFINED_GRAMMAR"):
        fail(
            f"successful evidence report has invalid refinement_exactness "
            f"{refinement!r} for {row_id}: {report}"
        )
    cache = report_value(report, "cache_strategy")
    if cache not in (
        "all_active_patterns",
        "pattern_batches",
        "lazy_multisite_chart",
    ):
        fail(
            f"successful evidence report has invalid cache_strategy {cache!r} "
            f"for {row_id}: {report}"
        )
    effective_batch = report_value(report, "effective_pattern_batch_size")
    if not effective_batch.isdigit() or int(effective_batch) <= 0:
        fail(
            "successful evidence report has non-positive "
            f"effective_pattern_batch_size {effective_batch!r} for {row_id}: "
            f"{report}"
        )
    return refinement, cache, effective_batch


def collect_structural_evidence(
    repo_root: Path,
    baseline_dir: Path,
    captures: Mapping[str, CaptureSpec],
    rows: Sequence[RowSpec],
    fixture_hashes: Mapping[str, tuple[str, str]],
) -> dict[tuple[str, ...], StructuralEvidence]:
    """Freeze unanimous structural/cache fields from successful chart reports."""

    evidence: dict[tuple[str, ...], StructuralEvidence] = {}
    for spec in sorted(rows, key=lambda row: row.row_id):
        if spec.method == "sample_explore_merge" or spec.real_role == "preflight":
            continue
        capture = captures[spec.capture_id]
        capture_dir = baseline_dir / "bootstrap-phase0/captures" / capture.capture_id
        raw = find_raw_row(capture_dir, spec)
        if raw["status"] != "ok":
            continue
        report = Path(raw["report_path"]).resolve(strict=True)
        try:
            report.relative_to(capture_dir.resolve(strict=True))
        except ValueError:
            fail(f"structural evidence report escapes its capture: {report}")
        primary_sha, refseq_sha = fixture_hashes[capture.fixture]
        key = structural_evidence_key(
            capture, spec, primary_sha, refseq_sha
        )
        refinement, cache, effective_batch = report_structural_values(
            report, spec.row_id
        )
        sidecar, _ = exact_canonical_asset_pair(
            capture_dir, capture, (spec.method, spec.requested_workers)
        )
        full_refinement = str(sidecar_contract(sidecar)["refinement_exactness"])
        if full_refinement != refinement:
            fail(
                "successful timed/full reports disagree on structural evidence "
                f"for {spec.row_id}: timed={refinement}, full={full_refinement}"
            )
        candidate = StructuralEvidence(
            refinement_exactness=refinement,
            cache_strategy=cache,
            effective_pattern_batch_size=effective_batch,
            source_row_id=spec.row_id,
            source_report=report,
            source_report_sha256=sha256_file(report),
            evidence_key_sha256=structural_evidence_key_sha256(key),
        )
        previous = evidence.get(key)
        if previous is not None:
            previous_values = (
                previous.refinement_exactness,
                previous.cache_strategy,
                previous.effective_pattern_batch_size,
            )
            candidate_values = (refinement, cache, effective_batch)
            if previous_values != candidate_values:
                fail(
                    "compatible successful reports disagree on mandatory "
                    "timeout fields for evidence key "
                    f"{candidate.evidence_key_sha256}: "
                    f"{previous.source_row_id}={previous_values}, "
                    f"{spec.row_id}={candidate_values}"
                )
            # Iteration is sorted by row ID; keep the first report as a stable,
            # auditable witness after proving unanimity.
            continue
        evidence[key] = candidate

    missing: list[str] = []
    for spec in rows:
        if spec.method == "sample_explore_merge" or spec.real_role == "preflight":
            continue
        capture = captures[spec.capture_id]
        primary_sha, refseq_sha = fixture_hashes[capture.fixture]
        key = structural_evidence_key(capture, spec, primary_sha, refseq_sha)
        if key not in evidence:
            missing.append(spec.row_id)
    if missing:
        fail(
            "chart rows have no compatible successful report from which to "
            f"freeze mandatory timeout fields: {sorted(missing)}"
        )
    return evidence


def sidecar_contract(path: Path) -> dict[str, object]:
    contracts: list[dict[str, object]] = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                fail(
                    f"canonical sidecar has invalid JSON on line "
                    f"{line_number}: {path}: {error}"
                )
            if not isinstance(value, dict):
                fail(
                    f"canonical sidecar record is not an object on line "
                    f"{line_number}: {path}"
                )
            if value.get("record") == "contract":
                contracts.append(value)
    if len(contracts) != 1:
        fail(
            f"canonical sidecar has {len(contracts)} contract records; "
            f"expected exactly one: {path}"
        )
    return contracts[0]


def sidecar_keep_kind(path: Path) -> str:
    kinds: set[str] = set()
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            value = json.loads(line)
            if "keep_mask_kind" in value:
                kinds.add(str(value["keep_mask_kind"]))
    if not kinds:
        return "-"
    if len(kinds) != 1:
        fail(f"canonical sidecar has multiple keep-mask kinds: {path}: {sorted(kinds)}")
    return next(iter(kinds))


def canonical_argv_digest(arguments: Sequence[str]) -> str:
    parts = [b"wric-canonical-argv-v1\n", f"argc={len(arguments)}\n".encode()]
    for argument in arguments:
        encoded = argument.encode("utf-8")
        if b"\n" in encoded:
            fail("canonical argv token contains a newline")
        parts.append(str(len(encoded)).encode() + b":" + encoded + b"\n")
    return sha256_bytes(b"".join(parts))


def trial_digest(method: str, search: str, output: str, argv: str) -> str:
    data = (
        "wric-trial-semantic-v1\n"
        f"method={method}\n"
        f"search_semantic_sha256={search}\n"
        f"output_semantic_sha256={output}\n"
        f"canonical_argv_sha256={argv}\n"
    ).encode()
    return sha256_bytes(data)


def manifest_chart_argv(capture: CaptureSpec, spec: RowSpec, primary_sha: str, refseq_sha: str) -> list[str]:
    contract = base_manifest_contract(capture, spec, primary_sha, refseq_sha)
    if capture.fixture == "small":
        argv = ["@binary:working_chart", "--dag-pb", f"@primary:{primary_sha}"]
    else:
        argv = [
            "@binary:working_chart",
            "--tree-pb",
            f"@primary:{primary_sha}",
            "--refseq",
            f"@refseq:{refseq_sha}",
        ]
    argv += [
        "--validate",
        "--force-no-vcf",
        "--wric-polytomy-mode", contract["polytomy_mode"],
        "--wric-polytomy-max-exact-arity", contract["polytomy_max_exact_arity"],
        "--wric-polytomy-max-shapes", contract["polytomy_max_shapes"],
        "--wric-polytomy-max-productions", contract["polytomy_max_productions"],
        "--wric-polytomy-max-clades", contract["polytomy_max_clades"],
        "--wric-lazy-chart", contract["lazy_policy"],
        "--chart-spr-search",
        "--chart-spr-max-iterations", contract["iterations"],
        "--chart-spr-max-candidates", contract["chart_max_candidates"],
        "--chart-spr-top-k-exact", contract["chart_top_k_exact"],
        "--chart-spr-candidate-selection", contract["candidate_selection"],
        "--chart-spr-candidate-source", contract["candidate_source"],
        "--chart-spr-acceptance", contract["acceptance"],
    ]
    if contract["topology_selector"] != "none":
        argv += ["--chart-spr-topology-selector", contract["topology_selector"]]
    argv += [
        "--chart-spr-sampled-tree-count", contract["sampled_tree_count"],
        "--chart-spr-sampled-tree-radius", contract["sampled_tree_radius"],
        "--chart-spr-max-upward-path-expansions", contract["max_upward_path_expansions"],
        "--chart-spr-max-path-pairs", contract["max_path_pairs"],
        "--chart-spr-min-moved-clade-size", contract["min_moved_clade_size"],
        "--chart-spr-max-moved-clade-size", contract["max_moved_clade_size"],
        "--chart-spr-min-target-clade-size", contract["min_target_clade_size"],
        "--chart-spr-max-target-clade-size", contract["max_target_clade_size"],
        "--chart-spr-max-cached-patterns", contract["max_cached_patterns"],
        "--chart-spr-pattern-batch-size", contract["pattern_batch_size"],
        "--chart-spr-candidate-batch-size", contract["candidate_batch_size"],
        "--chart-spr-memory-budget", contract["memory_budget_bytes"],
        "--chart-spr-commit-mode", contract["commit_mode"],
        "--chart-spr-verification-mode", contract["verification_mode"],
        "--chart-bnb-dominance", contract["dominance_mode"],
    ]
    if contract["max_frontier_entries"] != "0":
        argv += ["--chart-bnb-max-frontier", contract["max_frontier_entries"]]
    argv += ["--seed", contract["seed"]]
    if contract["worker_option"] == "chart_spr_workers":
        argv += ["--chart-spr-workers", contract["requested_workers"]]
    argv += [
        "--chart-spr-canonical-result",
        "@search-canonical-result",
        "-o",
        "@output",
    ]
    return argv


def find_raw_row(capture_dir: Path, spec: RowSpec) -> dict[str, str]:
    rows = read_raw_trials(capture_dir / "raw_trials.tsv")
    wanted_worker = spec.requested_workers
    if spec.method == "sample_explore_merge":
        wanted_worker = "native"
    matches = [
        row
        for row in rows
        if row["method"] == spec.method
        and row["requested_workers"] == wanted_worker
    ]
    trial1 = [row for row in matches if row.get("trial_index") == "1"]
    if len(trial1) != 1:
        fail(
            f"wanted exactly one characterization trial for {spec.row_id}, "
            f"found {len(trial1)} "
            f"in {capture_dir}"
        )
    expected_count = 1 if trial1[0]["status"] == "timeout" else spec.measured_trial_target
    if len(matches) != expected_count:
        fail(
            f"row {spec.row_id} has {len(matches)}/{expected_count} canonical "
            f"trials in {capture_dir}"
        )
    return trial1[0]


def make_manifest_row(
    header: Sequence[str],
    repo_root: Path,
    baseline_dir: Path,
    capture: CaptureSpec,
    spec: RowSpec,
    raw: Mapping[str, str],
    structural_evidence: Mapping[tuple[str, ...], StructuralEvidence],
    timeout_evidence_records: list[dict[str, str]],
    real_outcome_approval: Mapping[str, object] | None,
) -> dict[str, str]:
    primary, refseq = fixture_paths(repo_root, capture.fixture)
    primary_sha = sha256_file(primary)
    refseq_sha = "-" if refseq is None else sha256_file(refseq)
    values = {field: "-" for field in header}
    values.update(base_manifest_contract(capture, spec, primary_sha, refseq_sha))
    values.update(
        {
            "row_id": spec.row_id,
            "run_group": spec.run_group,
            "workload_name": spec.workload_name,
            "fixture_id": {
                "small": "small-test-5-tree0",
                "medium": "medium-seedtree",
                "real20d": "real-20D-one-tree",
            }[capture.fixture],
            "primary_uri": f"repo://{normalized_repo_path(repo_root, primary)}",
            "secondary_uri": "-",
            "secondary_sha256": "-",
            "refseq_uri": "-" if refseq is None else f"repo://{normalized_repo_path(repo_root, refseq)}",
            "rss_limit_bytes": str(capture.rss_limit_bytes),
            "expected_reason_code": "-",
            "expected_reason_sha256": "-",
            "scale_resource": "-",
            "scale_limit": "-",
            "scale_largest_candidates": "-",
            "scale_largest_top_k": "-",
        }
    )
    native = spec.method == "sample_explore_merge"
    status = raw["status"]
    if native:
        values["expected_resolved_workers"] = "-"
    elif status in ("timeout", "expected_infeasible"):
        # A timeout or the pre-scheduler high-arity refusal did not emit a
        # complete chart report.  The requested option/token remains exact
        # command evidence, but neither a resolved count nor a product policy
        # was observed.
        values["expected_resolved_workers"] = "-"
        values["expected_worker_policy"] = "-"
    elif spec.requested_workers in ("auto", "default"):
        values["expected_resolved_workers"] = "policy"
    else:
        resolved = raw.get("resolved_workers", "")
        if resolved != spec.requested_workers:
            fail(
                f"explicit worker resolution changed for {spec.row_id}: "
                f"requested={spec.requested_workers}, resolved={resolved}"
            )
        values["expected_resolved_workers"] = resolved

    if spec.real_role == "preflight":
        if real_outcome_approval is None:
            fail(f"real preflight row lacks explicit outcome approval: {spec.row_id}")
        approved_ids = real_outcome_approval.get("approved_row_ids")
        if not isinstance(approved_ids, list) or spec.row_id not in approved_ids:
            fail(f"real preflight row is absent from exact approval: {spec.row_id}")
        if capture.fixture != "real20d" or spec.timeout_policy != "classify":
            fail(f"invalid real expected-infeasible row contract: {spec.row_id}")
        expected_argv_sha = canonical_argv_digest(
            manifest_chart_argv(capture, spec, primary_sha, refseq_sha)
        )
        if expected_argv_sha != REAL20D_CANONICAL_ARGV_SHA256.get(
            spec.requested_workers
        ):
            fail(f"real manifest argv changed from frozen contract: {spec.row_id}")
        values.update(
            {
                "expected_outcome": "expected_infeasible",
                "expected_timeout_trials": "0",
                "expected_reason_code": "high_arity_refinement_refusal",
                "expected_reason_sha256": str(
                    real_outcome_approval["expected_reason_sha256"]
                ),
                "expected_initial_score": REAL20D_INITIAL_SCORE,
                "expected_final_score": "-",
                "expected_validated_parsimony": "-",
                "oracle_search_semantic_sha256": "-",
                "oracle_output_semantic_sha256": "-",
                "oracle_trial_semantic_sha256": "-",
                "canonical_sidecar_uri": "-",
                "canonical_sidecar_sha256": "-",
                "oracle_report_uri": "-",
                "oracle_report_sha256": "-",
                "canonical_argv_sha256": expected_argv_sha,
            }
        )
        # The process refuses before constructing/reporting chart state.  All
        # runtime-only structural/result fields intentionally retain '-'; the
        # strict expected_infeasible schema rejects invented values here.
        return values

    evidence: StructuralEvidence | None = None
    if not native:
        evidence_key = structural_evidence_key(
            capture, spec, primary_sha, refseq_sha
        )
        evidence = structural_evidence.get(evidence_key)
        if evidence is None:
            fail(
                "chart row lacks unanimous compatible structural evidence: "
                f"{spec.row_id}"
            )
        values.update(
            {
                "expected_refinement_exactness": evidence.refinement_exactness,
                "expected_cache_strategy": evidence.cache_strategy,
                "expected_effective_pattern_batch_size": (
                    evidence.effective_pattern_batch_size
                ),
            }
        )

    values["expected_initial_score"] = raw.get("initial_validated_parsimony_min", "-")
    if status == "timeout":
        if native or spec.timeout_policy != "allow" or evidence is None:
            fail(
                f"row {spec.row_id} timed out without a frozen chart timeout "
                "allowance and compatible evidence"
            )
        try:
            source_report_uri = (
                "manifest://"
                + evidence.source_report.relative_to(baseline_dir).as_posix()
            )
        except ValueError:
            fail(
                "timeout structural evidence report is outside the baseline: "
                f"{evidence.source_report}"
            )
        timeout_evidence_records.append(
            {
                "row_id": spec.row_id,
                "capture_id": spec.capture_id,
                "source_row_id": evidence.source_row_id,
                "source_report_uri": source_report_uri,
                "source_report_sha256": evidence.source_report_sha256,
                "evidence_key_sha256": evidence.evidence_key_sha256,
                "expected_refinement_exactness": evidence.refinement_exactness,
                "expected_cache_strategy": evidence.cache_strategy,
                "expected_effective_pattern_batch_size": (
                    evidence.effective_pattern_batch_size
                ),
            }
        )
        values.update(
            {
                "expected_outcome": "timeout",
                "expected_timeout_trials": "1",
                "expected_final_score": "-",
                "expected_validated_parsimony": "-",
                "oracle_search_semantic_sha256": "-",
                "oracle_output_semantic_sha256": "-",
                "oracle_trial_semantic_sha256": "-",
                "canonical_sidecar_uri": "-",
                "canonical_sidecar_sha256": "-",
                "oracle_report_uri": "-",
                "oracle_report_sha256": "-",
            }
        )
        values["canonical_argv_sha256"] = (
            raw["canonical_argv_sha256"]
            if native
            else canonical_argv_digest(manifest_chart_argv(capture, spec, primary_sha, refseq_sha))
        )
        return values
    if status != "ok" or raw.get("validation_status") != "ok":
        fail(f"cannot manifest unsuccessful non-timeout row {spec.row_id}: {status}")

    values["expected_outcome"] = "ok"
    values["expected_timeout_trials"] = "0"
    output_sha = raw["output_semantic_sha256"]
    if native:
        search_sha = "-"
        argv_sha = raw["canonical_argv_sha256"]
        values.update(
            {
                "expected_final_score": raw["final_validated_parsimony_min"],
                "expected_validated_parsimony": raw["final_validated_parsimony_min"],
                "oracle_search_semantic_sha256": "-",
                "oracle_output_semantic_sha256": output_sha,
                "canonical_sidecar_uri": "-",
                "canonical_sidecar_sha256": "-",
                "oracle_report_uri": "-",
                "oracle_report_sha256": "-",
            }
        )
    else:
        search_sha = raw["search_semantic_sha256"]
        sidecar, oracle_report = exact_canonical_asset_pair(
            baseline_dir / "bootstrap-phase0/captures" / capture.capture_id,
            capture,
            (spec.method, spec.requested_workers),
        )
        contract = sidecar_contract(sidecar)
        timed_report = Path(raw["report_path"]).resolve(strict=True)
        capture_root = (baseline_dir / "bootstrap-phase0/captures" / capture.capture_id).resolve(strict=True)
        try:
            timed_report.relative_to(capture_root)
        except ValueError:
            fail(f"raw report path escapes its capture: {timed_report}")
        expected_fields = {
            "expected_final_compaction_exactness": "final_compaction_exactness_kind",
            "expected_chain_exactness": "chain_per_accept_exactness_label",
            "expected_active_patterns": "active_patterns",
            "expected_initial_clades": "initial_grammar_clades",
            "expected_initial_productions": "initial_grammar_productions",
            "expected_candidates_generated": "candidates_generated",
            "expected_iterations": "iterations",
            "expected_accepted_moves": "accepted_moves",
        }
        for manifest_field, report_field in expected_fields.items():
            value = report_value(timed_report, report_field)
            if value == "-":
                fail(f"timed report lacks {report_field} for {spec.row_id}")
            values[manifest_field] = value
        values["expected_stop_reason"] = report_stop_reason(timed_report)
        values.update(
            {
                "expected_keep_mask_kind": sidecar_keep_kind(sidecar),
                "expected_candidates_scored": raw["candidates_scored"],
                "expected_exact_verifications": raw["exact_verifications"],
                "expected_final_score": report_value(timed_report, "final_score"),
                "expected_validated_parsimony": raw["final_validated_parsimony_min"],
                "oracle_search_semantic_sha256": search_sha,
                "oracle_output_semantic_sha256": output_sha,
                "canonical_sidecar_uri": f"manifest://{sidecar.relative_to(baseline_dir).as_posix()}",
                "canonical_sidecar_sha256": search_sha,
                "oracle_report_uri": f"manifest://{oracle_report.relative_to(baseline_dir).as_posix()}",
                "oracle_report_sha256": sha256_file(oracle_report),
            }
        )
        timed_structural = report_structural_values(timed_report, spec.row_id)
        frozen_structural = (
            values["expected_refinement_exactness"],
            values["expected_cache_strategy"],
            values["expected_effective_pattern_batch_size"],
        )
        if timed_structural != frozen_structural:
            fail(
                f"timed report differs from unanimous structural evidence for "
                f"{spec.row_id}: timed={timed_structural}, "
                f"frozen={frozen_structural}"
            )
        if str(contract["refinement_exactness"]) != values["expected_refinement_exactness"]:
            fail(
                f"full sidecar differs from unanimous refinement evidence for "
                f"{spec.row_id}: full={contract['refinement_exactness']}, "
                f"frozen={values['expected_refinement_exactness']}"
            )
        if report_value(timed_report, "refinement_exactness") != values["expected_refinement_exactness"]:
            fail(
                f"timed/full refinement exactness mismatch for {spec.row_id}: "
                f"timed={report_value(timed_report, 'refinement_exactness')}, "
                f"full={values['expected_refinement_exactness']}"
            )
        argv_sha = canonical_argv_digest(manifest_chart_argv(capture, spec, primary_sha, refseq_sha))
    values["canonical_argv_sha256"] = argv_sha
    values["oracle_trial_semantic_sha256"] = trial_digest(spec.method, search_sha, output_sha, argv_sha)
    return values


def manifest_header(metadata: Mapping[str, object]) -> list[str]:
    command = [
        *frozen_harness_command(metadata),
        "--dagutil", str(metadata["frozen_oracle"]),
        "--larch2", str(metadata["frozen_larch2"]),
        "--process-metrics", str(metadata["process_metrics"]),
        "--print-workload-manifest-header",
    ]
    result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE)
    header = result.stdout.rstrip("\n").split("\t")
    if len(header) < 90 or len(header) != len(set(header)):
        fail("benchmark harness returned an invalid workload manifest header")
    return header


def write_detached_seal(path: Path) -> None:
    seal = path.with_name(path.name + ".sha256")
    write_bytes_exclusive(seal, detached_seal_bytes(path), 0o444)


def detached_seal_bytes(path: Path) -> bytes:
    return f"{sha256_file(path)}  {path.name}\n".encode("ascii")


def ensure_exact_file(path: Path, data: bytes, label: str) -> None:
    """Create once, or accept only the exact immutable bytes on resume."""

    if path_occupied(path):
        require_exact_regular_file(path, label, immutable=True)
        if path.read_bytes() != data:
            fail(f"resumed {label} differs from the required bytes: {path}")
        return
    write_bytes_exclusive(path, data, 0o444)


def ensure_detached_seal(path: Path, seal: Path) -> None:
    expected_path = path.with_name(path.name + ".sha256")
    if seal != expected_path:
        fail(f"internal detached-seal path mismatch: {seal} != {expected_path}")
    ensure_exact_file(seal, detached_seal_bytes(path), "detached artifact seal")


def validate_capture_completion(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    capture: CaptureSpec,
    rows: Sequence[RowSpec],
) -> list[dict[str, str]]:
    capture_dir = paths["captures"] / capture.capture_id
    raw_rows, observed_ids, exact_checks = validate_capture_evidence(
        metadata, paths, capture, rows
    )
    status_path = paths["bootstrap_dir"] / f"capture-{capture.capture_id}.status.json"
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(
            f"capture lacks a valid exclusive completion record "
            f"({capture.capture_id}): {error}"
        )
    if set(status) != set(exact_checks):
        fail(
            f"capture completion key set changed for {capture.capture_id}: "
            f"missing={sorted(set(exact_checks) - set(status))}, "
            f"unexpected={sorted(set(status) - set(exact_checks))}"
        )
    for field, expected in exact_checks.items():
        if status.get(field) != expected:
            fail(
                f"capture completion record mismatch for {capture.capture_id}: "
                f"{field}={status.get(field)!r}, expected {expected!r}"
            )
    approval_path = timeout_approval_path(paths, capture.capture_id)
    if observed_ids:
        try:
            approval = json.loads(approval_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError) as error:
            fail(
                "observed timeouts lack a valid separate post-capture approval "
                f"for {capture.capture_id}: {error}"
            )
        approval_checks = {
            "capture_id": capture.capture_id,
            "approved_observed_timeout_row_ids": observed_ids,
            "capture_status_sha256": sha256_file(status_path),
            "raw_trials_sha256": sha256_file(capture_dir / "raw_trials.tsv"),
        }
        assert_exact_approval_mapping(
            approval, approval_checks, f"timeout {capture.capture_id}"
        )
    elif path_occupied(approval_path):
        fail(
            "timeout approval exists for a capture with no observed timeout: "
            f"{approval_path}"
        )
    return raw_rows


def read_manifest_rows(path: Path) -> dict[str, dict[str, str]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        fail(f"workload manifest is missing: {path}")
    data_lines = [line for line in lines if not line.startswith("#")]
    if len(data_lines) < 2:
        fail(f"workload manifest has no data rows: {path}")
    reader = csv.DictReader(data_lines, dialect="excel-tab")
    fields = reader.fieldnames
    if (
        fields is None
        or any(not field for field in fields)
        or len(fields) != len(set(fields))
    ):
        fail(f"workload manifest data header is invalid: {path}")
    result: dict[str, dict[str, str]] = {}
    for row in reader:
        if None in row or set(row) != set(fields) or any(
            value is None for value in row.values()
        ):
            fail(f"workload manifest row has extra/missing cells: {path}")
        row_id = row.get("row_id", "")
        if not row_id or row_id in result:
            fail(f"workload manifest has an empty/duplicate row ID: {path}")
        result[row_id] = row
    return result


STRICT_SMOKE_ROW_IDS = (
    "p0-native-small-physical-i1-m50",
    "p0-small-dense64-grammar-lower-bound-heuristic-w1",
)

STRICT_SUCCESS_SEMANTIC_BINDINGS = (
    ("search_semantic_sha256", "oracle_search_semantic_sha256"),
    ("output_semantic_sha256", "oracle_output_semantic_sha256"),
    ("trial_semantic_sha256", "oracle_trial_semantic_sha256"),
    ("canonical_digest", "oracle_trial_semantic_sha256"),
    ("canonical_argv_sha256", "canonical_argv_sha256"),
)


def validate_success_semantic_manifest_bindings(
    raw: Mapping[str, str], manifest_row: Mapping[str, str], label: str
) -> None:
    for raw_field, manifest_field in STRICT_SUCCESS_SEMANTIC_BINDINGS:
        if raw.get(raw_field) != manifest_row.get(manifest_field):
            fail(
                f"{label} {raw_field} differs from the manifest oracle: "
                f"{raw.get(raw_field)!r} != {manifest_row.get(manifest_field)!r}"
            )


def smoke_stream_paths(status_path: Path) -> tuple[Path, Path]:
    suffix = ".status.json"
    if not status_path.name.endswith(suffix):
        raise AssertionError(f"unexpected strict-smoke status path: {status_path}")
    prefix = status_path.name.removesuffix(suffix)
    return (
        status_path.with_name(prefix + ".stdout"),
        status_path.with_name(prefix + ".stderr"),
    )


def strict_smoke_command(
    metadata: Mapping[str, object], manifest: Path, smoke_dir: Path
) -> list[str]:
    return [
        "taskset",
        "-c",
        str(metadata["physical_affinity"]),
        *frozen_harness_command(metadata),
        "--dagutil",
        str(metadata["frozen_oracle"]),
        "--larch2",
        str(metadata["frozen_larch2"]),
        "--process-metrics",
        str(metadata["process_metrics"]),
        "--out-dir",
        str(smoke_dir),
        "--workload-manifest",
        str(manifest),
        "--run-manifest-group",
        "p0-small-dense-physical",
        "--workers-list",
        "1",
        "--warmups",
        "0",
        "--repetitions",
        "1",
    ]


def validate_strict_smoke(
    metadata: Mapping[str, object],
    manifest: Path,
    smoke_dir: Path,
    status_path: Path,
) -> None:
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"strict manifest smoke completion record is invalid: {error}")
    raw_path = smoke_dir / "raw_trials.tsv"
    raw_rows = read_raw_trials(raw_path)
    if len(raw_rows) != len(STRICT_SMOKE_ROW_IDS):
        fail(
            f"strict manifest smoke emitted {len(raw_rows)} rows, expected "
            f"{len(STRICT_SMOKE_ROW_IDS)}"
        )
    by_id = {row.get("row_id", ""): row for row in raw_rows}
    if set(by_id) != set(STRICT_SMOKE_ROW_IDS):
        fail(
            "strict manifest smoke did not select the exact paired native/chart "
            f"rows: observed={sorted(by_id)}, expected={list(STRICT_SMOKE_ROW_IDS)}"
        )
    manifest_rows = read_manifest_rows(manifest)
    for row_id in STRICT_SMOKE_ROW_IDS:
        raw = by_id[row_id]
        if raw.get("status") != "ok" or raw.get("validation_status") != "ok":
            fail(
                "strict manifest smoke did not execute and validate the frozen "
                f"row: {row_id} status={raw.get('status')} "
                f"validation={raw.get('validation_status')}"
            )
        for field, expected in {
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
            "rss_limit_trigger_bytes": "0",
            "rss_limit_term_sent": "0",
            "rss_limit_kill_sent": "0",
            "process_rss_limit_bytes": str(RSS_LIMIT_BYTES),
        }.items():
            if raw.get(field) != expected:
                fail(
                    f"strict smoke {row_id} process {field} changed: "
                    f"{raw.get(field)!r} != {expected!r}"
                )
        manifest_row = manifest_rows.get(row_id)
        if manifest_row is None:
            fail(f"strict smoke row {row_id} is missing from {manifest}")
        manifest_cap = require_unsigned_text(
            manifest_row.get("rss_limit_bytes", ""),
            f"strict smoke {row_id} manifest RSS cap",
            positive=True,
        )
        if manifest_cap != RSS_LIMIT_BYTES:
            fail(
                f"strict smoke {row_id} manifest RSS cap changed: "
                f"{manifest_cap} != {RSS_LIMIT_BYTES}"
            )
        if raw.get("manifest_rss_limit_bytes") != str(manifest_cap):
            fail(
                f"strict smoke {row_id} raw manifest RSS cap changed: "
                f"{raw.get('manifest_rss_limit_bytes')!r} != {manifest_cap}"
            )
        validate_observed_rss_within_cap(
            raw, manifest_cap, f"strict smoke {row_id}"
        )
        validate_success_semantic_manifest_bindings(
            raw, manifest_row, f"strict smoke {row_id}"
        )
    exact_status = {
        "command": strict_smoke_command(metadata, manifest, smoke_dir),
        "manifest_sha256": sha256_file(manifest),
        "row_ids": list(STRICT_SMOKE_ROW_IDS),
        "harness_exit_code": 0,
        "raw_trials_sha256": sha256_file(raw_path),
    }
    smoke_stdout, smoke_stderr = smoke_stream_paths(status_path)
    for path in (smoke_stdout, smoke_stderr):
        if not path.is_file():
            fail(f"strict smoke stream artifact is missing: {path}")
    exact_status.update(
        {
            "stdout_sha256": sha256_file(smoke_stdout),
            "stderr_sha256": sha256_file(smoke_stderr),
        }
    )
    if set(status) != set(exact_status):
        fail(
            "strict smoke completion key set changed; "
            f"missing={sorted(set(exact_status) - set(status))}, "
            f"unexpected={sorted(set(status) - set(exact_status))}"
        )
    for field, expected in exact_status.items():
        if status.get(field) != expected:
            fail(
                f"strict smoke completion record mismatch: {field}="
                f"{status.get(field)!r}, expected {expected!r}"
            )


def execute_strict_smoke(
    metadata: Mapping[str, object],
    manifest: Path,
    smoke_dir: Path,
    status_path: Path,
) -> None:
    smoke_stdout, smoke_stderr = smoke_stream_paths(status_path)
    for path in (smoke_dir, status_path, smoke_stdout, smoke_stderr):
        if path_occupied(path):
            fail(f"strict smoke destination exists; refusing overwrite: {path}")
    command = strict_smoke_command(metadata, manifest, smoke_dir)
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if result.returncode != 0:
        fail(
            "real strict manifest smoke failed before artifact sealing: "
            f"exit={result.returncode}, stderr={result.stderr!r}"
        )
    write_text_exclusive(smoke_stdout, result.stdout, 0o444)
    write_text_exclusive(smoke_stderr, result.stderr, 0o444)
    raw_path = smoke_dir / "raw_trials.tsv"
    status = {
        "command": command,
        "manifest_sha256": sha256_file(manifest),
        "row_ids": list(STRICT_SMOKE_ROW_IDS),
        "harness_exit_code": result.returncode,
        "raw_trials_sha256": sha256_file(raw_path),
        "stdout_sha256": sha256_file(smoke_stdout),
        "stderr_sha256": sha256_file(smoke_stderr),
    }
    write_text_exclusive(
        status_path,
        json.dumps(status, sort_keys=True, indent=2) + "\n",
        0o444,
    )
    validate_strict_smoke(metadata, manifest, smoke_dir, status_path)


REAL_STRICT_SMOKE_ROW_IDS = (
    "p0-real20d-preflight-grammar-exact-w1",
    "p0-real20d-preflight-grammar-exact-w8",
)


def real_strict_smoke_command(
    metadata: Mapping[str, object], manifest: Path, smoke_dir: Path
) -> list[str]:
    return [
        "taskset",
        "-c",
        str(metadata["physical_affinity"]),
        *frozen_harness_command(metadata),
        "--dagutil",
        str(metadata["frozen_oracle"]),
        "--larch2",
        str(metadata["frozen_larch2"]),
        "--process-metrics",
        str(metadata["process_metrics"]),
        "--out-dir",
        str(smoke_dir),
        "--workload-manifest",
        str(manifest),
        "--run-manifest-group",
        "real-bounded",
        "--workers-list",
        "1,8",
        "--warmups",
        "0",
        "--repetitions",
        "1",
    ]


def validate_real_strict_smoke(
    metadata: Mapping[str, object],
    manifest: Path,
    smoke_dir: Path,
    status_path: Path,
) -> None:
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        fail(f"strict real-manifest smoke completion record is invalid: {error}")
    raw_path = smoke_dir / "raw_trials.tsv"
    raw_rows = read_raw_trials(raw_path)
    if len(raw_rows) != len(REAL_STRICT_SMOKE_ROW_IDS):
        fail(
            f"strict real smoke emitted {len(raw_rows)} rows, expected "
            f"{len(REAL_STRICT_SMOKE_ROW_IDS)}"
        )
    by_id = {row.get("row_id", ""): row for row in raw_rows}
    if set(by_id) != set(REAL_STRICT_SMOKE_ROW_IDS):
        fail(
            "strict real smoke did not select exact W1/W8 refusal rows: "
            f"observed={sorted(by_id)}"
        )
    manifest_rows = read_manifest_rows(manifest)
    for row_id in REAL_STRICT_SMOKE_ROW_IDS:
        raw = by_id[row_id]
        if (
            raw.get("status") != "expected_infeasible"
            or raw.get("validation_status") != "not_applicable"
            or raw.get("exit_code") != "1"
            or raw.get("term_signal") != "0"
            or raw.get("core_dumped") != "0"
            or raw.get("timed_out") != "0"
            or raw.get("monitor_error") != "0"
            or raw.get("runner_outcome") != "exited"
            or raw.get("runner_exit_code") != "1"
            or raw.get("rss_limit_enabled") != "1"
            or raw.get("rss_limit_observed") != "0"
            or raw.get("rss_limit_exceeded") != "0"
            or raw.get("rss_limit_trigger_bytes") != "0"
            or raw.get("rss_limit_term_sent") != "0"
            or raw.get("rss_limit_kill_sent") != "0"
            or raw.get("process_rss_limit_bytes") != str(REAL_PREFLIGHT_BYTES)
            or raw.get("manifest_rss_limit_bytes") != str(REAL_PREFLIGHT_BYTES)
            or raw.get("resolved_workers") != "NA"
            or raw.get("worker_policy") != "unobserved"
            or raw.get("canonical_argv_sha256")
            != manifest_rows[row_id]["canonical_argv_sha256"]
            or raw.get("initial_validated_parsimony_min") != REAL20D_INITIAL_SCORE
        ):
            fail(f"strict real smoke differs from frozen refusal row {row_id}: {raw}")
        validate_unavailable_semantic_sentinels(
            raw, f"strict real refusal row {row_id}"
        )
        validate_observed_rss_within_cap(
            raw, REAL_PREFLIGHT_BYTES, f"strict real refusal {row_id}"
        )
        report = Path(raw.get("report_path", "")).resolve(strict=True)
        try:
            report.relative_to(smoke_dir.resolve(strict=True))
        except ValueError:
            fail(f"strict real refusal report escapes smoke output: {report}")
        expected_output = smoke_dir / "outputs" / f"{report.stem}.pb.gz"
        require_absent_output(
            expected_output, f"strict real expected-infeasible row {row_id}"
        )
        stderr_path = report.with_suffix(".err")
        if (
            not stderr_path.is_file()
            or stderr_path.stat().st_size != REAL20D_STDERR_SIZE
            or sha256_file(stderr_path) != REAL20D_REFUSAL_SHA256
            or not stderr_path.read_text(encoding="utf-8").endswith(
                REAL20D_REFUSAL_TEXT
            )
        ):
            fail(f"strict real refusal stderr changed: {row_id}: {stderr_path}")
        if (
            report.stat().st_size != REAL20D_STDOUT_SIZE
            or sha256_file(report) != REAL20D_STDOUT_SHA256
            or report.read_text(encoding="utf-8") != REAL20D_STDOUT_TEXT
        ):
            fail(f"strict real refusal stdout changed: {row_id}: {report}")
        output_glob = list(
            (smoke_dir / "outputs").glob(
                f"*_{raw['method']}_trial1_w{raw['requested_workers']}.pb.gz"
            )
        )
        if output_glob:
            fail(f"strict real refusal unexpectedly created output: {output_glob}")
    output_root = smoke_dir / "outputs"
    if not output_root.is_dir() or output_root.is_symlink():
        fail(f"strict real refusal output namespace is not a real directory: {output_root}")
    with os.scandir(output_root) as entries:
        unexpected_outputs = [entry.name for entry in entries]
    if unexpected_outputs:
        fail(
            "strict real refusal output namespace is not empty: "
            f"{unexpected_outputs}"
        )
    exact_status = {
        "command": real_strict_smoke_command(metadata, manifest, smoke_dir),
        "manifest_sha256": sha256_file(manifest),
        "row_ids": list(REAL_STRICT_SMOKE_ROW_IDS),
        "harness_exit_code": 0,
        "raw_trials_sha256": sha256_file(raw_path),
    }
    smoke_stdout, smoke_stderr = smoke_stream_paths(status_path)
    for path in (smoke_stdout, smoke_stderr):
        if not path.is_file():
            fail(f"strict real-smoke stream artifact is missing: {path}")
    exact_status.update(
        {
            "stdout_sha256": sha256_file(smoke_stdout),
            "stderr_sha256": sha256_file(smoke_stderr),
        }
    )
    if set(status) != set(exact_status):
        fail(
            "strict real-smoke completion key set changed; "
            f"missing={sorted(set(exact_status) - set(status))}, "
            f"unexpected={sorted(set(status) - set(exact_status))}"
        )
    for field, expected in exact_status.items():
        if status.get(field) != expected:
            fail(
                f"strict real smoke completion mismatch: {field}="
                f"{status.get(field)!r}, expected {expected!r}"
            )


def execute_real_strict_smoke(
    metadata: Mapping[str, object],
    manifest: Path,
    smoke_dir: Path,
    status_path: Path,
) -> None:
    smoke_stdout, smoke_stderr = smoke_stream_paths(status_path)
    for path in (smoke_dir, status_path, smoke_stdout, smoke_stderr):
        if path_occupied(path):
            fail(f"strict real smoke destination exists: {path}")
    command = real_strict_smoke_command(metadata, manifest, smoke_dir)
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if result.returncode != 0:
        fail(
            "strict real expected-infeasible execution failed: "
            f"exit={result.returncode}, stderr={result.stderr!r}"
        )
    write_text_exclusive(smoke_stdout, result.stdout, 0o444)
    write_text_exclusive(smoke_stderr, result.stderr, 0o444)
    raw_path = smoke_dir / "raw_trials.tsv"
    status = {
        "command": command,
        "manifest_sha256": sha256_file(manifest),
        "row_ids": list(REAL_STRICT_SMOKE_ROW_IDS),
        "harness_exit_code": result.returncode,
        "raw_trials_sha256": sha256_file(raw_path),
        "stdout_sha256": sha256_file(smoke_stdout),
        "stderr_sha256": sha256_file(smoke_stderr),
    }
    write_text_exclusive(
        status_path, json.dumps(status, sort_keys=True, indent=2) + "\n", 0o444
    )
    validate_real_strict_smoke(metadata, manifest, smoke_dir, status_path)


@dataclasses.dataclass(frozen=True)
class DerivedPendingArtifacts:
    manifest: bytes
    field_evidence: bytes
    row_count: int


def derive_pending_artifacts(
    metadata: Mapping[str, object], paths: Mapping[str, Path]
) -> DerivedPendingArtifacts:
    """Reconstruct every pending byte from frozen plans and capture evidence."""

    baseline_dir = paths["baseline_dir"]
    captures = {
        item.capture_id: item
        for item in (
            deserialize_capture(value)
            for value in metadata["captures"]  # type: ignore[index]
        )
    }
    rows = [
        deserialize_row(value)
        for value in metadata["rows"]  # type: ignore[index]
    ]
    real_outcome_approval: dict[str, object] | None = None
    for capture in captures.values():
        if capture.capture_role == "real_preflight":
            real_outcome_approval = load_real_outcome_approval(
                metadata, paths, capture, rows
            )
        else:
            validate_capture_completion(metadata, paths, capture, rows)

    header = manifest_header(metadata)
    repo_root = Path(str(metadata["repo_root"]))
    fixture_hashes = {
        name: (
            sha256_file(fixture_paths(repo_root, name)[0]),
            "-"
            if fixture_paths(repo_root, name)[1] is None
            else sha256_file(fixture_paths(repo_root, name)[1]),  # type: ignore[arg-type]
        )
        for name in ("small", "medium", "real20d")
    }
    assert_unique_resolution(rows, captures, fixture_hashes)
    assert_wall_gate_pairs(rows, captures)
    structural_evidence = collect_structural_evidence(
        repo_root, baseline_dir, captures, rows, fixture_hashes
    )
    manifest_rows: list[dict[str, str]] = []
    timeout_evidence_records: list[dict[str, str]] = []
    fixture_initial_scores: dict[str, set[str]] = {}
    for spec in rows:
        capture = captures[spec.capture_id]
        raw = (
            {
                "status": "expected_infeasible",
                "resolved_workers": "NA",
                "worker_policy": "unobserved",
                "initial_validated_parsimony_min": REAL20D_INITIAL_SCORE,
            }
            if spec.real_role == "preflight"
            else find_raw_row(paths["captures"] / capture.capture_id, spec)
        )
        initial_score = str(
            require_unsigned_text(
                raw.get("initial_validated_parsimony_min", ""),
                f"manifest row {spec.row_id} initial validated parsimony",
            )
        )
        fixture_initial_scores.setdefault(capture.fixture, set()).add(
            initial_score
        )
        manifest_rows.append(
            make_manifest_row(
                header,
                repo_root,
                baseline_dir,
                capture,
                spec,
                raw,
                structural_evidence,
                timeout_evidence_records,
                real_outcome_approval,
            )
        )
    for fixture, scores in fixture_initial_scores.items():
        if len(scores) != 1:
            fail(
                "Phase-0 fixture has non-unanimous initial validated "
                f"parsimony across captures: {fixture}: {sorted(scores)}"
            )
    if len(manifest_rows) != len(rows):
        fail("derived manifest row count differs from the frozen row plan")
    if [row["row_id"] for row in manifest_rows] != [spec.row_id for spec in rows]:
        fail("derived manifest row order/identity differs from the frozen row plan")
    for row in manifest_rows:
        missing = [field for field in header if row.get(field, "") == ""]
        if missing:
            fail(f"manifest row {row['row_id']} has empty fields: {missing}")

    evidence_fields = (
        "row_id",
        "capture_id",
        "source_row_id",
        "source_report_uri",
        "source_report_sha256",
        "evidence_key_sha256",
        "expected_refinement_exactness",
        "expected_cache_strategy",
        "expected_effective_pattern_batch_size",
    )
    evidence_lines = ["\t".join(evidence_fields)]
    for record in sorted(timeout_evidence_records, key=lambda item: item["row_id"]):
        evidence_lines.append("\t".join(record[field] for field in evidence_fields))
    field_evidence = ("\n".join(evidence_lines) + "\n").encode("utf-8")

    preamble = [
        "# schema=wric_chart_parallelization_workloads",
        "# schema_version=1",
        "# kind=base",
        "# manifest_id=phase0-408434e",
        "# parent_sha256=-",
        f"# repo_revision={metadata['repo_revision']}",
        f"# merge_base={metadata['merge_base']}",
        f"# frozen_larch2_uri=repo://{normalized_repo_path(repo_root, Path(str(metadata['frozen_larch2'])))}",
        f"# frozen_larch2_sha256={metadata['frozen_larch2_sha256']}",
        f"# frozen_oracle_dagutil_uri=repo://{normalized_repo_path(repo_root, Path(str(metadata['frozen_oracle'])))}",
        f"# frozen_oracle_dagutil_sha256={metadata['frozen_oracle_sha256']}",
        f"# commands_uri=manifest://{paths['commands'].relative_to(baseline_dir).as_posix()}",
        f"# commands_sha256={sha256_file(paths['commands'])}",
    ]
    lines = [*preamble, "\t".join(header)]
    lines.extend("\t".join(row[field] for field in header) for row in manifest_rows)
    return DerivedPendingArtifacts(
        manifest=("\n".join(lines) + "\n").encode("utf-8"),
        field_evidence=field_evidence,
        row_count=len(manifest_rows),
    )


def require_derived_bytes(path: Path, expected: bytes, label: str) -> None:
    require_exact_regular_file(path, label)
    if path.read_bytes() != expected:
        fail(f"{label} differs from its complete semantic reconstruction: {path}")


def finalize(args: argparse.Namespace) -> None:
    baseline_dir = Path(args.baseline_dir).resolve()
    metadata, paths = load_metadata(baseline_dir)
    assert_no_sealed_base(paths)
    verify_metadata_inputs(metadata)
    for path in (
        paths["pending_manifest"],
        paths["pending_seal"],
        paths["artifact_manifest"],
        paths["artifact_manifest_seal"],
        paths["field_evidence"],
        paths["pending_smoke"],
        paths["pending_smoke_status"],
        *smoke_stream_paths(paths["pending_smoke_status"]),
        paths["pending_real_smoke"],
        paths["pending_real_smoke_status"],
        *smoke_stream_paths(paths["pending_real_smoke_status"]),
        paths["seal_transaction"],
        *final_transaction_destinations(paths),
    ):
        if path_occupied(path):
            fail(f"finalize destination exists; refusing overwrite: {path}")
    derived = derive_pending_artifacts(metadata, paths)
    repo_root = Path(str(metadata["repo_root"]))
    write_bytes_exclusive(paths["field_evidence"], derived.field_evidence, 0o444)
    write_bytes_exclusive(paths["pending_manifest"], derived.manifest, 0o444)
    write_detached_seal(paths["pending_manifest"])
    strict_manifest_schema_audit(metadata, paths, paths["pending_manifest"], "pending")
    execute_strict_smoke(
        metadata,
        paths["pending_manifest"],
        paths["pending_smoke"],
        paths["pending_smoke_status"],
    )
    execute_real_strict_smoke(
        metadata,
        paths["pending_manifest"],
        paths["pending_real_smoke"],
        paths["pending_real_smoke_status"],
    )
    artifact_paths = collect_artifact_paths(metadata, paths, include_final=False)
    write_artifact_ledger(
        repo_root,
        paths["artifact_manifest"],
        paths["artifact_manifest_seal"],
        artifact_paths,
    )
    audit_pending(baseline_dir)
    print(f"wrote and audited {derived.row_count} rows: {paths['pending_manifest']}")
    print(f"artifact hashes: {paths['artifact_manifest']}")
    print("The base remains pending; promotion requires the separate seal command.")


def verify_detached_seal(path: Path, seal: Path) -> None:
    require_exact_regular_file(path, "sealed artifact")
    require_exact_regular_file(seal, "detached artifact seal")
    expected = f"{sha256_file(path)}  {path.name}\n"
    if seal.read_text(encoding="ascii") != expected:
        fail(f"detached seal is not exact GNU sha256sum format: {seal}")


def collect_artifact_paths(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    *,
    include_final: bool,
) -> set[Path]:
    repo_root = Path(str(metadata["repo_root"]))
    required_root_evidence = (
        paths["capture_metadata"],
        paths["native_loop_proof"],
        paths["provenance_commands"],
        paths["unsealed_inputs"],
        paths["frozen_wrapper_calibration"],
        paths["frozen_calibration_controller"],
        paths["frozen_calibration_workload_source"],
        paths["frozen_calibration_workload"],
    )
    for path in required_root_evidence:
        if not path.is_file():
            fail(f"required Phase-0 provenance evidence is missing: {path}")
    result: set[Path] = {
        paths["pending_manifest"],
        paths["pending_seal"],
        paths["commands"],
        paths["metadata"],
        paths["capture_plan"],
        paths["row_plan"],
        paths["timeout_plan"],
        paths["prepared_contract"],
        paths["prepared_contract_seal"],
        paths["field_evidence"],
        paths["frozen_helper"],
        *required_root_evidence,
        Path(str(metadata["frozen_larch2"])),
        Path(str(metadata["frozen_oracle"])),
        Path(str(metadata["process_metrics"])),
        Path(str(metadata["harness"])),
    }
    for fixture in ("small", "medium", "real20d"):
        result.update(
            path for path in fixture_paths(repo_root, fixture) if path is not None
        )
    for path in paths["bootstrap_dir"].rglob("*"):
        if path.is_symlink():
            fail(f"bootstrap artifact closure contains a symlink/alias: {path}")
        if path.is_file():
            result.add(path)
        elif not path.is_dir():
            fail(f"bootstrap artifact closure contains a non-file entry: {path}")
    if include_final:
        result.update(
            {
                paths["artifact_manifest"],
                paths["artifact_manifest_seal"],
                paths["final_manifest"],
                paths["final_seal"],
            }
        )
    missing = sorted(str(path) for path in result if not path.is_file())
    if missing:
        fail(f"artifact closure contains missing files: {missing}")
    return result


def write_artifact_ledger(
    repo_root: Path,
    ledger: Path,
    ledger_seal: Path,
    artifact_paths: set[Path],
) -> None:
    if ledger in artifact_paths or ledger_seal in artifact_paths:
        fail("artifact ledger must exclude itself and its detached seal")
    if path_occupied(ledger) or path_occupied(ledger_seal):
        fail(f"artifact ledger destination exists: {ledger} or {ledger_seal}")
    expected_seal = ledger.with_name(ledger.name + ".sha256")
    if expected_seal != ledger_seal:
        fail("internal artifact-ledger seal path mismatch")
    data = render_artifact_ledger(repo_root, ledger, ledger_seal, artifact_paths)
    write_bytes_exclusive(ledger, data, 0o444)
    write_detached_seal(ledger)


def render_artifact_ledger(
    repo_root: Path,
    ledger: Path,
    ledger_seal: Path,
    artifact_paths: set[Path],
) -> bytes:
    if ledger in artifact_paths or ledger_seal in artifact_paths:
        fail("artifact ledger must exclude itself and its detached seal")
    if ledger.with_name(ledger.name + ".sha256") != ledger_seal:
        fail("internal artifact-ledger seal path mismatch")
    lines = ["sha256\turi"]
    for path in sorted(
        artifact_paths, key=lambda item: normalized_repo_path(repo_root, item)
    ):
        lines.append(
            f"{sha256_file(path)}\trepo://{normalized_repo_path(repo_root, path)}"
        )
    return ("\n".join(lines) + "\n").encode("utf-8")


def ensure_artifact_ledger(
    repo_root: Path,
    ledger: Path,
    ledger_seal: Path,
    artifact_paths: set[Path],
) -> None:
    data = render_artifact_ledger(repo_root, ledger, ledger_seal, artifact_paths)
    ensure_exact_file(ledger, data, "final artifact ledger")
    ensure_detached_seal(ledger, ledger_seal)


def audit_artifacts(
    repo_root: Path,
    artifact_manifest: Path,
    artifact_manifest_seal: Path,
    *,
    expected_paths: set[Path] | None = None,
    required_paths: Sequence[Path] = (),
) -> None:
    verify_detached_seal(artifact_manifest, artifact_manifest_seal)
    seen: set[str] = set()
    with artifact_manifest.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, dialect="excel-tab")
        if reader.fieldnames != ["sha256", "uri"]:
            fail("artifact manifest header is invalid")
        for row in reader:
            if None in row or set(row) != {"sha256", "uri"}:
                fail("artifact manifest row has extra/missing TSV cells")
            if row["sha256"] is None or row["uri"] is None:
                fail("artifact manifest row has an empty structural cell")
            if not re.fullmatch(r"[0-9a-f]{64}", row["sha256"]):
                fail("artifact manifest SHA-256 is not canonical lowercase hex")
            uri = row["uri"]
            if uri in seen:
                fail(f"artifact manifest has duplicate URI: {uri}")
            seen.add(uri)
            if not uri.startswith("repo://"):
                fail(f"artifact URI is not repo-confined: {uri}")
            relative = uri.removeprefix("repo://")
            if Path(relative).is_absolute() or ".." in Path(relative).parts:
                fail(f"artifact URI is not normalized: {uri}")
            path = repo_root / relative
            try:
                lexical = normalized_repo_path(repo_root, path)
            except (FileNotFoundError, RuntimeError):
                fail(f"artifact path is missing or escapes the repository: {uri}")
            if lexical != relative:
                fail(f"artifact URI uses a symlink/alias instead of lexical bytes: {uri}")
            require_exact_regular_file(path, "artifact-ledger member")
            if sha256_file(path) != row["sha256"]:
                fail(f"artifact hash mismatch: {uri}")
    forbidden = {
        f"repo://{normalized_repo_path(repo_root, artifact_manifest)}",
        f"repo://{normalized_repo_path(repo_root, artifact_manifest_seal)}",
    }
    if seen & forbidden:
        fail(
            "artifact ledger violates the non-circular design by listing "
            "itself or its detached seal"
        )
    for path in required_paths:
        uri = f"repo://{normalized_repo_path(repo_root, path)}"
        if uri not in seen:
            fail(f"artifact ledger omits required closure member: {uri}")
    if expected_paths is not None:
        expected_uris = {
            f"repo://{normalized_repo_path(repo_root, path)}"
            for path in expected_paths
        }
        if seen != expected_uris:
            fail(
                "artifact ledger is not the exact expected closure; "
                f"missing={sorted(expected_uris - seen)}, "
                f"unexpected={sorted(seen - expected_uris)}"
            )


def strict_manifest_schema_audit(
    metadata: Mapping[str, object],
    paths: Mapping[str, Path],
    manifest: Path,
    label: str,
) -> None:
    sentinel_out = (
        paths["bootstrap_dir"] / f"audit-{label}-sentinel-output-must-not-exist"
    )
    if path_occupied(sentinel_out):
        fail(f"audit sentinel output unexpectedly exists: {sentinel_out}")
    command = [
        *frozen_harness_command(metadata),
        "--dagutil",
        str(metadata["frozen_oracle"]),
        "--larch2",
        str(metadata["frozen_larch2"]),
        "--process-metrics",
        str(metadata["process_metrics"]),
        "--out-dir",
        str(sentinel_out),
        "--workload-manifest",
        str(manifest),
        "--run-manifest-group",
        MANIFEST_SENTINEL_GROUP,
    ]
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    wanted = f"error: manifest group has no rows: {MANIFEST_SENTINEL_GROUP}"
    if result.returncode == 0 or wanted not in result.stderr:
        fail(
            f"strict {label} manifest audit did not reach its post-validation "
            f"sentinel; exit={result.returncode}, stderr={result.stderr!r}"
        )
    if path_occupied(sentinel_out):
        fail("strict audit executed a benchmark unexpectedly")


def audit_resolver_uniqueness(manifest: Path) -> None:
    seen: dict[tuple[str, ...], str] = {}
    for row in read_manifest_rows(manifest).values():
        key = tuple(row[field] for field in RESOLUTION_FIELDS)
        if key in seen:
            fail(
                "manifest has ambiguous non-group rows: "
                f"{seen[key]} and {row['row_id']}"
            )
        seen[key] = row["row_id"]


def seal_transaction_bytes(paths: Mapping[str, Path]) -> bytes:
    require_exact_regular_file(paths["pending_manifest"], "pending manifest")
    value = {
        "schema": "wric_phase0_seal_transaction",
        "schema_version": 1,
        "pending_manifest_sha256": sha256_file(paths["pending_manifest"]),
        "final_manifest": paths["final_manifest"].name,
        "final_artifact_manifest": paths["final_artifact_manifest"].name,
    }
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")


def final_transaction_destinations(paths: Mapping[str, Path]) -> tuple[Path, ...]:
    """Every final path whose ownership begins with the seal transaction."""

    return (
        paths["final_manifest"],
        paths["final_seal"],
        paths["final_artifact_manifest"],
        paths["final_artifact_manifest_seal"],
        paths["final_smoke"],
        paths["final_smoke_status"],
        *smoke_stream_paths(paths["final_smoke_status"]),
        paths["final_real_smoke"],
        paths["final_real_smoke_status"],
        *smoke_stream_paths(paths["final_real_smoke_status"]),
    )


def validate_seal_transaction(paths: Mapping[str, Path]) -> None:
    require_derived_bytes(
        paths["seal_transaction"],
        seal_transaction_bytes(paths),
        "seal transaction record",
    )


def audit_pending(baseline_dir: Path) -> None:
    metadata, paths = load_metadata(baseline_dir)
    verify_metadata_inputs(metadata)
    repo_root = Path(str(metadata["repo_root"]))
    verify_detached_seal(paths["pending_manifest"], paths["pending_seal"])
    derived = derive_pending_artifacts(metadata, paths)
    require_derived_bytes(
        paths["pending_manifest"], derived.manifest, "derived pending manifest"
    )
    require_derived_bytes(
        paths["field_evidence"],
        derived.field_evidence,
        "derived timeout-field evidence",
    )
    if path_occupied(paths["seal_transaction"]):
        validate_seal_transaction(paths)
    strict_manifest_schema_audit(
        metadata, paths, paths["pending_manifest"], "pending"
    )
    validate_strict_smoke(
        metadata,
        paths["pending_manifest"],
        paths["pending_smoke"],
        paths["pending_smoke_status"],
    )
    validate_real_strict_smoke(
        metadata,
        paths["pending_manifest"],
        paths["pending_real_smoke"],
        paths["pending_real_smoke_status"],
    )
    expected_paths = None
    if not path_occupied(paths["final_manifest"]) and not path_occupied(
        paths["seal_transaction"]
    ):
        expected_paths = collect_artifact_paths(
            metadata, paths, include_final=False
        )
    audit_artifacts(
        repo_root,
        paths["artifact_manifest"],
        paths["artifact_manifest_seal"],
        expected_paths=expected_paths,
        required_paths=(
            paths["pending_manifest"],
            paths["pending_seal"],
            paths["pending_smoke_status"],
            paths["pending_real_smoke_status"],
            paths["prepared_contract"],
            paths["prepared_contract_seal"],
            paths["capture_metadata"],
            paths["native_loop_proof"],
            paths["provenance_commands"],
            paths["unsealed_inputs"],
            paths["frozen_wrapper_calibration"],
            paths["frozen_calibration_controller"],
            paths["frozen_calibration_workload_source"],
            paths["frozen_calibration_workload"],
        ),
    )
    audit_resolver_uniqueness(paths["pending_manifest"])
    print(f"strict pending-manifest audit passed: {paths['pending_manifest']}")


def audit_final(baseline_dir: Path) -> None:
    metadata, paths = load_metadata(baseline_dir)
    verify_metadata_inputs(metadata)
    repo_root = Path(str(metadata["repo_root"]))
    verify_detached_seal(paths["final_manifest"], paths["final_seal"])
    derived = derive_pending_artifacts(metadata, paths)
    require_derived_bytes(
        paths["pending_manifest"], derived.manifest, "derived pending manifest"
    )
    require_derived_bytes(
        paths["final_manifest"], derived.manifest, "derived final manifest"
    )
    require_derived_bytes(
        paths["field_evidence"],
        derived.field_evidence,
        "derived timeout-field evidence",
    )
    validate_seal_transaction(paths)
    strict_manifest_schema_audit(metadata, paths, paths["final_manifest"], "final")
    validate_strict_smoke(
        metadata,
        paths["final_manifest"],
        paths["final_smoke"],
        paths["final_smoke_status"],
    )
    validate_real_strict_smoke(
        metadata,
        paths["final_manifest"],
        paths["final_real_smoke"],
        paths["final_real_smoke_status"],
    )
    audit_resolver_uniqueness(paths["final_manifest"])
    expected_paths = collect_artifact_paths(metadata, paths, include_final=True)
    audit_artifacts(
        repo_root,
        paths["final_artifact_manifest"],
        paths["final_artifact_manifest_seal"],
        expected_paths=expected_paths,
        required_paths=(
            paths["final_manifest"],
            paths["final_seal"],
            paths["artifact_manifest"],
            paths["artifact_manifest_seal"],
            paths["final_smoke_status"],
            paths["final_real_smoke_status"],
            paths["prepared_contract"],
            paths["prepared_contract_seal"],
            paths["frozen_wrapper_calibration"],
            paths["frozen_calibration_controller"],
            paths["frozen_calibration_workload_source"],
            paths["frozen_calibration_workload"],
        ),
    )
    print(f"strict final-manifest/artifact audit passed: {paths['final_manifest']}")


def audit(args: argparse.Namespace) -> None:
    baseline_dir = Path(args.baseline_dir).resolve()
    if path_occupied(baseline_dir / "workloads.tsv"):
        audit_final(baseline_dir)
    else:
        audit_pending(baseline_dir)


def remove_transaction_owned_path(path: Path) -> None:
    """Remove only an incomplete reserved seal artifact, never an alias."""

    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if path.is_symlink():
        fail(f"refusing to clean an aliased seal-transaction path: {path}")
    if stat.S_ISREG(info.st_mode):
        path.unlink()
        return
    if stat.S_ISDIR(info.st_mode):
        for child in path.iterdir():
            remove_transaction_owned_path(child)
        path.rmdir()
        return
    fail(f"refusing to clean a special seal-transaction path: {path}")


def ensure_final_smoke_bundle(
    metadata: Mapping[str, object],
    manifest: Path,
    smoke_dir: Path,
    status_path: Path,
    *,
    real: bool,
    allow_transaction_cleanup: bool,
) -> None:
    bundle = (smoke_dir, status_path, *smoke_stream_paths(status_path))
    if smoke_bundle_complete_or_clean(
        bundle, allow_cleanup=allow_transaction_cleanup
    ):
        if real:
            validate_real_strict_smoke(
                metadata, manifest, smoke_dir, status_path
            )
        else:
            validate_strict_smoke(metadata, manifest, smoke_dir, status_path)
        return
    if real:
        execute_real_strict_smoke(metadata, manifest, smoke_dir, status_path)
    else:
        execute_strict_smoke(metadata, manifest, smoke_dir, status_path)


def smoke_bundle_complete_or_clean(
    bundle: Sequence[Path], *, allow_cleanup: bool
) -> bool:
    """Recognize a complete bundle; clean partial bytes only on exact resume."""

    occupied = [path_occupied(path) for path in bundle]
    if all(occupied):
        return True
    if any(occupied):
        if not allow_cleanup:
            fail(
                "fresh seal encountered a pre-existing partial final smoke "
                f"bundle: {[str(path) for path, present in zip(bundle, occupied) if present]}"
            )
        # The immutable transaction record proves these reserved paths belong
        # to an interrupted seal attempt.  A status is written last, so a
        # partial bundle has never become admissible evidence and is safe to
        # retry from an empty destination.
        for path in reversed(bundle):
            remove_transaction_owned_path(path)
    return False


def seal(args: argparse.Namespace) -> None:
    if not args.confirm_seal_base:
        fail("seal requires the literal --confirm-seal-base guard")
    baseline_dir = Path(args.baseline_dir).resolve()
    metadata, paths = load_metadata(baseline_dir)
    resuming = path_occupied(paths["seal_transaction"])
    audit_pending(baseline_dir)
    data = paths["pending_manifest"].read_bytes()
    if resuming:
        validate_seal_transaction(paths)
    else:
        occupied = [
            path
            for path in final_transaction_destinations(paths)
            if path_occupied(path)
        ]
        if occupied:
            fail(
                "fresh seal found pre-existing final destination(s) before "
                f"transaction ownership: {[str(path) for path in occupied]}"
            )
    ensure_exact_file(
        paths["seal_transaction"],
        seal_transaction_bytes(paths),
        "seal transaction record",
    )
    ensure_exact_file(paths["final_manifest"], data, "promoted final manifest")
    ensure_detached_seal(paths["final_manifest"], paths["final_seal"])
    # Audit the promoted path through the strict harness itself.  The manifest
    # bytes are identical, but this also re-resolves manifest:// assets from
    # the final path before the final artifact root is created.
    strict_manifest_schema_audit(metadata, paths, paths["final_manifest"], "final")
    ensure_final_smoke_bundle(
        metadata,
        paths["final_manifest"],
        paths["final_smoke"],
        paths["final_smoke_status"],
        real=False,
        allow_transaction_cleanup=resuming,
    )
    ensure_final_smoke_bundle(
        metadata,
        paths["final_manifest"],
        paths["final_real_smoke"],
        paths["final_real_smoke_status"],
        real=True,
        allow_transaction_cleanup=resuming,
    )
    final_artifacts = collect_artifact_paths(metadata, paths, include_final=True)
    ensure_artifact_ledger(
        Path(str(metadata["repo_root"])),
        paths["final_artifact_manifest"],
        paths["final_artifact_manifest_seal"],
        final_artifacts,
    )
    audit_final(baseline_dir)
    print(f"sealed immutable Phase-0 base: {paths['final_manifest']}")
    print(f"manifest SHA-256: {sha256_file(paths['final_manifest'])}")
    print(f"final artifact ledger: {paths['final_artifact_manifest']}")


def matrix(args: argparse.Namespace) -> None:
    unpinned = args.unpinned_affinity or observed_affinity()
    captures, rows = build_matrix(args.physical_affinity, args.smt_affinity, unpinned)
    print("kind\tid\taffinity\tfixture\tcontract")
    for capture in captures:
        contract = (
            f"i{capture.iterations}/m{capture.native_max_moves}/"
            f"c{capture.chart_max_candidates}/k{capture.chart_top_k_exact}/"
            f"modes={','.join(capture.modes)}/workers={','.join(capture.workers)}/"
            f"lazy={capture.lazy_policy}/memory={capture.memory_budget_bytes}/"
            f"rss={capture.rss_limit_bytes}/role={capture.capture_role}/"
            "repeat=characterize1_then_warmup1_success_only"
        )
        print(f"capture\t{capture.capture_id}\t{capture.affinity_cpus}\t{capture.fixture}\t{contract}")
    for row in rows:
        print(
            f"row\t{row.row_id}\t-\t-\t{row.method}@{row.requested_workers}/"
            f"{row.run_group}/target={row.measured_trial_target}"
        )


def self_test(_: argparse.Namespace) -> None:
    captures, rows = build_matrix(PHYSICAL_AFFINITY, SMT_AFFINITY, SMT_AFFINITY)
    by_id = capture_map(captures)

    def assert_rejected(action: object, label: str) -> None:
        try:
            action()  # type: ignore[operator]
        except BootstrapError:
            return
        raise AssertionError(f"invalid {label} was accepted")

    fixture_hashes = {
        "small": ("small", "-"),
        "medium": ("medium", "ref"),
        "real20d": (REAL20D_PRIMARY_SHA256, REAL20D_REFSEQ_SHA256),
    }
    assert_unique_resolution(rows, by_id, fixture_hashes)
    compact_output_suffix = [
        "--chart-spr-canonical-result",
        "@search-canonical-result",
        "-o",
        "@output",
    ]
    for row in rows:
        if row.method == "sample_explore_merge":
            continue
        capture = by_id[row.capture_id]
        assert expected_chart_argv(
            capture, (row.method, row.requested_workers)
        )[-4:] == compact_output_suffix
        primary_sha, raw_refseq_sha = RAW_FIXTURE_HASHES[capture.fixture]
        assert manifest_chart_argv(
            capture,
            row,
            primary_sha,
            "-" if raw_refseq_sha == "NA" else raw_refseq_sha,
        )[-4:] == compact_output_suffix

    primary = by_id["medium-primary32k4-physical"]
    assert primary.iterations == 1 and primary.chart_max_candidates == 32
    assert primary.chart_top_k_exact == 4 and primary.workers == ("1", "2", "4", "8")
    assert set(primary.modes) == set(("sampled_tree_fixed", "grammar_exact", "hybrid_exact"))
    command_metadata = {
        "repo_root": "/synthetic/repo",
        "harness": "/synthetic/frozen-harness",
        "frozen_oracle": "/synthetic/frozen-oracle",
        "frozen_larch2": "/synthetic/frozen-larch2",
        "process_metrics": "/synthetic/process-metrics",
    }
    command_paths = {"captures": Path("/synthetic/captures")}
    for standard_capture in (
        item for item in captures if item.capture_role != "real_preflight"
    ):
        command = capture_command(command_metadata, command_paths, standard_capture)
        assert command.count("--capture-rss-limit-bytes") == 1
        cap_index = command.index("--capture-rss-limit-bytes")
        assert command[cap_index + 1] == str(standard_capture.rss_limit_bytes)
        assert "-i" in command and "LC_ALL=C" in command
        assert command[command.index("--warmups") + 1] == "0"
        assert command[command.index("--repetitions") + 1] == "1"
        assert command[command.index("--out-dir") + 1].endswith(
            f"/{standard_capture.capture_id}/characterization"
        )
    mutated_cap_command = capture_command(
        command_metadata,
        command_paths,
        dataclasses.replace(primary, rss_limit_bytes=primary.rss_limit_bytes + 1),
    )
    assert mutated_cap_command != capture_command(
        command_metadata, command_paths, primary
    )
    chart_repeat_dir, chart_repeat_command = repeat_stage_command(
        command_metadata,
        command_paths,
        primary,
        ("chart_spr_grammar_exact", "8"),
        5,
    )
    assert chart_repeat_dir.as_posix().endswith(
        "/medium-primary32k4-physical/repeats/chart_spr_grammar_exact--8"
    )
    assert chart_repeat_command[chart_repeat_command.index("--warmups") + 1] == "1"
    assert chart_repeat_command[
        chart_repeat_command.index("--repetitions") + 1
    ] == "4"
    assert chart_repeat_command[chart_repeat_command.index("--modes") + 1] == "grammar_exact"
    assert chart_repeat_command[
        chart_repeat_command.index("--workers-list") + 1
    ] == "8"
    assert "--native-only" not in chart_repeat_command
    assert "--full-canonical-correctness" not in chart_repeat_command
    _, native_repeat_command = repeat_stage_command(
        command_metadata,
        command_paths,
        primary,
        ("sample_explore_merge", "native"),
        5,
    )
    assert "--native-only" in native_repeat_command
    assert "--workers-list" not in native_repeat_command
    assert [expected_execution_order(0, trial) for trial in range(1, 6)] == [
        "baseline-first",
        "chart-first",
        "baseline-first",
        "chart-first",
        "baseline-first",
    ]
    stress = by_id["medium-stress128k16-physical"]
    assert stress.iterations == 3 and stress.chart_max_candidates == 128
    assert stress.chart_top_k_exact == 16 and set(stress.modes) == set(primary.modes)
    small_primary = by_id["small-primary32k4-physical"]
    assert (
        small_primary.iterations == 1
        and small_primary.chart_max_candidates == 32
        and small_primary.chart_top_k_exact == 4
        and small_primary.workers == ("1", "2", "4", "8")
        and set(small_primary.modes) == set(primary.modes)
    )
    small_stress = by_id["small-stress128k16-physical"]
    assert (
        small_stress.iterations == 3
        and small_stress.chart_max_candidates == 128
        and small_stress.chart_top_k_exact == 16
        and small_stress.workers == ("1", "2", "4", "8")
        and set(small_stress.modes) == set(primary.modes)
    )
    smt = by_id["medium-primary32k4-smt"]
    assert smt.workers == ("1", "2", "4", "8", "16", "auto", "default")
    real = by_id["real20d-exact1-preflight-physical"]
    assert real.workers == ("1", "8") and real.capture_role == "real_preflight"
    assert real.chart_max_candidates == 1 and real.chart_top_k_exact == 1
    assert real.memory_budget_bytes == MEMORY_BUDGET_BYTES
    assert real.rss_limit_bytes == REAL_PREFLIGHT_BYTES
    assert len(REAL20D_STDOUT_TEXT.encode("utf-8")) == REAL20D_STDOUT_SIZE
    assert sha256_bytes(REAL20D_STDOUT_TEXT.encode("utf-8")) == REAL20D_STDOUT_SHA256
    assert FROZEN_ORACLE_SHA256 == "7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c"
    real_rows = [row for row in rows if row.real_role == "preflight"]
    assert [row.requested_workers for row in real_rows] == ["1", "8"]
    for row in real_rows:
        assert canonical_argv_digest(
            manifest_chart_argv(
                real, row, REAL20D_PRIMARY_SHA256, REAL20D_REFSEQ_SHA256
            )
        ) == REAL20D_CANONICAL_ARGV_SHA256[row.requested_workers]
    allowed_timeouts = [row for row in rows if row.timeout_policy == "allow"]
    assert len(allowed_timeouts) == 35
    assert all(
        row.method != "sample_explore_merge"
        and by_id[row.capture_id].fixture == "medium"
        and row.method != "chart_spr_grammar_lower_bound_heuristic"
        for row in allowed_timeouts
    )
    assert all(
        row.timeout_policy == "forbid"
        for row in rows
        if row.method == "sample_explore_merge"
        or by_id[row.capture_id].fixture == "small"
        or row.method == "chart_spr_grammar_lower_bound_heuristic"
    )
    for capture in captures:
        capture_timeouts = timeout_rows_for_capture(rows, capture.capture_id)
        assert len(timeout_key_map(capture_timeouts)) == len(capture_timeouts)
        if capture.capture_role != "real_preflight":
            capture_row_specs(capture, rows)
    target_by_id = {row.row_id: row.measured_trial_target for row in rows}
    assert target_by_id["p0-native-medium-physical-i1-m50"] == 5
    assert target_by_id["p0-native-medium-physical-i3-m50"] == 3
    assert all(
        row.measured_trial_target == 3
        for row in rows
        if row.capture_id
        in {"small-primary32k4-physical", "small-stress128k16-physical"}
    )
    assert all(
        row.measured_trial_target == 5
        for row in rows
        if row.workload_name
        in {
            "dense-local-small-64",
            "exact-small-one",
            "dense-local-medium-64",
            "cache-medium",
            "exact-medium-one",
            "lazy-compression-medium",
            "small-auto-overhead",
        }
    )
    smt_targets = {
        row.requested_workers: row.measured_trial_target
        for row in rows
        if row.capture_id == "medium-primary32k4-smt"
        and row.method != "sample_explore_merge"
    }
    assert smt_targets == {
        "1": 3,
        "2": 3,
        "4": 3,
        "8": 3,
        "16": 3,
        "auto": 5,
        "default": 5,
    }
    assert len(captures) == 13 and len(rows) == 89

    valid_instrumentation = {
        "cache_build_ms": "10.000",
        "total_ms": "20.000",
        "initial_chart_construction_ms": "5.000",
        "materialization_ms": "6.000",
        "materialization_exact_verification_ms": "1.000",
        "materialization_accepted_update_ms": "2.000",
        "materialization_final_compaction_ms": "3.000",
        "exact_verifications": "2",
        "peak_concurrent_exact_verifiers": "1",
        "configured_chart_memory_budget": str(MEMORY_BUDGET_BYTES),
        "exact_candidate_admission_batches": "1",
        "exact_candidate_parallel_batches": "1",
        "exact_candidate_inner_parallel_batches": "0",
        "exact_candidate_memory_limited_batches": "0",
        "exact_candidate_peak_admitted_bytes": "4096",
        "exact_candidate_peak_projected_resident_bytes": "8192",
        "exact_candidate_queued_for_memory_ms": "0.000",
    }
    validate_chart_phase0_instrumentation(valid_instrumentation, ("chart", "1"))
    validate_chart_phase0_instrumentation(
        valid_instrumentation
        | {
            "exact_candidate_admission_batches": "2",
            "exact_candidate_memory_limited_batches": "1",
            "exact_candidate_queued_for_memory_ms": "0.125",
        },
        ("chart", "1"),
    )
    for mutation in (
        {"materialization_ms": "7.000"},
        {"initial_chart_construction_ms": "11.000"},
        {"peak_concurrent_exact_verifiers": "3"},
        {"exact_verifications": "0", "peak_concurrent_exact_verifiers": "1"},
        {"exact_candidate_admission_batches": ""},
        {"exact_candidate_parallel_batches": "2"},
        {"exact_candidate_inner_parallel_batches": "1"},
        {"exact_candidate_memory_limited_batches": "2"},
        {"exact_candidate_peak_admitted_bytes": "8193"},
        {"exact_candidate_admission_batches": "0"},
        {"exact_candidate_queued_for_memory_ms": "0.001"},
        {"exact_candidate_queued_for_memory_ms": "NaN"},
        {
            "exact_candidate_peak_projected_resident_bytes": str(
                MEMORY_BUDGET_BYTES + 1
            )
        },
    ):
        try:
            validate_chart_phase0_instrumentation(
                valid_instrumentation | mutation, ("chart", "1")
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"invalid Phase-0 instrumentation was accepted: {mutation}"
            )

    coverage_raw_fields = {
        "row_id",
        "status",
        "method",
        "requested_workers",
    }
    for values in PLAN_FIELD_COVERAGE_ROWS:
        template = dict(
            zip(PLAN_FIELD_COVERAGE_TEMPLATE_FIELDS, values, strict=True)
        )
        if template["source_kind"] in ("raw_tsv", "canonical_digest"):
            coverage_raw_fields.update(template["source_field"].split(","))
    coverage_native = {field: "-" for field in coverage_raw_fields}
    native_argv = expected_native_argv_sha256(primary)
    native_output = "a" * 64
    native_trial = trial_digest(
        "sample_explore_merge", "-", native_output, native_argv
    )
    coverage_native.update(
        {
            "row_id": "seedtree.pb.gz/sample_explore_merge@native",
            "fixture": "seedtree.pb.gz",
            "status": "ok",
            "validation_status": "ok",
            "benchmark_scope": "CHART_SPR_PHASE0_SEARCH_COMPARISON",
            "method": "sample_explore_merge",
            "requested_workers": "native",
            "resolved_workers": "native",
            "worker_policy": "native",
            "trial_index": "1",
            "execution_order": "baseline-first",
            "wall_clock_s": "1.0",
            "user_cpu_s": "0.5",
            "system_cpu_s": "0.1",
            "max_rss_kb": "100",
            "peak_sampled_rss_kb": "120",
            "peak_sampled_swap_kb": "0",
            "configured_chart_memory_budget": "NA",
            "manifest_rss_limit_bytes": str(primary.rss_limit_bytes),
            "input_sha256": MEDIUM_PRIMARY_SHA256,
            "refseq_sha256": MEDIUM_REFSEQ_SHA256,
            "initial_validated_parsimony_min": "10",
            "final_validated_parsimony_min": "9",
            "best_reported_objective": "9",
            "best_validated_parsimony_min": "9",
            "iterations": str(primary.iterations),
            "seed": "1",
            "acceptance": "sample_explore_merge",
            "objective": "parsimony_sampling",
            "candidate_selection": "native_best_moves",
            "candidate_source": "sampled_tree",
            "search_semantic_sha256": "-",
            "output_semantic_sha256": native_output,
            "canonical_argv_sha256": native_argv,
            "trial_semantic_sha256": native_trial,
            "canonical_digest": native_trial,
        }
    )
    coverage_timeout = {field: "-" for field in coverage_raw_fields}
    timeout_key = ("chart_spr_grammar_exact", "8")
    coverage_timeout.update(
        {
            "row_id": "seedtree.pb.gz/chart_spr_grammar_exact@8",
            "fixture": "seedtree.pb.gz",
            "status": "timeout",
            "validation_status": "not_run",
            "benchmark_scope": "CHART_SPR_PHASE0_SEARCH_COMPARISON",
            "method": "chart_spr_grammar_exact",
            "requested_workers": "8",
            "resolved_workers": "NA",
            "worker_policy": "timeout_unobserved",
            "trial_index": "1",
            "execution_order": "baseline-first",
            "wall_clock_s": "600.0",
            "user_cpu_s": "500.0",
            "system_cpu_s": "1.0",
            "max_rss_kb": "100",
            "peak_sampled_rss_kb": "120",
            "peak_sampled_swap_kb": "0",
            "configured_chart_memory_budget": str(primary.memory_budget_bytes),
            "manifest_rss_limit_bytes": str(primary.rss_limit_bytes),
            "input_sha256": MEDIUM_PRIMARY_SHA256,
            "refseq_sha256": MEDIUM_REFSEQ_SHA256,
            "initial_validated_parsimony_min": "10",
            "iterations": str(primary.iterations),
            "seed": "1",
            "acceptance": "exact_multisite",
            "objective": "grammar_exact",
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": "grammar",
            "canonical_argv_sha256": expected_chart_argv_sha256(
                primary, timeout_key
            ),
        }
    )
    synthetic_coverage = expected_plan_field_coverage_rows(
        primary,
        Path("/synthetic/capture"),
        (coverage_native, coverage_timeout),
        {
            ("sample_explore_merge", "native"): RowSpec(
                "synthetic-native",
                "synthetic",
                "synthetic",
                primary.capture_id,
                "sample_explore_merge",
                "native",
            ),
            ("chart_spr_grammar_exact", "8"): RowSpec(
                "synthetic-chart-timeout",
                "synthetic",
                "synthetic",
                primary.capture_id,
                "chart_spr_grammar_exact",
                "8",
            ),
        },
    )
    all_timeout_coverage = expected_plan_field_coverage_rows(
        primary,
        Path("/synthetic/capture"),
        (coverage_timeout,),
        {
            timeout_key: RowSpec(
                "synthetic-chart-timeout",
                "synthetic",
                "synthetic",
                primary.capture_id,
                timeout_key[0],
                timeout_key[1],
            )
        },
    )
    assert all(
        row["row_id"]
        in {"capture:medium-primary32k4-physical", "synthetic-chart-timeout"}
        for row in all_timeout_coverage
    )
    assert any(
        row["plan_field"] == "worker_request_policy"
        and row["row_id"] == "synthetic-chart-timeout"
        and row["phase0_state"] == "recorded"
        for row in all_timeout_coverage
    )
    assert any(
        row["plan_field"] == "worker_resolution"
        and row["row_id"] == "synthetic-chart-timeout"
        and row["phase0_state"] == "not_applicable"
        for row in all_timeout_coverage
    )
    coverage_by_identity = {
        (row["plan_field"], row["row_id"]): row for row in synthetic_coverage
    }
    assert coverage_by_identity[("worker_request_policy", "synthetic-native")][
        "phase0_state"
    ] == "not_applicable"
    assert coverage_by_identity[
        ("worker_request_policy", "synthetic-chart-timeout")
    ]["phase0_state"] == "recorded"
    assert coverage_by_identity[("worker_resolution", "synthetic-native")][
        "phase0_state"
    ] == "not_applicable"
    assert coverage_by_identity[
        ("worker_resolution", "synthetic-chart-timeout")
    ]["phase0_state"] == "not_applicable"
    assert coverage_by_identity[("active_patterns", "synthetic-native")][
        "phase0_state"
    ] == "not_applicable"
    assert coverage_by_identity[
        ("active_patterns", "synthetic-chart-timeout")
    ]["phase0_state"] == "not_applicable"
    assert coverage_by_identity[
        ("candidate_and_accept_semantics", "synthetic-native")
    ]["required_for_status"] == "ok"
    assert coverage_by_identity[
        ("candidate_and_accept_semantics", "synthetic-chart-timeout")
    ]["source_kind"] == "unavailable"

    # A capture that owns no canonical native row and whose chart rows all
    # time out has no repeat stage by design.  Its exact characterization argv
    # remains valid command evidence; an empty, closed repeat array is not an
    # omitted command field.
    validate_command_log_mapping(
        {
            "characterization_command": ["env", "-i", "synthetic-harness"],
            "repeat_stages": [],
        },
        "synthetic-all-timeout-status",
    )
    valid_repeat_record = {
        "key": ["chart_spr_grammar_exact", "8"],
        "row_id": "synthetic-chart-success",
        "stage": "repeats/chart_spr_grammar_exact--8",
        "command": ["env", "-i", "synthetic-harness"],
        "harness_exit_code": 0,
        "raw_trials_sha256": "1" * 64,
        "summary_sha256": "2" * 64,
        "stdout_sha256": "3" * 64,
        "stderr_sha256": "4" * 64,
        "canonical_rows": 2,
        "pair_peer_rows": 2,
    }
    validate_command_log_mapping(
        {
            "characterization_command": ["env", "-i", "synthetic-harness"],
            "repeat_stages": [valid_repeat_record],
        },
        "synthetic-success-status",
    )
    for invalid_repeat in (
        valid_repeat_record | {"unexpected": "open"},
        valid_repeat_record | {"stage": "repeats/../escape"},
        valid_repeat_record | {"harness_exit_code": False},
        valid_repeat_record | {"raw_trials_sha256": 1},
        valid_repeat_record | {"canonical_rows": 0},
        valid_repeat_record | {"pair_peer_rows": 1},
    ):
        try:
            validate_command_log_mapping(
                {
                    "characterization_command": ["synthetic-harness"],
                    "repeat_stages": [invalid_repeat],
                },
                "synthetic-invalid-status",
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"invalid repeat-stage command evidence was accepted: {invalid_repeat}"
            )
    try:
        validate_command_log_mapping(
            {
                "characterization_command": ["synthetic-harness"],
                "repeat_stages": [valid_repeat_record, valid_repeat_record],
            },
            "synthetic-duplicate-repeat-status",
        )
    except BootstrapError:
        pass
    else:
        raise AssertionError("duplicate repeat-stage command evidence was accepted")
    assert json_path_exists(
        {"characterization_command": ["cmd"], "repeat_stages": []},
        "repeat_stages",
    )
    assert not json_path_exists(
        {"characterization_command": ["cmd"], "repeat_stages": []},
        "repeat_stages.command",
    )
    for mutation in (
        {"input_sha256": "0" * 64},
        {"resolved_workers": "8"},
        {"worker_policy": "explicit"},
        {"wall_clock_s": "NA"},
        {"peak_sampled_swap_kb": "NOT_A_METRIC"},
        {"initial_validated_parsimony_min": "NOT_A_SCORE"},
        {"initial_validated_parsimony_min": "11"},
        {"iterations": "999"},
        {"seed": "999"},
        {"acceptance": "forged"},
        {"objective": "forged"},
        {"candidate_selection": "forged"},
        {"candidate_source": "forged"},
    ):
        try:
            expected_plan_field_coverage_rows(
                primary,
                Path("/synthetic/capture"),
                (coverage_native, coverage_timeout | mutation),
                {
                    ("sample_explore_merge", "native"): RowSpec(
                        "synthetic-native",
                        "synthetic",
                        "synthetic",
                        primary.capture_id,
                        "sample_explore_merge",
                        "native",
                    ),
                    timeout_key: RowSpec(
                        "synthetic-chart-timeout",
                        "synthetic",
                        "synthetic",
                        primary.capture_id,
                        timeout_key[0],
                        timeout_key[1],
                    ),
                },
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"invalid row-explicit coverage evidence was accepted: {mutation}"
            )

    # Exercise the successful-chart-only branch independently of canonical
    # companion lookup.  Every raw field used as Phase-0 evidence must have a
    # real domain and, where product-owned, agree byte-for-byte with the timed
    # report rather than merely being present in the TSV.
    with tempfile.TemporaryDirectory(prefix="wric-phase0-domain-") as temporary:
        synthetic_root = Path(temporary)
        synthetic_report = synthetic_root / "chart.report"
        success_key = ("chart_spr_grammar_exact", "8")
        success_argv = expected_chart_argv_sha256(primary, success_key)
        success_search = "1" * 64
        success_output = "2" * 64
        success_trial = trial_digest(
            success_key[0], success_search, success_output, success_argv
        )
        successful_chart = coverage_timeout | {
            "row_id": "seedtree.pb.gz/chart_spr_grammar_exact@8",
            "status": "ok",
            "validation_status": "ok",
            "resolved_workers": "8",
            "worker_policy": "explicit",
            "wall_clock_s": "1.000000",
            "user_cpu_s": "0.750000",
            "system_cpu_s": "0.125000",
            "initial_validated_parsimony_min": "10",
            "final_validated_parsimony_min": "9",
            "best_reported_objective": "9",
            "best_validated_parsimony_min": "9",
            "iterations": str(primary.iterations),
            "seed": "1",
            "acceptance": "exact_multisite",
            "objective": "grammar_exact",
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": "grammar",
            "candidates_generated": "4",
            "candidates_scored": "3",
            "exact_verifications": "2",
            "accepted_moves": "1",
            "candidate_accepts_attempted": "1",
            "post_materialization_rejections": "0",
            "active_patterns": "2",
            "grammar_clades": "3",
            "grammar_productions": "4",
            "chart_cache_resident_bytes": "4096",
            "final_dag_nodes": "10",
            "final_dag_edges": "12",
            "full_search_state_rebuilds": "2",
            "final_compaction_rebuilds": "0",
            "initial_search_state_rebuilds": "1",
            "sidecar_rebuilds_after_accept": "1",
            "overlay_materializations_for_exact_verification": "2",
            "overlay_materializations_for_accept_materialization": "1",
            "overlay_materializations_for_final_compaction": "0",
            "overlay_materializations_for_oracle": "0",
            "full_overlay_materializations": "3",
            "upward_path_iterator_steps": "20",
            "path_pairs_considered": "10",
            "candidates_pruned_before_construction": "1",
            "candidates_pruned_after_construction": "2",
            "candidates_generated_after_dedup": "4",
            "candidate_cap_cutoffs": "0",
            "path_budget_cutoffs": "0",
            "reachable_clades_traversed": "6",
            "reachable_productions_traversed": "8",
            "reachability_full_grammar_like_passes": "0",
            "peak_concurrent_exact_verifiers": "2",
            "exact_candidate_admission_batches": "1",
            "exact_candidate_parallel_batches": "1",
            "exact_candidate_inner_parallel_batches": "0",
            "exact_candidate_memory_limited_batches": "0",
            "exact_candidate_peak_admitted_bytes": "4096",
            "exact_candidate_peak_projected_resident_bytes": "8192",
            "exact_candidate_queued_for_memory_ms": "0.000",
            "exact_candidate_timing_count": "2",
            "candidate_generation_ms": "1.000",
            "cache_build_ms": "2.000",
            "initial_chart_construction_ms": "1.500",
            "local_scoring_ms": "3.000",
            "local_candidates_per_second": "1000.000",
            "local_ms_per_candidate": "1.000000",
            "exact_initialization_ms": "0.500",
            "exact_verification_ms": "10.000",
            "exact_candidate_verification_ms_min": "4.000",
            "exact_candidate_verification_ms_mean": "5.000",
            "exact_candidate_verification_ms_max": "6.000",
            "exact_ms_per_candidate": "5.000",
            "accepted_rebuild_ms": "0.250",
            "final_compaction_ms": "0.000",
            "post_materialization_check_ms": "0.125",
            "materialization_ms": "0.300",
            "materialization_exact_verification_ms": "0.100",
            "materialization_accepted_update_ms": "0.200",
            "materialization_final_compaction_ms": "0.000",
            "total_ms": "20.000",
            "final_compaction_exactness_kind": "none",
            "committed_attempt_ratio": "1.000000",
            "affected_mean": "2.000",
            "affected_p50": "2",
            "affected_p95": "3",
            "affected_max": "3",
            "search_semantic_sha256": success_search,
            "output_semantic_sha256": success_output,
            "trial_semantic_sha256": success_trial,
            "canonical_argv_sha256": success_argv,
            "canonical_digest": success_trial,
            "report_path": str(synthetic_report),
        }
        report_fields = {
            "chart_workers_requested": "8",
            "chart_workers_resolved": "8",
            "local_score_workers": "8",
            "chart_worker_policy": "explicit",
            "iterations": str(primary.iterations),
            "seed": "1",
            "acceptance": "exact_multisite",
            "objective": "grammar_exact",
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": "grammar",
            "memory_budget_bytes": str(primary.memory_budget_bytes),
            "initial_score": "10",
            "final_score": "9",
            "chain_per_accept_exactness_label": "none_conservative_materialize_rebuild",
            "final_compaction_exactness_kind": "none",
            "active_patterns": "2",
            "final_grammar_clades": "3",
            "final_grammar_productions": "4",
            "candidates_generated": "4",
            "candidates_scored": "3",
            "exact_verifications": "2",
            "accepted_moves": "1",
            "candidate_accepts_attempted": "1",
            "post_materialization_rejections": "0",
            "chart_cache_resident_bytes": "4096",
            "full_search_state_rebuilds": "2",
            "final_compaction_rebuilds": "0",
            "initial_search_state_rebuilds": "1",
            "sidecar_rebuilds_after_accept": "1",
            "overlay_materializations_for_exact_verification": "2",
            "overlay_materializations_for_accept_materialization": "1",
            "overlay_materializations_for_final_compaction": "0",
            "candidate_generation_ms": "1.000",
            "cache_build_ms": "2.000",
            "initial_chart_construction_ms": "1.500",
            "local_scoring_ms": "3.000",
            "local_candidates_per_second": "1000.000",
            "exact_initialization_ms": "0.500",
            "exact_verification_ms": "10.000",
            "exact_candidate_timing_count": "2",
            "exact_candidate_verification_ms_min": "4.000",
            "exact_candidate_verification_ms_mean": "5.000",
            "exact_candidate_verification_ms_max": "6.000",
            "accepted_rebuild_ms": "0.250",
            "final_compaction_ms": "0.000",
            "post_materialization_check_ms": "0.125",
            "materialization_ms": "0.300",
            "materialization_exact_verification_ms": "0.100",
            "materialization_accepted_update_ms": "0.200",
            "materialization_final_compaction_ms": "0.000",
            "peak_concurrent_exact_verifiers": "2",
            "exact_candidate_admission_batches": "1",
            "exact_candidate_parallel_batches": "1",
            "exact_candidate_inner_parallel_batches": "0",
            "exact_candidate_memory_limited_batches": "0",
            "exact_candidate_peak_admitted_bytes": "4096",
            "exact_candidate_peak_projected_resident_bytes": "8192",
            "exact_candidate_queued_for_memory_ms": "0.000",
            "total_ms": "20.000",
        }
        counter_fields = {
            "upward_path_iterator_steps": "20",
            "path_pairs_considered": "10",
            "candidates_pruned_before_construction": "1",
            "candidates_pruned_after_construction": "2",
            "candidates_generated_after_dedup": "4",
            "candidate_cap_cutoffs": "0",
            "path_budget_cutoffs": "0",
            "overlay_materializations_for_oracle": "0",
            "full_overlay_materializations": "3",
            "reachable_clades_traversed": "6",
            "reachable_productions_traversed": "8",
            "reachability_full_grammar_like_passes": "0",
            "exact_verifications": "2",
            "accepted_moves": "1",
            "candidate_accepts_attempted": "1",
            "post_materialization_rejections": "0",
            "sidecar_rebuilds_after_accept": "1",
            "overlay_materializations_for_exact_verification": "2",
            "overlay_materializations_for_accept_materialization": "1",
            "overlay_materializations_for_final_compaction": "0",
        }
        synthetic_report.write_text(
            "".join(f"  {field}: {value}\n" for field, value in report_fields.items())
            + "  final_dag:\n"
            + "    nodes: 10\n"
            + "    edges: 12\n"
            + "  affected_clade_count_distribution:\n"
            + "    mean: 2.000\n"
            + "    p50: 2\n"
            + "    p95: 3\n"
            + "    max: 3\n"
            + "  counters:\n"
            + "".join(
                f"    {field}: {value}\n"
                for field, value in counter_fields.items()
            ),
            encoding="utf-8",
        )
        validate_raw_trial_binding(
            primary, successful_chart, success_key, "ok"
        )
        validate_successful_raw_domains(
            synthetic_root, primary, successful_chart, success_key
        )
        for mutation in (
            {"worker_policy": "automatic"},
            {"candidates_generated": "1"},
            {"exact_candidate_verification_ms_min": "7.000"},
            {"exact_candidate_parallel_batches": "0"},
            {"exact_candidate_queued_for_memory_ms": ""},
            {"final_compaction_exactness_kind": "unknown"},
            {"active_patterns": "3"},
        ):
            try:
                validate_successful_raw_domains(
                    synthetic_root,
                    primary,
                    successful_chart | mutation,
                    success_key,
                )
            except BootstrapError:
                pass
            else:
                raise AssertionError(
                    f"invalid successful-chart evidence was accepted: {mutation}"
                )

    # Report parsing is section- and indentation-aware. Duplicate/nested
    # summary keys, duplicate counters, and non-unanimous iteration stop
    # reasons must never be accepted as interchangeable evidence.
    with tempfile.TemporaryDirectory(prefix="wric-phase0-parser-") as temporary:
        parser_report = Path(temporary) / "report.out"
        parser_text = (
            "  target: top\n"
            "    target: nested\n"
            "  iterations: 2\n"
            "  iteration_reports:\n"
            "    - iteration: 0\n"
            "      candidate_generation:\n"
            "        stop_reason: candidate_cap\n"
            "    - iteration: 1\n"
            "      candidate_generation:\n"
            "        stop_reason: candidate_cap\n"
            "  counters:\n"
            "    local_score_worker_tasks: 4\n"
        )
        parser_report.write_text(parser_text, encoding="utf-8")
        assert report_value(parser_report, "target") == "top"
        assert report_counter_value(parser_report, "local_score_worker_tasks") == "4"
        assert report_stop_reason(parser_report) == "candidate_cap"

        def rejects_parser_text(text: str, operation: str) -> None:
            parser_report.write_text(text, encoding="utf-8")
            action = {
                "top": lambda: report_value(parser_report, "target"),
                "counter": lambda: report_counter_value(
                    parser_report, "local_score_worker_tasks"
                ),
                "stop": lambda: report_stop_reason(parser_report),
            }[operation]
            assert_rejected(action, f"report parser mutation ({operation})")

        rejects_parser_text(parser_text + "  target: duplicate\n", "top")
        rejects_parser_text(parser_text.replace("  target: top\n", ""), "top")
        rejects_parser_text(
            parser_text.replace(
                "    local_score_worker_tasks: 4\n",
                "    local_score_worker_tasks: 4\n"
                "    local_score_worker_tasks: 5\n",
            ),
            "counter",
        )
        rejects_parser_text(
            parser_text + "  counters:\n    local_score_worker_tasks: 4\n",
            "counter",
        )
        rejects_parser_text(
            parser_text.replace(
                "        stop_reason: candidate_cap\n"
                "  counters:",
                "        stop_reason: exhausted\n  counters:",
            ),
            "stop",
        )
        rejects_parser_text(
            parser_text.replace("    - iteration: 1\n", "    - iteration: 3\n"),
            "stop",
        )
        rejects_parser_text(
            parser_text.replace(
                "      candidate_generation:\n"
                "        stop_reason: candidate_cap\n"
                "    - iteration: 1\n"
                "      candidate_generation:\n"
                "        stop_reason: candidate_cap\n",
                "      candidate_generation:\n"
                "        stop_reason: candidate_cap\n"
                "      candidate_generation:\n"
                "        stop_reason: candidate_cap\n"
                "    - iteration: 1\n",
            ),
            "stop",
        )
        rejects_parser_text(
            parser_text.replace(
                "  iteration_reports:\n"
                "    - iteration: 0\n",
                "  iteration_reports:\n"
                "      candidate_generation:\n"
                "        stop_reason: candidate_cap\n"
                "    - iteration: 0\n",
            ),
            "stop",
        )

    # Report/curve paths are exact stage-local, injective identities. This
    # rejects characterization/repeat and W1/W8 report splices even when the
    # substituted file is otherwise a valid regular capture artifact.
    with tempfile.TemporaryDirectory(prefix="wric-phase0-stage-") as temporary:
        capture_root = Path(temporary).resolve()
        stage = capture_root / "characterization"
        logs = stage / "logs"
        curves = stage / "curves"
        logs.mkdir(parents=True)
        curves.mkdir()
        report = logs / "fixture_chart_spr_grammar_exact_trial1_w8.out"
        curve = curves / "fixture_chart_spr_grammar_exact_trial1_w8.tsv"
        report.write_text("report\n", encoding="utf-8")
        curve.write_text("curve\n", encoding="utf-8")
        stage_row = {
            "fixture": "fixture",
            "row_id": "fixture/chart_spr_grammar_exact@8",
            "method": "chart_spr_grammar_exact",
            "requested_workers": "8",
            "trial_index": "1",
            "report_path": str(report),
            "curve_path": str(curve),
        }
        validate_stage_artifact_paths(capture_root, stage, (stage_row,), "fixture")

        native_report = logs / "fixture_sample_explore_merge_trial1.err"
        native_curve = curves / "fixture_sample_explore_merge_trial1.tsv"
        native_report.write_text("native report\n", encoding="utf-8")
        native_curve.write_text("native curve\n", encoding="utf-8")
        native_row = {
            "fixture": "fixture",
            "row_id": "fixture/sample_explore_merge@native",
            "method": "sample_explore_merge",
            "requested_workers": "native",
            "trial_index": "1",
            "report_path": str(native_report),
            "curve_path": str(native_curve),
        }
        validate_stage_artifact_paths(capture_root, stage, (native_row,), "fixture")
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (native_row | {"report_path": str(report)},),
                "fixture",
            ),
            "native stdout/report identity splice",
        )

        wrong_prefix_report = (
            logs / "other_chart_spr_grammar_exact_trial1_w8.out"
        )
        wrong_prefix_curve = (
            curves / "other_chart_spr_grammar_exact_trial1_w8.tsv"
        )
        wrong_prefix_report.write_text("report\n", encoding="utf-8")
        wrong_prefix_curve.write_text("curve\n", encoding="utf-8")
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (stage_row | {"report_path": str(wrong_prefix_report)},),
                "fixture",
            ),
            "arbitrary report prefix splice",
        )
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (stage_row | {"curve_path": str(wrong_prefix_curve)},),
                "fixture",
            ),
            "mismatched report/curve prefix splice",
        )
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (
                    stage_row
                    | {
                        "fixture": "other",
                        "row_id": "other/chart_spr_grammar_exact@8",
                        "report_path": str(wrong_prefix_report),
                        "curve_path": str(wrong_prefix_curve),
                    },
                ),
                "fixture",
            ),
            "consistent mutable-fixture artifact splice",
        )

        repeat_logs = capture_root / "repeats" / "w8" / "logs"
        repeat_logs.mkdir(parents=True)
        cross_stage = repeat_logs / report.name
        cross_stage.write_text("report\n", encoding="utf-8")
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (stage_row | {"report_path": str(cross_stage)},),
                "fixture",
            ),
            "cross-stage report splice",
        )
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (
                    stage_row,
                    stage_row
                    | {
                        "requested_workers": "1",
                        "curve_path": str(curve),
                    },
                ),
                "fixture",
            ),
            "W1/W8 report splice",
        )
        report_alias = logs / "alias_chart_spr_grammar_exact_trial1_w8.out"
        report_alias.symlink_to(report)
        assert_rejected(
            lambda: validate_stage_artifact_paths(
                capture_root,
                stage,
                (stage_row | {"report_path": str(report_alias)},),
                "fixture",
            ),
            "aliased report path",
        )

    # Every successful chart child must leave a distinct, single-link,
    # schema-v1 compact at the exact report-adjacent path.  Exercise the
    # descriptor reader independently so deletion, tamper, alias, and reuse
    # cannot be masked by the later artifact-ledger walk.
    with tempfile.TemporaryDirectory(
        prefix="wric-phase0-timed-compact-"
    ) as temporary:
        compact_root = Path(temporary).resolve()
        compact_stage = compact_root / "repeat"
        compact_logs = compact_stage / "logs"
        compact_logs.mkdir(parents=True)
        method = "chart_spr_grammar_exact"
        key = (method, "8")
        safe_fixture = re.sub(
            r"[^A-Za-z0-9_.-]", "_", RAW_FIXTURE_LABELS[primary.fixture]
        )
        report_path = compact_logs / f"{safe_fixture}_{method}_trial1_w8.out"
        report_path.write_text("report\n", encoding="utf-8")
        compact_path = report_path.with_suffix(".canonical.json")
        compact_mapping: dict[str, object] = {
            "schema": "larch.chart_spr.semantic_digest",
            "schema_version": 1,
            "digest_algorithm": "sha256",
            "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
            "semantic_sha256": "0" * 64,
            "contract_sha256": "1" * 64,
            "candidates_sha256": "2" * 64,
            "exact_sha256": "3" * 64,
            "acceptance_sha256": "4" * 64,
            "chain_sha256": "5" * 64,
            "final_topology_sha256": "6" * 64,
            "record_count": 1,
            "candidate_count": 0,
            "exact_candidate_count": 0,
            "iteration_count": 0,
        }
        compact_data = (
            json.dumps(compact_mapping, sort_keys=True, separators=(",", ":"))
            + "\n"
        ).encode("utf-8")
        compact_path.write_bytes(compact_data)
        observed_mapping, observed_data, identity = read_timed_chart_compact(
            compact_path, compact_stage, "synthetic timed chart compact"
        )
        assert observed_mapping == compact_mapping
        assert observed_data == compact_data
        assert measured_chart_compact_path(
            {"report_path": str(report_path)}
        ) == compact_path
        assert warmup_chart_compact_path(
            compact_stage, primary, key, 1
        ) == compact_logs / f"{safe_fixture}_{method}_warmup1_w8.canonical.json"

        missing = compact_logs / "missing.canonical.json"
        assert_rejected(
            lambda: read_timed_chart_compact(
                missing, compact_stage, "missing timed chart compact"
            ),
            "missing timed compact",
        )
        schema_tamper = compact_logs / "schema-tamper.canonical.json"
        schema_tamper.write_text(
            json.dumps(compact_mapping | {"schema_version": 2}) + "\n",
            encoding="utf-8",
        )
        assert_rejected(
            lambda: read_timed_chart_compact(
                schema_tamper, compact_stage, "schema-tampered chart compact"
            ),
            "timed compact schema tamper",
        )
        duplicate = compact_logs / "duplicate.canonical.json"
        duplicate.write_bytes(
            compact_data[:-2]
            + b',"schema":"larch.chart_spr.semantic_digest"}\n'
        )
        assert_rejected(
            lambda: read_timed_chart_compact(
                duplicate, compact_stage, "duplicate-key chart compact"
            ),
            "timed compact duplicate key",
        )
        oversized = compact_logs / "oversized.canonical.json"
        oversized.write_bytes(b" " * (MAX_CHART_SEARCH_DIGEST_BYTES + 1))
        assert_rejected(
            lambda: read_timed_chart_compact(
                oversized, compact_stage, "oversized chart compact"
            ),
            "oversized timed compact",
        )
        hardlink = compact_logs / "hardlink.canonical.json"
        os.link(compact_path, hardlink)
        assert_rejected(
            lambda: read_timed_chart_compact(
                compact_path, compact_stage, "hard-linked timed chart compact"
            ),
            "timed compact hard link",
        )
        hardlink.unlink()

        tampered_mapping = compact_mapping | {"semantic_sha256": "f" * 64}
        tampered_data = (
            json.dumps(tampered_mapping, sort_keys=True, separators=(",", ":"))
            + "\n"
        ).encode("utf-8")
        assert_rejected(
            lambda: require_chart_compact_match(
                tampered_mapping,
                tampered_data,
                compact_mapping,
                compact_data,
                "tampered warmup compact",
            ),
            "timed compact semantic tamper",
        )
        seen_paths: set[Path] = set()
        seen_identities: dict[tuple[int, int], Path] = {}
        register_unique_chart_compact(
            compact_path,
            identity,
            seen_paths,
            seen_identities,
            "synthetic measured compact",
        )
        assert_rejected(
            lambda: register_unique_chart_compact(
                compact_path,
                identity,
                seen_paths,
                seen_identities,
                "reused measured compact",
            ),
            "timed compact path reuse",
        )
        assert_rejected(
            lambda: register_unique_chart_compact(
                compact_logs / "other.canonical.json",
                identity,
                seen_paths,
                seen_identities,
                "aliased warmup compact",
            ),
            "timed compact inode reuse",
        )

    with tempfile.TemporaryDirectory(prefix="wric-phase0-sidecar-") as temporary:
        sidecar = Path(temporary) / "canonical.ndjson"
        sidecar.write_text(
            '{"record":"contract","value":1}\n'
            '{"record":"candidate","value":2}\n',
            encoding="utf-8",
        )
        assert sidecar_contract(sidecar)["value"] == 1
        sidecar.write_text(
            '{"record":"contract","value":1}\n'
            '{"record":"contract","value":2}\n',
            encoding="utf-8",
        )
        assert_rejected(lambda: sidecar_contract(sidecar), "duplicate contract")

    with tempfile.TemporaryDirectory(
        prefix="wric-phase0-sidecar-digest-"
    ) as temporary:
        canonical_root = Path(temporary)
        canonical_sidecar = canonical_root / "full.ndjson"
        canonical_digest = canonical_root / "full.json"
        canonical_records: list[tuple[str, dict[str, object]]] = [
            (
                "contract",
                {
                    "record": "schema",
                    "schema": "larch.chart_spr.semantic.ndjson",
                    "schema_version": 1,
                },
            ),
            ("contract", {"record": "contract", "value": "frozen"}),
            ("contract", {"record": "initial_state", "initial_score": 7}),
            ("acceptance", {"record": "iteration_begin", "iteration": 0}),
            (
                "candidates",
                {"record": "candidate", "signature": "candidate-0"},
            ),
            (
                "candidates",
                {"record": "candidate_lower_bound", "delta": -1},
            ),
            ("exact", {"record": "candidate_exact", "delta": -1}),
            (
                "exact",
                {
                    "record": "exact_evidence",
                    "evidence_kind": "score_only",
                    "keep_mask_kind": "score_only",
                    "keep_production_exact": False,
                },
            ),
            (
                "acceptance",
                {
                    "record": "iteration_outcome",
                    "selected_signature": "candidate-0",
                    "accepted_move_committed": False,
                },
            ),
            ("final_topology", {"record": "final_state", "final_score": 7}),
        ]

        def render_synthetic_canonical(
            values: Sequence[tuple[str, Mapping[str, object]]],
        ) -> dict[str, object]:
            chunks: dict[str, list[bytes]] = {
                name: []
                for name in (
                    "contract",
                    "candidates",
                    "exact",
                    "acceptance",
                    "chain",
                    "final_topology",
                )
            }
            all_lines: list[bytes] = []
            for section, value in values:
                line = (
                    json.dumps(value, sort_keys=True, separators=(",", ":"))
                    + "\n"
                ).encode("utf-8")
                chunks[section].append(line)
                all_lines.append(line)
            data = b"".join(all_lines)
            canonical_sidecar.write_bytes(data)
            report: dict[str, object] = {
                "schema": "larch.chart_spr.semantic_digest",
                "schema_version": 1,
                "digest_algorithm": "sha256",
                "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
                "semantic_sha256": hashlib.sha256(data).hexdigest(),
                "record_count": len(values),
                "candidate_count": sum(
                    value.get("record") == "candidate" for _, value in values
                ),
                "exact_candidate_count": sum(
                    value.get("record") == "candidate_exact"
                    for _, value in values
                ),
                "iteration_count": sum(
                    value.get("record") == "iteration_begin"
                    for _, value in values
                ),
            }
            for section, section_chunks in chunks.items():
                report[f"{section}_sha256"] = hashlib.sha256(
                    b"".join(section_chunks)
                ).hexdigest()
            canonical_digest.write_text(
                json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
            )
            return report

        canonical_report = render_synthetic_canonical(canonical_records)
        validate_canonical_sidecar_source(
            canonical_root, ("full.ndjson", "full.json"), "synthetic-row"
        )
        for label, mutation in (
            ("component digest", {"candidates_sha256": "0" * 64}),
            ("record count", {"record_count": 999}),
            ("zero count with candidate record", {"candidate_count": 0}),
            ("compact schema", {"schema": "forged"}),
            ("compact extra key", {"unexpected": 0}),
        ):
            canonical_digest.write_text(
                json.dumps(canonical_report | mutation, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            assert_rejected(
                lambda: validate_canonical_sidecar_source(
                    canonical_root,
                    ("full.ndjson", "full.json"),
                    "synthetic-row",
                ),
                f"canonical sidecar {label} mutation",
            )
        forged_records = list(canonical_records)
        forged_records[0] = (
            "contract",
            {
                "record": "schema",
                "schema": "forged.ndjson",
                "schema_version": 1,
            },
        )
        render_synthetic_canonical(forged_records)
        assert_rejected(
            lambda: validate_canonical_sidecar_source(
                canonical_root,
                ("full.ndjson", "full.json"),
                "synthetic-row",
            ),
            "canonical NDJSON schema mutation",
        )

    # Byte reconstruction and terminal promotion helpers are non-overwriting.
    # A complete artifact is reusable only byte-for-byte; a transaction-owned
    # incomplete smoke bundle is emptied for a deterministic retry.
    with tempfile.TemporaryDirectory(prefix="wric-phase0-seal-") as temporary:
        transaction_root = Path(temporary)
        exact = transaction_root / "exact.tsv"
        ensure_exact_file(exact, b"exact\n", "synthetic exact artifact")
        ensure_exact_file(exact, b"exact\n", "synthetic exact artifact")
        require_derived_bytes(exact, b"exact\n", "synthetic derivation")
        assert_rejected(
            lambda: ensure_exact_file(
                exact, b"different\n", "synthetic exact artifact"
            ),
            "terminal overwrite",
        )
        assert_rejected(
            lambda: require_derived_bytes(
                exact, b"different\n", "synthetic derivation"
            ),
            "derived-byte mismatch",
        )
        for occupied_count in range(1, 4):
            partial_dir = transaction_root / f"partial-{occupied_count}-smoke"
            partial_status = transaction_root / f"partial-{occupied_count}.status"
            partial_stdout = transaction_root / f"partial-{occupied_count}.stdout"
            partial_stderr = transaction_root / f"partial-{occupied_count}.stderr"
            partial_bundle = (
                partial_dir,
                partial_status,
                partial_stdout,
                partial_stderr,
            )
            partial_dir.mkdir()
            for path in partial_bundle[1:occupied_count]:
                path.write_text("partial\n", encoding="utf-8")
            assert_rejected(
                lambda bundle=partial_bundle: smoke_bundle_complete_or_clean(
                    bundle, allow_cleanup=False
                ),
                f"fresh pre-existing partial smoke prefix {occupied_count}",
            )
            assert all(path_occupied(path) for path in partial_bundle[:occupied_count])
            assert not smoke_bundle_complete_or_clean(
                partial_bundle, allow_cleanup=True
            )
            assert not any(path_occupied(path) for path in partial_bundle)
        complete_dir = transaction_root / "complete-smoke"
        complete_dir.mkdir()
        complete_status = transaction_root / "complete.status"
        complete_stdout = transaction_root / "complete.stdout"
        complete_stderr = transaction_root / "complete.stderr"
        for path in (complete_status, complete_stdout, complete_stderr):
            path.write_text("complete\n", encoding="utf-8")
        assert smoke_bundle_complete_or_clean(
            (complete_dir, complete_status, complete_stdout, complete_stderr),
            allow_cleanup=False,
        )
        assert complete_status.read_text(encoding="utf-8") == "complete\n"

    valid_cpufreq = {
        "0": {
            "scaling_governor": "performance",
            "scaling_cur_freq": "2000000",
            "scaling_min_freq": "1000000",
            "scaling_max_freq": "3000000",
            "energy_performance_preference": "performance",
            "boost": "1",
        }
    }
    validate_live_cpufreq(valid_cpufreq, {"0"}, "synthetic-live")
    for mutation in (
        {"0": {key: value for key, value in valid_cpufreq["0"].items() if key != "boost"}},
        {"0": valid_cpufreq["0"] | {"scaling_min_freq": "4000000"}},
        {"0": valid_cpufreq["0"] | {"scaling_cur_freq": "0"}},
        {"0": valid_cpufreq["0"] | {"energy_performance_preference": ""}},
    ):
        try:
            validate_live_cpufreq(mutation, {"0"}, "synthetic-live")
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"invalid live cpufreq evidence was accepted: {mutation}"
            )
    for invalid_cpus in ([0, 0], [1, 0], [True], [], [0, -1]):
        try:
            validate_sorted_cpu_list(invalid_cpus, "synthetic")
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"invalid live CPU list was accepted: {invalid_cpus}"
            )
    synthetic_unpinned = dataclasses.replace(
        primary, affinity_kind="unpinned", affinity_cpus="0-1"
    )
    validate_live_affinity_lists(
        synthetic_unpinned, [0, 1], [0, 1], [0, 1, 2], "synthetic-unpinned"
    )
    assert_rejected(
        lambda: validate_live_affinity_lists(
            synthetic_unpinned,
            [0, 1],
            [0, 2],
            [0, 1, 2],
            "synthetic-unpinned",
        ),
        "unpinned inherited-affinity mismatch",
    )

    summary_spec = RowSpec(
        "summary-row",
        "summary-group",
        "summary-workload",
        primary.capture_id,
        "chart_spr_grammar_exact",
        "8",
        measured_trial_target=3,
    )
    summary_key = (summary_spec.method, summary_spec.requested_workers)
    summary_rows = [
        {
            "method": summary_key[0],
            "requested_workers": summary_key[1],
            "status": "ok",
            "trial_index": str(index),
            "wall_clock_s": wall,
            "user_cpu_s": wall,
            "system_cpu_s": "0.100000",
            "max_rss_kb": str(100 + index),
            "peak_sampled_rss_kb": str(200 + index),
        }
        for index, wall in ((1, "3.000000"), (2, "1.000000"), (3, "2.000000"))
    ]
    summary = render_capture_summary(
        summary_rows,
        {summary_key: summary_spec},
        {summary_key: (Decimal("1.5"), Decimal("0.5"), Decimal("1.0"))},
    ).decode("utf-8")
    assert "\t3\t2.000000\t3.000000\t2.000000\t0.100000\t103\t203\t1.000000000\n" in summary
    try:
        render_capture_summary(
            summary_rows[:1],
            {summary_key: summary_spec},
            {summary_key: (Decimal("1"),)},
        )
    except BootstrapError:
        pass
    else:
        raise AssertionError("finite N=1 timing was accepted as a median")
    timeout_summary = render_capture_summary(
        [summary_rows[0] | {"status": "timeout"}],
        {summary_key: summary_spec},
        {},
    ).decode("utf-8")
    assert "timeout_characterization_non_performance\t1\t-\t-\t-\t-" in timeout_summary

    unanimous_native = {
        field: "same" for field in REPEATED_UNANIMOUS_RAW_FIELDS
    }
    unanimous_native.update(
        {
            "method": "sample_explore_merge",
            "requested_workers": "native",
            "output_semantic_sha256": "a" * 64,
            "trial_semantic_sha256": "b" * 64,
            "canonical_argv_sha256": "c" * 64,
        }
    )
    validate_repeated_unanimity(
        Path("/synthetic"),
        ("sample_explore_merge", "native"),
        (unanimous_native, dict(unanimous_native)),
    )
    try:
        validate_repeated_unanimity(
            Path("/synthetic"),
            ("sample_explore_merge", "native"),
            (
                unanimous_native,
                unanimous_native | {"output_semantic_sha256": "d" * 64},
            ),
        )
    except BootstrapError:
        pass
    else:
        raise AssertionError("repeated semantic mutation was accepted")

    process_key = ("chart_spr_grammar_exact", "1")
    capped_process = {
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
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
        "process_rss_limit_bytes": str(primary.rss_limit_bytes),
        "manifest_rss_limit_bytes": str(primary.rss_limit_bytes),
        "max_rss_kb": "100",
        "peak_sampled_rss_kb": "120",
    }
    validate_standard_process_contract(primary, capped_process, "ok", process_key)
    capped_timeout_process = capped_process | {
        "runner_outcome": "timeout",
        "runner_exit_code": "124",
        "exit_code": "-1",
        "term_signal": "15",
        "timed_out": "1",
    }
    validate_standard_process_contract(
        primary, capped_timeout_process, "timeout", process_key
    )

    unavailable_semantic_sentinels = {
        field: "-" for field in UNAVAILABLE_SEMANTIC_FIELDS
    }
    validate_unavailable_semantic_sentinels(
        unavailable_semantic_sentinels, "synthetic unavailable row"
    )
    for field in UNAVAILABLE_SEMANTIC_FIELDS:
        mutation = unavailable_semantic_sentinels | {field: "f" * 64}
        try:
            validate_unavailable_semantic_sentinels(
                mutation, "synthetic unavailable row"
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"timeout semantic sentinel mutation was accepted: {field}"
            )

    synthetic_manifest_semantics = {
        "oracle_search_semantic_sha256": "1" * 64,
        "oracle_output_semantic_sha256": "2" * 64,
        "oracle_trial_semantic_sha256": "3" * 64,
        "canonical_argv_sha256": "4" * 64,
    }
    synthetic_success_semantics = {
        "search_semantic_sha256": "1" * 64,
        "output_semantic_sha256": "2" * 64,
        "trial_semantic_sha256": "3" * 64,
        "canonical_digest": "3" * 64,
        "canonical_argv_sha256": "4" * 64,
    }
    validate_success_semantic_manifest_bindings(
        synthetic_success_semantics,
        synthetic_manifest_semantics,
        "synthetic strict smoke",
    )
    for raw_field, _ in STRICT_SUCCESS_SEMANTIC_BINDINGS:
        try:
            validate_success_semantic_manifest_bindings(
                synthetic_success_semantics | {raw_field: "f" * 64},
                synthetic_manifest_semantics,
                "synthetic strict smoke",
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"strict-smoke semantic mutation was accepted: {raw_field}"
            )

    def assert_process_mutation_rejected(
        row: Mapping[str, str], status: str, field: str, value: str
    ) -> None:
        try:
            validate_standard_process_contract(
                primary, row | {field: value}, status, process_key
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError(
                f"standard {status} process mutation was accepted: {field}={value}"
            )

    for field, value in (
        ("rss_limit_enabled", "0"),
        ("rss_limit_observed", "1"),
        ("rss_limit_exceeded", "1"),
        ("rss_limit_trigger_bytes", str(primary.rss_limit_bytes + 1)),
        ("rss_limit_term_sent", "1"),
        ("rss_limit_kill_sent", "1"),
        ("process_rss_limit_bytes", "0"),
        ("manifest_rss_limit_bytes", "-"),
        ("runner_outcome", "rss_limit"),
        ("runner_exit_code", "123"),
        ("timed_out", "1"),
        ("max_rss_kb", str(primary.rss_limit_bytes // 1024 + 1)),
        ("peak_sampled_rss_kb", str(primary.rss_limit_bytes // 1024 + 1)),
        ("exit_code", "FORGED"),
        ("term_signal", "FORGED"),
        ("exit_code", "-1"),
        ("term_signal", "15"),
    ):
        assert_process_mutation_rejected(capped_process, "ok", field, value)
    for field, value in (
        ("rss_limit_enabled", "0"),
        ("rss_limit_observed", "1"),
        ("rss_limit_exceeded", "1"),
        ("rss_limit_trigger_bytes", str(primary.rss_limit_bytes + 1)),
        ("rss_limit_term_sent", "1"),
        ("rss_limit_kill_sent", "1"),
        ("process_rss_limit_bytes", "0"),
        ("manifest_rss_limit_bytes", "-"),
        ("runner_outcome", "exited"),
        ("runner_exit_code", "0"),
        ("timed_out", "0"),
        ("max_rss_kb", str(primary.rss_limit_bytes // 1024 + 1)),
        ("peak_sampled_rss_kb", str(primary.rss_limit_bytes // 1024 + 1)),
        ("exit_code", "FORGED"),
        ("term_signal", "FORGED"),
        ("exit_code", "0"),
        ("term_signal", "0"),
    ):
        assert_process_mutation_rejected(
            capped_timeout_process, "timeout", field, value
        )
    assert any(row.requested_workers == "default" and row.method == "chart_spr_grammar_exact" for row in rows)
    assert any(row.requested_workers == "auto" and row.workload_name == "small-auto-overhead" for row in rows)
    # Same SMT/unpinned affinity must reuse medium auto rather than creating an
    # ambiguous second row.  A distinct affinity must add both auto/default.
    captures_distinct, rows_distinct = build_matrix(PHYSICAL_AFFINITY, SMT_AFFINITY, "2-15")
    assert "medium-primary32k4-unpinned" in capture_map(captures_distinct)
    assert any(
        row.run_group == "p0-primary-unpinned"
        and row.requested_workers == "default"
        for row in rows_distinct
    )
    assert_unique_resolution(
        rows_distinct,
        capture_map(captures_distinct),
        fixture_hashes,
    )
    auto_spec = next(
        row for row in rows if row.requested_workers == "auto" and row.capture_id == "medium-primary32k4-smt"
    )
    auto_contract = base_manifest_contract(smt, auto_spec, "medium", "ref")
    assert auto_contract["requested_workers"] == "0"
    default_spec = next(row for row in rows if row.requested_workers == "default")
    default_contract = base_manifest_contract(by_id[default_spec.capture_id], default_spec, "medium", "ref")
    assert default_contract["worker_option"] == "none"
    argv_hash = canonical_argv_digest(("@binary:working_chart", "--seed", "1", "-o", "@output"))
    assert argv_hash == canonical_argv_digest(("@binary:working_chart", "--seed", "1", "-o", "@output"))
    assert len(trial_digest("m", "-", "0" * 64, argv_hash)) == 64
    classify_defaults = {
        "exit_code": 1,
        "term_signal": 0,
        "timed_out": False,
        "rss_limit_exceeded": False,
        "core_dumped": False,
        "output_exists": False,
        "stderr_sha256": REAL20D_REFUSAL_SHA256,
    }
    assert classify_real_preflight_observation(**classify_defaults) == "expected_infeasible"
    assert classify_real_preflight_observation(
        **(classify_defaults | {"exit_code": 0, "output_exists": True})
    ) == "ok"
    assert classify_real_preflight_observation(
        **(classify_defaults | {"timed_out": True, "stderr_sha256": "-"})
    ) == "scale_limit:timeout_seconds"
    assert classify_real_preflight_observation(
        **(
            classify_defaults
            | {
                "timed_out": True,
                "rss_limit_exceeded": True,
                "stderr_sha256": "-",
            }
        )
    ) == "scale_limit:rss_limit_bytes"
    assert classify_real_preflight_observation(
        **(classify_defaults | {"term_signal": 9})
    ) == "blocked"
    approval = {
        "approved_row_ids": ["w1", "w8"],
        "expected_reason_sha256": REAL20D_REFUSAL_SHA256,
        "capture_status_sha256": "a" * 64,
    }
    approval_expectation = dict(approval)
    assert_exact_approval_mapping(approval, approval_expectation, "synthetic")
    try:
        assert_exact_approval_mapping(
            approval | {"capture_status_sha256": "b" * 64},
            approval_expectation,
            "synthetic",
        )
    except BootstrapError:
        pass
    else:
        raise AssertionError("mutated real approval binding was accepted")
    try:
        assert_exact_approval_mapping(
            approval | {"uncontracted": "value"},
            approval_expectation,
            "synthetic",
        )
    except BootstrapError:
        pass
    else:
        raise AssertionError("approval with an extra key was accepted")

    assert render_capture_plan(captures) != render_capture_plan(
        [dataclasses.replace(captures[0], iterations=9), *captures[1:]]
    )
    assert render_row_plan(rows) != render_row_plan(
        [dataclasses.replace(rows[0], row_id="mutated-row"), *rows[1:]]
    )
    assert render_timeout_plan(rows) != render_timeout_plan(
        [dataclasses.replace(rows[0], timeout_policy="allow"), *rows[1:]]
    )

    with tempfile.TemporaryDirectory(prefix="wric-phase0-metrics-test-") as temp:
        metrics_root = Path(temp)
        normal_metrics = {
            "schema_version": "2",
            "outcome": "exited",
            "exit_code": "1",
            "term_signal": "0",
            "timed_out": "0",
            "runner_exit_code": "1",
            "wall_seconds": "0.001",
            "user_seconds": "0.000",
            "system_seconds": "0.000",
            "max_rss_kb": "1",
            "peak_sampled_rss_kb": "1",
            "peak_sampled_swap_kb": "0",
            "rss_kb_unit": "1024_bytes",
            "proc_status_samples": "1",
            "proc_rss_samples": "1",
            "proc_swap_samples": "1",
            "proc_group_samples": "1",
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
            "rss_limit_bytes": str(REAL_PREFLIGHT_BYTES),
            "rss_limit_enabled": "1",
            "rss_limit_observed": "0",
            "rss_limit_exceeded": "0",
            "rss_limit_trigger_bytes": "0",
            "rss_limit_term_sent": "0",
            "rss_limit_kill_sent": "0",
        }
        assert set(normal_metrics) == PROCESS_METRICS_V2_FIELDS
        assert (
            len(WRAPPER_CALIBRATION_WORKLOAD_STDOUT_TEXT.encode("ascii"))
            == WRAPPER_CALIBRATION_WORKLOAD_STDOUT_BYTES
        )
        assert sha256_bytes(
            WRAPPER_CALIBRATION_WORKLOAD_STDOUT_TEXT.encode("ascii")
        ) == WRAPPER_CALIBRATION_WORKLOAD_STDOUT_SHA256

        calibration_stdout = {
            "bytes": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_BYTES,
            "sha256": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_SHA256,
        }
        empty_stream = {"bytes": 0, "sha256": EMPTY_SHA256}
        calibration_wait4 = {
            "core_dumped": False,
            "exit_code": 0,
            "max_rss_kb": 2048,
            "system_us": 0,
            "term_signal": 0,
            "user_us": 48_000_000,
        }
        calibration_metrics = normal_metrics | {
            "exit_code": "0",
            "runner_exit_code": "0",
            "wall_seconds": "6.050000000",
            "user_seconds": "48.000000000",
            "system_seconds": "0.000000000",
            "rss_limit_bytes": str(RSS_LIMIT_BYTES),
        }

        calibration_clock = 10_000_000_000
        first_arm_started_ns = 0
        last_arm_ended_ns = 0

        def calibration_arm(*, wrapped: bool) -> dict[str, object]:
            nonlocal calibration_clock, first_arm_started_ns, last_arm_ended_ns
            wall_ns = 6_060_000_000 if wrapped else 6_000_000_000
            started_ns = calibration_clock
            ended_ns = started_ns + wall_ns
            if first_arm_started_ns == 0:
                first_arm_started_ns = started_ns
            last_arm_ended_ns = ended_ns
            calibration_clock = ended_ns + 10_000_000
            result: dict[str, object] = {
                "argv": list(WRAPPER_CALIBRATION_ARGV),
                "outer_clock": "CLOCK_MONOTONIC",
                "outer_ended_ns": ended_ns,
                "outer_started_ns": started_ns,
                "outer_wall_ns": wall_ns,
                "redirections": dict(WRAPPER_CALIBRATION_REDIRECTIONS),
                "stderr": dict(empty_stream),
                "stdout": dict(calibration_stdout),
                "wait4": dict(calibration_wait4),
            }
            if wrapped:
                result.update(
                    {
                        "driver_stderr": dict(empty_stream),
                        "driver_stdout": dict(empty_stream),
                        "process_metrics": dict(calibration_metrics),
                    }
                )
            return result

        def calibration_pair(index: int) -> dict[str, object]:
            if index % 2:
                direct = calibration_arm(wrapped=False)
                wrapped = calibration_arm(wrapped=True)
            else:
                wrapped = calibration_arm(wrapped=True)
                direct = calibration_arm(wrapped=False)
            return {
                "direct": direct,
                "index": index,
                "order": (
                    "direct_then_wrapped" if index % 2 else "wrapped_then_direct"
                ),
                "wrapped": wrapped,
            }

        calibration_warmups = [calibration_pair(1), calibration_pair(2)]
        calibration_pairs = [calibration_pair(index) for index in range(1, 12)]
        coverage_started_ns = first_arm_started_ns - 100_000_000
        coverage_ended_ns = last_arm_ended_ns + 100_000_000
        coverage_duration_ns = coverage_ended_ns - coverage_started_ns
        minimum_scan_count = max(
            2,
            (
                coverage_duration_ns
                + WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
                - 1
            )
            // WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
            + 1,
        )
        synthetic_scan_count = (
            coverage_duration_ns + 250_000_000 - 1
        ) // 250_000_000 + 2

        def synthetic_identity(
            sha256: str, inode: int, mode: int, size_bytes: int
        ) -> dict[str, object]:
            return {
                "ctime_ns": 1_700_000_000_000_000_000 + inode,
                "device": 42,
                "inode": inode,
                "mode": mode,
                "mtime_ns": 1_600_000_000_000_000_000 + inode,
                "sha256": sha256,
                "size_bytes": size_bytes,
            }

        def synthetic_closure(identity: Mapping[str, object]) -> dict[str, object]:
            return {"end": dict(identity), "start": dict(identity)}

        def synthetic_directory_identity(
            inode: int, mode: int
        ) -> dict[str, object]:
            return {
                "ctime_ns": 1_700_000_000_000_000_000 + inode,
                "device": 42,
                "inode": inode,
                "mode": mode,
                "mtime_ns": 1_600_000_000_000_000_000 + inode,
            }

        provenance_specs = {
            "controller": ("b" * 64, 101, 0o755, 40_000),
            "runner": ("c" * 64, 102, 0o755, 11_000_000),
            "workload": ("d" * 64, 103, 0o755, 9_000_000),
            "workload_source": (
                WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256,
                104,
                0o644,
                2_000,
            ),
        }
        input_provenance: dict[str, object] = {}
        for offset, (role, (sha256, inode, mode, size_bytes)) in enumerate(
            provenance_specs.items(), 1
        ):
            input_provenance[role] = synthetic_closure(
                synthetic_identity(sha256, inode, mode, size_bytes)
            )
            snapshot_mode = 0o444 if role == "workload_source" else 0o555
            input_provenance[f"{role}_snapshot"] = synthetic_closure(
                synthetic_identity(sha256, 200 + offset, snapshot_mode, size_bytes)
            )
        for role in ("controller", "runner", "workload"):
            input_provenance[f"{role}_execution"] = json.loads(
                json.dumps(input_provenance[f"{role}_snapshot"])
            )
        input_provenance["launch_state"] = synthetic_closure(
            synthetic_identity("e" * 64, 305, 0o444, 1_000)
        )
        input_provenance["stage_directory"] = synthetic_closure(
            synthetic_directory_identity(301, 0o700)
        )
        input_provenance["input_snapshot_directory"] = synthetic_closure(
            synthetic_directory_identity(302, 0o700)
        )
        output_parent_identity = {"device": 42, "inode": 303, "mode": 0o755}
        input_provenance["output_parent_directory"] = synthetic_closure(
            output_parent_identity
        )

        calibration_summary = {
            "decision": "PASS",
            "direct_median_ns": "6000000000",
            "median_paired_ratio": "1.010000000000",
            "ratio_of_medians": "1.010000000000",
            "threshold": "1.020000000000",
            "wrapped_median_ns": "6060000000",
        }
        calibration = {
            "affinity_cpus": PHYSICAL_AFFINITY,
            "clock": "CLOCK_MONOTONIC",
            "controller": {
                "schema": WRAPPER_CALIBRATION_CONTROLLER_SCHEMA,
                "sha256": "b" * 64,
                "uri": "repo://tools/wric_wrapper_calibration.py",
                "version": WRAPPER_CALIBRATION_CONTROLLER_VERSION,
            },
            "environment": dict(WRAPPER_CALIBRATION_ENVIRONMENT),
            "input_provenance": input_provenance,
            "live_guard": {
                "coverage_duration_ns": coverage_duration_ns,
                "coverage_ended_ns": coverage_ended_ns,
                "coverage_started_ns": coverage_started_ns,
                "final_heartbeat_ns": coverage_ended_ns + 100_000_000,
                "first_heartbeat_ns": coverage_started_ns - 100_000_000,
                "forbidden_process_matches": 0,
                "max_consecutive_heartbeat_gap_ns": 500_000_000,
                "max_consecutive_heartbeat_gap_threshold_ns": (
                    WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
                ),
                "max_unselected_smt_busy_ppm": 0,
                "minimum_scan_count": minimum_scan_count,
                "poll_interval_ms": WRAPPER_CALIBRATION_LIVE_POLL_MS,
                "scan_count": synthetic_scan_count,
                "scan_failures": 0,
                "started_ns": coverage_started_ns - 200_000_000,
                "stopped_ns": coverage_ended_ns + 200_000_000,
                "threshold_ppm": WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM,
            },
            "measured_pairs": calibration_pairs,
            "physical_cores": wrapper_calibration_physical_cores(),
            "preflight": {
                "duration_ns": WRAPPER_CALIBRATION_MIN_PREFLIGHT_NS,
                "max_logical_cpu_busy_ppm": 0,
                "threshold_ppm": WRAPPER_CALIBRATION_QUIET_THRESHOLD_PPM,
            },
            "repo_revision": FROZEN_REVISION,
            "rss_limit_bytes": RSS_LIMIT_BYTES,
            "runner": {
                "process_metrics_schema_version": 2,
                "sha256": "c" * 64,
                "uri": "repo://build/bin/wric-process-metrics",
            },
            "schema": WRAPPER_CALIBRATION_SCHEMA,
            "schema_version": WRAPPER_CALIBRATION_SCHEMA_VERSION,
            "summary": calibration_summary,
            "timeout_seconds": TIMEOUT_SECONDS,
            "warmup_pairs": calibration_warmups,
            "workload": {
                "argv": list(WRAPPER_CALIBRATION_ARGV),
                "schema": WRAPPER_CALIBRATION_WORKLOAD_SCHEMA,
                "sha256": "d" * 64,
                "source_sha256": WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256,
                "source_uri": "repo://tools/wric_wrapper_calibration_workload.cpp",
                "stderr_bytes": 0,
                "stderr_sha256": EMPTY_SHA256,
                "stdout_bytes": calibration_stdout["bytes"],
                "stdout_sha256": calibration_stdout["sha256"],
                "stdout_text": WRAPPER_CALIBRATION_WORKLOAD_STDOUT_TEXT,
                "uri": "repo://build/bin/wric-wrapper-calibration-workload",
                "version": WRAPPER_CALIBRATION_WORKLOAD_VERSION,
                "version_text": WRAPPER_CALIBRATION_WORKLOAD_VERSION_TEXT,
            },
        }

        def validate_synthetic_calibration(value: object) -> None:
            validate_wrapper_calibration_mapping(
                value,
                expected_runner_sha256="c" * 64,
                expected_controller_sha256="b" * 64,
                expected_workload_sha256="d" * 64,
                expected_workload_source_sha256=(
                    WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256
                ),
                label="synthetic wrapper calibration",
            )

        validate_synthetic_calibration(calibration)

        def mutated_calibration() -> dict[str, object]:
            return json.loads(json.dumps(calibration))

        invalid_calibrations: list[tuple[str, dict[str, object]]] = []
        mutation = mutated_calibration()
        mutation["uncontracted"] = "value"
        invalid_calibrations.append(("extra-top-key", mutation))
        mutation = mutated_calibration()
        mutation["runner"]["sha256"] = "f" * 64  # type: ignore[index]
        invalid_calibrations.append(("runner-sha", mutation))
        mutation = mutated_calibration()
        mutation["controller"]["version"] = True  # type: ignore[index]
        invalid_calibrations.append(("boolean-controller-version", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][1]["order"] = "direct_then_wrapped"  # type: ignore[index]
        invalid_calibrations.append(("pair-order", mutation))
        mutation = mutated_calibration()
        del mutation["measured_pairs"][0]["wrapped"]["process_metrics"]["proc_group_samples"]  # type: ignore[index]
        invalid_calibrations.append(("missing-process-metric", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["wrapped"]["process_metrics"]["live_descendants_at_return"] = "1"  # type: ignore[index]
        invalid_calibrations.append(("lifecycle-error", mutation))
        mutation = mutated_calibration()
        for pair in mutation["measured_pairs"]:  # type: ignore[union-attr]
            pair["wrapped"]["outer_wall_ns"] = 6_180_000_000
        invalid_calibrations.append(("overhead-gate", mutation))
        mutation = mutated_calibration()
        mutation["summary"]["ratio_of_medians"] = "1.000000000000"  # type: ignore[index]
        invalid_calibrations.append(("forged-summary", mutation))
        mutation = mutated_calibration()
        mutation["workload"]["source_sha256"] = "e" * 64  # type: ignore[index]
        invalid_calibrations.append(("unbound-workload-source", mutation))
        mutation = mutated_calibration()
        mutation["workload"]["stdout_text"] += "forged\n"  # type: ignore[index,operator]
        invalid_calibrations.append(("forged-workload-stdout", mutation))
        mutation = mutated_calibration()
        mutation["physical_cores"][1]["core_id"] = mutation["physical_cores"][0]["core_id"]  # type: ignore[index]
        invalid_calibrations.append(("aliased-physical-core", mutation))
        mutation = mutated_calibration()
        mutation["live_guard"]["scan_count"] = 1  # type: ignore[index]
        invalid_calibrations.append(("watcher-one-scan", mutation))
        mutation = mutated_calibration()
        mutation["live_guard"]["max_consecutive_heartbeat_gap_ns"] = (  # type: ignore[index]
            WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS + 1
        )
        invalid_calibrations.append(("watcher-large-heartbeat-gap", mutation))
        mutation = mutated_calibration()
        truncated_end = mutation["measured_pairs"][-1]["direct"]["outer_ended_ns"] - 1  # type: ignore[index]
        mutation["live_guard"]["coverage_ended_ns"] = truncated_end  # type: ignore[index]
        truncated_duration = (
            truncated_end
            - mutation["live_guard"]["coverage_started_ns"]  # type: ignore[index]
        )
        mutation["live_guard"]["coverage_duration_ns"] = truncated_duration  # type: ignore[index]
        mutation["live_guard"]["minimum_scan_count"] = max(  # type: ignore[index]
            2,
            (
                truncated_duration
                + WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
                - 1
            )
            // WRAPPER_CALIBRATION_LIVE_MAX_GAP_NS
            + 1,
        )
        invalid_calibrations.append(("watcher-truncated-arm-coverage", mutation))
        mutation = mutated_calibration()
        mutation["live_guard"]["final_heartbeat_ns"] = 0  # type: ignore[index]
        invalid_calibrations.append(("watcher-no-heartbeat", mutation))
        mutation = mutated_calibration()
        mutation["live_guard"]["scan_failures"] = 1  # type: ignore[index]
        invalid_calibrations.append(("watcher-failed-scan", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["direct"]["wait4"]["user_us"] = 40_000_000  # type: ignore[index]
        invalid_calibrations.append(("direct-under-saturation", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["direct"]["wait4"]["user_us"] = 54_000_000  # type: ignore[index]
        invalid_calibrations.append(("direct-over-saturation", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["direct"]["outer_wall_ns"] = 4_000_000_000  # type: ignore[index]
        invalid_calibrations.append(("shortened-workload", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["wrapped"]["process_metrics"]["wall_seconds"] = "5.800000000"  # type: ignore[index]
        invalid_calibrations.append(("spliced-inner-wall", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["wrapped"]["process_metrics"]["user_seconds"] = "47.000000000"  # type: ignore[index]
        invalid_calibrations.append(("spliced-inner-cpu", mutation))
        mutation = mutated_calibration()
        mutation["measured_pairs"][0]["wrapped"]["process_metrics"]["max_rss_kb"] = "2049"  # type: ignore[index]
        invalid_calibrations.append(("spliced-inner-rss", mutation))
        mutation = mutated_calibration()
        direct_arm = mutation["measured_pairs"][0]["direct"]  # type: ignore[index]
        wrapped_arm = mutation["measured_pairs"][0]["wrapped"]  # type: ignore[index]
        wrapped_arm["outer_started_ns"] = direct_arm["outer_ended_ns"] - 1
        wrapped_arm["outer_ended_ns"] = (
            wrapped_arm["outer_started_ns"] + wrapped_arm["outer_wall_ns"]
        )
        invalid_calibrations.append(("spliced-overlapping-arm-interval", mutation))
        mutation = mutated_calibration()
        for arm_name in ("direct", "wrapped"):
            source_arm = mutation["measured_pairs"][1][arm_name]  # type: ignore[index]
            copied_arm = mutation["measured_pairs"][3][arm_name]  # type: ignore[index]
            copied_arm["outer_started_ns"] = source_arm["outer_started_ns"]
            copied_arm["outer_ended_ns"] = source_arm["outer_ended_ns"]
        invalid_calibrations.append(("copied-measured-arm-intervals", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["runner"]["end"]["inode"] += 1  # type: ignore[index,operator]
        invalid_calibrations.append(("runner-live-identity-replaced", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["workload_snapshot"]["end"]["mode"] = 0o755  # type: ignore[index]
        invalid_calibrations.append(("writable-workload-snapshot", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["controller_execution"]["start"]["inode"] += 1  # type: ignore[index,operator]
        mutation["input_provenance"]["controller_execution"]["end"]["inode"] += 1  # type: ignore[index,operator]
        invalid_calibrations.append(("controller-execution-not-snapshot", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["runner_execution"]["end"]["sha256"] = "f" * 64  # type: ignore[index]
        invalid_calibrations.append(("runner-execution-hash-changed", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["stage_directory"]["end"]["inode"] += 1  # type: ignore[index,operator]
        invalid_calibrations.append(("stage-directory-replaced", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["stage_directory"]["end"]["ctime_ns"] += 1  # type: ignore[index,operator]
        invalid_calibrations.append(("stage-directory-ctime-changed", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["input_snapshot_directory"]["end"]["mode"] = 0o755  # type: ignore[index]
        invalid_calibrations.append(("input-directory-mode-changed", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["launch_state"]["end"]["inode"] += 1  # type: ignore[index,operator]
        invalid_calibrations.append(("launch-state-replaced", mutation))
        mutation = mutated_calibration()
        mutation["input_provenance"]["output_parent_directory"]["end"]["inode"] += 1  # type: ignore[index,operator]
        invalid_calibrations.append(("output-parent-replaced", mutation))
        for name, mutation in invalid_calibrations:
            try:
                validate_synthetic_calibration(mutation)
            except BootstrapError:
                pass
            else:
                raise AssertionError(
                    f"invalid synthetic wrapper calibration accepted: {name}"
                )

        calibration_path = metrics_root / "wrapper-calibration.json"
        write_text_exclusive(
            calibration_path,
            json.dumps(calibration, sort_keys=True, indent=2) + "\n",
            0o444,
        )
        load_wrapper_calibration(
            calibration_path,
            expected_runner_sha256="c" * 64,
            expected_controller_sha256="b" * 64,
            expected_workload_sha256="d" * 64,
            expected_workload_source_sha256=(
                WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256
            ),
            label="synthetic canonical wrapper calibration",
        )
        os.chmod(calibration_path, 0o644)
        try:
            load_wrapper_calibration(
                calibration_path,
                expected_runner_sha256="c" * 64,
                expected_controller_sha256="b" * 64,
                expected_workload_sha256="d" * 64,
                expected_workload_source_sha256=(
                    WRAPPER_CALIBRATION_WORKLOAD_SOURCE_SHA256
                ),
                label="synthetic writable wrapper calibration",
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError("writable wrapper calibration was accepted")

        def write_synthetic_metrics(name: str, values: Mapping[str, str]) -> Path:
            path = metrics_root / name
            write_text_exclusive(
                path,
                "".join(f"{key}={value}\n" for key, value in values.items()),
            )
            return path

        normal_path = write_synthetic_metrics("normal.metrics", normal_metrics)
        read_process_metrics(normal_path, 1, REAL_PREFLIGHT_BYTES)

        def raw_process_copy(values: Mapping[str, str]) -> dict[str, str]:
            return {
                raw_field: values[metric_field]
                for raw_field, metric_field in RAW_PROCESS_METRIC_BINDINGS
            }

        normal_raw_process = raw_process_copy(normal_metrics)
        validate_raw_process_metric_bindings(
            normal_raw_process, normal_metrics, "synthetic successful row"
        )
        for raw_field in (
            "wall_clock_s",
            "user_cpu_s",
            "max_rss_kb",
            "peak_sampled_rss_kb",
            "peak_sampled_swap_kb",
            "runner_outcome",
            "exit_code",
        ):
            try:
                validate_raw_process_metric_bindings(
                    normal_raw_process | {raw_field: "forged"},
                    normal_metrics,
                    "synthetic successful row",
                )
            except BootstrapError:
                pass
            else:
                raise AssertionError(
                    f"successful raw/process splice was accepted: {raw_field}"
                )

        canonical_dag = {
            "schema": "larch.dag.semantic_digest",
            "schema_version": 1,
            "digest_algorithm": "sha256",
            "semantic_sha256": "1" * 64,
            "clades_sha256": "2" * 64,
            "productions_sha256": "3" * 64,
            "clade_count": 10,
            "production_count": 9,
            "parsimony_min": 7,
        }
        canonical_dag_path = metrics_root / "canonical-dag.json"
        write_text_exclusive(
            canonical_dag_path,
            json.dumps(canonical_dag, sort_keys=True) + "\n",
        )
        assert read_canonical_dag_result(
            canonical_dag_path, "synthetic canonical DAG"
        ) == canonical_dag
        dag_info_path = metrics_root / "dag-info.out"
        write_text_exclusive(
            dag_info_path,
            "nodes: 10\nedges: 9\nparsimony_min: score:7, count:1\n",
        )
        assert dag_info_unsigned_value(dag_info_path, "nodes") == "10"
        assert dag_info_unsigned_value(dag_info_path, "edges") == "9"
        assert dag_info_parsimony_min(dag_info_path) == "7"
        for name, mutation in (
            ("extra", {"unexpected": 0}),
            ("schema", {"schema": "forged"}),
            ("hash", {"semantic_sha256": "forged"}),
            ("count", {"clade_count": True}),
        ):
            path = metrics_root / f"canonical-dag-{name}.json"
            write_text_exclusive(
                path,
                json.dumps(canonical_dag | mutation, sort_keys=True) + "\n",
            )
            try:
                read_canonical_dag_result(path, "synthetic canonical DAG")
            except BootstrapError:
                pass
            else:
                raise AssertionError(
                    f"canonical DAG mutation was accepted: {name}"
                )
        precedence_peak_kb = REAL_PREFLIGHT_BYTES // 1024 + 1
        timeout_precedence_metrics = normal_metrics | {
            "outcome": "timeout",
            "exit_code": "-1",
            "term_signal": "9",
            "timed_out": "1",
            "runner_exit_code": "124",
            "peak_sampled_rss_kb": str(precedence_peak_kb),
            "timeout_term_sent": "1",
            "timeout_kill_sent": "1",
            "rss_limit_observed": "1",
            "rss_limit_trigger_bytes": str(precedence_peak_kb * 1024),
        }
        timeout_precedence_path = write_synthetic_metrics(
            "timeout-precedence.metrics", timeout_precedence_metrics
        )
        # Timeout wins the runner outcome during TERM grace, while the RSS
        # crossing remains valid forensic schema. Standard capture acceptance
        # rejects the independently visible crossing (tested above).
        read_process_metrics(
            timeout_precedence_path, 124, REAL_PREFLIGHT_BYTES
        )
        timeout_raw_process = raw_process_copy(timeout_precedence_metrics)
        validate_raw_process_metric_bindings(
            timeout_raw_process,
            timeout_precedence_metrics,
            "synthetic timeout row",
        )
        for raw_field in (
            "wall_clock_s",
            "peak_sampled_rss_kb",
            "rss_limit_observed",
            "term_signal",
        ):
            try:
                validate_raw_process_metric_bindings(
                    timeout_raw_process | {raw_field: "forged"},
                    timeout_precedence_metrics,
                    "synthetic timeout row",
                )
            except BootstrapError:
                pass
            else:
                raise AssertionError(
                    f"timeout raw/process splice was accepted: {raw_field}"
                )
        restore_sigchld_metrics = normal_metrics | {
            "outcome": "setup_error",
            "exit_code": "125",
            "runner_exit_code": "125",
            "child_error_stage": "restore_sigchld",
            "child_error_errno": "22",
        }
        restore_sigchld_path = write_synthetic_metrics(
            "restore-sigchld.metrics", restore_sigchld_metrics
        )
        read_process_metrics(
            restore_sigchld_path, 125, REAL_PREFLIGHT_BYTES
        )
        invalid_metrics = (
            ("extra", normal_metrics | {"uncontracted": "0"}),
            (
                "missing",
                {key: value for key, value in normal_metrics.items() if key != "monitor_error"},
            ),
            ("old-schema", normal_metrics | {"schema_version": "1"}),
            (
                "live-descendant",
                normal_metrics | {"live_descendants_at_return": "1"},
            ),
            (
                "monitor-count",
                normal_metrics | {"monitor_error_count": "1"},
            ),
            (
                "rss-trigger",
                normal_metrics
                | {
                    "rss_limit_observed": "1",
                    "rss_limit_trigger_bytes": str(REAL_PREFLIGHT_BYTES),
                },
            ),
            (
                "rss-trigger-over-peak",
                normal_metrics
                | {
                    "rss_limit_observed": "1",
                    "rss_limit_trigger_bytes": str(REAL_PREFLIGHT_BYTES + 1024),
                },
            ),
        )
        for name, values in invalid_metrics:
            path = write_synthetic_metrics(f"{name}.metrics", values)
            try:
                read_process_metrics(path, 1, REAL_PREFLIGHT_BYTES)
            except BootstrapError:
                pass
            else:
                raise AssertionError(f"invalid schema-v2 metrics accepted: {name}")
        try:
            read_process_metrics(normal_path, 1, RSS_LIMIT_BYTES)
        except BootstrapError:
            pass
        else:
            raise AssertionError("direct metrics accepted a changed RSS limit")

    with tempfile.TemporaryDirectory(prefix="wric-phase0-namespace-test-") as temp:
        namespace_root = Path(temp)
        namespace_baseline = namespace_root / BASELINE_RELATIVE
        namespace_paths = metadata_paths(namespace_root, namespace_baseline)
        namespace_paths["captures"].mkdir(parents=True)
        namespace_metadata = {
            "captures": [serialize_capture(captures[0])],
        }
        verify_bootstrap_namespace(namespace_metadata, namespace_paths)
        unexpected = namespace_paths["bootstrap_dir"] / "uncontracted.bin"
        write_text_exclusive(unexpected, "must be rejected\n")
        try:
            verify_bootstrap_namespace(namespace_metadata, namespace_paths)
        except BootstrapError:
            pass
        else:
            raise AssertionError("unexpected bootstrap-namespace file was accepted")

    with tempfile.TemporaryDirectory(prefix="wric-phase0-ledger-test-") as temp:
        root = Path(temp)
        artifact = root / "artifact.txt"
        write_text_exclusive(artifact, "immutable evidence\n")
        ledger = root / "artifacts.tsv"
        ledger_seal = root / "artifacts.tsv.sha256"
        write_artifact_ledger(root, ledger, ledger_seal, {artifact})
        audit_artifacts(
            root,
            ledger,
            ledger_seal,
            expected_paths={artifact},
            required_paths=(artifact,),
        )
        omitted = root / "omitted.txt"
        write_text_exclusive(omitted, "required but unlisted\n")
        try:
            audit_artifacts(
                root,
                ledger,
                ledger_seal,
                required_paths=(omitted,),
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError("artifact ledger omitted a required file")
        extra_ledger = root / "extra-cell.tsv"
        extra_seal = root / "extra-cell.tsv.sha256"
        write_text_exclusive(
            extra_ledger,
            "sha256\turi\n"
            f"{sha256_file(artifact)}\trepo://artifact.txt\tUNVALIDATED_EXTRA\n",
            0o444,
        )
        write_detached_seal(extra_ledger)
        try:
            audit_artifacts(
                root,
                extra_ledger,
                extra_seal,
                expected_paths={artifact},
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError("artifact ledger with an extra TSV cell was accepted")
        wrong_seal_ledger = root / "wrong-seal.tsv"
        try:
            write_artifact_ledger(
                root,
                wrong_seal_ledger,
                root / "not-the-derived-seal.sha256",
                {artifact},
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError("artifact ledger accepted a mismatched seal path")
        assert not path_occupied(wrong_seal_ledger)
        bad_seal_ledger = root / "bad-seal.tsv"
        bad_seal = root / "bad-seal.tsv.sha256"
        write_artifact_ledger(
            root, bad_seal_ledger, bad_seal, {artifact}
        )
        os.chmod(bad_seal, 0o644)
        bad_seal.write_text("0" * 64 + "  bad-seal.tsv\n", encoding="ascii")
        try:
            audit_artifacts(root, bad_seal_ledger, bad_seal)
        except BootstrapError:
            pass
        else:
            raise AssertionError("mutated detached artifact seal was accepted")
        dangling = root / "dangling"
        dangling.symlink_to(root / "missing-target")
        assert path_occupied(dangling) and not dangling.exists()
        assert classify_real_preflight_observation(
            **(classify_defaults | {"output_exists": path_occupied(dangling)})
        ) == "blocked"
        try:
            require_absent_output(dangling, "synthetic expected-infeasible run")
        except BootstrapError:
            pass
        else:
            raise AssertionError("dangling expected-infeasible output was accepted")
        try:
            require_exact_regular_file(dangling, "synthetic alias")
        except BootstrapError:
            pass
        else:
            raise AssertionError("dangling symlink was accepted as a regular file")
        os.chmod(artifact, 0o644)
        artifact.write_text("mutated evidence\n", encoding="utf-8")
        try:
            audit_artifacts(root, ledger, ledger_seal)
        except BootstrapError:
            pass
        else:
            raise AssertionError("mutated artifact closure was accepted")
        circular = root / "circular.tsv"
        try:
            write_artifact_ledger(
                root,
                circular,
                root / "circular.tsv.sha256",
                {circular},
            )
        except BootstrapError:
            pass
        else:
            raise AssertionError("self-referential artifact ledger was accepted")
    print(
        f"PASS: {len(captures)} captures/{len(rows)} unique rows; "
        "physical+SMT+auto/default/unpinned resolver, exact timeout allowlist, "
        "and wall-ratio pairing contracts are non-ambiguous"
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)

    def affinities(subparser: argparse.ArgumentParser) -> None:
        subparser.add_argument("--physical-affinity", default=PHYSICAL_AFFINITY)
        subparser.add_argument("--smt-affinity", default=SMT_AFFINITY)
        subparser.add_argument(
            "--unpinned-affinity",
            help="freeze this observed unpinned list (default: taskset -pc self)",
        )

    prepare_parser = subparsers.add_parser("prepare", help="write plans/commands only")
    prepare_parser.add_argument("--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e")
    prepare_parser.add_argument(
        "--expected-oracle-sha256",
        required=True,
        help="independently verified corrected Phase-0 dagutil SHA-256",
    )
    prepare_parser.add_argument(
        "--wrapper-calibration",
        required=True,
        metavar="PATH/wrapper-calibration.json",
        help=(
            "canonical read-only passing process-wrapper calibration bound "
            "to the exact build/bin runner"
        ),
    )
    affinities(prepare_parser)
    prepare_parser.set_defaults(function=prepare)

    capture_parser = subparsers.add_parser("capture", help="run one exclusive unsealed capture")
    capture_parser.add_argument("--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e")
    capture_parser.add_argument("--capture-id", required=True)
    capture_parser.add_argument("--confirm-long-medium", action="store_true")
    capture_parser.add_argument("--confirm-real-preflight", action="store_true")
    capture_parser.set_defaults(function=capture_one)

    approve_parser = subparsers.add_parser(
        "approve-timeouts",
        help="post-capture approval of every and only observed eligible timeout",
    )
    approve_parser.add_argument(
        "--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e"
    )
    approve_parser.add_argument("--capture-id", required=True)
    approve_parser.add_argument(
        "--allow-expected-timeout",
        action="append",
        default=[],
        metavar="ROW_ID",
        help=(
            "repeat for every and only frozen timeout-eligible row in this "
            "capture; this records intent and cannot waive other failures"
        ),
    )
    approve_parser.set_defaults(function=approve_timeouts)

    real_approve_parser = subparsers.add_parser(
        "approve-real-outcome",
        help="freeze the exact observed real-20D pre-existing refusal",
    )
    real_approve_parser.add_argument(
        "--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e"
    )
    real_approve_parser.add_argument(
        "--capture-id", default="real20d-exact1-preflight-physical"
    )
    real_approve_parser.add_argument(
        "--expected-infeasible", action="append", default=[], metavar="ROW_ID"
    )
    real_approve_parser.add_argument("--expected-reason-sha256", required=True)
    real_approve_parser.add_argument("--confirm-real-outcome", action="store_true")
    real_approve_parser.set_defaults(function=approve_real_outcome)

    finalize_parser = subparsers.add_parser("finalize", help="derive, seal, and audit workloads.pending.tsv")
    finalize_parser.add_argument("--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e")
    finalize_parser.set_defaults(function=finalize)

    audit_parser = subparsers.add_parser("audit", help="rerun strict read-only pending-manifest audit")
    audit_parser.add_argument("--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e")
    audit_parser.set_defaults(function=audit)

    seal_parser = subparsers.add_parser("seal", help="promote audited pending bytes without overwrite")
    seal_parser.add_argument("--baseline-dir", default="build/wric-chart-parallelization/baseline-408434e")
    seal_parser.add_argument("--confirm-seal-base", action="store_true")
    seal_parser.set_defaults(function=seal)

    matrix_parser = subparsers.add_parser("matrix", help="print the matrix without writing/capturing")
    affinities(matrix_parser)
    matrix_parser.set_defaults(function=matrix)

    self_parser = subparsers.add_parser("self-test", help="run pure matrix/digest invariants")
    self_parser.set_defaults(function=self_test)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        args.function(args)
        return 0
    except BootstrapError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
