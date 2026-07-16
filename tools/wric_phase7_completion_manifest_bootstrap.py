#!/usr/bin/env python3
"""Build and audit the eight missing Phase-7 workload rows.

The sealed Phase-0 manifest already owns forced dense/lazy seedtree rows and
forced-dense ``data/test_5_trees/tree_0.pb.gz`` rows.  This tool publishes a
separate immutable supplement containing only:

* seedtree ``lazy=auto`` at W1/2/4/8; and
* tree_0 ``lazy=on`` at W1/2/4/8.

The old frozen oracle cannot parse ``lazy=auto``.  Each medium auto row is
therefore qualified with the explicitly hash-bound current working chart.  Its
resolved policy and canonical input/search/output semantics must match the
corresponding forced Phase-0 row byte-for-byte.  The manifest semantics are
then re-derived from that sealed forced row.  The genuinely new tree_0-on rows
are captured with the frozen oracle under the 600-second process runner.

Publication reuses the Phase-9 crash-durable, no-replace transaction.  Audit
re-derives the exact eight-row closure and asks the production benchmark
harness to parse the sealed base/supplement pair before accepting it.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from typing import Callable, Mapping, NoReturn, Sequence

sys.dont_write_bytecode = True

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import wric_phase0_manifest_bootstrap as phase0  # noqa: E402
import wric_phase78_manifest_bootstrap as phase78  # noqa: E402
import wric_phase9_manifest_bootstrap as core  # noqa: E402


WORKERS = (1, 2, 4, 8)
OUTPUT_NAME = "phase7-lazy-completion.tsv"
MANIFEST_ID = "phase7-lazy-completion"
MEDIUM_GROUP = "phase7-lazy-medium-auto"
SMALL_GROUP = "phase7-lazy-small-on"
HARNESS_SENTINEL_GROUP = "phase7-lazy-completion-validation-sentinel"
TIMEOUT_SECONDS = 600
RSS_LIMIT_BYTES = core.RSS_LIMIT_BYTES
MEMORY_BUDGET_BYTES = core.MEMORY_BUDGET_BYTES
CAPTURE_SCHEMA = "wric_phase7_lazy_completion_capture"
CAPTURE_SCHEMA_VERSION = 1
QUALIFICATION_STATUS_SCHEMA = "wric_phase7_auto_qualification_status"
QUALIFICATION_STATUS_VERSION = 2

TREE_FIXTURE = phase78.FixtureSpec(
    key="tree0",
    fixture_id="small-test-5-tree0",
    workload_name="phase7-lazy-small-on",
    relative_path="data/test_5_trees/tree_0.pb.gz",
    sha256="e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6",
)

TREE_PROFILE = phase78.Profile(
    name="phase7",
    manifest_id=MANIFEST_ID,
    output_name=OUTPUT_NAME,
    method="chart_spr_grammar_lower_bound_heuristic",
    acceptance="lower_bound_heuristic",
    objective="composite_lower_bound_heuristic",
    candidate_source="grammar",
    topology_selector="none",
    max_candidates=64,
    top_k_exact=0,
    # Both values are needed to validate the sealed Phase-0 source rows.  The
    # completion manifest itself is still constructed explicitly and contains
    # only the four ``on`` rows.
    policies=("off", "on"),
    fixtures=(TREE_FIXTURE,),
)

MEDIUM_PRIMARY_SHA256 = phase0.MEDIUM_PRIMARY_SHA256
MEDIUM_REFSEQ_SHA256 = phase0.MEDIUM_REFSEQ_SHA256
MEDIUM_PRIMARY_RELATIVE = "data/seedtree/seedtree.pb.gz"
MEDIUM_REFSEQ_RELATIVE = "data/seedtree/refseq.txt.gz"

SOURCE_ROW_IDS: Mapping[tuple[str, str, int], tuple[str, str]] = {
    **{
        ("medium", "off", workers): (
            f"p0-medium-dense64-grammar-lower-bound-heuristic-w{workers}",
            "p0-medium-dense-physical",
        )
        for workers in WORKERS
    },
    **{
        ("medium", "on", workers): (
            f"p0-medium-lazy64-grammar-lower-bound-heuristic-w{workers}",
            "p0-medium-lazy-physical",
        )
        for workers in WORKERS
    },
    **{
        ("tree0", "off", workers): (
            f"p0-small-dense64-grammar-lower-bound-heuristic-w{workers}",
            "p0-small-dense-physical",
        )
        for workers in WORKERS
    },
}

EXPECTED_RESULT_FIELDS = (
    "expected_refinement_exactness",
    "expected_effective_pattern_batch_size",
    "expected_keep_mask_kind",
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

QUALIFICATION_FILES = phase78.ROW_FILES


class CompletionError(RuntimeError):
    """Stable command-line rejection for an untrusted completion bundle."""


def fail(message: str) -> NoReturn:
    raise CompletionError(message)


@dataclasses.dataclass(frozen=True)
class SourceEvidence:
    fixture: str
    policy: str
    workers: int
    row: Mapping[str, str]
    sidecar: Path
    compact: Path


@dataclasses.dataclass(frozen=True)
class QualificationEvidence:
    workers: int
    directory: Path
    resolved_policy: str
    source: SourceEvidence


@dataclasses.dataclass(frozen=True)
class AuditedBundle:
    base: core.Manifest
    supplement: core.Manifest
    process_metrics_sha256: str
    working_chart_sha256: str
    affinity: str


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def canonical_row_bytes(row: Mapping[str, str]) -> bytes:
    return json_bytes({field: row[field] for field in core.MANIFEST_HEADER})


def tree_row_id(workers: int) -> str:
    return f"phase7-lazy-completion-tree0-on-w{workers}"


def medium_row_id(workers: int) -> str:
    return f"phase7-lazy-completion-medium-auto-w{workers}"


def canonical_argv(row: Mapping[str, str]) -> list[str]:
    """Return the production-harness argv for DAG or tree+reference input."""

    argv = phase78.canonical_argv(row)
    if row["input_kind"] == "tree_pb_refseq":
        argv[1:3] = [
            "--tree-pb",
            f"@primary:{row['primary_sha256']}",
            "--refseq",
            f"@refseq:{row['refseq_sha256']}",
        ]
    elif row["input_kind"] != "dag_pb":
        fail(f"unsupported completion input kind {row['input_kind']!r}")
    return argv


def actual_chart_command(
    binary: Path,
    primary: Path,
    refseq: Path | None,
    row: Mapping[str, str],
    directory: Path,
) -> list[str]:
    argv = canonical_argv(row)
    replacements = {
        "@binary:working_chart": os.fspath(binary),
        f"@primary:{row['primary_sha256']}": os.fspath(primary),
        "@output": os.fspath(directory / "output.pb.gz"),
    }
    if refseq is not None:
        replacements[f"@refseq:{row['refseq_sha256']}"] = os.fspath(refseq)
    argv = [replacements.get(token, token) for token in argv]
    output_flag = argv.index("-o")
    argv[output_flag:output_flag] = [
        "--chart-spr-canonical-result",
        os.fspath(directory / "canonical.json"),
        "--chart-spr-canonical-sidecar",
        os.fspath(directory / "canonical.ndjson"),
    ]
    return ["taskset", "-c", row["affinity_cpus"], *argv]


def output_command(
    oracle: Path, output: Path, canonical: Path, affinity: str
) -> list[str]:
    return [
        "taskset",
        "-c",
        affinity,
        os.fspath(oracle),
        "--dag-pb",
        os.fspath(output),
        "--force-no-vcf",
        "--validate",
        "--dag-info",
        "--canonical-dag-result",
        os.fspath(canonical),
    ]


def tree_contract(workers: int, affinity: str, primary_uri: str) -> dict[str, str]:
    row = phase78.contract_row(
        TREE_PROFILE, TREE_FIXTURE, "on", workers, affinity, primary_uri
    )
    row.update(
        {
            "row_id": tree_row_id(workers),
            "run_group": SMALL_GROUP,
            "workload_name": "phase7-lazy-small-on",
        }
    )
    return row


def expected_source_contract(
    fixture: str, policy: str, workers: int, affinity: str
) -> dict[str, str]:
    if fixture == "tree0":
        row = phase78.contract_row(
            TREE_PROFILE,
            TREE_FIXTURE,
            policy,
            workers,
            affinity,
            f"repo://{TREE_FIXTURE.relative_path}",
        )
    elif fixture == "medium":
        row = phase78.contract_row(
            TREE_PROFILE,
            TREE_FIXTURE,
            policy,
            workers,
            affinity,
            f"repo://{MEDIUM_PRIMARY_RELATIVE}",
        )
        row.update(
            {
                "input_kind": "tree_pb_refseq",
                "primary_uri": f"repo://{MEDIUM_PRIMARY_RELATIVE}",
                "primary_sha256": MEDIUM_PRIMARY_SHA256,
                "refseq_uri": f"repo://{MEDIUM_REFSEQ_RELATIVE}",
                "refseq_sha256": MEDIUM_REFSEQ_SHA256,
            }
        )
    else:
        raise AssertionError(fixture)
    return row


def resolve_pair(manifest: core.Manifest, row: Mapping[str, str], root: Path) -> tuple[Path, Path]:
    sidecar = core.resolve_manifest_uri(
        manifest.path, row["canonical_sidecar_uri"], root
    )
    compact = core.resolve_manifest_uri(manifest.path, row["oracle_report_uri"], root)
    return sidecar, compact


def validate_contract_record(
    records: Sequence[Mapping[str, object]], label: str
) -> None:
    contracts = [item for item in records if item.get("record") == "contract"]
    if len(contracts) != 1:
        fail(f"{label}: canonical stream does not have exactly one contract")
    wanted: Mapping[str, object] = {
        "acceptance": TREE_PROFILE.acceptance,
        "objective": TREE_PROFILE.objective,
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "topology_selection": "none",
        "commit_mode": "overlay_delta",
        "verification_mode": "transient",
        "candidate_cap_semantics": "post_dedup",
        "max_iterations": 1,
        "max_candidates": 64,
        "top_k_exact": 0,
        "seed": 1,
        "polytomy_max_shapes": 1,
        "score_ua_edge": False,
        "use_bound_pruning": True,
        "require_exact_keep_mask": True,
        "randomize_order": False,
        "reservoir_sample": False,
        "include_immediate_reversals": False,
    }
    for key, expected in wanted.items():
        if contracts[0].get(key) != expected:
            fail(
                f"{label}: canonical contract {key}={contracts[0].get(key)!r}, "
                f"expected {expected!r}"
            )


def validate_source_row(
    base: core.Manifest,
    root: Path,
    fixture: str,
    policy: str,
    workers: int,
    affinity: str,
) -> SourceEvidence:
    wanted_id, wanted_group = SOURCE_ROW_IDS[(fixture, policy, workers)]
    matches = [row for row in base.rows if row["row_id"] == wanted_id]
    if len(matches) != 1:
        fail(f"sealed base lacks exact source row {wanted_id}")
    row = matches[0]
    if row["run_group"] != wanted_group:
        fail(f"sealed base source row {wanted_id} moved from {wanted_group}")
    contract = expected_source_contract(fixture, policy, workers, affinity)
    differing = [
        field
        for field in phase0.RESOLUTION_FIELDS
        if row[field] != contract[field]
    ]
    if differing:
        fail(
            f"sealed base source row {wanted_id} changes its resolution contract: "
            + ", ".join(differing)
        )
    if row["expected_outcome"] != "ok" or row["expected_timeout_trials"] != "0":
        fail(f"sealed base source row {wanted_id} is not a successful oracle row")
    if row["expected_candidates_generated"] != "64" or row[
        "expected_candidates_scored"
    ] != "64":
        fail(f"sealed base source row {wanted_id} does not freeze 64 candidates")
    if row["expected_exact_verifications"] != "0" or row[
        "expected_iterations"
    ] != "1":
        fail(f"sealed base source row {wanted_id} changes Phase-7 work")
    if row["expected_accepted_moves"] != "0":
        fail(f"sealed base source row {wanted_id} unexpectedly accepted a move")
    wanted_cache = "lazy_multisite_chart" if policy == "on" else "all_active_patterns"
    if row["expected_cache_strategy"] != wanted_cache:
        fail(f"sealed base source row {wanted_id} did not resolve forced {policy}")

    sidecar, compact = resolve_pair(base, row, root)
    common = Path(os.path.commonpath((sidecar.parent, compact.parent)))
    try:
        phase0.validate_canonical_sidecar_source(
            common,
            (
                sidecar.relative_to(common).as_posix(),
                compact.relative_to(common).as_posix(),
            ),
            wanted_id,
        )
    except phase0.BootstrapError as error:
        fail(str(error))
    records = phase78.sidecar_records(sidecar)
    validate_contract_record(records, wanted_id)
    phase78.validate_profile_canonical_stream(TREE_PROFILE, records, row, wanted_id)
    compact_value = core.read_json_object(compact, f"{wanted_id} compact canonical")
    if compact_value.get("semantic_sha256") != row["oracle_search_semantic_sha256"]:
        fail(f"sealed base source row {wanted_id} compact search hash changed")
    if core.sha256_file(sidecar) != row["canonical_sidecar_sha256"] or core.sha256_file(
        compact
    ) != row["oracle_report_sha256"]:
        fail(f"sealed base source row {wanted_id} canonical assets changed")
    argv_sha = core.canonical_argv_digest(canonical_argv(row))
    if argv_sha != row["canonical_argv_sha256"]:
        fail(f"sealed base source row {wanted_id} canonical argv changed")
    trial_sha = core.trial_digest(
        row["method"],
        row["oracle_search_semantic_sha256"],
        row["oracle_output_semantic_sha256"],
        argv_sha,
    )
    if trial_sha != row["oracle_trial_semantic_sha256"]:
        fail(f"sealed base source row {wanted_id} trial digest changed")
    return SourceEvidence(fixture, policy, workers, row, sidecar, compact)


def validate_source_matrix(
    base: core.Manifest, root: Path, affinity: str
) -> dict[tuple[str, str, int], SourceEvidence]:
    result = {
        key: validate_source_row(base, root, *key, affinity)
        for key in SOURCE_ROW_IDS
    }
    for fixture, policies in (("medium", ("off", "on")), ("tree0", ("off",))):
        del policies  # documents the closed fixture domain below
        for workers in WORKERS:
            if fixture == "medium":
                off = result[(fixture, "off", workers)]
                on = result[(fixture, "on", workers)]
                for field in (
                    "oracle_search_semantic_sha256",
                    "oracle_output_semantic_sha256",
                    *EXPECTED_RESULT_FIELDS,
                ):
                    if off.row[field] != on.row[field]:
                        fail(
                            f"sealed medium forced branches disagree at W{workers}: {field}"
                        )
    return result


def source_rows_contract(
    source: Mapping[tuple[str, str, int], SourceEvidence]
) -> list[dict[str, object]]:
    return [
        {
            "fixture": fixture,
            "policy": policy,
            "workers": workers,
            "row_id": evidence.row["row_id"],
            "row_sha256": core.sha256_bytes(canonical_row_bytes(evidence.row)),
            "canonical_sidecar_sha256": core.sha256_file(evidence.sidecar),
            "oracle_report_sha256": core.sha256_file(evidence.compact),
        }
        for (fixture, policy, workers), evidence in sorted(source.items())
    ]


def capture_contract_bytes(
    base: core.Manifest,
    source: Mapping[tuple[str, str, int], SourceEvidence],
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
) -> bytes:
    return json_bytes(
        {
            "schema": CAPTURE_SCHEMA,
            "schema_version": CAPTURE_SCHEMA_VERSION,
            "base_manifest_sha256": base.sha256,
            "base_manifest_id": base.preamble["manifest_id"],
            "frozen_oracle_sha256": base.preamble[
                "frozen_oracle_dagutil_sha256"
            ],
            "process_metrics_sha256": process_metrics_sha256,
            "working_chart_sha256": working_chart_sha256,
            "affinity_cpus": affinity,
            "timeout_seconds": TIMEOUT_SECONDS,
            "rss_limit_bytes": RSS_LIMIT_BYTES,
            "memory_budget_bytes": MEMORY_BUDGET_BYTES,
            "tree_fixture": dataclasses.asdict(TREE_FIXTURE),
            "medium_fixture": {
                "primary_relative_path": MEDIUM_PRIMARY_RELATIVE,
                "primary_sha256": MEDIUM_PRIMARY_SHA256,
                "refseq_relative_path": MEDIUM_REFSEQ_RELATIVE,
                "refseq_sha256": MEDIUM_REFSEQ_SHA256,
            },
            "tree_oracle_rows": [tree_row_id(workers) for workers in WORKERS],
            "medium_auto_qualification_rows": [
                medium_row_id(workers) for workers in WORKERS
            ],
            "source_rows": source_rows_contract(source),
        }
    )


def completion_capture_identity(
    base: core.Manifest,
    source: Mapping[tuple[str, str, int], SourceEvidence],
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
) -> phase78.CaptureIdentity:
    contract = capture_contract_bytes(
        base, source, affinity, process_metrics_sha256, working_chart_sha256
    )
    return phase78.CaptureIdentity(
        capture_contract_sha256=core.sha256_bytes(contract),
        base_manifest_sha256=base.sha256,
        frozen_oracle_sha256=base.preamble["frozen_oracle_dagutil_sha256"],
        process_metrics_sha256=process_metrics_sha256,
    )


def initialize_capture(
    capture_dir: Path,
    base: core.Manifest,
    source: Mapping[tuple[str, str, int], SourceEvidence],
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
) -> Path:
    capture = capture_dir.absolute()
    if os.path.lexists(capture):
        core.require_lexical_directory(capture, "Phase-7 completion capture")
    else:
        parent = core.durable_mkdir_parents(
            capture.parent, "Phase-7 completion capture parent"
        )
        capture = parent / capture.name
        capture.mkdir(mode=0o755)
        core.fsync_directory(parent)
    contract = capture_contract_bytes(
        base, source, affinity, process_metrics_sha256, working_chart_sha256
    )
    phase78.ensure_exact_capture_file(
        capture / "capture-contract.json", contract, "completion capture contract"
    )
    phase78.ensure_exact_capture_file(
        capture / "capture-contract.json.sha256",
        core.seal_bytes("capture-contract.json", contract),
        "completion capture contract seal",
    )
    for name in ("inputs", "rows", "qualifications"):
        directory = capture / name
        if os.path.lexists(directory):
            core.require_lexical_directory(directory, f"capture {name}")
        else:
            directory.mkdir(mode=0o755)
            core.fsync_directory(capture)
    wanted = {
        "capture-contract.json",
        "capture-contract.json.sha256",
        "inputs",
        "rows",
        "qualifications",
    }
    observed = {path.name for path in capture.iterdir()}
    if observed != wanted:
        fail(f"completion capture has unexpected members: {sorted(observed - wanted)}")
    return capture


def report_value(report, key: str, label: str) -> str:
    try:
        return core.acceptance_module().require_top(report, key, label)
    except core.acceptance_module().AcceptanceError as error:
        fail(str(error))


def require_report(report, key: str, wanted: str, label: str) -> None:
    actual = report_value(report, key, label)
    if actual != wanted:
        fail(f"{label}: report {key}={actual!r}, expected {wanted!r}")


def report_unsigned(report, key: str, label: str) -> int:
    value = report_value(report, key, label)
    if re.fullmatch(r"0|[1-9][0-9]*", value) is None:
        fail(f"{label}: report {key} is not a canonical unsigned integer")
    return int(value)


def lazy_policy_index_hash(active_patterns: int, pilot_patterns: int) -> int:
    """Reproduce the frozen v1 midpoint-strata integer hash."""

    mask = (1 << 64) - 1
    value = 1469598103934665603
    base, remainder = divmod(active_patterns, pilot_patterns)
    for stratum in range(pilot_patterns):
        begin = stratum * base + min(stratum, remainder)
        width = base + (1 if stratum < remainder else 0)
        index = begin + (width - 1) // 2
        mixed = (
            index
            + 0x9E3779B97F4A7C15
            + ((value << 6) & mask)
            + (value >> 2)
        ) & mask
        value = (value ^ mixed) & mask
    return value


def validate_auto_policy_v1(
    report,
    workers: int,
    source_matrix: Mapping[tuple[str, str, int], SourceEvidence],
) -> str:
    """Independently resolve the complete frozen v1 integer auto policy."""

    label = medium_row_id(workers)
    require_report(report, "lazy_policy_version", "1", label)
    require_report(report, "lazy_policy_requested", "auto", label)
    require_report(report, "lazy_policy_frozen", "true", label)
    require_report(report, "lazy_policy_measurements_available", "true", label)
    require_report(report, "lazy_policy_pilot_runs", "1", label)

    integer_fields = (
        "lazy_policy_active_patterns",
        "lazy_policy_pilot_patterns",
        "lazy_policy_pilot_pattern_index_hash",
        "lazy_policy_pilot_inside_chart_builds",
        "lazy_policy_pilot_outside_chart_builds",
        "lazy_policy_pilot_exact_builds",
        "lazy_policy_pilot_scheduler_submissions",
        "lazy_policy_pilot_internal_structural_classes_max",
        "lazy_policy_pilot_structural_ratio_numerator",
        "lazy_policy_pilot_structural_ratio_denominator",
        "lazy_policy_pilot_inside_rows",
        "lazy_policy_pilot_dense_rows",
        "lazy_policy_pilot_row_ratio_numerator",
        "lazy_policy_pilot_row_ratio_denominator",
        "lazy_policy_pilot_estimated_allocation_bytes",
        "lazy_policy_estimated_lazy_cache_bytes",
        "lazy_policy_estimated_dense_cache_bytes",
        "lazy_policy_pilot_key_words",
        "lazy_policy_pilot_dense_row_work",
        "lazy_policy_frozen_reuses",
    )
    values = {key: report_unsigned(report, key, label) for key in integer_fields}
    active = values["lazy_policy_active_patterns"]
    expected_source = source_matrix[("medium", "off", workers)].row
    if active != int(expected_source["expected_active_patterns"]):
        fail(f"{label}: auto-policy active-pattern count differs from sealed source")
    if values["lazy_policy_estimated_lazy_cache_bytes"] == 0 or values[
        "lazy_policy_estimated_dense_cache_bytes"
    ] == 0:
        fail(f"{label}: auto-policy cache byte estimates must be positive")

    zero_after_preflight = (
        "lazy_policy_pilot_patterns",
        "lazy_policy_pilot_pattern_index_hash",
        "lazy_policy_pilot_inside_chart_builds",
        "lazy_policy_pilot_outside_chart_builds",
        "lazy_policy_pilot_exact_builds",
        "lazy_policy_pilot_scheduler_submissions",
        "lazy_policy_pilot_internal_structural_classes_max",
        "lazy_policy_pilot_structural_ratio_numerator",
        "lazy_policy_pilot_structural_ratio_denominator",
        "lazy_policy_pilot_inside_rows",
        "lazy_policy_pilot_dense_rows",
        "lazy_policy_pilot_row_ratio_numerator",
        "lazy_policy_pilot_row_ratio_denominator",
        "lazy_policy_pilot_key_words",
        "lazy_policy_pilot_dense_row_work",
    )
    if active == 0:
        wanted_reason = "no_active_patterns"
    elif values["lazy_policy_estimated_lazy_cache_bytes"] > MEMORY_BUDGET_BYTES:
        wanted_reason = "full_lazy_budget_exceeded"
    elif values["lazy_policy_pilot_estimated_allocation_bytes"] > MEMORY_BUDGET_BYTES:
        wanted_reason = "pilot_budget_exceeded"
    else:
        pilot = values["lazy_policy_pilot_patterns"]
        wanted_pilot = min(32, active)
        clades = int(expected_source["expected_initial_clades"])
        dense_rows = values["lazy_policy_pilot_dense_rows"]
        if (
            pilot != wanted_pilot
            or values["lazy_policy_pilot_pattern_index_hash"]
            != lazy_policy_index_hash(active, wanted_pilot)
            or values["lazy_policy_pilot_inside_chart_builds"] != 1
            or values["lazy_policy_pilot_outside_chart_builds"] != 0
            or values["lazy_policy_pilot_exact_builds"] != 0
            or values["lazy_policy_pilot_scheduler_submissions"] != 0
            or dense_rows != wanted_pilot * clades
            or values["lazy_policy_pilot_structural_ratio_numerator"]
            != values["lazy_policy_pilot_internal_structural_classes_max"]
            or values["lazy_policy_pilot_structural_ratio_denominator"] != pilot
            or values["lazy_policy_pilot_row_ratio_numerator"]
            != values["lazy_policy_pilot_inside_rows"]
            or values["lazy_policy_pilot_row_ratio_denominator"] != dense_rows
            or values["lazy_policy_pilot_dense_row_work"] != dense_rows
        ):
            fail(f"{label}: auto-policy pilot integer identities are inconsistent")
        internal = values["lazy_policy_pilot_internal_structural_classes_max"]
        inside_rows = values["lazy_policy_pilot_inside_rows"]
        key_words = values["lazy_policy_pilot_key_words"]
        if pilot == 0 or internal == 0:
            wanted_reason = "no_internal_structural_classes"
        elif internal > pilot // 3 and inside_rows > dense_rows // 8:
            wanted_reason = "structural_and_strong_row_ratios_exceeded"
        elif inside_rows > dense_rows // 2:
            wanted_reason = "row_ratio_above_one_half"
        elif key_words > dense_rows * 8:
            wanted_reason = "key_work_above_eight_dense_rows"
        else:
            wanted_reason = "compression_thresholds_and_budget_safe"

    if wanted_reason in (
        "no_active_patterns",
        "full_lazy_budget_exceeded",
        "pilot_budget_exceeded",
    ) and any(values[key] != 0 for key in zero_after_preflight):
        fail(f"{label}: auto-policy preflight rejection retains pilot measurements")
    resolved = (
        "on"
        if wanted_reason == "compression_thresholds_and_budget_safe"
        else "off"
    )
    require_report(report, "lazy_policy_reason", wanted_reason, label)
    require_report(report, "lazy_policy_resolved", resolved, label)
    return resolved


def qualification_status_bytes(
    directory: Path,
    workers: int,
    source: SourceEvidence,
    base_sha256: str,
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
    capture_identity: phase78.CaptureIdentity,
) -> bytes:
    auto_row = qualification_row(
        source,
        workers,
        affinity,
        "manifest://medium.pb.gz",
        "manifest://medium.refseq",
    )
    argv_sha256 = core.canonical_argv_digest(canonical_argv(auto_row))
    return json_bytes(
        {
            "schema": QUALIFICATION_STATUS_SCHEMA,
            "schema_version": QUALIFICATION_STATUS_VERSION,
            "capture_identity": dataclasses.asdict(capture_identity),
            "row_id": medium_row_id(workers),
            "workers": workers,
            "resolved_policy": source.policy,
            "source_row_id": source.row["row_id"],
            "source_row_sha256": core.sha256_bytes(canonical_row_bytes(source.row)),
            "base_manifest_sha256": base_sha256,
            "affinity_cpus": affinity,
            "process_metrics_sha256": process_metrics_sha256,
            "working_chart_sha256": working_chart_sha256,
            "canonical_argv_sha256": argv_sha256,
            "trial_semantic_sha256": core.trial_digest(
                auto_row["method"],
                source.row["oracle_search_semantic_sha256"],
                source.row["oracle_output_semantic_sha256"],
                argv_sha256,
            ),
            "files": phase78.file_hashes(directory, QUALIFICATION_FILES),
        }
    )


def qualification_row(
    source: SourceEvidence, workers: int, affinity: str, primary_uri: str, refseq_uri: str
) -> dict[str, str]:
    row = dict(source.row)
    row.update(
        {
            "row_id": medium_row_id(workers),
            "run_group": MEDIUM_GROUP,
            "workload_name": "phase7-lazy-medium-auto",
            "primary_uri": primary_uri,
            "refseq_uri": refseq_uri,
            "lazy_policy": "auto",
            "affinity_cpus": affinity,
        }
    )
    return row


def validate_qualification(
    directory: Path,
    workers: int,
    source_matrix: Mapping[tuple[str, str, int], SourceEvidence],
    base_sha256: str,
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
    capture_identity: phase78.CaptureIdentity,
    *,
    validate_status: bool = True,
) -> QualificationEvidence:
    directory = core.require_lexical_directory(directory, "medium auto qualification")
    expected_names = set(QUALIFICATION_FILES)
    if validate_status:
        expected_names.update(phase78.STATUS_FILES)
    observed_names = {path.name for path in directory.iterdir()}
    if observed_names != expected_names:
        fail(
            f"medium auto qualification closure changed: missing="
            f"{sorted(expected_names - observed_names)}, extra="
            f"{sorted(observed_names - expected_names)}"
        )
    for name in expected_names:
        path = directory / name
        core.require_regular(path, f"qualification {name}")
        if path.stat().st_nlink != 1:
            fail(f"qualification member is externally hard-linked: {path}")
    phase78.validate_capture_receipt(
        directory / "process-metrics.txt", "medium auto qualification receipt"
    )
    phase78.validate_capture_receipt(
        directory / "output-process-metrics.txt", "medium auto output receipt"
    )
    phase78.validate_canonical_companion(directory, medium_row_id(workers))
    acceptance = core.acceptance_module()
    try:
        report = acceptance.parse_report(directory / "report.txt")
        search = acceptance.validate_search_digest(directory / "canonical.json")
        output = acceptance.validate_dag_digest(directory / "output-canonical.json")
    except acceptance.AcceptanceError as error:
        fail(str(error))
    bindings = {
        "acceptance": TREE_PROFILE.acceptance,
        "objective": TREE_PROFILE.objective,
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "candidate_cap_semantics": "post-dedup",
        "topology_selector": "none",
        "requested_max_iterations": "1",
        "configured_max_candidates": "64",
        "top_k_exact_verify": "0",
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
        "lazy_policy": "auto",
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
        require_report(report, key, wanted, medium_row_id(workers))
    resolved = validate_auto_policy_v1(report, workers, source_matrix)
    source = source_matrix[("medium", resolved, workers)]
    wanted_cache = "lazy_multisite_chart" if resolved == "on" else "all_active_patterns"
    require_report(report, "cache_strategy", wanted_cache, medium_row_id(workers))

    records = phase78.sidecar_records(directory / "canonical.ndjson")
    validate_contract_record(records, medium_row_id(workers))
    phase78.validate_profile_canonical_stream(
        TREE_PROFILE, records, source.row, medium_row_id(workers)
    )
    sidecar_sha = core.sha256_file(directory / "canonical.ndjson")
    if search["semantic_sha256"] != sidecar_sha:
        fail(f"medium auto qualification W{workers} compact/full search disagree")
    if search["semantic_sha256"] != source.row["oracle_search_semantic_sha256"]:
        fail(f"medium auto qualification W{workers} search differs from forced {resolved}")
    if output["semantic_sha256"] != source.row["oracle_output_semantic_sha256"]:
        fail(f"medium auto qualification W{workers} output differs from forced {resolved}")
    report_fields = {
        "expected_refinement_exactness": "refinement_exactness",
        "expected_cache_strategy": "cache_strategy",
        "expected_effective_pattern_batch_size": "effective_pattern_batch_size",
        "expected_final_compaction_exactness": "final_compaction_exactness_kind",
        "expected_chain_exactness": "chain_per_accept_exactness_label",
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
    for manifest_field, report_field in report_fields.items():
        require_report(
            report,
            report_field,
            source.row[manifest_field],
            medium_row_id(workers),
        )
    if core.report_stop_reason(directory / "report.txt") != source.row[
        "expected_stop_reason"
    ]:
        fail(f"medium auto qualification W{workers} stop reason changed")
    if str(output["parsimony_min"]) != source.row["expected_validated_parsimony"]:
        fail(f"medium auto qualification W{workers} validated score changed")
    if validate_status:
        wanted_status = qualification_status_bytes(
            directory,
            workers,
            source,
            base_sha256,
            affinity,
            process_metrics_sha256,
            working_chart_sha256,
            capture_identity,
        )
        phase78.validate_status_file(
            directory / "status.json", wanted_status, "medium auto qualification"
        )
    return QualificationEvidence(workers, directory, resolved, source)


def capture_qualification(
    destination_root: Path,
    workers: int,
    source_matrix: Mapping[tuple[str, str, int], SourceEvidence],
    base_sha256: str,
    primary: Path,
    refseq: Path,
    working_chart: Path,
    oracle: Path,
    process_metrics: Path,
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
    capture_identity: phase78.CaptureIdentity,
    *,
    before_publish: Callable[[], None] | None = None,
) -> QualificationEvidence:
    destination = destination_root / medium_row_id(workers)
    staging = destination_root / f".{medium_row_id(workers)}.staging"
    if os.path.lexists(destination):
        return validate_qualification(
            destination,
            workers,
            source_matrix,
            base_sha256,
            affinity,
            process_metrics_sha256,
            working_chart_sha256,
            capture_identity,
        )
    phase78.remove_staging(staging)
    staging.mkdir(mode=0o755)
    core.fsync_directory(destination_root)
    # Off and on have the same defining contract except lazy policy and frozen
    # expected cache strategy.  Use off only to construct literal-auto argv;
    # the report decides which sealed branch becomes authoritative.
    provisional = qualification_row(
        source_matrix[("medium", "off", workers)],
        workers,
        affinity,
        "manifest://medium.pb.gz",
        "manifest://medium.refseq",
    )
    environment = {
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "TZ": "Europe/Sofia",
    }
    phase78.run_enforced_capture_command(
        process_metrics,
        actual_chart_command(working_chart, primary, refseq, provisional, staging),
        staging / "report.txt",
        staging / "stderr.txt",
        staging / "process-metrics.txt",
        environment,
        f"medium auto W{workers}",
    )
    phase78.run_enforced_capture_command(
        process_metrics,
        output_command(
            oracle,
            staging / "output.pb.gz",
            staging / "output-canonical.json",
            affinity,
        ),
        staging / "output-report.txt",
        staging / "output-stderr.txt",
        staging / "output-process-metrics.txt",
        environment,
        f"medium auto W{workers} output",
    )
    evidence = validate_qualification(
        staging,
        workers,
        source_matrix,
        base_sha256,
        affinity,
        process_metrics_sha256,
        working_chart_sha256,
        capture_identity,
        validate_status=False,
    )
    status = qualification_status_bytes(
        staging,
        workers,
        evidence.source,
        base_sha256,
        affinity,
        process_metrics_sha256,
        working_chart_sha256,
        capture_identity,
    )
    core.copy_bytes(staging / "status.json", status, 0o444)
    core.copy_bytes(
        staging / "status.json.sha256",
        core.seal_bytes("status.json", status),
        0o444,
    )
    phase78.seal_capture_directory(
        staging, destination, before_publish=before_publish
    )
    return validate_qualification(
        destination,
        workers,
        source_matrix,
        base_sha256,
        affinity,
        process_metrics_sha256,
        working_chart_sha256,
        capture_identity,
    )


def compare_tree_evidence(
    evidence: phase78.RowEvidence, source: SourceEvidence
) -> None:
    if evidence.search_semantic_sha256 != source.row[
        "oracle_search_semantic_sha256"
    ] or evidence.output_semantic_sha256 != source.row[
        "oracle_output_semantic_sha256"
    ]:
        fail(f"new tree0-on W{evidence.workers} semantics differ from sealed dense")
    for field in EXPECTED_RESULT_FIELDS:
        if evidence.expected[field] != source.row[field]:
            fail(
                f"new tree0-on W{evidence.workers} differs from sealed dense: {field}"
            )


def capture_all(
    capture_dir: Path,
    base: core.Manifest,
    source: Mapping[tuple[str, str, int], SourceEvidence],
    root: Path,
    oracle: Path,
    working_chart: Path,
    process_metrics: Path,
    affinity: str,
    process_metrics_sha256: str,
    working_chart_sha256: str,
) -> tuple[
    Path,
    phase78.InputEvidence,
    tuple[phase78.RowEvidence, ...],
    tuple[QualificationEvidence, ...],
]:
    capture = initialize_capture(
        capture_dir,
        base,
        source,
        affinity,
        process_metrics_sha256,
        working_chart_sha256,
    )
    identity = completion_capture_identity(
        base,
        source,
        affinity,
        process_metrics_sha256,
        working_chart_sha256,
    )
    tree_path = phase78.fixture_path(root, TREE_FIXTURE)
    tree_input = phase78.capture_input(
        TREE_PROFILE,
        TREE_FIXTURE,
        tree_path,
        oracle,
        process_metrics,
        affinity,
        capture / "inputs",
        identity,
    )
    tree_rows: list[phase78.RowEvidence] = []
    for workers in WORKERS:
        evidence = phase78.capture_row(
            TREE_PROFILE,
            TREE_FIXTURE,
            tree_path,
            "on",
            workers,
            tree_input,
            oracle,
            process_metrics,
            affinity,
            capture / "rows",
            identity,
        )
        compare_tree_evidence(evidence, source[("tree0", "off", workers)])
        tree_rows.append(evidence)
    medium = core.resolve_lexical_regular(
        root, Path(MEDIUM_PRIMARY_RELATIVE), "medium seedtree"
    )
    refseq = core.resolve_lexical_regular(
        root, Path(MEDIUM_REFSEQ_RELATIVE), "medium reference"
    )
    if core.sha256_file(medium) != MEDIUM_PRIMARY_SHA256 or core.sha256_file(
        refseq
    ) != MEDIUM_REFSEQ_SHA256:
        fail("tracked medium seedtree/reference hash changed")
    qualifications = tuple(
        capture_qualification(
            capture / "qualifications",
            workers,
            source,
            base.sha256,
            medium,
            refseq,
            working_chart,
            oracle,
            process_metrics,
            affinity,
            process_metrics_sha256,
            working_chart_sha256,
            identity,
        )
        for workers in WORKERS
    )
    closures = {
        "inputs": {TREE_FIXTURE.key},
        "rows": {tree_row_id(workers) for workers in WORKERS},
        "qualifications": {medium_row_id(workers) for workers in WORKERS},
    }
    for name, wanted in closures.items():
        directory = capture / name
        observed = {path.name for path in directory.iterdir()}
        if observed != wanted:
            fail(
                f"completion capture {name} closure changed: "
                f"missing={sorted(wanted - observed)}, extra={sorted(observed - wanted)}"
            )
    return capture, tree_input, tuple(tree_rows), qualifications


def make_manifest_rows(
    source: Mapping[tuple[str, str, int], SourceEvidence],
    tree_evidence: Sequence[phase78.RowEvidence],
    qualifications: Sequence[QualificationEvidence],
    affinity: str,
    asset_name: str,
) -> list[dict[str, str]]:
    tree_by_worker = {item.workers: item for item in tree_evidence}
    qualification_by_worker = {item.workers: item for item in qualifications}
    if set(tree_by_worker) != set(WORKERS) or set(qualification_by_worker) != set(
        WORKERS
    ):
        fail("completion evidence is not exactly W1/2/4/8")
    rows: list[dict[str, str]] = []
    for workers in WORKERS:
        item = qualification_by_worker[workers]
        source_row = item.source.row
        row = qualification_row(
            item.source,
            workers,
            affinity,
            f"manifest://{asset_name}/fixtures/medium.pb.gz",
            f"manifest://{asset_name}/fixtures/medium.refseq",
        )
        prefix = f"provenance/base/medium-{item.resolved_policy}-w{workers}"
        row.update(
            {
                "canonical_sidecar_uri": f"manifest://{asset_name}/{prefix}/canonical.ndjson",
                "canonical_sidecar_sha256": source_row[
                    "canonical_sidecar_sha256"
                ],
                "oracle_report_uri": f"manifest://{asset_name}/{prefix}/canonical.json",
                "oracle_report_sha256": source_row["oracle_report_sha256"],
            }
        )
        argv_sha = core.canonical_argv_digest(canonical_argv(row))
        row["canonical_argv_sha256"] = argv_sha
        row["oracle_trial_semantic_sha256"] = core.trial_digest(
            row["method"],
            row["oracle_search_semantic_sha256"],
            row["oracle_output_semantic_sha256"],
            argv_sha,
        )
        rows.append(row)
    for workers in WORKERS:
        evidence = tree_by_worker[workers]
        row = tree_contract(
            workers,
            affinity,
            f"manifest://{asset_name}/fixtures/tree0.pb.gz",
        )
        row.update(dict(evidence.expected))
        prefix = f"provenance/rows/{tree_row_id(workers)}"
        row.update(
            {
                "oracle_search_semantic_sha256": evidence.search_semantic_sha256,
                "oracle_output_semantic_sha256": evidence.output_semantic_sha256,
                "canonical_sidecar_uri": f"manifest://{asset_name}/{prefix}/canonical.ndjson",
                "canonical_sidecar_sha256": evidence.sidecar_sha256,
                "oracle_report_uri": f"manifest://{asset_name}/{prefix}/canonical.json",
                "oracle_report_sha256": evidence.canonical_result_sha256,
            }
        )
        argv_sha = core.canonical_argv_digest(canonical_argv(row))
        row["canonical_argv_sha256"] = argv_sha
        row["oracle_trial_semantic_sha256"] = core.trial_digest(
            row["method"],
            row["oracle_search_semantic_sha256"],
            row["oracle_output_semantic_sha256"],
            argv_sha,
        )
        rows.append(row)
    return rows


def assert_no_resolution_collisions(
    base: core.Manifest, rows: Sequence[Mapping[str, str]]
) -> None:
    base_ids = {row["row_id"] for row in base.rows}
    new_ids = [row["row_id"] for row in rows]
    if len(new_ids) != len(set(new_ids)) or base_ids & set(new_ids):
        fail("completion supplement has a duplicate/overridden row ID")
    seen: dict[tuple[str, ...], str] = {}
    for row in (*base.rows, *rows):
        key = tuple(row[field] for field in phase0.RESOLUTION_FIELDS)
        previous = seen.get(key)
        if previous is not None:
            fail(
                f"completion row {row['row_id']} collides with resolver row {previous}"
            )
        seen[key] = row["row_id"]


def add_asset(files: dict[str, bytes], relative: str, path: Path) -> None:
    phase78.add_asset(files, relative, path.read_bytes())


def asset_files(
    root: Path,
    capture: Path,
    source: Mapping[tuple[str, str, int], SourceEvidence],
) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    add_asset(files, "fixtures/tree0.pb.gz", root / TREE_FIXTURE.relative_path)
    add_asset(files, "fixtures/medium.pb.gz", root / MEDIUM_PRIMARY_RELATIVE)
    add_asset(files, "fixtures/medium.refseq", root / MEDIUM_REFSEQ_RELATIVE)
    for name in ("capture-contract.json", "capture-contract.json.sha256"):
        add_asset(files, f"provenance/{name}", capture / name)
    for name in (*phase78.INPUT_FILES, *phase78.STATUS_FILES):
        add_asset(
            files,
            f"provenance/inputs/tree0/{name}",
            capture / "inputs/tree0" / name,
        )
    for workers in WORKERS:
        name = tree_row_id(workers)
        for member in (*phase78.ROW_FILES, *phase78.STATUS_FILES):
            add_asset(
                files,
                f"provenance/rows/{name}/{member}",
                capture / "rows" / name / member,
            )
        qname = medium_row_id(workers)
        for member in (*QUALIFICATION_FILES, *phase78.STATUS_FILES):
            add_asset(
                files,
                f"provenance/qualifications/{qname}/{member}",
                capture / "qualifications" / qname / member,
            )
    for (fixture, policy, workers), evidence in sorted(source.items()):
        prefix = f"provenance/base/{fixture}-{policy}-w{workers}"
        phase78.add_asset(files, f"{prefix}/row.json", canonical_row_bytes(evidence.row))
        add_asset(files, f"{prefix}/canonical.ndjson", evidence.sidecar)
        add_asset(files, f"{prefix}/canonical.json", evidence.compact)
    return files


def expected_archive_members() -> set[str]:
    result = {
        "fixtures/tree0.pb.gz",
        "fixtures/medium.pb.gz",
        "fixtures/medium.refseq",
        "provenance/capture-contract.json",
        "provenance/capture-contract.json.sha256",
    }
    result.update(
        f"provenance/inputs/tree0/{name}"
        for name in (*phase78.INPUT_FILES, *phase78.STATUS_FILES)
    )
    for workers in WORKERS:
        result.update(
            f"provenance/rows/{tree_row_id(workers)}/{name}"
            for name in (*phase78.ROW_FILES, *phase78.STATUS_FILES)
        )
        result.update(
            f"provenance/qualifications/{medium_row_id(workers)}/{name}"
            for name in (*QUALIFICATION_FILES, *phase78.STATUS_FILES)
        )
    for fixture, policy, workers in SOURCE_ROW_IDS:
        prefix = f"provenance/base/{fixture}-{policy}-w{workers}"
        result.update(
            f"{prefix}/{name}" for name in ("row.json", "canonical.ndjson", "canonical.json")
        )
    return result


def shell_token(token: str) -> str:
    if token.startswith('"$') and token.endswith('"'):
        return token
    return shlex.quote(token)


def render_commands(
    rows: Sequence[Mapping[str, str]],
    ledger_sha256: str,
    oracle_sha256: str,
    working_chart_sha256: str,
    process_metrics_sha256: str,
) -> bytes:
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        'assets=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)',
        f"readonly expected_ledger_sha256={ledger_sha256}",
        f"readonly expected_oracle_sha256={oracle_sha256}",
        f"readonly expected_working_chart_sha256={working_chart_sha256}",
        f"readonly expected_process_metrics_sha256={process_metrics_sha256}",
        '[[ $(sha256sum "$assets/assets.sha256" | awk \'{print $1}\') == "$expected_ledger_sha256" ]] || { echo "completion asset ledger hash mismatch" >&2; exit 1; }',
        '(cd "$assets" && sha256sum --check --strict assets.sha256)',
        '[[ ${1:-} != --verify-only ]] || exit 0',
        'oracle=${WRIC_PHASE7_FROZEN_ORACLE:?set WRIC_PHASE7_FROZEN_ORACLE}',
        'working=${WRIC_PHASE7_WORKING_CHART:?set WRIC_PHASE7_WORKING_CHART}',
        '[[ $(sha256sum "$oracle" | awk \'{print $1}\') == "$expected_oracle_sha256" ]] || { echo "completion oracle hash mismatch" >&2; exit 1; }',
        '[[ $(sha256sum "$working" | awk \'{print $1}\') == "$expected_working_chart_sha256" ]] || { echo "completion working-chart hash mismatch" >&2; exit 1; }',
        'out=${1:?usage: commands.sh OUTPUT_DIRECTORY}',
        'mkdir -- "$out"',
        "",
    ]
    for row in rows:
        row_id = row["row_id"]
        binary = '"$working"' if row["lazy_policy"] == "auto" else '"$oracle"'
        argv = canonical_argv(row)
        replacements = {
            "@binary:working_chart": binary,
            f"@primary:{row['primary_sha256']}": (
                '"$assets/fixtures/medium.pb.gz"'
                if row["input_kind"] == "tree_pb_refseq"
                else '"$assets/fixtures/tree0.pb.gz"'
            ),
            "@output": f'"$out/{row_id}.pb.gz"',
        }
        if row["input_kind"] == "tree_pb_refseq":
            replacements[f"@refseq:{row['refseq_sha256']}"] = (
                '"$assets/fixtures/medium.refseq"'
            )
        argv = [replacements.get(token, token) for token in argv]
        output_flag = argv.index("-o")
        argv[output_flag:output_flag] = [
            "--chart-spr-canonical-result",
            f'"$out/{row_id}.canonical.json"',
            "--chart-spr-canonical-sidecar",
            f'"$out/{row_id}.canonical.ndjson"',
        ]
        command = ["taskset", "-c", row["affinity_cpus"], *argv]
        lines.append(
            f"# {row_id} canonical_argv_sha256={row['canonical_argv_sha256']}"
        )
        lines.append(
            " \\\n+  ".join(shell_token(token) for token in command)
            + f' >"$out/{row_id}.report.txt" 2>"$out/{row_id}.stderr.txt"'
        )
        lines.append("")
    return ("\n".join(lines) + "\n").encode("utf-8")


def read_capture_contract(
    asset_root: Path,
    base: core.Manifest,
    source: Mapping[tuple[str, str, int], SourceEvidence],
    expected_affinity: str,
    expected_process_metrics_sha256: str,
    expected_working_chart_sha256: str,
) -> phase78.CaptureIdentity:
    path = asset_root / "provenance/capture-contract.json"
    digest = core.verify_detached_seal(path)
    value = core.read_json_object(path, "completion capture contract")
    affinity = value.get("affinity_cpus")
    if affinity != expected_affinity:
        fail("completion capture affinity differs from the sealed source affinity")
    wanted = capture_contract_bytes(
        base,
        source,
        expected_affinity,
        expected_process_metrics_sha256,
        expected_working_chart_sha256,
    )
    if path.read_bytes() != wanted or digest != core.sha256_bytes(wanted):
        fail("completion capture contract differs from exact base/binary binding")
    return phase78.CaptureIdentity(
        capture_contract_sha256=digest,
        base_manifest_sha256=base.sha256,
        frozen_oracle_sha256=base.preamble["frozen_oracle_dagutil_sha256"],
        process_metrics_sha256=expected_process_metrics_sha256,
    )


def audit_archived_canonical_identity(
    asset_root: Path,
    oracle: Path,
    process_metrics: Path,
    affinity: str,
) -> None:
    """Recompute every archived protobuf identity under the approved runner."""

    environment = {
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "PYTHONDONTWRITEBYTECODE": "1",
        "TZ": "Europe/Sofia",
    }
    with tempfile.TemporaryDirectory(
        prefix="wric-phase7-completion-canonical-audit-"
    ) as temporary:
        replay_root = Path(temporary)
        input_replay = replay_root / "tree0-input"
        input_replay.mkdir(mode=0o755)
        phase78.run_enforced_capture_command(
            process_metrics,
            core.actual_input_command(
                oracle,
                affinity,
                asset_root / "fixtures/tree0.pb.gz",
                input_replay,
            ),
            input_replay / "report.txt",
            input_replay / "stderr.txt",
            input_replay / "process-metrics.txt",
            environment,
            "completion archived tree0 input canonical audit",
        )
        archived_input = asset_root / "provenance/inputs/tree0/canonical.json"
        if (input_replay / "canonical.json").read_bytes() != archived_input.read_bytes():
            fail("archived tree0 input canonical identity differs from sealed oracle replay")

        output_directories = [
            asset_root / f"provenance/rows/{tree_row_id(workers)}"
            for workers in WORKERS
        ] + [
            asset_root / f"provenance/qualifications/{medium_row_id(workers)}"
            for workers in WORKERS
        ]
        for index, archived in enumerate(output_directories):
            replay = replay_root / f"output-{index}"
            replay.mkdir(mode=0o755)
            phase78.run_enforced_capture_command(
                process_metrics,
                output_command(
                    oracle,
                    archived / "output.pb.gz",
                    replay / "output-canonical.json",
                    affinity,
                ),
                replay / "output-report.txt",
                replay / "output-stderr.txt",
                replay / "output-process-metrics.txt",
                environment,
                f"completion archived output canonical audit {archived.name}",
            )
            if (replay / "output-canonical.json").read_bytes() != (
                archived / "output-canonical.json"
            ).read_bytes():
                fail(
                    "archived output protobuf canonical identity differs from "
                    f"sealed oracle replay: {archived.name}"
                )


def audit_archive(
    base: core.Manifest,
    supplement: core.Manifest,
    source: Mapping[tuple[str, str, int], SourceEvidence],
    root: Path,
    expected_affinity: str,
    expected_process_metrics_sha256: str,
    expected_working_chart_sha256: str,
    oracle: Path,
    process_metrics: Path,
) -> tuple[tuple[phase78.RowEvidence, ...], tuple[QualificationEvidence, ...]]:
    expected_asset_name = supplement.path.stem + ".assets"
    expected_commands_uri = f"manifest://{expected_asset_name}/commands.sh"
    if supplement.preamble["commands_uri"] != expected_commands_uri:
        fail("completion commands_uri leaves the exact supplement asset namespace")
    commands = core.resolve_manifest_uri(
        supplement.path, supplement.preamble["commands_uri"], root
    )
    expected_commands = core.resolve_lexical_regular(
        supplement.path.parent,
        Path(expected_asset_name) / "commands.sh",
        "exact completion commands asset",
    )
    if commands != expected_commands:
        fail("completion commands asset is not in the exact supplement namespace")
    asset_root = core.require_lexical_directory(
        commands.parent, "completion supplement assets"
    )
    ledger = core.resolve_lexical_regular(
        asset_root, Path("assets.sha256"), "completion asset ledger"
    )
    members = phase78.parse_asset_ledger(ledger)
    wanted_members = expected_archive_members()
    if set(members) != wanted_members:
        fail(
            "completion asset ledger is not exact: "
            f"missing={sorted(wanted_members - set(members))}, "
            f"extra={sorted(set(members) - wanted_members)}"
        )
    for relative, digest in members.items():
        member = core.resolve_lexical_regular(
            asset_root, Path(relative), f"completion asset {relative}"
        )
        if member.stat().st_nlink != 1 or core.sha256_file(member) != digest:
            fail(f"completion asset changed: {relative}")
    core.audit_exact_asset_tree(asset_root, wanted_members | {"assets.sha256", "commands.sh"})
    identity = read_capture_contract(
        asset_root,
        base,
        source,
        expected_affinity,
        expected_process_metrics_sha256,
        expected_working_chart_sha256,
    )
    fixture_hashes = {
        "fixtures/tree0.pb.gz": TREE_FIXTURE.sha256,
        "fixtures/medium.pb.gz": MEDIUM_PRIMARY_SHA256,
        "fixtures/medium.refseq": MEDIUM_REFSEQ_SHA256,
    }
    for relative, digest in fixture_hashes.items():
        if core.sha256_file(asset_root / relative) != digest:
            fail(f"archived completion fixture changed: {relative}")
    for (fixture, policy, workers), evidence in sorted(source.items()):
        prefix = asset_root / f"provenance/base/{fixture}-{policy}-w{workers}"
        if (prefix / "row.json").read_bytes() != canonical_row_bytes(evidence.row):
            fail(f"archived source row changed: {evidence.row['row_id']}")
        if (prefix / "canonical.ndjson").read_bytes() != evidence.sidecar.read_bytes():
            fail(f"archived source sidecar changed: {evidence.row['row_id']}")
        if (prefix / "canonical.json").read_bytes() != evidence.compact.read_bytes():
            fail(f"archived source compact report changed: {evidence.row['row_id']}")
    tree_input = phase78.parse_input_evidence(
        TREE_FIXTURE, asset_root / "provenance/inputs/tree0", identity
    )
    tree_rows: list[phase78.RowEvidence] = []
    qualifications: list[QualificationEvidence] = []
    for workers in WORKERS:
        tree = phase78.parse_row_evidence(
            TREE_PROFILE,
            TREE_FIXTURE,
            "on",
            workers,
            asset_root / f"provenance/rows/{tree_row_id(workers)}",
            tree_input,
            expected_affinity,
            identity,
        )
        compare_tree_evidence(tree, source[("tree0", "off", workers)])
        tree_rows.append(tree)
        qualifications.append(
            validate_qualification(
                asset_root
                / f"provenance/qualifications/{medium_row_id(workers)}",
                workers,
                source,
                base.sha256,
                expected_affinity,
                expected_process_metrics_sha256,
                expected_working_chart_sha256,
                identity,
            )
        )
    audit_archived_canonical_identity(
        asset_root, oracle, process_metrics, expected_affinity
    )
    rows = make_manifest_rows(
        source,
        tree_rows,
        qualifications,
        expected_affinity,
        supplement.path.stem + ".assets",
    )
    wanted_commands = render_commands(
        rows,
        core.sha256_file(ledger),
        base.preamble["frozen_oracle_dagutil_sha256"],
        expected_working_chart_sha256,
        expected_process_metrics_sha256,
    )
    if commands.read_bytes() != wanted_commands or not commands.stat().st_mode & 0o111:
        fail("completion commands asset changed")
    return tuple(tree_rows), tuple(qualifications)


def audit_supplement(
    base_manifest_path: Path,
    expected_parent_sha256: str,
    supplement_path: Path,
    root: Path,
    expected_process_metrics_sha256: str,
    expected_working_chart_sha256: str,
    process_metrics_path: Path,
) -> AuditedBundle:
    core.validate_hash(expected_parent_sha256, "expected parent SHA-256")
    core.validate_hash(expected_process_metrics_sha256, "process-metrics SHA-256")
    core.validate_hash(expected_working_chart_sha256, "working-chart SHA-256")
    try:
        base = core.read_manifest(base_manifest_path, root, expected_kind="base")
        supplement = core.read_manifest(supplement_path, root, expected_kind="supplement")
    except core.BootstrapError as error:
        fail(str(error))
    if base.sha256 != expected_parent_sha256 or base.preamble["parent_sha256"] != "-":
        fail("sealed base is not the exact expected Phase-0 root")
    process_metrics, runner_sha = validate_executable(
        process_metrics_path,
        expected_process_metrics_sha256,
        "process-metrics runner",
    )
    oracle_path = core.resolve_manifest_uri(
        base.path, base.preamble["frozen_oracle_dagutil_uri"], root
    )
    oracle, oracle_sha = validate_executable(
        oracle_path,
        base.preamble["frozen_oracle_dagutil_sha256"],
        "sealed frozen oracle",
    )
    if supplement.path.name != OUTPUT_NAME:
        fail(f"completion supplement basename must be {OUTPUT_NAME}")
    if supplement.preamble["manifest_id"] != MANIFEST_ID:
        fail(f"completion supplement manifest_id must be {MANIFEST_ID}")
    if supplement.preamble["parent_sha256"] != base.sha256:
        fail("completion supplement parent hash differs from sealed base")
    for key in (
        "schema",
        "schema_version",
        "repo_revision",
        "merge_base",
        "frozen_larch2_sha256",
        "frozen_oracle_dagutil_sha256",
    ):
        if supplement.preamble[key] != base.preamble[key]:
            fail(f"completion supplement changes inherited role {key}")
    for key in ("frozen_larch2_uri", "frozen_oracle_dagutil_uri"):
        if core.resolve_manifest_uri(supplement.path, supplement.preamble[key], root) != core.resolve_manifest_uri(
            base.path, base.preamble[key], root
        ):
            fail(f"completion supplement changes inherited role {key}")
    # Affinity is in every exact source contract; discover it only after the
    # base rows are present, then revalidate the whole matrix with that value.
    source_ids = {row["row_id"]: row for row in base.rows}
    seed_id = SOURCE_ROW_IDS[("medium", "off", 1)][0]
    if seed_id not in source_ids:
        fail(f"sealed base lacks exact source row {seed_id}")
    affinity = source_ids[seed_id]["affinity_cpus"]
    core.validate_affinity(affinity, "completion source")
    source = validate_source_matrix(base, root, affinity)
    tree_rows, qualifications = audit_archive(
        base,
        supplement,
        source,
        root,
        affinity,
        expected_process_metrics_sha256,
        expected_working_chart_sha256,
        oracle,
        process_metrics,
    )
    expected_rows = make_manifest_rows(
        source,
        tree_rows,
        qualifications,
        affinity,
        supplement.path.stem + ".assets",
    )
    if list(supplement.rows) != expected_rows:
        observed_ids = [row["row_id"] for row in supplement.rows]
        wanted_ids = [row["row_id"] for row in expected_rows]
        if observed_ids != wanted_ids:
            fail("completion supplement row order/set changed")
        for observed, wanted in zip(supplement.rows, expected_rows, strict=True):
            differing = [field for field in core.MANIFEST_HEADER if observed[field] != wanted[field]]
            if differing:
                fail(
                    f"completion row {observed['row_id']} differs in "
                    + ", ".join(differing)
                )
        fail("completion supplement rows differ from re-derived evidence")
    assert_no_resolution_collisions(base, expected_rows)
    if core.sha256_file(process_metrics) != runner_sha or core.sha256_file(
        oracle
    ) != oracle_sha:
        fail("runner/frozen oracle changed during completion canonical audit")
    return AuditedBundle(
        base,
        supplement,
        expected_process_metrics_sha256,
        expected_working_chart_sha256,
        affinity,
    )


def validate_with_harness(
    bundle: AuditedBundle,
    benchmark_harness: Path,
    process_metrics: Path,
    root: Path,
) -> None:
    harness = core.require_canonical_regular(
        benchmark_harness, "benchmark harness", executable=True
    )
    repository_harness = core.require_canonical_regular(
        TOOLS / "wric_spr_search_benchmark.sh",
        "repository benchmark harness",
        executable=True,
    )
    if harness != repository_harness:
        fail("--benchmark-harness is not the repository production harness")
    runner = core.require_canonical_regular(
        process_metrics, "process-metrics runner", executable=True
    )
    if runner.stat().st_nlink != 1 or core.sha256_file(runner) != bundle.process_metrics_sha256:
        fail("process-metrics runner differs from completion capture")
    oracle = core.resolve_manifest_uri(
        bundle.base.path, bundle.base.preamble["frozen_oracle_dagutil_uri"], root
    )
    larch2 = core.resolve_manifest_uri(
        bundle.base.path, bundle.base.preamble["frozen_larch2_uri"], root
    )
    sentinel = bundle.supplement.path.parent / (
        "." + bundle.supplement.path.name + ".harness-validation"
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
            os.fspath(runner),
            "--out-dir",
            os.fspath(sentinel),
            "--workload-manifest",
            os.fspath(bundle.base.path),
            "--supplemental-workload-manifest",
            os.fspath(bundle.supplement.path),
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
    expected = f"error: manifest group has no rows: {HARNESS_SENTINEL_GROUP}\n"
    if os.path.lexists(sentinel):
        fail("benchmark harness created output before sentinel validation")
    if result.returncode != 1 or result.stdout != "" or result.stderr != expected:
        fail("benchmark harness did not produce exact post-validation sentinel")


def validate_executable(path: Path, expected_sha256: str, label: str) -> tuple[Path, str]:
    core.validate_hash(expected_sha256, f"expected {label} SHA-256")
    executable = core.require_canonical_regular(path, label, executable=True)
    if executable.stat().st_nlink != 1:
        fail(f"{label} must not be externally hard-linked")
    digest = core.sha256_file(executable)
    if digest != expected_sha256:
        fail(f"{label} differs from its explicit expected SHA-256")
    return executable, digest


def validate_build_inputs(
    args: argparse.Namespace, root: Path
) -> tuple[
    core.Manifest,
    dict[tuple[str, str, int], SourceEvidence],
    Path,
    Path,
    Path,
    Path,
    Path,
    Path,
    str,
    str,
]:
    try:
        base = core.read_manifest(args.base_manifest, root, expected_kind="base")
    except core.BootstrapError as error:
        fail(str(error))
    core.validate_hash(args.expected_parent_sha256, "expected parent SHA-256")
    if base.sha256 != args.expected_parent_sha256 or base.preamble["parent_sha256"] != "-":
        fail("sealed base is not the exact expected Phase-0 root")
    core.validate_affinity(args.affinity_cpus, "Phase-7 completion")
    source = validate_source_matrix(base, root, args.affinity_cpus)
    tree = phase78.fixture_path(root, TREE_FIXTURE)
    medium = core.resolve_lexical_regular(root, Path(MEDIUM_PRIMARY_RELATIVE), "medium seedtree")
    refseq = core.resolve_lexical_regular(root, Path(MEDIUM_REFSEQ_RELATIVE), "medium reference")
    if core.sha256_file(medium) != MEDIUM_PRIMARY_SHA256 or core.sha256_file(refseq) != MEDIUM_REFSEQ_SHA256:
        fail("tracked medium seedtree/reference hash changed")
    oracle = core.resolve_manifest_uri(
        base.path, base.preamble["frozen_oracle_dagutil_uri"], root
    )
    larch2 = core.resolve_manifest_uri(
        base.path, base.preamble["frozen_larch2_uri"], root
    )
    core.require_regular(oracle, "frozen oracle", executable=True)
    core.require_regular(larch2, "frozen larch2", executable=True)
    runner, runner_sha = validate_executable(
        args.process_metrics,
        args.expected_process_metrics_sha256,
        "process-metrics runner",
    )
    working, working_sha = validate_executable(
        args.working_chart,
        args.expected_working_chart_sha256,
        "working chart",
    )
    return base, source, tree, medium, refseq, oracle, runner, working, runner_sha, working_sha


def build_locked(
    args: argparse.Namespace,
    publication: core.PublicationPaths,
    root: Path,
) -> None:
    (
        base,
        source,
        _tree,
        _medium,
        _refseq,
        oracle,
        runner,
        working,
        runner_sha,
        working_sha,
    ) = validate_build_inputs(args, root)
    capture_absolute = args.capture_dir.absolute()
    for reserved in dataclasses.astuple(publication):
        if (
            capture_absolute == reserved
            or capture_absolute.is_relative_to(reserved)
            or reserved.is_relative_to(capture_absolute)
        ):
            fail("--capture-dir overlaps supplement publication namespace")
    with core.exclusive_capture_lock(args.capture_dir):
        capture, _input, tree_rows, qualifications = capture_all(
            args.capture_dir,
            base,
            source,
            root,
            oracle,
            working,
            runner,
            args.affinity_cpus,
            runner_sha,
            working_sha,
        )
    if core.sha256_file(oracle) != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("frozen oracle changed during completion capture")
    if core.sha256_file(runner) != runner_sha or core.sha256_file(working) != working_sha:
        fail("runner/working chart changed during completion capture")
    files = asset_files(root, capture, source)
    ledger = phase78.asset_ledger_bytes(files)
    ledger_sha = core.sha256_bytes(ledger)
    files["assets.sha256"] = ledger
    rows = make_manifest_rows(
        source, tree_rows, qualifications, args.affinity_cpus, publication.assets.name
    )
    assert_no_resolution_collisions(base, rows)
    commands = render_commands(
        rows,
        ledger_sha,
        base.preamble["frozen_oracle_dagutil_sha256"],
        working_sha,
        runner_sha,
    )
    files["commands.sh"] = commands
    preamble = {
        "schema": core.SCHEMA,
        "schema_version": core.SCHEMA_VERSION,
        "kind": "supplement",
        "manifest_id": MANIFEST_ID,
        "parent_sha256": base.sha256,
        "repo_revision": base.preamble["repo_revision"],
        "merge_base": base.preamble["merge_base"],
        "frozen_larch2_uri": core.repo_uri(
            root,
            core.resolve_manifest_uri(
                base.path, base.preamble["frozen_larch2_uri"], root
            ),
        ),
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
        bundle = audit_supplement(
            args.base_manifest,
            args.expected_parent_sha256,
            staged_output,
            root,
            runner_sha,
            working_sha,
            runner,
        )
        validate_with_harness(bundle, args.benchmark_harness, runner, root)

    try:
        core.publish_immutable_supplement(
            publication,
            files,
            manifest_data,
            seal_data,
            ownership=ownership,
            prepublish_validator=validate_private,
        )
        bundle = audit_supplement(
            args.base_manifest,
            args.expected_parent_sha256,
            publication.output,
            root,
            runner_sha,
            working_sha,
            runner,
        )
        validate_with_harness(bundle, args.benchmark_harness, runner, root)
        core.finish_publication(publication)
    except BaseException as error:
        try:
            core.rollback_owned_publication(publication, ownership)
        except BaseException as rollback_error:
            fail(f"publication rollback failed after {error}: {rollback_error}")
        raise


def common_audit(args: argparse.Namespace, root: Path, supplement: Path) -> AuditedBundle:
    runner, runner_sha = validate_executable(
        args.process_metrics,
        args.expected_process_metrics_sha256,
        "process-metrics runner",
    )
    _working, working_sha = validate_executable(
        args.working_chart,
        args.expected_working_chart_sha256,
        "working chart",
    )
    bundle = audit_supplement(
        args.base_manifest,
        args.expected_parent_sha256,
        supplement,
        root,
        runner_sha,
        working_sha,
        runner,
    )
    validate_with_harness(bundle, args.benchmark_harness, runner, root)
    return bundle


def build(args: argparse.Namespace) -> None:
    root = core.repo_root(args.repo_root)
    output = args.output.absolute()
    if output.name != OUTPUT_NAME:
        fail(f"completion output basename must be {OUTPUT_NAME}")
    output_parent = core.durable_mkdir_parents(output.parent, "completion output parent")
    output = output_parent / output.name
    with core.exclusive_output_lock(output) as publication:

        def validate_private(staged_output: Path) -> None:
            common_audit(args, root, staged_output)

        try:
            recovered = core.recover_interrupted_publication(
                publication, prepublish_validator=validate_private
            )
        except core.BootstrapError as error:
            fail(str(error))
        if recovered:
            common_audit(args, root, publication.output)
            core.finish_publication(publication)
            return
        existing = [
            path
            for path in (publication.output, publication.assets, publication.seal)
            if os.path.lexists(path)
        ]
        if existing:
            fail(f"exclusive immutable output already exists: {existing[0]}")
        build_locked(args, publication, root)


def audit_command(args: argparse.Namespace) -> None:
    root = core.repo_root(args.repo_root)
    common_audit(args, root, args.supplement)


def plan_payload() -> dict[str, object]:
    rows = [
        {
            "row_id": medium_row_id(workers),
            "run_group": MEDIUM_GROUP,
            "fixture": MEDIUM_PRIMARY_RELATIVE,
            "lazy_policy": "auto",
            "workers": workers,
            "evidence": "current-auto qualification -> matching sealed P0 forced branch",
        }
        for workers in WORKERS
    ] + [
        {
            "row_id": tree_row_id(workers),
            "run_group": SMALL_GROUP,
            "fixture": TREE_FIXTURE.relative_path,
            "lazy_policy": "on",
            "workers": workers,
            "evidence": "new frozen-oracle capture checked against sealed P0 dense row",
        }
        for workers in WORKERS
    ]
    return {
        "schema": "wric_phase7_lazy_completion_plan",
        "schema_version": 1,
        "manifest_id": MANIFEST_ID,
        "required_output_basename": OUTPUT_NAME,
        "row_count": len(rows),
        "new_frozen_oracle_chart_capture_count": 4,
        "current_auto_qualification_count": 4,
        "rows": rows,
    }


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("plan", help="print the exact eight-row closure")
    for action in ("build", "audit"):
        command = commands.add_parser(action)
        command.add_argument("--base-manifest", type=Path, required=True)
        command.add_argument("--expected-parent-sha256", required=True)
        command.add_argument("--repo-root", type=Path)
        command.add_argument("--benchmark-harness", type=Path, required=True)
        command.add_argument("--process-metrics", type=Path, required=True)
        command.add_argument("--expected-process-metrics-sha256", required=True)
        command.add_argument("--working-chart", type=Path, required=True)
        command.add_argument("--expected-working-chart-sha256", required=True)
        if action == "build":
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
            print(json.dumps(plan_payload(), indent=2, sort_keys=True))
        elif args.command == "build":
            build(args)
        elif args.command == "audit":
            audit_command(args)
        else:
            raise AssertionError(args.command)
    except (
        CompletionError,
        phase78.Phase78Error,
        phase0.BootstrapError,
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
