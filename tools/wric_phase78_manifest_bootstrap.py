#!/usr/bin/env python3
"""Produce and audit the immutable Phase-7/8 workload supplements.

The Phase-0 base manifest is the root of trust.  ``build`` executes its frozen
``dagutil`` under the process-metrics resource wrapper, records a restartable
capture, derives the closed workload matrix, and publishes the supplement by
using the Phase-9 builder's crash-durable no-replace transaction.  ``audit``
re-derives every row from the supplement-owned capture and asks the production
benchmark harness to parse the complete base/supplement chain.

The Phase-0 oracle predates ``--wric-lazy-chart auto``.  Phase 7 therefore
captures forced ``off`` and ``on`` for every fixture/worker.  An ``auto`` row
is derived only after those forced executions have byte-identical canonical
search streams and equal output semantics.  Its evidence comes from the
profile's explicitly selected safe branch, while its argv/trial digest still
contains the literal ``auto`` policy.

This builder seals frozen-oracle workload rows and the evidence needed to
reproduce them.  It does not seal same-revision timing medians, speedup verdicts,
or Phase-7/8 acceptance results; those remain the later benchmark/acceptance
workflow's responsibility.
"""

from __future__ import annotations

import argparse
import dataclasses
from decimal import Decimal, InvalidOperation
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
from typing import Callable, Iterable, Mapping, NoReturn, Sequence

sys.dont_write_bytecode = True

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import wric_phase9_manifest_bootstrap as core  # noqa: E402


WORKERS = (1, 2, 4, 8)
ITERATIONS = 1
SEED = 1
WORKLOAD_TIMEOUT_SECONDS = 600
CAPTURE_TIMEOUT_SECONDS = WORKLOAD_TIMEOUT_SECONDS
RSS_LIMIT_BYTES = core.RSS_LIMIT_BYTES
MEMORY_BUDGET_BYTES = core.MEMORY_BUDGET_BYTES
MIN_ACTIVE_PATTERNS = 64
HARNESS_SENTINEL_GROUP = "phase78-bootstrap-validation-sentinel"
CAPTURE_SCHEMA = "wric_phase78_frozen_capture"
CAPTURE_SCHEMA_VERSION = 1
CAPTURE_STATUS_SCHEMA = "wric_phase78_capture_status"
CAPTURE_STATUS_VERSION = 2

INPUT_FILES = (
    "report.txt",
    "stderr.txt",
    "process-metrics.txt",
    "canonical.json",
)
ROW_FILES = (
    "report.txt",
    "stderr.txt",
    "process-metrics.txt",
    "canonical.ndjson",
    "canonical.json",
    "output.pb.gz",
    "output-report.txt",
    "output-stderr.txt",
    "output-process-metrics.txt",
    "output-canonical.json",
)
STATUS_FILES = ("status.json", "status.json.sha256")


class Phase78Error(RuntimeError):
    """Stable command-line failure for a rejected supplement or capture."""


def fail(message: str) -> NoReturn:
    raise Phase78Error(message)


def phase0_module():
    """Load the frozen canonical-stream validator without duplicating its schema."""

    import wric_phase0_manifest_bootstrap  # noqa: PLC0415

    return wric_phase0_manifest_bootstrap


@dataclasses.dataclass(frozen=True)
class FixtureSpec:
    key: str
    fixture_id: str
    workload_name: str
    relative_path: str
    sha256: str
    auto_source_policy: str = "off"
    ref_relative_path: str | None = None
    ref_sha256: str | None = None


@dataclasses.dataclass(frozen=True)
class Profile:
    name: str
    manifest_id: str
    output_name: str
    method: str
    acceptance: str
    objective: str
    candidate_source: str
    topology_selector: str
    max_candidates: int
    top_k_exact: int
    policies: tuple[str, ...]
    fixtures: tuple[FixtureSpec, ...]


PROFILES: Mapping[str, Profile] = {
    "phase7": Profile(
        name="phase7",
        manifest_id="phase7-lazy",
        output_name="phase7-lazy.tsv",
        method="chart_spr_grammar_lower_bound_heuristic",
        acceptance="lower_bound_heuristic",
        objective="composite_lower_bound_heuristic",
        candidate_source="grammar",
        topology_selector="none",
        max_candidates=64,
        top_k_exact=0,
        policies=("off", "on", "auto"),
        fixtures=(
            FixtureSpec(
                "high-compression",
                "wric-lazy-high-compression",
                "phase7-lazy-high-compression",
                "test/wric_lazy_high_compression.pb.gz",
                "e103be6cd1df36e5a002ae9dd9ffb53110b35c840eed1d8fa867475ad3109874",
                "on",
                "test/wric_lazy_high_compression.ref",
                "86f9d532555a1cf709ea3a9a3efe7c72aa9cd6c6a01bd7aa2614636a846a9128",
            ),
            FixtureSpec(
                "dense-favoring",
                "wric-lazy-dense-favoring",
                "phase7-lazy-dense-favoring",
                "test/wric_lazy_dense_favoring.pb.gz",
                "e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6",
                "off",
                "test/wric_lazy_dense_favoring.ref",
                "b16c732ac5692f8644afc19f0c454422c381e9d88eff5dacb40ac96a603f8404",
            ),
        ),
    ),
    "phase8": Profile(
        name="phase8",
        manifest_id="phase8-generation",
        output_name="phase8-generation.tsv",
        method="chart_spr_sampled_tree_fixed_topology",
        acceptance="fixed_topology_exact",
        objective="fixed_topology_exact",
        candidate_source="sampled_tree",
        topology_selector="first_reachable_overlay_topology",
        max_candidates=256,
        top_k_exact=1,
        policies=("off",),
        fixtures=(
            FixtureSpec(
                "tree0",
                "test-5-trees-tree-0",
                "phase8-generation-tree0",
                "data/test_5_trees/tree_0.pb.gz",
                "e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6",
            ),
        ),
    ),
}


@dataclasses.dataclass(frozen=True)
class InputEvidence:
    semantic_sha256: str
    parsimony_min: int
    canonical_sha256: str


@dataclasses.dataclass(frozen=True)
class CaptureIdentity:
    """Externally rooted identity inherited by every capture status record."""

    capture_contract_sha256: str
    base_manifest_sha256: str
    frozen_oracle_sha256: str
    process_metrics_sha256: str


@dataclasses.dataclass(frozen=True)
class RowEvidence:
    fixture: FixtureSpec
    policy: str
    workers: int
    directory: Path
    search_semantic_sha256: str
    output_semantic_sha256: str
    sidecar_sha256: str
    canonical_result_sha256: str
    output_canonical_sha256: str
    expected: Mapping[str, str]
    total_ms: Decimal
    initial_chart_ms: Decimal
    candidate_generation_ms: Decimal
    lazy_internal_ratio: Decimal | None
    lazy_merge_ratio: Decimal | None


@dataclasses.dataclass(frozen=True)
class AuditedBundle:
    base: core.Manifest
    supplement: core.Manifest
    process_metrics_sha256: str
    profile: Profile


@dataclasses.dataclass(frozen=True)
class FixtureSource:
    """Externally revision-bound source for build-only fixture ingestion."""

    root: Path
    revision: str


def profile_named(name: str) -> Profile:
    try:
        return PROFILES[name]
    except KeyError:
        fail(f"unsupported profile {name!r}")


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def decimal(value: str, label: str) -> Decimal:
    if re.fullmatch(r"(?:0|[1-9][0-9]*)(?:[.][0-9]+)?", value) is None:
        fail(f"{label} is not a canonical nonnegative decimal: {value!r}")
    try:
        result = Decimal(value)
    except InvalidOperation as error:
        fail(f"{label} is not a decimal: {value!r}: {error}")
    if not result.is_finite() or result < 0:
        fail(f"{label} is not a finite nonnegative decimal: {value!r}")
    return result


def capture_row_id(profile: Profile, fixture: FixtureSpec, policy: str, workers: int) -> str:
    return f"{profile.manifest_id}-{fixture.key}-{policy}-w{workers}"


def manifest_row_id(profile: Profile, fixture: FixtureSpec, policy: str, workers: int) -> str:
    return capture_row_id(profile, fixture, policy, workers)


def forced_policies(profile: Profile) -> tuple[str, ...]:
    return ("off", "on") if profile.name == "phase7" else ("off",)


def fixture_path(root: Path, fixture: FixtureSpec) -> Path:
    path = core.resolve_lexical_regular(root, Path(fixture.relative_path), fixture.fixture_id)
    if path.stat().st_nlink != 1:
        fail(f"fixture must not be externally hard-linked: {path}")
    if core.sha256_file(path) != fixture.sha256:
        fail(f"tracked fixture hash changed: {fixture.relative_path}")
    return path


def fixture_ref_path(root: Path, fixture: FixtureSpec) -> Path | None:
    if (fixture.ref_relative_path is None) != (fixture.ref_sha256 is None):
        fail(f"fixture {fixture.key} has a partial reference path/hash contract")
    if fixture.ref_relative_path is None:
        return None
    assert fixture.ref_sha256 is not None
    path = core.resolve_lexical_regular(
        root, Path(fixture.ref_relative_path), f"{fixture.fixture_id} reference"
    )
    if path.stat().st_nlink != 1:
        fail(f"fixture reference must not be externally hard-linked: {path}")
    if core.sha256_file(path) != fixture.ref_sha256:
        fail(f"tracked fixture reference hash changed: {fixture.ref_relative_path}")
    return path


def fixture_relative_paths(profile: Profile) -> tuple[str, ...]:
    """Return the closed, hard-coded source path set for one profile."""

    paths = [fixture.relative_path for fixture in profile.fixtures]
    paths.extend(
        fixture.ref_relative_path
        for fixture in profile.fixtures
        if fixture.ref_relative_path is not None
    )
    if len(paths) != len(set(paths)):
        fail(f"{profile.name} fixture source paths are not unique")
    for text in paths:
        relative = Path(text)
        if (
            not text
            or relative.is_absolute()
            or "." in relative.parts
            or ".." in relative.parts
            or "\\" in text
            or "//" in text
        ):
            fail(f"fixture source path is not normalized and confined: {text!r}")
    return tuple(paths)


