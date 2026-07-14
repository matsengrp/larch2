#!/usr/bin/env python3
"""Deterministic positive/negative fixtures for wric_phase4_acceptance.py."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "wric_phase4_acceptance.py"
sys.path.insert(0, str(TOOL.parent))
import wric_phase4_acceptance as acceptance  # noqa: E402


HEX = {
    "input": "1" * 64,
    "refseq": "2" * 64,
    "search": "3" * 64,
    "output": "4" * 64,
    "trial": "5" * 64,
    "argv": "6" * 64,
}


class Dataset:
    def __init__(self, root: Path, repetitions: int = 3) -> None:
        self.root = root
        self.repetitions = repetitions
        self.raw = root / "raw_trials.tsv"
        self.rows: list[dict[str, str]] = []
        self.reports: dict[str, list[str]] = {}
        for workload, prefix in (
            ("local", acceptance.DEFAULT_LOCAL_PREFIX),
            ("construction", acceptance.DEFAULT_CONSTRUCTION_PREFIX),
        ):
            for worker in acceptance.WORKERS:
                for trial in range(1, repetitions + 1):
                    self._add(workload, prefix, worker, trial)

    @staticmethod
    def timing(workload: str, worker: int) -> tuple[str, str]:
        local = {1: "100", 2: "75", 4: "60", 8: "50"}[worker]
        construction = {1: "200", 2: "140", 4: "100", 8: "100"}[worker]
        return (local, "25") if workload == "local" else ("10", construction)

    @staticmethod
    def axis_values(workload: str, worker: int) -> dict[str, dict[str, int]]:
        values = {
            axis: {field: 0 for field in acceptance.AXIS_FIELDS}
            for axis in acceptance.AXES
        }
        active = (
            ("initial_chart", "local_candidate")
            if workload == "local"
            else ("initial_chart", "inside_cache", "outside_cache")
        )
        for axis in active:
            ranges = 1 if worker == 1 else worker
            values[axis] = {
                "operations": 1,
                "items": 64,
                "ranges": ranges,
                "tasks": 0 if worker == 1 else ranges,
                "active_worker_high_water": 1 if worker == 1 else worker,
            }
        return values

    def _add(self, workload: str, prefix: str, worker: int, trial: int) -> None:
        local_ms, construction_ms = self.timing(workload, worker)
        row_id = f"{prefix}{worker}"
        report_name = f"reports/{workload}-w{worker}-t{trial}.out"
        rss = "2000" if worker == 8 else ("1000" if worker == 1 else "1200")
        row = {column: "0" for column in acceptance.REQUIRED_COLUMNS}
        row.update(
            {
                "row_id": row_id,
                "fixture": "medium.pb.gz",
                "method": "chart_spr_grammar_lower_bound_heuristic",
                "status": "ok",
                "validation_status": "ok",
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
                "user_cpu_s": "4",
                "system_cpu_s": "1",
                "wall_clock_s": "5",
                "peak_sampled_rss_kb": rss,
                "peak_sampled_swap_kb": "0",
                "process_rss_limit_bytes": str(32 * 1024**3),
                "configured_chart_memory_budget": str(12 * 1024**3),
                "manifest_rss_limit_bytes": str(32 * 1024**3),
                "input_sha256": HEX["input"],
                "refseq_sha256": HEX["refseq"],
                "search_semantic_sha256": HEX["search"],
                "output_semantic_sha256": HEX["output"],
                "trial_semantic_sha256": HEX["trial"],
                "canonical_argv_sha256": HEX["argv"],
                "canonical_digest": HEX["trial"],
                "local_scoring_ms": local_ms,
                "initial_chart_construction_ms": construction_ms,
                "local_inside_cache_initialization_ms": "0",
                "local_outside_cache_initialization_ms": "0",
                "report_path": report_name,
            }
        )
        self.rows.append(row)

        axes = self.axis_values(workload, worker)
        operations = sum(values["operations"] for values in axes.values())
        ranges = sum(values["ranges"] for values in axes.values())
        tasks = sum(values["tasks"] for values in axes.values())
        hwm = max(values["active_worker_high_water"] for values in axes.values())
        report = {
            "local_scoring_ms": local_ms,
            "initial_chart_construction_ms": construction_ms,
            "local_inside_cache_initialization_ms": "0",
            "local_outside_cache_initialization_ms": "0",
            "chart_workers_requested": str(worker),
            "chart_workers_resolved": str(worker),
            "chart_worker_policy": "explicit",
            "chart_worker_resolution_policy": "explicit",
            "chart_workers_actually_active_high_water": str(hwm),
            "chart_scheduler_operations": str(operations),
            "chart_scheduler_parallel_operations": str(0 if worker == 1 else operations),
            "chart_scheduler_serial_fallbacks": str(operations if worker == 1 else 0),
            "chart_scheduler_ranges_created": str(ranges),
            "chart_scheduler_ranges_completed": str(ranges),
            "chart_scheduler_ranges_cancelled": "0",
            "chart_scheduler_tasks_submitted": str(tasks),
            "chart_scheduler_tasks_completed": str(tasks),
            "chart_scheduler_tasks_joined": str(tasks),
            "chart_scheduler_queue_wait_samples": str(tasks),
            "chart_scheduler_queue_wait_nanoseconds": str(tasks * 10),
            "chart_scheduler_queue_wait_nanoseconds_max": "0" if tasks == 0 else "10",
            "chart_scheduler_last_active_workers": str(hwm),
            "chart_scheduler_pending_tasks": "0",
            "chart_scheduler_pending_tasks_at_shutdown": "0",
            "chart_scheduler_nested_serial_fallbacks": "0",
            "chart_scheduler_rejected_concurrent_operations": "0",
            "chart_scheduler_pool_lifetimes": "0" if worker == 1 else "1",
            "chart_scheduler_pool_lifetimes_stopped": "0" if worker == 1 else "1",
            "chart_scheduler_live_pool_threads": "0",
            "chart_scheduler_minimum_effective_grain": "1",
            "chart_scheduler_maximum_effective_grain": "1",
            "chart_scheduler_shutdown": "true",
        }
        for axis, values in axes.items():
            for field, value in values.items():
                report[f"chart_axis_{axis}_{field}"] = str(value)
            operations = values["operations"]
            report[f"chart_axis_{axis}_parallel_operations"] = str(
                0 if worker == 1 else operations
            )
            report[f"chart_axis_{axis}_minimum_effective_grain"] = str(
                1 if operations else 0
            )
            report[f"chart_axis_{axis}_maximum_effective_grain"] = str(
                1 if operations else 0
            )
        self.reports[report_name] = [
            "chart_spr_search:",
            *(f"  {key}: {value}" for key, value in report.items()),
        ]

    def matching(self, workload: str, worker: int | None = None) -> list[dict[str, str]]:
        prefix = (
            acceptance.DEFAULT_LOCAL_PREFIX
            if workload == "local"
            else acceptance.DEFAULT_CONSTRUCTION_PREFIX
        )
        result = [row for row in self.rows if row["row_id"].startswith(prefix)]
        if worker is not None:
            result = [row for row in result if row["requested_workers"] == str(worker)]
        return result

    def report_dict(self, row: dict[str, str]) -> dict[str, str]:
        result: dict[str, str] = {}
        for line in self.reports[row["report_path"]][1:]:
            key, value = line.strip().split(": ", 1)
            result[key] = value
        return result

    def set_report(self, row: dict[str, str], key: str, value: str) -> None:
        lines = self.reports[row["report_path"]]
        prefix = f"  {key}: "
        for index, line in enumerate(lines):
            if line.startswith(prefix):
                lines[index] = prefix + value
                return
        raise AssertionError(f"report key not found: {key}")

    def set_timing(self, workload: str, worker: int, field: str, value: str) -> None:
        for row in self.matching(workload, worker):
            row[field] = value
            self.set_report(row, field, value)

    def set_rss(self, workload: str, worker: int, value: int) -> None:
        for row in self.matching(workload, worker):
            row["peak_sampled_rss_kb"] = str(value)
            row["process_rss_limit_bytes"] = str(max(value * 2048, 1))
            row["manifest_rss_limit_bytes"] = str(max(value * 2048, 1))

    def write(self, *, columns: list[str] | None = None) -> None:
        for name, lines in self.reports.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        header = columns or list(acceptance.REQUIRED_COLUMNS)
        with self.raw.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=header,
                delimiter="\t",
                lineterminator="\n",
                extrasaction="ignore",
            )
            writer.writeheader()
            writer.writerows(self.rows)


class Phase4AcceptanceTest(unittest.TestCase):
    maxDiff = None

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="wric-phase4-acceptance-")
        self.root = Path(self.temporary.name)
        self.data = Dataset(self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_tool(
        self,
        *,
        baseline: Path | None = None,
        defer: bool = True,
        physical_memory: int = 64 * 1024**3,
        extra: list[str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, object] | None]:
        self.data.write()
        command = [
            sys.executable,
            str(TOOL),
            "--raw-trials",
            str(self.data.raw),
            "--repetitions",
            str(self.data.repetitions),
            "--physical-memory-bytes",
            str(physical_memory),
        ]
        if baseline is not None:
            command.extend(("--phase3-raw-trials", str(baseline)))
        elif defer:
            command.extend(("--defer-phase3-baseline", "Phase 0 capture pending"))
        if extra:
            command.extend(extra)
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        try:
            parsed = json.loads(completed.stdout)
        except json.JSONDecodeError:
            parsed = None
        return completed, parsed

    def assert_pass(self, **kwargs: object) -> dict[str, object]:
        completed, result = self.run_tool(**kwargs)
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        assert result is not None
        return result

    def assert_failure(self, expected: str, **kwargs: object) -> dict[str, object]:
        completed, result = self.run_tool(**kwargs)
        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        assert result is not None
        self.assertIn(expected, str(result.get("failure", "")))
        return result

    def make_phase3(self, local_ms: str = "100") -> Path:
        baseline_root = self.root / "phase3"
        baseline = Dataset(baseline_root, repetitions=self.data.repetitions)
        baseline.set_timing("local", 1, "local_scoring_ms", local_ms)
        baseline.write()
        return baseline.raw

    def test_all_exact_boundaries_pass_with_explicit_deferral(self) -> None:
        # Both local and construction have W8/W1 == 0.50 and W8/W4 == 1.10.
        self.data.set_timing("local", 1, "local_scoring_ms", "220")
        self.data.set_timing("local", 4, "local_scoring_ms", "100")
        self.data.set_timing("local", 8, "local_scoring_ms", "110")
        self.data.set_timing("construction", 1, "initial_chart_construction_ms", "220")
        self.data.set_timing("construction", 4, "initial_chart_construction_ms", "100")
        self.data.set_timing("construction", 8, "initial_chart_construction_ms", "110")
        result = self.assert_pass()
        self.assertEqual(result["status"], "deferred_baseline")
        self.assertEqual(result["phase3_baseline"]["status"], "deferred")

    def test_phase3_exact_105_percent_boundary_passes(self) -> None:
        baseline = self.make_phase3("100")
        self.data.set_timing("local", 1, "local_scoring_ms", "105")
        self.data.set_timing("local", 8, "local_scoring_ms", "52.5")
        result = self.assert_pass(baseline=baseline, defer=False)
        self.assertEqual(result["status"], "pass")

    def test_phase3_just_over_boundary_fails(self) -> None:
        baseline = self.make_phase3("100")
        self.data.set_timing("local", 1, "local_scoring_ms", "105.001")
        self.data.set_timing("local", 8, "local_scoring_ms", "50")
        self.assert_failure("local_w1_over_phase3_w1", baseline=baseline, defer=False)

    def test_missing_baseline_or_explicit_deferral_is_usage_error(self) -> None:
        completed, _ = self.run_tool(defer=False)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("one of the arguments", completed.stderr)

    def test_local_scaling_just_over_boundary_fails(self) -> None:
        self.data.set_timing("local", 8, "local_scoring_ms", "50.001")
        self.assert_failure("local_w8_over_w1")

    def test_local_w8_w4_just_over_boundary_fails(self) -> None:
        self.data.set_timing("local", 1, "local_scoring_ms", "240")
        self.data.set_timing("local", 4, "local_scoring_ms", "100")
        self.data.set_timing("local", 8, "local_scoring_ms", "110.001")
        self.assert_failure("local_w8_over_w4")

    def test_construction_w8_w1_just_over_boundary_fails(self) -> None:
        self.data.set_timing("construction", 8, "initial_chart_construction_ms", "100.001")
        self.assert_failure("construction_w8_over_w1")

    def test_construction_w8_w4_just_over_boundary_fails(self) -> None:
        self.data.set_timing("construction", 1, "initial_chart_construction_ms", "240")
        self.data.set_timing("construction", 4, "initial_chart_construction_ms", "100")
        self.data.set_timing("construction", 8, "initial_chart_construction_ms", "110.001")
        self.assert_failure("construction_w8_over_w4")

    def test_deferred_cache_components_define_construction_span(self) -> None:
        for worker in acceptance.WORKERS:
            self.data.set_timing(
                "construction", worker, "initial_chart_construction_ms", "0"
            )
        for worker, inside, outside in (
            (1, "120", "80"),
            (2, "80", "60"),
            (4, "55", "45"),
            (8, "55", "45"),
        ):
            self.data.set_timing(
                "construction",
                worker,
                "local_inside_cache_initialization_ms",
                inside,
            )
            self.data.set_timing(
                "construction",
                worker,
                "local_outside_cache_initialization_ms",
                outside,
            )
        result = self.assert_pass()
        self.assertEqual(
            result["workloads"]["construction"]["component_fields"],
            [
                "initial_chart_construction_ms",
                "local_inside_cache_initialization_ms",
                "local_outside_cache_initialization_ms",
            ],
        )

    def test_system_user_exact_boundary_passes_and_just_over_blocks(self) -> None:
        self.assert_pass()
        row = self.data.matching("local", 8)[0]
        row["system_cpu_s"] = "1.0001"
        result = self.assert_failure("system/user CPU ratio")
        self.assertEqual(result["status"], "profiling_required")

    def test_rss_exact_two_x_boundary_passes_and_just_over_fails(self) -> None:
        self.assert_pass()
        self.data.set_rss("local", 8, 2001)
        self.assert_failure("exceeds 2x W1")

    def test_rss_global_cap_boundary_passes_and_just_over_fails(self) -> None:
        # 8 MiB physical => 2 MiB == 2048 KiB global cap.
        self.data.set_rss("local", 1, 1024)
        self.data.set_rss("local", 8, 2048)
        self.data.set_rss("construction", 1, 1024)
        self.data.set_rss("construction", 8, 2048)
        self.assert_pass(physical_memory=8 * 1024**2)
        self.data.set_rss("local", 1, 2048)
        self.data.set_rss("local", 8, 2049)
        self.assert_failure("exceeds global cap", physical_memory=8 * 1024**2)

    def test_nonzero_swap_fails(self) -> None:
        self.data.matching("local", 8)[0]["peak_sampled_swap_kb"] = "1"
        self.assert_failure("peak sampled swap is nonzero")

    def test_incomplete_worker_matrix_fails(self) -> None:
        doomed = self.data.matching("local", 4)[0]
        self.data.rows.remove(doomed)
        self.assert_failure("expected 3 trials, found 2")

    def test_duplicate_trial_index_fails(self) -> None:
        rows = self.data.matching("local", 2)
        rows[1]["trial_index"] = rows[0]["trial_index"]
        self.assert_failure("trial indexes are not exactly")

    def test_unexpected_row_under_workload_prefix_fails(self) -> None:
        extra = dict(self.data.matching("local", 1)[0])
        extra["row_id"] = acceptance.DEFAULT_LOCAL_PREFIX + "16"
        self.data.rows.append(extra)
        self.assert_failure("unexpected rows under prefix")

    def test_malformed_and_mismatched_hashes_fail(self) -> None:
        self.data.matching("local", 1)[0]["input_sha256"] = "ABC"
        self.assert_failure("is not a lowercase SHA-256")

        self.data = Dataset(self.root)
        self.data.matching("local", 2)[0]["output_semantic_sha256"] = "a" * 64
        self.assert_failure("canonical semantics differ")

        self.data = Dataset(self.root)
        self.data.matching("local", 1)[0]["canonical_digest"] = "b" * 64
        self.assert_failure("canonical/trial semantic digests differ")

        self.data = Dataset(self.root)
        self.data.matching("local", 1)[0]["canonical_argv_sha256"] = "c" * 64
        self.assert_failure("canonical_argv_sha256 changed across repeated trials")

    def test_missing_or_malformed_report_fails(self) -> None:
        row = self.data.matching("local", 1)[0]
        self.data.reports.pop(row["report_path"])
        self.assert_failure("cannot read report_path")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 1)[0]
        self.data.reports[row["report_path"]] = ["not_the_search:", "  x: 1"]
        self.assert_failure("missing chart_spr_search section")

    def test_missing_and_duplicate_report_keys_fail(self) -> None:
        row = self.data.matching("local", 1)[0]
        lines = self.data.reports[row["report_path"]]
        lines[:] = [line for line in lines if "chart_scheduler_operations:" not in line]
        self.assert_failure("report lacks chart_scheduler_operations")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 1)[0]
        self.data.reports[row["report_path"]].append("  local_scoring_ms: 100")
        self.assert_failure("duplicate top-level report key local_scoring_ms")

    def test_scheduler_operation_range_and_task_accounting_fail_closed(self) -> None:
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_scheduler_operations", "3")
        self.assert_failure("operation accounting does not reconcile")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_scheduler_ranges_completed", "15")
        self.assert_failure("range accounting does not reconcile")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_scheduler_tasks_joined", "15")
        self.assert_failure("task accounting does not reconcile")

    def test_axis_reconciliation_and_other_bucket_fail_closed(self) -> None:
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_scheduler_operations", "3")
        self.data.set_report(row, "chart_scheduler_parallel_operations", "3")
        self.assert_failure("axis/global operation totals do not reconcile")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 8)[0]
        changes = {
            "chart_axis_other_operations": "1",
            "chart_axis_other_items": "1",
            "chart_axis_other_ranges": "1",
            "chart_axis_other_tasks": "1",
            "chart_axis_other_active_worker_high_water": "1",
            "chart_axis_other_parallel_operations": "1",
            "chart_axis_other_minimum_effective_grain": "1",
            "chart_axis_other_maximum_effective_grain": "1",
            "chart_scheduler_operations": "3",
            "chart_scheduler_parallel_operations": "3",
            "chart_scheduler_ranges_created": "17",
            "chart_scheduler_ranges_completed": "17",
            "chart_scheduler_tasks_submitted": "17",
            "chart_scheduler_tasks_completed": "17",
            "chart_scheduler_tasks_joined": "17",
            "chart_scheduler_queue_wait_samples": "17",
        }
        for key, value in changes.items():
            self.data.set_report(row, key, value)
        self.assert_failure("uncategorized scheduler work is nonzero")

    def test_axis_parallel_and_grain_diagnostics_fail_closed(self) -> None:
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_axis_local_candidate_parallel_operations", "0")
        self.assert_failure("parallel-operation totals do not reconcile")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_axis_local_candidate_minimum_effective_grain", "2")
        self.assert_failure("invalid local_candidate effective-grain extrema")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 8)[0]
        self.data.set_report(row, "chart_scheduler_maximum_effective_grain", "2")
        self.assert_failure("axis/global effective-grain extrema disagree")

    def test_parallel_high_water_cannot_silently_fall_back(self) -> None:
        for row in self.data.matching("local", 8):
            self.data.set_report(row, "chart_workers_actually_active_high_water", "1")
            self.data.set_report(row, "chart_scheduler_last_active_workers", "1")
            self.data.set_report(row, "chart_axis_initial_chart_active_worker_high_water", "1")
            self.data.set_report(row, "chart_axis_local_candidate_active_worker_high_water", "1")
        self.assert_failure("expected real multi-worker execution")

    def test_missing_column_empty_field_and_malformed_number_fail(self) -> None:
        columns = list(acceptance.REQUIRED_COLUMNS)
        columns.remove("report_path")
        self.data.write(columns=columns)
        command = [
            sys.executable,
            str(TOOL),
            "--raw-trials",
            str(self.data.raw),
            "--repetitions",
            "3",
            "--defer-phase3-baseline",
            "pending",
            "--physical-memory-bytes",
            str(64 * 1024**3),
        ]
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertIn("missing required columns", completed.stdout)

        self.data = Dataset(self.root)
        self.data.matching("local", 1)[0]["fixture"] = ""
        self.assert_failure("empty TSV field")

        self.data = Dataset(self.root)
        self.data.matching("local", 1)[0]["wall_clock_s"] = "nan"
        self.assert_failure("expected a positive finite decimal")

    def test_process_status_and_report_timing_mismatch_fail(self) -> None:
        self.data.matching("local", 1)[0]["status"] = "timeout"
        self.assert_failure("status/validation")

        self.data = Dataset(self.root)
        row = self.data.matching("local", 1)[0]
        self.data.set_report(row, "local_scoring_ms", "99")
        self.assert_failure("TSV/report local_scoring_ms values disagree")

    def test_phase3_baseline_identity_mismatch_fails(self) -> None:
        baseline = self.make_phase3("100")
        # Rewrite one baseline identity after creation.
        with baseline.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            fields = list(reader.fieldnames or [])
            rows = list(reader)
        rows[0]["input_sha256"] = "a" * 64
        with baseline.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        self.assert_failure("input_sha256 changed across trials", baseline=baseline, defer=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
