#!/usr/bin/env python3
"""End-to-end trust tests for the restartable Phase-9 supplement builder."""

from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

REPO = Path(__file__).resolve().parents[1]
TOOLS = REPO / "tools"
sys.path.insert(0, os.fspath(TOOLS))
import wric_phase9_manifest_bootstrap as bootstrap  # noqa: E402


HELPER = TOOLS / "wric_phase9_manifest_bootstrap.py"
HARNESS = TOOLS / "wric_spr_search_benchmark.sh"
ACCEPTANCE = TOOLS / "wric_phase9_acceptance.py"


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_text(label: str) -> str:
    return digest_bytes(label.encode("utf-8"))


def file_digest(path: Path) -> str:
    return digest_bytes(path.read_bytes())


def write_bytes(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(mode)


def seal(path: Path) -> None:
    write_bytes(
        path.with_name(path.name + ".sha256"),
        f"{file_digest(path)}  {path.name}\n".encode("ascii"),
    )


def allowed_affinity() -> str:
    completed = subprocess.run(
        ["taskset", "-pc", str(os.getpid())],
        check=True,
        text=True,
        capture_output=True,
    )
    return completed.stdout.strip().split(": ", 1)[1]


def report_bytes(seed: int, workers: int, accepted_ms: str = "120.250") -> bytes:
    top = dict(bootstrap.REPORT_BINDINGS)
    top.update(
        {
            "search_mode": "phase5_accept_reject_overlay_chain_local_commit",
            "accepted_state_update_mode": "overlay_chain_local_cache_commit",
            "accepted_state_materialization": "none_per_accept_overlay_chain_local_cache_update",
            "final_compaction_mode": "grammar_level_exact_multi_tree_compaction",
            "preserves_full_accepted_overlay_dag": "true",
            "actual_dag_mutation": "true",
            "output_dag_mutated": "true",
            "seed": str(seed),
            "chart_workers_requested": str(workers),
            "chart_workers_resolved": str(workers),
            "chart_worker_policy": "explicit",
            "refinement_exactness": "EXACT",
            "cache_strategy": "all_active_patterns",
            "effective_pattern_batch_size": "64",
            "final_compaction_exactness_kind": "exact_optimal_production_union",
            "chain_per_accept_exactness_label": "exact_multisite",
            "active_patterns": "64",
            "initial_grammar_clades": "23",
            "initial_grammar_productions": "11",
            "iterations": "3",
            "accepted_moves": "3",
            "local_commit_accepted_moves": "3",
            "candidate_accepts_attempted": "3",
            "accepted_exact_trims_reused": "3",
            "accepted_exact_trim_reuse_rejections": "0",
            "candidates_generated": "96",
            "candidates_scored": "96",
            "exact_verifications": "12",
            "post_materialization_rejections": "0",
            "sidecar_rebuilds_after_accept": "0",
            "initial_search_state_rebuilds": "1",
            "full_search_state_rebuilds": "1",
            "overlay_materializations_for_exact_verification": "3",
            "overlay_materializations_for_accept_materialization": "0",
            "overlay_materializations_for_final_compaction": "1",
            "final_compaction_rebuilds": "1",
            "local_commit_tombstone_scope_skips": "0",
            "inside_rows_recomputed_on_commit": "192",
            "outside_rows_recomputed_on_commit": "96",
            "local_commit_tip_grammar_refreshes": "3",
            "transient_chain_extensions_for_verification": "9",
            "transient_chain_diagnostic_cache_extensions": "0",
            "transient_chain_extension_fallbacks": "0",
            "transient_chain_extension_oracle_mismatches": "0",
            "local_leaf_state_owned_copies": "0",
            "pattern_batch_cache_builds": "0",
            "local_commit_inside_row_view_pattern_visits": "6144",
            "initial_state_inside_charts_built": "64",
            "inside_cache_inside_charts_built": "64",
            "inside_cache_resident_inside_charts_consumed": "0",
            "outside_cache_inside_charts_built": "0",
            "outside_cache_inside_charts_reused": "64",
            "outside_cache_outside_charts_built": "64",
            "initial_score": str(100 + seed),
            "final_score": str(70 + seed),
            "accepted_rebuild_ms": accepted_ms,
            "total_ms": "1000.000",
        }
    )
    acceptance = bootstrap.acceptance_module()
    parallel = workers > 1
    effective_grain = (
        64 if workers == 1 else max(1, (64 + 4 * workers - 1) // (4 * workers))
    )
    ranges_per_operation = (64 + effective_grain - 1) // effective_grain
    per_axis_ranges = 4 * ranges_per_operation
    per_axis_tasks = 0 if workers == 1 else 4 * min(
        workers, ranges_per_operation
    )
    active_axes = ("inside_cache", "outside_cache")
    for axis in acceptance.AXES:
        active = axis in active_axes
        axis_fields = {
            "operations": 4 if active else 0,
            "items": 256 if active else 0,
            "ranges": per_axis_ranges if active else 0,
            "tasks": per_axis_tasks if active else 0,
            "active_worker_high_water": workers if active else 0,
            "parallel_operations": 4 if active and parallel else 0,
            "minimum_effective_grain": effective_grain if active else 0,
            "maximum_effective_grain": effective_grain if active else 0,
        }
        for field, value in axis_fields.items():
            top[f"chart_axis_{axis}_{field}"] = str(value)
    total_ranges = 2 * per_axis_ranges
    total_tasks = 2 * per_axis_tasks
    top.update(
        {
            "chart_worker_resolution_policy": "explicit",
            "chart_workers_actually_active_high_water": str(workers),
            "chart_scheduler_operations": "8",
            "chart_scheduler_parallel_operations": "8" if parallel else "0",
            "chart_scheduler_serial_fallbacks": "0" if parallel else "8",
            "chart_scheduler_ranges_created": str(total_ranges),
            "chart_scheduler_ranges_completed": str(total_ranges),
            "chart_scheduler_ranges_cancelled": "0",
            "chart_scheduler_tasks_submitted": str(total_tasks),
            "chart_scheduler_tasks_completed": str(total_tasks),
            "chart_scheduler_tasks_joined": str(total_tasks),
            "chart_scheduler_pending_tasks": "0",
            "chart_scheduler_pending_tasks_at_shutdown": "0",
            "chart_scheduler_nested_serial_fallbacks": "0",
            "chart_scheduler_rejected_concurrent_operations": "0",
            "chart_scheduler_pool_lifetimes": "1" if parallel else "0",
            "chart_scheduler_pool_lifetimes_stopped": "1" if parallel else "0",
            "chart_scheduler_live_pool_threads": "0",
            "chart_scheduler_shutdown": "true",
            "chart_scheduler_minimum_effective_grain": str(effective_grain),
            "chart_scheduler_maximum_effective_grain": str(effective_grain),
            "chart_scheduler_queue_wait_nanoseconds": str(total_tasks * 10),
            "chart_scheduler_queue_wait_nanoseconds_max": "10" if parallel else "0",
            "chart_scheduler_queue_wait_samples": str(total_tasks),
        }
    )
    lines = ["chart_spr_search:"]
    lines.extend(f"  {key}: {value}" for key, value in top.items())
    lines.append("  iteration_reports:")
    for iteration in range(3):
        before = 100 + seed - 10 * iteration
        after = before - 10
        iteration_fields = {
            "candidates_generated": "32",
            "candidates_scored": "32",
            "candidates_exact_verified": "4",
            "candidate_score_failures": "0",
            "accepted_move_present": "true",
            "accepted_move_committed": "true",
            "post_materialization_rejected": "false",
            "reused_patterns_after_accept": "true",
            "accepted_exact_kind": "grammar_exact",
            "state_score_before": str(before),
            "state_score_after": str(after),
            "accepted_exact_new_score": str(after),
            "accepted_exact_delta": "-10",
            "accepted_lower_bound_new_score": str(after),
            "accepted_lower_bound_delta": "-10",
            "post_materialization_rebuilt_score": str(after),
            "accepted_candidate_signature": f"seed{seed}-move{iteration}",
            "accepted_affected_clades": "12",
            "accepted_inside_rows_recomputed": "64",
            "accepted_outside_rows_recomputed": "32",
            "accepted_topology_selection": "none",
        }
        lines.append(f"    - iteration: {iteration}")
        lines.extend(
            f"      {key}: {value}" for key, value in iteration_fields.items()
        )
        lines.extend(("      candidate_generation:", "        stop_reason: candidate_cap"))
    return ("\n".join(lines) + "\n").encode("utf-8")


def sidecar_bytes(seed: int, *, variant: str = "") -> bytes:
    initial = 100 + seed
    final = 70 + seed
    records: list[dict[str, object]] = [
        {
            "record": "schema",
            "schema": "larch.chart_spr.semantic.ndjson",
            "schema_version": 1,
        },
        {
            "record": "contract",
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
            "keep_mask_kind": "exact_optimal_production_union",
            "candidate_cap_semantics": "post_dedup",
            "max_iterations": 3,
            "max_candidates": 32,
            "top_k_exact": 4,
            "seed": seed,
            "polytomy_max_shapes": 1,
            "refinement_exactness": "EXACT",
            "use_bound_pruning": True,
            "require_exact_keep_mask": True,
            "score_ua_edge": False,
            "randomize_order": False,
            "reservoir_sample": False,
            "include_immediate_reversals": False,
        },
        {
            "record": "initial_state",
            "initial_score": initial,
            "active_patterns": 64,
        },
    ]
    if variant:
        records.append({"record": "metadata", "variant": variant})
    for iteration in range(3):
        before = initial - iteration * 10
        after = before - 10
        selected_signature = f"seed{seed}-move{iteration}"
        records.append(
            {
                "record": "iteration_begin",
                "iteration": iteration,
                "seed": seed + iteration,
                "state_score_before": before,
            }
        )
        for stream in range(32):
            signature = selected_signature if stream == 0 else f"seed{seed}-i{iteration}-c{stream}"
            records.append(
                {
                    "record": "candidate",
                    "iteration": iteration,
                    "stream_index": stream,
                    "signature": signature,
                    "valid": True,
                    "affected_clade_count": 12,
                }
            )
            candidate_new = after if stream == 0 else before
            records.append(
                {
                    "record": "candidate_lower_bound",
                    "iteration": iteration,
                    "stream_index": stream,
                    "kind": "composite_lower_bound",
                    "exact_multisite": False,
                    "old_score": before,
                    "new_score": candidate_new,
                    "delta": candidate_new - before,
                }
            )
            if stream < 4:
                records.append(
                    {
                        "record": "candidate_exact",
                        "iteration": iteration,
                        "stream_index": stream,
                        "kind": "grammar_exact",
                        "exact_multisite": True,
                        "old_score": before,
                        "new_score": candidate_new,
                        "delta": candidate_new - before,
                    }
                )
        records.extend(
            (
                {
                    "record": "iteration_outcome",
                    "iteration": iteration,
                    "generation_stop_reason": "candidate_cap",
                    "candidates_generated": 32,
                    "candidates_scored": 32,
                    "candidates_exact_verified": 4,
                    "accepted_move_present": True,
                    "accepted_move_committed": True,
                    "post_materialization_rejected": False,
                    "selected_stream_index": 0,
                    "selected_signature": selected_signature,
                    "state_score_after": after,
                },
                {
                    "record": "chain_entry",
                    "position": iteration,
                    "commit_source": "spr_overlay_delta",
                    "added_production_keys": [f"added-{seed}-{iteration}"],
                    "tombstoned_production_keys": [f"removed-{seed}-{iteration}"],
                },
            )
        )
    records.append(
        {"record": "final_state", "final_score": final, "accepted_moves": 3}
    )
    return b"".join(
        (json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n").encode()
        for record in records
    )


def search_digest(
    seed: int, semantic_sha256: str, record_count: int
) -> dict[str, object]:
    return {
        "schema": "larch.chart_spr.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
        "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
        "semantic_sha256": semantic_sha256,
        "contract_sha256": digest_text(f"contract-{seed}"),
        "candidates_sha256": digest_text(f"candidates-{seed}"),
        "exact_sha256": digest_text(f"exact-{seed}"),
        "acceptance_sha256": digest_text(f"acceptance-{seed}"),
        "chain_sha256": digest_text(f"chain-{seed}"),
        "final_topology_sha256": digest_text(f"topology-{seed}"),
        "record_count": record_count,
        "candidate_count": 96,
        "exact_candidate_count": 12,
        "iteration_count": 3,
    }


def output_digest(seed: int) -> dict[str, object]:
    return {
        "schema": "larch.dag.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
        "semantic_sha256": digest_text(f"output-{seed}"),
        "clades_sha256": digest_text(f"clades-{seed}"),
        "productions_sha256": digest_text(f"productions-{seed}"),
        "clade_count": 23,
        "production_count": 11,
        "parsimony_min": 120 + seed,
    }


def input_digest() -> dict[str, object]:
    return {
        "schema": "larch.dag.semantic_digest",
        "schema_version": 1,
        "digest_algorithm": "sha256",
        "semantic_sha256": digest_text("input"),
        "clades_sha256": digest_text("input-clades"),
        "productions_sha256": digest_text("input-productions"),
        "clade_count": 31,
        "production_count": 17,
        "parsimony_min": 200,
    }


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n").encode()


class Integration:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.base_repo_root = root
        subprocess.run(["git", "init", "-q", root], check=True)
        subprocess.run(
            ["git", "-C", os.fspath(root), "config", "user.email", "phase9@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", os.fspath(root), "config", "user.name", "Phase 9 test"],
            check=True,
        )
        marker = root / "sealed-base-revision.txt"
        marker.write_text("synthetic sealed base revision\n")
        subprocess.run(
            ["git", "-C", os.fspath(root), "add", marker.name], check=True
        )
        subprocess.run(
            ["git", "-C", os.fspath(root), "commit", "-q", "-m", "sealed base"],
            check=True,
        )
        self.base_revision = subprocess.run(
            ["git", "-C", os.fspath(root), "rev-parse", "--verify", "HEAD"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()
        self.affinity = allowed_affinity()
        self.base_dir = root / "base"
        self.golden = root / "golden"
        self.characterization_dir = root / "characterization"
        self.fixture = root / "fixture.pb.gz"
        self.counter = root / "oracle-chart-invocations.txt"
        write_bytes(self.fixture, b"synthetic strict Phase-9 fixture\n")
        self.fixture_sha = file_digest(self.fixture)
        self.input_counter = root / "oracle-input-invocations.txt"
        self.failure_marker = root / "oracle-fail-row.txt"
        self._write_golden()
        self.oracle = self.base_dir / "dagutil"
        self.larch2 = self.base_dir / "larch2"
        try:
            self.runner = Path(
                os.environ["WRIC_PHASE9_TEST_PROCESS_METRICS"]
            ).resolve(strict=True)
        except KeyError as error:
            raise RuntimeError(
                "WRIC_PHASE9_TEST_PROCESS_METRICS must name the explicit CMake target"
            ) from error
        self._write_executables()
        self.base = self._write_base()
        self.parent_sha = file_digest(self.base)
        self.characterization = self._write_characterization(
            self.characterization_dir, self.parent_sha
        )

    def evidence_directory(self, seed: int, workers: int) -> Path:
        return self.golden / f"seed{seed}" / f"w{workers}"

    def _write_golden(self) -> None:
        write_bytes(
            self.golden / "input-canonical.json", json_bytes(input_digest())
        )
        for seed in bootstrap.SEEDS:
            sidecar = sidecar_bytes(seed)
            search = search_digest(
                seed, digest_bytes(sidecar), len(sidecar.splitlines())
            )
            output = output_digest(seed)
            for workers in bootstrap.WORKERS:
                directory = self.evidence_directory(seed, workers)
                write_bytes(directory / "report.txt", report_bytes(seed, workers))
                write_bytes(directory / "canonical.ndjson", sidecar)
                write_bytes(directory / "canonical.json", json_bytes(search))
                write_bytes(directory / "output-canonical.json", json_bytes(output))

    def _write_executables(self) -> None:
        script = f'''#!/usr/bin/python3
import pathlib
import shutil
import sys

args = sys.argv[1:]
if "--help" in args:
    print("  --canonical-dag-result PATH")
    raise SystemExit(0)
def value(flag):
    return args[args.index(flag) + 1]
golden = pathlib.Path({os.fspath(self.golden)!r})
failure_marker = pathlib.Path({os.fspath(self.failure_marker)!r})
if "--chart-spr-search" in args:
    seed = int(value("--seed"))
    workers = int(value("--chart-spr-workers"))
    source = golden / f"seed{{seed}}" / f"w{{workers}}"
    pathlib.Path({os.fspath(self.counter)!r}).parent.mkdir(parents=True, exist_ok=True)
    with pathlib.Path({os.fspath(self.counter)!r}).open("a") as stream:
        stream.write(f"{{seed}} {{workers}}\\n")
    if failure_marker.exists() and failure_marker.read_text().strip() == f"{{seed}} {{workers}}":
        failure_marker.unlink()
        print("injected characterization row failure", file=sys.stderr)
        raise SystemExit(42)
    shutil.copyfile(source / "canonical.json", value("--chart-spr-canonical-result"))
    shutil.copyfile(source / "canonical.ndjson", value("--chart-spr-canonical-sidecar"))
    pathlib.Path(value("-o")).write_text(f"{{seed}} {{workers}}\\n")
    sys.stdout.buffer.write((source / "report.txt").read_bytes())
else:
    dag_text = pathlib.Path(value("--dag-pb")).read_text().split()
    if len(dag_text) == 2 and all(item.isdigit() for item in dag_text):
        seed, workers = map(int, dag_text)
        source = golden / f"seed{{seed}}" / f"w{{workers}}"
        canonical = source / "output-canonical.json"
        parsimony = 120 + seed
    else:
        canonical = golden / "input-canonical.json"
        parsimony = 200
        pathlib.Path({os.fspath(self.input_counter)!r}).parent.mkdir(parents=True, exist_ok=True)
        with pathlib.Path({os.fspath(self.input_counter)!r}).open("a") as stream:
            stream.write("input\\n")
    shutil.copyfile(canonical, value("--canonical-dag-result"))
    print("dag_info:")
    print(f"  parsimony_min: score:{{parsimony}}")
'''
        write_bytes(self.oracle, script.encode(), 0o555)
        write_bytes(
            self.larch2,
            b"#!/usr/bin/env bash\n[[ ${1:-} == --help ]] && exit 0\nexit 0\n",
            0o555,
        )

    def evidence_values(self, seed: int, workers: int, root: Path) -> dict[str, str]:
        directory = root / f"seed{seed}" / f"w{workers}"
        sidecar = directory / "canonical.ndjson"
        canonical = directory / "canonical.json"
        output = directory / "output-canonical.json"
        contract = bootstrap.phase9_contract_row(
            seed,
            workers,
            self.fixture_sha,
            "manifest://fixture.pb.gz",
            self.affinity,
        )
        argv_sha = bootstrap.canonical_argv_digest(bootstrap.canonical_argv(contract))
        search_semantic = str(json.loads(canonical.read_text())["semantic_sha256"])
        output_semantic = str(json.loads(output.read_text())["semantic_sha256"])
        prefix = f"evidence/seed{seed}/w{workers}"
        return {
            "seed": str(seed),
            "workers": str(workers),
            "product_report_path": f"{prefix}/report.txt",
            "product_report_sha256": file_digest(directory / "report.txt"),
            "canonical_sidecar_path": f"{prefix}/canonical.ndjson",
            "canonical_sidecar_sha256": file_digest(sidecar),
            "canonical_result_path": f"{prefix}/canonical.json",
            "canonical_result_sha256": file_digest(canonical),
            "output_canonical_path": f"{prefix}/output-canonical.json",
            "output_canonical_sha256": file_digest(output),
            "canonical_argv_sha256": argv_sha,
            "oracle_search_semantic_sha256": search_semantic,
            "oracle_output_semantic_sha256": output_semantic,
            "oracle_trial_semantic_sha256": bootstrap.trial_digest(
                bootstrap.METHOD, search_semantic, output_semantic, argv_sha
            ),
        }

    def _write_base(self) -> Path:
        self.base_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.fixture, self.base_dir / "fixture.pb.gz")
        source = self.evidence_directory(1, 1)
        shutil.copyfile(source / "canonical.ndjson", self.base_dir / "canonical.ndjson")
        shutil.copyfile(source / "canonical.json", self.base_dir / "canonical.json")
        write_bytes(self.base_dir / "commands.sh", b"#!/usr/bin/env bash\nexit 0\n", 0o555)
        item = bootstrap.phase9_contract_row(
            1,
            1,
            self.fixture_sha,
            "manifest://fixture.pb.gz",
            self.affinity,
        )
        item.update(
            {
                "row_id": "synthetic-base-row",
                "run_group": "synthetic-base",
                "workload_name": "synthetic-base-workload",
                "fixture_id": "synthetic-base-fixture",
                "expected_refinement_exactness": "EXACT",
                "expected_cache_strategy": "all_active_patterns",
                "expected_effective_pattern_batch_size": "64",
                "expected_keep_mask_kind": "exact_optimal_production_union",
                "expected_final_compaction_exactness": "exact_optimal_production_union",
                "expected_chain_exactness": "exact_multisite",
                "expected_active_patterns": "64",
                "expected_initial_clades": "23",
                "expected_initial_productions": "11",
                "expected_candidates_generated": "96",
                "expected_candidates_scored": "96",
                "expected_exact_verifications": "12",
                "expected_stop_reason": "candidate_cap",
                "expected_iterations": "3",
                "expected_accepted_moves": "3",
                "expected_initial_score": "200",
                "expected_final_score": "71",
                "expected_validated_parsimony": "121",
                "oracle_search_semantic_sha256": digest_bytes(
                    (self.base_dir / "canonical.ndjson").read_bytes()
                ),
                "oracle_output_semantic_sha256": str(
                    output_digest(1)["semantic_sha256"]
                ),
                "canonical_sidecar_uri": "manifest://canonical.ndjson",
                "canonical_sidecar_sha256": file_digest(
                    self.base_dir / "canonical.ndjson"
                ),
                "oracle_report_uri": "manifest://canonical.json",
                "oracle_report_sha256": file_digest(self.base_dir / "canonical.json"),
            }
        )
        item["canonical_argv_sha256"] = bootstrap.canonical_argv_digest(
            bootstrap.canonical_argv(item)
        )
        item["oracle_trial_semantic_sha256"] = bootstrap.trial_digest(
            bootstrap.METHOD,
            item["oracle_search_semantic_sha256"],
            item["oracle_output_semantic_sha256"],
            item["canonical_argv_sha256"],
        )
        preamble = {
            "schema": bootstrap.SCHEMA,
            "schema_version": bootstrap.SCHEMA_VERSION,
            "kind": "base",
            "manifest_id": "synthetic-phase0",
            "parent_sha256": "-",
            "repo_revision": self.base_revision,
            "merge_base": "1" * 40,
            "frozen_larch2_uri": "manifest://larch2",
            "frozen_larch2_sha256": file_digest(self.larch2),
            "frozen_oracle_dagutil_uri": "manifest://dagutil",
            "frozen_oracle_dagutil_sha256": file_digest(self.oracle),
            "commands_uri": "manifest://commands.sh",
            "commands_sha256": file_digest(self.base_dir / "commands.sh"),
        }
        path = self.base_dir / "workloads.tsv"
        write_bytes(path, bootstrap.tsv_bytes(preamble, [item]))
        seal(path)
        return path

    def _write_characterization(self, directory: Path, parent_sha: str) -> Path:
        evidence_root = directory / "evidence"
        shutil.copytree(self.golden, evidence_root)
        rows = [
            self.evidence_values(seed, workers, evidence_root)
            for seed in bootstrap.SEEDS
            for workers in bootstrap.WORKERS
        ]
        preamble = {
            "schema": bootstrap.CHAR_SCHEMA,
            "schema_version": bootstrap.CHAR_SCHEMA_VERSION,
            "parent_sha256": parent_sha,
            "primary_sha256": self.fixture_sha,
            "input_canonical_path": "evidence/input-canonical.json",
            "input_canonical_sha256": file_digest(
                evidence_root / "input-canonical.json"
            ),
            "frozen_oracle_sha256": file_digest(self.oracle),
            "affinity_cpus": self.affinity,
            "timeout_seconds": str(bootstrap.TIMEOUT_SECONDS),
            "rss_limit_bytes": str(bootstrap.RSS_LIMIT_BYTES),
            "memory_budget_bytes": str(bootstrap.MEMORY_BUDGET_BYTES),
        }
        path = directory / "frozen-input.tsv"
        write_bytes(path, bootstrap.characterization_tsv_bytes(preamble, rows))
        seal(path)
        return path

    def build_command(
        self,
        output: Path,
        *,
        characterization: Path | None = None,
        fixture: Path | None = None,
        capture: Path | None = None,
        expected_parent: str | None = None,
    ) -> list[str]:
        return [
            sys.executable,
            os.fspath(HELPER),
            "build",
            "--base-repo-root",
            os.fspath(self.base_repo_root),
            "--base-manifest",
            os.fspath(self.base),
            "--expected-parent-sha256",
            expected_parent or self.parent_sha,
            "--characterization",
            os.fspath(characterization or self.characterization),
            "--fixture",
            os.fspath(fixture or self.fixture),
            "--expected-fixture-sha256",
            self.fixture_sha,
            "--capture-dir",
            os.fspath(capture or (self.root / "capture")),
            "--output",
            os.fspath(output),
            "--benchmark-harness",
            os.fspath(HARNESS),
            "--process-metrics",
            os.fspath(self.runner),
        ]

    def characterize_command(
        self,
        output: Path,
        *,
        capture: Path | None = None,
        process_metrics: Path | None = None,
        expected_process_metrics_sha256: str | None = None,
        expected_parent: str | None = None,
        fixture: Path | None = None,
    ) -> list[str]:
        runner = process_metrics or self.runner
        return [
            sys.executable,
            os.fspath(HELPER),
            "characterize",
            "--base-repo-root",
            os.fspath(self.base_repo_root),
            "--base-manifest",
            os.fspath(self.base),
            "--expected-parent-sha256",
            expected_parent or self.parent_sha,
            "--fixture",
            os.fspath(fixture or self.fixture),
            "--expected-fixture-sha256",
            self.fixture_sha,
            "--affinity-cpus",
            self.affinity,
            "--capture-dir",
            os.fspath(capture or (self.root / "characterization-capture")),
            "--output",
            os.fspath(output),
            "--process-metrics",
            os.fspath(runner),
            "--expected-process-metrics-sha256",
            expected_process_metrics_sha256 or file_digest(self.runner),
        ]

    def write_current_run(self, run_root: Path, supplement: Path) -> None:
        acceptance = bootstrap.acceptance_module()
        audited = bootstrap.audited_frozen_characterization(
            self.base, self.parent_sha, supplement, self.base_repo_root
        )
        frozen_rows = {
            (int(row["seed"]), int(row["requested_workers"])): row
            for row in audited.supplement.rows
        }
        raw_path = run_root / "raw_trials.tsv"
        rows: list[dict[str, str]] = []
        for seed in bootstrap.SEEDS:
            for workers in acceptance.MEASURED_WORKERS:
                frozen = frozen_rows[(seed, workers)]
                accepted_ms = "180.000" if workers == 1 else "100.000"
                golden = self.evidence_directory(seed, workers)
                warmup = acceptance.expected_phase9_warmup_compact_path(
                    run_root,
                    {
                        "fixture": bootstrap.workload_name(seed),
                        "row_id": bootstrap.row_id(seed, workers),
                    },
                    workers,
                )
                write_bytes(warmup, (golden / "canonical.json").read_bytes())
                for trial in range(1, 4):
                    report_relative = (
                        f"logs/{bootstrap.workload_name(seed)}_{bootstrap.METHOD}_"
                        f"trial{trial}_{bootstrap.row_id(seed, workers)}_w{workers}.out"
                    )
                    report_path = run_root / report_relative
                    write_bytes(report_path, report_bytes(seed, workers, accepted_ms))
                    row = {column: "0" for column in acceptance.REQUIRED_COLUMNS}
                    row.update(
                        {
                            "fixture": bootstrap.workload_name(seed),
                            "method": bootstrap.METHOD,
                            "status": "ok",
                            "validation_status": "ok",
                            "initial_validated_parsimony_min": frozen[
                                "expected_initial_score"
                            ],
                            "final_validated_parsimony_min": frozen[
                                "expected_validated_parsimony"
                            ],
                            "best_reported_objective": frozen[
                                "expected_final_score"
                            ],
                            "best_validated_parsimony_min": frozen[
                                "expected_validated_parsimony"
                            ],
                            "iterations": "3",
                            "seed": str(seed),
                            "acceptance": "exact_multisite",
                            "candidate_selection": "lower_bound_top_k",
                            "candidate_source": "grammar",
                            "objective": "grammar_exact",
                            "candidates_generated": "96",
                            "candidates_scored": "96",
                            "exact_verifications": "12",
                            "accepted_moves": "3",
                            "candidate_accepts_attempted": "3",
                            "post_materialization_rejections": "0",
                            "accepted_rebuild_ms": accepted_ms,
                            "total_ms": "1000.000",
                            "active_patterns": "64",
                            "report_path": report_relative,
                            "row_id": bootstrap.row_id(seed, workers),
                            "requested_workers": str(workers),
                            "resolved_workers": str(workers),
                            "worker_policy": "explicit",
                            "trial_index": str(trial),
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
                            "wall_clock_s": "1.100",
                            "user_cpu_s": "0.800" if workers == 1 else "4.000",
                            "system_cpu_s": "0.100",
                            "peak_sampled_rss_kb": "1000" if workers == 1 else "1500",
                            "peak_sampled_swap_kb": "0",
                            "process_rss_limit_bytes": str(bootstrap.RSS_LIMIT_BYTES),
                            "configured_chart_memory_budget": str(
                                bootstrap.MEMORY_BUDGET_BYTES
                            ),
                            "manifest_rss_limit_bytes": str(bootstrap.RSS_LIMIT_BYTES),
                            "input_sha256": frozen["primary_sha256"],
                            "refseq_sha256": "NA",
                            "search_semantic_sha256": frozen[
                                "oracle_search_semantic_sha256"
                            ],
                            "output_semantic_sha256": frozen[
                                "oracle_output_semantic_sha256"
                            ],
                            "trial_semantic_sha256": frozen[
                                "oracle_trial_semantic_sha256"
                            ],
                            "canonical_argv_sha256": frozen[
                                "canonical_argv_sha256"
                            ],
                            "canonical_digest": frozen[
                                "oracle_trial_semantic_sha256"
                            ],
                        }
                    )
                    compact, full, sidecar, dag = acceptance.canonical_paths(
                        raw_path, row, report_path
                    )
                    write_bytes(compact, (golden / "canonical.json").read_bytes())
                    write_bytes(full, (golden / "canonical.json").read_bytes())
                    write_bytes(sidecar, (golden / "canonical.ndjson").read_bytes())
                    write_bytes(dag, (golden / "output-canonical.json").read_bytes())
                    rows.append(row)
        run_root.mkdir(parents=True, exist_ok=True)
        with raw_path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=acceptance.REQUIRED_COLUMNS,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(rows)
        write_bytes(
            run_root / "summary.md",
            b"# Synthetic Phase-9 run\n\nConfiguration: workers=1 8, warmups=1, repetitions=3, full-canonical=true\n",
        )
        execution_labels = [
            f"{suffix}_{bootstrap.row_id(seed, workers)}"
            for seed in bootstrap.SEEDS
            for workers in acceptance.MEASURED_WORKERS
            for suffix in ("warmup1", "trial1", "trial2", "trial3")
        ]
        write_bytes(
            run_root / "commands.sh",
            ("#!/usr/bin/env bash\n" + "\n".join(f"# {item}" for item in execution_labels) + "\n").encode(),
            0o555,
        )

    @staticmethod
    def run(command: list[str], *, success: bool) -> subprocess.CompletedProcess[str]:
        completed = subprocess.run(command, check=False, text=True, capture_output=True)
        if success and completed.returncode != 0:
            raise AssertionError(completed.stderr + completed.stdout)
        if not success and completed.returncode == 0:
            raise AssertionError(f"command unexpectedly passed: {command}")
        return completed

    def clone_characterization(self, name: str) -> Path:
        target = self.root / name
        shutil.copytree(self.characterization_dir, target)
        return target / self.characterization.name

    @staticmethod
    def edit_tsv(path: Path, editor) -> None:  # type: ignore[no-untyped-def]
        lines = path.read_text().splitlines()
        header_index = len(bootstrap.CHAR_PREAMBLE_KEYS)
        header = lines[header_index].split("\t")
        rows = list(csv.DictReader(lines[header_index:], delimiter="\t"))
        editor(lines, header, rows)
        prefix = lines[:header_index]
        rendered = prefix + ["\t".join(header)]
        rendered.extend("\t".join(row[field] for field in header) for row in rows)
        path.write_text("\n".join(rendered) + "\n")
        seal(path)


def tree_bytes(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in root.rglob("*")
        if path.is_file()
    }


def final_publication_bytes(output: Path) -> dict[str, bytes]:
    assets = output.with_name(output.stem + ".assets")
    seal_path = output.with_name(output.name + ".sha256")
    result = {
        output.name: output.read_bytes(),
        seal_path.name: seal_path.read_bytes(),
    }
    result.update(
        {
            f"{assets.name}/{relative}": data
            for relative, data in tree_bytes(assets).items()
        }
    )
    return result


def inject_publication_crash(
    output: Path, source: Path, crash_point: str
) -> bootstrap.PublicationPaths:
    """Fork a hard exit after one durable publication boundary."""

    output.parent.mkdir(parents=True)
    source_assets = source.with_name(source.stem + ".assets")
    source_seal = source.with_name(source.name + ".sha256")
    asset_files = tree_bytes(source_assets)
    process = os.fork()
    if process == 0:
        try:
            with bootstrap.exclusive_output_lock(output) as publication:
                bootstrap.publish_immutable_supplement(
                    publication,
                    asset_files,
                    source.read_bytes(),
                    source_seal.read_bytes(),
                    crash_hook=lambda point: (
                        os._exit(86) if point == crash_point else None
                    ),
                )
        except BaseException:
            os._exit(87)
        os._exit(88)
    _, status = os.waitpid(process, 0)
    assert os.WIFEXITED(status), status
    assert os.WEXITSTATUS(status) == 86, (crash_point, status)
    return bootstrap.publication_paths(output)


def replace_file(path: Path, data: bytes, mode: int) -> None:
    path.chmod(0o644)
    path.write_bytes(data)
    path.chmod(mode)


def replace_sealed_file(path: Path, data: bytes, mode: int = 0o444) -> None:
    replace_file(path, data, mode)
    seal_path = path.with_name(path.name + ".sha256")
    replace_file(
        seal_path,
        f"{file_digest(path)}  {path.name}\n".encode("ascii"),
        0o444,
    )


def clone_supplement(source: Path, parent: Path) -> Path:
    target = parent / source.name
    parent.mkdir(parents=True)
    shutil.copy2(source, target)
    shutil.copy2(source.with_name(source.name + ".sha256"), target.with_name(target.name + ".sha256"))
    shutil.copytree(
        source.with_name(source.stem + ".assets"),
        target.with_name(target.stem + ".assets"),
    )
    return target


def rebind_supplement_commands(supplement: Path, commands_data: bytes) -> None:
    assets = supplement.with_name(supplement.stem + ".assets")
    replace_file(assets / "commands.sh", commands_data, 0o555)
    preamble, rows = bootstrap.parse_preamble_and_rows(
        supplement, bootstrap.PREAMBLE_KEYS, bootstrap.MANIFEST_HEADER
    )
    updated = dict(preamble)
    updated["commands_sha256"] = digest_bytes(commands_data)
    replace_sealed_file(supplement, bootstrap.tsv_bytes(updated, rows))


def rebuild_asset_ledger_and_commands(supplement: Path) -> None:
    assets = supplement.with_name(supplement.stem + ".assets")
    members = {
        path.relative_to(assets).as_posix(): path.read_bytes()
        for path in assets.rglob("*")
        if path.is_file()
        and path.name not in {"assets.sha256", "commands.sh"}
    }
    ledger_data = bootstrap.asset_ledger_bytes(members)
    replace_file(assets / "assets.sha256", ledger_data, 0o444)
    preamble, rows = bootstrap.parse_preamble_and_rows(
        supplement, bootstrap.PREAMBLE_KEYS, bootstrap.MANIFEST_HEADER
    )
    archived = bootstrap.command_declaration(
        assets / "commands.sh", "archived_characterization", bootstrap.LEDGER_RELATIVE
    )
    commands_data = bootstrap.render_commands(
        rows,
        preamble["frozen_oracle_dagutil_sha256"],
        rows[0]["affinity_cpus"],
        digest_bytes(ledger_data),
        archived,
    )
    rebind_supplement_commands(supplement, commands_data)


def expect_bootstrap_error(action, fragment: str) -> None:  # type: ignore[no-untyped-def]
    try:
        action()
    except bootstrap.BootstrapError as error:
        assert fragment in str(error), str(error)
    else:
        raise AssertionError(f"expected BootstrapError containing {fragment!r}")


def main() -> None:
    with tempfile.TemporaryDirectory(
        prefix="wric-phase9-manifest-bootstrap-", dir=REPO / "build"
    ) as temporary:
        case = Integration(Path(temporary))

        produced = case.root / "produced" / "frozen-input.tsv"
        producer_capture = case.root / "producer-capture"
        case.run(
            case.characterize_command(produced, capture=producer_capture),
            success=True,
        )
        assert case.input_counter.read_text().splitlines() == ["input"]
        assert case.counter.read_text().splitlines() == [
            f"{seed} {workers}"
            for seed in bootstrap.SEEDS
            for workers in bootstrap.WORKERS
        ]
        produced_characterization = bootstrap.audit_characterization_publication(
            case.base,
            case.parent_sha,
            produced,
            case.fixture,
            case.fixture_sha,
            case.runner,
            file_digest(case.runner),
            case.affinity,
            case.base_repo_root,
        )
        assert produced_characterization.preamble["schema_version"] == "3"
        assert produced_characterization.preamble["timeout_seconds"] == str(
            bootstrap.TIMEOUT_SECONDS
        )
        produced_input_parts = Path(
            produced_characterization.preamble["input_canonical_path"]
        ).parts
        assert produced_input_parts[0] == "frozen-input.assets"
        assert produced_input_parts[1].startswith(bootstrap.CHAR_CLOSURE_PREFIX)
        produced_assets = produced.with_name(produced.stem + ".assets")
        produced_contract = bootstrap.read_json_object(
            produced_assets
            / produced_input_parts[1]
            / "provenance/capture-contract.json",
            "produced characterization contract",
        )
        assert produced_contract["process_metrics_sha256"] == file_digest(
            case.runner
        )
        assert produced_contract["parent_sha256"] == case.parent_sha
        assert produced_contract["fixture_sha256"] == case.fixture_sha
        assert len(produced_contract["rows"]) == 12
        assert all(
            row["canonical_argv_sha256"]
            == bootstrap.canonical_argv_digest(row["canonical_argv"])
            for row in produced_contract["rows"]
        )
        assert all(
            row["canonical_argv"][-4:]
            == [
                "--chart-spr-canonical-result",
                "@search-canonical-result",
                "-o",
                "@output",
            ]
            for row in produced_contract["rows"]
        )

        producer_calls = case.counter.read_bytes()
        input_calls = case.input_counter.read_bytes()
        produced_bytes = final_publication_bytes(produced)
        case.run(
            case.characterize_command(produced, capture=producer_capture),
            success=True,
        )
        assert case.counter.read_bytes() == producer_calls
        assert case.input_counter.read_bytes() == input_calls
        assert final_publication_bytes(produced) == produced_bytes

        reused = case.root / "producer-reuse" / "frozen-input.tsv"
        case.run(
            case.characterize_command(reused, capture=producer_capture),
            success=True,
        )
        assert case.counter.read_bytes() == producer_calls
        assert case.input_counter.read_bytes() == input_calls
        assert final_publication_bytes(reused) == produced_bytes

        recovered_characterization = (
            case.root / "producer-publication-recovery" / "frozen-input.tsv"
        )
        inject_publication_crash(
            recovered_characterization,
            produced,
            "assets_published",
        )
        case.run(
            case.characterize_command(
                recovered_characterization,
                capture=producer_capture,
            ),
            success=True,
        )
        assert final_publication_bytes(recovered_characterization) == produced_bytes
        assert case.counter.read_bytes() == producer_calls
        assert case.input_counter.read_bytes() == input_calls

        no_replace_output = (
            case.root / "producer-no-replace-race" / "frozen-input.tsv"
        )
        no_replace_output.parent.mkdir()
        no_replace_publication = bootstrap.publication_paths(no_replace_output)
        no_replace_ownership = bootstrap.PublicationOwnership()
        no_replace_foreign = b"foreign target inserted after audit\n"
        with bootstrap.exclusive_output_lock(no_replace_output) as publication:
            def insert_characterization_foreign_target(point: str) -> None:
                if point == "staging_validated":
                    write_bytes(publication.output, no_replace_foreign, 0o444)

            try:
                bootstrap.publish_immutable_supplement(
                    publication,
                    tree_bytes(produced_assets),
                    produced.read_bytes(),
                    produced.with_name(produced.name + ".sha256").read_bytes(),
                    crash_hook=insert_characterization_foreign_target,
                    ownership=no_replace_ownership,
                    prepublish_validator=lambda staged: (
                        bootstrap.audit_characterization_publication(
                            case.base,
                            case.parent_sha,
                            staged,
                            case.fixture,
                            case.fixture_sha,
                            case.runner,
                            file_digest(case.runner),
                            case.affinity,
                            case.base_repo_root,
                        )
                    ),
                )
            except bootstrap.BootstrapError as error:
                assert "already exists" in str(error)
            else:
                raise AssertionError("characterization publication replaced a race")
            bootstrap.rollback_owned_publication(
                publication,
                no_replace_ownership,
            )
        assert no_replace_publication.output.read_bytes() == no_replace_foreign
        assert no_replace_publication.assets.is_dir()
        assert not no_replace_publication.seal.exists()

        wrong_runner = case.root / "wrong-process-metrics"
        write_bytes(
            wrong_runner,
            case.runner.read_bytes() + b"wrong frozen runner\n",
            0o555,
        )
        wrong_runner_output = case.root / "wrong-runner" / "frozen-input.tsv"
        result = case.run(
            case.characterize_command(
                wrong_runner_output,
                capture=case.root / "wrong-runner-capture",
                process_metrics=wrong_runner,
            ),
            success=False,
        )
        assert "expected-process-metrics-sha256" in result.stderr
        assert not wrong_runner_output.exists()

        foreign_characterization = (
            case.root / "foreign-characterization" / "frozen-input.tsv"
        )
        foreign_characterization.parent.mkdir()
        foreign_bytes = b"foreign characterization target\n"
        write_bytes(foreign_characterization, foreign_bytes, 0o444)
        result = case.run(
            case.characterize_command(
                foreign_characterization,
                capture=case.root / "foreign-characterization-capture",
            ),
            success=False,
        )
        assert "characterization output already exists" in result.stderr
        assert foreign_characterization.read_bytes() == foreign_bytes

        independent_supplement = (
            case.root / "produced-source-build" / "phase9-local-commit.tsv"
        )
        case.run(
            case.build_command(
                independent_supplement,
                characterization=produced,
                capture=case.root / "produced-source-build-capture",
            ),
            success=True,
        )
        assert len(case.counter.read_text().splitlines()) == 24
        assert case.input_counter.read_text().splitlines() == ["input", "input"]
        bootstrap.audited_frozen_characterization(
            case.base,
            case.parent_sha,
            independent_supplement,
            case.base_repo_root,
        )

        case.counter.unlink()
        case.input_counter.unlink()
        partial_capture = case.root / "producer-partial-capture"
        partial_output = case.root / "producer-partial" / "frozen-input.tsv"
        write_bytes(case.failure_marker, b"1 2\n")
        result = case.run(
            case.characterize_command(partial_output, capture=partial_capture),
            success=False,
        )
        assert "process-metrics" in result.stderr
        assert not partial_output.exists()
        partial_rows = partial_capture / "rows"
        first_identity = bootstrap.row_id(1, 1)
        failed_identity = bootstrap.row_id(1, 2)
        assert (partial_rows / first_identity / "status.json.sha256").is_file()
        assert not (partial_rows / f".{failed_identity}.staging").exists()
        assert case.input_counter.read_text().splitlines() == ["input"]
        assert case.counter.read_text().splitlines() == ["1 1", "1 2"]

        foreign_row = partial_rows / failed_identity
        foreign_row.mkdir()
        foreign_row_bytes = b"foreign capture row\n"
        write_bytes(foreign_row / "foreign.txt", foreign_row_bytes)
        result = case.run(
            case.characterize_command(partial_output, capture=partial_capture),
            success=False,
        )
        assert "exact file closure" in result.stderr
        assert (foreign_row / "foreign.txt").read_bytes() == foreign_row_bytes
        shutil.rmtree(foreign_row)

        case.run(
            case.characterize_command(partial_output, capture=partial_capture),
            success=True,
        )
        assert case.input_counter.read_text().splitlines() == ["input"]
        assert len(case.counter.read_text().splitlines()) == 13
        assert case.counter.read_text().splitlines().count("1 1") == 1
        assert case.counter.read_text().splitlines().count("1 2") == 2

        partial_characterization = bootstrap.read_characterization(partial_output)
        tampered_report = bootstrap.relative_evidence_path(
            partial_characterization.path,
            partial_characterization.rows[(1, 1)]["product_report_path"],
            "producer tamper report",
        )
        tampered_report.chmod(0o644)
        tampered_bytes = tampered_report.read_bytes().replace(
            b"  accepted_rebuild_ms: 120.250\n",
            b"  accepted_rebuild_ms: 121.250\n",
        )
        assert tampered_bytes != tampered_report.read_bytes()
        tampered_report.write_bytes(tampered_bytes)
        result = case.run(
            case.characterize_command(partial_output, capture=partial_capture),
            success=False,
        )
        assert "hash mismatch" in result.stderr
        assert tampered_report.read_bytes() == tampered_bytes

        case.counter.unlink()
        case.input_counter.unlink()
        output_a = case.root / "out-a" / "phase9-local-commit.tsv"
        output_b = case.root / "out-b" / "phase9-local-commit.tsv"
        source_hardlink = case.root / "source-characterization-hardlink.tsv"
        os.link(case.characterization, source_hardlink)
        result = case.run(
            case.build_command(case.root / "source-hardlink-rejected.tsv"),
            success=False,
        )
        assert "externally hard-linked" in result.stderr
        source_hardlink.unlink()

        base_hardlink = case.root / "base-manifest-hardlink.tsv"
        os.link(case.base, base_hardlink)
        result = case.run(
            case.build_command(case.root / "base-hardlink-rejected.tsv"),
            success=False,
        )
        assert "externally hard-linked" in result.stderr
        base_hardlink.unlink()

        fake_root_command = case.build_command(case.root / "fake-root-rejected.tsv")
        root_flag = fake_root_command.index("--base-repo-root")
        fake_root_command[root_flag + 1] = os.fspath(case.root / "base")
        result = case.run(fake_root_command, success=False)
        assert "repository root" in result.stderr, result.stderr

        locked_output = (
            case.root / "concurrent-publication" / "phase9-local-commit.tsv"
        )
        locked_output.parent.mkdir()
        with bootstrap.exclusive_output_lock(locked_output) as locked_publication:
            result = case.run(case.build_command(locked_output), success=False)
            assert "owned by another builder" in result.stderr
            assert not any(
                os.path.lexists(path)
                for path in (
                    locked_publication.output,
                    locked_publication.assets,
                    locked_publication.seal,
                    locked_publication.journal,
                    locked_publication.journal_staging,
                    locked_publication.staging,
                    locked_publication.validation,
                )
            )

        collision_output = (
            case.root / "capture-publication-collision" / "phase9-local-commit.tsv"
        )
        collision_output.parent.mkdir()
        collision_publication = bootstrap.publication_paths(collision_output)
        result = case.run(
            case.build_command(
                collision_output, capture=collision_publication.staging
            ),
            success=False,
        )
        assert "complete supplement publication namespace" in result.stderr
        assert not any(
            os.path.lexists(path)
            for path in (
                collision_publication.output,
                collision_publication.assets,
                collision_publication.seal,
                collision_publication.journal,
                collision_publication.journal_staging,
                collision_publication.staging,
                collision_publication.validation,
            )
        )

        with bootstrap.exclusive_capture_lock(case.root / "capture"):
            result = case.run(
                case.build_command(case.root / "concurrent-owner-rejected.tsv"),
                success=False,
            )
        assert "owned by another builder" in result.stderr
        assert not (case.root / "capture").exists()

        capture = case.root / "capture"
        capture_rows = capture / "rows"
        partial_row = capture_rows / (
            "." + bootstrap.row_id(1, 1) + ".staging"
        )
        partial_row.mkdir(parents=True)
        write_bytes(partial_row / "report.txt", b"interrupted row write\n")
        partial_input = capture / ".input.staging"
        partial_input.mkdir()
        write_bytes(partial_input / "canonical.json", b"interrupted input\n")
        write_bytes(capture / ".capture-contract.json.staging", b"{\n")
        write_bytes(
            capture / ".capture-contract.json.sha256.staging", b"partial seal\n"
        )
        case.run(case.build_command(output_a), success=True)
        assert not (capture / ".capture-contract.json.staging").exists()
        assert not (capture / ".capture-contract.json.sha256.staging").exists()
        assert not partial_row.exists()
        assert not partial_input.exists()
        assert case.input_counter.read_text().splitlines() == ["input"]
        assert case.counter.read_text().splitlines() == [
            f"{seed} {workers}"
            for seed in bootstrap.SEEDS
            for workers in bootstrap.WORKERS
        ]

        staged_identity = bootstrap.row_id(1, 1)
        staged_row = capture_rows / staged_identity
        staged_row.rename(capture_rows / f".{staged_identity}.staging")
        staged_input = capture / "input"
        staged_input.rename(capture / ".input.staging")
        writable_row = capture_rows / bootstrap.row_id(1, 2)
        writable_row.chmod(0o755)
        (writable_row / "report.txt").chmod(0o644)
        case.run(case.build_command(output_b), success=True)
        assert len(case.counter.read_text().splitlines()) == 12, "capture did not resume"
        assert case.input_counter.read_text().splitlines() == ["input"]
        assert staged_input.is_dir() and not staged_input.stat().st_mode & 0o222
        assert staged_row.is_dir() and not staged_row.stat().st_mode & 0o222
        assert not writable_row.stat().st_mode & 0o222
        assert not (writable_row / "report.txt").stat().st_mode & 0o222
        assert output_a.read_bytes() == output_b.read_bytes()
        assert output_a.with_name(output_a.name + ".sha256").read_bytes() == output_b.with_name(
            output_b.name + ".sha256"
        ).read_bytes()
        assert tree_bytes(output_a.with_name(output_a.stem + ".assets")) == tree_bytes(
            output_b.with_name(output_b.stem + ".assets")
        )

        audited = bootstrap.audited_frozen_characterization(
            case.base, case.parent_sha, output_a, case.base_repo_root
        )
        assert set(audited.evidence) == {
            (seed, workers)
            for seed in bootstrap.SEEDS
            for workers in bootstrap.WORKERS
        }
        assert {row["row_id"] for row in audited.supplement.rows} == {
            f"phase9-local-commit-seed{seed}-w{workers}"
            for seed in bootstrap.SEEDS
            for workers in bootstrap.WORKERS
        }
        assert {row["timeout_seconds"] for row in audited.supplement.rows} == {
            str(bootstrap.WORKLOAD_TIMEOUT_SECONDS)
        }
        assert audited.characterization.path.name == case.characterization.name
        assert audited.input_evidence.parsimony_min == 200
        for row in audited.supplement.rows:
            seed = int(row["seed"])
            assert bootstrap.canonical_argv(row)[-4:] == [
                "--chart-spr-canonical-result",
                "@search-canonical-result",
                "-o",
                "@output",
            ]
            assert row["expected_initial_score"] == "200"
            assert row["expected_final_score"] == str(70 + seed)
            assert row["expected_validated_parsimony"] == str(120 + seed)
            assert len(
                {
                    row["expected_initial_score"],
                    row["expected_final_score"],
                    row["expected_validated_parsimony"],
                }
            ) == 3
        assert {
            row["product_report_path"]
            for row in audited.characterization.rows.values()
        } == {
            f"evidence/seed{seed}/w{workers}/report.txt"
            for seed in bootstrap.SEEDS
            for workers in bootstrap.WORKERS
        }
        assets_a = output_a.with_name(output_a.stem + ".assets")
        source_characterization = bootstrap.read_characterization(
            assets_a / "provenance/source" / case.characterization.name
        )
        assert source_characterization.preamble["timeout_seconds"] == str(
            bootstrap.TIMEOUT_SECONDS
        )
        capture_contract = bootstrap.read_json_object(
            assets_a / "provenance/capture-contract.json", "capture contract"
        )
        assert capture_contract["timeout_seconds"] == bootstrap.TIMEOUT_SECONDS
        source_input = bootstrap.exact_input_evidence(source_characterization)
        bootstrap.require_matching_input_evidence(
            source_input, audited.input_evidence, "test input evidence"
        )
        source_evidence = [
            bootstrap.exact_evidence(source_characterization, row, source_input)
            for row in source_characterization.rows.values()
        ]
        bootstrap.validate_worker_independent_evidence(source_evidence)
        assert audited.process_metrics_sha256 == file_digest(case.runner)
        first_capture = (
            assets_a / "provenance" / bootstrap.row_id(1, 1)
        )
        for receipt_name in (
            "process-metrics.txt",
            "output-process-metrics.txt",
        ):
            receipt = bootstrap.validate_process_metrics_receipt(
                first_capture / receipt_name, receipt_name
            )
            assert receipt["rss_limit_bytes"] == str(bootstrap.RSS_LIMIT_BYTES)
            assert receipt["rss_limit_enabled"] == "1"
        input_receipt = bootstrap.validate_process_metrics_receipt(
            assets_a / "provenance/input/process-metrics.txt",
            "input process-metrics receipt",
        )
        assert input_receipt["rss_limit_bytes"] == str(bootstrap.RSS_LIMIT_BYTES)

        bad_receipt = case.root / "bad-process-metrics.txt"
        bad_receipt.write_bytes(
            (first_capture / "process-metrics.txt")
            .read_bytes()
            .replace(
                f"rss_limit_bytes={bootstrap.RSS_LIMIT_BYTES}\n".encode(),
                f"rss_limit_bytes={bootstrap.RSS_LIMIT_BYTES - 1}\n".encode(),
            )
        )
        expect_bootstrap_error(
            lambda: bootstrap.validate_process_metrics_receipt(
                bad_receipt, "tampered process-metrics receipt"
            ),
            "rss_limit_bytes",
        )

        nonfinite_json = case.root / "nonfinite.json"
        nonfinite_json.write_text('{"unvalidated":NaN}\n')
        expect_bootstrap_error(
            lambda: bootstrap.read_json_object(nonfinite_json, "nonfinite probe"),
            "forbidden nonfinite",
        )
        nonfinite_sidecar = case.root / "nonfinite.ndjson"
        nonfinite_sidecar.write_bytes(
            sidecar_bytes(1).replace(b'"record":"contract"', b'"probe":Infinity,"record":"contract"', 1)
        )
        expect_bootstrap_error(
            lambda: bootstrap.sidecar_contract(nonfinite_sidecar),
            "forbidden nonfinite",
        )
        audit_command = [
            sys.executable,
            os.fspath(HELPER),
            "audit",
            "--base-repo-root",
            os.fspath(case.base_repo_root),
            "--base-manifest",
            os.fspath(case.base),
            "--expected-parent-sha256",
            case.parent_sha,
            "--supplement",
            os.fspath(output_a),
            "--benchmark-harness",
            os.fspath(HARNESS),
            "--process-metrics",
            os.fspath(case.runner),
        ]
        case.run(audit_command, success=True)

        separate_base_root = case.root / "separate-sealed-base-worktree"
        subprocess.run(["git", "init", "-q", separate_base_root], check=True)
        subprocess.run(
            [
                "git",
                "-C",
                os.fspath(separate_base_root),
                "fetch",
                "-q",
                os.fspath(case.base_repo_root),
                case.base_revision,
            ],
            check=True,
        )
        subprocess.run(
            [
                "git",
                "-C",
                os.fspath(separate_base_root),
                "update-ref",
                "HEAD",
                "FETCH_HEAD",
            ],
            check=True,
        )
        for source in (case.larch2, case.oracle):
            relative = source.relative_to(case.base_repo_root)
            destination = separate_base_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            destination.chmod(source.stat().st_mode & 0o777)
        split_audit = list(audit_command)
        root_flag = split_audit.index("--base-repo-root")
        split_audit[root_flag + 1] = os.fspath(separate_base_root)
        case.run(split_audit, success=True)
        separate_alias = case.root / "separate-sealed-base-alias"
        separate_alias.symlink_to(separate_base_root, target_is_directory=True)
        alias_audit = list(split_audit)
        alias_audit[root_flag + 1] = os.fspath(separate_alias)
        result = case.run(alias_audit, success=False)
        assert "must be a lexical directory" in result.stderr, result.stderr

        immutable_output = final_publication_bytes(output_a)
        result = case.run(case.build_command(output_a), success=False)
        assert "exclusive Phase-9 output already exists" in result.stderr
        assert final_publication_bytes(output_a) == immutable_output
        case.run(audit_command, success=True)

        foreign_output = (
            case.root / "foreign-target-race" / "phase9-local-commit.tsv"
        )
        foreign_output.parent.mkdir()
        foreign_bytes = b"foreign immutable publication\n"
        source_assets = output_a.with_name(output_a.stem + ".assets")
        source_seal = output_a.with_name(output_a.name + ".sha256")
        ownership = bootstrap.PublicationOwnership()
        with bootstrap.exclusive_output_lock(foreign_output) as publication:
            def insert_foreign_target(point: str) -> None:
                if point == "staging_validated":
                    write_bytes(publication.output, foreign_bytes, 0o444)

            try:
                bootstrap.publish_immutable_supplement(
                    publication,
                    tree_bytes(source_assets),
                    output_a.read_bytes(),
                    source_seal.read_bytes(),
                    crash_hook=insert_foreign_target,
                    ownership=ownership,
                    prepublish_validator=lambda _path: None,
                )
            except bootstrap.BootstrapError as error:
                assert "target already exists" in str(error)
            else:
                raise AssertionError("foreign publication target was replaced")
            bootstrap.rollback_owned_publication(publication, ownership)
        assert foreign_output.read_bytes() == foreign_bytes
        assert tree_bytes(
            foreign_output.with_name(foreign_output.stem + ".assets")
        ) == tree_bytes(source_assets)
        assert not foreign_output.with_name(foreign_output.name + ".sha256").exists()
        result = case.run(case.build_command(foreign_output), success=False)
        assert "duplicate staged/final components" in result.stderr
        assert foreign_output.read_bytes() == foreign_bytes

        checkpoint_output = (
            case.root / "post-rename-checkpoint" / "phase9-local-commit.tsv"
        )
        checkpoint_output.parent.mkdir()
        checkpoint_ownership = bootstrap.PublicationOwnership()
        with bootstrap.exclusive_output_lock(checkpoint_output) as publication:
            def interrupt_before_record(point: str) -> None:
                if point == "assets_renamed":
                    raise bootstrap.BootstrapError("injected post-rename interrupt")

            try:
                bootstrap.publish_immutable_supplement(
                    publication,
                    tree_bytes(source_assets),
                    output_a.read_bytes(),
                    source_seal.read_bytes(),
                    crash_hook=interrupt_before_record,
                    ownership=checkpoint_ownership,
                    prepublish_validator=lambda _path: None,
                )
            except bootstrap.BootstrapError as error:
                assert "injected post-rename interrupt" in str(error)
            else:
                raise AssertionError("post-rename interrupt was not injected")
            assert "assets" not in checkpoint_ownership.components
            bootstrap.rollback_owned_publication(
                publication, checkpoint_ownership
            )
            assert os.path.lexists(publication.journal)
            assert os.path.lexists(publication.staging)
            assert os.path.lexists(publication.assets)
        case.run(case.build_command(checkpoint_output), success=True)
        assert final_publication_bytes(checkpoint_output) == immutable_output
        checkpoint_publication = bootstrap.publication_paths(checkpoint_output)
        assert not os.path.lexists(checkpoint_publication.journal)
        assert not os.path.lexists(checkpoint_publication.staging)

        journal_payload_assets = {"owned.txt": b"journal payload\n"}
        journal_payload_manifest = b"foreign partial manifest\n"
        journal_payload_seal = bootstrap.seal_bytes(
            "phase9-local-commit.tsv", journal_payload_manifest
        )
        for journal_kind in ("symlink", "hardlink"):
            journal_output = (
                case.root
                / f"{journal_kind}-publication-journal"
                / "phase9-local-commit.tsv"
            )
            journal_output.parent.mkdir()
            publication = bootstrap.publication_paths(journal_output)
            write_bytes(journal_output, journal_payload_manifest, 0o444)
            backing = journal_output.parent / "foreign-journal.json"
            journal_data = bootstrap.publication_journal_bytes(
                publication,
                journal_payload_assets,
                journal_payload_manifest,
                journal_payload_seal,
            )
            write_bytes(backing, journal_data, 0o444)
            if journal_kind == "symlink":
                publication.journal.symlink_to(backing.name)
            else:
                os.link(backing, publication.journal)
            try:
                bootstrap.recover_interrupted_publication(
                    publication, prepublish_validator=lambda _path: None
                )
            except bootstrap.BootstrapError as error:
                expected = "regular file" if journal_kind == "symlink" else "hard-linked"
                assert expected in str(error)
            else:
                raise AssertionError(f"{journal_kind} journal was trusted")
            assert journal_output.read_bytes() == journal_payload_manifest
            assert backing.read_bytes() == journal_data
            assert os.path.lexists(publication.journal)

        forged_output = (
            case.root / "forged-publication-journal" / "phase9-local-commit.tsv"
        )
        forged_output.parent.mkdir()
        forged_publication = bootstrap.publication_paths(forged_output)
        forged_manifest = b"hash-consistent but unauditable manifest\n"
        forged_seal = bootstrap.seal_bytes(forged_output.name, forged_manifest)
        forged_assets = {"owned.txt": b"foreign asset\n"}
        forged_publication.assets.mkdir()
        write_bytes(
            forged_publication.assets / "owned.txt", forged_assets["owned.txt"], 0o444
        )
        write_bytes(forged_output, forged_manifest, 0o444)
        write_bytes(forged_publication.seal, forged_seal, 0o444)
        forged_journal = bootstrap.publication_journal_bytes(
            forged_publication, forged_assets, forged_manifest, forged_seal
        )
        write_bytes(forged_publication.journal, forged_journal, 0o444)
        forged_before = final_publication_bytes(forged_output)
        result = case.run(case.build_command(forged_output), success=False)
        assert "TSV has no complete preamble/header/rows" in result.stderr
        assert final_publication_bytes(forged_output) == forged_before
        assert forged_publication.journal.read_bytes() == forged_journal

        forged_partial_output = (
            case.root
            / "forged-partial-publication"
            / "phase9-local-commit.tsv"
        )
        forged_partial_output.parent.mkdir()
        forged_partial = bootstrap.publication_paths(forged_partial_output)
        shutil.copytree(source_assets, forged_partial.assets)
        forged_partial.staging.mkdir()
        forged_partial_paths = bootstrap.staged_publication_paths(forged_partial)
        forged_partial_manifest = b"invalid staged manifest\n"
        forged_partial_seal = bootstrap.seal_bytes(
            forged_partial_output.name, forged_partial_manifest
        )
        write_bytes(
            forged_partial_paths["manifest"], forged_partial_manifest, 0o444
        )
        write_bytes(forged_partial_paths["seal"], forged_partial_seal, 0o444)
        forged_partial_assets = tree_bytes(forged_partial.assets)
        forged_partial_journal = bootstrap.publication_journal_bytes(
            forged_partial,
            forged_partial_assets,
            forged_partial_manifest,
            forged_partial_seal,
        )
        write_bytes(forged_partial.journal, forged_partial_journal, 0o444)
        result = case.run(case.build_command(forged_partial_output), success=False)
        assert "TSV has no complete preamble/header/rows" in result.stderr
        assert not os.path.lexists(forged_partial.output)
        assert not os.path.lexists(forged_partial.seal)
        assert tree_bytes(forged_partial.assets) == forged_partial_assets
        assert forged_partial_paths["manifest"].read_bytes() == forged_partial_manifest
        assert forged_partial_paths["seal"].read_bytes() == forged_partial_seal
        assert forged_partial.journal.read_bytes() == forged_partial_journal
        assert not os.path.lexists(forged_partial.validation)


        for crash_point in (
            "journal_renamed",
            "journal_published",
            "staging_durable",
            "staging_validated",
            "assets_renamed",
            "assets_published",
            "manifest_published",
            "seal_published",
        ):
            crash_output = (
                case.root
                / f"publication-crash-{crash_point}"
                / output_a.name
            )
            publication = inject_publication_crash(
                crash_output, output_a, crash_point
            )
            if crash_point == "seal_published":
                before_restart = final_publication_bytes(crash_output)
                case.run(case.build_command(crash_output), success=True)
                assert final_publication_bytes(crash_output) == before_restart
            else:
                case.run(case.build_command(crash_output), success=True)
            assert final_publication_bytes(crash_output) == immutable_output
            assert not os.path.lexists(publication.journal)
            assert not os.path.lexists(publication.journal_staging)
            assert not os.path.lexists(publication.staging)
            assert {path.name for path in crash_output.parent.iterdir()} == {
                publication.output.name,
                publication.assets.name,
                publication.seal.name,
                publication.lock.name,
            }
            crash_audit = list(audit_command)
            crash_audit[crash_audit.index(os.fspath(output_a))] = os.fspath(
                crash_output
            )
            case.run(crash_audit, success=True)

        relocated_runner = case.root / "explicit-process-metrics"
        shutil.copy2(case.runner, relocated_runner)
        relocated_audit = list(audit_command)
        relocated_audit[relocated_audit.index(os.fspath(case.runner))] = os.fspath(
            relocated_runner
        )
        case.run(relocated_audit, success=True)
        fake_harness = case.root / "fake-benchmark-harness.sh"
        write_bytes(
            fake_harness,
            (
                "#!/usr/bin/env bash\n"
                f"echo 'error: manifest group has no rows: "
                f"{bootstrap.HARNESS_SENTINEL_GROUP}' >&2\n"
                "exit 1\n"
            ).encode(),
            0o555,
        )
        impersonated = list(audit_command)
        impersonated[impersonated.index(os.fspath(HARNESS))] = os.fspath(fake_harness)
        result = case.run(impersonated, success=False)
        assert "not the repository production harness" in result.stderr
        commands = output_a.with_name(output_a.stem + ".assets") / "commands.sh"
        case.run(["bash", "-n", os.fspath(commands)], success=True)
        case.run([os.fspath(commands), "--verify-only"], success=True)

        command_tampered = clone_supplement(
            output_a, case.root / "command-tampered"
        )
        command_asset = command_tampered.with_name(
            command_tampered.stem + ".assets"
        ) / "commands.sh"
        affinity_token = shlex.quote(case.affinity).encode("utf-8")
        tampered_commands = command_asset.read_bytes().replace(
            b"  " + affinity_token + b" \\\n", b"  0 \\\n", 1
        )
        assert tampered_commands != command_asset.read_bytes()
        rebind_supplement_commands(command_tampered, tampered_commands)
        command_audit = list(audit_command)
        command_audit[command_audit.index(os.fspath(output_a))] = os.fspath(
            command_tampered
        )
        result = case.run(command_audit, success=False)
        assert "exact rendered byte stream" in result.stderr

        source_tampered = clone_supplement(
            output_a, case.root / "source-stable-tampered"
        )
        source_assets = source_tampered.with_name(
            source_tampered.stem + ".assets"
        )
        source_path = (
            source_assets / "provenance/source" / case.characterization.name
        )
        source = bootstrap.read_characterization(source_path)
        changed_rows: list[dict[str, str]] = []
        for key, source_row_value in source.rows.items():
            source_row = dict(source_row_value)
            if key[0] == 1:
                source_report = bootstrap.relative_evidence_path(
                    source_path,
                    source_row["product_report_path"],
                    "tampered source report",
                )
                original_report = source_report.read_bytes()
                changed_report = original_report.replace(
                    b"  accepted_exact_trim_reuse_rejections: 0\n",
                    b"  accepted_exact_trim_reuse_rejections: 1\n",
                    1,
                )
                assert changed_report != original_report
                replace_file(source_report, changed_report, 0o444)
                source_row["product_report_sha256"] = file_digest(source_report)
            changed_rows.append(source_row)
        replace_sealed_file(
            source_path,
            bootstrap.characterization_tsv_bytes(source.preamble, changed_rows),
        )
        contract_path = source_assets / "provenance/capture-contract.json"
        contract = bootstrap.read_json_object(contract_path, "tampered contract")
        contract["source_characterization_sha256"] = file_digest(source_path)
        contract["source_characterization_seal_sha256"] = file_digest(
            source_path.with_name(source_path.name + ".sha256")
        )
        replace_sealed_file(
            contract_path,
            (json.dumps(contract, indent=2, sort_keys=True) + "\n").encode(),
        )
        rebuild_asset_ledger_and_commands(source_tampered)
        source_audit = list(audit_command)
        source_audit[source_audit.index(os.fspath(output_a))] = os.fspath(
            source_tampered
        )
        result = case.run(source_audit, success=False)
        assert "differs from the sealed source characterization" in result.stderr

        input_tampered = clone_supplement(
            output_a, case.root / "source-input-tampered"
        )
        input_assets = input_tampered.with_name(input_tampered.stem + ".assets")
        input_source_path = (
            input_assets / "provenance/source" / case.characterization.name
        )
        input_source = bootstrap.read_characterization(input_source_path)
        input_canonical_path = bootstrap.relative_evidence_path(
            input_source_path,
            input_source.preamble["input_canonical_path"],
            "tampered source input canonical",
        )
        input_value = json.loads(input_canonical_path.read_text())
        input_value["parsimony_min"] = 201
        replace_file(input_canonical_path, json_bytes(input_value), 0o444)
        input_preamble = dict(input_source.preamble)
        input_preamble["input_canonical_sha256"] = file_digest(
            input_canonical_path
        )
        replace_sealed_file(
            input_source_path,
            bootstrap.characterization_tsv_bytes(
                input_preamble, list(input_source.rows.values())
            ),
        )
        changed_input = bootstrap.exact_input_evidence(
            bootstrap.read_characterization(input_source_path)
        )
        input_contract_path = input_assets / "provenance/capture-contract.json"
        input_contract = bootstrap.read_json_object(
            input_contract_path, "input-tampered contract"
        )
        input_contract["source_characterization_sha256"] = file_digest(
            input_source_path
        )
        input_contract["source_characterization_seal_sha256"] = file_digest(
            input_source_path.with_name(input_source_path.name + ".sha256")
        )
        input_contract["source_input_canonical_sha256"] = (
            changed_input.canonical_sha256
        )
        input_contract["source_input_semantic_sha256"] = (
            changed_input.semantic_sha256
        )
        input_contract["source_input_parsimony_min"] = changed_input.parsimony_min
        replace_sealed_file(
            input_contract_path,
            (json.dumps(input_contract, indent=2, sort_keys=True) + "\n").encode(),
        )
        rebuild_asset_ledger_and_commands(input_tampered)
        input_audit = list(audit_command)
        input_audit[input_audit.index(os.fspath(output_a))] = os.fspath(
            input_tampered
        )
        result = case.run(input_audit, success=False)
        assert "sealed source input canonical evidence" in result.stderr

        current_run = case.root / "same-revision-run"
        case.write_current_run(current_run, output_a)
        frozen_report_arguments = [
            argument
            for seed in bootstrap.SEEDS
            for argument in (
                "--frozen-oracle-report",
                f"{seed}={audited.evidence[(seed, 1)].product_report}",
            )
        ]
        evaluation = case.run(
            [
                sys.executable,
                os.fspath(ACCEPTANCE),
                "evaluate",
                "--benchmark-dir",
                os.fspath(current_run),
                "--defer-frozen-oracle-characterization",
                "synthetic bootstrap integration is intentionally non-final",
                *frozen_report_arguments,
            ],
            success=True,
        )
        deferred = json.loads(evaluation.stdout)
        assert deferred["status"] == "deferred_non_final"
        assert deferred["matrix"] == {
            "seeds": [1, 7, 19],
            "workers": [1, 8],
            "repetitions": 3,
            "row_id_template": "phase9-local-commit-seed{seed}-w{worker}",
            "fixture_by_seed": {
                "1": "phase9-three-accepts-seed1",
                "7": "phase9-three-accepts-seed7",
                "19": "phase9-three-accepts-seed19",
            },
        }
        assert (
            deferred["frozen_oracle_characterization"]["status"]
            == "deferred_non_final"
        )
        assert set(
            deferred["frozen_oracle_characterization"][
                "ad_hoc_reports_validated"
            ]
        ) == {"1", "7", "19"}

        wrong_parent = digest_text("wrong parent")
        failed = case.root / "wrong-parent.tsv"
        case.run(
            case.build_command(failed, expected_parent=wrong_parent), success=False
        )
        assert not failed.exists() and not failed.with_name(failed.stem + ".assets").exists()

        parent_bad = case.clone_characterization("parent-bad")
        case.edit_tsv(
            parent_bad,
            lambda lines, _header, _rows: lines.__setitem__(2, "# parent_sha256=" + "f" * 64),
        )
        case.run(
            case.build_command(
                case.root / "parent-bad.tsv",
                characterization=parent_bad,
                capture=case.root / "parent-bad-capture",
            ),
            success=False,
        )

        external_nonimproving = case.clone_characterization(
            "external-nonimproving"
        )
        external_characterization = bootstrap.read_characterization(
            external_nonimproving
        )
        external_input_path = bootstrap.relative_evidence_path(
            external_nonimproving,
            external_characterization.preamble["input_canonical_path"],
            "nonimproving external input",
        )
        external_input_value = json.loads(external_input_path.read_text())
        external_input_value["parsimony_min"] = 100
        external_input_path.write_bytes(json_bytes(external_input_value))
        external_preamble = dict(external_characterization.preamble)
        external_preamble["input_canonical_sha256"] = file_digest(
            external_input_path
        )
        replace_sealed_file(
            external_nonimproving,
            bootstrap.characterization_tsv_bytes(
                external_preamble,
                list(external_characterization.rows.values()),
            ),
        )
        result = case.run(
            case.build_command(
                case.root / "external-nonimproving.tsv",
                characterization=external_nonimproving,
                capture=case.root / "external-nonimproving-capture",
            ),
            success=False,
        )
        assert "external canonical output did not strictly improve" in result.stderr

        report_nonimproving = case.clone_characterization("report-nonimproving")
        report_path = report_nonimproving.parent / "evidence/seed1/w1/report.txt"
        report_data = report_path.read_text()
        for old, new in (
            ("      state_score_after: 91", "      state_score_after: 101"),
            ("      accepted_exact_new_score: 91", "      accepted_exact_new_score: 101"),
            ("      accepted_exact_delta: -10", "      accepted_exact_delta: 0"),
            (
                "      accepted_lower_bound_new_score: 91",
                "      accepted_lower_bound_new_score: 101",
            ),
            ("      accepted_lower_bound_delta: -10", "      accepted_lower_bound_delta: 0"),
            (
                "      post_materialization_rebuilt_score: 91",
                "      post_materialization_rebuilt_score: 101",
            ),
        ):
            report_data = report_data.replace(old, new, 1)
        report_path.write_text(report_data)
        def repair_nonimproving_report(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[0]["product_report_sha256"] = file_digest(report_path)
        case.edit_tsv(report_nonimproving, repair_nonimproving_report)
        result = case.run(
            case.build_command(
                case.root / "report-nonimproving.tsv",
                characterization=report_nonimproving,
                capture=case.root / "report-nonimproving-capture",
            ),
            success=False,
        )
        assert "strictly improve" in result.stderr

        score_ua = case.clone_characterization("score-ua-edge")
        score_ua_root = score_ua.parent / "evidence/seed1/w1"
        score_ua_sidecar = score_ua_root / "canonical.ndjson"
        changed_sidecar = score_ua_sidecar.read_bytes().replace(
            b'"score_ua_edge":false', b'"score_ua_edge":true', 1
        )
        assert changed_sidecar != score_ua_sidecar.read_bytes()
        score_ua_sidecar.write_bytes(changed_sidecar)
        score_ua_search = search_digest(
            1, digest_bytes(changed_sidecar), len(changed_sidecar.splitlines())
        )
        (score_ua_root / "canonical.json").write_bytes(
            json_bytes(score_ua_search)
        )
        def repair_score_ua(_lines, _header, rows):  # type: ignore[no-untyped-def]
            row = rows[0]
            row["canonical_sidecar_sha256"] = file_digest(score_ua_sidecar)
            row["canonical_result_sha256"] = file_digest(
                score_ua_root / "canonical.json"
            )
            row["oracle_search_semantic_sha256"] = file_digest(score_ua_sidecar)
            row["oracle_trial_semantic_sha256"] = bootstrap.trial_digest(
                bootstrap.METHOD,
                row["oracle_search_semantic_sha256"],
                row["oracle_output_semantic_sha256"],
                row["canonical_argv_sha256"],
            )
        case.edit_tsv(score_ua, repair_score_ua)
        result = case.run(
            case.build_command(
                case.root / "score-ua-edge.tsv",
                characterization=score_ua,
                capture=case.root / "score-ua-edge-capture",
            ),
            success=False,
        )
        assert "score_ua_edge" in result.stderr

        nonfinite = case.clone_characterization("nonfinite")
        report = nonfinite.parent / "evidence/seed1/w1/report.txt"
        report.write_text(
            report.read_text().replace("  accepted_rebuild_ms: 120.250", "  accepted_rebuild_ms: nan")
        )
        def repair_nonfinite(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[0]["product_report_sha256"] = file_digest(report)
        case.edit_tsv(nonfinite, repair_nonfinite)
        case.run(
            case.build_command(
                case.root / "nonfinite.tsv",
                characterization=nonfinite,
                capture=case.root / "nonfinite-capture",
            ),
            success=False,
        )

        bad_count = case.clone_characterization("bad-count")
        count_report = bad_count.parent / "evidence/seed1/w1/report.txt"
        count_report.write_text(
            count_report.read_text().replace(
                "  candidates_generated: 96", "  candidates_generated: 95", 1
            )
        )
        def repair_count(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[0]["product_report_sha256"] = file_digest(count_report)
        case.edit_tsv(bad_count, repair_count)
        case.run(
            case.build_command(
                case.root / "bad-count.tsv",
                characterization=bad_count,
                capture=case.root / "bad-count-capture",
            ),
            success=False,
        )

        missing_binding = case.clone_characterization("missing-binding")
        binding_report = missing_binding.parent / "evidence/seed1/w1/report.txt"
        binding_report.write_text(
            "\n".join(
                line
                for line in binding_report.read_text().splitlines()
                if not line.startswith("  sampled_tree_count:")
            )
            + "\n"
        )
        def repair_binding(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[0]["product_report_sha256"] = file_digest(binding_report)
        case.edit_tsv(missing_binding, repair_binding)
        result = case.run(
            case.build_command(
                case.root / "missing-binding.tsv",
                characterization=missing_binding,
                capture=case.root / "missing-binding-capture",
            ),
            success=False,
        )
        assert result.stderr.startswith("error:") and "Traceback" not in result.stderr

        duplicate = case.clone_characterization("duplicate")
        def duplicate_path(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[1]["product_report_path"] = rows[0]["product_report_path"]
            rows[1]["product_report_sha256"] = rows[0]["product_report_sha256"]
        case.edit_tsv(duplicate, duplicate_path)
        case.run(
            case.build_command(
                case.root / "duplicate.tsv",
                characterization=duplicate,
                capture=case.root / "duplicate-capture",
            ),
            success=False,
        )

        hardlink = case.clone_characterization("hardlink-evidence")
        hardlink_root = hardlink.parent / "evidence"
        hardlink_report = hardlink_root / "seed1/w2/report.txt"
        hardlink_report.unlink()
        os.link(hardlink_root / "seed1/w1/report.txt", hardlink_report)
        def repair_hardlink(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[1]["product_report_sha256"] = file_digest(hardlink_report)
        case.edit_tsv(hardlink, repair_hardlink)
        result = case.run(
            case.build_command(
                case.root / "hardlink-evidence.tsv",
                characterization=hardlink,
                capture=case.root / "hardlink-evidence-capture",
            ),
            success=False,
        )
        assert "hard-linked" in result.stderr

        symlink_characterization = case.clone_characterization("symlink-evidence")
        linked_report = symlink_characterization.parent / "evidence/seed1/w1/report.txt"
        linked_report.unlink()
        linked_report.symlink_to("../w2/report.txt")
        def repair_link(_lines, _header, rows):  # type: ignore[no-untyped-def]
            rows[0]["product_report_sha256"] = file_digest(linked_report)
        case.edit_tsv(symlink_characterization, repair_link)
        case.run(
            case.build_command(
                case.root / "symlink-evidence.tsv",
                characterization=symlink_characterization,
                capture=case.root / "symlink-evidence-capture",
            ),
            success=False,
        )

        fixture_link = case.root / "fixture-link.pb.gz"
        fixture_link.symlink_to(case.fixture)
        case.run(
            case.build_command(
                case.root / "symlink-fixture.tsv",
                fixture=fixture_link,
                capture=case.root / "symlink-fixture-capture",
            ),
            success=False,
        )

        fabricated = case.clone_characterization("fabricated")
        fabricated_evidence = fabricated.parent / "evidence"
        def fabricate(_lines, _header, rows):  # type: ignore[no-untyped-def]
            for row in rows:
                if row["seed"] != "7":
                    continue
                workers = int(row["workers"])
                directory = fabricated_evidence / "seed7" / f"w{workers}"
                sidecar = sidecar_bytes(7, variant="fabricated-input")
                (directory / "canonical.ndjson").write_bytes(sidecar)
                search = search_digest(
                    7, digest_bytes(sidecar), len(sidecar.splitlines())
                )
                (directory / "canonical.json").write_bytes(json_bytes(search))
                row["canonical_sidecar_sha256"] = digest_bytes(sidecar)
                row["canonical_result_sha256"] = file_digest(directory / "canonical.json")
                row["oracle_search_semantic_sha256"] = digest_bytes(sidecar)
                row["oracle_trial_semantic_sha256"] = bootstrap.trial_digest(
                    bootstrap.METHOD,
                    row["oracle_search_semantic_sha256"],
                    row["oracle_output_semantic_sha256"],
                    row["canonical_argv_sha256"],
                )
        case.edit_tsv(fabricated, fabricate)
        fabricated_output = case.root / "fabricated.tsv"
        result = case.run(
            case.build_command(
                fabricated_output,
                characterization=fabricated,
                capture=case.root / "fabricated-capture",
            ),
            success=False,
        )
        assert "fresh frozen-oracle" in result.stderr
        assert not fabricated_output.exists()

        reformatted = case.evidence_directory(1, 1)
        for filename in ("canonical.json", "output-canonical.json"):
            path = reformatted / filename
            path.write_text(json.dumps(json.loads(path.read_text()), indent=2) + "\n")
        reformat_output = case.root / "reformatted-oracle-output.tsv"
        result = case.run(
            case.build_command(
                reformat_output,
                capture=case.root / "reformatted-oracle-capture",
            ),
            success=False,
        )
        assert "fresh frozen-oracle" in result.stderr
        assert not reformat_output.exists()

        assets_b = output_b.with_name(output_b.stem + ".assets")
        supplement_index = audit_command.index(os.fspath(output_a))
        audit_b = [
            *audit_command[:supplement_index],
            os.fspath(output_b),
            *audit_command[supplement_index + 1 :],
        ]
        extra_asset = assets_b / "unledgered.txt"
        extra_asset.write_text("not in the exact closure\n")
        result = case.run(audit_b, success=False)
        assert "exact sealed closure" in result.stderr
        extra_asset.unlink()

        external_fixture_link = case.root / "external-fixture-hardlink.pb.gz"
        os.link(assets_b / "fixture.pb.gz", external_fixture_link)
        result = case.run(audit_b, success=False)
        assert "externally hard-linked" in result.stderr
        external_fixture_link.unlink()

        archived_report = assets_b / "archive/evidence/seed1/w1/report.txt"
        archived_report.unlink()
        archived_report.symlink_to(
            Path("../../../provenance/phase9-local-commit-seed1-w1/report.txt")
        )
        case.run(
            audit_b,
            success=False,
        )

    print("wric_phase9_manifest_bootstrap_test: PASS")


if __name__ == "__main__":
    main()