def validate_fixture_source_tree(
    profile: Profile, root: Path, expected_revision: str
) -> FixtureSource:
    """Validate exact Git provenance and every relevant tracked fixture byte."""

    if core.REVISION.fullmatch(expected_revision) is None:
        fail("expected fixture-source revision is not a full Git object ID")
    try:
        source_root = core.base_repo_root(root)
        core.require_base_revision(source_root, expected_revision)
    except core.BootstrapError as error:
        fail(f"fixture source provenance is invalid: {error}")
    relatives = fixture_relative_paths(profile)
    try:
        tracked = core.git_output(
            source_root,
            ("--literal-pathspecs", "ls-files", "--stage", "--", *relatives),
            "fixture source tracked-file audit",
        )
        dirty = core.git_output(
            source_root,
            (
                "--literal-pathspecs",
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
                "--",
                *relatives,
            ),
            "fixture source relevant-status audit",
        )
    except core.BootstrapError as error:
        fail(str(error))
    observed: list[str] = []
    for line in tracked.splitlines():
        if "\t" not in line:
            fail("fixture source tracked-file record is malformed")
        metadata, relative = line.split("\t", 1)
        fields = metadata.split()
        if len(fields) != 3 or fields[0] != "100644" or fields[2] != "0":
            fail(f"fixture source tracked-file mode/stage is invalid: {relative}")
        observed.append(relative)
    if sorted(observed) != sorted(relatives):
        fail(
            "fixture source does not track the exact required path set: "
            f"observed={sorted(observed)}, expected={sorted(relatives)}"
        )
    if dirty:
        fail(
            "fixture source has dirty relevant files: "
            + dirty.splitlines()[0]
        )
    for fixture in profile.fixtures:
        fixture_path(source_root, fixture)
        fixture_ref_path(source_root, fixture)
    return FixtureSource(source_root, expected_revision)


def fixture_source_for_build(
    args: argparse.Namespace,
    profile: Profile,
    base_root: Path,
    base: core.Manifest,
) -> FixtureSource:
    """Select explicit split-root provenance or safe sealed same-root defaults."""

    source_argument, revision_argument = fixture_source_options(args)
    if source_argument is None:
        return validate_fixture_source_tree(
            profile, base_root, base.preamble["repo_revision"]
        )
    return validate_fixture_source_tree(
        profile, Path(source_argument), str(revision_argument)
    )


def fixture_source_options(
    args: argparse.Namespace,
) -> tuple[Path | None, str | None]:
    """Reject partial or self-derived explicit fixture provenance."""

    source_argument = getattr(args, "fixture_source_root", None)
    revision_argument = getattr(args, "expected_fixture_source_revision", None)
    if (source_argument is None) != (revision_argument is None):
        fail(
            "--fixture-source-root and --expected-fixture-source-revision "
            "must be supplied together"
        )
    if revision_argument is not None and core.REVISION.fullmatch(
        str(revision_argument)
    ) is None:
        fail("--expected-fixture-source-revision is not a full Git object ID")
    return source_argument, revision_argument


def contract_row(
    profile: Profile,
    fixture: FixtureSpec,
    policy: str,
    workers: int,
    affinity: str,
    primary_uri: str,
) -> dict[str, str]:
    if policy not in profile.policies:
        fail(f"{profile.name} does not permit lazy policy {policy!r}")
    if workers not in WORKERS:
        fail(f"unsupported worker count {workers}")
    row = {field: "-" for field in core.MANIFEST_HEADER}
    row.update(
        {
            "row_id": manifest_row_id(profile, fixture, policy, workers),
            "run_group": profile.manifest_id,
            "workload_name": fixture.workload_name,
            "fixture_id": fixture.fixture_id,
            "method": profile.method,
            "input_kind": "dag_pb",
            "primary_uri": primary_uri,
            "primary_sha256": fixture.sha256,
            "binary_role": "working_chart",
            "worker_option": "chart_spr_workers",
            "requested_workers": str(workers),
            "expected_resolved_workers": str(workers),
            "expected_worker_policy": "explicit",
            "affinity_cpus": affinity,
            "timeout_seconds": str(WORKLOAD_TIMEOUT_SECONDS),
            "rss_limit_bytes": str(RSS_LIMIT_BYTES),
            "expected_outcome": "ok",
            "expected_timeout_trials": "0",
            "iterations": str(ITERATIONS),
            "seed": str(SEED),
            "chart_max_candidates": str(profile.max_candidates),
            "chart_top_k_exact": str(profile.top_k_exact),
            "candidate_cap_semantics": "post-dedup",
            "acceptance": profile.acceptance,
            "objective": profile.objective,
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": profile.candidate_source,
            "topology_selector": profile.topology_selector,
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
            "lazy_policy": policy,
            "max_cached_patterns": "0",
            "pattern_batch_size": "0",
            "candidate_batch_size": "0",
            "memory_budget_bytes": str(MEMORY_BUDGET_BYTES),
            "commit_mode": "overlay_delta",
            "verification_mode": "transient",
            "local_accept_updates": "false",
            "dominance_mode": "off",
            "bound_pruning": "true",
            "require_exact_keep_mask": "true",
            "max_frontier_entries": "0",
            "score_ua_edge": "false",
            "validate": "true",
            "force_no_vcf": "true",
        }
    )
    return row


def canonical_argv(row: Mapping[str, str]) -> list[str]:
    """Mirror the manifest-group argv order in the production harness."""

    result = core.canonical_argv(row)
    if row["topology_selector"] != "none":
        position = result.index("--chart-spr-acceptance") + 2
        result[position:position] = [
            "--chart-spr-topology-selector",
            row["topology_selector"],
        ]
    return result


def actual_chart_command(
    oracle: Path, fixture: Path, row: Mapping[str, str], directory: Path
) -> list[str]:
    argv = canonical_argv(row)
    argv[0] = os.fspath(oracle)
    argv[2] = os.fspath(fixture)
    argv[-1] = os.fspath(directory / "output.pb.gz")
    output_flag = len(argv) - 2
    argv[output_flag:output_flag] = [
        "--chart-spr-canonical-result",
        os.fspath(directory / "canonical.json"),
        "--chart-spr-canonical-sidecar",
        os.fspath(directory / "canonical.ndjson"),
    ]
    return ["taskset", "-c", row["affinity_cpus"], *argv]


