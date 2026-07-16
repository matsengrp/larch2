#!/usr/bin/env python3
"""Strict synthetic integration for the Phase-7 completion supplement."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

REPO = Path(__file__).resolve().parents[1]
TOOLS = REPO / "tools"
TESTS = REPO / "test"
sys.path[:0] = [os.fspath(TOOLS), os.fspath(TESTS)]

import wric_phase7_completion_manifest_bootstrap as bootstrap  # noqa: E402
import wric_phase78_manifest_bootstrap_test as phase78_test  # noqa: E402
import wric_phase9_manifest_bootstrap_test as phase9_test  # noqa: E402


HELPER = TOOLS / "wric_phase7_completion_manifest_bootstrap.py"
HARNESS = TOOLS / "wric_spr_search_benchmark.sh"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(argv: list[str], *, success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, cwd=REPO, text=True, capture_output=True, check=False)
    if success and result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}):\n{result.stdout}\n{result.stderr}"
        )
    if not success and result.returncode == 0:
        raise AssertionError("command unexpectedly succeeded")
    return result


def patch_oracle(case: phase9_test.Integration) -> Path:
    """Install the Phase78 fake plus tree-input and auto-policy reporting."""

    phase78_test.replace_oracle(case)
    text = case.oracle.read_text()
    old_input = 'input_path = pathlib.Path(value("--dag-pb"))'
    new_input = (
        'input_path = pathlib.Path(value("--dag-pb") if "--dag-pb" in args '
        'else value("--tree-pb"))'
    )
    if text.count(old_input) != 1:
        raise AssertionError("fake oracle input hook changed")
    text = text.replace(old_input, new_input)
    marker = 'print("chart_spr_search:")'
    pilot_hash = bootstrap.lazy_policy_index_hash(113, 32)
    auto = f'''if policy == "auto":
    top["lazy_policy_requested"] = "auto"
    top["lazy_policy_resolved"] = "off"
    top["lazy_policy_version"] = "1"
    top["lazy_policy_reason"] = "structural_and_strong_row_ratios_exceeded"
    top["lazy_policy_frozen"] = "true"
    top["lazy_policy_measurements_available"] = "true"
    top["lazy_policy_active_patterns"] = "113"
    top["lazy_policy_pilot_patterns"] = "32"
    top["lazy_policy_pilot_pattern_index_hash"] = "{pilot_hash}"
    top["lazy_policy_pilot_inside_chart_builds"] = "1"
    top["lazy_policy_pilot_outside_chart_builds"] = "0"
    top["lazy_policy_pilot_exact_builds"] = "0"
    top["lazy_policy_pilot_scheduler_submissions"] = "0"
    top["lazy_policy_pilot_internal_structural_classes_max"] = "16"
    top["lazy_policy_pilot_structural_ratio_numerator"] = "16"
    top["lazy_policy_pilot_structural_ratio_denominator"] = "32"
    top["lazy_policy_pilot_inside_rows"] = "1000"
    top["lazy_policy_pilot_dense_rows"] = "4448"
    top["lazy_policy_pilot_row_ratio_numerator"] = "1000"
    top["lazy_policy_pilot_row_ratio_denominator"] = "4448"
    top["lazy_policy_pilot_estimated_allocation_bytes"] = "1000000"
    top["lazy_policy_estimated_lazy_cache_bytes"] = "2000000"
    top["lazy_policy_estimated_dense_cache_bytes"] = "3000000"
    top["lazy_policy_pilot_key_words"] = "1000"
    top["lazy_policy_pilot_dense_row_work"] = "4448"
    top["lazy_policy_pilot_runs"] = "1"
    top["lazy_policy_frozen_reuses"] = "0"
'''
    if text.count(marker) != 1:
        raise AssertionError("fake oracle report hook changed")
    text = text.replace(marker, auto + marker)
    case.oracle.chmod(0o755)
    phase9_test.write_bytes(case.oracle, text.encode(), 0o555)
    working = case.root / "working-chart"
    phase9_test.write_bytes(working, case.oracle.read_bytes(), 0o555)
    return working


def expected_values(policy: str) -> dict[str, str]:
    return {
        "expected_refinement_exactness": "BOUNDED_REFINED_GRAMMAR",
        "expected_cache_strategy": (
            "lazy_multisite_chart" if policy == "on" else "all_active_patterns"
        ),
        "expected_effective_pattern_batch_size": "113",
        "expected_keep_mask_kind": "-",
        "expected_final_compaction_exactness": "none",
        "expected_chain_exactness": "none_conservative_materialize_rebuild",
        "expected_active_patterns": "113",
        "expected_initial_clades": "139",
        "expected_initial_productions": "69",
        "expected_candidates_generated": "64",
        "expected_candidates_scored": "64",
        "expected_exact_verifications": "0",
        "expected_stop_reason": "candidate_cap",
        "expected_iterations": "1",
        "expected_accepted_moves": "0",
        "expected_initial_score": "174",
        "expected_final_score": "174",
        "expected_validated_parsimony": "174",
    }


def source_row(
    case: phase9_test.Integration,
    fixture: str,
    policy: str,
    workers: int,
) -> dict[str, str]:
    row = bootstrap.expected_source_contract(fixture, policy, workers, case.affinity)
    row_id, run_group = bootstrap.SOURCE_ROW_IDS[(fixture, policy, workers)]
    row.update(
        {
            "row_id": row_id,
            "run_group": run_group,
            "workload_name": (
                "dense-local-small-64"
                if fixture == "tree0"
                else (
                    "dense-local-medium-64"
                    if policy == "off"
                    else "lazy-compression-medium"
                )
            ),
            "fixture_id": (
                "small-test-5-tree0" if fixture == "tree0" else "medium-seedtree"
            ),
        }
    )
    primary = REPO / (
        bootstrap.TREE_FIXTURE.relative_path
        if fixture == "tree0"
        else bootstrap.MEDIUM_PRIMARY_RELATIVE
    )
    refseq = None if fixture == "tree0" else REPO / bootstrap.MEDIUM_REFSEQ_RELATIVE
    directory = case.base_dir / "source" / f"{fixture}-{policy}-w{workers}"
    directory.mkdir(parents=True)
    chart = bootstrap.actual_chart_command(
        case.oracle, primary, refseq, row, directory
    )
    with (directory / "report.txt").open("wb") as stdout, (
        directory / "stderr.txt"
    ).open("wb") as stderr:
        result = subprocess.run(chart, cwd=REPO, stdout=stdout, stderr=stderr, check=False)
    if result.returncode != 0:
        raise AssertionError(f"synthetic source chart failed: {row_id}")
    with (directory / "output-report.txt").open("wb") as stdout, (
        directory / "output-stderr.txt"
    ).open("wb") as stderr:
        result = subprocess.run(
            bootstrap.output_command(
                case.oracle,
                directory / "output.pb.gz",
                directory / "output-canonical.json",
                case.affinity,
            ),
            cwd=REPO,
            stdout=stdout,
            stderr=stderr,
            check=False,
        )
    if result.returncode != 0:
        raise AssertionError(f"synthetic source output scoring failed: {row_id}")
    compact = json.loads((directory / "canonical.json").read_text())
    output = json.loads((directory / "output-canonical.json").read_text())
    row.update(expected_values(policy))
    relative = directory.relative_to(case.base_dir).as_posix()
    row.update(
        {
            "canonical_sidecar_uri": f"manifest://{relative}/canonical.ndjson",
            "canonical_sidecar_sha256": phase9_test.file_digest(
                directory / "canonical.ndjson"
            ),
            "oracle_report_uri": f"manifest://{relative}/canonical.json",
            "oracle_report_sha256": phase9_test.file_digest(directory / "canonical.json"),
            "oracle_search_semantic_sha256": compact["semantic_sha256"],
            "oracle_output_semantic_sha256": output["semantic_sha256"],
        }
    )
    argv_sha = bootstrap.core.canonical_argv_digest(bootstrap.canonical_argv(row))
    row["canonical_argv_sha256"] = argv_sha
    row["oracle_trial_semantic_sha256"] = bootstrap.core.trial_digest(
        row["method"],
        row["oracle_search_semantic_sha256"],
        row["oracle_output_semantic_sha256"],
        argv_sha,
    )
    return row


def install_source_matrix(case: phase9_test.Integration) -> None:
    preamble, original_rows = bootstrap.core.parse_preamble_and_rows(
        case.base, bootstrap.core.PREAMBLE_KEYS, bootstrap.core.MANIFEST_HEADER
    )
    rows = [
        source_row(case, fixture, policy, workers)
        for fixture, policy in (("medium", "off"), ("medium", "on"), ("tree0", "off"))
        for workers in bootstrap.WORKERS
    ]
    preamble = dict(preamble)
    preamble["frozen_oracle_dagutil_sha256"] = phase9_test.file_digest(case.oracle)
    phase9_test.write_bytes(
        case.base,
        bootstrap.core.tsv_bytes(preamble, [*original_rows, *rows]),
    )
    phase9_test.seal(case.base)
    case.parent_sha = phase9_test.file_digest(case.base)


def command(
    case: phase9_test.Integration,
    working: Path,
    action: str,
    output: Path,
) -> list[str]:
    result = [
        sys.executable,
        os.fspath(HELPER),
        action,
        "--repo-root",
        os.fspath(REPO),
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
        "--working-chart",
        os.fspath(working),
        "--expected-working-chart-sha256",
        phase9_test.file_digest(working),
    ]
    if action == "build":
        result.extend(
            [
                "--capture-dir",
                os.fspath(case.root / "completion-capture"),
                "--output",
                os.fspath(output),
                "--affinity-cpus",
                case.affinity,
            ]
        )
    else:
        result.extend(["--supplement", os.fspath(output)])
    return result


def reledger_and_reseal(output: Path) -> None:
    """Seal coherent synthetic mutations so inner validators must catch them."""

    assets = output.with_name("phase7-lazy-completion.assets")
    members = bootstrap.phase78.parse_asset_ledger(assets / "assets.sha256")
    ledger = b"".join(
        f"{phase9_test.file_digest(assets / name)}  {name}\n".encode()
        for name in sorted(members)
    )
    (assets / "assets.sha256").chmod(0o644)
    (assets / "assets.sha256").write_bytes(ledger)
    (assets / "assets.sha256").chmod(0o444)


def replace_readonly(path: Path, data: bytes, mode: int = 0o444) -> None:
    path.chmod(0o644)
    path.write_bytes(data)
    path.chmod(mode)


def replace_sealed_json(path: Path, value: dict[str, object]) -> None:
    payload = bootstrap.json_bytes(value)
    replace_readonly(path, payload)
    replace_readonly(
        path.with_name(path.name + ".sha256"),
        bootstrap.core.seal_bytes(path.name, payload),
    )


def replace_manifest(
    output: Path,
    preamble: dict[str, str],
    rows: list[dict[str, str]],
) -> None:
    payload = bootstrap.core.tsv_bytes(preamble, rows)
    replace_readonly(output, payload)
    replace_readonly(
        output.with_name(output.name + ".sha256"),
        bootstrap.core.seal_bytes(output.name, payload),
    )


def main() -> None:
    runner = os.environ.get("WRIC_PHASE7_COMPLETION_TEST_PROCESS_METRICS")
    if not runner:
        raise RuntimeError("WRIC_PHASE7_COMPLETION_TEST_PROCESS_METRICS is required")
    os.environ["WRIC_PHASE9_TEST_PROCESS_METRICS"] = runner
    plan = json.loads(run([sys.executable, os.fspath(HELPER), "plan"]).stdout)
    assert plan["row_count"] == 8
    assert plan["new_frozen_oracle_chart_capture_count"] == 4
    assert plan["current_auto_qualification_count"] == 4

    with tempfile.TemporaryDirectory(
        prefix="wric-phase7-completion-", dir=REPO / "build"
    ) as temporary:
        case = phase9_test.Integration(Path(temporary))
        working = patch_oracle(case)
        install_source_matrix(case)
        invocation_log = phase78_test.wrap_process_metrics(case)
        output = case.root / "published" / bootstrap.OUTPUT_NAME

        wrong_runner_hash = command(case, working, "build", output)
        wrong_runner_hash[
            wrong_runner_hash.index("--expected-process-metrics-sha256") + 1
        ] = "0" * 64
        rejected = run(wrong_runner_hash, success=False)
        assert "process-metrics runner differs" in rejected.stderr
        assert not invocation_log.exists()

        wrong_hash = command(case, working, "build", output)
        wrong_hash[wrong_hash.index("--expected-working-chart-sha256") + 1] = "0" * 64
        rejected = run(wrong_hash, success=False)
        assert "working chart differs" in rejected.stderr
        assert not invocation_log.exists()

        # The completion caller must inherit the shared deterministic-file
        # installer's refusal to resume an externally hard-linked contract.
        base_for_contract = bootstrap.core.read_manifest(
            case.base, REPO, expected_kind="base"
        )
        source_for_contract = bootstrap.validate_source_matrix(
            base_for_contract, REPO, case.affinity
        )
        hardlink_capture = case.root / "hardlink-capture"
        hardlink_capture.mkdir()
        external_contract = case.root / "external-capture-contract.json"
        contract_data = bootstrap.capture_contract_bytes(
            base_for_contract,
            source_for_contract,
            case.affinity,
            phase9_test.file_digest(case.runner),
            phase9_test.file_digest(working),
        )
        phase9_test.write_bytes(external_contract, contract_data, 0o444)
        os.link(external_contract, hardlink_capture / "capture-contract.json")
        try:
            bootstrap.initialize_capture(
                hardlink_capture,
                base_for_contract,
                source_for_contract,
                case.affinity,
                phase9_test.file_digest(case.runner),
                phase9_test.file_digest(working),
            )
        except bootstrap.phase78.Phase78Error as error:
            assert "externally hard-linked" in str(error)
        else:
            raise AssertionError("hard-linked completion contract was accepted")
        assert external_contract.read_bytes() == contract_data
        assert external_contract.stat().st_nlink == 2

        run(command(case, working, "build", output))
        run(command(case, working, "audit", output))
        existing = run(command(case, working, "build", output), success=False)
        assert "already exists" in existing.stderr

        manifest = bootstrap.core.read_manifest(output, REPO, expected_kind="supplement")
        assert len(manifest.rows) == 8
        assert [row["row_id"] for row in manifest.rows] == [
            *(bootstrap.medium_row_id(worker) for worker in bootstrap.WORKERS),
            *(bootstrap.tree_row_id(worker) for worker in bootstrap.WORKERS),
        ]
        assert {row["run_group"] for row in manifest.rows[:4]} == {
            bootstrap.MEDIUM_GROUP
        }
        assert {row["run_group"] for row in manifest.rows[4:]} == {
            bootstrap.SMALL_GROUP
        }
        assert {row["expected_cache_strategy"] for row in manifest.rows[:4]} == {
            "all_active_patterns"
        }
        assert {row["expected_cache_strategy"] for row in manifest.rows[4:]} == {
            "lazy_multisite_chart"
        }
        assert all(row["lazy_policy"] == "auto" for row in manifest.rows[:4])
        assert all(row["lazy_policy"] == "on" for row in manifest.rows[4:])
        assets = output.with_name("phase7-lazy-completion.assets")
        run([os.fspath(assets / "commands.sh"), "--verify-only"])

        # Exercise the exact completion payload through an interrupted
        # assets->manifest->seal publication and roll it forward on restart.
        recovery_output = case.root / "recovery" / bootstrap.OUTPUT_NAME
        recovery_output.parent.mkdir()
        recovery_files = {
            path.relative_to(assets).as_posix(): path.read_bytes()
            for path in assets.rglob("*")
            if path.is_file()
        }

        class SyntheticCrash(RuntimeError):
            pass

        with bootstrap.core.exclusive_output_lock(recovery_output) as publication:
            try:
                bootstrap.core.publish_immutable_supplement(
                    publication,
                    recovery_files,
                    output.read_bytes(),
                    output.with_name(output.name + ".sha256").read_bytes(),
                    crash_hook=lambda point: (
                        (_ for _ in ()).throw(SyntheticCrash(point))
                        if point == "manifest_published"
                        else None
                    ),
                )
            except SyntheticCrash as error:
                assert str(error) == "manifest_published"
            else:
                raise AssertionError("synthetic publication did not crash")
        assert recovery_output.exists()
        assert recovery_output.with_name("phase7-lazy-completion.assets").is_dir()
        assert not recovery_output.with_name(recovery_output.name + ".sha256").exists()

        def validate_recovery(staged: Path) -> None:
            bootstrap.audit_supplement(
                case.base,
                case.parent_sha,
                staged,
                REPO,
                phase9_test.file_digest(case.runner),
                phase9_test.file_digest(working),
                case.runner,
            )

        with bootstrap.core.exclusive_output_lock(recovery_output) as publication:
            assert bootstrap.core.recover_interrupted_publication(
                publication, prepublish_validator=validate_recovery
            )
            validate_recovery(publication.output)
            bootstrap.core.finish_publication(publication)
        assert recovery_output.with_name(recovery_output.name + ".sha256").is_file()

        base_manifest = bootstrap.core.read_manifest(
            case.base, REPO, expected_kind="base"
        )
        runner_sha = phase9_test.file_digest(case.runner)
        working_sha = phase9_test.file_digest(working)
        source_matrix = bootstrap.validate_source_matrix(
            base_manifest, REPO, case.affinity
        )
        capture_identity = bootstrap.completion_capture_identity(
            base_manifest,
            source_matrix,
            case.affinity,
            runner_sha,
            working_sha,
        )
        expected_identity = {
            "capture_contract_sha256": capture_identity.capture_contract_sha256,
            "base_manifest_sha256": base_manifest.sha256,
            "frozen_oracle_sha256": base_manifest.preamble[
                "frozen_oracle_dagutil_sha256"
            ],
            "process_metrics_sha256": runner_sha,
        }
        status_paths = [
            assets / "provenance/inputs/tree0/status.json",
            *(
                assets / f"provenance/rows/{bootstrap.tree_row_id(worker)}/status.json"
                for worker in bootstrap.WORKERS
            ),
            *(
                assets
                / f"provenance/qualifications/{bootstrap.medium_row_id(worker)}/status.json"
                for worker in bootstrap.WORKERS
            ),
        ]
        for status_path in status_paths:
            status_value = json.loads(status_path.read_text())
            assert status_value["schema_version"] == 2
            assert status_value["capture_identity"] == expected_identity
        collision = dict(manifest.rows[0])
        collision["row_id"] = "foreign-colliding-completion-row"
        try:
            bootstrap.assert_no_resolution_collisions(
                base_manifest, [manifest.rows[0], collision]
            )
        except bootstrap.CompletionError as error:
            assert "collides" in str(error)
        else:
            raise AssertionError("resolver-equivalent completion row was accepted")

        invocations = [json.loads(line) for line in invocation_log.read_text().splitlines()]
        # Capture uses 17 bounded commands.  Each private/final/build/audit or
        # recovery audit then replays one input plus eight output protobufs.
        assert len(invocations) == 17 + 5 * 9
        for invocation in invocations:
            index = invocation.index("--timeout-seconds")
            assert invocation[index + 1] == "600"

        # Archive-local affinity is not allowed to replace the affinity sealed
        # by the source matrix, even when the contract, seal, and ledger agree.
        contract_path = assets / "provenance/capture-contract.json"
        contract_seal = contract_path.with_name(contract_path.name + ".sha256")
        original_contract = contract_path.read_bytes()
        original_contract_seal = contract_seal.read_bytes()
        contract_value = json.loads(original_contract)
        first_cpu = min(os.sched_getaffinity(0))
        alternate_affinity = str(first_cpu)
        if alternate_affinity == case.affinity:
            alternate_affinity = f"{first_cpu},{first_cpu}"
        contract_value["affinity_cpus"] = alternate_affinity
        replace_sealed_json(contract_path, contract_value)
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "sealed source affinity" in rejected.stderr
        replace_readonly(contract_path, original_contract)
        replace_readonly(contract_seal, original_contract_seal)
        reledger_and_reseal(output)

        # commands_uri is basename-bound; a byte-identical sibling asset tree
        # must not become the supplement namespace through a manifest rewrite.
        alternate_assets = output.parent / "alternate-completion.assets"
        shutil.copytree(assets, alternate_assets)
        original_manifest = output.read_bytes()
        original_manifest_seal = output.with_name(output.name + ".sha256").read_bytes()
        manifest_preamble, manifest_rows = bootstrap.core.parse_preamble_and_rows(
            output, bootstrap.core.PREAMBLE_KEYS, bootstrap.core.MANIFEST_HEADER
        )
        namespace_preamble = dict(manifest_preamble)
        namespace_preamble["commands_uri"] = (
            "manifest://alternate-completion.assets/commands.sh"
        )
        replace_manifest(output, namespace_preamble, list(manifest_rows))
        rejected = run(command(case, working, "audit", output), success=False)
        assert "exact supplement asset namespace" in rejected.stderr
        replace_readonly(output, original_manifest)
        replace_readonly(
            output.with_name(output.name + ".sha256"), original_manifest_seal
        )

        # A row status cannot be spliced from a capture made under a different
        # runner/contract, even when its detached seal and the ledger are new.
        tree_status = (
            assets
            / f"provenance/rows/{bootstrap.tree_row_id(1)}/status.json"
        )
        tree_status_seal = tree_status.with_name(tree_status.name + ".sha256")
        original_tree_status = tree_status.read_bytes()
        original_tree_status_seal = tree_status_seal.read_bytes()
        spliced_status = json.loads(original_tree_status)
        spliced_status["capture_identity"]["process_metrics_sha256"] = "0" * 64
        replace_sealed_json(tree_status, spliced_status)
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "capture row status" in rejected.stderr
        replace_readonly(tree_status, original_tree_status)
        replace_readonly(tree_status_seal, original_tree_status_seal)
        reledger_and_reseal(output)

        # Structurally valid, exactly re-statused input JSON still has to be the
        # canonical identity produced by replaying the archived protobuf.
        input_directory = assets / "provenance/inputs/tree0"
        input_canonical = input_directory / "canonical.json"
        input_status = input_directory / "status.json"
        input_status_seal = input_status.with_name(input_status.name + ".sha256")
        original_input_canonical = input_canonical.read_bytes()
        original_input_status = input_status.read_bytes()
        original_input_status_seal = input_status_seal.read_bytes()
        mutated_input = json.loads(original_input_canonical)
        for key in ("semantic_sha256", "clades_sha256", "productions_sha256"):
            mutated_input[key] = "0" * 64
        replace_readonly(input_canonical, bootstrap.json_bytes(mutated_input))
        input_evidence = bootstrap.phase78.InputEvidence(
            semantic_sha256=str(mutated_input["semantic_sha256"]),
            parsimony_min=int(mutated_input["parsimony_min"]),
            canonical_sha256=phase9_test.file_digest(input_canonical),
        )
        input_status_data = bootstrap.phase78.input_status_bytes(
            bootstrap.TREE_FIXTURE,
            input_evidence,
            input_directory,
            capture_identity,
        )
        replace_readonly(input_status, input_status_data)
        replace_readonly(
            input_status_seal,
            bootstrap.core.seal_bytes(input_status.name, input_status_data),
        )
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "input canonical identity" in rejected.stderr
        replace_readonly(input_canonical, original_input_canonical)
        replace_readonly(input_status, original_input_status)
        replace_readonly(input_status_seal, original_input_status_seal)
        reledger_and_reseal(output)

        # Likewise, a changed output protobuf with a freshly derived exact
        # status must fail the sealed-oracle canonical replay.
        qualification_directory = (
            assets
            / f"provenance/qualifications/{bootstrap.medium_row_id(1)}"
        )
        qualification_output = qualification_directory / "output.pb.gz"
        qualification_status = qualification_directory / "status.json"
        qualification_status_seal = qualification_status.with_name(
            qualification_status.name + ".sha256"
        )
        original_qualification_output = qualification_output.read_bytes()
        original_qualification_status = qualification_status.read_bytes()
        original_qualification_status_seal = qualification_status_seal.read_bytes()
        replace_readonly(
            qualification_output,
            original_qualification_output + b"canonical-identity-mutation\n",
        )
        qualification_status_data = bootstrap.qualification_status_bytes(
            qualification_directory,
            1,
            source_matrix[("medium", "off", 1)],
            base_manifest.sha256,
            case.affinity,
            runner_sha,
            working_sha,
            capture_identity,
        )
        replace_readonly(qualification_status, qualification_status_data)
        replace_readonly(
            qualification_status_seal,
            bootstrap.core.seal_bytes(
                qualification_status.name, qualification_status_data
            ),
        )
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "output protobuf canonical identity" in rejected.stderr
        replace_readonly(qualification_output, original_qualification_output)
        replace_readonly(qualification_status, original_qualification_status)
        replace_readonly(
            qualification_status_seal, original_qualification_status_seal
        )
        reledger_and_reseal(output)

        # No-replace qualification publication preserves a foreign empty
        # destination and leaves the complete owned staging evidence intact.
        race_root = case.root / "qualification-race"
        race_root.mkdir()
        staging = race_root / ".phase7-lazy-completion-medium-auto-w1.staging"
        staging.mkdir()
        marker = staging / "owned"
        marker.write_text("owned\n")
        destination = race_root / bootstrap.medium_row_id(1)

        def inject() -> None:
            destination.mkdir()

        try:
            bootstrap.phase78.seal_capture_directory(
                staging, destination, before_publish=inject
            )
        except bootstrap.core.BootstrapError as error:
            assert "already exists" in str(error)
        else:
            raise AssertionError("qualification publication replaced foreign target")
        assert destination.is_dir() and not list(destination.iterdir())
        assert marker.read_text() == "owned\n"

        # Archived source evidence is independently bound to the sealed base,
        # not merely to a self-consistent supplement ledger.
        archived_source = assets / "provenance/base/medium-off-w1/row.json"
        original_archived_source = archived_source.read_bytes()
        replace_readonly(archived_source, original_archived_source + b" ")
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "archived source row changed" in rejected.stderr
        replace_readonly(archived_source, original_archived_source)
        reledger_and_reseal(output)

        # Flip the complete W1 qualification branch coherently: report cache
        # strategy, exact status, manifest row, commands, seals, and ledger all
        # select the forced-on source.  The frozen v1 pilot integers still
        # independently derive "off", so this must be rejected at that gate.
        qreport = qualification_directory / "report.txt"
        flipped_report = qreport.read_bytes().replace(
            b"  lazy_policy_resolved: off\n",
            b"  lazy_policy_resolved: on\n",
        ).replace(
            b"  cache_strategy: all_active_patterns\n",
            b"  cache_strategy: lazy_multisite_chart\n",
        )
        assert flipped_report != qreport.read_bytes()
        replace_readonly(qreport, flipped_report)
        flipped_source = source_matrix[("medium", "on", 1)]
        flipped_status_data = bootstrap.qualification_status_bytes(
            qualification_directory,
            1,
            flipped_source,
            base_manifest.sha256,
            case.affinity,
            runner_sha,
            working_sha,
            capture_identity,
        )
        replace_readonly(qualification_status, flipped_status_data)
        replace_readonly(
            qualification_status_seal,
            bootstrap.core.seal_bytes(
                qualification_status.name, flipped_status_data
            ),
        )
        reledger_and_reseal(output)

        flipped_rows = [dict(row) for row in manifest.rows]
        flipped_row = bootstrap.qualification_row(
            flipped_source,
            1,
            case.affinity,
            "manifest://phase7-lazy-completion.assets/fixtures/medium.pb.gz",
            "manifest://phase7-lazy-completion.assets/fixtures/medium.refseq",
        )
        source_prefix = "provenance/base/medium-on-w1"
        flipped_row.update(
            {
                "canonical_sidecar_uri": (
                    "manifest://phase7-lazy-completion.assets/"
                    f"{source_prefix}/canonical.ndjson"
                ),
                "canonical_sidecar_sha256": flipped_source.row[
                    "canonical_sidecar_sha256"
                ],
                "oracle_report_uri": (
                    "manifest://phase7-lazy-completion.assets/"
                    f"{source_prefix}/canonical.json"
                ),
                "oracle_report_sha256": flipped_source.row[
                    "oracle_report_sha256"
                ],
            }
        )
        flipped_argv_sha = bootstrap.core.canonical_argv_digest(
            bootstrap.canonical_argv(flipped_row)
        )
        flipped_row["canonical_argv_sha256"] = flipped_argv_sha
        flipped_row["oracle_trial_semantic_sha256"] = bootstrap.core.trial_digest(
            flipped_row["method"],
            flipped_row["oracle_search_semantic_sha256"],
            flipped_row["oracle_output_semantic_sha256"],
            flipped_argv_sha,
        )
        flipped_rows[0] = flipped_row
        commands_path = assets / "commands.sh"
        flipped_commands = bootstrap.render_commands(
            flipped_rows,
            phase9_test.file_digest(assets / "assets.sha256"),
            base_manifest.preamble["frozen_oracle_dagutil_sha256"],
            working_sha,
            runner_sha,
        )
        replace_readonly(commands_path, flipped_commands, 0o555)
        flipped_preamble = dict(manifest.preamble)
        flipped_preamble["commands_sha256"] = sha256(flipped_commands)
        replace_manifest(output, flipped_preamble, flipped_rows)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "lazy_policy_resolved='on', expected 'off'" in rejected.stderr

    print("wric_phase7_completion_manifest_bootstrap_test: PASS")


if __name__ == "__main__":
    main()
