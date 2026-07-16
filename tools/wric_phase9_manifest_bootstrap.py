#!/usr/bin/env python3
"""Build and audit the immutable Phase-9 local-commit workload supplement.

The Phase-0 workload manifest is the root of trust.  This helper will not
bless an unsealed input or replace an existing artifact.  It consumes an
explicitly named and sealed base manifest plus a sealed index of expected
frozen-oracle evidence, re-executes the input canonical command and all 12
canonical search/output pairs, and accepts only exact semantic/counter
agreement.  Completed commands are
sealed in a restartable capture directory.  Fresh reports, canonical files,
and per-row receipts are copied into a supplement-owned, resealed archive;
the derived manifest and its detached seal are then checked by both this
module and the production benchmark harness.

The frozen characterization has one row for every seed/worker combination.
Paths in it are normalized paths relative to the characterization TSV.  Its
exact schema can be printed with ``print-characterization-template``.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import csv
import dataclasses
import errno
import hashlib
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
from typing import Callable, Iterable, Iterator, Mapping, NoReturn, Sequence

sys.dont_write_bytecode = True

TOOLS_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY_ROOT = TOOLS_DIRECTORY.parent
if str(TOOLS_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIRECTORY))


def acceptance_module():
    """Import lazily so the acceptance tool can consume this module's archive API."""

    import wric_phase9_acceptance  # noqa: PLC0415

    return wric_phase9_acceptance


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
EXPECTED_CANDIDATES = ITERATIONS * MAX_CANDIDATES
EXPECTED_EXACT = ITERATIONS * TOP_K_EXACT
# Frozen-oracle characterization is an evidence-production diagnostic, not a
# measured performance gate.  Its largest W1 run has exceeded 15 minutes
# under load, while published workload trials retain their 600-second limit.
WORKLOAD_TIMEOUT_SECONDS = 600
TIMEOUT_SECONDS = 1800
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
CHAR_SCHEMA_VERSION = "3"
CHAR_PREAMBLE_KEYS = (
    "schema",
    "schema_version",
    "parent_sha256",
    "primary_sha256",
    "input_canonical_path",
    "input_canonical_sha256",
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
    "canonical_argv_sha256",
    "oracle_search_semantic_sha256",
    "oracle_output_semantic_sha256",
    "oracle_trial_semantic_sha256",
)

CHAR_CAPTURE_SCHEMA = "wric_phase9_characterization_capture"
CHAR_CAPTURE_SCHEMA_VERSION = 1
CHAR_CLOSURE_PREFIX = "closure-"
CHAR_CLOSURE_LEDGER = "assets.sha256"

HEX64 = re.compile(r"[0-9a-f]{64}")
REVISION = re.compile(r"[0-9a-f]{40,64}")
SAFE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*")
AFFINITY = re.compile(r"[0-9]+(?:[,-][0-9]+)*")
LEDGER_RELATIVE = re.compile(
    r"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*"
)


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
class InputEvidence:
    canonical: Path
    canonical_sha256: str
    semantic_sha256: str
    parsimony_min: int


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
    stable_report_sha256: str
    expected: Mapping[str, str]


@dataclasses.dataclass(frozen=True)
class AuditedFrozenCharacterization:
    """The supplement-owned frozen evidence after the complete trust audit."""

    base: Manifest
    supplement: Manifest
    characterization: Characterization
    input_evidence: InputEvidence
    evidence: Mapping[tuple[int, int], Evidence]
    process_metrics_sha256: str


@dataclasses.dataclass(frozen=True)
class PublicationPaths:
    """Canonical sibling paths covered by one immutable-output transaction."""

    output: Path
    assets: Path
    seal: Path
    lock: Path
    journal: Path
    journal_staging: Path
    staging: Path
    validation: Path


@dataclasses.dataclass
class PublicationOwnership:
    """Live-process checkpoints; disk state remains authoritative after a crash."""

    journal: tuple[int, int] | None = None
    staging: tuple[int, int] | None = None
    components: dict[str, tuple[int, int]] = dataclasses.field(default_factory=dict)


def fail(message: str) -> NoReturn:
    raise BootstrapError(message)


def reject_nonfinite_json_constant(value: str) -> NoReturn:
    """Reject Python's non-standard NaN/Infinity JSON extensions."""

    fail(f"JSON contains forbidden nonfinite constant {value!r}")


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


def require_canonical_regular(
    path: Path, label: str, *, executable: bool = False
) -> Path:
    """Reject a symlink in the file or any ancestor, not just after resolve()."""

    absolute = path.absolute()
    require_regular(absolute, label, executable=executable)
    resolved = absolute.resolve(strict=True)
    if absolute != resolved:
        fail(f"{label} uses a symlink or noncanonical lexical path: {path}")
    return resolved


def resolve_lexical_regular(
    root: Path, relative: Path, label: str, *, executable: bool = False
) -> Path:
    """Resolve one confined file while rejecting every lexical symlink component."""

    current = root
    for component in relative.parts:
        current = current / component
        try:
            info = current.lstat()
        except FileNotFoundError:
            fail(f"{label} is missing: {current}")
        if stat.S_ISLNK(info.st_mode):
            fail(f"{label} uses a lexical symlink component: {current}")
    require_regular(current, label, executable=executable)
    resolved = current.resolve(strict=True)
    try:
        resolved.relative_to(root)
    except ValueError:
        fail(f"{label} escapes its lexical root: {current}")
    return resolved


def require_lexical_directory(path: Path, label: str) -> Path:
    try:
        info = path.lstat()
    except FileNotFoundError:
        fail(f"{label} is missing: {path}")
    if not stat.S_ISDIR(info.st_mode) or path.is_symlink():
        fail(f"{label} must be a lexical directory, not an alias: {path}")
    resolved = path.resolve(strict=True)
    if path.absolute() != resolved:
        fail(f"{label} is not lexically canonical: {path}")
    return resolved


def detached_seal_bytes(path: Path) -> bytes:
    return f"{sha256_file(path)}  {path.name}\n".encode("ascii")


def verify_detached_seal(path: Path) -> str:
    require_regular(path, "sealed file")
    if path.stat().st_nlink != 1:
        fail(f"sealed file must not be externally hard-linked: {path}")
    seal = path.with_name(path.name + ".sha256")
    require_regular(seal, "detached seal")
    if seal.stat().st_nlink != 1:
        fail(f"detached seal must not be externally hard-linked: {seal}")
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


def git_output(root: Path, arguments: Sequence[str], label: str) -> str:
    """Run Git without inheriting mutable user configuration."""

    result = subprocess.run(
        ["/usr/bin/git", "-C", os.fspath(root), *arguments],
        check=False,
        stdin=subprocess.DEVNULL,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
            "HOME": "/nonexistent",
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
        },
    )
    if result.returncode != 0:
        fail(
            f"git {label} failed for {root}: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result.stdout


def base_repo_root(path: Path | None) -> Path:
    """Return an explicit, canonical Git toplevel for sealed ``repo://`` assets.

    The Phase-0 worktree is intentionally allowed to differ from the checkout
    containing this builder.  Its HEAD is bound separately to the sealed base
    manifest in :func:`read_manifest`; resolving the path is never sufficient
    evidence by itself.
    """

    candidate = path if path is not None else REPOSITORY_ROOT
    root = require_lexical_directory(candidate.absolute(), "sealed base repository root")
    top_text = git_output(root, ("rev-parse", "--show-toplevel"), "base repository root").strip()
    if not top_text:
        fail(f"git returned an empty base repository root for {root}")
    top = require_lexical_directory(
        Path(top_text).absolute(), "Git sealed base repository toplevel"
    )
    if top != root:
        fail(f"sealed base repository root {root} is not exact Git toplevel {top}")
    return root


def require_base_revision(root: Path, expected_revision: str) -> str:
    """Bind an already-canonical base worktree to its sealed revision."""

    if REVISION.fullmatch(expected_revision) is None:
        fail(f"sealed base revision is not a full Git object ID: {expected_revision!r}")
    head = git_output(
        root, ("rev-parse", "--verify", "HEAD"), "sealed base HEAD"
    ).strip()
    if REVISION.fullmatch(head) is None:
        fail(f"sealed base Git HEAD is not a full object ID: {head!r}")
    if head != expected_revision:
        fail(
            "sealed base repository HEAD differs from manifest repo_revision: "
            f"{head} != {expected_revision}"
        )
    return head


def repo_root(path: Path | None) -> Path:
    """Backward-compatible internal name for the sealed base asset root."""

    return base_repo_root(path)


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
    return resolve_lexical_regular(base, rel, f"manifest asset {uri}")


def repo_uri(root: Path, path: Path) -> str:
    require_regular(path, "base frozen asset")
    resolved = path.resolve(strict=True)
    try:
        relative = resolved.relative_to(root)
    except ValueError:
        fail(f"base frozen asset is outside --repo-root: {path}")
    lexical = path.absolute()
    if lexical != resolved:
        fail(f"base frozen asset uses a symlink or noncanonical path: {path}")
    return "repo://" + relative.as_posix()


def validate_hash(value: str, label: str) -> None:
    if HEX64.fullmatch(value) is None:
        fail(f"{label} is not canonical lowercase SHA-256: {value!r}")


def validate_affinity(value: str, label: str) -> None:
    if AFFINITY.fullmatch(value) is None:
        fail(f"{label} affinity is not a canonical taskset CPU list: {value!r}")
    result = subprocess.run(
        ["taskset", "-c", value, "true"],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        fail(f"{label} affinity cannot execute on this host: {result.stderr.strip()}")


def validate_manifest_assets(manifest: Manifest, root: Path) -> None:
    observed: dict[Path, str] = {}

    def require_hash(asset: Path, expected: str, label: str) -> None:
        if asset.stat().st_nlink != 1:
            fail(f"{label} must not be externally hard-linked: {asset}")
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
    root = base_repo_root(root)
    path = require_canonical_regular(path, "workload manifest")
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
    if expected_kind == "base":
        require_base_revision(root, preamble["repo_revision"])
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
    return resolve_lexical_regular(root, relative, label)


def read_characterization(path: Path) -> Characterization:
    path = require_canonical_regular(path, "Phase-9 characterization")
    digest = verify_detached_seal(path)
    preamble, input_rows = parse_preamble_and_rows(path, CHAR_PREAMBLE_KEYS, CHAR_HEADER)
    if (
        preamble["schema"] != CHAR_SCHEMA
        or preamble["schema_version"] != CHAR_SCHEMA_VERSION
    ):
        fail(f"unsupported Phase-9 characterization schema: {path}")
    for key in (
        "parent_sha256",
        "primary_sha256",
        "input_canonical_sha256",
        "frozen_oracle_sha256",
    ):
        validate_hash(preamble[key], f"characterization {key}")
    validate_affinity(preamble["affinity_cpus"], "characterization")
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
            matrix_key = (int(row["seed"]), int(row["workers"]))
        except ValueError:
            fail(f"characterization has non-integer seed/workers: {row}")
        if matrix_key in rows:
            fail(f"duplicate characterization seed/worker row: {matrix_key}")
        rows[matrix_key] = row
    wanted_keys = {(seed, workers) for seed in SEEDS for workers in WORKERS}
    if set(rows) != wanted_keys:
        fail(
            "characterization matrix is not exactly seeds 1/7/19 x workers "
            f"1/2/4/8: missing={sorted(wanted_keys - set(rows))}, "
            f"unexpected={sorted(set(rows) - wanted_keys)}"
        )
    evidence_paths = [preamble["input_canonical_path"]] + [
        row[f"{prefix}_path"]
        for row in rows.values()
        for prefix in (
            "product_report",
            "canonical_sidecar",
            "canonical_result",
            "output_canonical",
        )
    ]
    if len(set(evidence_paths)) != len(evidence_paths):
        fail("characterization must own independent, non-aliased evidence paths")
    identities: set[tuple[int, int]] = set()
    for index, relative in enumerate(evidence_paths):
        resolved = relative_evidence_path(
            path, relative, f"characterization evidence path {index + 1}"
        )
        info = resolved.stat()
        if info.st_nlink != 1:
            fail(f"characterization evidence path is externally hard-linked: {resolved}")
        identities.add((info.st_dev, info.st_ino))
    if len(identities) != len(evidence_paths):
        fail("characterization evidence paths must not be hard-linked aliases")
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
    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, item in pairs:
            if key in result:
                fail(f"{label} contains duplicate JSON key {key!r}: {path}")
            result[key] = item
        return result

    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicates,
            parse_constant=reject_nonfinite_json_constant,
        )
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
        def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
            result: dict[str, object] = {}
            for key, item in pairs:
                if key in result:
                    fail(
                        f"canonical sidecar line {line_number} contains duplicate "
                        f"JSON key {key!r}: {path}"
                    )
                result[key] = item
            return result

        try:
            record = json.loads(
                line,
                object_pairs_hook=reject_duplicates,
                parse_constant=reject_nonfinite_json_constant,
            )
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


def exact_input_evidence(characterization: Characterization) -> InputEvidence:
    """Validate the external canonical digest of the unmodified input fixture."""

    expected_hash = characterization.preamble["input_canonical_sha256"]
    validate_hash(expected_hash, "characterization input canonical hash")
    path = relative_evidence_path(
        characterization.path,
        characterization.preamble["input_canonical_path"],
        "characterization input canonical",
    )
    if sha256_file(path) != expected_hash:
        fail(f"characterization input canonical hash mismatch: {path}")
    acceptance = acceptance_module()
    try:
        value = acceptance.validate_dag_digest(path)
    except acceptance.AcceptanceError as error:
        fail(str(error))
    return InputEvidence(
        path,
        expected_hash,
        str(value["semantic_sha256"]),
        int(value["parsimony_min"]),
    )


def _exact_evidence(
    characterization: Characterization,
    row: Mapping[str, str],
    input_evidence: InputEvidence,
    acceptance,
) -> Evidence:
    seed, workers = int(row["seed"]), int(row["workers"])
    paths: dict[str, Path] = {}
    for prefix in (
        "product_report",
        "canonical_sidecar",
        "canonical_result",
        "output_canonical",
    ):
        expected_hash = row[f"{prefix}_sha256"]
        validate_hash(
            expected_hash, f"characterization {seed}/W{workers} {prefix} hash"
        )
        path = relative_evidence_path(
            characterization.path, row[f"{prefix}_path"], f"{seed}/W{workers} {prefix}"
        )
        if sha256_file(path) != expected_hash:
            fail(f"characterization evidence hash mismatch for {seed}/W{workers}: {path}")
        paths[prefix] = path

    report = paths["product_report"]
    label = f"frozen-oracle seed {seed} W{workers}"
    try:
        parsed_report = acceptance.parse_report(report)
        report_summary = acceptance.validate_report_characterization(
            parsed_report, seed, label
        )
        canonical_summary = acceptance.read_full_canonical_sidecar(
            paths["canonical_sidecar"], seed, label
        )
        accepted_ms = acceptance.parse_decimal(
            acceptance.require_top(parsed_report, "accepted_rebuild_ms", label),
            f"{label} accepted_rebuild_ms",
            positive=True,
        )
        total_ms = acceptance.parse_decimal(
            acceptance.require_top(parsed_report, "total_ms", label),
            f"{label} total_ms",
            positive=True,
        )
    except acceptance.AcceptanceError as error:
        fail(str(error))
    for key, wanted in REPORT_BINDINGS.items():
        actual = acceptance.require_top(parsed_report, key, label)
        if actual != wanted:
            fail(f"{label}: {key}={actual!r}, expected {wanted!r}")
    worker_bindings = {
        "seed": str(seed),
        "chart_workers_requested": str(workers),
        "chart_workers_resolved": str(workers),
        "chart_worker_policy": "explicit",
    }
    for key, wanted in worker_bindings.items():
        actual = acceptance.require_top(parsed_report, key, label)
        if actual != wanted:
            fail(f"{label}: {key}={actual!r}, expected {wanted!r}")
    if accepted_ms > total_ms:
        fail(f"{label}: accepted_rebuild_ms exceeds total_ms")
    if seed == 1 and workers == 1 and accepted_ms < acceptance.MIN_FROZEN_ACCEPTED_REBUILD_MS:
        fail(
            "frozen seed-1 W1 accepted update is below the required 100 ms: "
            f"{accepted_ms}"
        )

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
        "expected_final_score": "final_score",
    }
    expected: dict[str, str] = {
        manifest_key: acceptance.require_top(parsed_report, report_key, label)
        for manifest_key, report_key in expected_fields.items()
    }
    report_initial_score = acceptance.require_top(parsed_report, "initial_score", label)
    report_final_score = expected["expected_final_score"]
    if not report_initial_score.isdigit() or not report_final_score.isdigit():
        fail(f"{label}: report-domain initial/final score is not unsigned")
    if int(report_final_score) >= int(report_initial_score):
        fail(
            f"{label}: report-domain final score did not strictly improve "
            f"{report_initial_score} -> {report_final_score}"
        )
    expected["expected_initial_score"] = str(input_evidence.parsimony_min)
    expected["expected_stop_reason"] = report_stop_reason(report)
    for field in (
        "expected_effective_pattern_batch_size",
        "expected_active_patterns",
        "expected_initial_clades",
        "expected_initial_productions",
        "expected_candidates_generated",
        "expected_candidates_scored",
        "expected_exact_verifications",
        "expected_iterations",
        "expected_accepted_moves",
        "expected_initial_score",
        "expected_final_score",
    ):
        if not expected[field].isdigit():
            fail(f"{label}: {field} is not unsigned")

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
        "score_ua_edge": False,
    }
    for key, contract_wanted in contract_checks.items():
        if contract.get(key) != contract_wanted:
            fail(
                f"canonical sidecar contract {seed}/W{workers} {key}="
                f"{contract.get(key)!r}, expected {contract_wanted!r}"
            )
    expected["expected_keep_mask_kind"] = keep_kind

    try:
        search = acceptance.validate_search_digest(paths["canonical_result"])
        output = acceptance.validate_dag_digest(paths["output_canonical"])
    except acceptance.AcceptanceError as error:
        fail(str(error))
    search_semantic = str(search["semantic_sha256"])
    if search_semantic != row["canonical_sidecar_sha256"]:
        fail(f"compact/full canonical search mismatch: {seed}/W{workers}")
    if (
        search["candidate_count"] != EXPECTED_CANDIDATES
        or search["exact_candidate_count"] != EXPECTED_EXACT
        or search["iteration_count"] != ITERATIONS
    ):
        fail(f"canonical search counts violate the 32/4/3 contract: {seed}/W{workers}")
    canonical_reconciliation = {
        "record_count": search["record_count"],
        "initial_score": int(report_initial_score),
        "final_score": int(report_final_score),
        "active_patterns": int(expected["expected_active_patterns"]),
    }
    if any(
        canonical_summary[field] != wanted
        for field, wanted in canonical_reconciliation.items()
    ) or acceptance.report_sequence_projection(
        canonical_summary["accepted_sequence"]
    ) != report_summary["accepted_sequence"]:
        fail(f"canonical sidecar/report/compact evidence differs: {seed}/W{workers}")
    output_semantic = str(output["semantic_sha256"])
    output_parsimony = int(output["parsimony_min"])
    if output_parsimony >= input_evidence.parsimony_min:
        fail(
            f"external canonical output did not strictly improve the input: "
            f"{seed}/W{workers}: {output_parsimony} >= "
            f"{input_evidence.parsimony_min}"
        )
    expected["expected_validated_parsimony"] = str(output_parsimony)

    contract_row = phase9_contract_row(
        seed,
        workers,
        characterization.preamble["primary_sha256"],
        "manifest://fixture.pb.gz",
        characterization.preamble["affinity_cpus"],
    )
    argv_sha = canonical_argv_digest(canonical_argv(contract_row))
    trial_sha = trial_digest(METHOD, search_semantic, output_semantic, argv_sha)
    exact_digests = {
        "canonical_argv_sha256": argv_sha,
        "oracle_search_semantic_sha256": search_semantic,
        "oracle_output_semantic_sha256": output_semantic,
        "oracle_trial_semantic_sha256": trial_sha,
    }
    for field, wanted in exact_digests.items():
        validate_hash(row[field], f"characterization {seed}/W{workers} {field}")
        if row[field] != wanted:
            fail(f"characterization {seed}/W{workers} {field} differs from derived evidence")

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
        stable_report_digest(report),
        expected,
    )