def sidecar_records(path: Path) -> list[dict[str, object]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        fail(f"canonical sidecar is not UTF-8: {path}: {error}")
    if not lines:
        fail(f"canonical sidecar is empty: {path}")
    records: list[dict[str, object]] = []
    for number, line in enumerate(lines, 1):
        def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
            value: dict[str, object] = {}
            for key, item in items:
                if key in value:
                    fail(f"canonical sidecar line {number} duplicates {key!r}: {path}")
                value[key] = item
            return value

        try:
            record = json.loads(
                line,
                object_pairs_hook=pairs,
                parse_constant=core.reject_nonfinite_json_constant,
            )
        except json.JSONDecodeError as error:
            fail(f"canonical sidecar line {number} is malformed: {path}: {error}")
        if not isinstance(record, dict):
            fail(f"canonical sidecar line {number} is not an object: {path}")
        records.append(record)
    return records


def validate_canonical_companion(directory: Path, label: str) -> None:
    """Validate exact full-stream closure and every compact component digest."""

    phase0 = phase0_module()
    try:
        phase0.validate_canonical_sidecar_source(
            directory,
            ("canonical.ndjson", "canonical.json"),
            label,
        )
    except phase0.BootstrapError as error:
        fail(str(error))


def validate_profile_canonical_stream(
    profile: Profile,
    records: Sequence[Mapping[str, object]],
    expected: Mapping[str, str],
    label: str,
) -> None:
    """Reconcile stream indexes/counters beyond the generic P0 schema closure."""

    by_kind: dict[str, list[Mapping[str, object]]] = {}
    for record in records:
        kind = record.get("record")
        if isinstance(kind, str):
            by_kind.setdefault(kind, []).append(record)

    candidates = by_kind.get("candidate", [])
    wanted_candidate_keys = [(0, index) for index in range(profile.max_candidates)]
    observed_candidate_keys = [
        (record.get("iteration"), record.get("stream_index"))
        for record in candidates
    ]
    if observed_candidate_keys != wanted_candidate_keys:
        fail(f"{label}: candidate stream indexes are not the exact canonical prefix")
    signatures = [record.get("signature") for record in candidates]
    if len(set(signatures)) != len(signatures):
        fail(f"{label}: candidate signatures are not unique after deduplication")

    lower_bounds = by_kind.get("candidate_lower_bound", [])
    lower_bound_keys = [
        (record.get("iteration"), record.get("stream_index"))
        for record in lower_bounds
    ]
    if lower_bound_keys != wanted_candidate_keys:
        fail(f"{label}: lower-bound stream does not cover every candidate exactly once")

    ranks = by_kind.get("candidate_rank", [])
    rank_values = [record.get("rank") for record in ranks]
    rank_streams = [record.get("stream_index") for record in ranks]
    if (
        len(ranks) != 1
        or rank_values != list(range(len(ranks)))
        or len(set(rank_streams)) != len(rank_streams)
        or any(record.get("iteration") != 0 for record in ranks)
        or any(
            not isinstance(index, int)
            or isinstance(index, bool)
            or index not in range(profile.max_candidates)
            for index in rank_streams
        )
    ):
        fail(f"{label}: candidate ranks are not a closed stable stream-index ordering")

    begins = by_kind.get("iteration_begin", [])
    outcomes = by_kind.get("iteration_outcome", [])
    if [record.get("iteration") for record in begins] != [0] or [
        record.get("iteration") for record in outcomes
    ] != [0]:
        fail(f"{label}: canonical iteration indexes are not exactly [0]")
    outcome = outcomes[0]
    outcome_counts = {
        "candidates_generated": expected["expected_candidates_generated"],
        "candidates_scored": expected["expected_candidates_scored"],
        "candidates_exact_verified": expected["expected_exact_verifications"],
    }
    for field, wanted in outcome_counts.items():
        if outcome.get(field) != int(wanted):
            fail(f"{label}: iteration outcome {field} differs from report evidence")
    if outcome.get("generation_stop_reason") != expected["expected_stop_reason"]:
        fail(f"{label}: iteration outcome stop reason differs from report evidence")
    if (
        outcome.get("accepted_move_committed") is not False
        or outcome.get("selected_signature") != ""
        or outcome.get("state_score_after") != int(expected["expected_final_score"])
    ):
        fail(f"{label}: no-accept iteration outcome is not canonical")

    exact_records = by_kind.get("candidate_exact", [])
    verification_ranks = by_kind.get("candidate_exact_verification_rank", [])
    exact_evidence = by_kind.get("exact_evidence", [])
    before = by_kind.get("fixed_topology_before_production", [])
    after = by_kind.get("fixed_topology_after_production", [])
    exact_count = int(expected["expected_exact_verifications"])
    if len(exact_records) != exact_count or len(verification_ranks) != exact_count:
        fail(f"{label}: exact candidate/rank cardinality differs from report evidence")
    if profile.name == "phase7":
        forbidden = (
            "exact_evidence",
            "exact_keep_production",
            "exact_frontier_size",
            "exact_root_provenance_class",
            "fixed_topology_before_production",
            "fixed_topology_after_production",
        )
        if exact_count != 0 or any(by_kind.get(kind) for kind in forbidden):
            fail(f"{label}: lower-bound-only stream contains exact evidence")
    else:
        rank_indexes = [
            record.get("exact_verification_index") for record in verification_ranks
        ]
        exact_streams = [record.get("stream_index") for record in exact_records]
        ranked_streams = [record.get("stream_index") for record in verification_ranks]
        if (
            rank_indexes != list(range(exact_count))
            or exact_streams != ranked_streams
            or exact_streams != rank_streams
            or len(set(exact_streams)) != len(exact_streams)
            or any(record.get("iteration") != 0 for record in exact_records)
            or any(record.get("iteration") != 0 for record in verification_ranks)
        ):
            fail(f"{label}: exact verification rank/stream indexes disagree")
        exact_stream_set = set(exact_streams)
        if (
            {record.get("stream_index") for record in exact_evidence}
            != exact_stream_set
            or any(
                record.get("scope") != "candidate"
                or record.get("iteration") != 0
                or record.get("evidence_kind") != "fixed_topology_certificate"
                or record.get("keep_mask_kind")
                != "not_applicable_fixed_topology"
                or record.get("keep_production_exact") is not False
                for record in exact_evidence
            )
        ):
            fail(f"{label}: fixed-topology exact evidence is incomplete")
        for certificate_kind, certificate_records in (
            ("before", before),
            ("after", after),
        ):
            if (
                {record.get("stream_index") for record in certificate_records}
                != exact_stream_set
                or any(
                    record.get("scope") != "candidate"
                    or record.get("iteration") != 0
                    or not isinstance(record.get("production_key"), str)
                    or not record.get("production_key")
                    for record in certificate_records
                )
            ):
                fail(f"{label}: fixed-topology {certificate_kind} certificate is incomplete")

    final_states = by_kind.get("final_state", [])
    if len(final_states) != 1 or (
        final_states[0].get("final_score") != int(expected["expected_final_score"])
        or final_states[0].get("accepted_moves")
        != int(expected["expected_accepted_moves"])
    ):
        fail(f"{label}: final-state counters differ from report evidence")
    if not by_kind.get("chain_base_production"):
        fail(f"{label}: canonical chain lacks base productions")
    if not by_kind.get("final_clade") or not by_kind.get("final_production"):
        fail(f"{label}: canonical final topology is incomplete")


def require_report(report, key: str, wanted: str, label: str) -> None:
    acceptance = core.acceptance_module()
    try:
        actual = acceptance.require_top(report, key, label)
    except acceptance.AcceptanceError as error:
        fail(str(error))
    if actual != wanted:
        fail(f"{label}: {key}={actual!r}, expected {wanted!r}")


def report_value(report, key: str, label: str) -> str:
    acceptance = core.acceptance_module()
    try:
        return acceptance.require_top(report, key, label)
    except acceptance.AcceptanceError as error:
        fail(str(error))


def validate_capture_receipt(path: Path, label: str) -> Mapping[str, str]:
    """Validate success plus the Phase-7/8 600-second wall-time ceiling."""

    try:
        values = core.validate_process_metrics_receipt(path, label)
    except core.BootstrapError as error:
        fail(str(error))
    wall = decimal(values["wall_seconds"], f"{label} wall_seconds")
    if wall > Decimal(WORKLOAD_TIMEOUT_SECONDS):
        fail(
            f"{label} wall_seconds={wall} exceeds the "
            f"{WORKLOAD_TIMEOUT_SECONDS}-second workload contract"
        )
    return values


def run_enforced_capture_command(
    process_metrics: Path,
    command: Sequence[str],
    stdout: Path,
    stderr: Path,
    metrics: Path,
    environment: Mapping[str, str],
    label: str,
) -> None:
    """Run one frozen command under the actual 600-second workload limit."""

    runner = [
        os.fspath(process_metrics),
        "--timeout-seconds",
        str(WORKLOAD_TIMEOUT_SECONDS),
        "--rss-limit-bytes",
        str(RSS_LIMIT_BYTES),
        "--stdout",
        os.fspath(stdout),
        "--stderr",
        os.fspath(stderr),
        "--metrics",
        os.fspath(metrics),
        "--",
        *command,
    ]
    result = subprocess.run(
        runner,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=dict(environment),
        timeout=WORKLOAD_TIMEOUT_SECONDS + 60,
    )
    if result.stdout or result.stderr:
        fail(f"process-metrics emitted unexpected diagnostics for {label}")
    validate_capture_receipt(metrics, f"{label} process-metrics receipt")
    if result.returncode != 0:
        fail(f"{label} failed under process-metrics: {result.returncode}")


def parse_row_evidence(
    profile: Profile,
    fixture: FixtureSpec,
    policy: str,
    workers: int,
    directory: Path,
    input_evidence: InputEvidence,
    affinity: str,
    capture_identity: CaptureIdentity,
    *,
    validate_status: bool = True,
) -> RowEvidence:
    directory = core.require_lexical_directory(directory, "frozen capture row")
    expected_names = set(ROW_FILES)
    if validate_status:
        expected_names.update(STATUS_FILES)
    observed_names = {item.name for item in directory.iterdir()}
    if observed_names != expected_names:
        fail(
            f"capture row closure changed: {directory}: "
            f"missing={sorted(expected_names - observed_names)}, "
            f"extra={sorted(observed_names - expected_names)}"
        )
    for name in expected_names:
        path = directory / name
        core.require_regular(path, f"capture row {name}")
        if path.stat().st_nlink != 1:
            fail(f"capture row member is externally hard-linked: {path}")
    validate_capture_receipt(
        directory / "process-metrics.txt", "search capture receipt"
    )
    validate_capture_receipt(
        directory / "output-process-metrics.txt", "output capture receipt"
    )

    label = capture_row_id(profile, fixture, policy, workers)
    validate_canonical_companion(directory, label)
    acceptance = core.acceptance_module()
    try:
        report = acceptance.parse_report(directory / "report.txt")
        search = acceptance.validate_search_digest(directory / "canonical.json")
        output = acceptance.validate_dag_digest(directory / "output-canonical.json")
    except acceptance.AcceptanceError as error:
        fail(str(error))
    bindings = {
        "acceptance": profile.acceptance,
        "objective": profile.objective,
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": profile.candidate_source,
        "candidate_cap_semantics": "post-dedup",
        "topology_selector": profile.topology_selector,
        "requested_max_iterations": "1",
        "configured_max_candidates": str(profile.max_candidates),
        "top_k_exact_verify": str(profile.top_k_exact),
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
        "lazy_policy": policy,
        "max_cached_patterns": "0",
        "configured_pattern_batch_size": "0",
        "configured_candidate_batch_size": "0",
        "memory_budget_bytes": str(MEMORY_BUDGET_BYTES),
        "commit_mode": "overlay_delta",
        "verification_mode": "transient",
        "local_accept_updates": "false",
        "dominance_mode": "off",
        "bound_pruning": "true",
        "require_exact_keep_mask": "true",
        "max_frontier_entries": "0",
        "score_ua_edge": "false",
        "validate": "true",
        "force_no_vcf": "true",
        "chart_workers_requested": str(workers),
        "chart_workers_resolved": str(workers),
        "chart_worker_policy": "explicit",
        "local_score_workers": str(workers),
    }
    for key, wanted in bindings.items():
        require_report(report, key, wanted, label)

    records = sidecar_records(directory / "canonical.ndjson")
    contracts = [record for record in records if record.get("record") == "contract"]
    initial_states = [record for record in records if record.get("record") == "initial_state"]
    final_states = [record for record in records if record.get("record") == "final_state"]
    if len(contracts) != 1 or len(initial_states) != 1 or len(final_states) != 1:
        fail(f"{label}: sidecar lacks one contract/initial/final state")
    contract = contracts[0]
    topology = (
        "none"
        if profile.topology_selector == "none"
        else f"deterministic_selector:{profile.topology_selector}"
    )
    contract_bindings: Mapping[str, object] = {
        "acceptance": profile.acceptance,
        "objective": profile.objective,
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": profile.candidate_source,
        "topology_selection": topology,
        "commit_mode": "overlay_delta",
        "verification_mode": "transient",
        "candidate_cap_semantics": "post_dedup",
        "max_iterations": 1,
        "max_candidates": profile.max_candidates,
        "top_k_exact": profile.top_k_exact,
        "seed": 1,
        "polytomy_max_shapes": 1,
        "score_ua_edge": False,
        "use_bound_pruning": True,
        "require_exact_keep_mask": True,
        "randomize_order": False,
        "reservoir_sample": False,
        "include_immediate_reversals": False,
    }
    for key, wanted in contract_bindings.items():
        if contract.get(key) != wanted:
            fail(f"{label}: sidecar contract {key}={contract.get(key)!r}, expected {wanted!r}")

    expected_counts = (profile.max_candidates, profile.top_k_exact, 1)
    actual_counts = (
        search["candidate_count"],
        search["exact_candidate_count"],
        search["iteration_count"],
    )
    if actual_counts != expected_counts:
        fail(f"{label}: canonical search counts {actual_counts} != {expected_counts}")
    sidecar_sha = core.sha256_file(directory / "canonical.ndjson")
    if search["semantic_sha256"] != sidecar_sha:
        fail(f"{label}: compact search digest does not equal full-sidecar SHA-256")

    numeric = {
        "expected_active_patterns": "active_patterns",
        "expected_initial_clades": "initial_grammar_clades",
        "expected_initial_productions": "initial_grammar_productions",
        "expected_candidates_generated": "candidates_generated",
        "expected_candidates_scored": "candidates_scored",
        "expected_exact_verifications": "exact_verifications",
        "expected_iterations": "iterations",
        "expected_accepted_moves": "accepted_moves",
        "expected_final_score": "final_score",
    }
    expected: dict[str, str] = {}
    for manifest_key, report_key in numeric.items():
        value = report_value(report, report_key, label)
        if re.fullmatch(r"0|[1-9][0-9]*", value) is None:
            fail(f"{label}: report {report_key} is not an unsigned integer")
        expected[manifest_key] = value
    exact_counts = {
        "expected_candidates_generated": profile.max_candidates,
        "expected_candidates_scored": profile.max_candidates,
        "expected_exact_verifications": profile.top_k_exact,
        "expected_iterations": 1,
        "expected_accepted_moves": 0,
    }
    for key, wanted in exact_counts.items():
        if expected[key] != str(wanted):
            fail(f"{label}: {key}={expected[key]}, expected {wanted}")
    if int(expected["expected_active_patterns"]) < MIN_ACTIVE_PATTERNS:
        fail(f"{label}: fewer than {MIN_ACTIVE_PATTERNS} active patterns")
    initial_score = report_value(report, "initial_score", label)
    if re.fullmatch(r"0|[1-9][0-9]*", initial_score) is None:
        fail(f"{label}: report initial_score is not an unsigned integer")
    # The manifest's initial/final fields deliberately span two score domains:
    # initial_score is the external canonical parsimony reached before search,
    # while final_score is the product's configured objective (composite for
    # lower-bound mode).  validated_parsimony is independently re-derived from
    # the output DAG.  Equating these domains would reject valid Phase-7 rows.
    expected["expected_initial_score"] = str(input_evidence.parsimony_min)
    expected["expected_validated_parsimony"] = str(output["parsimony_min"])
    expected.update(
        {
            "expected_refinement_exactness": report_value(
                report, "refinement_exactness", label
            ),
            "expected_cache_strategy": report_value(report, "cache_strategy", label),
            "expected_effective_pattern_batch_size": report_value(
                report, "effective_pattern_batch_size", label
            ),
            "expected_final_compaction_exactness": report_value(
                report, "final_compaction_exactness_kind", label
            ),
            "expected_chain_exactness": report_value(
                report, "chain_per_accept_exactness_label", label
            ),
            "expected_stop_reason": core.report_stop_reason(directory / "report.txt"),
        }
    )
    if expected["expected_refinement_exactness"] not in (
        "EXACT",
        "BOUNDED_REFINED_GRAMMAR",
    ):
        fail(f"{label}: unsupported refinement exactness")
    wanted_cache = "lazy_multisite_chart" if policy == "on" else "all_active_patterns"
    if expected["expected_cache_strategy"] != wanted_cache:
        fail(f"{label}: forced policy did not select {wanted_cache}")
    if not expected["expected_effective_pattern_batch_size"].isdigit() or int(
        expected["expected_effective_pattern_batch_size"]
    ) <= 0:
        fail(f"{label}: effective pattern batch size is not positive")
    keep_kinds = {str(record["keep_mask_kind"]) for record in records if "keep_mask_kind" in record}
    if profile.name == "phase7":
        if keep_kinds:
            fail(f"{label}: lower-bound-only evidence unexpectedly has a keep mask")
        expected["expected_keep_mask_kind"] = "-"
    else:
        if keep_kinds != {"not_applicable_fixed_topology"}:
            fail(f"{label}: fixed-topology keep-mask evidence changed: {sorted(keep_kinds)}")
        expected["expected_keep_mask_kind"] = "not_applicable_fixed_topology"

    validate_profile_canonical_stream(profile, records, expected, label)

    if initial_states[0].get("initial_score") != int(initial_score):
        fail(f"{label}: sidecar/report initial score differs")
    if initial_states[0].get("active_patterns") != int(expected["expected_active_patterns"]):
        fail(f"{label}: sidecar/report active-pattern count differs")
    if final_states[0].get("final_score") != int(expected["expected_final_score"]):
        fail(f"{label}: sidecar/report final score differs")

    lazy_internal = None
    lazy_merge = None
    if policy == "on":
        lazy_internal = decimal(
            report_value(report, "lazy_internal_structural_class_ratio", label),
            f"{label} lazy internal structural ratio",
        )
        lazy_merge = decimal(
            report_value(report, "lazy_merge_ratio", label),
            f"{label} lazy merge ratio",
        )
    evidence = RowEvidence(
        fixture=fixture,
        policy=policy,
        workers=workers,
        directory=directory,
        search_semantic_sha256=str(search["semantic_sha256"]),
        output_semantic_sha256=str(output["semantic_sha256"]),
        sidecar_sha256=sidecar_sha,
        canonical_result_sha256=core.sha256_file(directory / "canonical.json"),
        output_canonical_sha256=core.sha256_file(directory / "output-canonical.json"),
        expected=expected,
        total_ms=decimal(report_value(report, "total_ms", label), f"{label} total_ms"),
        initial_chart_ms=decimal(
            report_value(report, "initial_chart_construction_ms", label),
            f"{label} initial_chart_construction_ms",
        ),
        candidate_generation_ms=decimal(
            report_value(report, "candidate_generation_ms", label),
            f"{label} candidate_generation_ms",
        ),
        lazy_internal_ratio=lazy_internal,
        lazy_merge_ratio=lazy_merge,
    )
    if validate_status:
        validate_row_status(profile, evidence, affinity, capture_identity)
    return evidence


def file_hashes(directory: Path, names: Sequence[str]) -> dict[str, str]:
    return {name: core.sha256_file(directory / name) for name in names}


def input_status_bytes(
    fixture: FixtureSpec,
    evidence: InputEvidence,
    directory: Path,
    capture_identity: CaptureIdentity,
) -> bytes:
    return json_bytes(
        {
            "schema": CAPTURE_STATUS_SCHEMA,
            "schema_version": CAPTURE_STATUS_VERSION,
            "kind": "input",
            "capture_identity": dataclasses.asdict(capture_identity),
            "fixture_key": fixture.key,
            "fixture_sha256": fixture.sha256,
            "semantic_sha256": evidence.semantic_sha256,
            "parsimony_min": evidence.parsimony_min,
            "canonical_sha256": evidence.canonical_sha256,
            "files": file_hashes(directory, INPUT_FILES),
        }
    )


def row_status_bytes(
    profile: Profile,
    evidence: RowEvidence,
    affinity: str,
    capture_identity: CaptureIdentity,
) -> bytes:
    row = contract_row(
        profile,
        evidence.fixture,
        evidence.policy,
        evidence.workers,
        affinity,
        "manifest://fixture.pb.gz",
    )
    return json_bytes(
        {
            "schema": CAPTURE_STATUS_SCHEMA,
            "schema_version": CAPTURE_STATUS_VERSION,
            "kind": "search",
            "capture_identity": dataclasses.asdict(capture_identity),
            "profile": profile.name,
            "row_id": capture_row_id(
                profile, evidence.fixture, evidence.policy, evidence.workers
            ),
            "fixture_key": evidence.fixture.key,
            "fixture_sha256": evidence.fixture.sha256,
            "lazy_policy": evidence.policy,
            "workers": evidence.workers,
            "canonical_argv_sha256": core.canonical_argv_digest(canonical_argv(row)),
            "search_semantic_sha256": evidence.search_semantic_sha256,
            "output_semantic_sha256": evidence.output_semantic_sha256,
            "expected": dict(evidence.expected),
            "files": file_hashes(evidence.directory, ROW_FILES),
        }
    )


def validate_status_file(path: Path, wanted: bytes, label: str) -> None:
    core.verify_detached_seal(path)
    if path.read_bytes() != wanted:
        fail(f"{label} differs from its re-derived exact bytes: {path}")


def validate_row_status(
    profile: Profile,
    evidence: RowEvidence,
    affinity: str,
    capture_identity: CaptureIdentity,
) -> None:
    validate_status_file(
        evidence.directory / "status.json",
        row_status_bytes(profile, evidence, affinity, capture_identity),
        "capture row status",
    )


def parse_input_evidence(
    fixture: FixtureSpec,
    directory: Path,
    capture_identity: CaptureIdentity,
    *,
    validate_status: bool = True,
) -> InputEvidence:
    directory = core.require_lexical_directory(directory, "frozen input capture")
    expected_names = set(INPUT_FILES)
    if validate_status:
        expected_names.update(STATUS_FILES)
    observed_names = {item.name for item in directory.iterdir()}
    if observed_names != expected_names:
        fail(
            f"input capture closure changed: {directory}: "
            f"missing={sorted(expected_names - observed_names)}, "
            f"extra={sorted(observed_names - expected_names)}"
        )
    for name in expected_names:
        path = directory / name
        core.require_regular(path, f"input capture {name}")
        if path.stat().st_nlink != 1:
            fail(f"input capture member is externally hard-linked: {path}")
    validate_capture_receipt(
        directory / "process-metrics.txt", "input capture receipt"
    )
    acceptance = core.acceptance_module()
    try:
        value = acceptance.validate_dag_digest(directory / "canonical.json")
    except acceptance.AcceptanceError as error:
        fail(str(error))
    evidence = InputEvidence(
        semantic_sha256=str(value["semantic_sha256"]),
        parsimony_min=int(value["parsimony_min"]),
        canonical_sha256=core.sha256_file(directory / "canonical.json"),
    )
    if validate_status:
        validate_status_file(
            directory / "status.json",
            input_status_bytes(fixture, evidence, directory, capture_identity),
            "input capture status",
        )
    return evidence


def capture_contract_bytes(
    profile: Profile,
    base: core.Manifest,
    process_metrics_sha256: str,
    affinity: str,
) -> bytes:
    rows: list[dict[str, object]] = []
    for fixture in profile.fixtures:
        for policy in forced_policies(profile):
            for workers in WORKERS:
                row = contract_row(
                    profile,
                    fixture,
                    policy,
                    workers,
                    affinity,
                    "manifest://fixture.pb.gz",
                )
                rows.append(
                    {
                        "row_id": capture_row_id(profile, fixture, policy, workers),
                        "fixture_key": fixture.key,
                        "lazy_policy": policy,
                        "workers": workers,
                        "canonical_argv_sha256": core.canonical_argv_digest(
                            canonical_argv(row)
                        ),
                    }
                )
    return json_bytes(
        {
            "schema": CAPTURE_SCHEMA,
            "schema_version": CAPTURE_SCHEMA_VERSION,
            "profile": profile.name,
            "manifest_id": profile.manifest_id,
            "parent_sha256": base.sha256,
            "frozen_oracle_sha256": base.preamble[
                "frozen_oracle_dagutil_sha256"
            ],
            "process_metrics_sha256": process_metrics_sha256,
            "affinity_cpus": affinity,
            "timeout_seconds": CAPTURE_TIMEOUT_SECONDS,
            "rss_limit_bytes": RSS_LIMIT_BYTES,
            "memory_budget_bytes": MEMORY_BUDGET_BYTES,
            "fixtures": [dataclasses.asdict(item) for item in profile.fixtures],
            "rows": rows,
        }
    )


def capture_identity(
    profile: Profile,
    base: core.Manifest,
    process_metrics_sha256: str,
    affinity: str,
) -> CaptureIdentity:
    contract = capture_contract_bytes(profile, base, process_metrics_sha256, affinity)
    return CaptureIdentity(
        capture_contract_sha256=core.sha256_bytes(contract),
        base_manifest_sha256=base.sha256,
        frozen_oracle_sha256=base.preamble["frozen_oracle_dagutil_sha256"],
        process_metrics_sha256=process_metrics_sha256,
    )


def initialize_capture(
    capture_dir: Path,
    profile: Profile,
    base: core.Manifest,
    process_metrics_sha256: str,
    affinity: str,
) -> Path:
    capture = capture_dir.absolute()
    core.durable_mkdir_parents(capture.parent, "Phase-7/8 capture parent")
    if os.path.lexists(capture):
        capture = core.require_lexical_directory(capture, "Phase-7/8 capture")
    else:
        capture.mkdir(mode=0o755)
        core.fsync_directory(capture.parent)
        capture = core.require_lexical_directory(capture, "Phase-7/8 capture")
    contract = capture_contract_bytes(
        profile, base, process_metrics_sha256, affinity
    )
    ensure_exact_capture_file(
        capture / "capture-contract.json", contract, "Phase-7/8 capture contract"
    )
    ensure_exact_capture_file(
        capture / "capture-contract.json.sha256",
        core.seal_bytes("capture-contract.json", contract),
        "Phase-7/8 capture contract seal",
    )
    for name in ("inputs", "rows"):
        directory = capture / name
        if os.path.lexists(directory):
            core.require_lexical_directory(directory, f"capture {name} directory")
        else:
            directory.mkdir(mode=0o755)
            core.fsync_directory(capture)
    allowed = {
        "capture-contract.json",
        "capture-contract.json.sha256",
        "inputs",
        "rows",
    }
    observed = {item.name for item in capture.iterdir()}
    if observed != allowed:
        fail(f"capture root has unexpected members: {sorted(observed - allowed)}")
    return capture


def ensure_exact_capture_file(
    path: Path,
    data: bytes,
    label: str,
    *,
    before_publish: Callable[[], None] | None = None,
) -> None:
    """Install or resume one deterministic capture file without replacement."""

    staging = path.with_name(f".{path.name}.staging")
    if os.path.lexists(path):
        core.require_regular(path, label)
        if path.stat().st_nlink != 1:
            fail(f"{label} is externally hard-linked: {path}")
        if path.read_bytes() != data:
            fail(f"resumed {label} differs from the required bytes: {path}")
        if os.path.lexists(staging):
            fail(f"completed {label} has an ambiguous staging peer: {staging}")
        return

    if os.path.lexists(staging):
        core.require_regular(staging, f"interrupted {label} staging file")
        if staging.stat().st_nlink != 1:
            fail(f"interrupted {label} staging file is externally hard-linked: {staging}")
        if staging.read_bytes() != data:
            fail(f"interrupted {label} staging file differs from required bytes: {staging}")
        core.fsync_regular_file(staging)
    else:
        core.copy_bytes(staging, data, 0o444)
    core.fsync_directory(path.parent)
    if before_publish is not None:
        before_publish()
    core.rename_noreplace(staging, path)
    core.fsync_directory(path.parent)


def remove_staging(path: Path) -> None:
    if not os.path.lexists(path):
        return
    root = core.require_lexical_directory(path, "incomplete Phase-7/8 capture staging")
    # Preflight the complete closure before changing modes or unlinking anything.
    # Symlinks are safe to unlink as aliases, but hard-linked regular files and
    # special nodes make ownership ambiguous and must remain untouched.
    for directory_text, directories, files in os.walk(root, followlinks=False):
        directory = Path(directory_text)
        for name in directories:
            child = directory / name
            info = child.lstat()
            if not stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode):
                fail(f"capture staging contains a non-directory alias: {child}")
        for name in files:
            child = directory / name
            info = child.lstat()
            if stat.S_ISLNK(info.st_mode):
                continue
            if not stat.S_ISREG(info.st_mode):
                fail(f"capture staging contains a special file: {child}")
            if info.st_nlink != 1:
                fail(f"capture staging contains an externally hard-linked file: {child}")
    for directory_text, directories, _files in os.walk(
        root, topdown=False, followlinks=False
    ):
        directory = Path(directory_text)
        for name in directories:
            child = directory / name
            info = child.lstat()
            if stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode):
                child.chmod(0o755)
        directory.chmod(0o755)
    shutil.rmtree(root)
    core.fsync_directory(root.parent)


