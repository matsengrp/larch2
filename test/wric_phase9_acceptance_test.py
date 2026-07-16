#!/usr/bin/env python3
"""Deterministic fixtures for the strict Phase-9 acceptance postprocessor."""

from __future__ import annotations

import csv
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "wric_phase9_acceptance.py"
sys.path.insert(0, str(TOOL.parent))
import wric_phase9_acceptance as acceptance  # noqa: E402


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


class Dataset:
    def __init__(self, root: Path, repetitions: int = 3) -> None:
        self.root = root
        self.repetitions = repetitions
        self.raw = root / "raw_trials.tsv"
        self.rows: list[dict[str, str]] = []
        self.reports: dict[tuple[int, int, int], dict[str, str]] = {}
        self.iterations: dict[tuple[int, int, int], list[dict[str, str]]] = {}
        self.search: dict[tuple[int, int], dict[str, object]] = {}
        self.sidecars: dict[tuple[int, int], bytes] = {}
        self.output: dict[tuple[int, int, int], dict[str, object]] = {}
        self.frozen: dict[int, tuple[dict[str, str], list[dict[str, str]]]] = {}
        self.extra_report_lines: dict[tuple[int, int, int], list[str]] = {}
        self.columns = list(acceptance.REQUIRED_COLUMNS)
        for seed in acceptance.SEEDS:
            for worker in acceptance.WORKERS:
                self._add_search_digest(seed, worker)
                for trial in range(1, repetitions + 1):
                    self._add(seed, worker, trial)
            frozen_top, frozen_iterations = self._report(seed, 1, "120", "1000")
            self.frozen[seed] = (frozen_top, frozen_iterations)

    @staticmethod
    def fixture(seed: int) -> str:
        return f"local-commit-three-s{seed}"

    @staticmethod
    def row_id(seed: int, worker: int) -> str:
        return acceptance.DEFAULT_ROW_ID_TEMPLATE.format(seed=seed, worker=worker)

    @staticmethod
    def accepted_ms(worker: int) -> str:
        return {1: "180", 2: "140", 4: "110", 8: "100"}[worker]

    @staticmethod
    def scheduler(worker: int) -> dict[str, str]:
        parallel = worker != 1
        ranges = 2 if not parallel else 2 * worker
        tasks = 0 if not parallel else ranges
        return {
            "chart_workers_requested": str(worker),
            "chart_workers_resolved": str(worker),
            "chart_worker_policy": "explicit",
            "chart_worker_resolution_policy": "explicit",
            "chart_workers_actually_active_high_water": str(worker),
            "chart_scheduler_operations": "2",
            "chart_scheduler_parallel_operations": "2" if parallel else "0",
            "chart_scheduler_serial_fallbacks": "0" if parallel else "2",
            "chart_scheduler_ranges_created": str(ranges),
            "chart_scheduler_ranges_completed": str(ranges),
            "chart_scheduler_ranges_cancelled": "0",
            "chart_scheduler_tasks_submitted": str(tasks),
            "chart_scheduler_tasks_completed": str(tasks),
            "chart_scheduler_tasks_joined": str(tasks),
            "chart_scheduler_pending_tasks": "0",
            "chart_scheduler_pending_tasks_at_shutdown": "0",
            "chart_scheduler_nested_serial_fallbacks": "0",
            "chart_scheduler_rejected_concurrent_operations": "0",
            "chart_scheduler_pool_lifetimes": "1" if parallel else "0",
            "chart_scheduler_pool_lifetimes_stopped": "1" if parallel else "0",
            "chart_scheduler_live_pool_threads": "0",
            "chart_scheduler_shutdown": "true",
            "chart_axis_inside_cache_operations": "1",
            "chart_axis_inside_cache_parallel_operations": "1" if parallel else "0",
            "chart_axis_inside_cache_active_worker_high_water": str(worker),
            "chart_axis_outside_cache_operations": "1",
            "chart_axis_outside_cache_parallel_operations": "1" if parallel else "0",
            "chart_axis_outside_cache_active_worker_high_water": str(worker),
        }

    @staticmethod
    def _iteration(index: int) -> dict[str, str]:
        before = 100 - index * 10
        after = before - 10
        return {
            "iteration": str(index),
            "candidates_generated": "32",
            "candidates_scored": "32",
            "candidate_score_failures": "0",
            "local_improving_candidates": "4",
            "locally_ranked_candidates_retained": "4",
            "candidates_exact_verified": "4",
            "accepted_move_present": "true",
            "accepted_move_committed": "true",
            "post_materialization_rejected": "false",
            "state_score_before": str(before),
            "state_score_after": str(after),
            "accepted_exact_kind": "grammar_exact",
            "accepted_exact_new_score": str(after),
            "reused_patterns_after_accept": "true",
            "post_materialization_rebuilt_score": str(after),
        }

    def _report(
        self, seed: int, worker: int, accepted_ms: str, total_ms: str
    ) -> tuple[dict[str, str], list[dict[str, str]]]:
        top = {
            "search_mode": "phase5_accept_reject_overlay_chain_local_commit",
            "accepted_state_update_mode": "overlay_chain_local_cache_commit",
            "accepted_state_materialization": "none_per_accept_overlay_chain_local_cache_update",
            "final_compaction_mode": "grammar_level_exact_multi_tree_compaction",
            "preserves_full_accepted_overlay_dag": "true",
            "acceptance": "exact_multisite",
            "candidate_selection": "lower_bound_top_k",
            "candidate_source": "grammar",
            "candidate_cap_semantics": "post-dedup",
            "randomize_order": "false",
            "reservoir_sample": "false",
            "include_immediate_reversals": "false",
            "top_k_exact_verify": "4",
            "configured_max_candidates": "32",
            "seed": str(seed),
            "objective": "grammar_exact",
            "commit_mode": "overlay_delta",
            "verification_mode": "transient",
            "local_accept_updates": "true",
            "chain_per_accept_exactness_label": "exact_multisite",
            "dominance_mode": "off",
            "bound_pruning": "true",
            "require_exact_keep_mask": "true",
            "lazy_policy": "off",
            "validate": "true",
            "force_no_vcf": "true",
            "active_patterns": "64",
            "requested_max_iterations": "3",
            "iterations": "3",
            "accepted_moves": "3",
            "local_commit_accepted_moves": "3",
            "local_commit_tombstone_scope_skips": "0",
            "output_dag_mutated": "true",
            "candidate_accepts_attempted": "3",
            "post_materialization_rejections": "0",
            "initial_score": "100",
            "final_score": "70",
            "candidates_generated": "96",
            "candidates_scored": "96",
            "exact_verifications": "12",
            "accepted_exact_trims_reused": "3",
            "accepted_exact_trim_reuse_rejections": "0",
            "initial_search_state_rebuilds": "1",
            "sidecar_rebuilds_after_accept": "0",
            "full_search_state_rebuilds": "0",
            "final_compaction_rebuilds": "1",
            "final_compaction_exactness_kind": "exact_optimal_production_union",
            "overlay_materializations_for_exact_verification": "3",
            "overlay_materializations_for_accept_materialization": "0",
            "overlay_materializations_for_final_compaction": "1",
            "inside_rows_recomputed_on_commit": "192",
            "outside_rows_recomputed_on_commit": "96",
            "local_commit_tip_grammar_refreshes": "3",
            "transient_chain_extensions_for_verification": "9",
            "transient_chain_diagnostic_cache_extensions": "0",
            "transient_chain_extension_fallbacks": "0",
            "transient_chain_extension_oracle_mismatches": "0",
            "local_leaf_state_owned_copies": "0",
            "pattern_batch_cache_builds": "0",
            "local_commit_inside_row_view_pattern_visits": "192",
            "accepted_rebuild_ms": accepted_ms,
            "total_ms": total_ms,
            **self.scheduler(worker),
        }
        return top, [self._iteration(index) for index in range(3)]

    def _add_search_digest(self, seed: int, worker: int) -> None:
        sidecar = f"canonical semantic payload for seed {seed}\n".encode()
        semantic = hashlib.sha256(sidecar).hexdigest()
        value: dict[str, object] = {
            "schema": "larch.chart_spr.semantic_digest",
            "schema_version": 1,
            "digest_algorithm": "sha256",
            "payload_encoding": "larch.chart_spr.semantic.ndjson.v1",
            "semantic_sha256": semantic,
            "contract_sha256": digest(f"contract-{seed}"),
            "candidates_sha256": digest(f"candidates-{seed}"),
            "exact_sha256": digest(f"exact-{seed}"),
            "acceptance_sha256": digest(f"acceptance-{seed}"),
            "chain_sha256": digest(f"chain-{seed}"),
            "final_topology_sha256": digest(f"topology-{seed}"),
            "record_count": 400,
            "candidate_count": 96,
            "exact_candidate_count": 12,
            "iteration_count": 3,
        }
        self.search[(seed, worker)] = value
        self.sidecars[(seed, worker)] = sidecar

    def _add(self, seed: int, worker: int, trial: int) -> None:
        row_id = self.row_id(seed, worker)
        fixture = self.fixture(seed)
        report_name = (
            f"logs/{fixture}_{acceptance.METHOD}_trial{trial}_{row_id}_w{worker}.out"
        )
        accepted_ms = self.accepted_ms(worker)
        top, iterations = self._report(seed, worker, accepted_ms, "1000")
        self.reports[(seed, worker, trial)] = top
        self.iterations[(seed, worker, trial)] = iterations

        output = {
            "schema": "larch.dag.semantic_digest",
            "schema_version": 1,
            "digest_algorithm": "sha256",
            "semantic_sha256": digest(f"output-{seed}"),
            "clades_sha256": digest(f"clades-{seed}"),
            "productions_sha256": digest(f"productions-{seed}"),
            "clade_count": 80,
            "production_count": 120,
            "parsimony_min": 70,
        }
        self.output[(seed, worker, trial)] = output
        argv = digest(f"argv-{seed}-{worker}")
        row = {column: "0" for column in acceptance.REQUIRED_COLUMNS}
        row.update(
            {
                "fixture": fixture,
                "method": acceptance.METHOD,
                "status": "ok",
                "validation_status": "ok",
                "initial_validated_parsimony_min": "100",
                "final_validated_parsimony_min": "70",
                "best_reported_objective": "70",
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
                "total_ms": "1000",
                "active_patterns": "64",
                "report_path": report_name,
                "row_id": row_id,
                "requested_workers": str(worker),
                "resolved_workers": str(worker),
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
                "wall_clock_s": "1",
                "peak_sampled_rss_kb": "2000" if worker == 8 else "1000",
                "peak_sampled_swap_kb": "0",
                "process_rss_limit_bytes": str(16 * 1024**3),
                "configured_chart_memory_budget": str(12 * 1024**3),
                "manifest_rss_limit_bytes": str(16 * 1024**3),
                "input_sha256": digest("input"),
                "refseq_sha256": "NA",
                "search_semantic_sha256": str(self.search[(seed, worker)]["semantic_sha256"]),
                "output_semantic_sha256": str(output["semantic_sha256"]),
                "canonical_argv_sha256": argv,
            }
        )
        row["trial_semantic_sha256"] = acceptance.trial_semantic_digest(row)
        row["canonical_digest"] = row["trial_semantic_sha256"]
        self.rows.append(row)

    def matching(self, seed: int, worker: int, trial: int | None = None) -> list[dict[str, str]]:
        result = [
            row
            for row in self.rows
            if row["seed"] == str(seed) and row["requested_workers"] == str(worker)
        ]
        if trial is not None:
            result = [row for row in result if row["trial_index"] == str(trial)]
        return result

    def set_report(
        self, seed: int, worker: int, trial: int, key: str, value: str
    ) -> None:
        self.reports[(seed, worker, trial)][key] = value

    def set_all_reports(self, seed: int, worker: int, key: str, value: str) -> None:
        for trial in range(1, self.repetitions + 1):
            self.set_report(seed, worker, trial, key, value)

    def set_raw(self, seed: int, worker: int, key: str, value: str) -> None:
        for row in self.matching(seed, worker):
            row[key] = value

    def set_timing(self, seed: int, worker: int, accepted: str, total: str) -> None:
        self.set_raw(seed, worker, "accepted_rebuild_ms", accepted)
        self.set_raw(seed, worker, "total_ms", total)
        self.set_all_reports(seed, worker, "accepted_rebuild_ms", accepted)
        self.set_all_reports(seed, worker, "total_ms", total)

    @staticmethod
    def write_report(
        path: Path,
        top: dict[str, str],
        iterations: list[dict[str, str]],
        extra: list[str] | None = None,
    ) -> None:
        lines = ["chart_spr_search:"]
        lines.extend(f"  {key}: {value}" for key, value in top.items())
        lines.append("  iteration_reports:")
        for iteration in iterations:
            lines.append(f"    - iteration: {iteration['iteration']}")
            lines.extend(
                f"      {key}: {value}"
                for key, value in iteration.items()
                if key != "iteration"
            )
        if extra:
            lines.extend(extra)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def write(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        for (seed, worker, trial), top in self.reports.items():
            row = self.matching(seed, worker, trial)[0]
            report = self.root / row["report_path"]
            self.write_report(
                report,
                top,
                self.iterations[(seed, worker, trial)],
                self.extra_report_lines.get((seed, worker, trial)),
            )
            _, _, _, dag_path = acceptance.canonical_paths(self.raw, row, report)
            dag_path.parent.mkdir(parents=True, exist_ok=True)
            dag_path.write_text(
                json.dumps(self.output[(seed, worker, trial)], separators=(",", ":"))
                + "\n",
                encoding="utf-8",
            )
        for (seed, worker), value in self.search.items():
            row = self.matching(seed, worker)[0]
            report = self.root / row["report_path"]
            compact, full, sidecar, _ = acceptance.canonical_paths(self.raw, row, report)
            for path in (compact, full):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(value, separators=(",", ":")) + "\n", encoding="utf-8")
            sidecar.write_bytes(self.sidecars[(seed, worker)])
        for seed, (top, iterations) in self.frozen.items():
            self.write_report(self.frozen_path(seed), top, iterations)
        with self.raw.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=self.columns,
                delimiter="\t",
                lineterminator="\n",
                extrasaction="ignore",
            )
            writer.writeheader()
            writer.writerows(self.rows)

    def frozen_path(self, seed: int) -> Path:
        return self.root / "frozen" / f"seed-{seed}.out"


