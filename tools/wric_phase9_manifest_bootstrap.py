#!/usr/bin/env python3
"""Build and audit the immutable Phase-9 local-commit workload supplement.

The Phase-0 workload manifest is the root of trust.  This helper will not
characterize a workload, bless an unsealed input, or replace an existing
artifact.  It consumes an explicitly named and sealed base manifest plus a
sealed index of frozen-oracle evidence, copies that evidence into a
supplement-owned asset directory, derives the path-independent argv/trial
digests, and writes the exact detached supplement seal.

The frozen characterization has one row for every seed/worker combination.
Paths in it are normalized paths relative to the characterization TSV.  Its
exact schema can be printed with ``print-characterization-template``.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
from typing import Iterable, Mapping, NoReturn, Sequence


SCHEMA = "wric_chart_parallelization_workloads"
SCHEMA_VERSION = "1"
MANIFEST_ID = "phase9-local-commit"
RUN_GROUP = "phase9-local-commit"
METHOD = "chart_spr_grammar_exact"
SEEDS = (1, 7, 19)
WORKERS = (1, 2, 4, 8)
ITERATIONS = 3
MAX_CANDIDATES = 32
TOP_K_EXACT = 4
TIMEOUT_SECONDS = 600
RSS_LIMIT_BYTES = 16 * 1024**3
MEMORY_BUDGET_BYTES = 12 * 1024**3
MIN_ACTIVE_PATTERNS = 64
MIN_ACCEPTED_MOVES = 3
MIN_AFFECTED_ROWS_PER_ACCEPT = 32
MIN_FROZEN_W1_ACCEPTED_UPDATE_MS = 100.0

PREAMBLE_KEYS = (
    "schema",
    "schema_version",
    "kind",
    "manifest_id",
    "parent_sha256",
    "repo_revision",
    "merge_base",
    "frozen_larch2_uri",
    "frozen_larch2_sha256",
    "frozen_oracle_dagutil_uri",
    "frozen_oracle_dagutil_sha256",
    "commands_uri",
    "commands_sha256",
)

# This is schema version 1, copied deliberately rather than learned from the
# mutable live harness.  A schema change requires a reviewed helper update.
MANIFEST_HEADER = """row_id\trun_group\tworkload_name\tfixture_id\tmethod\tinput_kind\tprimary_uri\tprimary_sha256\tsecondary_uri\tsecondary_sha256\trefseq_uri\trefseq_sha256\tbinary_role\tworker_option\trequested_workers\texpected_resolved_workers\texpected_worker_policy\taffinity_cpus\ttimeout_seconds\trss_limit_bytes\texpected_outcome\texpected_timeout_trials\texpected_reason_code\texpected_reason_sha256\tscale_resource\tscale_limit\tscale_largest_candidates\tscale_largest_top_k\titerations\tseed\tnative_max_moves\tchart_max_candidates\tchart_top_k_exact\tcandidate_cap_semantics\tacceptance\tobjective\tcandidate_selection\tcandidate_source\ttopology_selector\trandomize_order\treservoir_sample\tinclude_immediate_reversals\tsampled_tree_count\tsampled_tree_radius\tsampled_tree_score_threshold\tmax_upward_path_expansions\tmax_path_pairs\tmin_moved_clade_size\tmax_moved_clade_size\tmin_target_clade_size\tmax_target_clade_size\tmax_affected_clades\tpolytomy_mode\tpolytomy_max_exact_arity\tpolytomy_max_shapes\tpolytomy_max_productions\tpolytomy_max_clades\tlazy_policy\tmax_cached_patterns\tpattern_batch_size\tcandidate_batch_size\tmemory_budget_bytes\tcommit_mode\tverification_mode\tlocal_accept_updates\tdominance_mode\tbound_pruning\trequire_exact_keep_mask\tmax_frontier_entries\tscore_ua_edge\tvalidate\tforce_no_vcf\texpected_refinement_exactness\texpected_cache_strategy\texpected_effective_pattern_batch_size\texpected_keep_mask_kind\texpected_final_compaction_exactness\texpected_chain_exactness\texpected_active_patterns\texpected_initial_clades\texpected_initial_productions\texpected_candidates_generated\texpected_candidates_scored\texpected_exact_verifications\texpected_stop_reason\texpected_iterations\texpected_accepted_moves\texpected_initial_score\texpected_final_score\texpected_validated_parsimony\toracle_search_semantic_sha256\toracle_output_semantic_sha256\toracle_trial_semantic_sha256\tcanonical_sidecar_uri\tcanonical_sidecar_sha256\toracle_report_uri\toracle_report_sha256\tcanonical_argv_sha256""" .split("\t")

CHAR_SCHEMA = "wric_phase9_frozen_characterization"
CHAR_PREAMBLE_KEYS = (
    "schema",
    "schema_version",
    "primary_sha256",
    "frozen_oracle_sha256",
    "affinity_cpus",
    "timeout_seconds",
    "rss_limit_bytes",
    "memory_budget_bytes",
)
CHAR_HEADER = (
    "seed",
    "workers",
    "product_report_path",
    "product_report_sha256",
    "canonical_sidecar_path",
    "canonical_sidecar_sha256",
    "canonical_result_path",
    "canonical_result_sha256",
    "output_canonical_path",
    "output_canonical_sha256",
)

HEX64 = re.compile(r"[0-9a-f]{64}")
REVISION = re.compile(r"[0-9a-f]{40,64}")
SAFE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*")
AFFINITY = re.compile(r"[0-9]+(?:[,-][0-9]+)*")


class BootstrapError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class Manifest:
    path: Path
    sha256: str
    preamble: Mapping[str, str]
    rows: tuple[Mapping[str, str], ...]


@dataclasses.dataclass(frozen=True)
class Characterization:
    path: Path
    sha256: str
    preamble: Mapping[str, str]
    rows: Mapping[tuple[int, int], Mapping[str, str]]


@dataclasses.dataclass(frozen=True)
class Evidence:
    seed: int
    workers: int
    product_report: Path
    product_report_sha256: str
    canonical_sidecar: Path
    canonical_sidecar_sha256: str
    canonical_result: Path
    canonical_result_sha256: str
    output_canonical: Path
    output_canonical_sha256: str
    search_semantic_sha256: str
    output_semantic_sha256: str
    expected: Mapping[str, str]


def fail(message: str) -> NoReturn:
    raise BootstrapError(message)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_regular(path: Path, label: str, *, executable: bool = False) -> None:
    try:
        info = path.lstat()
    except FileNotFoundError:
        fail(f"{label} is missing: {path}")
    if not stat.S_ISREG(info.st_mode) or path.is_symlink():
        fail(f"{label} must be a lexical regular file: {path}")
    if executable and not info.st_mode & 0o111:
        fail(f"{label} is not executable: {path}")


def detached_seal_bytes(path: Path) -> bytes:
    return f"{sha256_file(path)}  {path.name}\n".encode("ascii")


def verify_detached_seal(path: Path) -> str:
    require_regular(path, "sealed file")
    seal = path.with_name(path.name + ".sha256")
    require_regular(seal, "detached seal")
    expected = detached_seal_bytes(path)
    if seal.read_bytes() != expected:
        fail(f"detached seal is not the exact SHA-256/basename bytes: {seal}")
    return expected[:64].decode("ascii")


def parse_preamble_and_rows(
    path: Path,
    preamble_keys: Sequence[str],
    header: Sequence[str],
) -> tuple[dict[str, str], list[dict[str, str]]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        fail(f"TSV is not UTF-8: {path}: {error}")
    if len(lines) < len(preamble_keys) + 2:
        fail(f"TSV has no complete preamble/header/rows: {path}")
    preamble: dict[str, str] = {}
    for index, key in enumerate(preamble_keys):
        prefix = f"# {key}="
        line = lines[index]
        if not line.startswith(prefix) or not line[len(prefix) :]:
            fail(f"TSV preamble line {index + 1} must define exactly {key}: {path}")
        value = line[len(prefix) :]
        if "\t" in value or "\n" in value or key in preamble:
            fail(f"invalid preamble value for {key}: {path}")
        preamble[key] = value
    if tuple(lines[len(preamble_keys)].split("\t")) != tuple(header):
        fail(f"TSV header/schema mismatch: {path}")
    data_lines = lines[len(preamble_keys) :]
    if any(line.startswith("#") for line in data_lines[1:]):
        fail(f"comments after the TSV header are forbidden: {path}")
    reader = csv.DictReader(data_lines, dialect="excel-tab")
    rows: list[dict[str, str]] = []
    for line_number, row in enumerate(reader, len(preamble_keys) + 2):
        if None in row or set(row) != set(header) or any(
            value is None or value == "" for value in row.values()
        ):
            fail(f"TSV row {line_number} has extra/missing/empty cells: {path}")
        rows.append(dict(row))
    if not rows:
        fail(f"TSV contains no rows: {path}")
    return preamble, rows


def repo_root(path: Path | None) -> Path:
    if path is not None:
        root = path.resolve(strict=True)
    else:
        result = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            fail(f"cannot derive repository root: {result.stderr.strip()}")
        root = Path(result.stdout.strip()).resolve(strict=True)
    if not root.is_dir():
        fail(f"repository root is not a directory: {root}")
    return root


def resolve_manifest_uri(manifest: Path, uri: str, root: Path) -> Path:
    if uri.startswith("repo://"):
        base, relative = root, uri.removeprefix("repo://")
    elif uri.startswith("manifest://"):
        base, relative = manifest.parent.resolve(strict=True), uri.removeprefix("manifest://")
    else:
        fail(f"manifest URI has unsupported scheme: {uri}")
    rel = Path(relative)
    if (
        not relative
        or rel.is_absolute()
        or ".." in rel.parts
        or "." in rel.parts
        or "\\" in relative
        or "//" in relative
    ):
        fail(f"manifest URI is not normalized and confined: {uri}")
    try:
        candidate = (base / rel).resolve(strict=True)
        candidate.relative_to(base)
    except (FileNotFoundError, ValueError):
        fail(f"manifest URI is missing or escapes its root: {uri}")
    require_regular(candidate, f"manifest asset {uri}")
    return candidate


def repo_uri(root: Path, path: Path) -> str:
    resolved = path.resolve(strict=True)
    try:
        relative = resolved.relative_to(root)
    except ValueError:
        fail(f"base frozen asset is outside --repo-root: {path}")
    if path.absolute() != resolved:
        fail(f"base frozen asset uses a symlink or noncanonical path: {path}")
    return "repo://" + relative.as_posix()


def validate_hash(value: str, label: str) -> None:
    if HEX64.fullmatch(value) is None:
        fail(f"{label} is not canonical lowercase SHA-256: {value!r}")


def validate_manifest_assets(manifest: Manifest, root: Path) -> None:
    observed: dict[Path, str] = {}

    def require_hash(asset: Path, expected: str, label: str) -> None:
        actual = observed.get(asset)
        if actual is None:
            actual = sha256_file(asset)
            observed[asset] = actual
        if actual != expected:
            fail(f"{label} hash mismatch: {asset}")

    preamble_pairs = (
        ("frozen_larch2_uri", "frozen_larch2_sha256"),
        ("frozen_oracle_dagutil_uri", "frozen_oracle_dagutil_sha256"),
        ("commands_uri", "commands_sha256"),
    )
    for uri_key, sha_key in preamble_pairs:
        sha = manifest.preamble[sha_key]
        validate_hash(sha, f"manifest {sha_key}")
        asset = resolve_manifest_uri(manifest.path, manifest.preamble[uri_key], root)
        require_hash(asset, sha, "manifest preamble asset")
    row_pairs = (
        ("primary_uri", "primary_sha256"),
        ("secondary_uri", "secondary_sha256"),
        ("refseq_uri", "refseq_sha256"),
        ("canonical_sidecar_uri", "canonical_sidecar_sha256"),
        ("oracle_report_uri", "oracle_report_sha256"),
    )
    for row in manifest.rows:
        for uri_key, sha_key in row_pairs:
            uri, sha = row[uri_key], row[sha_key]
            if (uri == "-") != (sha == "-"):
                fail(f"manifest row {row['row_id']} has partial {uri_key}/{sha_key}")
            if uri == "-":
                continue
            validate_hash(sha, f"manifest row {row['row_id']} {sha_key}")
            asset = resolve_manifest_uri(manifest.path, uri, root)
            require_hash(asset, sha, f"manifest row {row['row_id']} asset")


def read_manifest(path: Path, root: Path, *, expected_kind: str) -> Manifest:
    digest = verify_detached_seal(path)
    preamble, rows = parse_preamble_and_rows(path, PREAMBLE_KEYS, MANIFEST_HEADER)
    if preamble["schema"] != SCHEMA or preamble["schema_version"] != SCHEMA_VERSION:
        fail(f"unsupported workload manifest schema: {path}")
    if preamble["kind"] != expected_kind:
        fail(f"workload manifest kind is not {expected_kind}: {path}")
    if SAFE_ID.fullmatch(preamble["manifest_id"]) is None:
        fail(f"invalid workload manifest ID: {path}")
    for key in ("repo_revision", "merge_base"):
        if REVISION.fullmatch(preamble[key]) is None:
            fail(f"manifest {key} is not a full revision: {path}")
    ids: set[str] = set()
    for row in rows:
        row_id = row["row_id"]
        if SAFE_ID.fullmatch(row_id) is None or row_id in ids:
            fail(f"empty/invalid/duplicate manifest row ID: {row_id!r}")
        ids.add(row_id)
    manifest = Manifest(path.resolve(strict=True), digest, preamble, tuple(rows))
    validate_manifest_assets(manifest, root)
    return manifest


def relative_evidence_path(characterization: Path, text: str, label: str) -> Path:
    relative = Path(text)
    if (
        not text
        or relative.is_absolute()
        or ".." in relative.parts
        or "." in relative.parts
        or "\\" in text
        or "//" in text
    ):
        fail(f"{label} is not a normalized characterization-relative path: {text!r}")
    root = characterization.parent.resolve(strict=True)
    try:
        path = (root / relative).resolve(strict=True)
        path.relative_to(root)
    except (FileNotFoundError, ValueError):
        fail(f"{label} is missing or escapes characterization root: {text!r}")
    require_regular(path, label)
    return path


def read_characterization(path: Path) -> Characterization:
    digest = verify_detached_seal(path)
    preamble, input_rows = parse_preamble_and_rows(path, CHAR_PREAMBLE_KEYS, CHAR_HEADER)
    if preamble["schema"] != CHAR_SCHEMA or preamble["schema_version"] != "1":
        fail(f"unsupported Phase-9 characterization schema: {path}")
    for key in ("primary_sha256", "frozen_oracle_sha256"):
        validate_hash(preamble[key], f"characterization {key}")
    if AFFINITY.fullmatch(preamble["affinity_cpus"]) is None:
        fail("characterization affinity_cpus is not canonical")
    exact_numbers = {
        "timeout_seconds": TIMEOUT_SECONDS,
        "rss_limit_bytes": RSS_LIMIT_BYTES,
        "memory_budget_bytes": MEMORY_BUDGET_BYTES,
    }
    for key, wanted in exact_numbers.items():
        if preamble[key] != str(wanted):
            fail(f"characterization {key} changed: {preamble[key]} != {wanted}")
    rows: dict[tuple[int, int], Mapping[str, str]] = {}
    for row in input_rows:
        try:
            key = (int(row["seed"]), int(row["workers"]))
        except ValueError:
            fail(f"characterization has non-integer seed/workers: {row}")
        if key in rows:
            fail(f"duplicate characterization seed/worker row: {key}")
        rows[key] = row
    wanted_keys = {(seed, workers) for seed in SEEDS for workers in WORKERS}
    if set(rows) != wanted_keys:
        fail(
            "characterization matrix is not exactly seeds 1/7/19 x workers "
            f"1/2/4/8: missing={sorted(wanted_keys - set(rows))}, "
            f"unexpected={sorted(set(rows) - wanted_keys)}"
        )
    return Characterization(path.resolve(strict=True), digest, preamble, rows)


def report_value(path: Path, key: str) -> str:
    pattern = re.compile(rf"^  {re.escape(key)}:\s*(.*?)\s*$")
    values = [
        match.group(1)
        for line in path.read_text(encoding="utf-8").splitlines()
        if (match := pattern.fullmatch(line)) is not None
    ]
    if len(values) != 1 or not values[0]:
        fail(f"product report has {len(values)} top-level values for {key}: {path}")
    return values[0]


def report_stop_reason(path: Path) -> str:
    iterations_text = report_value(path, "iterations")
    if not iterations_text.isdigit():
        fail(f"product report iterations is not unsigned: {path}")
    iterations = int(iterations_text)
    lines = path.read_text(encoding="utf-8").splitlines()
    headers = [index for index, line in enumerate(lines) if line == "  iteration_reports:"]
    if len(headers) != 1:
        fail(f"product report has {len(headers)} iteration_reports sections: {path}")
    section = lines[headers[0] + 1 :]
    for index, line in enumerate(section):
        if line.startswith("  ") and not line.startswith("    "):
            section = section[:index]
            break
    item_pattern = re.compile(r"^    - iteration:\s*([0-9]+)\s*$")
    item_indices = [
        index for index, line in enumerate(section) if item_pattern.fullmatch(line)
    ]
    identities = [int(item_pattern.fullmatch(section[index]).group(1)) for index in item_indices]  # type: ignore[union-attr]
    if identities != list(range(iterations)):
        fail(f"product report iteration identities are not 0..{iterations - 1}: {path}")
    stop_pattern = re.compile(r"^        stop_reason:\s*(.*?)\s*$")
    reasons: list[str] = []
    for offset, item_start in enumerate(item_indices):
        item_end = item_indices[offset + 1] if offset + 1 < len(item_indices) else len(section)
        item = section[item_start + 1 : item_end]
        generation = [index for index, line in enumerate(item) if line == "      candidate_generation:"]
        if len(generation) != 1:
            fail(f"product report iteration has ambiguous candidate_generation: {path}")
        start = generation[0] + 1
        end = len(item)
        for index in range(start, len(item)):
            if item[index].startswith("      ") and not item[index].startswith("        "):
                end = index
                break
        values = [
            match.group(1)
            for line in item[start:end]
            if (match := stop_pattern.fullmatch(line)) is not None
        ]
        if len(values) != 1 or not values[0]:
            fail(f"product report iteration stop reason is ambiguous: {path}")
        reasons.append(values[0])
    if len(set(reasons)) != 1:
        fail(f"product report iteration stop reasons disagree: {reasons}: {path}")
    return reasons[0]


def read_json_object(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not one UTF-8 JSON object: {path}: {error}")
    if not isinstance(value, dict):
        fail(f"{label} is not a JSON object: {path}")
    return value


def sidecar_contract(path: Path) -> tuple[dict[str, object], str]:
    contracts: list[dict[str, object]] = []
    keep_kinds: set[str] = set()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        fail(f"canonical sidecar is not UTF-8: {path}: {error}")
    if not lines:
        fail(f"canonical sidecar is empty: {path}")
    for line_number, line in enumerate(lines, 1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            fail(f"canonical sidecar line {line_number} is invalid JSON: {path}: {error}")
        if not isinstance(record, dict):
            fail(f"canonical sidecar line {line_number} is not an object: {path}")
        if record.get("record") == "contract":
            contracts.append(record)
        if "keep_mask_kind" in record:
            keep_kinds.add(str(record["keep_mask_kind"]))
    if len(contracts) != 1 or len(keep_kinds) != 1:
        fail(
            "canonical sidecar must have one contract and one unanimous "
            f"keep-mask kind: {path}"
        )
    return contracts[0], next(iter(keep_kinds))


REPORT_BINDINGS = {
    "acceptance": "exact_multisite",
    "objective": "grammar_exact",
    "candidate_selection": "lower_bound_top_k",
    "candidate_source": "grammar",
    "candidate_cap_semantics": "post-dedup",
    "topology_selector": "none",
    "requested_max_iterations": str(ITERATIONS),
    "configured_max_candidates": str(MAX_CANDIDATES),
    "top_k_exact_verify": str(TOP_K_EXACT),
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
    "lazy_policy": "off",
    "max_cached_patterns": "0",
    "configured_pattern_batch_size": "0",
    "configured_candidate_batch_size": "0",
    "memory_budget_bytes": str(MEMORY_BUDGET_BYTES),
    "commit_mode": "overlay_delta",
    "verification_mode": "transient",
    "local_accept_updates": "true",
    "dominance_mode": "off",
    "bound_pruning": "true",
    "require_exact_keep_mask": "true",
    "max_frontier_entries": "0",
    "score_ua_edge": "false",
    "validate": "true",
    "force_no_vcf": "true",
}


def exact_evidence(characterization: Characterization, row: Mapping[str, str]) -> Evidence:
    seed, workers = int(row["seed"]), int(row["workers"])
    paths: dict[str, Path] = {}
    for prefix in (
        "product_report",
        "canonical_sidecar",
        "canonical_result",
        "output_canonical",
    ):
        expected = row[f"{prefix}_sha256"]
        validate_hash(expected, f"characterization {seed}/W{workers} {prefix} hash")
        path = relative_evidence_path(
            characterization.path, row[f"{prefix}_path"], f"{seed}/W{workers} {prefix}"
        )
        if sha256_file(path) != expected:
            fail(f"characterization evidence hash mismatch for {seed}/W{workers}: {path}")
        paths[prefix] = path

    report = paths["product_report"]
    for key, wanted in REPORT_BINDINGS.items():
        if report_value(report, key) != wanted:
            fail(f"frozen report {seed}/W{workers} changed {key} from {wanted}")
    report_exact = {
        "seed": str(seed),
        "chart_workers_requested": str(workers),
        "chart_workers_resolved": str(workers),
        "chart_worker_policy": "explicit",
        "iterations": str(ITERATIONS),
    }
    for key, wanted in report_exact.items():
        if report_value(report, key) != wanted:
            fail(f"frozen report {seed}/W{workers} changed {key} from {wanted}")

    expected_fields = {
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
        "expected_initial_score": "initial_score",
        "expected_final_score": "final_score",
    }
    expected = {
        manifest_key: report_value(report, report_key)
        for manifest_key, report_key in expected_fields.items()
    }
    expected["expected_stop_reason"] = report_stop_reason(report)
    unsigned_fields = tuple(expected_fields)
    for field in unsigned_fields:
        if field in (
            "expected_refinement_exactness",
            "expected_cache_strategy",
            "expected_final_compaction_exactness",
            "expected_chain_exactness",
        ):
            continue
        if not expected[field].isdigit():
            fail(f"frozen report {seed}/W{workers} {field} is not unsigned")
    if int(expected["expected_active_patterns"]) < MIN_ACTIVE_PATTERNS:
        fail(f"Phase-9 fixture has fewer than {MIN_ACTIVE_PATTERNS} active patterns")
    accepted = int(expected["expected_accepted_moves"])
    if accepted < MIN_ACCEPTED_MOVES:
        fail(f"Phase-9 characterization {seed}/W{workers} accepted only {accepted} moves")
    inside = report_value(report, "inside_rows_recomputed_on_commit")
    outside = report_value(report, "outside_rows_recomputed_on_commit")
    if not inside.isdigit() or not outside.isdigit():
        fail(f"Phase-9 affected-row evidence is not unsigned: {seed}/W{workers}")
    if int(inside) + int(outside) < accepted * MIN_AFFECTED_ROWS_PER_ACCEPT:
        fail(f"Phase-9 characterization lacks 32 affected rows per accept: {seed}/W{workers}")
    accepted_ms = report_value(report, "accepted_rebuild_ms")
    try:
        accepted_ms_value = float(accepted_ms)
    except ValueError:
        fail(f"Phase-9 accepted_rebuild_ms is not numeric: {seed}/W{workers}")
    if accepted_ms_value < 0:
        fail(f"Phase-9 accepted_rebuild_ms is negative: {seed}/W{workers}")
    if seed == 1 and workers == 1 and accepted_ms_value < MIN_FROZEN_W1_ACCEPTED_UPDATE_MS:
        fail(
            "frozen seed-1 W1 accepted update is below the required 100 ms: "
            f"{accepted_ms_value}"
        )

    contract, keep_kind = sidecar_contract(paths["canonical_sidecar"])
    contract_checks: Mapping[str, object] = {
        "acceptance": "exact_multisite",
        "objective": "grammar_exact",
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "topology_selection": "none",
        "commit_mode": "overlay_delta",
        "accepted_state_update": "overlay_chain_local_commit",
        "verification_mode": "transient",
        "max_iterations": ITERATIONS,
        "max_candidates": MAX_CANDIDATES,
        "top_k_exact": TOP_K_EXACT,
        "seed": seed,
        "polytomy_max_shapes": 1,
        "refinement_exactness": expected["expected_refinement_exactness"],
    }
    for key, wanted in contract_checks.items():
        if contract.get(key) != wanted:
            fail(
                f"canonical sidecar contract {seed}/W{workers} {key}="
                f"{contract.get(key)!r}, expected {wanted!r}"
            )
    expected["expected_keep_mask_kind"] = keep_kind

    search = read_json_object(paths["canonical_result"], "canonical search result")
    search_semantic = str(search.get("semantic_sha256", ""))
    if (
        search.get("schema_version") != 1
        or search.get("digest_algorithm") != "sha256"
        or search_semantic != row["canonical_sidecar_sha256"]
    ):
        fail(f"compact/full canonical search mismatch: {seed}/W{workers}")
    output = read_json_object(paths["output_canonical"], "canonical output result")
    output_semantic = str(output.get("semantic_sha256", ""))
    if (
        output.get("schema") != "larch.dag.semantic_digest"
        or output.get("schema_version") != 1
        or output.get("digest_algorithm") != "sha256"
        or HEX64.fullmatch(output_semantic) is None
    ):
        fail(f"canonical output has an invalid semantic contract: {seed}/W{workers}")
    if str(output.get("parsimony_min", "")) != expected["expected_final_score"]:
        fail(f"canonical output score differs from report: {seed}/W{workers}")
    expected["expected_validated_parsimony"] = expected["expected_final_score"]

    return Evidence(
        seed,
        workers,
        paths["product_report"],
        row["product_report_sha256"],
        paths["canonical_sidecar"],
        row["canonical_sidecar_sha256"],
        paths["canonical_result"],
        row["canonical_result_sha256"],
        paths["output_canonical"],
        row["output_canonical_sha256"],
        search_semantic,
        output_semantic,
        expected,
    )


def validate_worker_independent_evidence(evidence: Sequence[Evidence]) -> None:
    for seed in SEEDS:
        rows = [item for item in evidence if item.seed == seed]
        signatures = {
            (
                item.search_semantic_sha256,
                item.output_semantic_sha256,
                tuple(sorted(item.expected.items())),
            )
            for item in rows
        }
        if len(rows) != len(WORKERS) or len(signatures) != 1:
            fail(f"frozen W1/2/4/8 semantics or expected results disagree for seed {seed}")


def canonical_argv(row: Mapping[str, str]) -> list[str]:
    argv = [
        "@binary:working_chart",
        "--dag-pb",
        f"@primary:{row['primary_sha256']}",
        "--validate",
        "--force-no-vcf",
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
    ]
    if row["max_frontier_entries"] != "0":
        argv += ["--chart-bnb-max-frontier", row["max_frontier_entries"]]
    if row["local_accept_updates"] == "true":
        argv.append("--chart-spr-local-accept-updates")
    if row["bound_pruning"] != "true":
        argv.append("--chart-bnb-no-bound-pruning")
    if row["require_exact_keep_mask"] != "true":
        argv.append("--chart-bnb-score-only")
    if row["score_ua_edge"] == "true":
        argv.append("--chart-score-ua-edge")
    argv += ["--seed", row["seed"]]
    if row["worker_option"] == "chart_spr_workers":
        argv += ["--chart-spr-workers", row["requested_workers"]]
    else:
        fail(f"Phase-9 row has unsupported worker option: {row['row_id']}")
    argv += ["-o", "@output"]
    return argv


def canonical_argv_digest(arguments: Sequence[str]) -> str:
    parts = [b"wric-canonical-argv-v1\n", f"argc={len(arguments)}\n".encode("ascii")]
    for argument in arguments:
        encoded = argument.encode("utf-8")
        if b"\n" in encoded:
            fail("canonical argv token contains a newline")
        parts.append(str(len(encoded)).encode("ascii") + b":" + encoded + b"\n")
    return sha256_bytes(b"".join(parts))


def trial_digest(method: str, search: str, output: str, argv: str) -> str:
    return sha256_bytes(
        (
            "wric-trial-semantic-v1\n"
            f"method={method}\n"
            f"search_semantic_sha256={search}\n"
            f"output_semantic_sha256={output}\n"
            f"canonical_argv_sha256={argv}\n"
        ).encode("utf-8")
    )


def row_id(seed: int, workers: int) -> str:
    return f"phase9-local-commit-seed{seed}-w{workers}"


def workload_name(seed: int) -> str:
    return f"phase9-three-accepts-seed{seed}"


def make_manifest_row(
    evidence: Evidence,
    primary_sha256: str,
    primary_uri: str,
    sidecar_uri: str,
    canonical_result_uri: str,
    affinity: str,
) -> dict[str, str]:
    row = {field: "-" for field in MANIFEST_HEADER}
    row.update(
        {
            "row_id": row_id(evidence.seed, evidence.workers),
            "run_group": RUN_GROUP,
            "workload_name": workload_name(evidence.seed),
            "fixture_id": "wric-chart-three-accepts",
            "method": METHOD,
            "input_kind": "dag_pb",
            "primary_uri": primary_uri,
            "primary_sha256": primary_sha256,
            "binary_role": "working_chart",
            "worker_option": "chart_spr_workers",
            "requested_workers": str(evidence.workers),
            "expected_resolved_workers": str(evidence.workers),
            "expected_worker_policy": "explicit",
            "affinity_cpus": affinity,
            "timeout_seconds": str(TIMEOUT_SECONDS),
            "rss_limit_bytes": str(RSS_LIMIT_BYTES),
            "expected_outcome": "ok",
            "expected_timeout_trials": "0",
            "iterations": str(ITERATIONS),
            "seed": str(evidence.seed),
            "chart_max_candidates": str(MAX_CANDIDATES),
            "chart_top_k_exact": str(TOP_K_EXACT),
            "candidate_cap_semantics": "post-dedup",
            "acceptance": "exact_multisite",
            "objective": "grammar_exact",
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": "grammar",
            "topology_selector": "none",
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
            "lazy_policy": "off",
            "max_cached_patterns": "0",
            "pattern_batch_size": "0",
            "candidate_batch_size": "0",
            "memory_budget_bytes": str(MEMORY_BUDGET_BYTES),
            "commit_mode": "overlay_delta",
            "verification_mode": "transient",
            "local_accept_updates": "true",
            "dominance_mode": "off",
            "bound_pruning": "true",
            "require_exact_keep_mask": "true",
            "max_frontier_entries": "0",
            "score_ua_edge": "false",
            "validate": "true",
            "force_no_vcf": "true",
            **evidence.expected,
            "oracle_search_semantic_sha256": evidence.search_semantic_sha256,
            "oracle_output_semantic_sha256": evidence.output_semantic_sha256,
            "canonical_sidecar_uri": sidecar_uri,
            "canonical_sidecar_sha256": evidence.canonical_sidecar_sha256,
            "oracle_report_uri": canonical_result_uri,
            "oracle_report_sha256": evidence.canonical_result_sha256,
        }
    )
    argv_sha = canonical_argv_digest(canonical_argv(row))
    row["canonical_argv_sha256"] = argv_sha
    row["oracle_trial_semantic_sha256"] = trial_digest(
        METHOD,
        evidence.search_semantic_sha256,
        evidence.output_semantic_sha256,
        argv_sha,
    )
    return row


def tsv_bytes(preamble: Mapping[str, str], rows: Sequence[Mapping[str, str]]) -> bytes:
    lines = [f"# {key}={preamble[key]}" for key in PREAMBLE_KEYS]
    lines.append("\t".join(MANIFEST_HEADER))
    for row in rows:
        lines.append("\t".join(row[field] for field in MANIFEST_HEADER))
    return ("\n".join(lines) + "\n").encode("utf-8")


def copy_bytes(path: Path, data: bytes, mode: int) -> None:
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


def shell_command_tokens(row: Mapping[str, str]) -> list[str]:
    values = canonical_argv(row)
    values[0] = '"$oracle"'
    values[2] = '"$assets/fixture.pb.gz"'
    values[-1] = f'"$out/{row["row_id"]}.pb.gz"'
    insert = len(values) - 2
    values[insert:insert] = [
        "--chart-spr-canonical-result",
        f'"$out/{row["row_id"]}.canonical.json"',
        "--chart-spr-canonical-sidecar",
        f'"$out/{row["row_id"]}.canonical.ndjson"',
    ]
    return values


def render_commands(
    rows: Sequence[Mapping[str, str]],
    oracle_sha: str,
    affinity: str,
    ledger_sha: str,
) -> bytes:
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        "assets=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd -P)",
        f"readonly expected_ledger_sha256={ledger_sha}",
        f"readonly expected_oracle_sha256={oracle_sha}",
        '[[ $(sha256sum "$assets/assets.sha256" | awk \'{print $1}\') == "$expected_ledger_sha256" ]] || { echo "Phase-9 asset ledger hash mismatch" >&2; exit 1; }',
        '(cd "$assets" && sha256sum --check --strict assets.sha256)',
        '[[ ${1:-} != --verify-only ]] || exit 0',
        'oracle=${WRIC_PHASE9_FROZEN_ORACLE:?set WRIC_PHASE9_FROZEN_ORACLE to the base-manifest oracle}',
        '[[ $(sha256sum "$oracle" | awk \'{print $1}\') == "$expected_oracle_sha256" ]] || { echo "Phase-9 oracle hash mismatch" >&2; exit 1; }',
        'out=${1:?usage: commands.sh OUTPUT_DIRECTORY}',
        'mkdir -- "$out"',
        "",
    ]
    for row in rows:
        lines.append(f"# {row['row_id']} canonical_argv_sha256={row['canonical_argv_sha256']}")
        command = ["taskset", "-c", affinity, *shell_command_tokens(row)]
        lines.append(" \\\n  ".join(shlex.quote(token) if not token.startswith('"$') else token for token in command) +
                     f' >"$out/{row["row_id"]}.report.txt" 2>"$out/{row["row_id"]}.stderr.txt"')
        output_command = (
            "taskset",
            "-c",
            affinity,
            '"$oracle"',
            "--dag-pb",
            f'"$out/{row["row_id"]}.pb.gz"',
            "--force-no-vcf",
            "--validate",
            "--dag-info",
            "--canonical-dag-result",
            f'"$out/{row["row_id"]}.output-canonical.json"',
        )
        lines.append(
            " \\\n  ".join(
                shlex.quote(token) if not token.startswith('"$') else token
                for token in output_command
            )
            + f' >"$out/{row["row_id"]}.output-report.txt"'
            + f' 2>"$out/{row["row_id"]}.output-stderr.txt"'
        )
        lines.append("")
    return ("\n".join(lines) + "\n").encode("utf-8")


def asset_ledger_bytes(files: Mapping[str, bytes]) -> bytes:
    return b"".join(
        f"{sha256_bytes(data)}  {name}\n".encode("ascii")
        for name, data in sorted(files.items())
    )


def expected_phase9_rows(rows: Iterable[Mapping[str, str]]) -> dict[tuple[int, int], Mapping[str, str]]:
    result: dict[tuple[int, int], Mapping[str, str]] = {}
    for row in rows:
        if row["run_group"] != RUN_GROUP:
            fail(f"supplement has a row outside {RUN_GROUP}: {row['row_id']}")
        try:
            key = (int(row["seed"]), int(row["requested_workers"]))
        except ValueError:
            fail(f"supplement has non-integer seed/worker: {row['row_id']}")
        if key in result:
            fail(f"supplement has duplicate seed/worker: {key}")
        result[key] = row
    wanted = {(seed, workers) for seed in SEEDS for workers in WORKERS}
    if set(result) != wanted:
        fail(f"supplement does not contain the exact Phase-9 matrix: {sorted(result)}")
    return result


def audit_phase9_rows(manifest: Manifest) -> None:
    rows = expected_phase9_rows(manifest.rows)
    fixed = {
        "method": METHOD,
        "input_kind": "dag_pb",
        "binary_role": "working_chart",
        "worker_option": "chart_spr_workers",
        "expected_worker_policy": "explicit",
        "expected_outcome": "ok",
        "expected_timeout_trials": "0",
        "iterations": str(ITERATIONS),
        "chart_max_candidates": str(MAX_CANDIDATES),
        "chart_top_k_exact": str(TOP_K_EXACT),
        "acceptance": "exact_multisite",
        "objective": "grammar_exact",
        "candidate_selection": "lower_bound_top_k",
        "candidate_source": "grammar",
        "local_accept_updates": "true",
        "memory_budget_bytes": str(MEMORY_BUDGET_BYTES),
    }
    primary_hashes: set[str] = set()
    affinities: set[str] = set()
    for (seed, workers), row in rows.items():
        if row["row_id"] != row_id(seed, workers):
            fail(f"supplement row ID is not canonical: {row['row_id']}")
        if row["workload_name"] != workload_name(seed):
            fail(f"supplement workload name breaks seed/worker pairing: {row['row_id']}")
        for key, wanted in fixed.items():
            if row[key] != wanted:
                fail(f"supplement row {row['row_id']} {key}={row[key]!r}, expected {wanted!r}")
        if row["expected_resolved_workers"] != str(workers):
            fail(f"supplement row worker resolution mismatch: {row['row_id']}")
        if int(row["expected_active_patterns"]) < MIN_ACTIVE_PATTERNS:
            fail(f"supplement row lacks active-pattern evidence: {row['row_id']}")
        if int(row["expected_accepted_moves"]) < MIN_ACCEPTED_MOVES:
            fail(f"supplement row lacks three accepted moves: {row['row_id']}")
        argv_sha = canonical_argv_digest(canonical_argv(row))
        if row["canonical_argv_sha256"] != argv_sha:
            fail(f"supplement row canonical argv hash mismatch: {row['row_id']}")
        trial_sha = trial_digest(
            row["method"],
            row["oracle_search_semantic_sha256"],
            row["oracle_output_semantic_sha256"],
            argv_sha,
        )
        if row["oracle_trial_semantic_sha256"] != trial_sha:
            fail(f"supplement row trial semantic hash mismatch: {row['row_id']}")
        primary_hashes.add(row["primary_sha256"])
        affinities.add(row["affinity_cpus"])
    if len(primary_hashes) != 1 or len(affinities) != 1:
        fail("supplement rows disagree on fixture or affinity")
    for seed in SEEDS:
        seed_rows = [rows[(seed, workers)] for workers in WORKERS]
        semantic_fields = (
            "oracle_search_semantic_sha256",
            "oracle_output_semantic_sha256",
            "expected_initial_score",
            "expected_final_score",
            "expected_validated_parsimony",
            "expected_candidates_generated",
            "expected_candidates_scored",
            "expected_exact_verifications",
            "expected_accepted_moves",
        )
        for field in semantic_fields:
            if len({row[field] for row in seed_rows}) != 1:
                fail(f"supplement seed {seed} workers disagree on {field}")


def audit_supplement_asset_ledger(manifest: Manifest, root: Path) -> None:
    commands = resolve_manifest_uri(
        manifest.path, manifest.preamble["commands_uri"], root
    )
    lines = commands.read_text(encoding="utf-8").splitlines()
    prefix = "readonly expected_ledger_sha256="
    declarations = [line.removeprefix(prefix) for line in lines if line.startswith(prefix)]
    if len(declarations) != 1 or HEX64.fullmatch(declarations[0]) is None:
        fail("Phase-9 commands asset lacks one canonical asset-ledger hash")
    ledger = commands.parent / "assets.sha256"
    require_regular(ledger, "Phase-9 asset ledger")
    if sha256_file(ledger) != declarations[0]:
        fail("Phase-9 asset ledger differs from the commands-bound hash")
    seen: set[str] = set()
    pattern = re.compile(r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)")
    for line_number, line in enumerate(ledger.read_text(encoding="ascii").splitlines(), 1):
        match = pattern.fullmatch(line)
        if match is None:
            fail(f"Phase-9 asset ledger line {line_number} is not canonical")
        digest, relative_text = match.groups()
        if relative_text in seen or relative_text in ("assets.sha256", "commands.sh"):
            fail(f"Phase-9 asset ledger has a duplicate/circular member: {relative_text}")
        seen.add(relative_text)
        relative = Path(relative_text)
        try:
            member = (commands.parent / relative).resolve(strict=True)
            member.relative_to(commands.parent.resolve(strict=True))
        except (FileNotFoundError, ValueError):
            fail(f"Phase-9 asset ledger member is missing or escapes: {relative_text}")
        require_regular(member, f"Phase-9 asset ledger member {relative_text}")
        if sha256_file(member) != digest:
            fail(f"Phase-9 asset ledger member hash mismatch: {relative_text}")
    required = {"fixture.pb.gz", "characterization.tsv", "characterization.tsv.sha256"}
    for seed in SEEDS:
        for workers in WORKERS:
            stem = row_id(seed, workers)
            required.update(
                {
                    f"evidence/{stem}.report.txt",
                    f"evidence/{stem}.canonical.ndjson",
                    f"evidence/{stem}.canonical.json",
                    f"evidence/{stem}.output-canonical.json",
                }
            )
    if seen != required:
        fail(
            "Phase-9 asset ledger is not the exact evidence closure: "
            f"missing={sorted(required - seen)}, unexpected={sorted(seen - required)}"
        )


def audit_supplement(
    base_path: Path,
    expected_parent: str,
    supplement_path: Path,
    root: Path,
) -> None:
    validate_hash(expected_parent, "expected parent SHA-256")
    base = read_manifest(base_path, root, expected_kind="base")
    if base.sha256 != expected_parent:
        fail(f"sealed base hash differs from --expected-parent-sha256: {base.sha256}")
    if base.preamble["parent_sha256"] != "-":
        fail("base workload manifest parent_sha256 is not '-'")
    supplement = read_manifest(supplement_path, root, expected_kind="supplement")
    if supplement.preamble["manifest_id"] != MANIFEST_ID:
        fail("Phase-9 supplement manifest_id changed")
    if supplement.preamble["parent_sha256"] != base.sha256:
        fail("Phase-9 supplement parent hash does not match the sealed base")
    for key in (
        "schema",
        "schema_version",
        "repo_revision",
        "merge_base",
        "frozen_larch2_sha256",
        "frozen_oracle_dagutil_sha256",
    ):
        if supplement.preamble[key] != base.preamble[key]:
            fail(f"Phase-9 supplement changes base preamble field {key}")
    base_ids = {row["row_id"] for row in base.rows}
    supplement_ids = {row["row_id"] for row in supplement.rows}
    overlap = base_ids & supplement_ids
    if overlap:
        fail(f"Phase-9 supplement overrides base row IDs: {sorted(overlap)}")
    audit_phase9_rows(supplement)
    audit_supplement_asset_ledger(supplement, root)


def build(args: argparse.Namespace) -> None:
    root = repo_root(args.repo_root)
    output = args.output.absolute()
    assets = output.with_name(output.stem + ".assets")
    seal = output.with_name(output.name + ".sha256")
    for path in (output, assets, seal):
        if os.path.lexists(path):
            fail(f"exclusive Phase-9 output already exists: {path}")
    validate_hash(args.expected_parent_sha256, "expected parent SHA-256")
    validate_hash(args.expected_fixture_sha256, "expected fixture SHA-256")
    base = read_manifest(args.base_manifest, root, expected_kind="base")
    if base.sha256 != args.expected_parent_sha256:
        fail(f"sealed base hash differs from --expected-parent-sha256: {base.sha256}")
    if base.preamble["parent_sha256"] != "-":
        fail("base workload manifest must declare parent_sha256=-")
    characterization = read_characterization(args.characterization)
    if characterization.preamble["frozen_oracle_sha256"] != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("characterization frozen oracle hash differs from sealed base role")
    fixture = args.fixture.resolve(strict=True)
    require_regular(fixture, "Phase-9 fixture")
    fixture_sha = sha256_file(fixture)
    if fixture_sha != args.expected_fixture_sha256:
        fail(f"Phase-9 fixture hash mismatch: {fixture_sha}")
    if characterization.preamble["primary_sha256"] != fixture_sha:
        fail("characterization primary hash differs from Phase-9 fixture")
    evidence = [
        exact_evidence(characterization, characterization.rows[(seed, workers)])
        for seed in SEEDS
        for workers in WORKERS
    ]
    validate_worker_independent_evidence(evidence)

    asset_files: dict[str, bytes] = {"fixture.pb.gz": fixture.read_bytes()}
    asset_files["characterization.tsv"] = characterization.path.read_bytes()
    asset_files["characterization.tsv.sha256"] = characterization.path.with_name(
        characterization.path.name + ".sha256"
    ).read_bytes()
    for item in evidence:
        stem = row_id(item.seed, item.workers)
        asset_files[f"evidence/{stem}.report.txt"] = item.product_report.read_bytes()
        asset_files[f"evidence/{stem}.canonical.ndjson"] = item.canonical_sidecar.read_bytes()
        asset_files[f"evidence/{stem}.canonical.json"] = item.canonical_result.read_bytes()
        asset_files[f"evidence/{stem}.output-canonical.json"] = item.output_canonical.read_bytes()
    ledger = asset_ledger_bytes(asset_files)
    ledger_sha = sha256_bytes(ledger)
    asset_files["assets.sha256"] = ledger

    asset_name = assets.name
    rows: list[dict[str, str]] = []
    for item in evidence:
        stem = row_id(item.seed, item.workers)
        rows.append(
            make_manifest_row(
                item,
                fixture_sha,
                f"manifest://{asset_name}/fixture.pb.gz",
                f"manifest://{asset_name}/evidence/{stem}.canonical.ndjson",
                f"manifest://{asset_name}/evidence/{stem}.canonical.json",
                characterization.preamble["affinity_cpus"],
            )
        )
    commands = render_commands(
        rows,
        base.preamble["frozen_oracle_dagutil_sha256"],
        characterization.preamble["affinity_cpus"],
        ledger_sha,
    )
    asset_files["commands.sh"] = commands

    frozen_larch2 = resolve_manifest_uri(base.path, base.preamble["frozen_larch2_uri"], root)
    frozen_oracle = resolve_manifest_uri(base.path, base.preamble["frozen_oracle_dagutil_uri"], root)
    preamble = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "kind": "supplement",
        "manifest_id": MANIFEST_ID,
        "parent_sha256": base.sha256,
        "repo_revision": base.preamble["repo_revision"],
        "merge_base": base.preamble["merge_base"],
        "frozen_larch2_uri": repo_uri(root, frozen_larch2),
        "frozen_larch2_sha256": base.preamble["frozen_larch2_sha256"],
        "frozen_oracle_dagutil_uri": repo_uri(root, frozen_oracle),
        "frozen_oracle_dagutil_sha256": base.preamble["frozen_oracle_dagutil_sha256"],
        "commands_uri": f"manifest://{asset_name}/commands.sh",
        "commands_sha256": sha256_bytes(commands),
    }
    manifest_data = tsv_bytes(preamble, rows)

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.parent / f".{assets.name}.staging.{os.getpid()}"
    if os.path.lexists(staging):
        fail(f"exclusive Phase-9 staging path already exists: {staging}")
    staging.mkdir(parents=False, mode=0o755)
    published_assets = False
    try:
        for relative, data in sorted(asset_files.items()):
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            copy_bytes(destination, data, 0o555 if relative == "commands.sh" else 0o444)
        os.rename(staging, assets)
        published_assets = True
        copy_bytes(output, manifest_data, 0o444)
        copy_bytes(seal, detached_seal_bytes(output), 0o444)
        audit_supplement(args.base_manifest, args.expected_parent_sha256, output, root)
    except BaseException:
        if staging.exists():
            shutil.rmtree(staging)
        for path in (seal, output):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        if published_assets and assets.exists():
            shutil.rmtree(assets)
        raise


def print_characterization_template() -> None:
    print(f"# schema={CHAR_SCHEMA}")
    print("# schema_version=1")
    print("# primary_sha256=<64-lowercase-hex>")
    print("# frozen_oracle_sha256=<64-lowercase-hex>")
    print("# affinity_cpus=0,2,4,6,8,10,12,14")
    print(f"# timeout_seconds={TIMEOUT_SECONDS}")
    print(f"# rss_limit_bytes={RSS_LIMIT_BYTES}")
    print(f"# memory_budget_bytes={MEMORY_BUDGET_BYTES}")
    print("\t".join(CHAR_HEADER))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser(
        "print-characterization-template",
        help="print the exact sealed characterization preamble/header",
    )
    build_parser = subparsers.add_parser("build", help="build an exclusive sealed supplement")
    build_parser.add_argument("--base-manifest", type=Path, required=True)
    build_parser.add_argument("--expected-parent-sha256", required=True)
    build_parser.add_argument("--characterization", type=Path, required=True)
    build_parser.add_argument("--fixture", type=Path, required=True)
    build_parser.add_argument("--expected-fixture-sha256", required=True)
    build_parser.add_argument("--output", type=Path, required=True)
    build_parser.add_argument("--repo-root", type=Path)
    audit_parser = subparsers.add_parser("audit", help="audit a sealed Phase-9 supplement")
    audit_parser.add_argument("--base-manifest", type=Path, required=True)
    audit_parser.add_argument("--expected-parent-sha256", required=True)
    audit_parser.add_argument("--supplement", type=Path, required=True)
    audit_parser.add_argument("--repo-root", type=Path)
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    try:
        args = parse_args(argv)
        if args.command == "print-characterization-template":
            print_characterization_template()
        elif args.command == "build":
            build(args)
        elif args.command == "audit":
            audit_supplement(
                args.base_manifest,
                args.expected_parent_sha256,
                args.supplement,
                repo_root(args.repo_root),
            )
        else:
            raise AssertionError(args.command)
    except (
        BootstrapError,
        FileNotFoundError,
        PermissionError,
        OSError,
        UnicodeError,
        ValueError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