def seal_capture_directory(
    staging: Path,
    destination: Path,
    *,
    before_publish: Callable[[], None] | None = None,
) -> None:
    for member in staging.iterdir():
        core.require_regular(member, "fresh capture member")
        member.chmod(0o444)
        core.fsync_regular_file(member)
    staging.chmod(0o555)
    core.fsync_directory(staging)
    if before_publish is not None:
        before_publish()
    core.rename_noreplace(staging, destination)
    core.fsync_directory(destination.parent)


def capture_input(
    profile: Profile,
    fixture: FixtureSpec,
    fixture_file: Path,
    oracle: Path,
    process_metrics: Path,
    affinity: str,
    inputs: Path,
    capture_identity: CaptureIdentity,
) -> InputEvidence:
    del profile  # the input canonical contract is profile-independent
    destination = inputs / fixture.key
    staging = inputs / f".{fixture.key}.staging"
    if os.path.lexists(destination):
        evidence = parse_input_evidence(fixture, destination, capture_identity)
        remove_staging(staging)
        return evidence
    remove_staging(staging)
    staging.mkdir(mode=0o755)
    environment = {
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "PYTHONDONTWRITEBYTECODE": "1",
        "TZ": "Europe/Sofia",
    }
    try:
        run_enforced_capture_command(
            process_metrics,
            core.actual_input_command(oracle, affinity, fixture_file, staging),
            staging / "report.txt",
            staging / "stderr.txt",
            staging / "process-metrics.txt",
            environment,
            f"frozen input canonical {fixture.key}",
        )
        for name in INPUT_FILES:
            core.require_regular(staging / name, f"fresh input {name}")
        evidence = parse_input_evidence(
            fixture, staging, capture_identity, validate_status=False
        )
        status = input_status_bytes(fixture, evidence, staging, capture_identity)
        core.copy_bytes(staging / "status.json", status, 0o444)
        core.copy_bytes(
            staging / "status.json.sha256",
            core.seal_bytes("status.json", status),
            0o444,
        )
        parse_input_evidence(fixture, staging, capture_identity)
        seal_capture_directory(staging, destination)
        return parse_input_evidence(fixture, destination, capture_identity)
    except BaseException:
        # A non-cooperating writer can create the destination after our
        # existence check.  Keep both names for fail-closed inspection; never
        # infer that the foreign target is ours or discard the staged evidence
        # involved in an ambiguous no-replace failure.
        if not os.path.lexists(destination):
            remove_staging(staging)
        raise