class Phase9AcceptanceTest(unittest.TestCase):
    maxDiff = None

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="wric-phase9-acceptance-")
        self.root = Path(self.temporary.name)
        self.data = Dataset(self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def command(self, *, defer: bool = False) -> list[str]:
        command = [
            sys.executable,
            str(TOOL),
            "--benchmark-dir",
            str(self.root),
            "--repetitions",
            str(self.data.repetitions),
        ]
        if defer:
            command.extend(
                (
                    "--defer-frozen-oracle-characterization",
                    "historical baseline capture pending",
                )
            )
        else:
            for seed in acceptance.SEEDS:
                command.extend(
                    ("--frozen-oracle-report", f"{seed}={self.data.frozen_path(seed)}")
                )
        return command

    def run_tool(
        self, *, defer: bool = False, extra: list[str] | None = None
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, object] | None]:
        self.data.write()
        command = self.command(defer=defer)
        if extra:
            command.extend(extra)
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError:
            result = None
        return completed, result

    def assert_pass(self, *, defer: bool = False, extra: list[str] | None = None) -> dict[str, object]:
        completed, result = self.run_tool(defer=defer, extra=extra)
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        assert result is not None
        return result

    def assert_failure(self, expected: str, *, defer: bool = False) -> dict[str, object]:
        completed, result = self.run_tool(defer=defer)
        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        assert result is not None
        self.assertIn(expected, str(result.get("failure", "")))
        return result

    def test_complete_characterization_and_same_revision_matrix_pass(self) -> None:
        result = self.assert_pass()
        self.assertEqual(result["status"], "pass")
        frozen = result["frozen_oracle_characterization"]
        self.assertEqual(frozen["role"], "historical_fixture_characterization_only")
        self.assertFalse(frozen["used_for_same_revision_speed_gate"])
        same = result["same_revision"]
        self.assertEqual(same["role"], "only_source_for_w1_w8_timing_comparison")
        self.assertEqual(
            same["timings"]["1"]["w8_over_w1_accepted_rebuild"], "0.555556"
        )

    def test_explicit_deferral_is_successful_but_not_final_acceptance(self) -> None:
        result = self.assert_pass(defer=True)
        self.assertEqual(result["status"], "deferred_frozen_oracle")
        self.assertEqual(result["frozen_oracle_characterization"]["status"], "deferred")

    def test_json_and_markdown_outputs_are_deterministic(self) -> None:
        json_path = self.root / "acceptance.json"
        markdown_path = self.root / "acceptance.md"
        extra = ["--json-output", str(json_path), "--markdown-output", str(markdown_path)]
        self.assert_pass(defer=True, extra=extra)
        first_json = json_path.read_bytes()
        first_markdown = markdown_path.read_bytes()
        self.assert_pass(defer=True, extra=extra)
        self.assertEqual(first_json, json_path.read_bytes())
        self.assertEqual(first_markdown, markdown_path.read_bytes())
        self.assertIn(b"same-revision measured trials", first_markdown)

    def test_speed_boundary_and_below_ten_percent_exemption(self) -> None:
        self.data.set_timing(1, 1, "150", "1000")
        self.data.set_timing(1, 8, "100", "1000")
        self.data.set_timing(7, 1, "90", "1000")
        self.data.set_timing(7, 8, "90", "1000")
        result = self.assert_pass()
        timings = result["same_revision"]["timings"]
        self.assertEqual(timings["1"]["speed_gate"], "pass")
        self.assertEqual(timings["7"]["speed_gate"], "exempt_below_10_percent")

    def test_qualifying_same_revision_speed_failure(self) -> None:
        self.data.set_timing(1, 8, "121", "1000")
        self.assert_failure("not at least 1.5x faster")

    def test_frozen_characterization_threshold_is_not_replaced_by_current_w1(self) -> None:
        self.data.frozen[1][0]["accepted_rebuild_ms"] = "99.999"
        self.assert_failure("below 100 ms")

    def test_noncommitted_iteration_is_vacuous(self) -> None:
        self.data.iterations[(1, 1, 1)][1]["accepted_move_committed"] = "false"
        self.assert_failure("accepted_move_committed=false")

    def test_affected_row_floor_is_strict(self) -> None:
        self.data.set_report(1, 1, 1, "inside_rows_recomputed_on_commit", "95")
        self.assert_failure("expected at least 96")

    def test_exact_reuse_and_copy_counter_contract_is_strict(self) -> None:
        self.data.set_report(1, 1, 1, "accepted_exact_trim_reuse_rejections", "1")
        self.assert_failure("accepted_exact_trim_reuse_rejections=1")
        self.data.set_report(1, 1, 1, "accepted_exact_trim_reuse_rejections", "0")
        self.data.set_report(1, 1, 1, "local_leaf_state_owned_copies", "1")
        self.assert_failure("local_leaf_state_owned_copies=1")

    def test_final_compaction_and_transient_fallback_contract_is_strict(self) -> None:
        self.data.set_report(1, 1, 1, "overlay_materializations_for_final_compaction", "2")
        self.assert_failure("overlay_materializations_for_final_compaction=2")
        self.data.set_report(1, 1, 1, "overlay_materializations_for_final_compaction", "1")
        self.data.set_report(1, 1, 1, "transient_chain_extension_fallbacks", "1")
        self.assert_failure("transient_chain_extension_fallbacks=1")

    def test_scheduler_task_join_and_w8_cache_axis_are_strict(self) -> None:
        self.data.set_report(1, 8, 1, "chart_scheduler_tasks_joined", "15")
        self.assert_failure("scheduler task accounting does not reconcile")
        self.data.set_report(1, 8, 1, "chart_scheduler_tasks_joined", "16")
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_active_worker_high_water", "1")
        self.assert_failure("W8 inside_cache cache update did not activate multiple workers")

    def test_raw_semantic_drift_and_component_drift_are_rejected(self) -> None:
        row = self.data.matching(1, 8, 1)[0]
        row["search_semantic_sha256"] = digest("wrong-search")
        row["trial_semantic_sha256"] = acceptance.trial_semantic_digest(row)
        row["canonical_digest"] = row["trial_semantic_sha256"]
        self.assert_failure("raw/search-companion semantic digests differ")

        row["search_semantic_sha256"] = str(self.data.search[(1, 8)]["semantic_sha256"])
        row["trial_semantic_sha256"] = acceptance.trial_semantic_digest(row)
        row["canonical_digest"] = row["trial_semantic_sha256"]
        self.data.search[(1, 8)]["chain_sha256"] = digest("wrong-chain")
        self.assert_failure("canonical search component chain_sha256 differs")

    def test_external_canonical_dag_component_drift_is_rejected(self) -> None:
        self.data.output[(1, 8, 1)]["clades_sha256"] = digest("wrong-clades")
        self.assert_failure("canonical output component clades_sha256 differs")

    def test_rss_ratio_swap_and_limit_gates_are_strict(self) -> None:
        self.data.set_raw(1, 8, "peak_sampled_rss_kb", "2001")
        self.assert_failure("exceeds 2x W1")
        self.data.set_raw(1, 8, "peak_sampled_rss_kb", "2000")
        self.data.matching(1, 8, 1)[0]["peak_sampled_swap_kb"] = "1"
        self.assert_failure("peak sampled swap is nonzero")

    def test_missing_column_duplicate_column_and_missing_report_key_fail(self) -> None:
        self.data.columns.remove("accepted_rebuild_ms")
        self.assert_failure("missing required columns: accepted_rebuild_ms")
        self.data.columns = list(acceptance.REQUIRED_COLUMNS) + ["seed"]
        self.assert_failure("duplicate TSV columns: seed")
        self.data.columns = list(acceptance.REQUIRED_COLUMNS)
        del self.data.reports[(1, 1, 1)]["accepted_exact_trims_reused"]
        self.assert_failure("report lacks accepted_exact_trims_reused")

    def test_strict_canonical_json_schema_and_full_sidecar_hash(self) -> None:
        self.data.search[(1, 1)]["unexpected"] = 1
        self.assert_failure("JSON schema mismatch")
        del self.data.search[(1, 1)]["unexpected"]
        self.data.sidecars[(1, 1)] = b"wrong bytes\n"
        self.assert_failure("full sidecar SHA-256 does not equal compact semantic digest")

    def test_seed_workload_names_must_be_distinct(self) -> None:
        for row in self.data.rows:
            if row["seed"] == "7":
                row["fixture"] = self.data.fixture(1)
                old = Path(row["report_path"])
                row["report_path"] = str(old.with_name(old.name.replace(self.data.fixture(7), self.data.fixture(1), 1)))
        self.assert_failure("each seed must use a distinct fixture/workload name")


if __name__ == "__main__":
    unittest.main()
