#!/usr/bin/env python3
"""Adversarial fixtures for the strict Phase-9 acceptance postprocessor."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from unittest import mock


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
        self.columns = list(acceptance.REQUIRED_COLUMNS)
        for seed in acceptance.SEEDS:
            for worker in acceptance.MEASURED_WORKERS:
                self._add_search_digest(seed, worker)
                for trial in range(1, repetitions + 1):
                    self._add(seed, worker, trial)
            frozen_top, frozen_iterations = self._report(seed, 1, "120", "1000")
            # The sealed historical report predates Phase-9's per-commit row
            # deltas and emitted only the first accepted signature.  Keep that
            # old schema in the fixture so current-only evidence never leaks
            # into the frozen compatibility path.
            for index, iteration in enumerate(frozen_iterations):
                iteration.pop("accepted_inside_rows_recomputed")
                iteration.pop("accepted_outside_rows_recomputed")
                if index:
                    iteration.pop("accepted_candidate_signature")
            self.frozen[seed] = (frozen_top, frozen_iterations)

    @staticmethod
    def fixture(seed: int) -> str:
        return f"phase9-three-accepts-seed{seed}"

    @staticmethod
    def row_id(seed: int, worker: int) -> str:
        return acceptance.DEFAULT_ROW_ID_TEMPLATE.format(seed=seed, worker=worker)

    @staticmethod
    def accepted_ms(worker: int) -> str:
        return {1: "180", 8: "100"}[worker]

    @staticmethod
    def scheduler(worker: int) -> dict[str, str]:
        result: dict[str, str] = {}
        for axis in acceptance.AXES:
            active = axis in ("inside_cache", "outside_cache")
            values = {
                "operations": 4 if active else 0,
                "items": 256 if active else 0,
                "ranges": (4 if worker == 1 else 128) if active else 0,
                "tasks": (0 if worker == 1 else 32) if active else 0,
                "active_worker_high_water": (1 if worker == 1 else 8) if active else 0,
                "parallel_operations": (0 if worker == 1 else 4) if active else 0,
                "minimum_effective_grain": (64 if worker == 1 else 2) if active else 0,
                "maximum_effective_grain": (64 if worker == 1 else 2) if active else 0,
            }
            for field, value in values.items():
                result[f"chart_axis_{axis}_{field}"] = str(value)
        parallel = worker == 8
        ranges = 256 if parallel else 8
        tasks = 64 if parallel else 0
        result.update(
            {
                "chart_workers_requested": str(worker),
                "chart_workers_resolved": str(worker),
                "chart_worker_policy": "explicit",
                "chart_worker_resolution_policy": "explicit",
                "chart_workers_actually_active_high_water": str(worker),
                "chart_scheduler_operations": "8",
                "chart_scheduler_parallel_operations": "8" if parallel else "0",
                "chart_scheduler_serial_fallbacks": "0" if parallel else "8",
                "chart_scheduler_ranges_created": str(ranges),
                "chart_scheduler_ranges_completed": str(ranges),
                "chart_scheduler_ranges_cancelled": "0",
                "chart_scheduler_tasks_submitted": str(tasks),
                "chart_scheduler_tasks_completed": str(tasks),
                "chart_scheduler_tasks_joined": str(tasks),
                "chart_scheduler_pending_tasks": "0",
                "chart_scheduler_pending_tasks_at_shutdown": "0",
                "chart_scheduler_minimum_effective_grain": "2" if parallel else "64",
                "chart_scheduler_maximum_effective_grain": "2" if parallel else "64",
                "chart_scheduler_queue_wait_nanoseconds": "160" if parallel else "0",
                "chart_scheduler_queue_wait_nanoseconds_max": "10" if parallel else "0",
                "chart_scheduler_queue_wait_samples": str(tasks),
                "chart_scheduler_nested_serial_fallbacks": "0",
                "chart_scheduler_rejected_concurrent_operations": "0",
                "chart_scheduler_pool_lifetimes": "1" if parallel else "0",
                "chart_scheduler_pool_lifetimes_stopped": "1" if parallel else "0",
                "chart_scheduler_live_pool_threads": "0",
                "chart_scheduler_shutdown": "true",
            }
        )
        return result

    @staticmethod
    def iteration(seed: int, index: int) -> dict[str, str]:
        before = 100 - index * 10
        after = before - 10
        return {
            "iteration": str(index),
            "candidates_generated": "32",
            "candidates_scored": "32",
            "candidate_score_failures": "0",
            "candidates_exact_verified": "4",
            "accepted_move_present": "true",
            "accepted_move_committed": "true",
            "post_materialization_rejected": "false",
            "state_score_before": str(before),
            "state_score_after": str(after),
            "accepted_lower_bound_delta": "-10",
            "accepted_lower_bound_new_score": str(after),
            "accepted_exact_kind": "grammar_exact",
            "accepted_exact_delta": "-10",
            "accepted_exact_new_score": str(after),
            "accepted_affected_clades": "6",
            "accepted_topology_selection": "none",
            "accepted_inside_rows_recomputed": "32",
            "accepted_outside_rows_recomputed": "32",
            "accepted_candidate_signature": f"move-{seed}-{index}",
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
            "actual_dag_mutation": "true",
            "output_dag_mutated": "true",
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
            "full_search_state_rebuilds": "1",
            "final_compaction_rebuilds": "1",
            "final_compaction_exactness_kind": "exact_optimal_production_union",
            "overlay_materializations_for_exact_verification": "3",
            "overlay_materializations_for_accept_materialization": "0",
            "overlay_materializations_for_final_compaction": "1",
            "inside_rows_recomputed_on_commit": "96",
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
            "memory_budget_bytes": str(12 * 1024**3),
            "accepted_rebuild_ms": accepted_ms,
            "total_ms": total_ms,
            **self.scheduler(worker),
        }
        return top, [self.iteration(seed, index) for index in range(3)]

    @staticmethod
    def canonical_records(seed: int) -> list[dict[str, object]]:
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
                "candidate_cap_semantics": "post_dedup",
                "max_iterations": 3,
                "max_candidates": 32,
                "top_k_exact": 4,
                "seed": seed,
                "use_bound_pruning": True,
                "require_exact_keep_mask": True,
                "randomize_order": False,
                "reservoir_sample": False,
                "include_immediate_reversals": False,
            },
            {"record": "initial_state", "active_patterns": 64, "initial_score": 100},
        ]
        for iteration in range(3):
            before = 100 - iteration * 10
            after = before - 10
            records.append(
                {
                    "record": "iteration_begin",
                    "iteration": iteration,
                    "seed": seed + iteration,
                    "state_score_before": before,
                }
            )
            for stream in range(32):
                signature = f"move-{seed}-{iteration}" if stream == 0 else f"candidate-{seed}-{iteration}-{stream}"
                records.append(
                    {
                        "record": "candidate",
                        "iteration": iteration,
                        "stream_index": stream,
                        "signature": signature,
                        "valid": True,
                        "affected_clade_count": 6,
                    }
                )
                new = after if stream == 0 else before
                records.append(
                    {
                        "record": "candidate_lower_bound",
                        "iteration": iteration,
                        "stream_index": stream,
                        "kind": "composite_lower_bound",
                        "delta": new - before,
                        "old_score": before,
                        "new_score": new,
                        "exact_multisite": False,
                    }
                )
            for stream in range(4):
                new = after if stream == 0 else before
                records.append(
                    {
                        "record": "candidate_exact",
                        "iteration": iteration,
                        "stream_index": stream,
                        "kind": "grammar_exact",
                        "delta": new - before,
                        "old_score": before,
                        "new_score": new,
                        "exact_multisite": True,
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
                        "selected_signature": f"move-{seed}-{iteration}",
                        "state_score_after": after,
                    },
                    {
                        "record": "chain_entry",
                        "position": iteration,
                        "commit_source": "spr_overlay_delta",
                        "added_production_keys": [f"add-{iteration}"],
                        "tombstoned_production_keys": [f"remove-{iteration}"],
                    },
                )
            )
        records.append({"record": "final_state", "final_score": 70, "accepted_moves": 3})
        return records

    def _add_search_digest(self, seed: int, worker: int) -> None:
        records = self.canonical_records(seed)
        sidecar = b"".join(
            (json.dumps(record, separators=(",", ":")) + "\n").encode("utf-8")
            for record in records
        )
        semantic = hashlib.sha256(sidecar).hexdigest()
        self.search[(seed, worker)] = {
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
            "record_count": len(records),
            "candidate_count": 96,
            "exact_candidate_count": 12,
            "iteration_count": 3,
        }
        self.sidecars[(seed, worker)] = sidecar

    def _add(self, seed: int, worker: int, trial: int) -> None:
        row_id = self.row_id(seed, worker)
        fixture = self.fixture(seed)
        report_name = f"logs/{fixture}_{acceptance.METHOD}_trial{trial}_{row_id}_w{worker}.out"
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
            "parsimony_min": 80,
        }
        self.output[(seed, worker, trial)] = output
        row = {column: "0" for column in acceptance.REQUIRED_COLUMNS}
        row.update(
            {
                "fixture": fixture,
                "method": acceptance.METHOD,
                "status": "ok",
                "validation_status": "ok",
                "initial_validated_parsimony_min": "120",
                "final_validated_parsimony_min": "80",
                "best_reported_objective": "70",
                "best_validated_parsimony_min": "80",
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
                "user_cpu_s": "0.9" if worker == 1 else "4",
                "system_cpu_s": "0.05" if worker == 1 else "0.1",
                "peak_sampled_rss_kb": "2000" if worker == 8 else "1000",
                "peak_sampled_swap_kb": "0",
                "process_rss_limit_bytes": str(16 * 1024**3),
                "configured_chart_memory_budget": str(12 * 1024**3),
                "manifest_rss_limit_bytes": str(16 * 1024**3),
                "input_sha256": digest("input"),
                "refseq_sha256": "NA",
                "search_semantic_sha256": str(self.search[(seed, worker)]["semantic_sha256"]),
                "output_semantic_sha256": str(output["semantic_sha256"]),
                "canonical_argv_sha256": digest(f"argv-{seed}-{worker}"),
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
        return result if trial is None else [row for row in result if row["trial_index"] == str(trial)]

    def set_report(self, seed: int, worker: int, trial: int, key: str, value: str) -> None:
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
    def write_report(path: Path, top: dict[str, str], iterations: list[dict[str, str]]) -> None:
        lines = ["chart_spr_search:"]
        lines.extend(f"  {key}: {value}" for key, value in top.items())
        lines.append("  iteration_reports:")
        for iteration in iterations:
            lines.append(f"    - iteration: {iteration['iteration']}")
            lines.extend(
                f"      {key}: {value}" for key, value in iteration.items() if key != "iteration"
            )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def write(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        for (seed, worker, trial), top in self.reports.items():
            row = self.matching(seed, worker, trial)[0]
            report = self.root / row["report_path"]
            self.write_report(report, top, self.iterations[(seed, worker, trial)])
            _, _, _, dag_path = acceptance.canonical_paths(self.raw, row, report)
            dag_path.parent.mkdir(parents=True, exist_ok=True)
            dag_path.write_text(json.dumps(self.output[(seed, worker, trial)]) + "\n", encoding="utf-8")
        for (seed, worker), value in self.search.items():
            rows = self.matching(seed, worker)
            warmup = acceptance.expected_phase9_warmup_compact_path(
                self.root, rows[0], worker
            )
            warmup.parent.mkdir(parents=True, exist_ok=True)
            warmup.write_text(json.dumps(value) + "\n", encoding="utf-8")
            for row in rows:
                report = self.root / row["report_path"]
                compact, _, _, _ = acceptance.canonical_paths(
                    self.raw, row, report
                )
                compact.parent.mkdir(parents=True, exist_ok=True)
                compact.write_text(json.dumps(value) + "\n", encoding="utf-8")
            first = rows[0]
            report = self.root / first["report_path"]
            _, full, sidecar, _ = acceptance.canonical_paths(
                self.raw, first, report
            )
            full.parent.mkdir(parents=True, exist_ok=True)
            full.write_text(json.dumps(value) + "\n", encoding="utf-8")
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
        self.sandbox = Path(self.temporary.name)
        self.root = self.sandbox / "run"
        self.data = Dataset(self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def command(self, *, ad_hoc: bool = False) -> list[str]:
        command = [
            sys.executable,
            str(TOOL),
            "evaluate",
            "--benchmark-dir",
            str(self.root),
            "--defer-frozen-oracle-characterization",
            "sealed baseline capture pending",
        ]
        if ad_hoc:
            for seed in acceptance.SEEDS:
                command.extend(("--frozen-oracle-report", f"{seed}={self.data.frozen_path(seed)}"))
        return command

    def run_tool(self, *, ad_hoc: bool = False, extra: list[str] | None = None):
        self.data.write()
        command = self.command(ad_hoc=ad_hoc)
        if extra:
            command.extend(extra)
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError:
            result = None
        return completed, result

    def assert_pass(self, *, ad_hoc: bool = False, extra: list[str] | None = None) -> dict[str, object]:
        completed, result = self.run_tool(ad_hoc=ad_hoc, extra=extra)
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        return result

    def assert_failure(self, expected: str) -> dict[str, object]:
        completed, result = self.run_tool()
        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        self.assertIn(expected, str(result.get("failure", "")))
        return result

    def assert_written_failure(self, expected: str) -> dict[str, object]:
        completed = subprocess.run(
            self.command(), text=True, capture_output=True, check=False
        )
        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        result = json.loads(completed.stdout)
        self.assertIsInstance(result, dict)
        self.assertIn(expected, str(result.get("failure", "")))
        return result

    def test_explicit_deferral_is_successful_but_unambiguously_non_final(self) -> None:
        result = self.assert_pass(ad_hoc=True)
        self.assertEqual(result["schema_version"], 2)
        self.assertEqual(result["status"], "deferred_non_final")
        self.assertEqual(result["matrix"]["workers"], [1, 8])
        self.assertEqual(result["matrix"]["repetitions"], 3)
        self.assertEqual(result["provenance"]["status"], "deferred_non_final")
        self.assertEqual(result["same_revision"]["timings"]["1"]["w8_over_w1_accepted_rebuild"], "0.555556")
        self.assertEqual(
            result["same_revision"]["semantics"]["1"]["score_domains"],
            {
                "search_objective": {"initial": 100, "final": 70},
                "external_dag_parsimony": {"initial": 120, "final": 80},
            },
        )

    def test_exact_current_matrix_repetitions_and_row_ids_are_strict(self) -> None:
        extra = dict(self.data.rows[-1])
        extra["trial_index"] = "4"
        self.data.rows.append(extra)
        self.assert_failure("exactly 18 timed W1/W8 rows")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["row_id"] = "phase9-local-commit-custom"
        self.assert_failure("expected 3 measured trials, found 2")

    def test_score_reconciliation_rejects_raw_report_and_canonical_drift(self) -> None:
        self.data.matching(1, 1, 1)[0]["best_validated_parsimony_min"] = "71"
        self.assert_failure("raw external final/best-validated scores disagree")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["best_reported_objective"] = "80"
        self.assert_failure("report search final disagrees with raw best-reported objective")
        self.data = Dataset(self.root)
        row = self.data.matching(1, 1, 1)[0]
        row["final_validated_parsimony_min"] = "70"
        row["best_validated_parsimony_min"] = "70"
        self.assert_failure("external canonical DAG parsimony disagrees with raw validation")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "final_score", "71")
        self.assert_failure("top-level and iteration endpoint scores disagree")
        self.data = Dataset(self.root)
        records = self.data.canonical_records(1)
        next(record for record in records if record["record"] == "final_state")["final_score"] = 71
        payload = b"".join((json.dumps(record, separators=(",", ":")) + "\n").encode() for record in records)
        self.data.sidecars[(1, 1)] = payload
        self.data.search[(1, 1)]["semantic_sha256"] = hashlib.sha256(payload).hexdigest()
        self.data.search[(1, 1)]["record_count"] = len(records)
        for row in self.data.matching(1, 1):
            row["search_semantic_sha256"] = str(self.data.search[(1, 1)]["semantic_sha256"])
            row["trial_semantic_sha256"] = acceptance.trial_semantic_digest(row)
            row["canonical_digest"] = row["trial_semantic_sha256"]
        self.assert_failure("canonical iteration/final scores disagree")

    def test_search_and_external_score_endpoints_cannot_be_cross_wired(self) -> None:
        frozen_row = {
            "expected_initial_score": "120",
            "expected_final_score": "70",
            "expected_validated_parsimony": "80",
        }
        row = self.data.matching(1, 1, 1)[0]
        acceptance.validate_current_score_domain_bindings(row, frozen_row, "scores")

        cross_wired = dict(row)
        cross_wired["initial_validated_parsimony_min"] = "100"
        with self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "initial_validated_parsimony_min does not match sealed supplement expected_initial_score",
        ):
            acceptance.validate_current_score_domain_bindings(
                cross_wired, frozen_row, "scores"
            )

        cross_wired = dict(row)
        cross_wired["final_validated_parsimony_min"] = "70"
        with self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "final_validated_parsimony_min does not match sealed supplement expected_validated_parsimony",
        ):
            acceptance.validate_current_score_domain_bindings(
                cross_wired, frozen_row, "scores"
            )

        cross_wired = dict(row)
        cross_wired["best_reported_objective"] = "80"
        with self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "best_reported_objective does not match sealed supplement expected_final_score",
        ):
            acceptance.validate_current_score_domain_bindings(
                cross_wired, frozen_row, "scores"
            )

    def test_any_frozen_representative_at_ten_percent_forbids_exemption(self) -> None:
        below = acceptance.ParsedReport(
            path=Path("below.out"),
            top={"accepted_rebuild_ms": "99.9", "total_ms": "1000"},
            iterations=(),
        )
        boundary = acceptance.ParsedReport(
            path=Path("boundary.out"),
            top={"accepted_rebuild_ms": "100", "total_ms": "1000"},
            iterations=(),
        )
        shares = [acceptance.report_phase_share(below, f"frozen row {index}") for index in range(12)]
        self.assertFalse(acceptance.frozen_profile_requires_speed_gate(shares))
        shares[-1] = acceptance.report_phase_share(boundary, "adversarial frozen row")
        self.assertEqual(shares[-1], Decimal("0.1"))
        self.assertTrue(acceptance.frozen_profile_requires_speed_gate(shares))

        self.data.set_timing(7, 1, "90", "1000")
        self.data.set_timing(7, 8, "90", "1000")
        self.data.write()
        args = argparse.Namespace(
            benchmark_dir=str(self.root),
            raw_trials=None,
            defer_frozen_oracle_characterization=None,
            expected_run_ledger_sha256="0" * 64,
            base_repo_root=str(REPO),
            working_repo_root=str(REPO),
        )
        with (
            mock.patch.object(
                acceptance,
                "verify_expected_run_ledger_anchor",
                return_value="0" * 64,
            ),
            mock.patch.object(
                acceptance,
                "audit_sealed_frozen_archive",
                return_value=(object(), {}, True),
            ),
            mock.patch.object(
                acceptance,
                "frozen_oracle_search_digests",
                return_value={
                    key: acceptance.SearchDigestSnapshot(
                        path=self.root / "frozen" / f"{key[0]}-{key[1]}.json",
                        mapping=dict(value),
                        data=(json.dumps(value) + "\n").encode("utf-8"),
                        identity=(0, index),
                    )
                    for index, (key, value) in enumerate(
                        self.data.search.items(), start=1
                    )
                },
            ),
            mock.patch.object(
                acceptance,
                "audit_run_archive",
                return_value=({}, {}),
            ),
            self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "seed 7: same-revision W8 accepted_rebuild_ms median 90 is not at least 1.5x faster",
            ),
        ):
            acceptance.evaluate(args)

    def test_timed_accepted_sequence_and_delta_are_strict(self) -> None:
        self.data.iterations[(1, 1, 1)][2]["accepted_candidate_signature"] = "other"
        self.assert_failure("report/full-canonical timed accepted sequences disagree")
        self.data = Dataset(self.root)
        self.data.iterations[(1, 1, 1)][1].pop("accepted_candidate_signature")
        self.assert_failure("current accepted signature is absent")
        self.data = Dataset(self.root)
        self.data.iterations[(1, 1, 1)][1]["accepted_exact_delta"] = "-9"
        self.assert_failure("exact delta does not reconcile")
        self.data = Dataset(self.root)
        iteration = self.data.iterations[(1, 1, 1)][1]
        iteration["state_score_after"] = iteration["state_score_before"]
        iteration["accepted_exact_delta"] = "0"
        iteration["accepted_exact_new_score"] = iteration["state_score_before"]
        iteration["accepted_lower_bound_delta"] = "0"
        iteration["accepted_lower_bound_new_score"] = iteration["state_score_before"]
        iteration["post_materialization_rebuilt_score"] = iteration["state_score_before"]
        self.assert_failure("did not strictly improve")

    def test_local_commit_labels_mutation_rebuilds_and_attempts_are_exact(self) -> None:
        self.data.set_report(1, 1, 1, "actual_dag_mutation", "false")
        self.assert_failure("actual_dag_mutation='false'")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "full_search_state_rebuilds", "999")
        self.assert_failure("full_search_state_rebuilds=999")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["candidate_accepts_attempted"] = "4"
        self.assert_failure("expected exactly 3")

    def test_row_view_cache_build_and_memory_contracts_are_exact(self) -> None:
        self.data.set_report(1, 1, 1, "local_commit_inside_row_view_pattern_visits", "96")
        self.assert_failure("expected exactly 96*64=6144")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "initial_state_inside_charts_built", "0")
        self.assert_failure("initial_state_inside_charts_built=0, expected 64")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "inside_cache_inside_charts_built", "63")
        self.assert_failure("inside_cache_inside_charts_built=63, expected 64")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "inside_cache_resident_inside_charts_consumed", "1")
        self.assert_failure("inside_cache_resident_inside_charts_consumed=1, expected 0")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "outside_cache_inside_charts_built", "1")
        self.assert_failure("outside_cache_inside_charts_built=1, expected 0")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "outside_cache_inside_charts_reused", "63")
        self.assert_failure("outside_cache_inside_charts_reused=63, expected 64")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "outside_cache_outside_charts_built", "63")
        self.assert_failure("outside_cache_outside_charts_built=63, expected 64")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["process_rss_limit_bytes"] = str(99 * 1024**4)
        self.assert_failure("process_rss_limit_bytes=")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["configured_chart_memory_budget"] = "1"
        self.assert_failure("configured_chart_memory_budget=1")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["manifest_rss_limit_bytes"] = str(99 * 1024**4)
        self.assert_failure("manifest_rss_limit_bytes=")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "memory_budget_bytes", "1")
        self.assert_failure("report memory_budget_bytes=1")

    def test_each_current_commit_has_per_axis_row_floor_and_exact_sums(self) -> None:
        self.data.iterations[(1, 1, 1)][1]["accepted_inside_rows_recomputed"] = "31"
        self.data.set_report(1, 1, 1, "inside_rows_recomputed_on_commit", "95")
        self.assert_failure("accepted_inside_rows_recomputed=31, expected at least 32")
        self.data = Dataset(self.root)
        self.data.set_report(1, 1, 1, "inside_rows_recomputed_on_commit", "97")
        self.assert_failure("do not equal top-level inside/outside totals")
        self.data = Dataset(self.root)
        self.data.iterations[(1, 1, 1)][0].pop("accepted_outside_rows_recomputed")
        self.assert_failure("iteration report lacks accepted_outside_rows_recomputed")

    def test_scheduler_requires_three_updates_and_global_axis_reconciliation(self) -> None:
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_operations", "1")
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_parallel_operations", "1")
        self.data.set_report(1, 8, 1, "chart_scheduler_operations", "5")
        self.data.set_report(1, 8, 1, "chart_scheduler_parallel_operations", "5")
        self.assert_failure("expected exact initial-plus-three-commit value 4")
        self.data = Dataset(self.root)
        self.data.set_report(1, 8, 1, "chart_scheduler_ranges_created", "255")
        self.data.set_report(1, 8, 1, "chart_scheduler_ranges_completed", "255")
        self.assert_failure("global ranges=255 does not equal axis sum 256")
        self.data = Dataset(self.root)
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_items", "64")
        self.assert_failure("inside_cache items=64, expected exact")
        self.data = Dataset(self.root)
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_ranges", "32")
        self.data.set_report(1, 8, 1, "chart_scheduler_ranges_created", "160")
        self.data.set_report(1, 8, 1, "chart_scheduler_ranges_completed", "160")
        self.assert_failure("inside_cache ranges=32, expected exact")
        self.data = Dataset(self.root)
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_tasks", "8")
        for key in (
            "chart_scheduler_tasks_submitted",
            "chart_scheduler_tasks_completed",
            "chart_scheduler_tasks_joined",
            "chart_scheduler_queue_wait_samples",
        ):
            self.data.set_report(1, 8, 1, key, "40")
        self.assert_failure("inside_cache tasks=8, expected exact")
        self.data = Dataset(self.root)
        self.data.set_report(1, 8, 1, "chart_axis_inside_cache_minimum_effective_grain", "1")
        self.data.set_report(1, 8, 1, "chart_scheduler_minimum_effective_grain", "1")
        self.assert_failure("inside_cache minimum_effective_grain=1, expected exact")

    def test_cpu_contention_system_ratio_and_timer_wall_are_gated(self) -> None:
        self.data.matching(1, 1, 1)[0]["system_cpu_s"] = "0.3"
        self.assert_failure("system/user CPU ratio exceeds")
        self.data = Dataset(self.root)
        self.data.matching(1, 8, 1)[0]["user_cpu_s"] = "11"
        self.assert_failure("exceeds worker-count wall-clock capacity")
        self.data = Dataset(self.root)
        self.data.set_timing(1, 1, "180", "1100")
        self.assert_failure("total_ms exceeds runner wall clock")
        self.data = Dataset(self.root)
        self.data.matching(1, 1, 1)[0]["wall_clock_s"] = "100"
        self.assert_failure("wall clock is implausibly larger")
        self.data = Dataset(self.root)
        self.data.matching(1, 8, 1)[0]["user_cpu_s"] = "0.3"
        self.data.matching(1, 8, 1)[0]["system_cpu_s"] = "0.01"
        self.assert_failure("CPU is below 0.5x runner wall clock")

    def test_speed_boundary_exemption_rss_swap_and_semantic_components(self) -> None:
        self.data.set_timing(1, 1, "150", "1000")
        self.data.set_timing(1, 8, "100", "1000")
        self.data.set_timing(7, 1, "90", "1000")
        self.data.set_timing(7, 8, "90", "1000")
        result = self.assert_pass()
        self.assertEqual(result["same_revision"]["timings"]["7"]["speed_gate"], "exempt_below_10_percent")
        self.data.set_timing(1, 8, "101", "1000")
        self.assert_failure("not at least 1.5x faster")
        self.data = Dataset(self.root)
        self.data.set_raw(1, 8, "peak_sampled_rss_kb", "2001")
        self.assert_failure("exceeds 2x W1")
        self.data = Dataset(self.root)
        self.data.matching(1, 8, 1)[0]["peak_sampled_swap_kb"] = "1"
        self.assert_failure("peak sampled swap is nonzero")
        self.data = Dataset(self.root)
        self.data.search[(1, 8)]["chain_sha256"] = digest("wrong-chain")
        self.assert_failure("canonical search component chain_sha256 differs")

    def test_full_canonical_schema_counts_and_duplicate_keys_are_rejected(self) -> None:
        self.data.sidecars[(1, 1)] = self.data.sidecars[(1, 1)].replace(
            b'{"record":"schema"', b'{"record":"schema","record":"schema"', 1
        )
        semantic = hashlib.sha256(self.data.sidecars[(1, 1)]).hexdigest()
        self.data.search[(1, 1)]["semantic_sha256"] = semantic
        for row in self.data.matching(1, 1):
            row["search_semantic_sha256"] = semantic
            row["trial_semantic_sha256"] = acceptance.trial_semantic_digest(row)
            row["canonical_digest"] = row["trial_semantic_sha256"]
        self.assert_failure("duplicate JSON key")

    def test_each_timed_compact_result_is_present_and_untampered(self) -> None:
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        compact, _, _, _ = acceptance.canonical_paths(self.data.raw, row, report)
        compact.unlink()
        self.assert_written_failure("compact canonical result is missing")

        self.data = Dataset(self.root)
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        compact, _, _, _ = acceptance.canonical_paths(self.data.raw, row, report)
        value = json.loads(compact.read_text(encoding="utf-8"))
        value["semantic_sha256"] = digest("tampered timed compact")
        compact.write_text(json.dumps(value) + "\n", encoding="utf-8")
        self.assert_written_failure("compact and full search-digest bytes differ")

        self.data = Dataset(self.root)
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        compact, _, _, _ = acceptance.canonical_paths(self.data.raw, row, report)
        value = json.loads(compact.read_text(encoding="utf-8"))
        value["schema_version"] = 2
        compact.write_text(json.dumps(value) + "\n", encoding="utf-8")
        self.assert_written_failure("schema_version=2, expected 1")

        self.data = Dataset(self.root)
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        compact, _, _, _ = acceptance.canonical_paths(self.data.raw, row, report)
        value = json.loads(compact.read_text(encoding="utf-8"))
        compact.write_text(
            json.dumps(value, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        self.assert_written_failure("compact and full search-digest bytes differ")

    def test_raw_report_path_is_bound_to_the_exact_timed_command_name(self) -> None:
        for row in self.data.rows:
            original = Path(row["report_path"])
            row["report_path"] = os.fspath(
                original.with_name(f"alternate_{original.name}")
            )
        self.assert_failure(
            "report_path is not the exact deterministic Phase-9 timed report path"
        )

    def test_all_commanded_warmup_compacts_are_present_and_semantic(self) -> None:
        self.data.write()
        row = self.data.matching(1, 1)[0]
        warmup = acceptance.expected_phase9_warmup_compact_path(
            self.root, row, 1
        )
        warmup.unlink()
        self.assert_written_failure("warmup1 compact canonical result is missing")

        self.data = Dataset(self.root)
        self.data.write()
        row = self.data.matching(1, 1)[0]
        warmup = acceptance.expected_phase9_warmup_compact_path(
            self.root, row, 1
        )
        value = json.loads(warmup.read_text(encoding="utf-8"))
        value["semantic_sha256"] = digest("tampered warmup")
        warmup.write_text(json.dumps(value) + "\n", encoding="utf-8")
        self.assert_written_failure(
            "differs from the measured row/full canonical search-digest bytes"
        )

        self.data = Dataset(self.root)
        self.data.write()
        row = self.data.matching(1, 1)[0]
        warmup = acceptance.expected_phase9_warmup_compact_path(
            self.root, row, 1
        )
        value = json.loads(warmup.read_text(encoding="utf-8"))
        value["schema_version"] = 2
        warmup.write_text(json.dumps(value) + "\n", encoding="utf-8")
        self.assert_written_failure("schema_version=2, expected 1")

        self.data = Dataset(self.root)
        self.data.write()
        row = self.data.matching(1, 1)[0]
        warmup = acceptance.expected_phase9_warmup_compact_path(
            self.root, row, 1
        )
        value = json.loads(warmup.read_text(encoding="utf-8"))
        warmup.write_text(
            json.dumps(value, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        self.assert_written_failure(
            "differs from the measured row/full canonical search-digest bytes"
        )

    def test_warmup_compacts_reject_aliasing_and_oracle_drift(self) -> None:
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        measured, _, _, _ = acceptance.canonical_paths(
            self.data.raw, row, report
        )
        warmup = acceptance.expected_phase9_warmup_compact_path(
            self.root, row, 1
        )
        warmup.unlink()
        os.link(measured, warmup)
        self.assert_written_failure("compact canonical result is not singly linked")
        warmup.unlink()

        self.data = Dataset(self.root)
        self.data.write()
        _, rows = acceptance.read_tsv(self.data.raw)
        trials = acceptance.select_matrix(self.data.raw, rows)
        oracle = {
            (seed, worker): acceptance.SearchDigestSnapshot(
                path=acceptance.trials_for(trials, seed, worker)[0].compact_path,
                mapping=dict(
                    acceptance.trials_for(trials, seed, worker)[0].search_digest
                ),
                data=acceptance.trials_for(trials, seed, worker)[
                    0
                ].search_digest_bytes,
                identity=acceptance.trials_for(trials, seed, worker)[
                    0
                ].compact_identity,
            )
            for seed in acceptance.SEEDS
            for worker in acceptance.MEASURED_WORKERS
        }
        oracle[(1, 1)].mapping["chain_sha256"] = digest("oracle drift")
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "sealed frozen oracle"
        ):
            acceptance.validate_phase9_warmup_compacts(
                self.root, trials, oracle
            )

        oracle[(1, 1)].mapping["chain_sha256"] = trials[0].search_digest[
            "chain_sha256"
        ]
        oracle[(1, 1)] = acceptance.SearchDigestSnapshot(
            path=oracle[(1, 1)].path,
            mapping=oracle[(1, 1)].mapping,
            data=(
                json.dumps(oracle[(1, 1)].mapping, indent=2, sort_keys=True)
                + "\n"
            ).encode("utf-8"),
            identity=oracle[(1, 1)].identity,
        )
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "sealed frozen oracle bytes"
        ):
            acceptance.validate_phase9_warmup_compacts(
                self.root, trials, oracle
            )

    def test_digest_reader_rejects_path_replacement_during_read(self) -> None:
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        compact, _, _, _ = acceptance.canonical_paths(
            self.data.raw, row, report
        )
        replacement = compact.with_name("replacement.canonical.json")
        replacement.write_bytes(compact.read_bytes())
        real_read = os.read
        replaced = False

        def replacing_read(descriptor: int, count: int) -> bytes:
            nonlocal replaced
            data = real_read(descriptor, count)
            if data and not replaced:
                os.replace(replacement, compact)
                replaced = True
            return data

        with (
            mock.patch.object(os, "read", side_effect=replacing_read),
            self.assertRaisesRegex(
                acceptance.AcceptanceError, "changed while it was read"
            ),
        ):
            acceptance.read_stable_search_digest(
                compact,
                compact.parent,
                "replacement-raced digest",
                require_single_link=True,
            )

        oversized = compact.with_name("oversized.canonical.json")
        oversized.write_bytes(b" " * (acceptance.MAX_SEARCH_DIGEST_BYTES + 1))
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "exceeds 65536 bytes"
        ):
            acceptance.read_stable_search_digest(
                oversized,
                oversized.parent,
                "oversized digest",
                require_single_link=True,
            )

        alias = compact.with_name("alias.canonical.json")
        alias.symlink_to(compact.name)
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "lexical regular file, not an alias"
        ):
            acceptance.read_stable_search_digest(
                alias,
                alias.parent,
                "symlinked digest",
                require_single_link=True,
            )

    def test_timed_compact_results_reject_hard_links_and_path_reuse(self) -> None:
        self.data.write()
        first_row = self.data.matching(1, 1, 1)[0]
        second_row = self.data.matching(1, 1, 2)[0]
        first_report = self.root / first_row["report_path"]
        second_report = self.root / second_row["report_path"]
        first, _, _, _ = acceptance.canonical_paths(
            self.data.raw, first_row, first_report
        )
        second, _, _, _ = acceptance.canonical_paths(
            self.data.raw, second_row, second_report
        )
        second.unlink()
        os.link(first, second)
        self.assert_written_failure("compact canonical result is not singly linked")

        first.unlink()
        second.unlink()
        self.data = Dataset(self.root)
        first_row = self.data.matching(1, 1, 1)[0]
        self.data.matching(1, 1, 2)[0]["report_path"] = first_row["report_path"]
        self.assert_failure(
            "report_path is not the exact deterministic Phase-9 timed report path"
        )

    def test_phase9_command_auditor_requires_exact_timed_compact_paths(self) -> None:
        import wric_phase9_manifest_bootstrap as bootstrap

        primary = self.sandbox / "command-fixture.pb.gz"
        primary.write_bytes(b"synthetic fixture\n")
        dagutil_path = self.sandbox / "dagutil"
        dagutil_path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        dagutil_path.chmod(0o755)
        dagutil = os.fspath(dagutil_path.resolve())
        rows = tuple(
            bootstrap.phase9_contract_row(
                seed,
                worker,
                digest("command fixture"),
                "manifest://fixture.pb.gz",
                "0",
            )
            for seed in acceptance.SEEDS
            for worker in acceptance.MEASURED_WORKERS
        )
        audited = argparse.Namespace(
            supplement=argparse.Namespace(
                path=self.sandbox / "phase9-local-commit.tsv",
                rows=rows,
            )
        )
        entries: list[tuple[str, tuple[str, ...]]] = []
        first_timed_compact: Path | None = None
        for row in rows:
            worker = int(row["requested_workers"])
            fixture = row["workload_name"]
            row_id = row["row_id"]
            safe = acceptance.sanitize_harness_name(fixture)
            row_safe = acceptance.sanitize_harness_name(row_id)
            prefix = (
                dagutil,
                "--dag-pb",
                os.fspath(primary),
                *acceptance.option_tokens(row),
            )
            timed_prefix = (*prefix, "--chart-spr-workers", str(worker))
            for suffix in ("warmup1", "trial1", "trial2", "trial3"):
                execution_suffix = f"{suffix}_{row_safe}"
                compact = (
                    self.root
                    / "logs"
                    / f"{safe}_{acceptance.METHOD}_{execution_suffix}_w{worker}.canonical.json"
                )
                if first_timed_compact is None:
                    first_timed_compact = compact
                output = (
                    self.root
                    / "outputs"
                    / f"{safe}_{acceptance.METHOD}_{execution_suffix}_w{worker}.pb.gz"
                )
                entries.append(
                    (
                        f"{fixture} {acceptance.METHOD} workers={worker} {execution_suffix}",
                        (
                            *timed_prefix,
                            "--chart-spr-canonical-result",
                            os.fspath(compact),
                            "-o",
                            os.fspath(output),
                        ),
                    )
                )
            companion_stem = f"{safe}_{row_safe}_canonical_companion"
            entries.append(
                (
                    f"{fixture} {acceptance.METHOD} deferred semantic companion workers={worker}",
                    (
                        *timed_prefix,
                        "--chart-spr-canonical-result",
                        os.fspath(self.root / "logs" / f"{companion_stem}.json"),
                        "-o",
                        os.fspath(self.root / "outputs" / f"{companion_stem}.pb.gz"),
                    ),
                )
            )
            full_stem = f"{safe}_{row_safe}_full_canonical"
            entries.append(
                (
                    f"{fixture} {acceptance.METHOD} deferred explicit-W1 full correctness",
                    (
                        *prefix,
                        "--chart-spr-workers",
                        "1",
                        "--chart-spr-canonical-result",
                        os.fspath(self.root / "logs" / f"{full_stem}.json"),
                        "--chart-spr-canonical-sidecar",
                        os.fspath(self.root / "logs" / f"{full_stem}.ndjson"),
                        "-o",
                        os.fspath(self.root / "outputs" / f"{full_stem}.pb.gz"),
                    ),
                )
            )
        commands = self.sandbox / "phase9-commands.sh"

        def write_commands(values: list[tuple[str, tuple[str, ...]]]) -> None:
            lines: list[str] = []
            for label, tokens in values:
                lines.extend((f"# {label}", shlex.join(tokens)))
            commands.write_text("\n".join(lines) + "\n", encoding="utf-8")

        write_commands(entries)
        with (
            mock.patch.object(
                acceptance,
                "production_paths",
                return_value=(dagutil_path, dagutil_path, dagutil_path),
            ),
            mock.patch.object(
                bootstrap, "resolve_manifest_uri", return_value=primary
            ),
        ):
            contract_sha = acceptance.validate_phase9_commands(
                self.root,
                commands,
                audited,
                self.sandbox,
                self.sandbox,
            )
            self.assertRegex(contract_sha, r"^[0-9a-f]{64}$")
            assert first_timed_compact is not None
            bad_entries = list(entries)
            label, tokens = bad_entries[0]
            bad_entries[0] = (
                label,
                tuple(
                    os.fspath(self.sandbox / "wrong.json")
                    if token == os.fspath(first_timed_compact)
                    else token
                    for token in tokens
                ),
            )
            write_commands(bad_entries)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "lacks exactly one production command",
            ):
                acceptance.validate_phase9_commands(
                    self.root,
                    commands,
                    audited,
                    self.sandbox,
                    self.sandbox,
                )

    def test_nonfinite_json_and_ndjson_constants_are_rejected(self) -> None:
        self.data.search[(1, 1)]["record_count"] = float("nan")
        self.assert_failure("non-finite JSON constant 'NaN'")
        self.data = Dataset(self.root)
        payload = self.data.sidecars[(1, 1)].replace(
            b'"valid":true', b'"poison":NaN,"valid":true', 1
        )
        self.data.sidecars[(1, 1)] = payload
        semantic = hashlib.sha256(payload).hexdigest()
        self.data.search[(1, 1)]["semantic_sha256"] = semantic
        for row in self.data.matching(1, 1):
            row["search_semantic_sha256"] = semantic
            row["trial_semantic_sha256"] = acceptance.trial_semantic_digest(row)
            row["canonical_digest"] = row["trial_semantic_sha256"]
        self.assert_failure("non-finite JSON constant 'NaN'")

    def test_output_publication_is_exclusive_non_aliasing_and_outside_evidence(self) -> None:
        output_dir = self.sandbox / "results"
        output_dir.mkdir()
        json_path = output_dir / "acceptance.json"
        markdown_path = output_dir / "acceptance.md"
        self.assert_pass(extra=["--json-output", str(json_path), "--markdown-output", str(markdown_path)])
        first = json_path.read_bytes()
        completed, _ = self.run_tool(extra=["--json-output", str(json_path)])
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(json_path.read_bytes(), first)
        alias = output_dir / "alias"
        completed, _ = self.run_tool(
            extra=["--json-output", str(alias), "--markdown-output", str(alias)]
        )
        self.assertEqual(completed.returncode, 2)
        completed, _ = self.run_tool(extra=["--json-output", str(self.root / "inside.json")])
        self.assertEqual(completed.returncode, 2)
        with mock.patch.object(
            acceptance.tempfile, "mkstemp", side_effect=OSError("injected stage failure")
        ):
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "cannot create temporary JSON output"
            ):
                acceptance.publish_outputs_exclusive(
                    ((output_dir / "fresh.json", b"{}\n", "JSON output"),)
                )
        exclusive = output_dir / "exclusive.json"
        with mock.patch.object(
            acceptance,
            "fsync_directory",
            side_effect=acceptance.AcceptanceError("injected directory sync failure"),
        ):
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "injected directory sync failure"
            ):
                acceptance.exclusive_write(exclusive, b"{}\n", "exclusive test")
        self.assertFalse(exclusive.exists())

    def test_recorded_artifact_symlink_is_rejected(self) -> None:
        self.data.write()
        row = self.data.matching(1, 1, 1)[0]
        report = self.root / row["report_path"]
        real = report.with_suffix(".real")
        report.rename(real)
        report.symlink_to(real.name)
        completed = subprocess.run(self.command(), text=True, capture_output=True, check=False)
        result = json.loads(completed.stdout)
        self.assertEqual(completed.returncode, 1)
        self.assertIn("symlink or noncanonical lexical path", result["failure"])

    def test_final_mode_rejects_ad_hoc_or_partial_provenance(self) -> None:
        self.data.write()
        completed = subprocess.run(
            [sys.executable, str(TOOL), "evaluate", "--benchmark-dir", str(self.root)],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("final evaluation requires", completed.stderr)
        command = self.command(ad_hoc=True)
        command.extend(("--base-manifest", "partial"))
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("must not mix in partial sealed provenance", completed.stderr)
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "evaluate",
                "--benchmark-dir",
                str(self.root),
                "--base-manifest",
                "base",
                "--expected-parent-sha256",
                "0" * 64,
                "--supplement",
                "supplement",
                "--repo-root",
                str(REPO),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("--expected-run-ledger-sha256", completed.stderr)

    def test_clean_git_checkout_rejects_untracked_and_revision_drift(self) -> None:
        checkout = self.sandbox / "checkout"
        checkout.mkdir()
        subprocess.run(["git", "init", "-q", str(checkout)], check=True)
        subprocess.run(
            ["git", "-C", str(checkout), "config", "user.email", "phase9@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(checkout), "config", "user.name", "Phase 9 test"],
            check=True,
        )
        tracked = checkout / "tracked.txt"
        tracked.write_text("tracked\n")
        subprocess.run(["git", "-C", str(checkout), "add", "tracked.txt"], check=True)
        subprocess.run(
            ["git", "-C", str(checkout), "commit", "-q", "-m", "fixture"], check=True
        )
        head = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
        self.assertEqual(acceptance.validate_clean_git_checkout(checkout, head), head)
        self.assertEqual(
            acceptance.validate_base_repo_root(checkout, head), checkout
        )
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "must be this acceptance checkout"
        ):
            acceptance.validate_working_repo_root(checkout, head)
        with self.assertRaisesRegex(acceptance.AcceptanceError, "does not equal live Git HEAD"):
            acceptance.validate_clean_git_checkout(checkout, "0" * 40)
        checkout_alias = self.sandbox / "checkout-alias"
        checkout_alias.symlink_to(checkout, target_is_directory=True)
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "canonical, non-symlink directory"
        ):
            acceptance.validate_base_repo_root(checkout_alias, head)
        subdirectory = checkout / "subdirectory"
        subdirectory.mkdir()
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "not exact Git toplevel"
        ):
            acceptance.validate_base_repo_root(subdirectory, head)
        (checkout / "untracked.txt").write_text("untracked\n")
        self.assertEqual(
            acceptance.validate_base_repo_root(checkout, head), checkout
        )
        with self.assertRaisesRegex(acceptance.AcceptanceError, "not completely clean"):
            acceptance.validate_clean_git_checkout(checkout, head)

    def test_production_tool_identity_is_exact(self) -> None:
        fake = self.sandbox / "fake-harness"
        fake.write_text("#!/bin/sh\nexit 0\n")
        fake.chmod(0o755)
        with self.assertRaisesRegex(acceptance.AcceptanceError, "exact production executable"):
            acceptance.require_exact_production_tool(
                str(fake), acceptance.PRODUCTION_HARNESS, "benchmark harness"
            )

    def test_command_option_contract_matches_sealed_bootstrap_schema(self) -> None:
        import wric_phase9_manifest_bootstrap as bootstrap

        row = bootstrap.phase9_contract_row(
            1,
            8,
            digest("fixture"),
            "manifest://fixture.pb.gz",
            "0,2,4,6,8,10,12,14",
        )
        expected = [
            "@binary:working_chart",
            "--dag-pb",
            f"@primary:{row['primary_sha256']}",
            *acceptance.option_tokens(row),
            "--chart-spr-workers",
            "8",
            "--chart-spr-canonical-result",
            "@search-canonical-result",
            "-o",
            "@output",
        ]
        self.assertEqual(bootstrap.canonical_argv(row), expected)

    def test_run_ledger_detects_mutation_extra_file_and_seal_tamper(self) -> None:
        self.data.write()
        base_checkout = self.sandbox / "sealed-base-checkout"
        base_checkout.mkdir()
        subprocess.run(["git", "init", "-q", str(base_checkout)], check=True)
        subprocess.run(
            ["git", "-C", str(base_checkout), "config", "user.email", "phase9@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(base_checkout), "config", "user.name", "Phase 9 test"],
            check=True,
        )
        (base_checkout / "sealed.txt").write_text("sealed base\n")
        subprocess.run(
            ["git", "-C", str(base_checkout), "add", "sealed.txt"], check=True
        )
        subprocess.run(
            ["git", "-C", str(base_checkout), "commit", "-q", "-m", "sealed base"],
            check=True,
        )
        base_head = subprocess.run(
            ["git", "-C", str(base_checkout), "rev-parse", "HEAD"],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
        row_ids = sorted({row["row_id"] for row in self.data.rows})
        summary = self.root / "summary.md"
        summary.write_text(
            "# benchmark\nConfiguration: workers=1 8, warmups=1, repetitions=3, timeout_seconds=600.\n"
        )
        commands = self.root / "commands.sh"
        commands.write_text(
            "\n".join(
                f"run {suffix}_{row_id}"
                for row_id in row_ids
                for suffix in ("warmup1", "trial1", "trial2", "trial3")
            )
            + "\n"
        )
        metadata = {
            key: "0" * 64
            for key in acceptance.RUN_METADATA_KEYS
            if key.endswith("sha256")
        }
        metadata.update(
            {
                "schema": acceptance.RUN_SCHEMA,
                "schema_version": acceptance.RUN_SCHEMA_VERSION,
                "role": "same_revision_phase9_current_measurement",
                "run_group": acceptance.RUN_GROUP,
                "seeds": list(acceptance.SEEDS),
                "workers": list(acceptance.MEASURED_WORKERS),
                "repetitions": 3,
                "warmups_per_row": 1,
                "full_canonical": True,
                "affinity_cpus": "0",
                "fixture_sha256": digest("input"),
                "base_repo_root": str(base_checkout),
                "base_revision": base_head,
                "working_repo_root": str(REPO),
                "working_revision": "a" * 40,
                "repository_status": "clean",
                "binary_provenance_limit": (
                    "binary hashes bind the measured executables, but no reproducible-build "
                    "attestation proves derivation from HEAD"
                ),
                "working_larch2_sha256": acceptance.sha256_file(
                    acceptance.PRODUCTION_LARCH2, "larch2"
                ),
                "working_dagutil_sha256": acceptance.sha256_file(
                    acceptance.PRODUCTION_DAGUTIL, "dagutil"
                ),
                "benchmark_harness_sha256": acceptance.sha256_file(
                    acceptance.PRODUCTION_HARNESS, "harness"
                ),
                "raw_trials_sha256": acceptance.sha256_file(self.data.raw, "raw"),
                "summary_sha256": acceptance.sha256_file(summary, "summary"),
                "commands_sha256": acceptance.sha256_file(commands, "commands"),
                "command_contract_sha256": digest("command-contract"),
                "raw_trial_rows": 18,
                "row_ids": row_ids,
            }
        )
        metadata_path = self.root / acceptance.RUN_METADATA_NAME
        metadata_path.write_text(json.dumps(metadata, sort_keys=True) + "\n")
        members = acceptance.run_archive_members(self.root)
        ledger = self.root / acceptance.RUN_LEDGER_NAME
        seal = self.root / acceptance.RUN_LEDGER_SEAL_NAME
        ledger.write_bytes(acceptance.render_run_ledger(self.root, members))
        seal.write_bytes(acceptance.detached_seal_payload(ledger))

        class Stub:
            pass

        audited = Stub()
        audited.base = Stub()
        audited.base.sha256 = "0" * 64
        audited.base.preamble = {"repo_revision": base_head}
        audited.supplement = Stub()
        audited.supplement.sha256 = "0" * 64
        audited.supplement.rows = ({"primary_sha256": digest("input")},)
        audited.characterization = Stub()
        audited.characterization.sha256 = "0" * 64
        audited.characterization.preamble = {"affinity_cpus": "0"}
        ledger_sha = acceptance.sha256_file(ledger, "ledger")
        configuration = {
            "summary_sha256": metadata["summary_sha256"],
            "commands_sha256": metadata["commands_sha256"],
            "command_contract_sha256": metadata["command_contract_sha256"],
        }
        with self.assertRaisesRegex(acceptance.AcceptanceError, "external expected anchor"):
            acceptance.audit_run_archive(
                str(self.root), audited, str(base_checkout), str(REPO), "f" * 64
            )
        with (
            mock.patch.object(
                acceptance, "validate_clean_git_checkout", return_value="a" * 40
            ),
            mock.patch.object(
                acceptance, "validate_run_configuration", return_value=configuration
            ),
        ):
            acceptance.audit_run_archive(
                str(self.root), audited, str(base_checkout), str(REPO), ledger_sha
            )
            metadata["base_repo_root"] = str(REPO)
            metadata_path.write_text(json.dumps(metadata, sort_keys=True) + "\n")
            ledger.write_bytes(
                acceptance.render_run_ledger(
                    self.root, acceptance.run_archive_members(self.root)
                )
            )
            seal.write_bytes(acceptance.detached_seal_payload(ledger))
            self_anchored_sha = acceptance.sha256_file(ledger, "ledger")
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "run metadata base_repo_root"
            ):
                acceptance.audit_run_archive(
                    str(self.root),
                    audited,
                    str(base_checkout),
                    str(REPO),
                    self_anchored_sha,
                )
            metadata["base_repo_root"] = str(base_checkout)
            metadata_path.write_text(json.dumps(metadata, sort_keys=True) + "\n")
            ledger.write_bytes(
                acceptance.render_run_ledger(
                    self.root, acceptance.run_archive_members(self.root)
                )
            )
            seal.write_bytes(acceptance.detached_seal_payload(ledger))
            ledger_sha = acceptance.sha256_file(ledger, "ledger")
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "sealed base repository root revision"
            ):
                acceptance.audit_run_archive(
                    str(self.root), audited, str(REPO), str(base_checkout), ledger_sha
                )
            base_alias = self.sandbox / "sealed-base-alias"
            base_alias.symlink_to(base_checkout, target_is_directory=True)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "canonical, non-symlink directory"
            ):
                acceptance.audit_run_archive(
                    str(self.root), audited, str(base_alias), str(REPO), ledger_sha
                )
            (self.root / "extra.txt").write_text("extra\n")
            with self.assertRaisesRegex(acceptance.AcceptanceError, "exact archive closure"):
                acceptance.audit_run_archive(
                    str(self.root), audited, str(base_checkout), str(REPO), ledger_sha
                )
            (self.root / "extra.txt").unlink()
            seal.write_text("bad seal\n")
            with self.assertRaisesRegex(acceptance.AcceptanceError, "detached seal mismatch"):
                acceptance.audit_run_archive(
                    str(self.root), audited, str(base_checkout), str(REPO), ledger_sha
                )


if __name__ == "__main__":
    unittest.main()