def capture_row(
    profile: Profile,
    fixture: FixtureSpec,
    fixture_file: Path,
    policy: str,
    workers: int,
    input_evidence: InputEvidence,
    oracle: Path,
    process_metrics: Path,
    affinity: str,
    rows: Path,
    capture_identity: CaptureIdentity,
) -> RowEvidence:
    row_name = capture_row_id(profile, fixture, policy, workers)
    destination = rows / row_name
    staging = rows / f".{row_name}.staging"
    if os.path.lexists(destination):
        evidence = parse_row_evidence(
            profile,
            fixture,
            policy,
            workers,
            destination,
            input_evidence,
            affinity,
            capture_identity,
        )
        remove_staging(staging)
        return evidence
    remove_staging(staging)
    staging.mkdir(mode=0o755)
    row = contract_row(
        profile,
        fixture,
        policy,
        workers,
        affinity,
        "manifest://fixture.pb.gz",
    )
    environment = {
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "PYTHONDONTWRITEBYTECODE": "1",
        "TZ": "Europe/Sofia",
    }
    try:
        run_enforced_capture_command(
            process_metrics,
            actual_chart_command(oracle, fixture_file, row, staging),
            staging / "report.txt",
            staging / "stderr.txt",
            staging / "process-metrics.txt",
            environment,
            f"frozen search {row_name}",
        )
        run_enforced_capture_command(
            process_metrics,
            core.actual_output_command(oracle, affinity, staging),
            staging / "output-report.txt",
            staging / "output-stderr.txt",
            staging / "output-process-metrics.txt",
            environment,
            f"frozen output canonical {row_name}",
        )
        for name in ROW_FILES:
            core.require_regular(staging / name, f"fresh search capture {name}")
        evidence = parse_row_evidence(
            profile,
            fixture,
            policy,
            workers,
            staging,
            input_evidence,
            affinity,
            capture_identity,
            validate_status=False,
        )
        status = row_status_bytes(profile, evidence, affinity, capture_identity)
        core.copy_bytes(staging / "status.json", status, 0o444)
        core.copy_bytes(
            staging / "status.json.sha256",
            core.seal_bytes("status.json", status),
            0o444,
        )
        parse_row_evidence(
            profile,
            fixture,
            policy,
            workers,
            staging,
            input_evidence,
            affinity,
            capture_identity,
        )
        seal_capture_directory(staging, destination)
        return parse_row_evidence(
            profile,
            fixture,
            policy,
            workers,
            destination,
            input_evidence,
            affinity,
            capture_identity,
        )
    except BaseException:
        if not os.path.lexists(destination):
            remove_staging(staging)
        raise


def validate_matrix(profile: Profile, evidence: Sequence[RowEvidence]) -> None:
    by_key = {
        (item.fixture.key, item.policy, item.workers): item for item in evidence
    }
    wanted = {
        (fixture.key, policy, workers)
        for fixture in profile.fixtures
        for policy in forced_policies(profile)
        for workers in WORKERS
    }
    if set(by_key) != wanted or len(by_key) != len(evidence):
        fail("frozen capture does not contain the exact profile matrix")
    semantic_fields = (
        "expected_refinement_exactness",
        "expected_effective_pattern_batch_size",
        "expected_final_compaction_exactness",
        "expected_chain_exactness",
        "expected_active_patterns",
        "expected_initial_clades",
        "expected_initial_productions",
        "expected_candidates_generated",
        "expected_candidates_scored",
        "expected_exact_verifications",
        "expected_stop_reason",
        "expected_iterations",
        "expected_accepted_moves",
        "expected_initial_score",
        "expected_final_score",
        "expected_validated_parsimony",
    )
    for fixture in profile.fixtures:
        fixture_rows = [item for item in evidence if item.fixture == fixture]
        for field in semantic_fields:
            if len({item.expected[field] for item in fixture_rows}) != 1:
                fail(f"{fixture.key}: frozen workers/policies disagree on {field}")
        if len({item.search_semantic_sha256 for item in fixture_rows}) != 1:
            fail(f"{fixture.key}: frozen workers/policies disagree on search semantics")
        if len({item.sidecar_sha256 for item in fixture_rows}) != 1:
            fail(f"{fixture.key}: frozen workers/policies sidecars are not byte-identical")
        if len({item.canonical_result_sha256 for item in fixture_rows}) != 1:
            fail(f"{fixture.key}: compact canonical search results are not byte-identical")
        if len({item.output_semantic_sha256 for item in fixture_rows}) != 1:
            fail(f"{fixture.key}: frozen workers/policies disagree on output semantics")
        if len({item.output_canonical_sha256 for item in fixture_rows}) != 1:
            fail(f"{fixture.key}: canonical output summaries are not byte-identical")
        if profile.name == "phase7":
            off = by_key[(fixture.key, "off", 1)]
            on = by_key[(fixture.key, "on", 1)]
            if fixture.key == "high-compression":
                if on.lazy_internal_ratio is None or on.lazy_internal_ratio > Decimal("0.25"):
                    fail("high-compression structural-class ratio exceeds 0.25")
                if on.lazy_merge_ratio is None or on.lazy_merge_ratio > Decimal("0.50"):
                    fail("high-compression lazy/dense retained-row ratio exceeds 0.50")
                if on.initial_chart_ms <= Decimal("50"):
                    fail("high-compression W1 measured lazy phase does not exceed 50 ms")
            elif fixture.key == "dense-favoring":
                if off.total_ms > on.total_ms * Decimal("0.90"):
                    fail("dense-favoring forced dense is not at least 10% faster than lazy")
    if profile.name == "phase8":
        w1 = by_key[(profile.fixtures[0].key, "off", 1)]
        if w1.candidate_generation_ms < Decimal("100"):
            fail("Phase-8 W1 candidate generation is below 100 ms")