def exact_evidence(
    characterization: Characterization,
    row: Mapping[str, str],
    input_evidence: InputEvidence | None = None,
) -> Evidence:
    """Translate every acceptance-layer rejection into this tool's stable error API."""

    acceptance = acceptance_module()
    try:
        return _exact_evidence(
            characterization,
            row,
            input_evidence or exact_input_evidence(characterization),
            acceptance,
        )
    except acceptance.AcceptanceError as error:
        fail(str(error))


def stable_report_digest(report: Path) -> str:
    """Digest the complete product report after masking only volatile timings."""

    try:
        payload = report.read_bytes()
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"product report is not UTF-8 for stable digest: {report}: {error}")
    if not payload.endswith(b"\n") or b"\r" in payload:
        fail(f"product report is not canonical newline-delimited UTF-8: {report}")
    field = re.compile(r"^(\s*)([A-Za-z0-9_]+):[ \t]*(.*)$")
    normalized: list[str] = []
    for line in text.splitlines():
        match = field.fullmatch(line)
        if match is None:
            normalized.append(line)
            continue
        indentation, key, _value = match.groups()
        volatile = (
            key.endswith("_ms")
            or key.endswith("_per_second")
            or key.endswith("_nanoseconds")
            or key.endswith("_nanoseconds_max")
        )
        normalized.append(
            f"{indentation}{key}: <volatile>" if volatile else line
        )
    canonical = ("\n".join(normalized) + "\n").encode("utf-8")
    return sha256_bytes(b"wric-phase9-stable-report-v2\n" + canonical)


def require_matching_stable_evidence(
    source: Evidence, fresh: Evidence, label: str
) -> None:
    """Reconcile every stable semantic, canonical, report, and expected digest."""

    fields = (
        "canonical_sidecar_sha256",
        "canonical_result_sha256",
        "output_canonical_sha256",
        "search_semantic_sha256",
        "output_semantic_sha256",
        "stable_report_sha256",
    )
    differing = [
        field for field in fields if getattr(source, field) != getattr(fresh, field)
    ]
    if source.expected != fresh.expected:
        differing.append("expected_report_projection")
    if differing:
        fail(
            f"{label} differs from the sealed source characterization: "
            f"{', '.join(differing)}"
        )


def require_matching_input_evidence(
    source: InputEvidence, fresh: InputEvidence, label: str
) -> None:
    fields = (
        "canonical_sha256",
        "semantic_sha256",
        "parsimony_min",
    )
    differing = [
        field for field in fields if getattr(source, field) != getattr(fresh, field)
    ]
    if differing:
        fail(
            f"{label} differs from the sealed source input canonical evidence: "
            f"{', '.join(differing)}"
        )


def validate_worker_independent_evidence(evidence: Sequence[Evidence]) -> None:
    owned_paths = [
        path
        for item in evidence
        for path in (
            item.product_report,
            item.canonical_sidecar,
            item.canonical_result,
            item.output_canonical,
        )
    ]
    identities = {
        (path.stat().st_dev, path.stat().st_ino) for path in owned_paths
    }
    if len(identities) != len(owned_paths):
        fail("Phase-9 evidence rows contain hard-linked file aliases")
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
    argv += [
        "--chart-spr-canonical-result",
        "@search-canonical-result",
        "-o",
        "@output",
    ]
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


def phase9_contract_row(
    seed: int,
    workers: int,
    primary_sha256: str,
    primary_uri: str,
    affinity: str,
) -> dict[str, str]:
    row = {field: "-" for field in MANIFEST_HEADER}
    row.update(
        {
            "row_id": row_id(seed, workers),
            "run_group": RUN_GROUP,
            "workload_name": workload_name(seed),
            "fixture_id": "wric-chart-three-accepts",
            "method": METHOD,
            "input_kind": "dag_pb",
            "primary_uri": primary_uri,
            "primary_sha256": primary_sha256,
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
            "seed": str(seed),
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
        }
    )
    return row


