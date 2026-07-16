#!/usr/bin/env python3
"""Focused end-to-end trust tests for the Phase-7/8 supplement builder."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import stat
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

REPO = Path(__file__).resolve().parents[1]
TOOLS = REPO / "tools"
TESTS = REPO / "test"
sys.path[:0] = [os.fspath(TOOLS), os.fspath(TESTS)]

import wric_phase78_manifest_bootstrap as bootstrap  # noqa: E402
import wric_phase9_manifest_bootstrap_test as phase9_test  # noqa: E402


HELPER = TOOLS / "wric_phase78_manifest_bootstrap.py"
HARNESS = TOOLS / "wric_spr_search_benchmark.sh"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def replace_oracle(case: phase9_test.Integration) -> None:
    high_sha = bootstrap.PROFILES["phase7"].fixtures[0].sha256
    script = f'''#!/usr/bin/python3
import hashlib
import json
import pathlib
import sys

args = sys.argv[1:]
if "--help" in args:
    print("  --canonical-dag-result PATH")
    print("  --chart-spr-canonical-result PATH")
    raise SystemExit(0)

def value(flag):
    return args[args.index(flag) + 1]

def digest(label):
    return hashlib.sha256(label.encode()).hexdigest()

def write_json(path, value):
    pathlib.Path(path).write_text(json.dumps(value, separators=(",", ":"), sort_keys=True) + "\\n")

input_path = pathlib.Path(value("--dag-pb"))
if "--dag-info" in args:
    payload = input_path.read_bytes()
    if payload.startswith(b"fake-output:"):
        input_sha = payload.decode().strip().split(":", 1)[1]
    else:
        input_sha = hashlib.sha256(payload).hexdigest()
    # Phase-7 lower-bound reports use a composite score domain (8284 below),
    # deliberately distinct from externally validated DAG parsimony.
    parsimony = 9415 if input_sha == {high_sha!r} else 174
    write_json(value("--canonical-dag-result"), {{
        "schema": "larch.dag.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
        "semantic_sha256": digest("dag:" + input_sha),
        "clades_sha256": digest("clades:" + input_sha),
        "productions_sha256": digest("productions:" + input_sha),
        "clade_count": 1023 if input_sha == {high_sha!r} else 139,
        "production_count": 511 if input_sha == {high_sha!r} else 69,
        "parsimony_min": parsimony,
    }})
    print("dag_info:")
    print(f"  parsimony_min: score:{{parsimony}}")
    raise SystemExit(0)

input_sha = hashlib.sha256(input_path.read_bytes()).hexdigest()
policy = value("--wric-lazy-chart")
workers = int(value("--chart-spr-workers"))
source = value("--chart-spr-candidate-source")
phase8 = source == "sampled_tree"
candidates = int(value("--chart-spr-max-candidates"))
top_k = int(value("--chart-spr-top-k-exact"))
active = 2046 if input_sha == {high_sha!r} else 113
score = 8284 if input_sha == {high_sha!r} else 174
refinement = "EXACT" if input_sha == {high_sha!r} else "BOUNDED_REFINED_GRAMMAR"
acceptance = value("--chart-spr-acceptance")
objective = "fixed_topology_exact" if phase8 else "composite_lower_bound_heuristic"
topology = "deterministic_selector:first_reachable_overlay_topology" if phase8 else "none"
contract = {{
    "record": "contract",
    "acceptance": acceptance,
    "objective": objective,
    "candidate_selection": "lower_bound_top_k",
    "candidate_source": source,
    "topology_selection": topology,
    "commit_mode": "overlay_delta",
    "accepted_state_update": "materialize_rebuild",
    "verification_mode": "transient",
    "chain_per_accept_exactness": "none_conservative_materialize_rebuild",
    "keep_mask_contract": "exact_required",
    "candidate_cap_semantics": "post_dedup",
    "max_iterations": 1,
    "max_candidates": candidates,
    "top_k_exact": top_k,
    "seed": 1,
    "polytomy_max_shapes": 1,
    "refinement_exactness": refinement,
    "score_ua_edge": False,
    "use_bound_pruning": True,
    "require_exact_keep_mask": True,
    "randomize_order": False,
    "reservoir_sample": False,
    "include_immediate_reversals": False,
}}
records = [
    {{"record": "schema", "schema": "larch.chart_spr.semantic.ndjson", "schema_version": 1}},
    contract,
    {{"record": "initial_state", "active_patterns": active, "initial_score": score}},
    {{"record": "iteration_begin", "iteration": 0, "state_score_before": score}},
]
for index in range(candidates):
    signature = f"candidate-{{index}}"
    records.append({{
        "record": "candidate",
        "iteration": 0,
        "stream_index": index,
        "signature": signature,
    }})
    records.append({{
        "record": "candidate_lower_bound",
        "iteration": 0,
        "stream_index": index,
        "signature": signature,
        "lower_bound": score,
    }})
ranked_streams = [0]
for rank, stream_index in enumerate(ranked_streams):
    records.append({{
        "record": "candidate_rank",
        "iteration": 0,
        "rank": rank,
        "stream_index": stream_index,
        "signature": f"candidate-{{stream_index}}",
    }})
if phase8:
    records.extend([
        {{
            "record": "candidate_exact_verification_rank",
            "iteration": 0,
            "exact_verification_index": 0,
            "stream_index": 0,
            "signature": "candidate-0",
        }},
        {{
            "record": "candidate_exact",
            "iteration": 0,
            "stream_index": 0,
            "signature": "candidate-0",
            "exact_score": score,
        }},
        {{
            "record": "exact_evidence",
            "scope": "candidate",
            "iteration": 0,
            "stream_index": 0,
            "evidence_kind": "fixed_topology_certificate",
            "keep_mask_kind": "not_applicable_fixed_topology",
            "keep_production_exact": False,
        }},
        {{
            "record": "fixed_topology_before_production",
            "scope": "candidate",
            "iteration": 0,
            "stream_index": 0,
            "production_key": "before-root",
        }},
        {{
            "record": "fixed_topology_after_production",
            "scope": "candidate",
            "iteration": 0,
            "stream_index": 0,
            "production_key": "after-root",
        }},
    ])
records.extend([
    {{
        "record": "iteration_outcome",
        "iteration": 0,
        "generation_stop_reason": "candidate_cap",
        "candidates_generated": candidates,
        "candidates_scored": candidates,
        "candidates_exact_verified": top_k,
        "selected_signature": "",
        "accepted_move_committed": False,
        "state_score_after": score,
    }},
    {{"record": "chain_base_production", "production_key": "base-root"}},
    {{"record": "final_state", "final_score": score, "accepted_moves": 0}},
    {{"record": "final_clade", "clade_key": "root-clade"}},
    {{"record": "final_production", "production_key": "final-root"}},
])
record_sections = {{
    "schema": "contract",
    "contract": "contract",
    "initial_state": "contract",
    "candidate": "candidates",
    "candidate_lower_bound": "candidates",
    "candidate_rank": "candidates",
    "candidate_exact_verification_rank": "exact",
    "candidate_exact": "exact",
    "exact_evidence": "exact",
    "fixed_topology_before_production": "exact",
    "fixed_topology_after_production": "exact",
    "iteration_begin": "acceptance",
    "iteration_outcome": "acceptance",
    "chain_base_production": "chain",
    "final_state": "final_topology",
    "final_clade": "final_topology",
    "final_production": "final_topology",
}}
section_chunks = {{
    section: []
    for section in (
        "contract", "candidates", "exact", "acceptance", "chain", "final_topology"
    )
}}
sidecar_chunks = []
for item in records:
    raw = (json.dumps(item, separators=(",", ":"), sort_keys=True) + "\\n").encode()
    sidecar_chunks.append(raw)
    section_chunks[record_sections[item["record"]]].append(raw)
sidecar = b"".join(sidecar_chunks)
pathlib.Path(value("--chart-spr-canonical-sidecar")).write_bytes(sidecar)
search_sha = hashlib.sha256(sidecar).hexdigest()
compact = {{
    "schema": "larch.chart_spr.semantic_digest",
    "schema_version": 1,
    "digest_algorithm": "sha256",
    "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
    "semantic_sha256": search_sha,
    "record_count": len(records),
    "candidate_count": candidates,
    "exact_candidate_count": top_k,
    "iteration_count": 1,
}}
for section, chunks in section_chunks.items():
    compact[section + "_sha256"] = hashlib.sha256(b"".join(chunks)).hexdigest()
write_json(value("--chart-spr-canonical-result"), compact)
pathlib.Path(value("-o")).write_text("fake-output:" + input_sha + "\\n")

cache = "lazy_multisite_chart" if policy == "on" else "all_active_patterns"
total = "120.000" if input_sha != {high_sha!r} and policy == "on" else "100.000"
if input_sha == {high_sha!r} and policy == "on":
    total = "150.000"
initial_chart = "120.000" if input_sha == {high_sha!r} and policy == "on" else "80.000"
generation = "120.000" if phase8 else "10.000"
top = {{
    "acceptance": acceptance,
    "objective": objective,
    "candidate_selection": "lower_bound_top_k",
    "candidate_source": source,
    "candidate_cap_semantics": "post-dedup",
    "topology_selector": "first_reachable_overlay_topology" if phase8 else "none",
    "requested_max_iterations": "1",
    "configured_max_candidates": str(candidates),
    "top_k_exact_verify": str(top_k),
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
    "memory_budget_bytes": str(12 * 1024**3),
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
    "refinement_exactness": refinement,
    "cache_strategy": cache,
    "effective_pattern_batch_size": str(active),
    "final_compaction_exactness_kind": "none",
    "chain_per_accept_exactness_label": "none_conservative_materialize_rebuild",
    "active_patterns": str(active),
    "initial_grammar_clades": "1023" if input_sha == {high_sha!r} else "139",
    "initial_grammar_productions": "511" if input_sha == {high_sha!r} else "69",
    "candidates_generated": str(candidates),
    "candidates_scored": str(candidates),
    "exact_verifications": str(top_k),
    "iterations": "1",
    "accepted_moves": "0",
    "initial_score": str(score),
    "final_score": str(score),
    "initial_chart_construction_ms": initial_chart,
    "candidate_generation_ms": generation,
    "total_ms": total,
}}
if policy == "on":
    top["lazy_internal_structural_class_ratio"] = "0.125"
    top["lazy_merge_ratio"] = "0.10"
print("chart_spr_search:")
for key, item in top.items():
    print(f"  {{key}}: {{item}}")
print("  iteration_reports:")
print("    - iteration: 0")
print("      candidate_generation:")
print("        stop_reason: candidate_cap")
'''
    case.oracle.chmod(0o755)
    phase9_test.write_bytes(case.oracle, script.encode(), 0o555)
    lines = case.base.read_text().splitlines()
    prefix = "# frozen_oracle_dagutil_sha256="
    lines = [
        prefix + phase9_test.file_digest(case.oracle) if line.startswith(prefix) else line
        for line in lines
    ]
    phase9_test.write_bytes(case.base, ("\n".join(lines) + "\n").encode())
    phase9_test.seal(case.base)
    case.parent_sha = phase9_test.file_digest(case.base)


def wrap_process_metrics(case: phase9_test.Integration) -> Path:
    real_runner = case.runner
    invocation_log = case.root / "process-metrics-invocations.jsonl"
    wrapper = case.root / "process-metrics-wrapper"
    script = f'''#!/usr/bin/python3
import json
import os
import sys

with open({os.fspath(invocation_log)!r}, "a", encoding="utf-8") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
os.execv({os.fspath(real_runner)!r}, [{os.fspath(real_runner)!r}, *sys.argv[1:]])
'''
    phase9_test.write_bytes(wrapper, script.encode(), 0o555)
    case.runner = wrapper
    return invocation_log


def create_fixture_source(root: Path) -> tuple[Path, str]:
    """Create a stable tracked source worktree independent of the sealed base."""

    source = root / "tracked-fixture-source"
    source.mkdir()
    subprocess.run(["git", "init", "-q", source], check=True)
    subprocess.run(
        ["git", "-C", os.fspath(source), "config", "user.email", "phase78@example.invalid"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", os.fspath(source), "config", "user.name", "Phase 78 test"],
        check=True,
    )
    relative_paths = sorted(
        {
            relative
            for profile in bootstrap.PROFILES.values()
            for fixture in profile.fixtures
            for relative in (fixture.relative_path, fixture.ref_relative_path)
            if relative is not None
        }
    )
    for relative in relative_paths:
        destination = source / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / relative, destination)
        destination.chmod(0o644)
    subprocess.run(
        ["git", "-C", os.fspath(source), "add", "--", *relative_paths], check=True
    )
    subprocess.run(
        ["git", "-C", os.fspath(source), "commit", "-q", "-m", "tracked fixtures"],
        check=True,
    )
    revision = subprocess.run(
        ["git", "-C", os.fspath(source), "rev-parse", "--verify", "HEAD"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()
    return source, revision


def command(
    case: phase9_test.Integration,
    profile: str,
    action: str,
    output: Path,
) -> list[str]:
    common = [
        sys.executable,
        os.fspath(HELPER),
        action,
        "--profile",
        profile,
        "--repo-root",
        os.fspath(case.base_repo_root),
        "--base-manifest",
        os.fspath(case.base),
        "--expected-parent-sha256",
        case.parent_sha,
        "--benchmark-harness",
        os.fspath(HARNESS),
        "--process-metrics",
        os.fspath(case.runner),
        "--expected-process-metrics-sha256",
        phase9_test.file_digest(case.runner),
    ]
    if action == "build":
        common.extend(
            [
                "--fixture-source-root",
                os.fspath(case.fixture_source_root),
                "--expected-fixture-source-revision",
                case.fixture_source_revision,
                "--capture-dir",
                os.fspath(case.root / f"capture-{profile}"),
                "--output",
                os.fspath(output),
                "--affinity-cpus",
                case.affinity,
            ]
        )
    else:
        common.extend(["--supplement", os.fspath(output)])
    return common


def run(argv: list[str], success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, text=True, capture_output=True, cwd=REPO, check=False)
    if success and result.returncode != 0:
        raise AssertionError(f"command failed:\n{result.stdout}\n{result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError("command unexpectedly succeeded")
    return result


def main() -> None:
    runner = os.environ.get("WRIC_PHASE78_TEST_PROCESS_METRICS")
    if not runner:
        raise RuntimeError("WRIC_PHASE78_TEST_PROCESS_METRICS is required")
    os.environ["WRIC_PHASE9_TEST_PROCESS_METRICS"] = runner
    for profile, captures, rows in (("phase7", 16, 24), ("phase8", 4, 4)):
        payload = json.loads(
            run(
                [sys.executable, os.fspath(HELPER), "plan", "--profile", profile]
            ).stdout
        )
        assert payload["capture_count"] == captures
        assert payload["manifest_row_count"] == rows

    with tempfile.TemporaryDirectory(
        prefix="wric-phase78-manifest-bootstrap-", dir=REPO / "build"
    ) as temporary:
        cleanup_root = Path(temporary) / "capture-cleanup-symlink"
        external = Path(temporary) / "external-read-only.txt"
        external.write_text("must survive cleanup\n")
        external.chmod(0o400)
        cleanup_root.mkdir()
        (cleanup_root / "foreign-link").symlink_to(external)
        bootstrap.remove_staging(cleanup_root)
        assert not os.path.lexists(cleanup_root)
        assert external.read_text() == "must survive cleanup\n"
        assert stat.S_IMODE(external.stat().st_mode) == 0o400
        external.chmod(0o600)

        hardlink_root = Path(temporary) / "capture-cleanup-hardlink"
        hardlink_root.mkdir()
        hardlink_target = Path(temporary) / "external-hardlink.txt"
        hardlink_target.write_text("must remain multiply linked\n")
        os.link(hardlink_target, hardlink_root / "foreign-hardlink")
        try:
            bootstrap.remove_staging(hardlink_root)
        except bootstrap.Phase78Error as error:
            assert "externally hard-linked" in str(error)
        else:
            raise AssertionError("capture cleanup removed a hard-linked staging file")
        assert hardlink_root.is_dir()
        assert hardlink_target.stat().st_nlink == 2
        (hardlink_root / "foreign-hardlink").unlink()
        bootstrap.remove_staging(hardlink_root)

        special_root = Path(temporary) / "capture-cleanup-special"
        special_root.mkdir()
        fifo = special_root / "foreign-fifo"
        os.mkfifo(fifo)
        try:
            bootstrap.remove_staging(special_root)
        except bootstrap.Phase78Error as error:
            assert "special file" in str(error)
        else:
            raise AssertionError("capture cleanup removed a special staging node")
        assert stat.S_ISFIFO(fifo.lstat().st_mode)
        fifo.unlink()
        bootstrap.remove_staging(special_root)

        race_root = Path(temporary) / "capture-publish-race"
        staging = race_root / ".row.staging"
        destination = race_root / "row"
        staging.mkdir(parents=True)
        marker = staging / "owned.txt"
        marker.write_text("owned staged evidence\n")

        def inject_foreign_destination() -> None:
            destination.mkdir()

        try:
            bootstrap.seal_capture_directory(
                staging,
                destination,
                before_publish=inject_foreign_destination,
            )
        except bootstrap.core.BootstrapError as error:
            assert "already exists" in str(error)
        else:
            raise AssertionError("capture publication replaced a foreign destination")
        assert destination.is_dir() and not list(destination.iterdir())
        assert staging.is_dir() and marker.read_text() == "owned staged evidence\n"
        staging.chmod(0o755)
        marker.chmod(0o644)

        contract_root = Path(temporary) / "capture-contract-race"
        contract_root.mkdir()
        contract = contract_root / "capture-contract.json"
        required_contract = b"owned deterministic contract\n"
        foreign_contract = b"foreign contract must survive\n"

        def inject_foreign_contract() -> None:
            bootstrap.core.copy_bytes(contract, foreign_contract, 0o444)

        try:
            bootstrap.ensure_exact_capture_file(
                contract,
                required_contract,
                "focused capture contract",
                before_publish=inject_foreign_contract,
            )
        except bootstrap.core.BootstrapError as error:
            assert "already exists" in str(error)
        else:
            raise AssertionError("capture contract publication replaced a foreign file")
        assert contract.read_bytes() == foreign_contract
        contract_staging = contract_root / ".capture-contract.json.staging"
        assert contract_staging.read_bytes() == required_contract
        contract.unlink()
        bootstrap.ensure_exact_capture_file(
            contract, required_contract, "focused capture contract"
        )
        assert contract.read_bytes() == required_contract
        assert not os.path.lexists(contract_staging)

        sentinel_error = (
            "error: manifest group has no rows: "
            + bootstrap.HARNESS_SENTINEL_GROUP
            + "\n"
        )
        for result in (
            subprocess.CompletedProcess(
                args=["synthetic-harness"],
                returncode=-signal.SIGSEGV,
                stdout="",
                stderr=sentinel_error,
            ),
            subprocess.CompletedProcess(
                args=["synthetic-harness"],
                returncode=1,
                stdout="",
                stderr=sentinel_error + "unexpected later failure\n",
            ),
        ):
            try:
                bootstrap.validate_harness_sentinel_result(result)
            except bootstrap.Phase78Error as error:
                assert "exact post-validation" in str(error)
            else:
                raise AssertionError("non-exact harness sentinel result was accepted")

        case = phase9_test.Integration(Path(temporary))
        replace_oracle(case)
        runner_log = wrap_process_metrics(case)
        fixture_source, fixture_revision = create_fixture_source(case.root)
        case.fixture_source_root = fixture_source
        case.fixture_source_revision = fixture_revision

        phase8_profile = bootstrap.PROFILES["phase8"]
        same_root = bootstrap.fixture_source_for_build(
            argparse.Namespace(
                fixture_source_root=None,
                expected_fixture_source_revision=None,
            ),
            phase8_profile,
            fixture_source,
            argparse.Namespace(
                preamble={"repo_revision": fixture_revision}
            ),
        )
        assert same_root.root == fixture_source
        unrelated = fixture_source / "unrelated-untracked.txt"
        unrelated.write_text("irrelevant dirt is outside the trusted path set\n")
        bootstrap.validate_fixture_source_tree(
            phase8_profile, fixture_source, fixture_revision
        )

        def source_rejection(
            label: str,
            mutate,  # type: ignore[no-untyped-def]
            expected: str,
        ) -> None:
            candidate = command(
                case,
                "phase8",
                "build",
                case.root / f"source-negative-{label}/phase8-generation.tsv",
            )
            capture_index = candidate.index("--capture-dir") + 1
            candidate[capture_index] = os.fspath(
                case.root / f"source-negative-{label}.capture"
            )
            mutate(candidate)
            rejected = run(candidate, success=False)
            assert expected in rejected.stderr, rejected.stderr

        source_rejection(
            "wrong-revision",
            lambda argv: argv.__setitem__(
                argv.index("--expected-fixture-source-revision") + 1,
                "0" * 40,
            ),
            "fixture source provenance is invalid",
        )

        def remove_expected_revision(argv: list[str]) -> None:
            index = argv.index("--expected-fixture-source-revision")
            del argv[index : index + 2]

        source_rejection(
            "partial-provenance",
            remove_expected_revision,
            "must be supplied together",
        )
        source_alias = case.root / "tracked-fixture-source-alias"
        source_alias.symlink_to(fixture_source, target_is_directory=True)
        source_rejection(
            "symlink-root",
            lambda argv: argv.__setitem__(
                argv.index("--fixture-source-root") + 1,
                os.fspath(source_alias),
            ),
            "fixture source provenance is invalid",
        )
        source_rejection(
            "subdirectory-root",
            lambda argv: argv.__setitem__(
                argv.index("--fixture-source-root") + 1,
                os.fspath(fixture_source / "test"),
            ),
            "fixture source provenance is invalid",
        )
        source_rejection(
            "swapped-base-root",
            lambda argv: argv.__setitem__(
                argv.index("--fixture-source-root") + 1,
                os.fspath(case.base_repo_root),
            ),
            "fixture source provenance is invalid",
        )
        phase8_fixture = phase8_profile.fixtures[0]
        tracked_fixture = fixture_source / phase8_fixture.relative_path
        tracked_fixture.chmod(0o755)
        source_rejection(
            "dirty-relevant",
            lambda argv: None,
            "dirty relevant files",
        )
        tracked_fixture.chmod(0o644)
        assert subprocess.run(
            [
                "git",
                "-C",
                os.fspath(fixture_source),
                "status",
                "--porcelain=v1",
                "--",
                phase8_fixture.relative_path,
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout == ""
        external_hardlink = case.root / "external-fixture-hardlink.pb.gz"
        shutil.copyfile(tracked_fixture, external_hardlink)
        tracked_fixture.unlink()
        os.link(external_hardlink, tracked_fixture)
        try:
            bootstrap.fixture_path(fixture_source, phase8_fixture)
        except bootstrap.Phase78Error as error:
            assert "externally hard-linked" in str(error)
        else:
            raise AssertionError("hard-linked fixture source was accepted")
        tracked_fixture.unlink()
        shutil.copyfile(REPO / phase8_fixture.relative_path, tracked_fixture)
        tracked_fixture.chmod(0o644)
        external_hardlink.unlink()
        tracked_fixture.unlink()
        tracked_fixture.symlink_to(REPO / phase8_fixture.relative_path)
        try:
            bootstrap.fixture_path(fixture_source, phase8_fixture)
        except bootstrap.core.BootstrapError as error:
            assert "symlink" in str(error)
        else:
            raise AssertionError("symlinked fixture source was accepted")
        tracked_fixture.unlink()
        os.mkfifo(tracked_fixture)
        try:
            bootstrap.fixture_path(fixture_source, phase8_fixture)
        except bootstrap.core.BootstrapError as error:
            assert "regular file" in str(error)
        else:
            raise AssertionError("special fixture source was accepted")
        tracked_fixture.unlink()
        shutil.copyfile(REPO / phase8_fixture.relative_path, tracked_fixture)
        tracked_fixture.chmod(0o644)
        escaped_fixture = dataclasses.replace(
            phase8_fixture, relative_path="../escaped.pb.gz"
        )
        escaped_profile = dataclasses.replace(
            phase8_profile, fixtures=(escaped_fixture,)
        )
        try:
            bootstrap.fixture_relative_paths(escaped_profile)
        except bootstrap.Phase78Error as error:
            assert "not normalized and confined" in str(error)
        else:
            raise AssertionError("escaping fixture source path was accepted")

        noninvoking_marker = case.root / "noninvoking-runner-was-called"
        noninvoking_runner = case.root / "wrong-process-metrics"
        phase9_test.write_bytes(
            noninvoking_runner,
            (
                "#!/usr/bin/python3\n"
                "from pathlib import Path\n"
                f"Path({os.fspath(noninvoking_marker)!r}).write_text('invoked\\n')\n"
                "raise SystemExit(99)\n"
            ).encode(),
            0o555,
        )
        wrong_runner_command = command(
            case,
            "phase8",
            "build",
            case.root / "out-wrong-runner/phase8-generation.tsv",
        )
        wrong_runner_command[
            wrong_runner_command.index("--process-metrics") + 1
        ] = os.fspath(noninvoking_runner)
        rejected_runner = run(wrong_runner_command, success=False)
        assert "--expected-process-metrics-sha256" in rejected_runner.stderr
        assert not noninvoking_marker.exists()

        for profile in ("phase8", "phase7"):
            name = bootstrap.PROFILES[profile].output_name
            output = case.root / f"out-{profile}" / name
            run(command(case, profile, "build", output))
            run(command(case, profile, "audit", output))
            result = run(command(case, profile, "build", output), success=False)
            assert "already exists" in result.stderr
            manifest = bootstrap.core.read_manifest(
                output, case.base_repo_root, expected_kind="supplement"
            )
            assert len(manifest.rows) == (4 if profile == "phase8" else 24)
            profile_assets = output.with_name(output.stem + ".assets")
            expected_capture_identity = {
                "capture_contract_sha256": bootstrap.core.sha256_file(
                    profile_assets / "provenance/capture-contract.json"
                ),
                "base_manifest_sha256": manifest.preamble["parent_sha256"],
                "frozen_oracle_sha256": manifest.preamble[
                    "frozen_oracle_dagutil_sha256"
                ],
                "process_metrics_sha256": phase9_test.file_digest(case.runner),
            }
            profile_statuses = sorted(
                (profile_assets / "provenance").rglob("status.json")
            )
            assert profile_statuses
            for profile_status in profile_statuses:
                status_value = json.loads(profile_status.read_text())
                assert status_value["schema_version"] == 2
                assert status_value["capture_identity"] == expected_capture_identity
            if profile == "phase8":
                assets = output.with_name("phase8-generation.assets")
                source_row = (
                    assets
                    / "provenance/rows/phase8-generation-tree0-off-w1"
                )
                canonical_mutations = Path(temporary) / "canonical-mutations"
                canonical_mutations.mkdir()
                original_sidecar = (source_row / "canonical.ndjson").read_bytes()
                original_compact = json.loads(
                    (source_row / "canonical.json").read_text()
                )

                def reject_canonical_mutation(wanted: str) -> None:
                    try:
                        bootstrap.validate_canonical_companion(
                            canonical_mutations, "focused canonical mutation"
                        )
                    except bootstrap.Phase78Error as error:
                        assert wanted in str(error), str(error)
                    else:
                        raise AssertionError(
                            f"canonical mutation was accepted; wanted {wanted!r}"
                        )

                (canonical_mutations / "canonical.ndjson").write_bytes(
                    original_sidecar
                )
                bad_count = dict(original_compact)
                bad_count["record_count"] += 1
                (canonical_mutations / "canonical.json").write_bytes(
                    bootstrap.json_bytes(bad_count)
                )
                reject_canonical_mutation("record count disagrees")

                bad_component = dict(original_compact)
                bad_component["candidates_sha256"] = "0" * 64
                (canonical_mutations / "canonical.json").write_bytes(
                    bootstrap.json_bytes(bad_component)
                )
                reject_canonical_mutation("candidates component digest disagrees")

                mutated_lines: list[bytes] = []
                changed_kind = False
                for raw_line in original_sidecar.splitlines(keepends=True):
                    record = json.loads(raw_line)
                    if not changed_kind and record.get("record") == "candidate":
                        record["record"] = "unknown_record_kind"
                        raw_line = (
                            json.dumps(
                                record, separators=(",", ":"), sort_keys=True
                            )
                            + "\n"
                        ).encode()
                        changed_kind = True
                    mutated_lines.append(raw_line)
                assert changed_kind
                unknown_sidecar = b"".join(mutated_lines)
                unknown_compact = dict(original_compact)
                unknown_compact["semantic_sha256"] = sha256(unknown_sidecar)
                (canonical_mutations / "canonical.ndjson").write_bytes(
                    unknown_sidecar
                )
                (canonical_mutations / "canonical.json").write_bytes(
                    bootstrap.json_bytes(unknown_compact)
                )
                reject_canonical_mutation("unknown kind")

                ordered_records = bootstrap.sidecar_records(
                    source_row / "canonical.ndjson"
                )
                candidate_positions = [
                    index
                    for index, record in enumerate(ordered_records)
                    if record.get("record") == "candidate"
                ]
                assert len(candidate_positions) >= 2
                first, second = candidate_positions[:2]
                ordered_records[first], ordered_records[second] = (
                    ordered_records[second],
                    ordered_records[first],
                )
                final_state = next(
                    record
                    for record in ordered_records
                    if record.get("record") == "final_state"
                )
                stream_expected = {
                    "expected_candidates_generated": "256",
                    "expected_candidates_scored": "256",
                    "expected_exact_verifications": "1",
                    "expected_stop_reason": "candidate_cap",
                    "expected_final_score": str(final_state["final_score"]),
                    "expected_accepted_moves": "0",
                }
                try:
                    bootstrap.validate_profile_canonical_stream(
                        bootstrap.PROFILES["phase8"],
                        ordered_records,
                        stream_expected,
                        "focused canonical ordering mutation",
                    )
                except bootstrap.Phase78Error as error:
                    assert "candidate stream indexes" in str(error)
                else:
                    raise AssertionError("canonical candidate reordering was accepted")

                commands = assets / "commands.sh"
                assert "\n+  " not in commands.read_text()
                reproduction = case.root / "phase8-command-reproduction"
                environment = dict(os.environ)
                environment["WRIC_PHASE78_FROZEN_ORACLE"] = os.fspath(case.oracle)
                replay = subprocess.run(
                    [os.fspath(commands), os.fspath(reproduction)],
                    check=False,
                    text=True,
                    capture_output=True,
                    env=environment,
                    cwd=REPO,
                )
                if replay.returncode != 0:
                    raise AssertionError(
                        f"commands.sh replay failed:\n{replay.stdout}\n{replay.stderr}"
                    )
                for row in manifest.rows:
                    stem = row["row_id"]
                    assert (reproduction / f"{stem}.pb.gz").is_file()
                    assert (reproduction / f"{stem}.canonical.ndjson").is_file()
                    assert (reproduction / f"{stem}.canonical.json").is_file()
                    assert (reproduction / f"{stem}.output-canonical.json").is_file()
                    assert bootstrap.core.sha256_file(
                        reproduction / f"{stem}.canonical.ndjson"
                    ) == row["oracle_search_semantic_sha256"]
                hidden = case.root / "capture-phase8/inputs/.foreign-remnant"
                hidden.mkdir()
                hidden_output = (
                    case.root / "out-phase8-hidden" / "phase8-generation.tsv"
                )
                rejected = run(
                    command(case, "phase8", "build", hidden_output), success=False
                )
                assert "unknown dot-prefixed remnant" in rejected.stderr
                assert hidden.is_dir() and not list(hidden.iterdir())
                hidden.rmdir()
                wrong_expected = command(case, "phase8", "audit", output)
                wrong_expected[
                    wrong_expected.index("--expected-process-metrics-sha256") + 1
                ] = "0" * 64
                rejected_expected = run(wrong_expected, success=False)
                assert "archived capture process-metrics hash" in rejected_expected.stderr

                receipt = (
                    assets
                    / "provenance/rows/phase8-generation-tree0-off-w1/process-metrics.txt"
                )
                too_slow_receipt = case.root / "too-slow-process-metrics.txt"
                receipt_lines = receipt.read_text().splitlines()
                receipt_lines = [
                    "wall_seconds=600.001"
                    if line.startswith("wall_seconds=")
                    else line
                    for line in receipt_lines
                ]
                too_slow_receipt.write_text("\n".join(receipt_lines) + "\n")
                try:
                    bootstrap.validate_capture_receipt(
                        too_slow_receipt, "synthetic over-limit receipt"
                    )
                except bootstrap.Phase78Error as error:
                    assert "exceeds the 600-second workload contract" in str(error)
                else:
                    raise AssertionError("over-limit capture receipt was accepted")
            else:
                auto = [row for row in manifest.rows if row["lazy_policy"] == "auto"]
                assert len(auto) == 8
                assert {row["expected_cache_strategy"] for row in auto} == {
                    "all_active_patterns",
                    "lazy_multisite_chart",
                }
                assets = output.with_name("phase7-lazy.assets")
                reference = assets / "fixtures/high-compression.ref"
                reference.chmod(0o644)
                reference.write_bytes(reference.read_bytes() + b"X")
                rejected_reference = run(
                    command(case, "phase7", "audit", output), success=False
                )
                assert "asset ledger member changed" in rejected_reference.stderr

        invocations = [json.loads(line) for line in runner_log.read_text().splitlines()]
        assert invocations
        for invocation in invocations:
            timeout_index = invocation.index("--timeout-seconds")
            assert invocation[timeout_index + 1] == "600"

        phase8_output = case.root / "out-phase8" / "phase8-generation.tsv"
        shutil.rmtree(fixture_source)
        run(command(case, "phase8", "audit", phase8_output))
        extra = phase8_output.with_name("phase8-generation.assets") / "unledgered.txt"
        extra.write_text("not sealed\n")
        result = run(command(case, "phase8", "audit", phase8_output), success=False)
        assert "exact sealed closure" in result.stderr
        extra.unlink()

        status = (
            phase8_output.with_name("phase8-generation.assets")
            / "provenance/rows/phase8-generation-tree0-off-w1/status.json"
        )
        status.chmod(0o644)
        status.write_bytes(status.read_bytes() + b" ")
        result = run(command(case, "phase8", "audit", phase8_output), success=False)
        assert "hash mismatch" in result.stderr or "changed" in result.stderr

    print("wric_phase78_manifest_bootstrap_test: PASS")


if __name__ == "__main__":
    main()