def capture_profile(
    profile: Profile,
    base: core.Manifest,
    fixture_root: Path,
    capture_dir: Path,
    oracle: Path,
    process_metrics: Path,
    process_metrics_sha256: str,
    affinity: str,
) -> tuple[Path, Mapping[str, InputEvidence], tuple[RowEvidence, ...]]:
    capture = initialize_capture(
        capture_dir, profile, base, process_metrics_sha256, affinity
    )
    identity = capture_identity(profile, base, process_metrics_sha256, affinity)
    inputs: dict[str, InputEvidence] = {}
    evidence: list[RowEvidence] = []
    for fixture in profile.fixtures:
        path = fixture_path(fixture_root, fixture)
        inputs[fixture.key] = capture_input(
            profile,
            fixture,
            path,
            oracle,
            process_metrics,
            affinity,
            capture / "inputs",
            identity,
        )
        for policy in forced_policies(profile):
            for workers in WORKERS:
                evidence.append(
                    capture_row(
                        profile,
                        fixture,
                        path,
                        policy,
                        workers,
                        inputs[fixture.key],
                        oracle,
                        process_metrics,
                        affinity,
                        capture / "rows",
                        identity,
                    )
                )
    expected_input_names = {fixture.key for fixture in profile.fixtures}
    observed_input_names = {
        path.name for path in (capture / "inputs").iterdir() if not path.name.startswith(".")
    }
    if observed_input_names != expected_input_names:
        fail("capture input directory does not contain the exact fixture set")
    if any(path.name.startswith(".") for path in (capture / "inputs").iterdir()):
        fail("capture inputs directory retains an unknown dot-prefixed remnant")
    expected_row_names = {
        capture_row_id(profile, fixture, policy, workers)
        for fixture in profile.fixtures
        for policy in forced_policies(profile)
        for workers in WORKERS
    }
    observed_row_names = {
        path.name for path in (capture / "rows").iterdir() if not path.name.startswith(".")
    }
    if observed_row_names != expected_row_names:
        fail("capture rows directory does not contain the exact forced matrix")
    if any(path.name.startswith(".") for path in (capture / "rows").iterdir()):
        fail("capture rows directory retains interrupted staging")
    validate_matrix(profile, evidence)
    return capture, inputs, tuple(evidence)


def evidence_lookup(
    evidence: Iterable[RowEvidence],
) -> dict[tuple[str, str, int], RowEvidence]:
    result: dict[tuple[str, str, int], RowEvidence] = {}
    for item in evidence:
        key = (item.fixture.key, item.policy, item.workers)
        if key in result:
            fail(f"duplicate evidence row: {key}")
        result[key] = item
    return result


def make_manifest_rows(
    profile: Profile,
    evidence: Sequence[RowEvidence],
    affinity: str,
    asset_name: str,
) -> list[dict[str, str]]:
    by_key = evidence_lookup(evidence)
    rows: list[dict[str, str]] = []
    for fixture in profile.fixtures:
        for policy in profile.policies:
            for workers in WORKERS:
                source_policy = fixture.auto_source_policy if policy == "auto" else policy
                source = by_key[(fixture.key, source_policy, workers)]
                primary_uri = (
                    f"manifest://{asset_name}/fixtures/{fixture.key}.pb.gz"
                )
                row = contract_row(
                    profile, fixture, policy, workers, affinity, primary_uri
                )
                source_id = capture_row_id(
                    profile, fixture, source_policy, workers
                )
                sidecar_uri = (
                    f"manifest://{asset_name}/provenance/rows/"
                    f"{source_id}/canonical.ndjson"
                )
                compact_uri = (
                    f"manifest://{asset_name}/provenance/rows/"
                    f"{source_id}/canonical.json"
                )
                row.update(dict(source.expected))
                row.update(
                    {
                        "oracle_search_semantic_sha256": source.search_semantic_sha256,
                        "oracle_output_semantic_sha256": source.output_semantic_sha256,
                        "canonical_sidecar_uri": sidecar_uri,
                        "canonical_sidecar_sha256": source.sidecar_sha256,
                        "oracle_report_uri": compact_uri,
                        "oracle_report_sha256": source.canonical_result_sha256,
                    }
                )
                argv_sha = core.canonical_argv_digest(canonical_argv(row))
                row["canonical_argv_sha256"] = argv_sha
                row["oracle_trial_semantic_sha256"] = core.trial_digest(
                    profile.method,
                    source.search_semantic_sha256,
                    source.output_semantic_sha256,
                    argv_sha,
                )
                rows.append(row)
    return rows


def add_asset(files: dict[str, bytes], relative: str, data: bytes) -> None:
    if core.LEDGER_RELATIVE.fullmatch(relative) is None:
        fail(f"asset path is not canonical: {relative!r}")
    if relative in files:
        fail(f"asset path collision: {relative}")
    files[relative] = data


def capture_asset_files(
    profile: Profile, root: Path, capture: Path
) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for fixture in profile.fixtures:
        add_asset(
            files,
            f"fixtures/{fixture.key}.pb.gz",
            fixture_path(root, fixture).read_bytes(),
        )
        reference = fixture_ref_path(root, fixture)
        if reference is not None:
            add_asset(
                files,
                f"fixtures/{fixture.key}.ref",
                reference.read_bytes(),
            )
    for name in ("capture-contract.json", "capture-contract.json.sha256"):
        add_asset(files, f"provenance/{name}", (capture / name).read_bytes())
    for fixture in profile.fixtures:
        directory = capture / "inputs" / fixture.key
        for name in (*INPUT_FILES, *STATUS_FILES):
            add_asset(
                files,
                f"provenance/inputs/{fixture.key}/{name}",
                (directory / name).read_bytes(),
            )
    for fixture in profile.fixtures:
        for policy in forced_policies(profile):
            for workers in WORKERS:
                row_name = capture_row_id(profile, fixture, policy, workers)
                directory = capture / "rows" / row_name
                for name in (*ROW_FILES, *STATUS_FILES):
                    add_asset(
                        files,
                        f"provenance/rows/{row_name}/{name}",
                        (directory / name).read_bytes(),
                    )
    return files


def shell_token(token: str) -> str:
    if token.startswith('"$') and token.endswith('"'):
        return token
    return shlex.quote(token)


def render_commands(
    profile: Profile,
    affinity: str,
    oracle_sha256: str,
    process_metrics_sha256: str,
    ledger_sha256: str,
) -> bytes:
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        'assets=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)',
        f"readonly expected_ledger_sha256={ledger_sha256}",
        f"readonly expected_oracle_sha256={oracle_sha256}",
        f"readonly expected_process_metrics_sha256={process_metrics_sha256}",
        '[[ $(sha256sum "$assets/assets.sha256" | awk \'{print $1}\') == "$expected_ledger_sha256" ]] || { echo "Phase-7/8 asset ledger hash mismatch" >&2; exit 1; }',
        '(cd "$assets" && sha256sum --check --strict assets.sha256)',
        '[[ ${1:-} != --verify-only ]] || exit 0',
        'oracle=${WRIC_PHASE78_FROZEN_ORACLE:?set WRIC_PHASE78_FROZEN_ORACLE}',
        '[[ $(sha256sum "$oracle" | awk \'{print $1}\') == "$expected_oracle_sha256" ]] || { echo "Phase-7/8 oracle hash mismatch" >&2; exit 1; }',
        'out=${1:?usage: commands.sh OUTPUT_DIRECTORY}',
        'mkdir -- "$out"',
        "",
    ]
    for fixture in profile.fixtures:
        for policy in forced_policies(profile):
            for workers in WORKERS:
                name = capture_row_id(profile, fixture, policy, workers)
                row = contract_row(
                    profile,
                    fixture,
                    policy,
                    workers,
                    affinity,
                    "manifest://fixture.pb.gz",
                )
                argv = canonical_argv(row)
                argv[0] = '"$oracle"'
                argv[2] = f'"$assets/fixtures/{fixture.key}.pb.gz"'
                argv[-1] = f'"$out/{name}.pb.gz"'
                output_flag = len(argv) - 2
                argv[output_flag:output_flag] = [
                    "--chart-spr-canonical-result",
                    f'"$out/{name}.canonical.json"',
                    "--chart-spr-canonical-sidecar",
                    f'"$out/{name}.canonical.ndjson"',
                ]
                command = ["taskset", "-c", affinity, *argv]
                lines.append(
                    f"# {name} canonical_argv_sha256="
                    f"{core.canonical_argv_digest(canonical_argv(row))}"
                )
                lines.append(
                    " \\\n  ".join(shell_token(token) for token in command)
                    + f' >"$out/{name}.report.txt" 2>"$out/{name}.stderr.txt"'
                )
                output_command = (
                    "taskset",
                    "-c",
                    affinity,
                    '"$oracle"',
                    "--dag-pb",
                    f'"$out/{name}.pb.gz"',
                    "--force-no-vcf",
                    "--validate",
                    "--dag-info",
                    "--canonical-dag-result",
                    f'"$out/{name}.output-canonical.json"',
                )
                lines.append(
                    " \\\n  ".join(shell_token(token) for token in output_command)
                    + f' >"$out/{name}.output-report.txt"'
                    + f' 2>"$out/{name}.output-stderr.txt"'
                )
                lines.append("")
        if "auto" in profile.policies:
            lines.append(
                f"# {fixture.key}: auto rows derive from frozen forced "
                f"{fixture.auto_source_policy}; the Phase-0 oracle has no auto policy."
            )
            lines.append("")
    return ("\n".join(lines) + "\n").encode("utf-8")


def asset_ledger_bytes(files: Mapping[str, bytes]) -> bytes:
    return b"".join(
        f"{core.sha256_bytes(data)}  {name}\n".encode("ascii")
        for name, data in sorted(files.items())
    )


def parse_asset_ledger(path: Path) -> dict[str, str]:
    core.require_regular(path, "Phase-7/8 asset ledger")
    pattern = re.compile(
        r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)"
    )
    seen: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except UnicodeDecodeError as error:
        fail(f"asset ledger is not ASCII: {error}")
    for number, line in enumerate(lines, 1):
        match = pattern.fullmatch(line)
        if match is None:
            fail(f"asset ledger line {number} is not canonical")
        digest, relative = match.groups()
        if relative in seen or relative in ("assets.sha256", "commands.sh"):
            fail(f"asset ledger has duplicate/circular member {relative!r}")
        seen[relative] = digest
    wanted = b"".join(
        f"{seen[name]}  {name}\n".encode("ascii") for name in sorted(seen)
    )
    if path.read_bytes() != wanted:
        fail("asset ledger is not the canonical sorted byte stream")
    return seen


def expected_archive_members(profile: Profile) -> set[str]:
    result = {
        f"fixtures/{fixture.key}.pb.gz" for fixture in profile.fixtures
    }
    result.update(
        f"fixtures/{fixture.key}.ref"
        for fixture in profile.fixtures
        if fixture.ref_relative_path is not None
    )
    result.update(
        {
            "provenance/capture-contract.json",
            "provenance/capture-contract.json.sha256",
        }
    )
    for fixture in profile.fixtures:
        result.update(
            f"provenance/inputs/{fixture.key}/{name}"
            for name in (*INPUT_FILES, *STATUS_FILES)
        )
        for policy in forced_policies(profile):
            for workers in WORKERS:
                row_name = capture_row_id(profile, fixture, policy, workers)
                result.update(
                    f"provenance/rows/{row_name}/{name}"
                    for name in (*ROW_FILES, *STATUS_FILES)
                )
    return result


