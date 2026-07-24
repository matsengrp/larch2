#!/usr/bin/env python3
"""Deterministic positive/negative fixtures for wric_phase4_acceptance.py."""

from __future__ import annotations

import csv
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


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

    def retain_workload(self, workload: str) -> None:
        """Keep exactly one matrix and its reports in this raw-table owner."""

        kept = self.matching(workload)
        report_paths = {row["report_path"] for row in kept}
        self.rows = kept
        self.reports = {
            path: lines for path, lines in self.reports.items() if path in report_paths
        }

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
        raw_trials: list[Path] | None = None,
        write_data: bool = True,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, Any] | None]:
        if write_data:
            self.data.write()
        command = [
            sys.executable,
            str(TOOL),
            "--repetitions",
            str(self.data.repetitions),
            "--physical-memory-bytes",
            str(physical_memory),
        ]
        for raw_path in raw_trials or [self.data.raw]:
            command.extend(("--raw-trials", str(raw_path)))
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

    def assert_pass(self, **kwargs: Any) -> dict[str, Any]:
        completed, result = self.run_tool(**kwargs)
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        assert result is not None
        return result

    def assert_failure(self, expected: str, **kwargs: Any) -> dict[str, Any]:
        completed, result = self.run_tool(**kwargs)
        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertIsInstance(result, dict)
        assert result is not None
        self.assertIn(expected, str(result.get("failure", "")))
        return result

    def make_phase3(self, local_ms: str = "100") -> Path:
        baseline_root = self.root / "phase3"
        baseline = Dataset(baseline_root, repetitions=self.data.repetitions)
        current = self.data.matching("local", 1)[0]
        for row in baseline.rows:
            for field in (
                "input_sha256",
                "refseq_sha256",
                "search_semantic_sha256",
                "output_semantic_sha256",
            ):
                row[field] = current[field]
        baseline.set_timing("local", 1, "local_scoring_ms", local_ms)
        baseline.write()
        return baseline.raw

    @staticmethod
    def file_hash(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    @staticmethod
    def write_tsv(
        path: Path, fields: tuple[str, ...], rows: list[dict[str, object]]
    ) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle, fieldnames=fields, delimiter="\t", lineterminator="\n"
            )
            writer.writeheader()
            writer.writerows(rows)

    def rewrite_receipt(self, path: Path, receipt: dict[str, Any]) -> str:
        path.write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return self.file_hash(path)

    def receipt_arguments(
        self, receipt: Path, receipt_hash: str, revision: str, dagutil_hash: str
    ) -> list[str]:
        return [
            "--contention-investigation-receipt",
            str(receipt),
            "--expected-contention-investigation-receipt-sha256",
            receipt_hash,
            "--expected-contention-product-revision",
            revision,
            "--expected-contention-dagutil-sha256",
            dagutil_hash,
        ]

    def make_contention_receipt(
        self,
        baseline: Path,
        *,
        completion_marker_bytes: bytes = b"complete\n",
        fixed_control_violations: int = 10,
        metrics_mismatch: bool = False,
        preflight_mode: str = "valid",
        provenance_mismatch: bool = False,
    ) -> tuple[Path, dict[str, Any], str, str, str]:
        profile_root = self.root / "contention-profile"
        profile_root.mkdir(exist_ok=True)

        def artifact(relative: str, contents: bytes) -> dict[str, str]:
            path = profile_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
            return {"path": relative, "sha256": self.file_hash(path)}

        product_root = self.root / "product"
        product_root.mkdir(exist_ok=True)
        dagutil = product_root / "dagutil"
        dagutil.write_bytes(b"synthetic dagutil\n")
        input_path = product_root / "input.pb.gz"
        input_path.write_bytes(b"synthetic input\n")
        refseq_path = product_root / "refseq.txt.gz"
        refseq_path.write_bytes(b"synthetic refseq\n")
        metrics_path = product_root / "wric-process-metrics"
        metrics_path.write_bytes(b"synthetic process metrics\n")
        input_hash = self.file_hash(input_path)
        refseq_hash = self.file_hash(refseq_path)
        dagutil_hash = self.file_hash(dagutil)
        metrics_hash = self.file_hash(metrics_path)
        revision = "a" * 40

        for row in self.data.rows:
            row["input_sha256"] = input_hash
            row["refseq_sha256"] = refseq_hash
        self.data.write()

        with baseline.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            baseline_fields = list(reader.fieldnames or [])
            baseline_rows = list(reader)
        for row in baseline_rows:
            row["input_sha256"] = input_hash
            row["refseq_sha256"] = refseq_hash
        with baseline.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=baseline_fields,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(baseline_rows)

        raw_metadata = self.root / "wric-benchmark-run-metadata.json"
        raw_metadata.write_text("{}\n", encoding="utf-8")
        raw_ledger = self.root / "wric-evidence-run-ledger.tsv"
        raw_ledger.write_text("path\tsha256\n", encoding="utf-8")
        baseline_metadata = baseline.parent / "wric-benchmark-run-metadata.json"
        baseline_metadata.write_text("{}\n", encoding="utf-8")
        baseline_ledger = baseline.parent / "wric-evidence-run-ledger.tsv"
        baseline_ledger.write_text("path\tsha256\n", encoding="utf-8")

        timing_fields = (
            "role",
            "worker",
            "repetition",
            "user_s",
            "system_s",
            "wall_s",
            "peak_sampled_rss_kb",
            "semantic_sha256",
            "canonical_sha256",
            "metrics_sha256",
            "stdout_sha256",
            "stderr_sha256",
            "output_mode",
            "output_sha256",
        )

        native_groups = (
            ("cache", 1),
            ("cache", 2),
            ("cache", 4),
            ("cache", 8),
            ("dense", 1),
            ("dense", 2),
            ("dense", 4),
            ("dense", 8),
        )
        devnull_groups = (
            ("cache", 1),
            ("cache", 8),
            ("dense", 1),
            ("dense", 8),
            ("fixed", 1),
        )
        native_path = profile_root / "native/times.tsv"
        devnull_path = profile_root / "devnull/times.tsv"
        profile_semantics = {
            "cache": self.data.matching("construction")[0][
                "search_semantic_sha256"
            ],
            "dense": self.data.matching("local")[0]["search_semantic_sha256"],
            "fixed": self.data.matching("local")[0]["output_semantic_sha256"],
        }
        canonical_contents = {
            role: (json.dumps({"role": role}, sort_keys=True) + "\n").encode()
            for role in ("cache", "dense", "fixed")
        }
        profile_canonicals = {
            role: hashlib.sha256(contents).hexdigest()
            for role, contents in canonical_contents.items()
        }
        violation_counts = {
            "native": {
                ("cache", 1): 6,
                ("cache", 2): 6,
                ("cache", 4): 5,
                ("cache", 8): 6,
                ("dense", 1): 6,
                ("dense", 2): 3,
                ("dense", 4): 6,
                ("dense", 8): 5,
            },
            "devnull": {
                ("cache", 1): 6,
                ("cache", 8): 9,
                ("dense", 1): 5,
                ("dense", 8): 7,
                ("fixed", 1): fixed_control_violations,
            },
        }

        def timing_rows(
            directory: str, groups: tuple[tuple[str, int], ...]
        ) -> list[dict[str, object]]:
            rows = []
            for role, worker in groups:
                for repetition in range(1, 11):
                    stem = f"{role}-w{worker}-r{repetition}"
                    root = profile_root / directory
                    root.mkdir(parents=True, exist_ok=True)
                    user_s = "4"
                    system_s = (
                        "2"
                        if repetition <= violation_counts[directory][(role, worker)]
                        else "1"
                    )
                    metrics_values = [
                        ("schema_version", "2"),
                        ("outcome", "exited"),
                        ("exit_code", "0"),
                        ("term_signal", "0"),
                        ("timed_out", "0"),
                        ("runner_exit_code", "0"),
                        ("wall_seconds", "5"),
                        (
                            "user_seconds",
                            (
                                "9"
                                if metrics_mismatch
                                and directory == "native"
                                and role == "cache"
                                and worker == 1
                                and repetition == 1
                                else user_s
                            ),
                        ),
                        ("system_seconds", system_s),
                        ("max_rss_kb", "900"),
                        ("peak_sampled_rss_kb", "1000"),
                        ("peak_sampled_swap_kb", "0"),
                        ("rss_kb_unit", "1024_bytes"),
                        ("proc_status_samples", "2"),
                        ("proc_rss_samples", "2"),
                        ("proc_swap_samples", "2"),
                        ("proc_group_samples", "3"),
                        ("peak_sampled_process_count", "1"),
                        ("subreaper_enabled", "1"),
                        ("descendants_reaped", "0"),
                        ("post_leader_descendants", "0"),
                        ("descendant_cleanup_kill_sent", "0"),
                        ("live_descendants_at_return", "0"),
                        ("process_group_alive_at_return", "0"),
                        ("wait4_echild_at_return", "1"),
                        ("monitor_error", "0"),
                        ("monitor_error_count", "0"),
                        ("wait4_collected", "1"),
                        ("wait_errno", "0"),
                        ("child_error_stage", "none"),
                        ("child_error_errno", "0"),
                        ("core_dumped", "0"),
                        ("timeout_term_sent", "0"),
                        ("timeout_kill_sent", "0"),
                        ("rss_limit_bytes", "17179869184"),
                        ("rss_limit_enabled", "1"),
                        ("rss_limit_observed", "0"),
                        ("rss_limit_exceeded", "0"),
                        ("rss_limit_trigger_bytes", "0"),
                        ("rss_limit_term_sent", "0"),
                        ("rss_limit_kill_sent", "0"),
                    ]
                    per_run = {
                        "canonical.json": canonical_contents[role],
                        "metrics": (
                            "".join(
                                f"{key}={value}\n" for key, value in metrics_values
                            )
                        ).encode(),
                        "stdout": f"{stem} stdout\n".encode(),
                        "stderr": b"",
                    }
                    hashes = {}
                    for suffix, contents in per_run.items():
                        path = root / f"{stem}.{suffix}"
                        path.write_bytes(contents)
                        hashes[suffix] = self.file_hash(path)
                    output_hash = "-"
                    output_mode = "no_output" if role == "fixed" else "devnull"
                    if directory == "native":
                        output = root / f"{stem}.pb.gz"
                        output.write_bytes(f"{stem} output\n".encode())
                        output_hash = self.file_hash(output)
                        output_mode = "file"
                    rows.append(
                        {
                            "role": role,
                            "worker": worker,
                            "repetition": repetition,
                            "user_s": user_s,
                            "system_s": system_s,
                            "wall_s": "5",
                            "peak_sampled_rss_kb": "1000",
                            "semantic_sha256": profile_semantics[role],
                            "canonical_sha256": hashes["canonical.json"],
                            "metrics_sha256": hashes["metrics"],
                            "stdout_sha256": hashes["stdout"],
                            "stderr_sha256": hashes["stderr"],
                            "output_mode": output_mode,
                            "output_sha256": output_hash,
                        }
                    )
            return rows

        self.write_tsv(
            native_path, timing_fields, timing_rows("native", native_groups)
        )
        self.write_tsv(
            devnull_path, timing_fields, timing_rows("devnull", devnull_groups)
        )

        summary_fields = (
            "role",
            "worker",
            "semantic_sha256",
            "canonical_sha256",
            "output_sha256",
            "stdout_sha256",
            "stderr_sha256",
            "profile_sha256",
            "log_sha256",
            "annotation_sha256",
            "instructions",
            "syscall_count",
            "system_time_ns",
            "system_cpu_time_ns",
        )
        callgrind_runs: list[dict[str, object]] = []
        summary_rows: list[dict[str, object]] = []
        for role, worker in (("dense", 8), ("cache", 8), ("fixed", 1)):
            profile_path = profile_root / "callgrind" / f"{role}-w{worker}.callgrind"
            profile_path.parent.mkdir(parents=True, exist_ok=True)
            profile_path.write_text(f"{role} {worker}\n", encoding="utf-8")
            profile_hash = self.file_hash(profile_path)
            row = {
                "role": role,
                "worker": worker,
                "semantic_sha256": profile_semantics[role],
                "canonical_sha256": profile_canonicals[role],
                "output_sha256": "9" * 64,
                "stdout_sha256": "a" * 64,
                "stderr_sha256": "b" * 64,
                "profile_sha256": profile_hash,
                "log_sha256": "c" * 64,
                "annotation_sha256": "d" * 64,
                "instructions": "1",
                "syscall_count": "2",
                "system_time_ns": "3",
                "system_cpu_time_ns": "4",
            }
            summary_rows.append(row)
            callgrind_runs.append(
                {
                    "canonical_sha256": row["canonical_sha256"],
                    "profile_sha256": profile_hash,
                    "role": role,
                    "semantic_sha256": row["semantic_sha256"],
                    "syscall_count": 2,
                    "system_cpu_time_ns": 4,
                    "system_time_ns": 3,
                    "worker": worker,
                }
            )
        summary_path = profile_root / "callgrind/summary.tsv"
        self.write_tsv(summary_path, summary_fields, summary_rows)

        def group_summaries(
            directory: str, groups: tuple[tuple[str, int], ...]
        ) -> list[dict[str, object]]:
            return [
                {
                    "role": role,
                    "rows": 10,
                    "violations": violation_counts[directory][(role, worker)],
                    "worker": worker,
                }
                for role, worker in groups
            ]

        runner = artifact("profile-controller.sh", b"#!/bin/sh\nexit 0\n")
        provenance_values = [
            ("schema", "wric.phase4.contention_profile_provenance"),
            ("schema_version", "1"),
            ("controller_sha256", runner["sha256"]),
            (
                "product_revision",
                "c" * 40 if provenance_mismatch else revision,
            ),
            ("product_tree", "b" * 40),
            ("dagutil_sha256", dagutil_hash),
            ("input_sha256", input_hash),
            ("refseq_sha256", refseq_hash),
            ("process_metrics_sha256", metrics_hash),
            (
                "valgrind_sha256",
                "9e8422466bd87902983118bb7bc51e67679b0bbaa09788f7d6b2aa0af40406b3",
            ),
            (
                "callgrind_annotate_sha256",
                "8716f89225e5c49615788f9d04779a92fb20928626ba13cf25045697d40e5e4d",
            ),
            ("valgrind_version", "valgrind-3.27.1"),
            ("kernel", "synthetic-kernel"),
            ("affinity", "0,2,4,6,8,10,12,14"),
            (
                "environment",
                "HOME=/nonexistent LANG=C LC_ALL=C PATH=/usr/bin:/bin "
                "TMPDIR=/tmp TZ=Europe/Sofia",
            ),
            ("native_repetitions", "10"),
            ("native_warmups_per_cell", "1"),
            ("callgrind_interpretation", "qualitative_only"),
        ]
        provenance = artifact(
            "provenance.txt",
            "".join(f"{key}={value}\n" for key, value in provenance_values).encode(),
        )
        completion = artifact("profile-complete", completion_marker_bytes)
        preflight_rows = 14 if preflight_mode == "short" else 15
        preflight_text = (
            "sample\tepoch\tload1\trunnable_processes\ttotal_processes\n"
            + "".join(
                f"{sample}\t{1000 + sample}.0\t"
                f"{'2.0' if preflight_mode == 'busy' and sample == 1 else '0.5'}"
                "\t1\t100\n"
                for sample in range(1, preflight_rows + 1)
            )
        )
        preflight = artifact("host-preflight.tsv", preflight_text.encode())
        before = artifact("host-processes-before.txt", b"before\n")
        after = artifact("host-processes-after.txt", b"after\n")
        rationale = artifact("investigation.md", b"synthetic rationale\n")
        builder = artifact("build-investigation-receipt.py", b"# synthetic\n")

        raw_hash = self.file_hash(self.data.raw)
        violations = []
        for role, rows in (
            ("construction", self.data.matching("construction")),
            ("local", self.data.matching("local")),
        ):
            for row in rows:
                if (
                    acceptance.Decimal(row["system_cpu_s"])
                    > acceptance.Decimal(row["user_cpu_s"])
                    * acceptance.CONTENTION_LIMIT
                ):
                    violations.append(
                        {
                            "raw_trials_sha256": raw_hash,
                            "role": role,
                            "row_id": row["row_id"],
                            "system_cpu_s": row["system_cpu_s"],
                            "trial_index": int(row["trial_index"]),
                            "user_cpu_s": row["user_cpu_s"],
                            "worker": int(row["requested_workers"]),
                        }
                    )

        def bound_file(path: Path) -> dict[str, str]:
            return {"path": str(path.resolve()), "sha256": self.file_hash(path)}

        workloads = []
        for role, rows, prefix in (
            (
                "construction",
                self.data.matching("construction"),
                acceptance.DEFAULT_CONSTRUCTION_PREFIX,
            ),
            ("local", self.data.matching("local"), acceptance.DEFAULT_LOCAL_PREFIX),
        ):
            first = rows[0]
            workloads.append(
                {
                    "fixture": first["fixture"],
                    "input_sha256": first["input_sha256"],
                    "method": first["method"],
                    "output_semantic_sha256": first["output_semantic_sha256"],
                    "refseq_sha256": first["refseq_sha256"],
                    "role": role,
                    "search_semantic_sha256": first["search_semantic_sha256"],
                    "workers": [
                        {
                            "canonical_argv_sha256": self.data.matching(role, worker)[
                                0
                            ]["canonical_argv_sha256"],
                            "row_id": f"{prefix}{worker}",
                            "worker": worker,
                        }
                        for worker in acceptance.WORKERS
                    ],
                }
            )

        receipt: dict[str, Any] = {
            "capture": {
                "phase3_baseline": {
                    **bound_file(baseline),
                    "metadata": bound_file(baseline_metadata),
                    "run_ledger": bound_file(baseline_ledger),
                },
                "raw_trials": [
                    {
                        **bound_file(self.data.raw),
                        "metadata": bound_file(raw_metadata),
                        "role": role,
                        "run_ledger": bound_file(raw_ledger),
                    }
                    for role in ("construction", "local")
                ],
                "repetitions": self.data.repetitions,
                "row_prefixes": {
                    "construction": acceptance.DEFAULT_CONSTRUCTION_PREFIX,
                    "local": acceptance.DEFAULT_LOCAL_PREFIX,
                },
            },
            "disposition": "no_rollback_required_for_bound_capture",
            "finding": {
                "acceptance_timing_source": "bound_raw_trials_only",
                "alternate_timing_waiver": False,
                "classification": "short_run_fixed_overhead_and_parallel_contention_not_scaling_blocker",
                "diagnostic_timings_used_for_acceptance": False,
                "rationale": rationale,
                "rollback_review": "completed",
            },
            "product": {
                "dagutil": bound_file(dagutil),
                "input": bound_file(input_path),
                "process_metrics": bound_file(metrics_path),
                "refseq": bound_file(refseq_path),
                "revision": revision,
                "tree": "b" * 40,
            },
            "profile": {
                "affinity": "0,2,4,6,8,10,12,14",
                "callgrind": {
                    "artifact": {
                        "path": "callgrind/summary.tsv",
                        "sha256": self.file_hash(summary_path),
                    },
                    "interpretation": "qualitative_only",
                    "profiler": {
                        "binary_sha256": "9e8422466bd87902983118bb7bc51e67679b0bbaa09788f7d6b2aa0af40406b3",
                        "kind": "callgrind_collect_systime_nsec",
                        "version": "valgrind-3.27.1",
                    },
                    "runs": callgrind_runs,
                },
                "completion_marker": completion,
                "devnull_and_fixed_controls": {
                    "artifact": {
                        "path": "devnull/times.tsv",
                        "sha256": self.file_hash(devnull_path),
                    },
                    "group_violations": group_summaries(
                        "devnull", devnull_groups
                    ),
                    "measured_repetitions_per_cell": 10,
                    "rows": 50,
                    "violations": sum(violation_counts["devnull"].values()),
                    "warmups_per_cell": 1,
                },
                "environment": [
                    "HOME=/nonexistent",
                    "LANG=C",
                    "LC_ALL=C",
                    "PATH=/usr/bin:/bin",
                    "TMPDIR=/tmp",
                    "TZ=Europe/Sofia",
                ],
                "host_preflight": {
                    "processes_after": after,
                    "processes_before": before,
                    "samples": 15,
                    "times": preflight,
                },
                "native_rusage": {
                    "artifact": {
                        "path": "native/times.tsv",
                        "sha256": self.file_hash(native_path),
                    },
                    "group_violations": group_summaries("native", native_groups),
                    "measured_repetitions_per_cell": 10,
                    "rows": 80,
                    "violations": sum(violation_counts["native"].values()),
                    "warmups_per_cell": 1,
                },
                "process_metrics_sha256": metrics_hash,
                "provenance": provenance,
                "runner": {"kind": "preserved_script", **runner},
            },
            "receipt_builder": builder,
            "schema": "wric.phase4.contention_investigation",
            "schema_version": 1,
            "status": "complete",
            "threshold": {
                "comparison": "greater_than",
                "limit": "0.25",
                "metric": "system_cpu_s_over_user_cpu_s",
            },
            "violations": violations,
            "workloads": workloads,
        }
        receipt_path = profile_root / "phase4-contention-investigation.json"
        receipt_hash = self.rewrite_receipt(receipt_path, receipt)
        return receipt_path, receipt, receipt_hash, revision, dagutil_hash

    def split_workloads(self) -> tuple[Dataset, Dataset]:
        local = Dataset(self.root / "local", repetitions=self.data.repetitions)
        local.retain_workload("local")
        local.write()
        construction = Dataset(
            self.root / "construction", repetitions=self.data.repetitions
        )
        construction.retain_workload("construction")
        construction.write()
        return local, construction

    def test_one_file_interface_and_provenance_remain_compatible(self) -> None:
        result = self.assert_pass()
        raw = str(self.data.raw.resolve())
        self.assertEqual(result["raw_trials"], raw)
        self.assertEqual(result["raw_trial_inputs"], [raw])
        self.assertEqual(result["workloads"]["local"]["source_raw_trials"], [raw])
        self.assertEqual(
            result["workloads"]["construction"]["source_raw_trials"], [raw]
        )

    def test_split_raw_inputs_bind_reports_to_each_owning_file(self) -> None:
        local, construction = self.split_workloads()
        for row in construction.rows:
            row["fixture"] = "cache-medium"
        construction.write()
        result = self.assert_pass(raw_trials=[local.raw, construction.raw])
        expected = [str(local.raw.resolve()), str(construction.raw.resolve())]
        self.assertEqual(result["raw_trial_inputs"], expected)
        self.assertEqual(
            result["workloads"]["local"]["source_raw_trials"], [expected[0]]
        )
        self.assertEqual(
            result["workloads"]["construction"]["source_raw_trials"],
            [expected[1]],
        )

    def test_split_matrices_still_require_the_same_input_identity(self) -> None:
        local, construction = self.split_workloads()
        for row in construction.rows:
            row["fixture"] = "cache-medium"
            row["input_sha256"] = "a" * 64
        construction.write()
        self.assert_failure(
            "local/construction matrices differ in input_sha256",
            raw_trials=[local.raw, construction.raw],
        )

    def test_duplicate_global_row_key_across_raw_inputs_fails(self) -> None:
        local, construction = self.split_workloads()
        duplicate = Dataset(self.root / "duplicate", repetitions=self.data.repetitions)
        duplicate.retain_workload("local")
        duplicate.write()
        self.assert_failure(
            "duplicate global row key",
            raw_trials=[local.raw, construction.raw, duplicate.raw],
        )

    def test_repeated_same_raw_input_fails(self) -> None:
        self.assert_failure(
            "raw trials input is repeated",
            raw_trials=[self.data.raw, self.data.raw],
        )

    def test_omitted_split_input_or_required_row_fails(self) -> None:
        local, construction = self.split_workloads()
        self.assert_failure(
            "no rows match prefix",
            raw_trials=[local.raw],
        )

        construction.rows.remove(construction.matching("construction", 4)[0])
        construction.write()
        self.assert_failure(
            "expected 3 trials, found 2",
            raw_trials=[local.raw, construction.raw],
        )

    def test_raw_input_symlink_and_special_file_fail_before_reading(self) -> None:
        self.data.write()
        alias = self.root / "raw-alias.tsv"
        alias.symlink_to(self.data.raw.name)
        self.assert_failure(
            "raw trials input uses a symlink",
            raw_trials=[alias],
            write_data=False,
        )

        fifo = self.root / "raw.fifo"
        os.mkfifo(fifo)
        self.assert_failure(
            "raw trials input is not a regular file",
            raw_trials=[fifo],
            write_data=False,
        )

    def test_report_symlink_special_file_and_owner_escape_fail(self) -> None:
        self.data.write()
        row = self.data.matching("local", 1)[0]
        report = self.root / row["report_path"]
        contents = report.read_text(encoding="utf-8")
        target = self.root / "real-report.out"
        target.write_text(contents, encoding="utf-8")
        report.unlink()
        report.symlink_to(target)
        self.assert_failure("report_path uses a symlink", write_data=False)

        report.unlink()
        os.mkfifo(report)
        self.assert_failure("report_path is not a regular file", write_data=False)

        report.unlink()
        owned = Dataset(self.root / "owned", repetitions=self.data.repetitions)
        owned.retain_workload("local")
        escaped_row = owned.matching("local", 1)[0]
        outside = self.root / "outside-report.out"
        outside.write_text(
            "\n".join(owned.reports[escaped_row["report_path"]]) + "\n",
            encoding="utf-8",
        )
        escaped_row["report_path"] = str(outside)
        owned.write()
        self.assert_failure(
            "report_path escapes owning raw trials directory",
            raw_trials=[owned.raw],
        )

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

    def test_exact_contention_receipt_continues_with_investigated_gate(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        result = self.assert_pass(
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )
        cpu_gate = next(
            gate for gate in result["gates"] if gate["name"] == "system_over_user_cpu"
        )
        self.assertEqual(cpu_gate["status"], "investigated")
        self.assertEqual(cpu_gate["violations"], 1)
        self.assertEqual(
            cpu_gate["disposition"], "no_rollback_required_for_bound_capture"
        )

    def test_contention_receipt_is_forbidden_without_violations(self) -> None:
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        self.assert_failure(
            "receipt is forbidden",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

    def test_contention_receipt_requires_exact_violation_set(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt_path, receipt, _, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        original = list(receipt["violations"])
        for changed, label in (
            ([], "missing"),
            ([*original, dict(original[0])], "extra"),
        ):
            with self.subTest(label=label):
                receipt["violations"] = changed
                receipt_hash = self.rewrite_receipt(receipt_path, receipt)
                self.assert_failure(
                    "violation set does not exactly match",
                    baseline=baseline,
                    defer=False,
                    extra=self.receipt_arguments(
                        receipt_path, receipt_hash, revision, dagutil_hash
                    ),
                )
        receipt["violations"] = original

    def test_contention_receipt_detects_raw_product_and_profile_tamper(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt_path, receipt, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        arguments = self.receipt_arguments(
            receipt_path, receipt_hash, revision, dagutil_hash
        )

        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0002"
        self.data.write()
        self.assert_failure(
            "violation set does not exactly match",
            baseline=baseline,
            defer=False,
            extra=arguments,
            write_data=False,
        )

        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        self.data.write()
        dagutil = Path(receipt["product"]["dagutil"]["path"])
        dagutil.write_bytes(b"tampered dagutil\n")
        self.assert_failure(
            "product.dagutil SHA-256",
            baseline=baseline,
            defer=False,
            extra=arguments,
            write_data=False,
        )
        dagutil.write_bytes(b"synthetic dagutil\n")

        native = receipt_path.parent / receipt["profile"]["native_rusage"]["artifact"][
            "path"
        ]
        native.write_text(
            native.read_text(encoding="utf-8") + "tamper\n", encoding="utf-8"
        )
        self.assert_failure(
            "profile.native_rusage.artifact SHA-256",
            baseline=baseline,
            defer=False,
            extra=arguments,
            write_data=False,
        )

    def test_contention_receipt_hash_and_canonical_json_are_enforced(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt_path, receipt, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        self.assert_failure(
            "receipt SHA-256",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt_path, receipt_hash, revision, dagutil_hash
            ),
        )
        noncanonical_hash = self.file_hash(receipt_path)
        self.assert_failure(
            "JSON is not in canonical sorted form",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt_path, noncanonical_hash, revision, dagutil_hash
            ),
        )

    def test_contention_receipt_product_and_argv_identity_are_stale_closed(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt_path, receipt, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        self.assert_failure(
            "product revision does not match expected",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt_path, receipt_hash, "c" * 40, dagutil_hash
            ),
        )

        receipt["workloads"][1]["workers"][3]["canonical_argv_sha256"] = "f" * 64
        receipt_hash = self.rewrite_receipt(receipt_path, receipt)
        self.assert_failure(
            "workload/argv identity does not match",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt_path, receipt_hash, revision, dagutil_hash
            ),
        )

    def test_contention_profile_metrics_must_match_timing_rows(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline, metrics_mismatch=True)
        )
        self.assert_failure(
            "user_seconds does not exactly match the timing TSV",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

    def test_contention_profile_provenance_must_match_receipt(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(
                baseline, provenance_mismatch=True
            )
        )
        self.assert_failure(
            "provenance product_revision does not match receipt",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

    def test_contention_profile_completion_marker_is_semantic(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(
                baseline, completion_marker_bytes=b"not complete\n"
            )
        )
        self.assert_failure(
            "completion_marker is not exactly",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

    def test_contention_profile_preflight_cardinality_and_quiescence(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        for mode, expected in (
            ("short", "must contain 15 rows"),
            ("busy", "host preflight is not quiescent"),
        ):
            with self.subTest(mode=mode):
                receipt, _, receipt_hash, revision, dagutil_hash = (
                    self.make_contention_receipt(
                        baseline, preflight_mode=mode
                    )
                )
                self.assert_failure(
                    expected,
                    baseline=baseline,
                    defer=False,
                    extra=self.receipt_arguments(
                        receipt, receipt_hash, revision, dagutil_hash
                    ),
                )

    def test_contention_profile_requires_fixed_control_breaches(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(
                baseline, fixed_control_violations=0
            )
        )
        self.assert_failure(
            "fixed W1 control must breach in exactly 10/10 trials",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

    def test_contention_receipt_cli_arguments_are_atomic(self) -> None:
        completed, _ = self.run_tool(
            extra=["--contention-investigation-receipt", str(self.root / "x.json")]
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn(
            "all four contention-investigation receipt/product arguments",
            completed.stderr,
        )

    def test_inconclusive_or_rollback_contention_disposition_still_blocks(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        baseline = self.make_phase3()
        receipt_path, receipt, _, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        for disposition in ("inconclusive", "rollback_required"):
            with self.subTest(disposition=disposition):
                receipt["disposition"] = disposition
                receipt_hash = self.rewrite_receipt(receipt_path, receipt)
                result = self.assert_failure(
                    f"disposition is {disposition}",
                    baseline=baseline,
                    defer=False,
                    extra=self.receipt_arguments(
                        receipt_path, receipt_hash, revision, dagutil_hash
                    ),
                )
                self.assertEqual(result["status"], "profiling_required")

    def test_contention_receipt_does_not_waive_timing_or_rss_gates(self) -> None:
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        self.data.set_timing("local", 8, "local_scoring_ms", "50.001")
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        self.assert_failure(
            "local_w8_over_w1",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

        self.data = Dataset(self.root)
        self.data.matching("local", 8)[0]["system_cpu_s"] = "1.0001"
        self.data.set_rss("local", 8, 2001)
        baseline = self.make_phase3()
        receipt, _, receipt_hash, revision, dagutil_hash = (
            self.make_contention_receipt(baseline)
        )
        self.assert_failure(
            "exceeds 2x W1",
            baseline=baseline,
            defer=False,
            extra=self.receipt_arguments(
                receipt, receipt_hash, revision, dagutil_hash
            ),
        )

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
