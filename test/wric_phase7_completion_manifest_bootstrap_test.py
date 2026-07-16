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
    auto = '''if policy == "auto":
    top["lazy_policy_requested"] = "auto"
    top["lazy_policy_resolved"] = "off"
    top["lazy_policy_frozen"] = "true"
    top["lazy_policy_pilot_runs"] = "1"
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

        wrong_hash = command(case, working, "build", output)
        wrong_hash[wrong_hash.index("--expected-working-chart-sha256") + 1] = "0" * 64
        rejected = run(wrong_hash, success=False)
        assert "working chart differs" in rejected.stderr
        assert not invocation_log.exists()

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
        # tree input + four tree searches/outputs + four auto searches/outputs
        assert len(invocations) == 17
        for invocation in invocations:
            index = invocation.index("--timeout-seconds")
            assert invocation[index + 1] == "600"

        # A coherently re-ledgered report mutation reaches the qualification
        # resolver check instead of merely tripping the outer asset hash.
        qreport = (
            assets
            / "provenance/qualifications/phase7-lazy-completion-medium-auto-w1/report.txt"
        )
        original_report = qreport.read_bytes()
        qreport.chmod(0o644)
        qreport.write_bytes(
            original_report.replace(
                b"  lazy_policy_resolved: off\n", b"  lazy_policy_resolved: on\n"
            )
        )
        qreport.chmod(0o444)
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert (
            "cache_strategy" in rejected.stderr
            or "differs from forced on" in rejected.stderr
            or "qualification" in rejected.stderr
        )
        qreport.chmod(0o644)
        qreport.write_bytes(original_report)
        qreport.chmod(0o444)
        reledger_and_reseal(output)
        run(command(case, working, "audit", output))

        # Inner status binding rejects a coherent status mutation after the
        # outer ledger has deliberately been updated to the changed bytes.
        status = (
            assets
            / "provenance/qualifications/phase7-lazy-completion-medium-auto-w1/status.json"
        )
        original_status = status.read_bytes()
        value = json.loads(original_status)
        value["working_chart_sha256"] = "0" * 64
        status.chmod(0o644)
        status.write_bytes(bootstrap.json_bytes(value))
        status.chmod(0o444)
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "status" in rejected.stderr or "working" in rejected.stderr
        status.chmod(0o644)
        status.write_bytes(original_status)
        status.chmod(0o444)
        reledger_and_reseal(output)
        run(command(case, working, "audit", output))

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
        archived_source.chmod(0o644)
        archived_source.write_bytes(archived_source.read_bytes() + b" ")
        archived_source.chmod(0o444)
        reledger_and_reseal(output)
        rejected = run(command(case, working, "audit", output), success=False)
        assert "archived source row changed" in rejected.stderr

    print("wric_phase7_completion_manifest_bootstrap_test: PASS")


if __name__ == "__main__":
    main()