def read_capture_contract(
    profile: Profile, base: core.Manifest, asset_root: Path
) -> tuple[str, str, CaptureIdentity]:
    path = asset_root / "provenance" / "capture-contract.json"
    digest = core.verify_detached_seal(path)
    try:
        value = core.read_json_object(path, "Phase-7/8 capture contract")
    except core.BootstrapError as error:
        fail(str(error))
    process_metrics_sha256 = value.get("process_metrics_sha256")
    affinity = value.get("affinity_cpus")
    if not isinstance(process_metrics_sha256, str) or not isinstance(affinity, str):
        fail("capture contract lacks string process-metrics hash/affinity")
    core.validate_hash(process_metrics_sha256, "capture process-metrics SHA-256")
    core.validate_affinity(affinity, "capture")
    wanted = capture_contract_bytes(
        profile, base, process_metrics_sha256, affinity
    )
    if path.read_bytes() != wanted or digest != core.sha256_bytes(wanted):
        fail("archived capture contract differs from the closed profile contract")
    return (
        process_metrics_sha256,
        affinity,
        CaptureIdentity(
            capture_contract_sha256=digest,
            base_manifest_sha256=base.sha256,
            frozen_oracle_sha256=base.preamble["frozen_oracle_dagutil_sha256"],
            process_metrics_sha256=process_metrics_sha256,
        ),
    )


def audit_archive(
    profile: Profile, base: core.Manifest, supplement: core.Manifest, root: Path
) -> tuple[str, str, tuple[RowEvidence, ...]]:
    commands = core.resolve_manifest_uri(
        supplement.path, supplement.preamble["commands_uri"], root
    )
    asset_root = core.require_lexical_directory(
        commands.parent, "Phase-7/8 supplement asset directory"
    )
    ledger = core.resolve_lexical_regular(
        asset_root, Path("assets.sha256"), "Phase-7/8 asset ledger"
    )
    members = parse_asset_ledger(ledger)
    expected = expected_archive_members(profile)
    if set(members) != expected:
        fail(
            "asset ledger is not the exact profile closure: "
            f"missing={sorted(expected - set(members))}, "
            f"extra={sorted(set(members) - expected)}"
        )
    for relative, digest in members.items():
        member = core.resolve_lexical_regular(
            asset_root, Path(relative), f"asset ledger member {relative}"
        )
        if member.stat().st_nlink != 1 or core.sha256_file(member) != digest:
            fail(f"asset ledger member changed: {relative}")
    core.audit_exact_asset_tree(asset_root, expected | {"assets.sha256", "commands.sh"})
    process_metrics_sha256, affinity, identity = read_capture_contract(
        profile, base, asset_root
    )
    input_evidence: dict[str, InputEvidence] = {}
    evidence: list[RowEvidence] = []
    for fixture in profile.fixtures:
        fixture_copy = asset_root / "fixtures" / f"{fixture.key}.pb.gz"
        if core.sha256_file(fixture_copy) != fixture.sha256:
            fail(f"archived fixture hash changed: {fixture.key}")
        if fixture.ref_relative_path is not None:
            assert fixture.ref_sha256 is not None
            reference_copy = asset_root / "fixtures" / f"{fixture.key}.ref"
            if core.sha256_file(reference_copy) != fixture.ref_sha256:
                fail(f"archived fixture reference hash changed: {fixture.key}")
        input_directory = asset_root / "provenance" / "inputs" / fixture.key
        input_evidence[fixture.key] = parse_input_evidence(
            fixture, input_directory, identity
        )
        for policy in forced_policies(profile):
            for workers in WORKERS:
                row_name = capture_row_id(profile, fixture, policy, workers)
                evidence.append(
                    parse_row_evidence(
                        profile,
                        fixture,
                        policy,
                        workers,
                        asset_root / "provenance" / "rows" / row_name,
                        input_evidence[fixture.key],
                        affinity,
                        identity,
                    )
                )
    validate_matrix(profile, evidence)
    ledger_sha = core.sha256_file(ledger)
    wanted_commands = render_commands(
        profile,
        affinity,
        base.preamble["frozen_oracle_dagutil_sha256"],
        process_metrics_sha256,
        ledger_sha,
    )
    if commands.read_bytes() != wanted_commands:
        fail("commands asset differs from the exact reproduction script")
    if not commands.stat().st_mode & 0o111:
        fail("commands asset is not executable")
    return process_metrics_sha256, affinity, tuple(evidence)


def audit_supplement(
    profile: Profile,
    base_manifest_path: Path,
    expected_parent_sha256: str,
    expected_process_metrics_sha256: str,
    supplement_path: Path,
    root: Path,
) -> AuditedBundle:
    core.validate_hash(expected_parent_sha256, "expected parent SHA-256")
    core.validate_hash(
        expected_process_metrics_sha256, "expected process-metrics SHA-256"
    )
    try:
        base = core.read_manifest(base_manifest_path, root, expected_kind="base")
        supplement = core.read_manifest(
            supplement_path, root, expected_kind="supplement"
        )
    except core.BootstrapError as error:
        fail(str(error))
    if base.sha256 != expected_parent_sha256:
        fail("sealed base hash differs from --expected-parent-sha256")
    if base.preamble["parent_sha256"] != "-":
        fail("base manifest must declare parent_sha256=-")
    if supplement.path.name != profile.output_name:
        fail(f"{profile.name} supplement basename must be {profile.output_name}")
    if supplement.preamble["manifest_id"] != profile.manifest_id:
        fail("supplement manifest_id differs from the selected profile")
    if supplement.preamble["parent_sha256"] != base.sha256:
        fail("supplement parent hash differs from the sealed base")
    for key in (
        "schema",
        "schema_version",
        "repo_revision",
        "merge_base",
        "frozen_larch2_sha256",
        "frozen_oracle_dagutil_sha256",
    ):
        if supplement.preamble[key] != base.preamble[key]:
            fail(f"supplement changes base preamble role {key}")
    for uri_key in ("frozen_larch2_uri", "frozen_oracle_dagutil_uri"):
        base_asset = core.resolve_manifest_uri(base.path, base.preamble[uri_key], root)
        supplement_asset = core.resolve_manifest_uri(
            supplement.path, supplement.preamble[uri_key], root
        )
        if base_asset != supplement_asset:
            fail(f"supplement changes base frozen role {uri_key}")
    if set(row["row_id"] for row in base.rows) & set(
        row["row_id"] for row in supplement.rows
    ):
        fail("supplement overrides a base manifest row ID")

    process_metrics_sha256, affinity, evidence = audit_archive(
        profile, base, supplement, root
    )
    if process_metrics_sha256 != expected_process_metrics_sha256:
        fail(
            "archived capture process-metrics hash differs from "
            "--expected-process-metrics-sha256"
        )
    expected_rows = make_manifest_rows(
        profile, evidence, affinity, supplement.path.stem + ".assets"
    )
    if list(supplement.rows) != expected_rows:
        observed_ids = [row["row_id"] for row in supplement.rows]
        wanted_ids = [row["row_id"] for row in expected_rows]
        if observed_ids != wanted_ids:
            fail("supplement row set/order differs from the exact profile matrix")
        for observed, wanted in zip(supplement.rows, expected_rows, strict=True):
            differing = [key for key in core.MANIFEST_HEADER if observed[key] != wanted[key]]
            if differing:
                fail(
                    f"supplement row {observed['row_id']} differs in "
                    + ", ".join(differing)
                )
        fail("supplement rows differ from re-derived evidence")
    return AuditedBundle(
        base=base,
        supplement=supplement,
        process_metrics_sha256=process_metrics_sha256,
        profile=profile,
    )


def validate_harness_sentinel_result(
    result: subprocess.CompletedProcess[str],
) -> None:
    """Require the one ordinary failure that proves full manifest validation."""

    expected = f"error: manifest group has no rows: {HARNESS_SENTINEL_GROUP}"
    if (
        result.returncode != 1
        or result.stdout != ""
        or result.stderr != expected + "\n"
    ):
        detail = (result.stdout + result.stderr).strip().splitlines()
        fail(
            "benchmark harness did not produce the exact post-validation "
            "exit-1/empty-stdout/lone-stderr sentinel: "
            + (detail[-1] if detail else f"exit={result.returncode}")
        )


def validate_with_harness(
    audit: AuditedBundle,
    harness_path: Path,
    process_metrics_path: Path,
    root: Path,
) -> None:
    harness = core.require_canonical_regular(
        harness_path, "benchmark harness", executable=True
    )
    wanted_harness = core.require_canonical_regular(
        TOOLS / "wric_spr_search_benchmark.sh",
        "repository benchmark harness",
        executable=True,
    )
    if harness != wanted_harness:
        fail("--benchmark-harness is not the repository production harness")
    process_metrics = core.require_canonical_regular(
        process_metrics_path, "process-metrics runner", executable=True
    )
    if process_metrics.stat().st_nlink != 1:
        fail("process-metrics runner must not be externally hard-linked")
    if core.sha256_file(process_metrics) != audit.process_metrics_sha256:
        fail("process-metrics runner differs from the archived capture binary")
    oracle = core.resolve_manifest_uri(
        audit.base.path,
        audit.base.preamble["frozen_oracle_dagutil_uri"],
        root,
    )
    larch2 = core.resolve_manifest_uri(
        audit.base.path, audit.base.preamble["frozen_larch2_uri"], root
    )
    sentinel = audit.supplement.path.parent / (
        "." + audit.supplement.path.name + ".harness-validation"
    )
    if os.path.lexists(sentinel):
        fail(f"benchmark-validation sentinel already exists: {sentinel}")
    result = subprocess.run(
        [
            os.fspath(harness),
            "--dagutil",
            os.fspath(oracle),
            "--larch2",
            os.fspath(larch2),
            "--process-metrics",
            os.fspath(process_metrics),
            "--out-dir",
            os.fspath(sentinel),
            "--workload-manifest",
            os.fspath(audit.base.path),
            "--supplemental-workload-manifest",
            os.fspath(audit.supplement.path),
            "--run-manifest-group",
            HARNESS_SENTINEL_GROUP,
        ],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd=root,
        env={
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
            "TZ": "Europe/Sofia",
            "WRIC_REPO_ROOT": os.fspath(root),
        },
        timeout=60,
    )
    if os.path.lexists(sentinel):
        fail("benchmark harness created output before reaching the sentinel")
    validate_harness_sentinel_result(result)