def make_manifest_row(
    evidence: Evidence,
    primary_sha256: str,
    primary_uri: str,
    sidecar_uri: str,
    canonical_result_uri: str,
    affinity: str,
) -> dict[str, str]:
    row = phase9_contract_row(
        evidence.seed,
        evidence.workers,
        primary_sha256,
        primary_uri,
        affinity,
    )
    row.update(
        {
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


def characterization_tsv_bytes(
    preamble: Mapping[str, str], rows: Sequence[Mapping[str, str]]
) -> bytes:
    lines = [f"# {key}={preamble[key]}" for key in CHAR_PREAMBLE_KEYS]
    lines.append("\t".join(CHAR_HEADER))
    for row in rows:
        lines.append("\t".join(row[field] for field in CHAR_HEADER))
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


def fsync_directory(path: Path) -> None:
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def durable_mkdir_parents(path: Path, label: str) -> Path:
    """Create a canonical directory chain and persist every new parent entry."""

    absolute = path.absolute()
    missing: list[Path] = []
    current = absolute
    while not os.path.lexists(current):
        missing.append(current)
        parent = current.parent
        if parent == current:
            fail(f"{label} has no existing directory ancestor: {path}")
        current = parent
    require_lexical_directory(current, f"existing ancestor of {label}")
    for directory in reversed(missing):
        try:
            directory.mkdir(mode=0o755)
        except FileExistsError:
            # A racing creator is acceptable only when it produced the exact
            # canonical directory that this publication intended to create.
            require_lexical_directory(directory, label)
        else:
            require_lexical_directory(directory, label)
        fsync_directory(directory)
        fsync_directory(directory.parent)
    return require_lexical_directory(absolute, label)


def fsync_regular_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def remove_recoverable_staging_file(path: Path, label: str) -> None:
    """Remove only an owned regular staging file left before atomic publish."""

    if not os.path.lexists(path):
        return
    require_regular(path, label)
    if path.stat().st_nlink != 1:
        fail(f"{label} is externally hard-linked: {path}")
    path.unlink()
    fsync_directory(path.parent)


def ensure_exact_file(path: Path, data: bytes, label: str, mode: int = 0o444) -> None:
    """Atomically install immutable bytes and recover an interrupted staging write."""

    staging = path.with_name(f".{path.name}.staging")
    if os.path.lexists(path):
        require_regular(path, label)
        if path.read_bytes() != data:
            fail(f"resumed {label} differs from the required bytes: {path}")
        remove_recoverable_staging_file(staging, f"interrupted {label} staging file")
        return
    remove_recoverable_staging_file(staging, f"interrupted {label} staging file")
    copy_bytes(staging, data, mode)
    rename_noreplace(staging, path)
    fsync_directory(path.parent)


PUBLICATION_SCHEMA = "wric_phase9_supplement_publication"
PUBLICATION_SCHEMA_VERSION = 1
PUBLICATION_COMPONENTS = ("assets", "manifest", "seal")
AT_FDCWD = -100
RENAME_NOREPLACE = 1


def rename_noreplace(source: Path, destination: Path) -> None:
    """Atomically publish one path and fail if the destination exists."""

    libc = ctypes.CDLL(None, use_errno=True)
    try:
        renameat2 = libc.renameat2
    except AttributeError:
        fail("libc does not provide renameat2 required for immutable publication")
    renameat2.argtypes = (
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    result = renameat2(
        AT_FDCWD,
        os.fsencode(source),
        AT_FDCWD,
        os.fsencode(destination),
        RENAME_NOREPLACE,
    )
    if result == 0:
        return
    error_number = ctypes.get_errno()
    if error_number in (errno.EEXIST, errno.ENOTEMPTY):
        fail(f"immutable Phase-9 publication target already exists: {destination}")
    raise OSError(
        error_number,
        f"renameat2(RENAME_NOREPLACE) failed: {source} -> {destination}",
    )


def publication_paths(output: Path) -> PublicationPaths:
    """Derive the one canonical lock/journal namespace for an output closure."""

    output = output.absolute()
    parent = require_lexical_directory(
        output.parent, "Phase-9 output parent directory"
    )
    output = parent / output.name
    if output.suffix != ".tsv" or SAFE_ID.fullmatch(output.name) is None:
        fail("Phase-9 supplement output must have one canonical *.tsv basename")
    assets = output.with_name(output.stem + ".assets")
    seal = output.with_name(output.name + ".sha256")
    if len({output, assets, seal}) != 3:
        fail("Phase-9 output, asset directory, and seal paths must be distinct")
    scope = assets.name
    result = PublicationPaths(
        output=output,
        assets=assets,
        seal=seal,
        lock=parent / f".{scope}.publication.lock",
        journal=parent / f".{scope}.publication.json",
        journal_staging=parent / f".{scope}.publication.json.staging",
        staging=parent / f".{scope}.publication.staging",
        validation=parent / f".{scope}.publication.staging" / ".validation",
    )
    if len(set(dataclasses.astuple(result))) != len(dataclasses.astuple(result)):
        fail("Phase-9 publication control paths are not distinct")
    return result


@contextmanager
def exclusive_output_lock(output: Path) -> Iterator[PublicationPaths]:
    """Serialize one immutable supplement closure through audit and rollback."""

    publication = publication_paths(output)
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(publication.lock, flags, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size != 0:
            fail(
                "Phase-9 output publication lock is not one empty owned regular "
                f"file: {publication.lock}"
            )
        os.fsync(descriptor)
        fsync_directory(publication.lock.parent)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail(
                "Phase-9 output closure is owned by another builder: "
                f"{publication.output}"
            )
        yield publication
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def fsync_directory_tree(root: Path) -> None:
    """Persist every directory entry in a prepared tree, deepest first."""

    directories = [Path(directory) for directory, _, _ in os.walk(root)]
    for directory in sorted(
        directories, key=lambda item: len(item.relative_to(root).parts), reverse=True
    ):
        fsync_directory(directory)


def publication_journal_bytes(
    publication: PublicationPaths,
    asset_files: Mapping[str, bytes],
    manifest_data: bytes,
    seal_data: bytes,
) -> bytes:
    value = {
        "schema": PUBLICATION_SCHEMA,
        "schema_version": PUBLICATION_SCHEMA_VERSION,
        "output_name": publication.output.name,
        "assets_name": publication.assets.name,
        "seal_name": publication.seal.name,
        "manifest_sha256": sha256_bytes(manifest_data),
        "seal_sha256": sha256_bytes(seal_data),
        "assets_sha256": {
            relative: sha256_bytes(data)
            for relative, data in sorted(asset_files.items())
        },
    }
    return (json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def read_publication_journal(
    publication: PublicationPaths,
) -> tuple[dict[str, str], str, str]:
    require_regular(
        publication.journal, "Phase-9 supplement publication journal"
    )
    if publication.journal.stat().st_nlink != 1:
        fail(
            "Phase-9 supplement publication journal is externally hard-linked: "
            f"{publication.journal}"
        )
    value = read_json_object(
        publication.journal, "Phase-9 supplement publication journal"
    )
    expected_keys = {
        "schema",
        "schema_version",
        "output_name",
        "assets_name",
        "seal_name",
        "manifest_sha256",
        "seal_sha256",
        "assets_sha256",
    }
    if set(value) != expected_keys:
        fail("Phase-9 supplement publication journal schema keys changed")
    exact: Mapping[str, object] = {
        "schema": PUBLICATION_SCHEMA,
        "schema_version": PUBLICATION_SCHEMA_VERSION,
        "output_name": publication.output.name,
        "assets_name": publication.assets.name,
        "seal_name": publication.seal.name,
    }
    for key, wanted in exact.items():
        if value[key] != wanted:
            fail(
                f"Phase-9 supplement publication journal {key}="
                f"{value[key]!r}, expected {wanted!r}"
            )
    manifest_sha = value["manifest_sha256"]
    seal_sha = value["seal_sha256"]
    if not isinstance(manifest_sha, str) or not isinstance(seal_sha, str):
        fail("Phase-9 supplement publication journal file hashes are not strings")
    validate_hash(manifest_sha, "publication manifest hash")
    validate_hash(seal_sha, "publication seal hash")
    raw_assets = value["assets_sha256"]
    if not isinstance(raw_assets, dict) or not raw_assets:
        fail("Phase-9 supplement publication journal asset map is not an object")
    assets: dict[str, str] = {}
    for relative, digest in raw_assets.items():
        if not isinstance(relative, str) or LEDGER_RELATIVE.fullmatch(relative) is None:
            fail(f"publication journal has a non-canonical asset path: {relative!r}")
        if not isinstance(digest, str):
            fail(f"publication journal asset hash is not a string: {relative}")
        validate_hash(digest, f"publication journal asset {relative}")
        assets[relative] = digest
    return assets, manifest_sha, seal_sha


def expected_asset_directories(asset_paths: Iterable[str]) -> set[str]:
    result: set[str] = set()
    for relative in asset_paths:
        parent = Path(relative).parent
        while parent != Path("."):
            result.add(parent.as_posix())
            parent = parent.parent
    return result


def validate_publication_asset_tree(
    root: Path,
    expected: Mapping[str, str],
    label: str,
    *,
    complete: bool,
    verify_hashes: bool,
) -> None:
    root = require_lexical_directory(root, label)
    observed_files: set[str] = set()
    observed_directories: set[str] = set()
    for directory_text, directory_names, file_names in os.walk(
        root, followlinks=False
    ):
        directory = Path(directory_text)
        for name in directory_names:
            child = directory / name
            info = child.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
                fail(f"{label} contains a non-directory alias: {child}")
            observed_directories.add(child.relative_to(root).as_posix())
        for name in file_names:
            child = directory / name
            info = child.lstat()
            if (
                stat.S_ISLNK(info.st_mode)
                or not stat.S_ISREG(info.st_mode)
                or info.st_nlink != 1
            ):
                fail(f"{label} contains a non-owned regular file: {child}")
            relative = child.relative_to(root).as_posix()
            if relative not in expected:
                fail(f"{label} contains an unexpected file: {relative}")
            if verify_hashes and sha256_file(child) != expected[relative]:
                fail(f"{label} file hash changed: {relative}")
            observed_files.add(relative)
    expected_directories = expected_asset_directories(expected)
    if not observed_files <= set(expected) or not observed_directories <= expected_directories:
        fail(f"{label} is outside its journal-owned asset closure")
    if complete and (
        observed_files != set(expected)
        or observed_directories != expected_directories
    ):
        fail(f"{label} is not the complete journal-owned asset closure")


def validate_publication_regular(
    path: Path, digest: str, label: str, *, verify_hash: bool
) -> None:
    require_regular(path, label)
    info = path.stat()
    if info.st_nlink != 1:
        fail(f"{label} is externally hard-linked: {path}")
    if verify_hash and sha256_file(path) != digest:
        fail(f"{label} hash changed: {path}")


def publication_path_identity(path: Path) -> tuple[int, int]:
    info = path.lstat()
    return info.st_dev, info.st_ino


def require_publication_identity(
    path: Path, expected: tuple[int, int], label: str
) -> None:
    try:
        observed = publication_path_identity(path)
    except FileNotFoundError:
        fail(f"{label} disappeared before owned cleanup: {path}")
    if observed != expected:
        fail(f"{label} was replaced before owned cleanup: {path}")


def validate_publication_staging(
    publication: PublicationPaths,
    asset_hashes: Mapping[str, str],
    manifest_sha: str,
    seal_sha: str,
    *,
    complete: bool,
) -> set[str]:
    staging = require_lexical_directory(
        publication.staging, "Phase-9 publication staging directory"
    )
    staged_paths = staged_publication_paths(publication)
    names_to_components = {
        path.name: component for component, path in staged_paths.items()
    }
    observed_names = {path.name for path in staging.iterdir()}
    if not observed_names <= set(names_to_components):
        fail(
            "Phase-9 publication staging directory has unexpected members: "
            f"{sorted(observed_names - set(names_to_components))}"
        )
    observed = {names_to_components[name] for name in observed_names}
    if complete and observed != set(PUBLICATION_COMPONENTS):
        fail("Phase-9 publication staging directory is incomplete")
    if "assets" in observed:
        validate_publication_asset_tree(
            staged_paths["assets"],
            asset_hashes,
            "staged Phase-9 assets",
            complete=complete,
            verify_hashes=complete,
        )
    for name, digest in (("manifest", manifest_sha), ("seal", seal_sha)):
        if name in observed:
            validate_publication_regular(
                staged_paths[name],
                digest,
                f"staged Phase-9 {name}",
                verify_hash=complete,
            )
    return observed


def validate_final_publication_component(
    publication: PublicationPaths,
    component: str,
    asset_hashes: Mapping[str, str],
    manifest_sha: str,
    seal_sha: str,
) -> None:
    if component == "assets":
        validate_publication_asset_tree(
            publication.assets,
            asset_hashes,
            "published Phase-9 assets",
            complete=True,
            verify_hashes=True,
        )
        return
    path, digest = (
        (publication.output, manifest_sha)
        if component == "manifest"
        else (publication.seal, seal_sha)
    )
    validate_publication_regular(
        path, digest, f"published Phase-9 {component}", verify_hash=True
    )


def validate_staged_publication_component(
    publication: PublicationPaths,
    component: str,
    asset_hashes: Mapping[str, str],
    manifest_sha: str,
    seal_sha: str,
) -> None:
    path = staged_publication_paths(publication)[component]
    if component == "assets":
        validate_publication_asset_tree(
            path,
            asset_hashes,
            "staged Phase-9 assets",
            complete=True,
            verify_hashes=True,
        )
        return
    digest = manifest_sha if component == "manifest" else seal_sha
    validate_publication_regular(
        path, digest, f"staged Phase-9 {component}", verify_hash=True
    )


def staged_publication_paths(
    publication: PublicationPaths,
) -> dict[str, Path]:
    """Use final basenames privately so the complete audit runs before publish."""

    return {
        "assets": publication.staging / publication.assets.name,
        "manifest": publication.staging / publication.output.name,
        "seal": publication.staging / publication.seal.name,
    }


def remove_interrupted_publication_validation(
    publication: PublicationPaths,
) -> None:
    """Discard only the private, deterministic recovery-validation view."""

    if not os.path.lexists(publication.validation):
        return
    require_lexical_directory(
        publication.staging, "Phase-9 publication staging directory"
    )
    validation = require_lexical_directory(
        publication.validation, "Phase-9 private recovery-validation directory"
    )
    identity = publication_path_identity(validation)
    require_publication_identity(
        validation,
        identity,
        "Phase-9 private recovery-validation directory",
    )
    shutil.rmtree(validation)
    fsync_directory(publication.staging)


def validate_reconstructed_publication(
    publication: PublicationPaths,
    published: set[str],
    validator: Callable[[Path], None],
) -> None:
    """Audit a final-named private copy of a split staged/final transaction."""

    if os.path.lexists(publication.validation):
        fail("Phase-9 private recovery-validation directory already exists")
    publication.validation.mkdir(mode=0o755)
    validation_identity = publication_path_identity(publication.validation)
    fsync_directory(publication.staging)
    staged_paths = staged_publication_paths(publication)
    final_paths = {
        "assets": publication.assets,
        "manifest": publication.output,
        "seal": publication.seal,
    }
    private_paths = {
        "assets": publication.validation / publication.assets.name,
        "manifest": publication.validation / publication.output.name,
        "seal": publication.validation / publication.seal.name,
    }
    try:
        for component in PUBLICATION_COMPONENTS:
            source = (
                final_paths[component]
                if component in published
                else staged_paths[component]
            )
            destination = private_paths[component]
            if component == "assets":
                shutil.copytree(source, destination, copy_function=shutil.copy2)
            else:
                shutil.copy2(source, destination)
        validator(private_paths["manifest"])
    finally:
        require_publication_identity(
            publication.validation,
            validation_identity,
            "Phase-9 private recovery-validation directory",
        )
        shutil.rmtree(publication.validation)
        fsync_directory(publication.staging)


def rollback_owned_publication(
    publication: PublicationPaths, ownership: PublicationOwnership
) -> None:
    """Clean only unpublished paths whose live-process identities still match.

    Once any final component exists, the journal and staging tree are retained
    for deterministic restart roll-forward.  In particular, rollback never
    unlinks a final pathname that a non-cooperating writer could have replaced.
    """

    if ownership.journal is None:
        # The journal rename may have completed immediately before an injected
        # exception.  Retaining all paths lets restart inspect disk truth.
        return
    require_publication_identity(
        publication.journal,
        ownership.journal,
        "owned Phase-9 publication journal",
    )
    journal_identity = publication_path_identity(publication.journal)
    asset_hashes, manifest_sha, seal_sha = read_publication_journal(publication)
    require_publication_identity(
        publication.journal,
        journal_identity,
        "Phase-9 publication journal",
    )
    if any(
        os.path.lexists(path)
        for path in (publication.output, publication.assets, publication.seal)
    ):
        return
    if os.path.lexists(publication.journal_staging):
        fail("unexpected journal staging path appeared during owned rollback")
    if os.path.lexists(publication.staging):
        if ownership.staging is None:
            # As with a component rename, interruption can occur between the
            # mkdir and its in-memory ownership checkpoint.
            return
        require_publication_identity(
            publication.staging,
            ownership.staging,
            "owned Phase-9 publication staging directory",
        )
        validate_publication_staging(
            publication,
            asset_hashes,
            manifest_sha,
            seal_sha,
            complete=False,
        )
        shutil.rmtree(publication.staging)
        fsync_directory(publication.staging.parent)
    require_publication_identity(
        publication.journal,
        ownership.journal,
        "owned Phase-9 publication journal",
    )
    publication.journal.unlink()
    fsync_directory(publication.journal.parent)


def recover_interrupted_publication(
    publication: PublicationPaths,
    *,
    prepublish_validator: Callable[[Path], None],
) -> bool:
    """Roll a durable journal/staging partition forward, or discard preparation.

    A complete return still retains the journal until the caller re-runs the
    supplement audit and production-harness validation.  No self-declared
    complete triplet is ever treated as committed merely by recovery.
    """

    if os.path.lexists(publication.journal_staging):
        journal_staging_identity = publication_path_identity(
            publication.journal_staging
        )
        validate_publication_regular(
            publication.journal_staging,
            "0" * 64,
            "interrupted Phase-9 publication journal staging file",
            verify_hash=False,
        )
        if os.path.lexists(publication.journal):
            fail("journal staging path exists beside a durable Phase-9 journal")
        if os.path.lexists(publication.staging) or any(
            os.path.lexists(path)
            for path in (publication.output, publication.assets, publication.seal)
        ):
            fail("ambiguous Phase-9 publication exists beside an incomplete journal")
        require_publication_identity(
            publication.journal_staging,
            journal_staging_identity,
            "interrupted Phase-9 publication journal staging file",
        )
        publication.journal_staging.unlink()
        fsync_directory(publication.journal_staging.parent)
    if not os.path.lexists(publication.journal):
        if os.path.lexists(publication.staging):
            fail("unowned Phase-9 publication staging directory exists")
        return False

    journal_identity = publication_path_identity(publication.journal)
    asset_hashes, manifest_sha, seal_sha = read_publication_journal(publication)
    require_publication_identity(
        publication.journal,
        journal_identity,
        "Phase-9 publication journal",
    )
    remove_interrupted_publication_validation(publication)
    staged: set[str] = set()
    if os.path.lexists(publication.staging):
        staged = validate_publication_staging(
            publication,
            asset_hashes,
            manifest_sha,
            seal_sha,
            complete=False,
        )
    final_paths = {
        "assets": publication.assets,
        "manifest": publication.output,
        "seal": publication.seal,
    }
    published = {
        component for component, path in final_paths.items() if os.path.lexists(path)
    }
    overlap = staged & published
    if overlap:
        fail(
            "Phase-9 publication has duplicate staged/final components: "
            f"{sorted(overlap)}"
        )
    allowed_prefixes: tuple[set[str], ...] = (
        set(),
        {"assets"},
        {"assets", "manifest"},
        set(PUBLICATION_COMPONENTS),
    )
    if published not in allowed_prefixes:
        fail(
            "Phase-9 publication final components violate seal-last order: "
            f"{sorted(published)}"
        )
    for component in published:
        validate_final_publication_component(
            publication, component, asset_hashes, manifest_sha, seal_sha
        )
    complete = published == set(PUBLICATION_COMPONENTS)
    if complete:
        if staged:
            fail("complete Phase-9 publication retains staged components")
        return True

    # Before the first final rename, an incomplete staging tree is only a
    # preparation crash.  It owns no immutable output and can be discarded.
    if not published and staged != set(PUBLICATION_COMPONENTS):
        if os.path.lexists(publication.staging):
            shutil.rmtree(publication.staging)
            fsync_directory(publication.staging.parent)
        require_publication_identity(
            publication.journal,
            journal_identity,
            "Phase-9 publication journal",
        )
        publication.journal.unlink()
        fsync_directory(publication.journal.parent)
        return False

    if published | staged != set(PUBLICATION_COMPONENTS):
        fail("Phase-9 publication cannot reconstruct its exact component partition")
    for component in staged:
        validate_staged_publication_component(
            publication, component, asset_hashes, manifest_sha, seal_sha
        )
    if published:
        validate_reconstructed_publication(
            publication, published, prepublish_validator
        )
    else:
        prepublish_validator(staged_publication_paths(publication)["manifest"])
    # The validator consumed private copies for a split transaction.  Recheck
    # every source immediately before the no-replace publication renames.
    for component in published:
        validate_final_publication_component(
            publication, component, asset_hashes, manifest_sha, seal_sha
        )
    for component in staged:
        validate_staged_publication_component(
            publication, component, asset_hashes, manifest_sha, seal_sha
        )
    destinations = {
        "assets": publication.assets,
        "manifest": publication.output,
        "seal": publication.seal,
    }
    for component in PUBLICATION_COMPONENTS:
        if component not in staged:
            continue
        rename_noreplace(
            staged_publication_paths(publication)[component],
            destinations[component],
        )
        fsync_directory(publication.staging)
        fsync_directory(destinations[component].parent)
    return True


def publish_immutable_supplement(
    publication: PublicationPaths,
    asset_files: Mapping[str, bytes],
    manifest_data: bytes,
    seal_data: bytes,
    *,
    crash_hook: Callable[[str], None] | None = None,
    ownership: PublicationOwnership | None = None,
    prepublish_validator: Callable[[Path], None] | None = None,
) -> None:
    """Durably publish assets, manifest, then completion seal without replace."""

    hook = crash_hook or (lambda _point: None)
    journal_data = publication_journal_bytes(
        publication, asset_files, manifest_data, seal_data
    )
    copy_bytes(publication.journal_staging, journal_data, 0o444)
    journal_identity = publication_path_identity(publication.journal_staging)
    rename_noreplace(publication.journal_staging, publication.journal)
    hook("journal_renamed")
    if ownership is not None:
        ownership.journal = journal_identity
    fsync_directory(publication.journal.parent)
    hook("journal_published")

    publication.staging.mkdir(mode=0o755)
    staging_identity = publication_path_identity(publication.staging)
    if ownership is not None:
        ownership.staging = staging_identity
    fsync_directory(publication.staging.parent)
    staged_paths = staged_publication_paths(publication)
    staged_assets = staged_paths["assets"]
    staged_assets.mkdir(mode=0o755)
    for relative, data in sorted(asset_files.items()):
        destination = staged_assets / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        copy_bytes(
            destination, data, 0o555 if relative == "commands.sh" else 0o444
        )
    copy_bytes(staged_paths["manifest"], manifest_data, 0o444)
    copy_bytes(staged_paths["seal"], seal_data, 0o444)
    fsync_directory_tree(publication.staging)
    asset_hashes, manifest_sha, seal_sha = read_publication_journal(publication)
    validate_publication_staging(
        publication,
        asset_hashes,
        manifest_sha,
        seal_sha,
        complete=True,
    )
    hook("staging_durable")
    if prepublish_validator is not None:
        prepublish_validator(staged_paths["manifest"])
    hook("staging_validated")

    for component, destination in (
        ("assets", publication.assets),
        ("manifest", publication.output),
        ("seal", publication.seal),
    ):
        component_identity = publication_path_identity(staged_paths[component])
        rename_noreplace(staged_paths[component], destination)
        hook(component + "_renamed")
        if ownership is not None:
            ownership.components[component] = component_identity
        fsync_directory(publication.staging)
        fsync_directory(destination.parent)
        hook(component + "_published")


def finish_publication(publication: PublicationPaths) -> None:
    """Forget the transaction only after the published closure passed audit."""

    journal_identity = publication_path_identity(publication.journal)
    asset_hashes, manifest_sha, seal_sha = read_publication_journal(publication)
    require_publication_identity(
        publication.journal,
        journal_identity,
        "audited Phase-9 publication journal",
    )
    if os.path.lexists(publication.staging):
        observed = validate_publication_staging(
            publication,
            asset_hashes,
            manifest_sha,
            seal_sha,
            complete=False,
        )
        if observed:
            fail("audited Phase-9 publication retains staged components")
        publication.staging.rmdir()
        fsync_directory(publication.staging.parent)
    for component in PUBLICATION_COMPONENTS:
        validate_final_publication_component(
            publication, component, asset_hashes, manifest_sha, seal_sha
        )
    require_publication_identity(
        publication.journal,
        journal_identity,
        "audited Phase-9 publication journal",
    )
    publication.journal.unlink()
    fsync_directory(publication.journal.parent)


def seal_bytes(name: str, data: bytes) -> bytes:
    return f"{sha256_bytes(data)}  {name}\n".encode("ascii")


CAPTURE_SCHEMA = "wric_phase9_frozen_capture"
CAPTURE_SCHEMA_VERSION = 3
CAPTURE_ROW_SCHEMA_VERSION = 3
CAPTURE_INPUT_SCHEMA_VERSION = 1
PROCESS_METRICS_KEYS = (
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
)
CAPTURE_STATUS_FILES = (
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
CAPTURE_INPUT_FILES = (
    "report.txt",
    "stderr.txt",
    "process-metrics.txt",
    "canonical.json",
)


def validate_process_metrics_receipt(path: Path, label: str) -> Mapping[str, str]:
    """Validate one successful schema-v2 process-metrics enforcement receipt."""

    require_regular(path, label)
    if path.stat().st_nlink != 1:
        fail(f"{label} is externally hard-linked: {path}")
    try:
        payload = path.read_bytes()
        text = payload.decode("ascii")
    except UnicodeDecodeError as error:
        fail(f"{label} is not ASCII: {error}")
    if not payload.endswith(b"\n") or b"\r" in payload:
        fail(f"{label} is not canonical newline-delimited metrics")
    lines = text.splitlines()
    if len(lines) != len(PROCESS_METRICS_KEYS):
        fail(f"{label} does not contain the exact schema-v2 key count")
    values: dict[str, str] = {}
    for index, (line, wanted_key) in enumerate(
        zip(lines, PROCESS_METRICS_KEYS, strict=True), 1
    ):
        if "=" not in line:
            fail(f"{label} line {index} lacks '='")
        key, value = line.split("=", 1)
        if key != wanted_key or not value:
            fail(
                f"{label} line {index} is not canonical {wanted_key}=VALUE"
            )
        values[key] = value

    exact = {
        "schema_version": "2",
        "outcome": "exited",
        "exit_code": "0",
        "term_signal": "0",
        "timed_out": "0",
        "runner_exit_code": "0",
        "peak_sampled_swap_kb": "0",
        "rss_kb_unit": "1024_bytes",
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
        "rss_limit_bytes": str(RSS_LIMIT_BYTES),
        "rss_limit_enabled": "1",
        "rss_limit_observed": "0",
        "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0",
        "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0",
    }
    for key, wanted in exact.items():
        if values[key] != wanted:
            fail(f"{label} {key}={values[key]!r}, expected {wanted!r}")
    decimal = re.compile(r"(?:0|[1-9][0-9]*)[.][0-9]+")
    for key in ("wall_seconds", "user_seconds", "system_seconds"):
        if decimal.fullmatch(values[key]) is None:
            fail(f"{label} {key} is not a finite canonical decimal")
    unsigned = re.compile(r"0|[1-9][0-9]*")
    numeric_keys = (
        "max_rss_kb",
        "peak_sampled_rss_kb",
        "peak_sampled_swap_kb",
        "proc_status_samples",
        "proc_rss_samples",
        "proc_swap_samples",
        "proc_group_samples",
        "peak_sampled_process_count",
        "descendants_reaped",
    )
    for key in numeric_keys:
        if unsigned.fullmatch(values[key]) is None:
            fail(f"{label} {key} is not a canonical unsigned integer")
    for key in (
        "proc_status_samples",
        "proc_rss_samples",
        "proc_swap_samples",
        "proc_group_samples",
        "peak_sampled_process_count",
    ):
        if int(values[key]) == 0:
            fail(f"{label} {key} must be positive")
    if int(values["peak_sampled_rss_kb"]) * 1024 > RSS_LIMIT_BYTES:
        fail(f"{label} sampled RSS exceeds the enforced Phase-9 limit")
    return values


@contextmanager
def exclusive_capture_lock(capture_dir: Path) -> Iterator[None]:
    """Admit one capture owner; kernel locking releases cleanly after a crash."""

    capture = capture_dir.absolute()
    parent = durable_mkdir_parents(
        capture.parent, "Phase-9 capture parent directory"
    )
    lock_path = parent / (capture.name + ".lock")
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(lock_path, flags, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size != 0:
            fail(f"Phase-9 capture lock is not one empty owned regular file: {lock_path}")
        os.fsync(descriptor)
        fsync_directory(parent)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail(f"Phase-9 capture directory is owned by another builder: {capture}")
        yield
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def capture_contract_bytes(
    base: Manifest,
    characterization: Characterization,
    source_input: InputEvidence,
    fixture_sha256: str,
    process_metrics_sha256: str,
) -> bytes:
    rows = []
    for seed in SEEDS:
        for workers in WORKERS:
            contract = phase9_contract_row(
                seed,
                workers,
                fixture_sha256,
                "manifest://fixture.pb.gz",
                characterization.preamble["affinity_cpus"],
            )
            rows.append(
                {
                    "row_id": row_id(seed, workers),
                    "canonical_argv_sha256": canonical_argv_digest(
                        canonical_argv(contract)
                    ),
                }
            )
    value = {
        "schema": CAPTURE_SCHEMA,
        "schema_version": CAPTURE_SCHEMA_VERSION,
        "parent_sha256": base.sha256,
        "source_characterization_basename": characterization.path.name,
        "source_characterization_sha256": characterization.sha256,
        "source_characterization_seal_sha256": sha256_file(
            characterization.path.with_name(characterization.path.name + ".sha256")
        ),
        "source_input_canonical_sha256": source_input.canonical_sha256,
        "source_input_semantic_sha256": source_input.semantic_sha256,
        "source_input_parsimony_min": source_input.parsimony_min,
        "fixture_sha256": fixture_sha256,
        "frozen_oracle_sha256": base.preamble["frozen_oracle_dagutil_sha256"],
        "process_metrics_sha256": process_metrics_sha256,
        "timeout_seconds": TIMEOUT_SECONDS,
        "rss_limit_bytes": RSS_LIMIT_BYTES,
        "affinity_cpus": characterization.preamble["affinity_cpus"],
        "rows": rows,
    }
    return (
        json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def initialize_capture_directory(
    capture_dir: Path,
    base: Manifest,
    characterization: Characterization,
    source_input: InputEvidence,
    fixture_sha256: str,
    process_metrics_sha256: str,
) -> Path:
    capture = capture_dir.absolute()
    contract = capture_contract_bytes(
        base,
        characterization,
        source_input,
        fixture_sha256,
        process_metrics_sha256,
    )
    if os.path.lexists(capture):
        capture = require_lexical_directory(capture, "Phase-9 capture directory")
    else:
        capture.parent.mkdir(parents=True, exist_ok=True)
        capture.mkdir(mode=0o755)
        capture = require_lexical_directory(capture, "Phase-9 capture directory")
    contract_path = capture / "capture-contract.json"
    ensure_exact_file(contract_path, contract, "Phase-9 capture contract")
    ensure_exact_file(
        contract_path.with_name(contract_path.name + ".sha256"),
        seal_bytes(contract_path.name, contract),
        "Phase-9 capture contract seal",
    )
    rows = capture / "rows"
    if os.path.lexists(rows):
        require_lexical_directory(rows, "Phase-9 capture rows directory")
    else:
        rows.mkdir(mode=0o755)
    allowed = {
        "capture-contract.json",
        "capture-contract.json.sha256",
        ".input.staging",
        "input",
        "rows",
    }
    unexpected = sorted(path.name for path in capture.iterdir() if path.name not in allowed)
    if unexpected:
        fail(f"Phase-9 capture directory has unexpected artifacts: {unexpected}")
    return capture


def characterization_contract(
    base: Manifest,
    fixture_sha256: str,
    affinity: str,
    capture_dir: Path,
) -> Characterization:
    """Return the immutable Phase-9 contract used while evidence is produced."""

    placeholder = "0" * 64
    rows: dict[tuple[int, int], Mapping[str, str]] = {}
    for seed in SEEDS:
        for workers in WORKERS:
            rows[(seed, workers)] = {
                "seed": str(seed),
                "workers": str(workers),
                "product_report_path": "pending/report.txt",
                "product_report_sha256": placeholder,
                "canonical_sidecar_path": "pending/canonical.ndjson",
                "canonical_sidecar_sha256": placeholder,
                "canonical_result_path": "pending/canonical.json",
                "canonical_result_sha256": placeholder,
                "output_canonical_path": "pending/output-canonical.json",
                "output_canonical_sha256": placeholder,
                "canonical_argv_sha256": placeholder,
                "oracle_search_semantic_sha256": placeholder,
                "oracle_output_semantic_sha256": placeholder,
                "oracle_trial_semantic_sha256": placeholder,
            }
    preamble = {
        "schema": CHAR_SCHEMA,
        "schema_version": CHAR_SCHEMA_VERSION,
        "parent_sha256": base.sha256,
        "primary_sha256": fixture_sha256,
        "input_canonical_path": "input/canonical.json",
        "input_canonical_sha256": placeholder,
        "frozen_oracle_sha256": base.preamble[
            "frozen_oracle_dagutil_sha256"
        ],
        "affinity_cpus": affinity,
        "timeout_seconds": str(TIMEOUT_SECONDS),
        "rss_limit_bytes": str(RSS_LIMIT_BYTES),
        "memory_budget_bytes": str(MEMORY_BUDGET_BYTES),
    }
    return Characterization(
        capture_dir / "characterization-contract.tsv",
        "-",
        preamble,
        rows,
    )


def characterization_capture_contract_bytes(
    base: Manifest,
    characterization: Characterization,
    fixture_sha256: str,
    process_metrics_sha256: str,
) -> bytes:
    rows = []
    for seed in SEEDS:
        for workers in WORKERS:
            contract = phase9_contract_row(
                seed,
                workers,
                fixture_sha256,
                "manifest://fixture.pb.gz",
                characterization.preamble["affinity_cpus"],
            )
            argv = canonical_argv(contract)
            rows.append(
                {
                    "row_id": row_id(seed, workers),
                    "seed": seed,
                    "workers": workers,
                    "canonical_argv": argv,
                    "canonical_argv_sha256": canonical_argv_digest(argv),
                }
            )
    value = {
        "schema": CHAR_CAPTURE_SCHEMA,
        "schema_version": CHAR_CAPTURE_SCHEMA_VERSION,
        "parent_sha256": base.sha256,
        "fixture_sha256": fixture_sha256,
        "frozen_oracle_sha256": base.preamble[
            "frozen_oracle_dagutil_sha256"
        ],
        "process_metrics_sha256": process_metrics_sha256,
        "timeout_seconds": TIMEOUT_SECONDS,
        "rss_limit_bytes": RSS_LIMIT_BYTES,
        "memory_budget_bytes": MEMORY_BUDGET_BYTES,
        "affinity_cpus": characterization.preamble["affinity_cpus"],
        "rows": rows,
    }
    return (
        json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def initialize_characterization_capture_directory(
    capture_dir: Path,
    base: Manifest,
    characterization: Characterization,
    fixture_sha256: str,
    process_metrics_sha256: str,
) -> Path:
    """Create or resume only the exact producer-owned capture closure."""

    capture = capture_dir.absolute()
    contract = characterization_capture_contract_bytes(
        base,
        characterization,
        fixture_sha256,
        process_metrics_sha256,
    )
    if os.path.lexists(capture):
        capture = require_lexical_directory(
            capture, "Phase-9 characterization capture directory"
        )
    else:
        durable_mkdir_parents(
            capture.parent, "Phase-9 characterization capture parent"
        )
        capture.mkdir(mode=0o755)
        fsync_directory(capture)
        fsync_directory(capture.parent)
        capture = require_lexical_directory(
            capture, "Phase-9 characterization capture directory"
        )
    contract_path = capture / "capture-contract.json"
    ensure_exact_file(
        contract_path,
        contract,
        "Phase-9 characterization capture contract",
    )
    ensure_exact_file(
        contract_path.with_name(contract_path.name + ".sha256"),
        seal_bytes(contract_path.name, contract),
        "Phase-9 characterization capture contract seal",
    )
    verify_detached_seal(contract_path)
    rows = capture / "rows"
    if os.path.lexists(rows):
        require_lexical_directory(
            rows, "Phase-9 characterization capture rows directory"
        )
    else:
        rows.mkdir(mode=0o755)
        fsync_directory(rows)
        fsync_directory(capture)
    allowed = {
        "capture-contract.json",
        "capture-contract.json.sha256",
        ".input.staging",
        "input",
        "rows",
    }
    unexpected = sorted(
        path.name for path in capture.iterdir() if path.name not in allowed
    )
    if unexpected:
        fail(
            "Phase-9 characterization capture has unexpected artifacts: "
            f"{unexpected}"
        )
    return capture


def captured_row_mapping(
    characterization: Characterization,
    seed: int,
    workers: int,
    row_directory: Path,
) -> dict[str, str]:
    source = dict(characterization.rows[(seed, workers)])
    capture_root = row_directory.parent.parent
    relative_root = row_directory.relative_to(capture_root).as_posix()
    paths = {
        "product_report": "report.txt",
        "canonical_sidecar": "canonical.ndjson",
        "canonical_result": "canonical.json",
        "output_canonical": "output-canonical.json",
    }
    for prefix, filename in paths.items():
        path = row_directory / filename
        source[f"{prefix}_path"] = f"{relative_root}/{filename}"
        source[f"{prefix}_sha256"] = sha256_file(path)
    acceptance = acceptance_module()
    try:
        search = acceptance.validate_search_digest(row_directory / "canonical.json")
        output = acceptance.validate_dag_digest(row_directory / "output-canonical.json")
    except acceptance.AcceptanceError as error:
        fail(str(error))
    contract = phase9_contract_row(
        seed,
        workers,
        characterization.preamble["primary_sha256"],
        "manifest://fixture.pb.gz",
        characterization.preamble["affinity_cpus"],
    )
    argv_sha = canonical_argv_digest(canonical_argv(contract))
    source["canonical_argv_sha256"] = argv_sha
    source["oracle_search_semantic_sha256"] = str(search["semantic_sha256"])
    source["oracle_output_semantic_sha256"] = str(output["semantic_sha256"])
    source["oracle_trial_semantic_sha256"] = trial_digest(
        METHOD,
        source["oracle_search_semantic_sha256"],
        source["oracle_output_semantic_sha256"],
        argv_sha,
    )
    return source


def captured_evidence(
    characterization: Characterization,
    input_evidence: InputEvidence,
    seed: int,
    workers: int,
    row_directory: Path,
) -> tuple[Evidence, dict[str, str]]:
    row = captured_row_mapping(characterization, seed, workers, row_directory)
    synthetic = Characterization(
        row_directory.parent.parent / "capture-index.tsv",
        "-",
        characterization.preamble,
        {(seed, workers): row},
    )
    evidence = exact_evidence(synthetic, row, input_evidence)
    return evidence, row


def captured_input_evidence(path: Path) -> InputEvidence:
    require_regular(path, "captured input canonical evidence")
    acceptance = acceptance_module()
    try:
        value = acceptance.validate_dag_digest(path)
    except acceptance.AcceptanceError as error:
        fail(str(error))
    return InputEvidence(
        path,
        sha256_file(path),
        str(value["semantic_sha256"]),
        int(value["parsimony_min"]),
    )


def input_capture_status_bytes(
    base: Manifest,
    characterization: Characterization,
    fixture_sha256: str,
    process_metrics_sha256: str,
    evidence: InputEvidence,
    directory: Path,
) -> bytes:
    value = {
        "schema": "wric_phase9_frozen_input_capture",
        "schema_version": CAPTURE_INPUT_SCHEMA_VERSION,
        "parent_sha256": base.sha256,
        "fixture_sha256": fixture_sha256,
        "frozen_oracle_sha256": base.preamble["frozen_oracle_dagutil_sha256"],
        "process_metrics_sha256": process_metrics_sha256,
        "timeout_seconds": TIMEOUT_SECONDS,
        "rss_limit_bytes": RSS_LIMIT_BYTES,
        "affinity_cpus": characterization.preamble["affinity_cpus"],
        "canonical_sha256": evidence.canonical_sha256,
        "semantic_sha256": evidence.semantic_sha256,
        "parsimony_min": evidence.parsimony_min,
        "files": {
            filename: sha256_file(directory / filename)
            for filename in CAPTURE_INPUT_FILES
        },
    }
    return (
        json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def validate_captured_input(
    base: Manifest,
    characterization: Characterization,
    source_input: InputEvidence | None,
    fixture_sha256: str,
    process_metrics_sha256: str,
    directory: Path,
    *,
    require_readonly_directory: bool = True,
) -> InputEvidence:
    directory = require_lexical_directory(
        directory, "completed Phase-9 input capture"
    )
    if require_readonly_directory and directory.stat().st_mode & 0o222:
        fail(f"completed Phase-9 input capture is still writable: {directory}")
    expected_files = {
        *CAPTURE_INPUT_FILES,
        "status.json",
        "status.json.sha256",
    }
    observed = {path.name for path in directory.iterdir()}
    if observed != expected_files:
        fail(
            "completed Phase-9 input capture is not the exact file closure: "
            f"missing={sorted(expected_files - observed)}, "
            f"unexpected={sorted(observed - expected_files)}"
        )
    for filename in expected_files:
        member = directory / filename
        require_regular(member, f"completed input capture member {filename}")
        if member.stat().st_nlink != 1:
            fail(f"completed input capture member is externally hard-linked: {member}")
        if require_readonly_directory and member.stat().st_mode & 0o222:
            fail(f"completed input capture member is still writable: {member}")
    validate_process_metrics_receipt(
        directory / "process-metrics.txt", "completed input capture metrics"
    )
    status = directory / "status.json"
    verify_detached_seal(status)
    evidence = captured_input_evidence(directory / "canonical.json")
    expected_status = input_capture_status_bytes(
        base,
        characterization,
        fixture_sha256,
        process_metrics_sha256,
        evidence,
        directory,
    )
    if status.read_bytes() != expected_status:
        fail("completed Phase-9 input capture status changed")
    if source_input is not None:
        require_matching_input_evidence(
            source_input, evidence, "fresh frozen-oracle input execution"
        )
    return evidence


def capture_status_bytes(
    base: Manifest,
    characterization: Characterization,
    input_evidence: InputEvidence,
    fixture_sha256: str,
    process_metrics_sha256: str,
    evidence: Evidence,
    row_directory: Path,
) -> bytes:
    report = acceptance_module().parse_report(evidence.product_report)
    files = {
        filename: sha256_file(row_directory / filename)
        for filename in CAPTURE_STATUS_FILES
    }
    contract = phase9_contract_row(
        evidence.seed,
        evidence.workers,
        fixture_sha256,
        "manifest://fixture.pb.gz",
        characterization.preamble["affinity_cpus"],
    )
    value = {
        "schema": "wric_phase9_frozen_capture_row",
        "schema_version": CAPTURE_ROW_SCHEMA_VERSION,
        "row_id": row_id(evidence.seed, evidence.workers),
        "seed": evidence.seed,
        "workers": evidence.workers,
        "parent_sha256": base.sha256,
        "fixture_sha256": fixture_sha256,
        "frozen_oracle_sha256": base.preamble["frozen_oracle_dagutil_sha256"],
        "process_metrics_sha256": process_metrics_sha256,
        "input_canonical_sha256": input_evidence.canonical_sha256,
        "input_semantic_sha256": input_evidence.semantic_sha256,
        "input_parsimony_min": input_evidence.parsimony_min,
        "timeout_seconds": TIMEOUT_SECONDS,
        "rss_limit_bytes": RSS_LIMIT_BYTES,
        "affinity_cpus": characterization.preamble["affinity_cpus"],
        "canonical_argv": canonical_argv(contract),
        "canonical_argv_sha256": canonical_argv_digest(canonical_argv(contract)),
        "accepted_rebuild_ms": report.top["accepted_rebuild_ms"],
        "total_ms": report.top["total_ms"],
        "files": files,
    }
    return (
        json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def validate_captured_row(
    base: Manifest,
    characterization: Characterization,
    input_evidence: InputEvidence,
    source_evidence: Evidence | None,
    fixture_sha256: str,
    process_metrics_sha256: str,
    row_directory: Path,
    *,
    seed: int | None = None,
    workers: int | None = None,
    require_readonly_directory: bool = True,
) -> tuple[Evidence, dict[str, str]]:
    if source_evidence is not None:
        if seed is not None and seed != source_evidence.seed:
            fail("completed capture row seed disagrees with source evidence")
        if workers is not None and workers != source_evidence.workers:
            fail("completed capture row workers disagree with source evidence")
        seed, workers = source_evidence.seed, source_evidence.workers
    if seed not in SEEDS or workers not in WORKERS:
        fail("completed capture row lacks one canonical Phase-9 seed/worker identity")
    row_directory = require_lexical_directory(row_directory, "completed Phase-9 capture row")
    if require_readonly_directory and row_directory.stat().st_mode & 0o222:
        fail(f"completed Phase-9 capture row directory is still writable: {row_directory}")
    expected_files = {
        *CAPTURE_STATUS_FILES,
        "status.json",
        "status.json.sha256",
    }
    observed_files = {path.name for path in row_directory.iterdir()}
    if observed_files != expected_files:
        fail(
            f"completed Phase-9 capture row is not the exact file closure: "
            f"{row_directory}: missing={sorted(expected_files - observed_files)}, "
            f"unexpected={sorted(observed_files - expected_files)}"
        )
    for filename in expected_files:
        member = row_directory / filename
        require_regular(member, f"completed capture member {filename}")
        if member.stat().st_nlink != 1:
            fail(f"completed capture member is externally hard-linked: {member}")
        if require_readonly_directory and member.stat().st_mode & 0o222:
            fail(f"completed capture member is still writable: {member}")
    status = row_directory / "status.json"
    verify_detached_seal(status)
    validate_process_metrics_receipt(
        row_directory / "process-metrics.txt",
        f"completed capture {seed}/W{workers} chart metrics",
    )
    validate_process_metrics_receipt(
        row_directory / "output-process-metrics.txt",
        f"completed capture {seed}/W{workers} output metrics",
    )
    evidence, row = captured_evidence(
        characterization,
        input_evidence,
        seed,
        workers,
        row_directory,
    )
    expected_status = capture_status_bytes(
        base,
        characterization,
        input_evidence,
        fixture_sha256,
        process_metrics_sha256,
        evidence,
        row_directory,
    )
    if status.read_bytes() != expected_status:
        fail(f"completed Phase-9 capture row status changed: {row_directory}")
    if source_evidence is not None:
        require_matching_stable_evidence(
            source_evidence,
            evidence,
            "fresh frozen-oracle execution "
            + row_id(evidence.seed, evidence.workers),
        )
    return evidence, row


def actual_chart_command(
    oracle: Path,
    fixture: Path,
    contract: Mapping[str, str],
    row_directory: Path,
) -> list[str]:
    values = canonical_argv(contract)
    values[0] = os.fspath(oracle)
    values[2] = os.fspath(fixture)
    replacements = {
        "@search-canonical-result": os.fspath(row_directory / "canonical.json"),
        "@output": os.fspath(row_directory / "output.pb.gz"),
    }
    values = [replacements.get(token, token) for token in values]
    compact_index = values.index("--chart-spr-canonical-result")
    values[compact_index:compact_index] = [
        "--chart-spr-canonical-sidecar",
        os.fspath(row_directory / "canonical.ndjson"),
    ]
    return ["taskset", "-c", contract["affinity_cpus"], *values]


def actual_output_command(
    oracle: Path, affinity: str, row_directory: Path
) -> list[str]:
    return [
        "taskset",
        "-c",
        affinity,
        os.fspath(oracle),
        "--dag-pb",
        os.fspath(row_directory / "output.pb.gz"),
        "--force-no-vcf",
        "--validate",
        "--dag-info",
        "--canonical-dag-result",
        os.fspath(row_directory / "output-canonical.json"),
    ]


def actual_input_command(
    oracle: Path, affinity: str, fixture: Path, directory: Path
) -> list[str]:
    return [
        "taskset",
        "-c",
        affinity,
        os.fspath(oracle),
        "--dag-pb",
        os.fspath(fixture),
        "--force-no-vcf",
        "--validate",
        "--dag-info",
        "--canonical-dag-result",
        os.fspath(directory / "canonical.json"),
    ]


def run_enforced_capture_command(
    process_metrics: Path,
    command: Sequence[str],
    stdout: Path,
    stderr: Path,
    metrics: Path,
    environment: Mapping[str, str],
    label: str,
) -> None:
    """Run one command under the exact timeout/RSS contract and seal its receipt."""

    runner = [
        os.fspath(process_metrics),
        "--timeout-seconds",
        str(TIMEOUT_SECONDS),
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
        timeout=TIMEOUT_SECONDS + 60,
    )
    if result.stdout or result.stderr:
        fail(f"process-metrics emitted unexpected diagnostics for {label}")
    validate_process_metrics_receipt(metrics, f"{label} process-metrics receipt")
    if result.returncode != 0:
        fail(f"{label} failed under process-metrics: {result.returncode}")


def remove_capture_staging_directory(path: Path) -> None:
    """Discard only the private incomplete row directory after making it removable."""

    staging = require_lexical_directory(path, "incomplete Phase-9 capture row")
    for directory, directory_names, _ in os.walk(
        staging, topdown=False, followlinks=False
    ):
        for name in directory_names:
            child = Path(directory) / name
            info = child.lstat()
            if stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode):
                child.chmod(0o755)
        Path(directory).chmod(0o755)
    shutil.rmtree(staging)
    fsync_directory(staging.parent)


def run_frozen_capture_input(
    base: Manifest,
    characterization: Characterization,
    source_input: InputEvidence | None,
    fixture: Path,
    fixture_sha256: str,
    oracle: Path,
    process_metrics: Path,
    process_metrics_sha256: str,
    capture: Path,
) -> InputEvidence:
    destination = capture / "input"
    staging = capture / ".input.staging"
    if os.path.lexists(destination):
        destination = require_lexical_directory(
            destination, "completed Phase-9 input capture"
        )
        if destination.stat().st_mode & 0o222:
            validate_captured_input(
                base,
                characterization,
                source_input,
                fixture_sha256,
                process_metrics_sha256,
                destination,
                require_readonly_directory=False,
            )
            for member in destination.iterdir():
                member.chmod(0o444)
                fsync_regular_file(member)
            destination.chmod(0o555)
            fsync_directory(capture)
        result = validate_captured_input(
            base,
            characterization,
            source_input,
            fixture_sha256,
            process_metrics_sha256,
            destination,
        )
        if os.path.lexists(staging):
            remove_capture_staging_directory(staging)
        return result
    if os.path.lexists(staging):
        try:
            validate_captured_input(
                base,
                characterization,
                source_input,
                fixture_sha256,
                process_metrics_sha256,
                staging,
            )
        except BootstrapError:
            remove_capture_staging_directory(staging)
        else:
            rename_noreplace(staging, destination)
            fsync_directory(capture)
            return validate_captured_input(
                base,
                characterization,
                source_input,
                fixture_sha256,
                process_metrics_sha256,
                destination,
            )
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
            actual_input_command(
                oracle,
                characterization.preamble["affinity_cpus"],
                fixture,
                staging,
            ),
            staging / "report.txt",
            staging / "stderr.txt",
            staging / "process-metrics.txt",
            environment,
            "frozen oracle input canonical command",
        )
        for filename in CAPTURE_INPUT_FILES:
            require_regular(staging / filename, f"fresh input capture {filename}")
        evidence = captured_input_evidence(staging / "canonical.json")
        if source_input is not None:
            require_matching_input_evidence(
                source_input, evidence, "fresh frozen-oracle input execution"
            )
        status_data = input_capture_status_bytes(
            base,
            characterization,
            fixture_sha256,
            process_metrics_sha256,
            evidence,
            staging,
        )
        copy_bytes(staging / "status.json", status_data, 0o444)
        copy_bytes(
            staging / "status.json.sha256",
            seal_bytes("status.json", status_data),
            0o444,
        )
        for member in staging.iterdir():
            member.chmod(0o444)
            fsync_regular_file(member)
        staging.chmod(0o555)
        fsync_directory(staging)
        rename_noreplace(staging, destination)
        fsync_directory(capture)
    except BaseException:
        if staging.exists():
            remove_capture_staging_directory(staging)
        raise
    return validate_captured_input(
        base,
        characterization,
        source_input,
        fixture_sha256,
        process_metrics_sha256,
        destination,
    )


def run_frozen_capture_row(
    base: Manifest,
    characterization: Characterization,
    input_evidence: InputEvidence,
    source_evidence: Evidence | None,
    fixture: Path,
    fixture_sha256: str,
    oracle: Path,
    process_metrics: Path,
    process_metrics_sha256: str,
    rows_directory: Path,
    *,
    seed: int | None = None,
    workers: int | None = None,
) -> tuple[Evidence, dict[str, str]]:
    if source_evidence is not None:
        if seed is not None and seed != source_evidence.seed:
            fail("capture row seed disagrees with source evidence")
        if workers is not None and workers != source_evidence.workers:
            fail("capture row workers disagree with source evidence")
        seed, workers = source_evidence.seed, source_evidence.workers
    if seed not in SEEDS or workers not in WORKERS:
        fail("capture row lacks one canonical Phase-9 seed/worker identity")
    identity = row_id(seed, workers)
    destination = rows_directory / identity
    staging = rows_directory / f".{identity}.staging"
    if os.path.lexists(destination):
        destination = require_lexical_directory(
            destination, "completed Phase-9 capture row"
        )
        if destination.stat().st_mode & 0o222:
            # Recover the old rename-before-chmod crash window only after the
            # complete row proves exact against the sealed source.
            validate_captured_row(
                base,
                characterization,
                input_evidence,
                source_evidence,
                fixture_sha256,
                process_metrics_sha256,
                destination,
                seed=seed,
                workers=workers,
                require_readonly_directory=False,
            )
            for member in destination.iterdir():
                member.chmod(0o444)
                fsync_regular_file(member)
            destination.chmod(0o555)
            fsync_directory(rows_directory)
        result = validate_captured_row(
            base,
            characterization,
            input_evidence,
            source_evidence,
            fixture_sha256,
            process_metrics_sha256,
            destination,
            seed=seed,
            workers=workers,
        )
        if os.path.lexists(staging):
            remove_capture_staging_directory(staging)
        return result
    if os.path.lexists(staging):
        try:
            validate_captured_row(
                base,
                characterization,
                input_evidence,
                source_evidence,
                fixture_sha256,
                process_metrics_sha256,
                staging,
                seed=seed,
                workers=workers,
            )
        except BootstrapError:
            remove_capture_staging_directory(staging)
        else:
            rename_noreplace(staging, destination)
            fsync_directory(rows_directory)
            return validate_captured_row(
                base,
                characterization,
                input_evidence,
                source_evidence,
                fixture_sha256,
                process_metrics_sha256,
                destination,
                seed=seed,
                workers=workers,
            )
    staging.mkdir(mode=0o755)
    contract = phase9_contract_row(
        seed,
        workers,
        fixture_sha256,
        "manifest://fixture.pb.gz",
        characterization.preamble["affinity_cpus"],
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
            actual_chart_command(oracle, fixture, contract, staging),
            staging / "report.txt",
            staging / "stderr.txt",
            staging / "process-metrics.txt",
            environment,
            f"frozen oracle chart command {identity}",
        )
        run_enforced_capture_command(
            process_metrics,
            actual_output_command(
                oracle, characterization.preamble["affinity_cpus"], staging
            ),
            staging / "output-report.txt",
            staging / "output-stderr.txt",
            staging / "output-process-metrics.txt",
            environment,
            f"frozen oracle output command {identity}",
        )
        for filename in CAPTURE_STATUS_FILES:
            require_regular(staging / filename, f"fresh capture {identity} {filename}")
        evidence, row = captured_evidence(
            characterization,
            input_evidence,
            seed,
            workers,
            staging,
        )
        if source_evidence is not None:
            require_matching_stable_evidence(
                source_evidence,
                evidence,
                f"fresh frozen-oracle execution {identity}",
            )
        status_data = capture_status_bytes(
            base,
            characterization,
            input_evidence,
            fixture_sha256,
            process_metrics_sha256,
            evidence,
            staging,
        )
        copy_bytes(staging / "status.json", status_data, 0o444)
        copy_bytes(
            staging / "status.json.sha256",
            seal_bytes("status.json", status_data),
            0o444,
        )
        for path in staging.iterdir():
            path.chmod(0o444)
            fsync_regular_file(path)
        staging.chmod(0o555)
        fsync_directory(staging)
        rename_noreplace(staging, destination)
        fsync_directory(rows_directory)
    except BaseException:
        if staging.exists():
            remove_capture_staging_directory(staging)
        raise
    return validate_captured_row(
        base,
        characterization,
        input_evidence,
        source_evidence,
        fixture_sha256,
        process_metrics_sha256,
        destination,
        seed=seed,
        workers=workers,
    )


def capture_frozen_characterization(
    capture_dir: Path,
    base: Manifest,
    characterization: Characterization,
    source_input: InputEvidence,
    source_evidence: Sequence[Evidence],
    fixture: Path,
    fixture_sha256: str,
    oracle: Path,
    process_metrics: Path,
    process_metrics_sha256: str,
) -> tuple[InputEvidence, list[Evidence], list[dict[str, str]], Path]:
    if sha256_file(oracle) != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("frozen oracle changed before Phase-9 capture")
    if sha256_file(fixture) != fixture_sha256:
        fail("Phase-9 fixture changed before frozen capture")
    if sha256_file(process_metrics) != process_metrics_sha256:
        fail("process-metrics runner changed before Phase-9 capture")
    capture = initialize_capture_directory(
        capture_dir,
        base,
        characterization,
        source_input,
        fixture_sha256,
        process_metrics_sha256,
    )
    input_evidence = run_frozen_capture_input(
        base,
        characterization,
        source_input,
        fixture,
        fixture_sha256,
        oracle,
        process_metrics,
        process_metrics_sha256,
        capture,
    )
    rows_directory = capture / "rows"
    source = {(item.seed, item.workers): item for item in source_evidence}
    evidence: list[Evidence] = []
    mappings: list[dict[str, str]] = []
    for seed in SEEDS:
        for workers in WORKERS:
            item, mapping = run_frozen_capture_row(
                base,
                characterization,
                input_evidence,
                source[(seed, workers)],
                fixture,
                fixture_sha256,
                oracle,
                process_metrics,
                process_metrics_sha256,
                rows_directory,
            )
            evidence.append(item)
            mappings.append(mapping)
    unexpected = sorted(
        path.name
        for path in rows_directory.iterdir()
        if path.name not in {row_id(seed, worker) for seed in SEEDS for worker in WORKERS}
    )
    if unexpected:
        fail(f"Phase-9 capture rows directory has unexpected artifacts: {unexpected}")
    if os.path.lexists(capture / ".input.staging"):
        fail("Phase-9 input capture left an interrupted staging directory")
    if sha256_file(oracle) != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("frozen oracle changed during Phase-9 capture")
    if sha256_file(fixture) != fixture_sha256:
        fail("Phase-9 fixture changed during frozen capture")
    if sha256_file(process_metrics) != process_metrics_sha256:
        fail("process-metrics runner changed during Phase-9 capture")
    validate_worker_independent_evidence(evidence)
    return input_evidence, evidence, mappings, capture


def produce_frozen_characterization_capture(
    capture_dir: Path,
    base: Manifest,
    characterization: Characterization,
    fixture: Path,
    fixture_sha256: str,
    oracle: Path,
    process_metrics: Path,
    process_metrics_sha256: str,
) -> tuple[InputEvidence, list[Evidence], list[dict[str, str]], Path]:
    """Run and resume the 1/7/19 x W1/2/4/8 source characterization."""

    oracle_sha256 = base.preamble["frozen_oracle_dagutil_sha256"]
    if sha256_file(oracle) != oracle_sha256:
        fail("frozen oracle changed before Phase-9 characterization")
    if sha256_file(fixture) != fixture_sha256:
        fail("Phase-9 fixture changed before characterization")
    if sha256_file(process_metrics) != process_metrics_sha256:
        fail("process-metrics runner changed before Phase-9 characterization")
    capture = initialize_characterization_capture_directory(
        capture_dir,
        base,
        characterization,
        fixture_sha256,
        process_metrics_sha256,
    )
    input_evidence = run_frozen_capture_input(
        base,
        characterization,
        None,
        fixture,
        fixture_sha256,
        oracle,
        process_metrics,
        process_metrics_sha256,
        capture,
    )
    evidence: list[Evidence] = []
    mappings: list[dict[str, str]] = []
    rows_directory = capture / "rows"
    for seed in SEEDS:
        for workers in WORKERS:
            item, mapping = run_frozen_capture_row(
                base,
                characterization,
                input_evidence,
                None,
                fixture,
                fixture_sha256,
                oracle,
                process_metrics,
                process_metrics_sha256,
                rows_directory,
                seed=seed,
                workers=workers,
            )
            evidence.append(item)
            mappings.append(mapping)
    wanted_rows = {row_id(seed, workers) for seed in SEEDS for workers in WORKERS}
    unexpected = sorted(
        path.name for path in rows_directory.iterdir() if path.name not in wanted_rows
    )
    if unexpected:
        fail(
            "Phase-9 characterization rows directory has unexpected artifacts: "
            f"{unexpected}"
        )
    if os.path.lexists(capture / ".input.staging"):
        fail("Phase-9 characterization left an interrupted input staging directory")
    if sha256_file(oracle) != oracle_sha256:
        fail("frozen oracle changed during Phase-9 characterization")
    if sha256_file(fixture) != fixture_sha256:
        fail("Phase-9 fixture changed during characterization")
    if sha256_file(process_metrics) != process_metrics_sha256:
        fail("process-metrics runner changed during Phase-9 characterization")
    validate_worker_independent_evidence(evidence)
    return input_evidence, evidence, mappings, capture


def shell_command_tokens(row: Mapping[str, str]) -> list[str]:
    values = canonical_argv(row)
    values[0] = '"$oracle"'
    values[2] = '"$assets/fixture.pb.gz"'
    replacements = {
        "@search-canonical-result": f'"$out/{row["row_id"]}.canonical.json"',
        "@output": f'"$out/{row["row_id"]}.pb.gz"',
    }
    values = [replacements.get(token, token) for token in values]
    compact_index = values.index("--chart-spr-canonical-result")
    values[compact_index:compact_index] = [
        "--chart-spr-canonical-sidecar",
        f'"$out/{row["row_id"]}.canonical.ndjson"',
    ]
    return values


def render_commands(
    rows: Sequence[Mapping[str, str]],
    oracle_sha: str,
    affinity: str,
    ledger_sha: str,
    archived_characterization: str,
) -> bytes:
    if LEDGER_RELATIVE.fullmatch(archived_characterization) is None:
        fail("archived characterization path is not a canonical asset path")
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        "assets=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd -P)",
        f"readonly expected_ledger_sha256={ledger_sha}",
        f"readonly expected_oracle_sha256={oracle_sha}",
        f"readonly archived_characterization={archived_characterization}",
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
    wanted_order = [
        row_id(seed, workers) for seed in SEEDS for workers in WORKERS
    ]
    observed_order = [row["row_id"] for row in manifest.rows]
    if observed_order != wanted_order:
        fail("supplement rows are not in canonical seed/worker order")
    primary_hashes: set[str] = set()
    affinities: set[str] = set()
    for (seed, workers), row in rows.items():
        validate_affinity(row["affinity_cpus"], f"supplement row {row['row_id']}")
        contract = phase9_contract_row(
            seed,
            workers,
            row["primary_sha256"],
            row["primary_uri"],
            row["affinity_cpus"],
        )
        for key, wanted in contract.items():
            if wanted == "-":
                continue
            if row[key] != wanted:
                fail(f"supplement row {row['row_id']} {key}={row[key]!r}, expected {wanted!r}")
        unsigned_expected = (
            "expected_active_patterns",
            "expected_initial_clades",
            "expected_initial_productions",
            "expected_candidates_generated",
            "expected_candidates_scored",
            "expected_exact_verifications",
            "expected_iterations",
            "expected_accepted_moves",
            "expected_initial_score",
            "expected_final_score",
            "expected_validated_parsimony",
        )
        if any(not row[field].isdigit() for field in unsigned_expected):
            fail(f"supplement row has non-unsigned expected evidence: {row['row_id']}")
        if int(row["expected_active_patterns"]) < MIN_ACTIVE_PATTERNS:
            fail(f"supplement row lacks active-pattern evidence: {row['row_id']}")
        exact_counts: Mapping[str, int] = {
            "expected_candidates_generated": EXPECTED_CANDIDATES,
            "expected_candidates_scored": EXPECTED_CANDIDATES,
            "expected_exact_verifications": EXPECTED_EXACT,
            "expected_iterations": ITERATIONS,
            "expected_accepted_moves": MIN_ACCEPTED_MOVES,
        }
        for field, exact_wanted in exact_counts.items():
            if row[field] != str(exact_wanted):
                fail(
                    f"supplement row {row['row_id']} {field}={row[field]}, "
                    f"expected exactly {exact_wanted}"
                )
        if int(row["expected_validated_parsimony"]) >= int(
            row["expected_initial_score"]
        ):
            fail(
                "supplement external canonical score did not strictly improve: "
                f"{row['row_id']}"
            )
        for field in (
            "primary_sha256",
            "oracle_search_semantic_sha256",
            "oracle_output_semantic_sha256",
            "oracle_trial_semantic_sha256",
            "canonical_sidecar_sha256",
            "oracle_report_sha256",
            "canonical_argv_sha256",
        ):
            validate_hash(row[field], f"supplement row {row['row_id']} {field}")
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


def command_declaration(commands: Path, name: str, pattern: re.Pattern[str]) -> str:
    prefix = f"readonly {name}="
    try:
        lines = commands.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        fail(f"Phase-9 commands asset is not UTF-8: {error}")
    values = [line.removeprefix(prefix) for line in lines if line.startswith(prefix)]
    if len(values) != 1 or pattern.fullmatch(values[0]) is None:
        fail(f"Phase-9 commands asset lacks one canonical {name} declaration")
    return values[0]


def audit_exact_asset_tree(root: Path, expected_files: set[str]) -> None:
    """Reject unledgered files/directories, aliases, and external hard links."""

    root = require_lexical_directory(root, "Phase-9 supplement asset directory")
    observed_files: set[str] = set()
    observed_directories: set[str] = set()
    for directory_text, directory_names, file_names in os.walk(
        root, followlinks=False
    ):
        directory = Path(directory_text)
        for name in directory_names:
            path = directory / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
                fail(f"Phase-9 asset tree has a non-directory alias: {path}")
            relative = path.relative_to(root).as_posix()
            if LEDGER_RELATIVE.fullmatch(relative) is None:
                fail(f"Phase-9 asset directory path is not canonical: {relative}")
            observed_directories.add(relative)
        for name in file_names:
            path = directory / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                fail(f"Phase-9 asset tree has a non-regular alias: {path}")
            if info.st_nlink != 1:
                fail(f"Phase-9 asset tree member is externally hard-linked: {path}")
            relative = path.relative_to(root).as_posix()
            if LEDGER_RELATIVE.fullmatch(relative) is None:
                fail(f"Phase-9 asset file path is not canonical: {relative}")
            observed_files.add(relative)

    expected_directories: set[str] = set()
    for relative in expected_files:
        parent = Path(relative).parent
        while parent != Path("."):
            expected_directories.add(parent.as_posix())
            parent = parent.parent
    if observed_files != expected_files or observed_directories != expected_directories:
        fail(
            "Phase-9 asset directory is not the exact sealed closure: "
            f"missing_files={sorted(expected_files - observed_files)}, "
            f"extra_files={sorted(observed_files - expected_files)}, "
            f"missing_directories={sorted(expected_directories - observed_directories)}, "
            f"extra_directories={sorted(observed_directories - expected_directories)}"
        )


def characterization_capture_asset_members(
    capture: Path,
    fixture: Path,
) -> dict[str, bytes]:
    """Copy the exact durable capture closure into publication-owned bytes."""

    members: dict[str, bytes] = {"fixture.pb.gz": fixture.read_bytes()}

    def add(relative: str, source: Path) -> None:
        if LEDGER_RELATIVE.fullmatch(relative) is None:
            fail(f"characterization asset path is not canonical: {relative!r}")
        if relative in members:
            fail(f"duplicate characterization asset path: {relative}")
        require_regular(source, f"characterization capture asset {relative}")
        if source.stat().st_nlink != 1:
            fail(f"characterization capture asset is externally hard-linked: {source}")
        members[relative] = source.read_bytes()

    for filename in ("capture-contract.json", "capture-contract.json.sha256"):
        add(f"provenance/{filename}", capture / filename)
    for filename in (*CAPTURE_INPUT_FILES, "status.json", "status.json.sha256"):
        add(f"input/{filename}", capture / "input" / filename)
    for seed in SEEDS:
        for workers in WORKERS:
            identity = row_id(seed, workers)
            for filename in (
                *CAPTURE_STATUS_FILES,
                "status.json",
                "status.json.sha256",
            ):
                add(
                    f"rows/{identity}/{filename}",
                    capture / "rows" / identity / filename,
                )
    return members


def render_characterization_publication(
    publication: PublicationPaths,
    contract: Characterization,
    input_evidence: InputEvidence,
    captured_rows: Sequence[Mapping[str, str]],
    capture: Path,
    fixture: Path,
) -> tuple[dict[str, bytes], bytes, bytes]:
    """Render a schema-v3 TSV whose paths commit to the full capture ledger."""

    closure_members = characterization_capture_asset_members(capture, fixture)
    ledger = asset_ledger_bytes(closure_members)
    ledger_sha256 = sha256_bytes(ledger)
    closure = CHAR_CLOSURE_PREFIX + ledger_sha256
    assets_relative = publication.assets.name
    asset_files = {
        f"{closure}/{relative}": data
        for relative, data in closure_members.items()
    }
    asset_files[f"{closure}/{CHAR_CLOSURE_LEDGER}"] = ledger

    preamble = dict(contract.preamble)
    preamble["input_canonical_path"] = (
        f"{assets_relative}/{closure}/input/canonical.json"
    )
    preamble["input_canonical_sha256"] = input_evidence.canonical_sha256
    rows: list[dict[str, str]] = []
    for captured in captured_rows:
        row = dict(captured)
        identity = row_id(int(row["seed"]), int(row["workers"]))
        paths = {
            "product_report": "report.txt",
            "canonical_sidecar": "canonical.ndjson",
            "canonical_result": "canonical.json",
            "output_canonical": "output-canonical.json",
        }
        for prefix, filename in paths.items():
            row[f"{prefix}_path"] = (
                f"{assets_relative}/{closure}/rows/{identity}/{filename}"
            )
        rows.append(row)
    manifest_data = characterization_tsv_bytes(preamble, rows)
    return (
        asset_files,
        manifest_data,
        seal_bytes(publication.output.name, manifest_data),
    )


def read_characterization_asset_ledger(
    assets: Path,
    closure: str,
) -> tuple[Path, dict[str, str]]:
    match = re.fullmatch(rf"{re.escape(CHAR_CLOSURE_PREFIX)}([0-9a-f]{{64}})", closure)
    if match is None:
        fail("Phase-9 characterization closure is not hash-addressed")
    closure_path = require_lexical_directory(
        assets / closure, "Phase-9 characterization closure directory"
    )
    ledger = resolve_lexical_regular(
        closure_path,
        Path(CHAR_CLOSURE_LEDGER),
        "Phase-9 characterization asset ledger",
    )
    if sha256_file(ledger) != match.group(1):
        fail("Phase-9 characterization closure name differs from its ledger hash")
    pattern = re.compile(
        r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)"
    )
    try:
        lines = ledger.read_text(encoding="ascii").splitlines()
    except UnicodeDecodeError as error:
        fail(f"Phase-9 characterization asset ledger is not ASCII: {error}")
    members: dict[str, str] = {}
    for line_number, line in enumerate(lines, 1):
        parsed = pattern.fullmatch(line)
        if parsed is None:
            fail(
                "Phase-9 characterization asset ledger line "
                f"{line_number} is not canonical"
            )
        digest, relative = parsed.groups()
        if relative in members or relative == CHAR_CLOSURE_LEDGER:
            fail(
                "Phase-9 characterization asset ledger has a duplicate/circular "
                f"member: {relative}"
            )
        member = resolve_lexical_regular(
            closure_path,
            Path(relative),
            f"Phase-9 characterization asset {relative}",
        )
        if sha256_file(member) != digest:
            fail(f"Phase-9 characterization asset hash mismatch: {relative}")
        members[relative] = digest
    expected = b"".join(
        f"{members[name]}  {name}\n".encode("ascii")
        for name in sorted(members)
    )
    if ledger.read_bytes() != expected:
        fail("Phase-9 characterization asset ledger is not canonical and sorted")
    return closure_path, members


def audit_characterization_publication(
    base_path: Path,
    expected_parent_sha256: str,
    characterization_path: Path,
    fixture_path: Path,
    expected_fixture_sha256: str,
    process_metrics_path: Path,
    expected_process_metrics_sha256: str,
    affinity: str,
    root: Path,
) -> Characterization:
    """Audit every byte in a produced characterization and its provenance."""

    root = repo_root(root)
    for value, label in (
        (expected_parent_sha256, "expected parent SHA-256"),
        (expected_fixture_sha256, "expected fixture SHA-256"),
        (expected_process_metrics_sha256, "expected process-metrics SHA-256"),
    ):
        validate_hash(value, label)
    validate_affinity(affinity, "characterization command")
    base = read_manifest(base_path, root, expected_kind="base")
    if base.sha256 != expected_parent_sha256:
        fail("sealed base hash differs from the characterization parent")
    if base.preamble["parent_sha256"] != "-":
        fail("base workload manifest must declare parent_sha256=-")
    fixture = require_canonical_regular(fixture_path, "Phase-9 fixture")
    if fixture.stat().st_nlink != 1:
        fail("Phase-9 fixture must not be externally hard-linked")
    if sha256_file(fixture) != expected_fixture_sha256:
        fail("Phase-9 fixture differs from its expected characterization hash")
    process_metrics = require_canonical_regular(
        process_metrics_path,
        "process-metrics runner",
        executable=True,
    )
    if process_metrics.stat().st_nlink != 1:
        fail("process-metrics runner must not be externally hard-linked")
    if sha256_file(process_metrics) != expected_process_metrics_sha256:
        fail("process-metrics runner differs from its expected frozen hash")
    oracle = resolve_manifest_uri(
        base.path, base.preamble["frozen_oracle_dagutil_uri"], root
    )
    require_regular(oracle, "base frozen oracle", executable=True)
    if sha256_file(oracle) != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("base frozen oracle changed before characterization audit")

    characterization = read_characterization(characterization_path)
    exact_preamble = {
        "parent_sha256": base.sha256,
        "primary_sha256": expected_fixture_sha256,
        "frozen_oracle_sha256": base.preamble[
            "frozen_oracle_dagutil_sha256"
        ],
        "affinity_cpus": affinity,
    }
    for key, wanted in exact_preamble.items():
        if characterization.preamble[key] != wanted:
            fail(f"produced characterization {key} differs from its command contract")

    publication = publication_paths(characterization.path)
    assets = require_lexical_directory(
        publication.assets, "Phase-9 characterization assets"
    )
    evidence_paths = [characterization.preamble["input_canonical_path"]]
    evidence_paths.extend(
        row[f"{prefix}_path"]
        for row in characterization.rows.values()
        for prefix in (
            "product_report",
            "canonical_sidecar",
            "canonical_result",
            "output_canonical",
        )
    )
    prefix_parts: set[tuple[str, str]] = set()
    for relative_text in evidence_paths:
        parts = Path(relative_text).parts
        if len(parts) < 3:
            fail("produced characterization evidence path lacks its sealed closure")
        prefix_parts.add((parts[0], parts[1]))
    if len(prefix_parts) != 1:
        fail("produced characterization evidence paths span multiple closures")
    assets_name, closure = next(iter(prefix_parts))
    if assets_name != assets.name:
        fail("produced characterization evidence path names a foreign asset root")
    closure_path, ledger_members = read_characterization_asset_ledger(
        assets, closure
    )

    expected_members = {"fixture.pb.gz", "provenance/capture-contract.json", "provenance/capture-contract.json.sha256"}
    expected_members.update(
        f"input/{filename}"
        for filename in (*CAPTURE_INPUT_FILES, "status.json", "status.json.sha256")
    )
    for seed in SEEDS:
        for workers in WORKERS:
            identity = row_id(seed, workers)
            expected_members.update(
                f"rows/{identity}/{filename}"
                for filename in (
                    *CAPTURE_STATUS_FILES,
                    "status.json",
                    "status.json.sha256",
                )
            )
    if set(ledger_members) != expected_members:
        fail(
            "Phase-9 characterization ledger is not the exact capture closure: "
            f"missing={sorted(expected_members - set(ledger_members))}, "
            f"unexpected={sorted(set(ledger_members) - expected_members)}"
        )
    expected_tree = {
        f"{closure}/{relative}" for relative in expected_members
    } | {f"{closure}/{CHAR_CLOSURE_LEDGER}"}
    audit_exact_asset_tree(assets, expected_tree)
    archived_fixture = closure_path / "fixture.pb.gz"
    if (
        sha256_file(archived_fixture) != expected_fixture_sha256
        or archived_fixture.read_bytes() != fixture.read_bytes()
    ):
        fail("characterization-owned fixture differs from the requested fixture")

    contract_characterization = characterization_contract(
        base,
        expected_fixture_sha256,
        affinity,
        closure_path,
    )
    contract_path = closure_path / "provenance/capture-contract.json"
    verify_detached_seal(contract_path)
    expected_contract = characterization_capture_contract_bytes(
        base,
        contract_characterization,
        expected_fixture_sha256,
        expected_process_metrics_sha256,
    )
    if contract_path.read_bytes() != expected_contract:
        fail("Phase-9 characterization capture contract changed")

    input_evidence = validate_captured_input(
        base,
        contract_characterization,
        None,
        expected_fixture_sha256,
        expected_process_metrics_sha256,
        closure_path / "input",
        require_readonly_directory=False,
    )
    published_input = exact_input_evidence(characterization)
    require_matching_input_evidence(
        published_input,
        input_evidence,
        "published Phase-9 characterization input",
    )
    wanted_input_path = f"{assets.name}/{closure}/input/canonical.json"
    if characterization.preamble["input_canonical_path"] != wanted_input_path:
        fail("published Phase-9 input canonical path is not canonical")

    published_evidence: list[Evidence] = []
    for seed in SEEDS:
        for workers in WORKERS:
            identity = row_id(seed, workers)
            captured, captured_row = validate_captured_row(
                base,
                contract_characterization,
                input_evidence,
                None,
                expected_fixture_sha256,
                expected_process_metrics_sha256,
                closure_path / "rows" / identity,
                seed=seed,
                workers=workers,
                require_readonly_directory=False,
            )
            published = exact_evidence(
                characterization,
                characterization.rows[(seed, workers)],
                published_input,
            )
            require_matching_stable_evidence(
                captured,
                published,
                f"published Phase-9 characterization {identity}",
            )
            row = characterization.rows[(seed, workers)]
            path_names = {
                "product_report": "report.txt",
                "canonical_sidecar": "canonical.ndjson",
                "canonical_result": "canonical.json",
                "output_canonical": "output-canonical.json",
            }
            for prefix, filename in path_names.items():
                expected_path = (
                    f"{assets.name}/{closure}/rows/{identity}/{filename}"
                )
                if row[f"{prefix}_path"] != expected_path:
                    fail(
                        f"published Phase-9 characterization {identity} "
                        f"{prefix}_path is not canonical"
                    )
                if row[f"{prefix}_sha256"] != captured_row[f"{prefix}_sha256"]:
                    fail(
                        f"published Phase-9 characterization {identity} "
                        f"{prefix}_sha256 differs from captured evidence"
                    )
            for field in (
                "canonical_argv_sha256",
                "oracle_search_semantic_sha256",
                "oracle_output_semantic_sha256",
                "oracle_trial_semantic_sha256",
            ):
                if row[field] != captured_row[field]:
                    fail(
                        f"published Phase-9 characterization {identity} "
                        f"{field} differs from captured evidence"
                    )
            published_evidence.append(published)
    validate_worker_independent_evidence(published_evidence)
    return characterization


def audit_capture_contract(
    contract_path: Path,
    base: Manifest,
    characterization: Characterization,
    fixture_sha256: str,
) -> tuple[Characterization, InputEvidence, str]:
    verify_detached_seal(contract_path)
    contract = read_json_object(contract_path, "Phase-9 capture contract")
    expected_keys = {
        "schema",
        "schema_version",
        "parent_sha256",
        "source_characterization_basename",
        "source_characterization_sha256",
        "source_characterization_seal_sha256",
        "source_input_canonical_sha256",
        "source_input_semantic_sha256",
        "source_input_parsimony_min",
        "fixture_sha256",
        "frozen_oracle_sha256",
        "process_metrics_sha256",
        "timeout_seconds",
        "rss_limit_bytes",
        "affinity_cpus",
        "rows",
    }
    if set(contract) != expected_keys:
        fail("Phase-9 capture contract schema keys changed")
    if (
        contract["schema"] != CAPTURE_SCHEMA
        or contract["schema_version"] != CAPTURE_SCHEMA_VERSION
        or contract["parent_sha256"] != base.sha256
        or contract["source_characterization_basename"]
        != characterization.path.name
        or contract["fixture_sha256"] != fixture_sha256
        or contract["frozen_oracle_sha256"]
        != base.preamble["frozen_oracle_dagutil_sha256"]
        or contract["timeout_seconds"] != TIMEOUT_SECONDS
        or contract["rss_limit_bytes"] != RSS_LIMIT_BYTES
        or contract["affinity_cpus"]
        != characterization.preamble["affinity_cpus"]
    ):
        fail("Phase-9 capture contract disagrees with its audited archive")
    for field in (
        "source_characterization_sha256",
        "source_characterization_seal_sha256",
        "source_input_canonical_sha256",
        "source_input_semantic_sha256",
        "process_metrics_sha256",
    ):
        if not isinstance(contract[field], str):
            fail(f"Phase-9 capture contract {field} is not a string")
        validate_hash(str(contract[field]), f"Phase-9 capture contract {field}")
    source_basename = str(contract["source_characterization_basename"])
    source_path = resolve_lexical_regular(
        contract_path.parent,
        Path("source") / source_basename,
        "archived source characterization",
    )
    source_digest = verify_detached_seal(source_path)
    source_seal = source_path.with_name(source_path.name + ".sha256")
    if (
        source_digest != contract["source_characterization_sha256"]
        or sha256_file(source_seal)
        != contract["source_characterization_seal_sha256"]
    ):
        fail("archived source characterization differs from the capture contract")
    source_characterization = read_characterization(source_path)
    source_input = exact_input_evidence(source_characterization)
    if (
        source_input.canonical_sha256 != contract["source_input_canonical_sha256"]
        or source_input.semantic_sha256
        != contract["source_input_semantic_sha256"]
        or type(contract["source_input_parsimony_min"]) is not int
        or source_input.parsimony_min != contract["source_input_parsimony_min"]
    ):
        fail("archived source input canonical differs from the capture contract")
    source_preamble = source_characterization.preamble
    if (
        source_preamble["schema"] != CHAR_SCHEMA
        or source_preamble["schema_version"] != CHAR_SCHEMA_VERSION
        or source_preamble["parent_sha256"] != base.sha256
        or source_preamble["primary_sha256"] != fixture_sha256
        or source_preamble["frozen_oracle_sha256"]
        != base.preamble["frozen_oracle_dagutil_sha256"]
        or source_preamble["affinity_cpus"]
        != characterization.preamble["affinity_cpus"]
    ):
        fail("archived source characterization contract/preamble changed")
    expected_rows = []
    for seed in SEEDS:
        for workers in WORKERS:
            row = phase9_contract_row(
                seed,
                workers,
                fixture_sha256,
                "manifest://fixture.pb.gz",
                characterization.preamble["affinity_cpus"],
            )
            expected_rows.append(
                {
                    "row_id": row_id(seed, workers),
                    "canonical_argv_sha256": canonical_argv_digest(canonical_argv(row)),
                }
            )
    if contract["rows"] != expected_rows:
        fail("Phase-9 capture contract row/argv matrix changed")
    return source_characterization, source_input, str(
        contract["process_metrics_sha256"]
    )


def audit_supplement_asset_ledger(
    base: Manifest, manifest: Manifest, root: Path
) -> tuple[Characterization, InputEvidence, dict[tuple[int, int], Evidence], str]:
    commands = resolve_manifest_uri(manifest.path, manifest.preamble["commands_uri"], root)
    require_regular(commands, "Phase-9 commands asset", executable=True)
    ledger_sha = command_declaration(commands, "expected_ledger_sha256", HEX64)
    oracle_sha = command_declaration(commands, "expected_oracle_sha256", HEX64)
    if oracle_sha != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("Phase-9 commands asset changes the base frozen-oracle hash")
    archived_relative = command_declaration(
        commands, "archived_characterization", LEDGER_RELATIVE
    )
    if Path(archived_relative).parent != Path("archive"):
        fail("Phase-9 archived characterization must be archive/<original-basename>")
    ledger = resolve_lexical_regular(
        commands.parent, Path("assets.sha256"), "Phase-9 asset ledger"
    )
    if sha256_file(ledger) != ledger_sha:
        fail("Phase-9 asset ledger differs from the commands-bound hash")

    seen: dict[str, str] = {}
    pattern = re.compile(
        r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)"
    )
    try:
        ledger_lines = ledger.read_text(encoding="ascii").splitlines()
    except UnicodeDecodeError as error:
        fail(f"Phase-9 asset ledger is not ASCII: {error}")
    for line_number, line in enumerate(ledger_lines, 1):
        match = pattern.fullmatch(line)
        if match is None:
            fail(f"Phase-9 asset ledger line {line_number} is not canonical")
        digest, relative_text = match.groups()
        if relative_text in seen or relative_text in ("assets.sha256", "commands.sh"):
            fail(f"Phase-9 asset ledger has a duplicate/circular member: {relative_text}")
        member = resolve_lexical_regular(
            commands.parent,
            Path(relative_text),
            f"Phase-9 asset ledger member {relative_text}",
        )
        if sha256_file(member) != digest:
            fail(f"Phase-9 asset ledger member hash mismatch: {relative_text}")
        seen[relative_text] = digest
    expected_ledger = b"".join(
        f"{seen[name]}  {name}\n".encode("ascii") for name in sorted(seen)
    )
    if ledger.read_bytes() != expected_ledger:
        fail("Phase-9 asset ledger is not the canonical sorted byte stream")

    characterization_path = resolve_lexical_regular(
        commands.parent,
        Path(archived_relative),
        "supplement-owned archived characterization",
    )
    characterization = read_characterization(characterization_path)
    if characterization.preamble["parent_sha256"] != base.sha256:
        fail("archived characterization parent differs from the sealed base")
    if (
        characterization.preamble["frozen_oracle_sha256"]
        != base.preamble["frozen_oracle_dagutil_sha256"]
    ):
        fail("archived characterization oracle differs from the sealed base")

    fixture = resolve_lexical_regular(
        commands.parent, Path("fixture.pb.gz"), "archived Phase-9 fixture"
    )
    fixture_sha256 = sha256_file(fixture)
    if characterization.preamble["primary_sha256"] != fixture_sha256:
        fail("archived characterization fixture hash differs from archived fixture")

    input_evidence = exact_input_evidence(characterization)
    evidence: dict[tuple[int, int], Evidence] = {}
    for seed in SEEDS:
        for workers in WORKERS:
            key = (seed, workers)
            evidence[key] = exact_evidence(
                characterization, characterization.rows[key], input_evidence
            )
    validate_worker_independent_evidence(list(evidence.values()))

    archive_prefix = Path(archived_relative).parent
    contract_path = resolve_lexical_regular(
        commands.parent,
        Path("provenance/capture-contract.json"),
        "archived Phase-9 capture contract",
    )
    (
        source_characterization,
        source_input,
        process_metrics_sha256,
    ) = audit_capture_contract(contract_path, base, characterization, fixture_sha256)
    require_matching_input_evidence(
        source_input, input_evidence, "archived Phase-9 input canonical evidence"
    )
    source_evidence: dict[tuple[int, int], Evidence] = {}
    for seed in SEEDS:
        for workers in WORKERS:
            key = (seed, workers)
            source_evidence[key] = exact_evidence(
                source_characterization,
                source_characterization.rows[key],
                source_input,
            )
    validate_worker_independent_evidence(list(source_evidence.values()))
    for key, fresh in evidence.items():
        require_matching_stable_evidence(
            source_evidence[key], fresh, f"archived Phase-9 evidence {row_id(*key)}"
        )

    required = {
        "fixture.pb.gz",
        archived_relative,
        archived_relative + ".sha256",
        "provenance/capture-contract.json",
        "provenance/capture-contract.json.sha256",
        f"provenance/source/{characterization.path.name}",
        f"provenance/source/{characterization.path.name}.sha256",
        (archive_prefix / characterization.preamble["input_canonical_path"]).as_posix(),
        (
            Path("provenance/source")
            / source_characterization.preamble["input_canonical_path"]
        ).as_posix(),
    }
    required.update(
        f"provenance/input/{filename}"
        for filename in (*CAPTURE_INPUT_FILES, "status.json", "status.json.sha256")
    )
    path_fields = (
        "product_report_path",
        "canonical_sidecar_path",
        "canonical_result_path",
        "output_canonical_path",
    )
    for key, row in characterization.rows.items():
        for field in path_fields:
            required.add((archive_prefix / row[field]).as_posix())
        stem = row_id(*key)
        required.update(
            f"provenance/{stem}/{filename}"
            for filename in (*CAPTURE_STATUS_FILES, "status.json", "status.json.sha256")
        )
    source_prefix = Path("provenance/source")
    for row in source_characterization.rows.values():
        for field in path_fields:
            required.add((source_prefix / row[field]).as_posix())
    if set(seen) != required:
        fail(
            "Phase-9 asset ledger is not the exact archived/provenance closure: "
            f"missing={sorted(required - set(seen))}, "
            f"unexpected={sorted(set(seen) - required)}"
        )
    audit_exact_asset_tree(
        commands.parent, required | {"assets.sha256", "commands.sh"}
    )

    captured_input = validate_captured_input(
        base,
        characterization,
        source_input,
        fixture_sha256,
        process_metrics_sha256,
        commands.parent / "provenance/input",
        require_readonly_directory=False,
    )
    capture_input_path = commands.parent / "provenance/input/canonical.json"
    if capture_input_path.read_bytes() != input_evidence.canonical.read_bytes():
        fail("archived input canonical is not the captured input byte stream")
    if (capture_input_path.stat().st_dev, capture_input_path.stat().st_ino) == (
        input_evidence.canonical.stat().st_dev,
        input_evidence.canonical.stat().st_ino,
    ):
        fail("archived input canonical is hard-linked to mutable provenance")
    require_matching_input_evidence(
        input_evidence, captured_input, "captured Phase-9 input canonical evidence"
    )

    capture_names = {
        "product_report": "report.txt",
        "canonical_sidecar": "canonical.ndjson",
        "canonical_result": "canonical.json",
        "output_canonical": "output-canonical.json",
    }
    for (seed, workers), fresh in evidence.items():
        source = source_evidence[(seed, workers)]
        provenance = commands.parent / "provenance" / row_id(seed, workers)
        captured, _ = validate_captured_row(
            base,
            characterization,
            input_evidence,
            source,
            fixture_sha256,
            process_metrics_sha256,
            provenance,
            require_readonly_directory=False,
        )
        for prefix, filename in capture_names.items():
            archive_digest = getattr(fresh, f"{prefix}_sha256")
            archive_path = getattr(fresh, prefix)
            capture_path = provenance / filename
            capture_digest = sha256_file(capture_path)
            if capture_digest != archive_digest:
                fail(
                    f"archived evidence is not the captured row byte stream: "
                    f"{row_id(seed, workers)} {prefix}"
                )
            archive_info = archive_path.stat()
            capture_info = capture_path.stat()
            if (archive_info.st_dev, archive_info.st_ino) == (
                capture_info.st_dev,
                capture_info.st_ino,
            ):
                fail(
                    f"archived evidence is hard-linked to mutable provenance: "
                    f"{row_id(seed, workers)} {prefix}"
                )
        if captured.expected != fresh.expected:
            raise AssertionError("validate_captured_row failed to reconcile evidence")

    asset_name = commands.parent.name
    rows = expected_phase9_rows(manifest.rows)
    ordered_rows = [rows[(seed, workers)] for seed in SEEDS for workers in WORKERS]
    expected_commands = render_commands(
        ordered_rows,
        base.preamble["frozen_oracle_dagutil_sha256"],
        characterization.preamble["affinity_cpus"],
        ledger_sha,
        archived_relative,
    )
    if commands.read_bytes() != expected_commands:
        fail("Phase-9 commands asset differs from the exact rendered byte stream")
    for key, item in evidence.items():
        char_row = characterization.rows[key]
        wanted = make_manifest_row(
            item,
            fixture_sha256,
            f"manifest://{asset_name}/fixture.pb.gz",
            "manifest://"
            + (Path(asset_name) / archive_prefix / char_row["canonical_sidecar_path"]).as_posix(),
            "manifest://"
            + (Path(asset_name) / archive_prefix / char_row["canonical_result_path"]).as_posix(),
            characterization.preamble["affinity_cpus"],
        )
        if dict(rows[key]) != wanted:
            differing = sorted(
                field for field in MANIFEST_HEADER if rows[key][field] != wanted[field]
            )
            fail(
                f"supplement row is not exactly derived from archived evidence: "
                f"{row_id(*key)} fields={differing}"
            )
    return characterization, input_evidence, evidence, process_metrics_sha256


def audited_frozen_characterization(
    base_path: Path,
    expected_parent: str,
    supplement_path: Path,
    root: Path,
) -> AuditedFrozenCharacterization:
    """Return frozen evidence only after auditing its sealed, supplement-owned chain."""

    root = repo_root(root)
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
    (
        characterization,
        input_evidence,
        evidence,
        process_metrics_sha256,
    ) = audit_supplement_asset_ledger(base, supplement, root)
    return AuditedFrozenCharacterization(
        base,
        supplement,
        characterization,
        input_evidence,
        evidence,
        process_metrics_sha256,
    )


def audit_supplement(
    base_path: Path,
    expected_parent: str,
    supplement_path: Path,
    root: Path,
) -> AuditedFrozenCharacterization:
    """Compatibility name for the public sealed-evidence audit API."""

    return audited_frozen_characterization(
        base_path, expected_parent, supplement_path, root
    )


HARNESS_SENTINEL_GROUP = "__phase9_sealed_supplement_validation_sentinel__"


def validate_with_benchmark_harness(
    audit: AuditedFrozenCharacterization,
    harness_path: Path,
    process_metrics_path: Path,
    root: Path,
) -> None:
    """Make the production harness parse the complete base+supplement chain."""

    harness = require_canonical_regular(
        harness_path, "benchmark harness", executable=True
    )
    expected_harness = require_canonical_regular(
        TOOLS_DIRECTORY / "wric_spr_search_benchmark.sh",
        "repository benchmark harness",
        executable=True,
    )
    if harness != expected_harness:
        fail("--benchmark-harness is not the repository production harness")
    process_metrics = require_canonical_regular(
        process_metrics_path, "process-metrics runner", executable=True
    )
    if process_metrics.stat().st_nlink != 1:
        fail("process-metrics runner must not be externally hard-linked")
    if sha256_file(process_metrics) != audit.process_metrics_sha256:
        fail("process-metrics runner differs from the capture-contract binary hash")
    oracle = resolve_manifest_uri(
        audit.base.path, audit.base.preamble["frozen_oracle_dagutil_uri"], root
    )
    frozen_larch2 = resolve_manifest_uri(
        audit.base.path, audit.base.preamble["frozen_larch2_uri"], root
    )
    require_regular(oracle, "base frozen oracle", executable=True)
    require_regular(frozen_larch2, "base frozen larch2", executable=True)
    sentinel_output = audit.supplement.path.parent / (
        "." + audit.supplement.path.name + ".harness-validation"
    )
    if os.path.lexists(sentinel_output):
        fail(f"benchmark-validation sentinel output already exists: {sentinel_output}")
    environment = {
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "TZ": "Europe/Sofia",
        "WRIC_REPO_ROOT": os.fspath(root),
    }
    result = subprocess.run(
        [
            os.fspath(harness),
            "--dagutil",
            os.fspath(oracle),
            "--larch2",
            os.fspath(frozen_larch2),
            "--process-metrics",
            os.fspath(process_metrics),
            "--out-dir",
            os.fspath(sentinel_output),
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
        env=environment,
        cwd=REPOSITORY_ROOT,
        timeout=60,
    )
    if os.path.lexists(sentinel_output):
        fail("benchmark harness created output before the validation sentinel")
    expected = f"error: manifest group has no rows: {HARNESS_SENTINEL_GROUP}"
    if result.returncode == 0 or expected not in result.stderr.splitlines():
        detail = result.stderr.strip().splitlines()
        fail(
            "benchmark harness did not reach the post-validation sentinel: "
            + (detail[-1] if detail else f"exit={result.returncode}")
        )


def build(args: argparse.Namespace) -> None:
    """Build while one canonical output lock owns recovery through rollback."""

    output = args.output.absolute()
    output_parent = durable_mkdir_parents(
        output.parent, "Phase-9 output parent directory"
    )
    output = output_parent / output.name
    with exclusive_output_lock(output) as publication:
        root = command_base_repo_root(args)
        validate_hash(args.expected_parent_sha256, "expected parent SHA-256")

        def validate_private_bundle(staged_output: Path) -> None:
            audited = audit_supplement(
                args.base_manifest,
                args.expected_parent_sha256,
                staged_output,
                root,
            )
            validate_with_benchmark_harness(
                audited, args.benchmark_harness, args.process_metrics, root
            )

        recovered_complete = recover_interrupted_publication(
            publication, prepublish_validator=validate_private_bundle
        )
        if recovered_complete:
            audited = audit_supplement(
                args.base_manifest,
                args.expected_parent_sha256,
                publication.output,
                root,
            )
            validate_with_benchmark_harness(
                audited, args.benchmark_harness, args.process_metrics, root
            )
            finish_publication(publication)
            return
        existing = [
            path
            for path in (publication.output, publication.assets, publication.seal)
            if os.path.lexists(path)
        ]
        if existing:
            rendered = existing[0]
            fail(f"exclusive Phase-9 output already exists: {rendered}")
        _build_with_output_lock(args, publication)


def _build_with_output_lock(
    args: argparse.Namespace, publication: PublicationPaths
) -> None:
    root = command_base_repo_root(args)
    output = publication.output
    assets = publication.assets
    seal = publication.seal
    validate_hash(args.expected_parent_sha256, "expected parent SHA-256")
    validate_hash(args.expected_fixture_sha256, "expected fixture SHA-256")
    base = read_manifest(args.base_manifest, root, expected_kind="base")
    if base.sha256 != args.expected_parent_sha256:
        fail(f"sealed base hash differs from --expected-parent-sha256: {base.sha256}")
    if base.preamble["parent_sha256"] != "-":
        fail("base workload manifest must declare parent_sha256=-")
    characterization = read_characterization(args.characterization)
    if characterization.preamble["parent_sha256"] != base.sha256:
        fail("characterization parent hash differs from the sealed base")
    if characterization.preamble["frozen_oracle_sha256"] != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("characterization frozen oracle hash differs from sealed base role")
    fixture = require_canonical_regular(args.fixture, "Phase-9 fixture")
    if fixture.stat().st_nlink != 1:
        fail("Phase-9 fixture must not be externally hard-linked")
    fixture_sha = sha256_file(fixture)
    if fixture_sha != args.expected_fixture_sha256:
        fail(f"Phase-9 fixture hash mismatch: {fixture_sha}")
    if characterization.preamble["primary_sha256"] != fixture_sha:
        fail("characterization primary hash differs from Phase-9 fixture")
    source_input = exact_input_evidence(characterization)
    source_evidence = [
        exact_evidence(
            characterization,
            characterization.rows[(seed, workers)],
            source_input,
        )
        for seed in SEEDS
        for workers in WORKERS
    ]
    validate_worker_independent_evidence(source_evidence)

    process_metrics = require_canonical_regular(
        args.process_metrics, "process-metrics runner", executable=True
    )
    if process_metrics.stat().st_nlink != 1:
        fail("process-metrics runner must not be externally hard-linked")
    process_metrics_sha256 = sha256_file(process_metrics)

    frozen_larch2 = resolve_manifest_uri(base.path, base.preamble["frozen_larch2_uri"], root)
    frozen_oracle = resolve_manifest_uri(
        base.path, base.preamble["frozen_oracle_dagutil_uri"], root
    )
    require_regular(frozen_larch2, "base frozen larch2", executable=True)
    require_regular(frozen_oracle, "base frozen oracle", executable=True)

    capture_absolute = args.capture_dir.absolute()
    for reserved in dataclasses.astuple(publication):
        if (
            capture_absolute == reserved
            or capture_absolute.is_relative_to(reserved)
            or reserved.is_relative_to(capture_absolute)
        ):
            fail(
                "--capture-dir must be disjoint from the complete supplement "
                "publication namespace"
            )
    with exclusive_capture_lock(args.capture_dir):
        input_evidence, evidence, captured_rows, capture = capture_frozen_characterization(
            args.capture_dir,
            base,
            characterization,
            source_input,
            source_evidence,
            fixture,
            fixture_sha,
            frozen_oracle,
            process_metrics,
            process_metrics_sha256,
        )
    captured_by_key = {
        (int(row["seed"]), int(row["workers"])): row for row in captured_rows
    }

    archived_name = characterization.path.name
    archived_relative = f"archive/{archived_name}"
    if LEDGER_RELATIVE.fullmatch(archived_relative) is None:
        fail("characterization basename cannot be represented in the sealed asset ledger")
    archive_rows: list[dict[str, str]] = []
    for seed in SEEDS:
        for workers in WORKERS:
            fresh = dict(captured_by_key[(seed, workers)])
            source_row = characterization.rows[(seed, workers)]
            for prefix in (
                "product_report",
                "canonical_sidecar",
                "canonical_result",
                "output_canonical",
            ):
                fresh[f"{prefix}_path"] = source_row[f"{prefix}_path"]
            archive_rows.append(fresh)
    archived_preamble = dict(characterization.preamble)
    archived_preamble["input_canonical_sha256"] = input_evidence.canonical_sha256
    archived_characterization = characterization_tsv_bytes(
        archived_preamble, archive_rows
    )

    asset_files: dict[str, bytes] = {}

    def add_asset(relative: str, data: bytes) -> None:
        if LEDGER_RELATIVE.fullmatch(relative) is None:
            fail(f"asset path cannot be represented canonically: {relative!r}")
        if relative in asset_files:
            fail(f"archive asset path collision: {relative}")
        asset_files[relative] = data

    add_asset("fixture.pb.gz", fixture.read_bytes())
    add_asset(archived_relative, archived_characterization)
    add_asset(
        archived_relative + ".sha256",
        seal_bytes(archived_name, archived_characterization),
    )
    add_asset(
        f"provenance/source/{archived_name}",
        characterization.path.read_bytes(),
    )
    add_asset(
        f"provenance/source/{archived_name}.sha256",
        characterization.path.with_name(
            characterization.path.name + ".sha256"
        ).read_bytes(),
    )
    add_asset(
        (
            Path("provenance/source")
            / characterization.preamble["input_canonical_path"]
        ).as_posix(),
        source_input.canonical.read_bytes(),
    )
    for item in source_evidence:
        source_row = characterization.rows[(item.seed, item.workers)]
        source_members = {
            source_row["product_report_path"]: item.product_report,
            source_row["canonical_sidecar_path"]: item.canonical_sidecar,
            source_row["canonical_result_path"]: item.canonical_result,
            source_row["output_canonical_path"]: item.output_canonical,
        }
        for relative, source_path in source_members.items():
            add_asset(
                (Path("provenance/source") / relative).as_posix(),
                source_path.read_bytes(),
            )
    evidence_by_key = {(item.seed, item.workers): item for item in evidence}
    add_asset(
        (
            Path("archive")
            / characterization.preamble["input_canonical_path"]
        ).as_posix(),
        input_evidence.canonical.read_bytes(),
    )
    for row in archive_rows:
        key = (int(row["seed"]), int(row["workers"]))
        item = evidence_by_key[key]
        archived_sources = {
            row["product_report_path"]: item.product_report,
            row["canonical_sidecar_path"]: item.canonical_sidecar,
            row["canonical_result_path"]: item.canonical_result,
            row["output_canonical_path"]: item.output_canonical,
        }
        for relative, source_path in archived_sources.items():
            add_asset((Path("archive") / relative).as_posix(), source_path.read_bytes())

    for filename in ("capture-contract.json", "capture-contract.json.sha256"):
        add_asset(f"provenance/{filename}", (capture / filename).read_bytes())
    for filename in (*CAPTURE_INPUT_FILES, "status.json", "status.json.sha256"):
        add_asset(
            f"provenance/input/{filename}",
            (capture / "input" / filename).read_bytes(),
        )
    for seed in SEEDS:
        for workers in WORKERS:
            stem = row_id(seed, workers)
            captured = capture / "rows" / stem
            for filename in (*CAPTURE_STATUS_FILES, "status.json", "status.json.sha256"):
                add_asset(
                    f"provenance/{stem}/{filename}",
                    (captured / filename).read_bytes(),
                )
    ledger = asset_ledger_bytes(asset_files)
    ledger_sha = sha256_bytes(ledger)
    asset_files["assets.sha256"] = ledger

    asset_name = assets.name
    rows: list[dict[str, str]] = []
    for item in evidence:
        archived_row = next(
            row
            for row in archive_rows
            if int(row["seed"]) == item.seed and int(row["workers"]) == item.workers
        )
        rows.append(
            make_manifest_row(
                item,
                fixture_sha,
                f"manifest://{asset_name}/fixture.pb.gz",
                "manifest://"
                + (
                    Path(asset_name)
                    / "archive"
                    / archived_row["canonical_sidecar_path"]
                ).as_posix(),
                "manifest://"
                + (
                    Path(asset_name)
                    / "archive"
                    / archived_row["canonical_result_path"]
                ).as_posix(),
                characterization.preamble["affinity_cpus"],
            )
        )
    commands = render_commands(
        rows,
        base.preamble["frozen_oracle_dagutil_sha256"],
        characterization.preamble["affinity_cpus"],
        ledger_sha,
        archived_relative,
    )
    asset_files["commands.sh"] = commands
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

    seal_data = seal_bytes(output.name, manifest_data)
    ownership = PublicationOwnership()

    def validate_private_bundle(staged_output: Path) -> None:
        audited = audit_supplement(
            args.base_manifest, args.expected_parent_sha256, staged_output, root
        )
        validate_with_benchmark_harness(
            audited, args.benchmark_harness, process_metrics, root
        )

    try:
        publish_immutable_supplement(
            publication,
            asset_files,
            manifest_data,
            seal_data,
            ownership=ownership,
            prepublish_validator=validate_private_bundle,
        )
        audited = audit_supplement(
            args.base_manifest, args.expected_parent_sha256, output, root
        )
        validate_with_benchmark_harness(
            audited, args.benchmark_harness, process_metrics, root
        )
        finish_publication(publication)
    except BaseException as error:
        try:
            rollback_owned_publication(publication, ownership)
        except BaseException as rollback_error:
            raise BootstrapError(
                "Phase-9 publication rollback failed closed after "
                f"{error}: {rollback_error}"
            ) from error
        raise


def characterize(args: argparse.Namespace) -> None:
    """Produce or validate one immutable frozen-oracle characterization."""

    output = args.output.absolute()
    output_parent = durable_mkdir_parents(
        output.parent, "Phase-9 characterization output parent"
    )
    output = output_parent / output.name
    with exclusive_output_lock(output) as publication:
        root = command_base_repo_root(args)

        def audit_output(path: Path) -> Characterization:
            return audit_characterization_publication(
                args.base_manifest,
                args.expected_parent_sha256,
                path,
                args.fixture,
                args.expected_fixture_sha256,
                args.process_metrics,
                args.expected_process_metrics_sha256,
                args.affinity_cpus,
                root,
            )

        recovered_complete = recover_interrupted_publication(
            publication,
            prepublish_validator=lambda path: audit_output(path),
        )
        if recovered_complete:
            audit_output(publication.output)
            finish_publication(publication)
            return
        existing = [
            path
            for path in (publication.output, publication.assets, publication.seal)
            if os.path.lexists(path)
        ]
        if existing:
            if len(existing) == len(PUBLICATION_COMPONENTS):
                audit_output(publication.output)
                return
            fail(
                "exclusive Phase-9 characterization output already exists: "
                f"{existing[0]}"
            )
        _characterize_with_output_lock(args, publication, root)


def _characterize_with_output_lock(
    args: argparse.Namespace,
    publication: PublicationPaths,
    root: Path,
) -> None:
    for value, label in (
        (args.expected_parent_sha256, "expected parent SHA-256"),
        (args.expected_fixture_sha256, "expected fixture SHA-256"),
        (
            args.expected_process_metrics_sha256,
            "expected process-metrics SHA-256",
        ),
    ):
        validate_hash(value, label)
    validate_affinity(args.affinity_cpus, "characterization command")
    base = read_manifest(args.base_manifest, root, expected_kind="base")
    if base.sha256 != args.expected_parent_sha256:
        fail("sealed base hash differs from --expected-parent-sha256")
    if base.preamble["parent_sha256"] != "-":
        fail("base workload manifest must declare parent_sha256=-")
    fixture = require_canonical_regular(args.fixture, "Phase-9 fixture")
    if fixture.stat().st_nlink != 1:
        fail("Phase-9 fixture must not be externally hard-linked")
    fixture_sha256 = sha256_file(fixture)
    if fixture_sha256 != args.expected_fixture_sha256:
        fail("Phase-9 fixture differs from --expected-fixture-sha256")
    process_metrics = require_canonical_regular(
        args.process_metrics,
        "process-metrics runner",
        executable=True,
    )
    if process_metrics.stat().st_nlink != 1:
        fail("process-metrics runner must not be externally hard-linked")
    process_metrics_sha256 = sha256_file(process_metrics)
    if process_metrics_sha256 != args.expected_process_metrics_sha256:
        fail("process-metrics runner differs from --expected-process-metrics-sha256")
    oracle = resolve_manifest_uri(
        base.path, base.preamble["frozen_oracle_dagutil_uri"], root
    )
    require_regular(oracle, "base frozen oracle", executable=True)
    if sha256_file(oracle) != base.preamble["frozen_oracle_dagutil_sha256"]:
        fail("base frozen oracle changed before characterization")

    capture_absolute = args.capture_dir.absolute()
    for reserved in dataclasses.astuple(publication):
        if (
            capture_absolute == reserved
            or capture_absolute.is_relative_to(reserved)
            or reserved.is_relative_to(capture_absolute)
        ):
            fail(
                "--capture-dir must be disjoint from the complete "
                "characterization publication namespace"
            )
    contract = characterization_contract(
        base,
        fixture_sha256,
        args.affinity_cpus,
        capture_absolute,
    )
    with exclusive_capture_lock(args.capture_dir):
        input_evidence, _evidence, captured_rows, capture = (
            produce_frozen_characterization_capture(
                args.capture_dir,
                base,
                contract,
                fixture,
                fixture_sha256,
                oracle,
                process_metrics,
                process_metrics_sha256,
            )
        )
    asset_files, manifest_data, seal_data = render_characterization_publication(
        publication,
        contract,
        input_evidence,
        captured_rows,
        capture,
        fixture,
    )
    ownership = PublicationOwnership()

    def audit_output(path: Path) -> None:
        audit_characterization_publication(
            args.base_manifest,
            args.expected_parent_sha256,
            path,
            fixture,
            args.expected_fixture_sha256,
            process_metrics,
            args.expected_process_metrics_sha256,
            args.affinity_cpus,
            root,
        )

    try:
        publish_immutable_supplement(
            publication,
            asset_files,
            manifest_data,
            seal_data,
            ownership=ownership,
            prepublish_validator=audit_output,
        )
        audit_output(publication.output)
        finish_publication(publication)
    except BaseException as error:
        try:
            rollback_owned_publication(publication, ownership)
        except BaseException as rollback_error:
            raise BootstrapError(
                "Phase-9 characterization rollback failed closed after "
                f"{error}: {rollback_error}"
            ) from error
        raise


def print_characterization_template() -> None:
    print(f"# schema={CHAR_SCHEMA}")
    print(f"# schema_version={CHAR_SCHEMA_VERSION}")
    print("# parent_sha256=<sealed-base-manifest-sha256>")
    print("# primary_sha256=<64-lowercase-hex>")
    print("# input_canonical_path=<normalized-relative-canonical-dag-json>")
    print("# input_canonical_sha256=<64-lowercase-hex>")
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
    build_parser.add_argument(
        "--capture-dir",
        type=Path,
        required=True,
        help=(
            "persistent restartable directory for the input canonical plus "
            "12 live frozen-oracle search captures"
        ),
    )
    build_parser.add_argument("--output", type=Path, required=True)
    build_roots = build_parser.add_mutually_exclusive_group()
    build_roots.add_argument(
        "--base-repo-root",
        type=Path,
        help="canonical Git worktree containing sealed Phase-0 repo:// assets",
    )
    build_roots.add_argument(
        "--repo-root",
        type=Path,
        help="backward-compatible same-root alias for --base-repo-root",
    )
    build_parser.add_argument("--benchmark-harness", type=Path, required=True)
    build_parser.add_argument("--process-metrics", type=Path, required=True)
    characterize_parser = subparsers.add_parser(
        "characterize",
        help="produce the sealed 12-row frozen-oracle source characterization",
    )
    characterize_parser.add_argument("--base-manifest", type=Path, required=True)
    characterize_parser.add_argument("--expected-parent-sha256", required=True)
    characterize_parser.add_argument("--fixture", type=Path, required=True)
    characterize_parser.add_argument("--expected-fixture-sha256", required=True)
    characterize_parser.add_argument("--affinity-cpus", required=True)
    characterize_parser.add_argument(
        "--capture-dir",
        type=Path,
        required=True,
        help="persistent restartable source-characterization capture directory",
    )
    characterize_parser.add_argument("--output", type=Path, required=True)
    characterize_roots = characterize_parser.add_mutually_exclusive_group()
    characterize_roots.add_argument(
        "--base-repo-root",
        type=Path,
        help="canonical Git worktree containing sealed Phase-0 repo:// assets",
    )
    characterize_roots.add_argument(
        "--repo-root",
        type=Path,
        help="backward-compatible same-root alias for --base-repo-root",
    )
    characterize_parser.add_argument("--process-metrics", type=Path, required=True)
    characterize_parser.add_argument(
        "--expected-process-metrics-sha256",
        required=True,
    )
    audit_parser = subparsers.add_parser("audit", help="audit a sealed Phase-9 supplement")
    audit_parser.add_argument("--base-manifest", type=Path, required=True)
    audit_parser.add_argument("--expected-parent-sha256", required=True)
    audit_parser.add_argument("--supplement", type=Path, required=True)
    audit_roots = audit_parser.add_mutually_exclusive_group()
    audit_roots.add_argument(
        "--base-repo-root",
        type=Path,
        help="canonical Git worktree containing sealed Phase-0 repo:// assets",
    )
    audit_roots.add_argument(
        "--repo-root",
        type=Path,
        help="backward-compatible same-root alias for --base-repo-root",
    )
    audit_parser.add_argument("--benchmark-harness", type=Path, required=True)
    audit_parser.add_argument("--process-metrics", type=Path, required=True)
    return parser.parse_args(argv)


def command_base_repo_root(args: argparse.Namespace) -> Path:
    """Select the canonical flag or its legacy alias without self-anchoring."""

    return base_repo_root(
        getattr(args, "base_repo_root", None) or getattr(args, "repo_root", None)
    )


def main(argv: Sequence[str]) -> int:
    try:
        args = parse_args(argv)
        if args.command == "print-characterization-template":
            print_characterization_template()
        elif args.command == "build":
            build(args)
        elif args.command == "characterize":
            characterize(args)
        elif args.command == "audit":
            root = command_base_repo_root(args)
            audited = audit_supplement(
                args.base_manifest,
                args.expected_parent_sha256,
                args.supplement,
                root,
            )
            validate_with_benchmark_harness(
                audited,
                args.benchmark_harness,
                args.process_metrics,
                root,
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
        subprocess.SubprocessError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