def validate_build_inputs(
    args: argparse.Namespace, profile: Profile, root: Path
) -> tuple[core.Manifest, Path, Path, Path, str, FixtureSource]:
    try:
        base = core.read_manifest(args.base_manifest, root, expected_kind="base")
    except core.BootstrapError as error:
        fail(str(error))
    core.validate_hash(args.expected_parent_sha256, "expected parent SHA-256")
    core.validate_hash(
        args.expected_process_metrics_sha256, "expected process-metrics SHA-256"
    )
    if base.sha256 != args.expected_parent_sha256:
        fail("sealed base hash differs from --expected-parent-sha256")
    if base.preamble["parent_sha256"] != "-":
        fail("base workload manifest must declare parent_sha256=-")
    core.validate_affinity(args.affinity_cpus, "Phase-7/8 capture")
    fixture_source = fixture_source_for_build(args, profile, root, base)
    oracle = core.resolve_manifest_uri(
        base.path, base.preamble["frozen_oracle_dagutil_uri"], root
    )
    larch2 = core.resolve_manifest_uri(
        base.path, base.preamble["frozen_larch2_uri"], root
    )
    core.require_regular(oracle, "base frozen oracle", executable=True)
    core.require_regular(larch2, "base frozen larch2", executable=True)
    process_metrics = core.require_canonical_regular(
        args.process_metrics, "process-metrics runner", executable=True
    )
    if process_metrics.stat().st_nlink != 1:
        fail("process-metrics runner must not be externally hard-linked")
    process_metrics_sha256 = core.sha256_file(process_metrics)
    if process_metrics_sha256 != args.expected_process_metrics_sha256:
        fail(
            "process-metrics runner differs from "
            "--expected-process-metrics-sha256"
        )
    return (
        base,
        oracle,
        larch2,
        process_metrics,
        process_metrics_sha256,
        fixture_source,
    )


def build_locked(
    args: argparse.Namespace,
    profile: Profile,
    publication: core.PublicationPaths,
    root: Path,
) -> None:
    (
        base,
        oracle,
        larch2,
        process_metrics,
        process_metrics_sha256,
        fixture_source,
    ) = validate_build_inputs(args, profile, root)
    capture_absolute = args.capture_dir.absolute()
    for reserved in dataclasses.astuple(publication):
        if (
            capture_absolute == reserved
            or capture_absolute.is_relative_to(reserved)
            or reserved.is_relative_to(capture_absolute)
        ):
            fail("--capture-dir overlaps the supplement publication namespace")
    with core.exclusive_capture_lock(args.capture_dir):
        capture, _inputs, evidence = capture_profile(
            profile,
            base,
            fixture_source.root,
            args.capture_dir,
            oracle,
            process_metrics,
            process_metrics_sha256,
            args.affinity_cpus,
        )
    if core.sha256_file(oracle) != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("frozen oracle changed during capture")
    if core.sha256_file(larch2) != base.preamble["frozen_larch2_sha256"]:
        fail("frozen larch2 changed during capture")
    if core.sha256_file(process_metrics) != process_metrics_sha256:
        fail("process-metrics runner changed during capture")

    # Revalidate both revision and relevant-file cleanliness after the long
    # capture, before any source byte enters the immutable archive.
    fixture_source = validate_fixture_source_tree(
        profile, fixture_source.root, fixture_source.revision
    )

    asset_files = capture_asset_files(profile, fixture_source.root, capture)
    ledger = asset_ledger_bytes(asset_files)
    ledger_sha = core.sha256_bytes(ledger)
    asset_files["assets.sha256"] = ledger
    commands = render_commands(
        profile,
        args.affinity_cpus,
        base.preamble["frozen_oracle_dagutil_sha256"],
        process_metrics_sha256,
        ledger_sha,
    )
    asset_files["commands.sh"] = commands
    rows = make_manifest_rows(
        profile,
        evidence,
        args.affinity_cpus,
        publication.assets.name,
    )
    preamble = {
        "schema": core.SCHEMA,
        "schema_version": core.SCHEMA_VERSION,
        "kind": "supplement",
        "manifest_id": profile.manifest_id,
        "parent_sha256": base.sha256,
        "repo_revision": base.preamble["repo_revision"],
        "merge_base": base.preamble["merge_base"],
        "frozen_larch2_uri": core.repo_uri(root, larch2),
        "frozen_larch2_sha256": base.preamble["frozen_larch2_sha256"],
        "frozen_oracle_dagutil_uri": core.repo_uri(root, oracle),
        "frozen_oracle_dagutil_sha256": base.preamble[
            "frozen_oracle_dagutil_sha256"
        ],
        "commands_uri": f"manifest://{publication.assets.name}/commands.sh",
        "commands_sha256": core.sha256_bytes(commands),
    }
    manifest_data = core.tsv_bytes(preamble, rows)
    seal_data = core.seal_bytes(publication.output.name, manifest_data)
    ownership = core.PublicationOwnership()

    def validate_private(staged_output: Path) -> None:
        audited = audit_supplement(
            profile,
            args.base_manifest,
            args.expected_parent_sha256,
            args.expected_process_metrics_sha256,
            staged_output,
            root,
        )
        validate_with_harness(audited, args.benchmark_harness, process_metrics, root)

    try:
        core.publish_immutable_supplement(
            publication,
            asset_files,
            manifest_data,
            seal_data,
            ownership=ownership,
            prepublish_validator=validate_private,
        )
        audited = audit_supplement(
            profile,
            args.base_manifest,
            args.expected_parent_sha256,
            args.expected_process_metrics_sha256,
            publication.output,
            root,
        )
        validate_with_harness(audited, args.benchmark_harness, process_metrics, root)
        core.finish_publication(publication)
    except BaseException as error:
        try:
            core.rollback_owned_publication(publication, ownership)
        except BaseException as rollback_error:
            fail(
                "publication rollback failed closed after "
                f"{error}: {rollback_error}"
            )
        raise


def build(args: argparse.Namespace) -> None:
    profile = profile_named(args.profile)
    fixture_source_options(args)
    root = core.repo_root(args.repo_root)
    output = args.output.absolute()
    if output.name != profile.output_name:
        fail(f"{profile.name} output basename must be {profile.output_name}")
    output_parent = core.durable_mkdir_parents(
        output.parent, "Phase-7/8 output parent"
    )
    output = output_parent / output.name
    with core.exclusive_output_lock(output) as publication:

        def validate_private(staged_output: Path) -> None:
            audited = audit_supplement(
                profile,
                args.base_manifest,
                args.expected_parent_sha256,
                args.expected_process_metrics_sha256,
                staged_output,
                root,
            )
            validate_with_harness(
                audited, args.benchmark_harness, args.process_metrics, root
            )

        try:
            recovered = core.recover_interrupted_publication(
                publication, prepublish_validator=validate_private
            )
        except core.BootstrapError as error:
            fail(str(error))
        if recovered:
            audited = audit_supplement(
                profile,
                args.base_manifest,
                args.expected_parent_sha256,
                args.expected_process_metrics_sha256,
                publication.output,
                root,
            )
            validate_with_harness(
                audited, args.benchmark_harness, args.process_metrics, root
            )
            core.finish_publication(publication)
            return
        existing = [
            path
            for path in (publication.output, publication.assets, publication.seal)
            if os.path.lexists(path)
        ]
        if existing:
            fail(f"exclusive immutable output already exists: {existing[0]}")
        build_locked(args, profile, publication, root)


def audit_command(args: argparse.Namespace) -> None:
    profile = profile_named(args.profile)
    root = core.repo_root(args.repo_root)
    audited = audit_supplement(
        profile,
        args.base_manifest,
        args.expected_parent_sha256,
        args.expected_process_metrics_sha256,
        args.supplement,
        root,
    )
    validate_with_harness(audited, args.benchmark_harness, args.process_metrics, root)


def plan_payload(profile: Profile) -> dict[str, object]:
    captures = [
        {
            "row_id": capture_row_id(profile, fixture, policy, workers),
            "fixture": fixture.relative_path,
            "fixture_reference": fixture.ref_relative_path,
            "lazy_policy": policy,
            "workers": workers,
        }
        for fixture in profile.fixtures
        for policy in forced_policies(profile)
        for workers in WORKERS
    ]
    rows = [
        {
            "row_id": manifest_row_id(profile, fixture, policy, workers),
            "fixture": fixture.relative_path,
            "lazy_policy": policy,
            "workers": workers,
            "evidence_policy": (
                fixture.auto_source_policy if policy == "auto" else policy
            ),
        }
        for fixture in profile.fixtures
        for policy in profile.policies
        for workers in WORKERS
    ]
    return {
        "schema": "wric_phase78_bootstrap_plan",
        "schema_version": 1,
        "profile": profile.name,
        "manifest_id": profile.manifest_id,
        "required_output_basename": profile.output_name,
        "method": profile.method,
        "iterations": 1,
        "seed": 1,
        "max_candidates": profile.max_candidates,
        "top_k_exact": profile.top_k_exact,
        "scope": "frozen_oracle_workload_rows_only",
        "does_not_seal": [
            "same_revision_timing_medians",
            "speedup_verdicts",
            "phase_acceptance_results",
        ],
        "capture_count": len(captures),
        "manifest_row_count": len(rows),
        "captures": captures,
        "manifest_rows": rows,
    }


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    plan = subparsers.add_parser("plan", help="print the closed profile matrix")
    plan.add_argument("--profile", choices=tuple(PROFILES), required=True)
    for name in ("build", "audit"):
        command = subparsers.add_parser(
            name,
            help=(
                "capture and publish a sealed supplement"
                if name == "build"
                else "audit a sealed supplement"
            ),
        )
        command.add_argument("--profile", choices=tuple(PROFILES), required=True)
        command.add_argument("--base-manifest", type=Path, required=True)
        command.add_argument("--expected-parent-sha256", required=True)
        command.add_argument("--repo-root", type=Path)
        command.add_argument("--benchmark-harness", type=Path, required=True)
        command.add_argument("--process-metrics", type=Path, required=True)
        command.add_argument(
            "--expected-process-metrics-sha256", required=True
        )
        if name == "build":
            command.add_argument(
                "--fixture-source-root",
                type=Path,
                help=(
                    "canonical Git worktree containing the profile's hard-coded "
                    "tracked fixture paths"
                ),
            )
            command.add_argument(
                "--expected-fixture-source-revision",
                help=(
                    "externally recorded full Git revision for "
                    "--fixture-source-root"
                ),
            )
            command.add_argument("--capture-dir", type=Path, required=True)
            command.add_argument("--output", type=Path, required=True)
            command.add_argument("--affinity-cpus", required=True)
        else:
            command.add_argument("--supplement", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    try:
        args = parse_args(argv)
        if args.command == "plan":
            print(json.dumps(plan_payload(profile_named(args.profile)), indent=2, sort_keys=True))
        elif args.command == "build":
            build(args)
        elif args.command == "audit":
            audit_command(args)
        else:
            raise AssertionError(args.command)
    except (
        Phase78Error,
        core.BootstrapError,
        FileNotFoundError,
        PermissionError,
        OSError,
        UnicodeError,
        ValueError,
        subprocess.SubprocessError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
