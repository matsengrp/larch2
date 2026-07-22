#!/usr/bin/env python3
"""Focused synthetic tests for the read-only cross-phase evaluator."""

from __future__ import annotations

import csv
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Callable, Mapping


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import wric_cross_phase_acceptance as cross  # noqa: E402


def digest(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


METHOD_SHORT = {
    cross.METHOD_SAMPLED: "sampled-tree-fixed-topology",
    cross.METHOD_EXACT: "grammar-exact",
    cross.METHOD_HYBRID: "hybrid-exact",
}


class SyntheticEvidence:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.base = root / "workloads.tsv"
        self.base_rows: dict[str, dict[str, str]] = {}
        self.supplement_rows: dict[str, dict[str, dict[str, str]]] = {
            label: {} for label in cross.SUPPLEMENT_SPECS
        }
        self.supplements: dict[str, Path] = {}
        self.supplement_shas: dict[str, str] = {}
        self.manifest_rows: dict[str, dict[str, str]] = {}
        self.raw_paths: dict[str, Path] = {}
        self.raw_inputs: dict[str, list[Path]] = {}
        self.phase0_capture_paths: list[Path] = []
        self.phase0_classification_paths: list[Path] = []
        self.phase0_inputs: list[Path] = []
        self.phase0_capture_by_name: dict[str, Path] = {}
        self.phase0_classification_by_name: dict[str, Path] = {}
        self.phase0_strict_real: Path | None = None
        self.phase0_artifact_ledger = root / "phase0-artifacts.tsv"
        self.phase0_ledger_member = root / "phase0-ledger-member.txt"
        self.refusal_stderr = "synthetic frozen larch2 diagnostic\n" + cross.REAL_REFUSAL_SUFFIX
        self.refusal_stderr_sha = hashlib.sha256(self.refusal_stderr.encode()).hexdigest()
        self._make_manifest_rows()
        self._write_manifests()
        self._write_phase0()
        self._write_runs()

    def add_manifest(
        self,
        row_id: str,
        group: str,
        fixture: str,
        method: str,
        worker: str,
        *,
        expected_outcome: str = "ok",
        supplement: str | None = None,
        semantic_key: str | None = None,
        top_k: int | None = None,
        max_candidates: int | None = None,
        iterations: int = 1,
    ) -> None:
        key = fixture if semantic_key is None else semantic_key
        requested = "-" if worker == "native" else ("0" if worker == "auto" else worker)
        policy = "-" if worker == "native" else (
            "automatic" if worker == "auto" else
            "automatic_default" if worker == "default" else "explicit"
        )
        resolved = "-" if worker == "native" else ("8" if worker in ("auto", "default") else worker)
        search = "-" if worker == "native" else digest(f"search:{key}:{method}")
        output = digest(f"output:{key}:{method}")
        row = {name: "-" for name in cross.MANIFEST_COLUMNS}
        row.update(
            row_id=row_id,
            run_group=group,
            workload_name=fixture,
            fixture_id=fixture,
            method=method,
            primary_sha256=digest(f"input:{key}"),
            refseq_sha256="-",
            requested_workers=requested,
            expected_resolved_workers=resolved,
            expected_worker_policy=policy,
            rss_limit_bytes=str(16 * 1024**3),
            expected_outcome=expected_outcome,
            expected_timeout_trials="1" if expected_outcome == "timeout" else "0",
            expected_reason_code=(
                cross.REAL_REFUSAL_REASON_CODE
                if expected_outcome == "expected_infeasible" else "-"
            ),
            expected_reason_sha256=(
                self.refusal_stderr_sha
                if expected_outcome == "expected_infeasible" else "-"
            ),
            iterations=str(iterations),
            seed="1",
            chart_max_candidates=str(
                max_candidates
                if max_candidates is not None else
                (64 if method == cross.METHOD_LB else 32)
            ),
            chart_top_k_exact=str(
                top_k
                if top_k is not None else
                (4 if method in (cross.METHOD_EXACT, cross.METHOD_HYBRID) else 0)
            ),
            acceptance="-" if worker == "native" else "synthetic",
            objective="-" if worker == "native" else "synthetic",
            candidate_source="-" if worker == "native" else "synthetic",
            memory_budget_bytes=str(12 * 1024**3) if worker != "native" else "-",
            expected_initial_score="100",
            expected_final_score="90" if expected_outcome == "ok" else "-",
            expected_validated_parsimony="90" if expected_outcome == "ok" else "-",
            oracle_search_semantic_sha256=search,
            oracle_output_semantic_sha256=output,
            oracle_trial_semantic_sha256=digest(f"trial:{row_id}"),
            canonical_argv_sha256=digest(f"argv:{row_id}"),
        )
        if expected_outcome in ("timeout", "expected_infeasible"):
            row["expected_resolved_workers"] = "-"
            row["expected_worker_policy"] = "-"
            row["oracle_search_semantic_sha256"] = "-"
            row["oracle_output_semantic_sha256"] = "-"
            row["oracle_trial_semantic_sha256"] = "-"
        self.manifest_rows[row_id] = row
        target = self.base_rows if supplement is None else self.supplement_rows[supplement]
        target[row_id] = row

    def _make_manifest_rows(self) -> None:
        # Phase 0/1/2/3/5 named rows.
        for template, method, fixture, group in (
            (cross.SMALL_DENSE, cross.METHOD_LB, "small", "p0-small-dense-physical"),
            (cross.SMALL_EXACT, cross.METHOD_EXACT, "small", "p0-small-exact1-physical"),
            (cross.MEDIUM_DENSE, cross.METHOD_LB, "medium", "p0-medium-dense-physical"),
            (cross.MEDIUM_CACHE, cross.METHOD_LB, "medium", "p0-medium-cache-physical"),
            (cross.MEDIUM_LAZY, cross.METHOD_LB, "medium", "p0-medium-lazy-physical"),
            (cross.MEDIUM_EXACT, cross.METHOD_EXACT, "medium", "p0-medium-exact1-physical"),
        ):
            for worker in (1, 2, 4, 8):
                outcome = "timeout" if template == cross.MEDIUM_LAZY and worker == 1 else "ok"
                self.add_manifest(
                    template.format(worker),
                    group,
                    fixture,
                    method,
                    str(worker),
                    expected_outcome=outcome,
                    top_k=(1 if template in (cross.SMALL_EXACT, cross.MEDIUM_EXACT) else 0),
                    max_candidates=(
                        1
                        if template in (cross.SMALL_EXACT, cross.MEDIUM_EXACT)
                        else None
                    ),
                )

        self.add_manifest("p0-native-medium-physical-i1-m50", "p0-primary-physical", "medium", cross.METHOD_NATIVE, "native")
        for method, short in METHOD_SHORT.items():
            for worker in (1, 2, 4, 8):
                self.add_manifest(
                    f"p0-medium-primary32k4-{short}-w{worker}",
                    "p0-primary-physical",
                    "medium",
                    method,
                    str(worker),
                    top_k=4,
                )

        self.add_manifest("p0-native-medium-smt-i1-m50", "p0-primary-smt", "medium", cross.METHOD_NATIVE, "native")
        for worker_token in ("1", "2", "4", "8", "16", "auto", "default"):
            suffix = f"w{worker_token}"
            self.add_manifest(f"p0-medium-primary32k4-smt-grammar-exact-{suffix}", "p0-primary-smt", "medium", cross.METHOD_EXACT, worker_token)

        self.add_manifest("p0-native-small-unpinned-i1-m1", "p0-small-auto", "small", cross.METHOD_NATIVE, "native")
        for worker_token in ("1", "auto"):
            self.add_manifest(f"p0-small-auto-grammar-lower-bound-heuristic-w{worker_token}", "p0-small-auto", "small", cross.METHOD_LB, worker_token)

        for fixture in ("small", "medium"):
            stress_group = (
                "p0-small-stress-physical"
                if fixture == "small" else "p0-stress-physical"
            )
            self.add_manifest(
                f"p0-native-{fixture}-physical-i3-m50",
                stress_group,
                fixture,
                cross.METHOD_NATIVE,
                "native",
                iterations=3,
            )
            for method, short in METHOD_SHORT.items():
                for worker in (1, 2, 4, 8):
                    self.add_manifest(
                        f"p0-{fixture}-stress128k16-{short}-w{worker}",
                        stress_group,
                        fixture,
                        method,
                        str(worker),
                        top_k=16,
                        max_candidates=128,
                        iterations=3,
                    )

        for worker in (1, 8):
            self.add_manifest(
                f"p0-real20d-preflight-grammar-exact-w{worker}",
                "real-bounded", "real20d", cross.METHOD_EXACT, str(worker),
                expected_outcome="expected_infeasible",
                top_k=1,
            )

        for fixture in ("high-compression", "dense-favoring"):
            for policy in ("off", "on", "auto"):
                for worker in (1, 2, 4, 8):
                    self.add_manifest(
                        f"phase7-lazy-{fixture}-{policy}-w{worker}",
                        "phase7-lazy",
                        f"phase7-lazy-{fixture}",
                        cross.METHOD_LB,
                        str(worker),
                        supplement="phase7",
                        semantic_key=fixture,
                    )
        for worker in (1, 2, 4, 8):
            self.add_manifest(
                f"phase7-lazy-completion-tree0-on-w{worker}",
                "phase7-lazy-small-on",
                "phase7-lazy-small-on",
                cross.METHOD_LB,
                str(worker),
                supplement="phase7-completion",
                semantic_key="small",
            )
            self.add_manifest(
                f"phase7-lazy-completion-medium-auto-w{worker}",
                "phase7-lazy-medium-auto",
                "phase7-lazy-medium-auto",
                cross.METHOD_LB,
                str(worker),
                supplement="phase7-completion",
                semantic_key="medium",
            )
            self.add_manifest(
                f"phase8-generation-tree0-off-w{worker}",
                "phase8-generation",
                "phase8-generation-tree0",
                cross.METHOD_SAMPLED,
                str(worker),
                supplement="phase8",
                semantic_key="small",
            )
        for seed in (1, 7, 19):
            for worker in (1, 2, 4, 8):
                row_id = f"phase9-local-commit-seed{seed}-w{worker}"
                self.add_manifest(
                    row_id,
                    "phase9-local-commit",
                    "phase9-local-commit",
                    cross.METHOD_EXACT,
                    str(worker),
                    supplement="phase9",
                    semantic_key=f"phase9-seed{seed}",
                    iterations=3,
                )
                self.manifest_rows[row_id]["seed"] = str(seed)

    @staticmethod
    def _seal(path: Path) -> str:
        value = hashlib.sha256(path.read_bytes()).hexdigest()
        path.with_name(path.name + ".sha256").write_text(
            f"{value}  {path.name}\n", encoding="ascii"
        )
        return value

    def _write_one_manifest(
        self,
        path: Path,
        rows: dict[str, dict[str, str]],
        *,
        kind: str,
        manifest_id: str,
        parent_sha256: str,
    ) -> str:
        preamble = (
            "# schema=wric_chart_parallelization_workloads\n"
            "# schema_version=1\n"
            f"# kind={kind}\n"
            f"# manifest_id={manifest_id}\n"
            f"# parent_sha256={parent_sha256}\n"
        )
        with path.open("w", encoding="utf-8", newline="") as handle:
            handle.write(preamble)
            writer = csv.DictWriter(handle, fieldnames=cross.MANIFEST_COLUMNS, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows.values())
        return self._seal(path)

    def _write_manifests(self) -> None:
        self.base_sha = self._write_one_manifest(
            self.base,
            self.base_rows,
            kind="base",
            manifest_id="phase0-base",
            parent_sha256="-",
        )
        for label, spec in cross.SUPPLEMENT_SPECS.items():
            path = self.root / spec.basename
            self.supplements[label] = path
            self.supplement_shas[label] = self._write_one_manifest(
                path,
                self.supplement_rows[label],
                kind="supplement",
                manifest_id=spec.manifest_id,
                parent_sha256=self.base_sha,
            )

    def raw_row(self, row_id: str, trial: int, *, label: str, wall: str = "1", **overrides: str) -> dict[str, str]:
        manifest = self.manifest_rows.get(row_id)
        if manifest:
            method = manifest["method"]
            fixture = manifest["workload_name"]
            worker = cross.manifest_worker(manifest)
            search = manifest["oracle_search_semantic_sha256"]
            output = manifest["oracle_output_semantic_sha256"]
            trial_hash = manifest["oracle_trial_semantic_sha256"]
            argv = manifest["canonical_argv_sha256"]
            policy = manifest["expected_worker_policy"]
            resolved = manifest["expected_resolved_workers"]
            iterations = manifest["iterations"]
            seed = manifest["seed"]
            input_sha = manifest["primary_sha256"]
            refseq_sha = manifest["refseq_sha256"]
            top_k = int(manifest["chart_top_k_exact"])
            max_candidates = int(manifest["chart_max_candidates"])
            expected_outcome = (
                manifest["expected_outcome"]
                if label in ("phase0", "final-real") else "ok"
            )
            if expected_outcome == "ok" and output == "-":
                search = digest(f"search:{fixture}:{method}")
                output = digest(f"output:{fixture}:{method}")
                trial_hash = digest(f"trial:{row_id}")
                policy = "explicit"
                resolved = worker
        else:
            worker = row_id.rsplit("-w", 1)[1]
            method = cross.METHOD_SAMPLED if row_id.startswith("phase8-") else (cross.METHOD_EXACT if row_id.startswith("phase9-") else cross.METHOD_LB)
            if row_id.startswith("phase9-"):
                fixture = "phase9"
                identity = row_id.split("-w", 1)[0]
            elif "high-compression" in row_id:
                fixture, identity = "high-compression", "high-compression"
            elif "dense-favoring" in row_id:
                fixture, identity = "dense-favoring", "dense-favoring"
            elif "completion-medium" in row_id:
                fixture, identity = "medium", "medium"
            else:
                fixture, identity = "small", "small"
            search = digest(f"search:{fixture}:{method}" if not row_id.startswith("phase9-") else f"search:{fixture}:{method}:{identity}")
            output = digest(f"output:{fixture}:{method}" if not row_id.startswith("phase9-") else f"output:{fixture}:{method}:{identity}")
            trial_hash = digest(f"trial:{row_id}")
            argv = digest(f"argv:{row_id}")
            policy = "explicit"
            resolved = worker
            iterations = "3" if row_id.startswith("phase9-") else "1"
            seed = row_id.split("seed", 1)[1].split("-", 1)[0] if row_id.startswith("phase9-") else "1"
            input_sha = digest(f"input:{fixture}")
            refseq_sha = "-"
            top_k = 4 if method in (cross.METHOD_EXACT, cross.METHOD_HYBRID) else 0
            max_candidates = 32
            expected_outcome = "ok"
        exact_count = (
            top_k * int(iterations)
            if method != cross.METHOD_NATIVE and expected_outcome == "ok"
            else 0
        )
        candidate_count = (
            max_candidates * int(iterations)
            if method != cross.METHOD_NATIVE and expected_outcome == "ok"
            else (96 if iterations == "3" else 32)
        )
        resolved_count = (
            int(resolved)
            if resolved not in ("-", "NA") else 0
        )
        candidate_parallel = int(exact_count > 1 and resolved_count > 1)
        inner_parallel = int(
            exact_count == 1 and resolved_count > 1
        )
        peak_exact = (
            min(exact_count, resolved_count)
            if candidate_parallel else int(exact_count > 0)
        )
        report = self.root / label / "reports" / f"{row_id}.{trial}.out"
        report.parent.mkdir(parents=True, exist_ok=True)
        lazy = overrides.pop("lazy", "on" if "high-compression" in row_id or "medium-auto" in row_id else "off")
        if expected_outcome == "expected_infeasible":
            report.write_text(cross.REAL_REFUSAL_STDOUT, encoding="utf-8")
            report.with_suffix(".err").write_text(self.refusal_stderr, encoding="utf-8")
            (self.root / label / "outputs").mkdir(parents=True, exist_ok=True)
        else:
            report.write_text(f"chart_spr_search:\n  lazy_policy_resolved: {lazy}\n", encoding="utf-8")
        recorded_report = (
            report.relative_to(self.root / label)
            if label != "phase0" else report
        )
        row = {name: "NA" for name in cross.RAW_COLUMNS}
        row.update(
            row_id=row_id, fixture=fixture, method=method, status="ok",
            validation_status="ok", requested_workers=worker,
            resolved_workers=resolved, worker_policy=policy, trial_index=str(trial),
            runner_outcome="exited", runner_exit_code="0", exit_code="0",
            term_signal="0", core_dumped="0", timed_out="0", monitor_error="0",
            rss_limit_enabled="1", rss_limit_observed="0", rss_limit_exceeded="0",
            rss_limit_trigger_bytes="0", rss_limit_term_sent="0",
            rss_limit_kill_sent="0",
            wall_clock_s=wall, user_cpu_s="1", system_cpu_s="0.1",
            max_rss_kb="100000", peak_sampled_rss_kb="100000", peak_sampled_swap_kb="0",
            process_rss_limit_bytes=str(16 * 1024**3),
            manifest_rss_limit_bytes=str(16 * 1024**3),
            configured_chart_memory_budget=str(12 * 1024**3),
            input_sha256=input_sha, refseq_sha256=refseq_sha,
            search_semantic_sha256=search, output_semantic_sha256=output,
            trial_semantic_sha256=trial_hash, canonical_argv_sha256=argv,
            canonical_digest=trial_hash, iterations=iterations, seed=seed,
            acceptance=(manifest["acceptance"] if manifest else "synthetic"),
            objective=(manifest["objective"] if manifest else "synthetic"),
            candidate_source=(manifest["candidate_source"] if manifest else "synthetic"),
            candidates_generated=str(candidate_count),
            candidates_scored=str(candidate_count),
            exact_verifications=str(exact_count),
            accepted_moves="3" if iterations == "3" else "0",
            initial_validated_parsimony_min="100", final_validated_parsimony_min="90",
            best_reported_objective="90", candidate_generation_ms="100",
            exact_initialization_ms="50", initial_chart_construction_ms="50",
            local_scoring_ms="100",
            exact_verification_ms="50" if exact_count else "0",
            accepted_rebuild_ms="0", total_ms="200",
            chart_cache_resident_bytes="1000000",
            peak_concurrent_exact_verifiers=str(peak_exact),
            chart_axis_exact_candidate_active_worker_high_water=str(peak_exact),
            exact_candidate_admission_batches=str(int(exact_count > 0)),
            exact_candidate_parallel_batches=str(candidate_parallel),
            exact_candidate_inner_parallel_batches=str(inner_parallel),
            exact_candidate_memory_limited_batches="0",
            exact_candidate_peak_admitted_bytes=(
                "1000000" if exact_count else "0"
            ),
            exact_candidate_peak_projected_resident_bytes=(
                "2000000" if exact_count else "0"
            ),
            exact_candidate_queued_for_memory_ms="0",
            exact_candidate_timing_count=str(exact_count),
            exact_candidate_verification_ms_min=("10" if exact_count else "0"),
            exact_candidate_verification_ms_mean=("10" if exact_count else "0"),
            exact_candidate_verification_ms_max=("10" if exact_count else "0"),
            report_path=str(recorded_report),
        )
        if method == cross.METHOD_NATIVE:
            row.update(search_semantic_sha256="-", configured_chart_memory_budget="product-default")
        if expected_outcome == "timeout":
            row.update(
                status="timeout", validation_status="not_run",
                runner_outcome="timed_out", runner_exit_code="124",
                exit_code="0", timed_out="1", resolved_workers="NA",
                worker_policy="timeout_unobserved",
                search_semantic_sha256="-", output_semantic_sha256="-",
                trial_semantic_sha256="-", canonical_digest="-",
            )
        elif expected_outcome == "expected_infeasible":
            row.update(
                status="expected_infeasible", validation_status="not_applicable",
                runner_exit_code="1", exit_code="1", resolved_workers="NA",
                worker_policy="unobserved", search_semantic_sha256="-",
                output_semantic_sha256="-", trial_semantic_sha256="-",
                canonical_digest="-", final_validated_parsimony_min="NA",
                best_reported_objective="NA",
            )
        row.update(overrides)
        return row

    def write_raw(self, label: str, ids: set[str], reps: int, times: dict[str, str] | None = None,
                  row_overrides: dict[str, dict[str, str]] | None = None) -> Path:
        path = self.root / label / "raw_trials.tsv"
        path.parent.mkdir(parents=True, exist_ok=True)
        rows = []
        for row_id in sorted(ids):
            for trial in range(1, reps + 1):
                overrides = dict((row_overrides or {}).get(row_id, {}))
                rows.append(self.raw_row(row_id, trial, label=label, wall=(times or {}).get(row_id, "1"), **overrides))
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=cross.RAW_COLUMNS, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        self.raw_paths[label] = path
        self.raw_inputs[label] = [path]
        return path

    def _write_phase0(self) -> None:
        phase0_raw_columns = tuple(
            field
            for field in cross.RAW_COLUMNS
            if field in cross.PHASE0_LEGACY_EXACT_TIMING_FIELDS
            or field not in cross.ADMISSION_FIELDS
        )
        gate_ids = {
            cross.MEDIUM_DENSE.format(1), cross.SMALL_DENSE.format(1),
            cross.SMALL_EXACT.format(1), cross.MEDIUM_CACHE.format(1),
            cross.MEDIUM_LAZY.format(1),
            "p0-medium-primary32k4-sampled-tree-fixed-topology-w8",
            "p0-medium-primary32k4-hybrid-exact-w8",
        }
        real_ids = {
            row_id
            for row_id, row in self.base_rows.items()
            if row["expected_outcome"] == "expected_infeasible"
        }
        captures: dict[str, set[str]] = {}
        for row_id, manifest in self.base_rows.items():
            if row_id in real_ids:
                continue
            captures.setdefault(manifest["run_group"], set()).add(row_id)
        for capture_id, ids in captures.items():
            path = (
                self.root / "bootstrap-phase0" / "captures" / capture_id
                / "raw_trials.tsv"
            )
            path.parent.mkdir(parents=True, exist_ok=True)
            rows: list[dict[str, str]] = []
            classifications: list[dict[str, str]] = []
            for row_id in sorted(ids):
                manifest = self.base_rows[row_id]
                outcome = manifest["expected_outcome"]
                repetitions = (
                    1 if outcome in ("timeout", "expected_infeasible")
                    else 5 if row_id in gate_ids
                    else 3
                )
                worker = cross.manifest_worker(manifest)
                method = manifest["method"]
                semantic_fixture = next(
                    fixture
                    for fixture in ("small", "medium")
                    if manifest["primary_sha256"] == digest(f"input:{fixture}")
                )
                source_fixture = f"{semantic_fixture}.pb.gz"
                source_row_id = f"{source_fixture}/{method}@{worker}"
                source_argv = digest(
                    f"pre-manifest-argv:{capture_id}:{method}:{worker}"
                )
                source_trial = digest(
                    f"pre-manifest-trial:{capture_id}:{method}:{worker}"
                )
                for trial in range(1, repetitions + 1):
                    wall = "600" if outcome == "timeout" else "1"
                    row = self.raw_row(row_id, trial, label="phase0", wall=wall)
                    row["fixture"] = source_fixture
                    row["row_id"] = source_row_id
                    if method != cross.METHOD_NATIVE:
                        row["canonical_argv_sha256"] = source_argv
                        if outcome == "ok":
                            row["trial_semantic_sha256"] = source_trial
                            row["canonical_digest"] = source_trial
                    if outcome == "timeout":
                        row.update(
                            runner_outcome="timeout",
                            runner_exit_code="124",
                            exit_code="-1",
                            term_signal="15",
                        )
                    if row_id == cross.MEDIUM_DENSE.format(1):
                        row["local_scoring_ms"] = "100"
                    rows.append(row)
                classifications.append(
                    {
                        "method": method,
                        "requested_workers": worker,
                        "row_id": row_id,
                        "measured_trial_target": str(
                            5 if outcome == "timeout" else repetitions
                        ),
                        "observed_trial_count": str(repetitions),
                        "outcome": outcome,
                        "evidence_kind": (
                            "timeout_characterization_non_performance"
                            if outcome == "timeout"
                            else "finite_performance"
                        ),
                        "characterization_stage": "characterization",
                        "repeat_stage": (
                            "-"
                            if outcome == "timeout"
                            else f"repeats/{method}--{worker}"
                        ),
                        "trial1_native_peer": (
                            "self"
                            if method == cross.METHOD_NATIVE
                            else "characterization:sample_explore_merge@native"
                        ),
                    }
                )
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=phase0_raw_columns,
                    delimiter="\t",
                    lineterminator="\n",
                    extrasaction="ignore",
                )
                writer.writeheader()
                writer.writerows(rows)
            classification_path = path.with_name("trial_classification.tsv")
            with classification_path.open(
                "w", encoding="utf-8", newline=""
            ) as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=cross.PHASE0_CLASSIFICATION_COLUMNS,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(
                    sorted(
                        classifications,
                        key=lambda row: (
                            row["method"], row["requested_workers"]
                        ),
                    )
                )
            self.phase0_capture_by_name[capture_id] = path
            self.phase0_classification_by_name[capture_id] = classification_path
        self.phase0_capture_paths = [
            self.phase0_capture_by_name[name]
            for name in sorted(self.phase0_capture_by_name)
        ]
        self.phase0_classification_paths = [
            self.phase0_classification_by_name[name]
            for name in sorted(self.phase0_classification_by_name)
        ]
        strict_real = (
            self.root / "bootstrap-phase0" / "strict-real-smoke-final"
            / "raw_trials.tsv"
        )
        strict_real.parent.mkdir(parents=True, exist_ok=True)
        (strict_real.parent / "outputs").mkdir()
        real_rows: list[dict[str, str]] = []
        for row_id in sorted(real_ids):
            row = self.raw_row(row_id, 1, label="phase0")
            source_report = Path(row["report_path"])
            report = strict_real.parent / "logs" / source_report.name
            report.parent.mkdir(exist_ok=True)
            report.write_bytes(source_report.read_bytes())
            report.with_suffix(".err").write_bytes(
                source_report.with_suffix(".err").read_bytes()
            )
            row["report_path"] = str(report.relative_to(strict_real.parent))
            real_rows.append(row)
        with strict_real.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=phase0_raw_columns,
                delimiter="\t",
                lineterminator="\n",
                extrasaction="ignore",
            )
            writer.writeheader()
            writer.writerows(real_rows)
        self.phase0_strict_real = strict_real
        self.phase0_inputs = [*self.phase0_capture_paths, strict_real]
        lazy_row_id = cross.MEDIUM_LAZY.format(1)
        for path in self.phase0_capture_paths:
            with path.with_name("trial_classification.tsv").open(
                encoding="utf-8", newline=""
            ) as handle:
                classified_ids = {
                    row["row_id"]
                    for row in csv.DictReader(handle, delimiter="\t")
                }
            if lazy_row_id in classified_ids:
                self.phase0 = path
                break
        else:
            raise AssertionError("synthetic Phase-0 lazy capture is absent")
        self.phase0_ledger_member.write_text(
            "synthetic immutable Phase-0 closure member\n", encoding="utf-8"
        )
        self.refresh_phase0_ledger()

    def refresh_phase0_ledger(self) -> None:
        assert self.phase0_strict_real is not None
        members = [
            *self.phase0_capture_paths,
            *self.phase0_classification_paths,
            self.phase0_strict_real,
            self.phase0_ledger_member,
        ]
        records = sorted(
            [
                (
                hashlib.sha256(path.read_bytes()).hexdigest(),
                "repo://" + path.relative_to(self.root).as_posix(),
                )
                for path in members
            ],
            key=lambda record: record[1],
        )
        with self.phase0_artifact_ledger.open(
            "w", encoding="utf-8", newline=""
        ) as handle:
            writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
            writer.writerow(("sha256", "uri"))
            writer.writerows(records)
        self.phase0_artifact_ledger_sha = self._seal(
            self.phase0_artifact_ledger
        )

    def _group(self, name: str, workers: set[str]) -> set[str]:
        return {rid for rid, row in self.manifest_rows.items() if row["run_group"] == name and (cross.manifest_worker(row) == "native" or cross.manifest_worker(row) in workers)}

    def _write_runs(self) -> None:
        p1 = {cross.MEDIUM_DENSE.format(1), cross.SMALL_DENSE.format(1), cross.SMALL_EXACT.format(1), cross.MEDIUM_CACHE.format(1), cross.MEDIUM_LAZY.format(1)}
        self.write_raw("phase1", p1, 5, row_overrides={cross.MEDIUM_DENSE.format(1): {"local_scoring_ms": "100"}})
        self.write_raw("phase2", {cross.MEDIUM_DENSE.format(1), cross.SMALL_EXACT.format(1)}, 5,
                       times={cross.SMALL_EXACT.format(1): "0.7"},
                       row_overrides={cross.MEDIUM_DENSE.format(1): {"local_scoring_ms": "65"}})
        phase3_ids = {
            cross.SMALL_DENSE.format(1),
            cross.SMALL_DENSE.format(8),
            cross.MEDIUM_DENSE.format(1),
        }
        self.write_raw(
            "phase3",
            phase3_ids,
            5,
            times={
                cross.SMALL_DENSE.format(1): "1",
                cross.SMALL_DENSE.format(8): "1.04",
            },
            row_overrides={
                cross.MEDIUM_DENSE.format(1): {"local_scoring_ms": "100"}
            },
        )
        phase4_ids = {
            template.format(worker)
            for template in (cross.MEDIUM_DENSE, cross.MEDIUM_CACHE)
            for worker in (1, 2, 4, 8)
        }
        phase4_overrides: dict[str, dict[str, str]] = {}
        for phase4_worker in (1, 2, 4, 8):
            phase4_overrides[cross.MEDIUM_DENSE.format(phase4_worker)] = {
                "local_scoring_ms": "100" if phase4_worker == 1 else "40"
            }
            phase4_overrides[cross.MEDIUM_CACHE.format(phase4_worker)] = {
                "initial_chart_construction_ms": "100" if phase4_worker == 1 else "40"
            }
        self.write_raw(
            "phase4", phase4_ids, 5, row_overrides=phase4_overrides
        )
        self.write_raw("phase5", {cross.MEDIUM_EXACT.format(1), cross.MEDIUM_EXACT.format(8)}, 5,
                       times={cross.MEDIUM_EXACT.format(1): "100", cross.MEDIUM_EXACT.format(8): "100"},
                       row_overrides={cross.MEDIUM_EXACT.format(1): {"exact_initialization_ms": "50", "exact_verification_ms": "50"}, cross.MEDIUM_EXACT.format(8): {"exact_initialization_ms": "20", "exact_verification_ms": "20"}})

        phase6_ids = {
            cross.MEDIUM_EXACT.format(worker) for worker in (1, 2, 4, 8)
        } | {
            row_id
            for row_id, row in self.manifest_rows.items()
            if row["run_group"] in ("p0-primary-physical", "p0-stress-physical")
            and row["method"] != cross.METHOD_NATIVE
        }
        phase6_overrides: dict[str, dict[str, str]] = {}
        for row_id in phase6_ids:
            row = self.manifest_rows[row_id]
            if row["run_group"] == "p0-primary-physical" and row["method"] == cross.METHOD_EXACT:
                phase6_worker = int(cross.manifest_worker(row))
                phase6_overrides[row_id] = {
                    "exact_verification_ms": {
                        1: "100",
                        2: "80",
                        4: "60",
                        8: "50",
                    }[phase6_worker]
                }
        self.write_raw(
            "phase6", phase6_ids, 3, row_overrides=phase6_overrides
        )

        high = {f"phase7-lazy-high-compression-{p}-w{w}" for p in ("off", "on", "auto") for w in (1, 8)}
        phase7_group_complete = {
            row_id for row_id, row in self.supplement_rows["phase7"].items()
            if row["requested_workers"] in ("1", "8")
        }
        high_times = {rid: ("1.5" if "-off-" in rid else ("0.6" if rid.endswith("w8") else "1")) for rid in high}
        self.write_raw("phase7-high", phase7_group_complete, 5, times=high_times,
                       row_overrides={rid: {"lazy": "on"} for rid in high if "-auto-" in rid})
        small = {cross.SMALL_DENSE.format(w) for w in (1, 8)} | {f"phase7-lazy-completion-tree0-on-w{w}" for w in (1, 8)}
        self.write_raw("phase7-small", small, 5, times={rid: ("1.05" if rid.endswith("on-w8") else "1") for rid in small})
        dense = {f"phase7-lazy-dense-favoring-{p}-w{w}" for p in ("off", "on", "auto") for w in (1, 8)}
        medium = {cross.MEDIUM_DENSE.format(w) for w in (1, 8)} | {cross.MEDIUM_LAZY.format(w) for w in (1, 8)} | {f"phase7-lazy-completion-medium-auto-w{w}" for w in (1, 8)}
        dense_times = {rid: ("1" if "-off-" in rid or "-auto-" in rid else "1.3") for rid in dense}
        medium_times = {rid: ("1" if "lazy64" in rid or "medium-auto" in rid else "1.3") for rid in medium}
        overrides = {rid: {"lazy": "off"} for rid in dense if "-auto-" in rid}
        overrides.update({rid: {"lazy": "on"} for rid in medium if "medium-auto" in rid})
        self.write_raw("phase7-auto", dense | medium, 5, times=dense_times | medium_times, row_overrides=overrides)

        p8 = {f"phase8-generation-tree0-off-w{w}" for w in (1, 8)}
        self.write_raw("phase8-generation", p8, 5, times={rid: "0.9" for rid in p8},
                       row_overrides={next(rid for rid in p8 if rid.endswith("w1")): {"candidate_generation_ms": "100"}, next(rid for rid in p8 if rid.endswith("w8")): {"candidate_generation_ms": "40"}})
        self.write_raw("phase8-end-to-end", p8, 5, times={rid: "1" for rid in p8})

        final_scaling = self._group(
            "p0-primary-physical", {"1", "2", "4", "8"}
        )
        scaling_times: dict[str, str] = {}
        scaling_overrides: dict[str, dict[str, str]] = {}
        for rid in final_scaling:
            row = self.manifest_rows[rid]
            scaling_worker = cross.manifest_worker(row)
            if row["method"] == cross.METHOD_NATIVE:
                scaling_times[rid] = "1"
            elif row["method"] == cross.METHOD_EXACT:
                scaling_times[rid] = {
                    "1": "0.8",
                    "2": "0.65",
                    "4": "0.5",
                    "8": "0.4",
                }[scaling_worker]
                scaling_overrides[rid] = {
                    "initial_chart_construction_ms": "100" if scaling_worker == "1" else "50",
                    "local_scoring_ms": "100" if scaling_worker == "1" else "50",
                    "exact_verification_ms": "100" if scaling_worker == "1" else "50",
                }
            else:
                scaling_times[rid] = "0.9"
        self.write_raw(
            "final-scaling",
            final_scaling,
            3,
            times=scaling_times,
            row_overrides=scaling_overrides,
        )

        primary = self._group("p0-primary-physical", {"1", "8"})
        p_times = {}
        p_overrides = {}
        for rid in primary:
            row = self.manifest_rows[rid]
            primary_worker = cross.manifest_worker(row)
            if row["method"] == cross.METHOD_NATIVE:
                p_times[rid] = "1"
            elif row["method"] == cross.METHOD_EXACT:
                p_times[rid] = "0.8" if primary_worker == "1" else "0.4"
                p_overrides[rid] = {"initial_chart_construction_ms": "100" if primary_worker == "1" else "50", "local_scoring_ms": "100" if primary_worker == "1" else "50", "exact_verification_ms": "100" if primary_worker == "1" else "50"}
            else:
                p_times[rid] = "0.9"
        self.write_raw("final-primary", primary, 5, times=p_times, row_overrides=p_overrides)

        smt = self._group("p0-primary-smt", {"1", "2", "4", "8", "16", "auto"})
        smt_wall = {"native": "1", "1": "0.5", "2": "0.3", "4": "0.2", "8": "0.15", "16": "0.14", "auto": "0.15"}
        self.write_raw("final-smt", smt, 3, times={rid: smt_wall[cross.manifest_worker(self.manifest_rows[rid])] for rid in smt})
        small_auto = self._group("p0-small-auto", {"1", "auto"})
        self.write_raw("final-small-auto", small_auto, 5, times={rid: ("1" if cross.manifest_worker(self.manifest_rows[rid]) in ("native", "1") else "1.05") for rid in small_auto})
        unpinned = self._group("p0-primary-smt", {"auto"})
        self.write_raw("final-unpinned-auto", unpinned, 5, times={rid: ("1" if cross.manifest_worker(self.manifest_rows[rid]) == "native" else "0.9") for rid in unpinned})
        default = self._group("p0-primary-smt", {"auto", "default"})
        self.write_raw(
            "final-default-auto",
            default,
            5,
            times={rid: "1" for rid in default},
        )

        stress = self._group("p0-stress-physical", {"1", "8"})
        stress_times, stress_overrides = {}, {}
        for rid in stress:
            stress_worker = cross.manifest_worker(self.manifest_rows[rid])
            stress_times[rid] = "1" if stress_worker in ("native", "1") else "0.9"
            if stress_worker not in ("native",):
                stress_overrides[rid] = {"total_ms": "200"}
        self.write_raw("final-stress", stress, 3, times=stress_times, row_overrides=stress_overrides)

        real = self._group("real-bounded", {"1", "8"})
        real_overrides = {rid: {"status": "expected_infeasible", "validation_status": "not_applicable", "runner_exit_code": "1", "exit_code": "1"} for rid in real}
        self.write_raw("final-real", real, 3, row_overrides=real_overrides)

        p9 = {f"phase9-local-commit-seed{s}-w{w}" for s in (1, 7, 19) for w in (1, 8)}
        p9_overrides = {rid: {"accepted_rebuild_ms": "200" if rid.endswith("w1") else "100", "total_ms": "1000", "accepted_moves": "3"} for rid in p9}
        p9_path = self.write_raw("phase9", p9, 3, row_overrides=p9_overrides)
        metadata = p9_path.parent / "phase9-run-metadata.json"
        metadata.write_text(json.dumps({
            "schema": "wric_phase9_benchmark_run",
            "schema_version": 2,
            "role": "same_revision_phase9_current_measurement",
            "run_group": "phase9-local-commit",
            "seeds": [1, 7, 19],
            "workers": [1, 8],
            "repetitions": 3,
            "warmups_per_row": 1,
            "full_canonical": True,
            "raw_trials_sha256": hashlib.sha256(p9_path.read_bytes()).hexdigest(),
            "raw_trial_rows": 18,
            "row_ids": sorted(p9),
        }, sort_keys=True) + "\n", encoding="utf-8")
        ledger = p9_path.parent / "phase9-run-artifacts.tsv"
        with ledger.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=("sha256", "bytes", "path"), delimiter="\t", lineterminator="\n")
            writer.writeheader()
            for member in (p9_path, metadata):
                writer.writerow({"sha256": hashlib.sha256(member.read_bytes()).hexdigest(), "bytes": member.stat().st_size, "path": member.name})
        ledger_sha = hashlib.sha256(ledger.read_bytes()).hexdigest()
        ledger.with_name(ledger.name + ".sha256").write_text(f"{ledger_sha}  {ledger.name}\n", encoding="ascii")
        self.phase9_ledger_sha = ledger_sha

    def command(
        self,
        *,
        omit: str | None = None,
        omit_raw_anchor: str | None = None,
        raw_anchor_override: dict[str, str] | None = None,
        omit_supplement: str | None = None,
        omit_supplement_anchor: str | None = None,
        supplement_anchor_override: dict[str, str] | None = None,
        ledger_anchor: str | None = None,
        phase0_ledger_anchor: str | None = None,
    ) -> list[str]:
        command = [
            "evaluate",
            "--base-manifest", str(self.base),
            "--expected-base-sha256", self.base_sha,
            "--base-repo-root", str(self.root),
            "--working-repo-root", str(ROOT),
            "--phase0-artifact-ledger", str(self.phase0_artifact_ledger),
            "--expected-phase0-artifact-ledger-sha256",
            (
                self.phase0_artifact_ledger_sha
                if phase0_ledger_anchor is None else phase0_ledger_anchor
            ),
        ]
        for label in cross.SUPPLEMENT_SPECS:
            if label != omit_supplement:
                command.extend(("--supplement", f"{label}={self.supplements[label]}"))
            if label != omit_supplement_anchor:
                value = (supplement_anchor_override or {}).get(label, self.supplement_shas[label])
                command.extend(("--expected-supplement-sha256", f"{label}={value}"))
        for path in self.phase0_inputs:
            command.extend(("--phase0-raw", str(path)))
            if omit_raw_anchor != "phase0":
                value = (raw_anchor_override or {}).get(
                    "phase0", hashlib.sha256(path.read_bytes()).hexdigest()
                )
                command.extend(("--expected-raw-sha256", f"phase0={value}"))
        for label in cross.RUN_LABELS:
            if label == omit:
                continue
            for path in self.raw_inputs[label]:
                command.extend(("--run", f"{label}={path}"))
                if label != omit_raw_anchor:
                    value = (raw_anchor_override or {}).get(
                        label, hashlib.sha256(path.read_bytes()).hexdigest()
                    )
                    command.extend(("--expected-raw-sha256", f"{label}={value}"))
        command.extend((
            "--expected-phase9-run-ledger-sha256",
            self.phase9_ledger_sha if ledger_anchor is None else ledger_anchor,
        ))
        return command

    def mutate(
        self,
        label: str,
        predicate: Callable[[Mapping[str, str]], bool],
        changes: dict[str, str],
    ) -> None:
        path = self.raw_paths[label]
        with path.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            header = reader.fieldnames
            assert header is not None
            rows = list(reader)
        changed = 0
        for row in rows:
            if predicate(row):
                row.update(changes)
                changed += 1
        assert changed
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=header, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)

    def remove_rows(
        self,
        label: str,
        predicate: Callable[[Mapping[str, str]], bool],
    ) -> None:
        path = self.raw_paths[label]
        with path.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None
            header = reader.fieldnames
            rows = list(reader)
        retained = [row for row in rows if not predicate(row)]
        assert len(retained) < len(rows)
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=header,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(retained)

    def remove_columns(self, label: str, columns: set[str]) -> None:
        path = self.raw_paths[label]
        with path.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None and columns <= set(reader.fieldnames)
            header = [field for field in reader.fieldnames if field not in columns]
            rows = list(reader)
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle, fieldnames=header, delimiter="\t", lineterminator="\n",
                extrasaction="ignore",
            )
            writer.writeheader()
            writer.writerows(rows)

    def remove_column(self, label: str, column: str) -> None:
        self.remove_columns(label, {column})

    def split_raw(self, label: str) -> None:
        source = self.raw_paths[label]
        with source.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None
            header = reader.fieldnames
            rows = list(reader)
        ids = sorted({row["row_id"] for row in rows})
        left_ids = set(ids[::2])
        outputs = [source.with_name("raw-part-a.tsv"), source.with_name("raw-part-b.tsv")]
        partitions = [
            [row for row in rows if row["row_id"] in left_ids],
            [row for row in rows if row["row_id"] not in left_ids],
        ]
        assert all(partitions)
        for path, partition in zip(outputs, partitions, strict=True):
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=header, delimiter="\t", lineterminator="\n")
                writer.writeheader()
                writer.writerows(partition)
        self.raw_inputs[label] = outputs

    def duplicate_across_repeated_inputs(self, label: str) -> None:
        self.split_raw(label)
        first, second = self.raw_inputs[label]
        with first.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None
            header = reader.fieldnames
            duplicate = next(iter(reader))
        with second.open(encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle, delimiter="\t"))
        rows.append(duplicate)
        with second.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=header, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)

    def append_timeout_trial(self) -> None:
        for path in self.phase0_capture_paths:
            with path.open(encoding="utf-8", newline="") as handle:
                reader = csv.DictReader(handle, delimiter="\t")
                assert reader.fieldnames is not None
                header = reader.fieldnames
                rows = list(reader)
            source = next(
                (row for row in rows if row["status"] == "timeout"),
                None,
            )
            if source is None:
                continue
            extra = dict(source)
            extra["trial_index"] = "2"
            rows.append(extra)
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=header,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(rows)
            self.refresh_phase0_ledger()
            return
        raise AssertionError("synthetic Phase-0 closure contains no timeout row")

    def remove_phase0_rows(
        self,
        predicate: Callable[[Mapping[str, str]], bool],
    ) -> None:
        removed = 0
        for path in self.phase0_capture_paths:
            classification_path = path.with_name("trial_classification.tsv")
            with classification_path.open(
                encoding="utf-8", newline=""
            ) as handle:
                classification_reader = csv.DictReader(handle, delimiter="\t")
                assert classification_reader.fieldnames is not None
                classification_header = classification_reader.fieldnames
                classifications = list(classification_reader)
            projected_ids = {
                (row["method"], row["requested_workers"]): row["row_id"]
                for row in classifications
            }
            with path.open(encoding="utf-8", newline="") as handle:
                reader = csv.DictReader(handle, delimiter="\t")
                assert reader.fieldnames is not None
                header = reader.fieldnames
                rows = list(reader)
            retained = []
            for row in rows:
                projected = dict(row)
                projected["row_id"] = projected_ids[
                    (row["method"], row["requested_workers"])
                ]
                if not predicate(projected):
                    retained.append(row)
            removed += len(rows) - len(retained)
            if len(retained) == len(rows):
                continue
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=header,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(retained)
            observed = {
                (row["method"], row["requested_workers"]): 0
                for row in retained
            }
            for row in retained:
                observed[(row["method"], row["requested_workers"])] += 1
            classifications = [
                row
                for row in classifications
                if (row["method"], row["requested_workers"]) in observed
            ]
            for row in classifications:
                count = observed[(row["method"], row["requested_workers"])]
                row["observed_trial_count"] = str(count)
                if row["outcome"] == "ok":
                    row["measured_trial_target"] = str(count)
            with classification_path.open(
                "w", encoding="utf-8", newline=""
            ) as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=classification_header,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(classifications)
        assert removed
        self.refresh_phase0_ledger()

    def duplicate_phase0_row_trial_across_captures(self) -> None:
        selected: tuple[Path, Path, dict[str, str]] | None = None
        for source in self.phase0_capture_paths:
            with source.open(encoding="utf-8", newline="") as handle:
                source_rows = list(csv.DictReader(handle, delimiter="\t"))
            for target in self.phase0_capture_paths:
                if target == source:
                    continue
                with target.with_name("trial_classification.tsv").open(
                    encoding="utf-8", newline=""
                ) as handle:
                    target_keys = {
                        (row["method"], row["requested_workers"])
                        for row in csv.DictReader(handle, delimiter="\t")
                    }
                duplicate = next(
                    (
                        row
                        for row in source_rows
                        if (row["method"], row["requested_workers"])
                        not in target_keys
                    ),
                    None,
                )
                if duplicate is not None:
                    selected = (source, target, duplicate)
                    break
            if selected is not None:
                break
        assert selected is not None
        source, target, duplicate = selected
        with target.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None
            header = reader.fieldnames
            rows = list(reader)
        rows.append(duplicate)
        with target.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=header,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(rows)
        source_classification = source.with_name("trial_classification.tsv")
        with source_classification.open(
            encoding="utf-8", newline=""
        ) as handle:
            source_reader = csv.DictReader(handle, delimiter="\t")
            copied_classification = next(
                row
                for row in source_reader
                if (
                    row["method"], row["requested_workers"]
                ) == (duplicate["method"], duplicate["requested_workers"])
            )
        target_classification = target.with_name("trial_classification.tsv")
        with target_classification.open(
            encoding="utf-8", newline=""
        ) as handle:
            target_reader = csv.DictReader(handle, delimiter="\t")
            assert target_reader.fieldnames is not None
            classification_header = target_reader.fieldnames
            classification_rows = list(target_reader)
        copied_classification["observed_trial_count"] = "1"
        copied_classification["measured_trial_target"] = "1"
        classification_rows.append(copied_classification)
        with target_classification.open(
            "w", encoding="utf-8", newline=""
        ) as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=classification_header,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(classification_rows)
        self.refresh_phase0_ledger()

    def rewrite_phase0_ledger(
        self,
        transform: Callable[[list[list[str]]], None],
    ) -> None:
        with self.phase0_artifact_ledger.open(
            encoding="utf-8", newline=""
        ) as handle:
            rows = list(csv.reader(handle, delimiter="\t"))
        transform(rows)
        with self.phase0_artifact_ledger.open(
            "w", encoding="utf-8", newline=""
        ) as handle:
            csv.writer(handle, delimiter="\t", lineterminator="\n").writerows(
                rows
            )
        self.phase0_artifact_ledger_sha = self._seal(
            self.phase0_artifact_ledger
        )

    def fabricate_supplement_row(self, label: str) -> None:
        path = self.supplements[label]
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
        split = next(index for index, line in enumerate(lines) if not line.startswith("# "))
        preamble = lines[:split]
        with path.open(encoding="utf-8", newline="") as handle:
            for _ in range(split):
                next(handle)
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None
            header = reader.fieldnames
            rows = list(reader)
        fabricated = dict(rows[0])
        fabricated["row_id"] = f"fabricated-{label}-row"
        rows.append(fabricated)
        with path.open("w", encoding="utf-8", newline="") as handle:
            handle.writelines(preamble)
            writer = csv.DictWriter(handle, fieldnames=header, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        self.supplement_shas[label] = self._seal(path)

    def append_disallowed_group_row(self, label: str) -> None:
        path = self.raw_paths[label]
        with path.open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            assert reader.fieldnames is not None
            header = reader.fieldnames
            rows = list(reader)
        for trial in range(1, 6):
            rows.append(self.raw_row("p0-native-medium-physical-i1-m50", trial, label=label))
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=header, delimiter="\t", lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)


class Invocation:
    def __init__(
        self,
        returncode: int,
        stdout: str,
        stderr: str,
        phase4_calls: list[tuple[object, ...]],
        phase9_calls: list[tuple[object, ...]],
    ) -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.phase4_calls = phase4_calls
        self.phase9_calls = phase9_calls


class CrossPhaseAcceptanceTest(unittest.TestCase):
    def run_case(
        self,
        data: SyntheticEvidence,
        *,
        phase4_failure: str | None = None,
        phase4_status: str = "pass",
        phase9_failure: str | None = None,
        **command_options: object,
    ) -> Invocation:
        phase4_calls: list[tuple[object, ...]] = []
        phase9_calls: list[tuple[object, ...]] = []

        def phase4_validator(*args: object) -> dict[str, object]:
            phase4_calls.append(args)
            if phase4_failure is not None:
                raise cross.AcceptanceError(phase4_failure)
            return {
                "schema_version": 1,
                "status": phase4_status,
                "gates": [
                    {"name": "synthetic_deep_phase4", "status": "pass"}
                ],
            }

        def phase9_validator(*args: object) -> dict[str, object]:
            phase9_calls.append(args)
            if phase9_failure is not None:
                raise cross.AcceptanceError(phase9_failure)
            return {
                "schema": "wric.phase9.acceptance",
                "schema_version": 2,
                "status": "pass",
                "gates": [{"name": "synthetic_deep_phase9", "status": "pass"}],
            }

        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            returncode = cross.main(
                data.command(**command_options),  # type: ignore[arg-type]
                phase4_validator=phase4_validator,
                phase9_validator=phase9_validator,
            )
        return Invocation(
            returncode,
            stdout.getvalue(),
            stderr.getvalue(),
            phase4_calls,
            phase9_calls,
        )

    def test_complete_group_shaped_matrix_passes_without_writes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-positive-") as name:
            data = SyntheticEvidence(Path(name))
            source_ids: list[str] = []
            for raw_path, classification_path in zip(
                data.phase0_capture_paths,
                data.phase0_classification_paths,
                strict=True,
            ):
                with raw_path.open(encoding="utf-8", newline="") as handle:
                    raw_rows = list(csv.DictReader(handle, delimiter="\t"))
                source_ids.extend(row["row_id"] for row in raw_rows)
                self.assertTrue(
                    all(not row["row_id"].startswith("p0-") for row in raw_rows)
                )
                with classification_path.open(
                    encoding="utf-8", newline=""
                ) as handle:
                    reader = csv.DictReader(handle, delimiter="\t")
                    self.assertEqual(
                        tuple(reader.fieldnames or ()),
                        cross.PHASE0_CLASSIFICATION_COLUMNS,
                    )
            self.assertLess(len(set(source_ids)), len(source_ids))
            assert data.phase0_strict_real is not None
            with data.phase0_strict_real.open(
                encoding="utf-8", newline=""
            ) as handle:
                self.assertEqual(
                    {
                        row["row_id"]
                        for row in csv.DictReader(handle, delimiter="\t")
                    },
                    {
                        "p0-real20d-preflight-grammar-exact-w1",
                        "p0-real20d-preflight-grammar-exact-w8",
                    },
                )
            before = {
                path: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in data.root.rglob("*") if path.is_file()
            }
            result = self.run_case(data)
            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertEqual(payload["status"], "pass")
            self.assertEqual(payload["schema_version"], cross.SCHEMA_VERSION)
            self.assertEqual(payload["run_labels"], list(cross.RUN_LABELS))
            self.assertEqual(payload["phase4_acceptance"]["status"], "pass")
            self.assertEqual(payload["phase9_acceptance"]["status"], "pass")
            self.assertEqual(len(result.phase4_calls), 1)
            phase4_evidence = result.phase4_calls[0][0]
            self.assertIsInstance(phase4_evidence, cross.RawEvidence)
            assert isinstance(phase4_evidence, cross.RawEvidence)
            self.assertEqual(phase4_evidence.label, "phase4")
            self.assertEqual(
                result.phase4_calls[0][1], data.raw_paths["phase3"].resolve()
            )
            self.assertEqual(result.phase4_calls[0][2], ROOT)
            phase4_memory = result.phase4_calls[0][3]
            self.assertIsInstance(phase4_memory, int)
            assert isinstance(phase4_memory, int)
            self.assertGreater(phase4_memory, 0)
            self.assertEqual(len(result.phase9_calls), 1)
            self.assertEqual(result.phase9_calls[0][2], data.root)
            self.assertEqual(result.phase9_calls[0][3], ROOT)
            self.assertEqual(result.phase9_calls[0][4], data.phase9_ledger_sha)
            observed_phase7 = {
                row.split("\t", 1)[0]
                for row in data.raw_paths["phase7-high"].read_text().splitlines()[1:]
            }
            self.assertGreater(len(observed_phase7), 6)
            self.assertTrue(any(
                gate.get("comparison") == "finite_completion_after_all_timeout"
                for gate in payload["gates"]
            ))
            self.assertEqual(
                before,
                {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in before},
            )

    def test_phase2_serial_gate_is_strict(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-ratio-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate("phase2", lambda row: row["row_id"] == cross.MEDIUM_DENSE.format(1), {"local_scoring_ms": "71"})
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("phase2_medium_local_vs_phase0", result.stderr)

    def test_phase4_matrix_and_deep_acceptance_are_mandatory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p4-deep-") as name:
            result = self.run_case(
                SyntheticEvidence(Path(name)),
                phase4_failure="synthetic deep Phase-4 failure",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("synthetic deep Phase-4 failure", result.stderr)
            self.assertEqual(len(result.phase4_calls), 1)
            self.assertFalse(result.phase9_calls)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p4-matrix-") as name:
            data = SyntheticEvidence(Path(name))
            missing = cross.MEDIUM_CACHE.format(2)
            data.remove_rows("phase4", lambda row: row["row_id"] == missing)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(missing, result.stderr)
            self.assertFalse(result.phase4_calls)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p4-deferred-") as name:
            result = self.run_case(
                SyntheticEvidence(Path(name)),
                phase4_status="deferred_baseline",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("did not return final status=pass", result.stderr)

    def test_phase6_complete_matrix_speed_and_scheduler_gates(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p6-matrix-") as name:
            data = SyntheticEvidence(Path(name))
            missing = cross.MEDIUM_EXACT.format(2)
            data.remove_rows("phase6", lambda row: row["row_id"] == missing)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(missing, result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p6-speed-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase6",
                lambda row: row["row_id"] == "p0-medium-primary32k4-grammar-exact-w8",
                {"exact_verification_ms": "51"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "phase6_topk4_exact_verification_w8_over_w1",
                result.stderr,
            )
        with tempfile.TemporaryDirectory(prefix="wric-cross-p6-axis-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase6",
                lambda row: row["row_id"] == "p0-medium-primary32k4-grammar-exact-w8",
                {"exact_candidate_parallel_batches": "0"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("candidate-parallel path was not activated", result.stderr)

    def test_phase6_admission_rss_and_repeat_gates_are_strict(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p6-admission-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase6",
                lambda row: row["row_id"] == "p0-medium-stress128k16-grammar-exact-w8"
                and row["trial_index"] == "1",
                {
                    "exact_candidate_peak_projected_resident_bytes": str(
                        12 * 1024**3 + 1
                    )
                },
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("projected resident bytes exceed", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p6-rss-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase6",
                lambda row: row["row_id"] == "p0-medium-stress128k16-grammar-exact-w8",
                {"peak_sampled_rss_kb": "210001"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("phase6_topk16_grammar_exact_ratio", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p6-repeat-") as name:
            data = SyntheticEvidence(Path(name))
            changed = digest("changed repeated W8 canonical result")
            data.mutate(
                "phase6",
                lambda row: row["row_id"] == "p0-medium-primary32k4-grammar-exact-w8"
                and row["trial_index"] == "2",
                {
                    "trial_semantic_sha256": changed,
                    "canonical_digest": changed,
                },
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("trial_semantic_sha256", result.stderr)

    def test_selected_chart_work_must_match_sealed_literal_contracts(self) -> None:
        cases = (
            (
                "phase6-topk1-candidates",
                "phase6",
                cross.MEDIUM_EXACT.format(1),
                {"candidates_scored": "0"},
                "iterations*chart_max_candidates=1",
            ),
            (
                "phase6-topk4-exact",
                "phase6",
                "p0-medium-primary32k4-grammar-exact-w1",
                {
                    "exact_verifications": "3",
                    "exact_candidate_timing_count": "3",
                },
                "iterations*chart_top_k_exact=4",
            ),
            (
                "phase6-topk16-exact",
                "phase6",
                "p0-medium-stress128k16-grammar-exact-w1",
                {
                    "exact_verifications": "47",
                    "exact_candidate_timing_count": "47",
                },
                "iterations*chart_top_k_exact=48",
            ),
            (
                "final-scaling",
                "final-scaling",
                "p0-medium-primary32k4-sampled-tree-fixed-topology-w1",
                {"candidates_scored": "31"},
                "iterations*chart_max_candidates=32",
            ),
            (
                "final-primary",
                "final-primary",
                "p0-medium-primary32k4-hybrid-exact-w1",
                {"candidates_scored": "31"},
                "iterations*chart_max_candidates=32",
            ),
            (
                "final-default",
                "final-default-auto",
                "p0-medium-primary32k4-smt-grammar-exact-wdefault",
                {"candidates_scored": "31"},
                "iterations*chart_max_candidates=32",
            ),
            (
                "final-stress",
                "final-stress",
                "p0-medium-stress128k16-sampled-tree-fixed-topology-w1",
                {"candidates_scored": "383"},
                "iterations*chart_max_candidates=384",
            ),
        )
        for case, label, row_id, changes, message in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                prefix=f"wric-cross-work-{case}-"
            ) as name:
                data = SyntheticEvidence(Path(name))

                def selected_trial(row: Mapping[str, str]) -> bool:
                    return (
                        row["row_id"] == row_id
                        and row["trial_index"] == "1"
                    )

                data.mutate(
                    label,
                    selected_trial,
                    changes,
                )
                result = self.run_case(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_projected_resident_covers_chart_and_admitted_bytes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-projection-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase6",
                lambda row: (
                    row["row_id"]
                    == "p0-medium-stress128k16-grammar-exact-w8"
                    and row["trial_index"] == "1"
                ),
                {cross.ADMISSION_FIELD: "1999999"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "do not cover chart resident plus admitted bytes",
                result.stderr,
            )

    def test_work_arithmetic_is_uint64_bounded_and_overflow_safe(self) -> None:
        with self.assertRaisesRegex(cross.AcceptanceError, "nonnegative integer"):
            cross.checked_uint64("-1", "synthetic selected work")
        with self.assertRaisesRegex(cross.AcceptanceError, "addition overflows"):
            cross.checked_uint64_sum(
                cross.UINT64_MAX, 1, "synthetic projected bytes"
            )
        with self.assertRaisesRegex(
            cross.AcceptanceError, "multiplication overflows"
        ):
            cross.checked_uint64_product(
                cross.UINT64_MAX, 2, "synthetic selected work"
            )
        with tempfile.TemporaryDirectory(prefix="wric-cross-uint64-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "final-primary",
                lambda row: (
                    row["row_id"]
                    == "p0-medium-primary32k4-grammar-exact-w1"
                    and row["trial_index"] == "1"
                ),
                {"candidates_scored": str(cross.UINT64_MAX + 1)},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exceeds uint64", result.stderr)

    def test_final_physical_scaling_matrix_and_speed_are_mandatory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-final-scale-matrix-") as name:
            data = SyntheticEvidence(Path(name))
            missing = "p0-medium-primary32k4-grammar-exact-w2"
            data.remove_rows("final-scaling", lambda row: row["row_id"] == missing)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(missing, result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-final-scale-speed-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "final-scaling",
                lambda row: row["row_id"] == "p0-medium-primary32k4-grammar-exact-w8",
                {"wall_clock_s": "0.41"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("final_scaling_grammar_w8_over_w1", result.stderr)

    def test_default_unpinned_median_must_reach_native_parity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-default-native-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "final-default-auto",
                lambda row: row["requested_workers"] == "default",
                {"wall_clock_s": "1.05"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("final_default_native_parity", result.stderr)

    def test_later_timeout_cannot_reuse_frozen_timeout(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-timeout-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase1",
                lambda row: row["row_id"] == cross.MEDIUM_LAZY.format(1) and row["trial_index"] == "1",
                {"status": "timeout", "validation_status": "not_run", "runner_outcome": "timed_out", "timed_out": "1"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("expected 'ok'", result.stderr)

    def test_exact_run_label_set_is_mandatory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-label-") as name:
            result = self.run_case(SyntheticEvidence(Path(name)), omit="final-stress")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("run-label set mismatch", result.stderr)

    def test_duplicate_raw_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-header-") as name:
            data = SyntheticEvidence(Path(name))
            path = data.raw_paths["phase3"]
            with path.open(encoding="utf-8", newline="") as handle:
                rows = list(csv.reader(handle, delimiter="\t"))
            rows[0].append("row_id")
            for row in rows[1:]:
                row.append(row[0])
            with path.open("w", encoding="utf-8", newline="") as handle:
                csv.writer(handle, delimiter="\t", lineterminator="\n").writerows(rows)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("duplicate header fields", result.stderr)

    def test_raw_anchors_are_mandatory_and_exact(self) -> None:
        for mode in ("missing", "wrong"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="wric-cross-raw-anchor-") as name:
                data = SyntheticEvidence(Path(name))
                options = ({"omit_raw_anchor": "phase3"} if mode == "missing" else {"raw_anchor_override": {"phase3": "0" * 64}})
                result = self.run_case(data, **options)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("raw", result.stderr.lower())

    def test_phase0_capture_arguments_are_the_complete_ledger_set(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-omit-capture-") as name:
            data = SyntheticEvidence(Path(name))
            data.phase0_inputs.remove(
                data.phase0_capture_paths[-1]
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exact ordered complete capture set", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-extra-capture-") as name:
            data = SyntheticEvidence(Path(name))
            extra = data.root / "extra-anchored-phase0.tsv"
            extra.write_bytes(data.phase0.read_bytes())
            data.phase0_inputs.append(extra)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exact ordered complete capture set", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-omit-real-") as name:
            data = SyntheticEvidence(Path(name))
            assert data.phase0_strict_real is not None
            data.phase0_inputs.remove(data.phase0_strict_real)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exact ordered complete capture set", result.stderr)

    def test_phase0_rejects_normalized_row_ids_in_aggregate_capture(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-source-id-") as name:
            data = SyntheticEvidence(Path(name))
            raw_path = data.phase0_capture_paths[0]
            classification_path = raw_path.with_name("trial_classification.tsv")
            with classification_path.open(
                encoding="utf-8", newline=""
            ) as handle:
                classification = next(csv.DictReader(handle, delimiter="\t"))
            with raw_path.open(encoding="utf-8", newline="") as handle:
                reader = csv.DictReader(handle, delimiter="\t")
                assert reader.fieldnames is not None
                header = reader.fieldnames
                rows = list(reader)
            target = next(
                row
                for row in rows
                if (
                    row["method"], row["requested_workers"]
                ) == (
                    classification["method"],
                    classification["requested_workers"],
                )
            )
            target["row_id"] = classification["row_id"]
            with raw_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=header,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(rows)
            data.refresh_phase0_ledger()
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("pre-manifest harness identity", result.stderr)

    def test_phase0_native_capture_binds_normalized_semantic_identity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-native-id-") as name:
            data = SyntheticEvidence(Path(name))
            selected: tuple[Path, list[str], list[dict[str, str]]] | None = None
            for raw_path in data.phase0_capture_paths:
                with raw_path.open(encoding="utf-8", newline="") as handle:
                    reader = csv.DictReader(handle, delimiter="\t")
                    assert reader.fieldnames is not None
                    header = reader.fieldnames
                    rows = list(reader)
                if any(row["method"] == cross.METHOD_NATIVE for row in rows):
                    selected = (raw_path, header, rows)
                    break
            assert selected is not None
            raw_path, header, rows = selected
            native = next(
                row
                for row in rows
                if row["method"] == cross.METHOD_NATIVE
            )
            native_key = (native["method"], native["requested_workers"])
            forged_argv = digest(
                "forged sealed Phase-0 native argv identity"
            )
            for row in rows:
                if (row["method"], row["requested_workers"]) == native_key:
                    row["canonical_argv_sha256"] = forged_argv
            with raw_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=header,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(rows)
            data.refresh_phase0_ledger()
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "canonical_argv_sha256 differs from sealed manifest row",
                result.stderr,
            )

    def test_phase0_strict_real_is_one_trial_and_outputless(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-real-count-") as name:
            data = SyntheticEvidence(Path(name))
            assert data.phase0_strict_real is not None
            with data.phase0_strict_real.open(
                encoding="utf-8", newline=""
            ) as handle:
                reader = csv.DictReader(handle, delimiter="\t")
                assert reader.fieldnames is not None
                header = reader.fieldnames
                rows = list(reader)
            extra = dict(rows[0])
            extra["trial_index"] = "2"
            rows.append(extra)
            with data.phase0_strict_real.open(
                "w", encoding="utf-8", newline=""
            ) as handle:
                writer = csv.DictWriter(
                    handle,
                    fieldnames=header,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(rows)
            data.refresh_phase0_ledger()
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "must have exactly one trial_index=1 observation",
                result.stderr,
            )
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-real-output-") as name:
            data = SyntheticEvidence(Path(name))
            assert data.phase0_strict_real is not None
            unexpected = data.phase0_strict_real.parent / "outputs" / "forged.pb.gz"
            unexpected.write_bytes(b"forged Phase-0 refusal output\n")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusal output namespace is not empty", result.stderr)

    def test_phase0_requires_every_sealed_base_row_and_unique_trials(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-missing-row-") as name:
            data = SyntheticEvidence(Path(name))
            omitted = "p0-native-medium-smt-i1-m50"
            data.remove_phase0_rows(lambda row: row["row_id"] == omitted)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("entire sealed base row set", result.stderr)
            self.assertIn(omitted, result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-duplicate-") as name:
            data = SyntheticEvidence(Path(name))
            data.duplicate_phase0_row_trial_across_captures()
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("more than one capture", result.stderr)

    def test_phase0_finite_trial_count_comes_only_from_sealed_capture(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-finite-count-") as name:
            data = SyntheticEvidence(Path(name))
            row_id = cross.SMALL_DENSE.format(1)
            data.remove_phase0_rows(
                lambda row: row["row_id"] == row_id
                and row["trial_index"] == "5"
            )
            result = self.run_case(data)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_phase0_ledger_anchor_seal_header_and_members_are_exact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-ledger-sha-") as name:
            result = self.run_case(
                SyntheticEvidence(Path(name)),
                phase0_ledger_anchor="0" * 64,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("external anchor", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-ledger-seal-") as name:
            data = SyntheticEvidence(Path(name))
            seal = data.phase0_artifact_ledger.with_name(
                data.phase0_artifact_ledger.name + ".sha256"
            )
            seal.write_text("forged detached seal\n", encoding="ascii")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("detached seal", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-ledger-header-") as name:
            data = SyntheticEvidence(Path(name))

            def swap_header(rows: list[list[str]]) -> None:
                rows[0] = ["uri", "sha256"]

            data.rewrite_phase0_ledger(swap_header)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("header", result.stderr)
            self.assertIn("not exactly", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-ledger-member-") as name:
            data = SyntheticEvidence(Path(name))
            data.phase0_ledger_member.write_text("mutated\n", encoding="utf-8")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("member hash mismatch", result.stderr)

    def test_phase0_final_unchanged_check_rehashes_every_member(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-toctou-") as name:
            data = SyntheticEvidence(Path(name))
            ledger = cross.load_phase0_artifact_ledger(
                data.phase0_artifact_ledger,
                data.phase0_artifact_ledger_sha,
                data.root,
            )
            data.phase0_ledger_member.write_text(
                "mutated after the sealed closure was loaded\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                cross.AcceptanceError,
                "artifact-ledger member changed during evaluation",
            ):
                ledger.unchanged()

    def test_phase0_ledger_uri_escape_and_raw_hash_divergence_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-uri-") as name:
            data = SyntheticEvidence(Path(name))

            def escape_uri(rows: list[list[str]]) -> None:
                rows[1][1] = "repo://../escape/raw_trials.tsv"

            data.rewrite_phase0_ledger(escape_uri)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not normalized", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-raw-ledger-") as name:
            data = SyntheticEvidence(Path(name))
            data.phase0.write_bytes(data.phase0.read_bytes() + b"\n")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("member hash mismatch", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-p0-raw-anchor-") as name:
            data = SyntheticEvidence(Path(name))
            result = self.run_case(
                data,
                raw_anchor_override={"phase0": "0" * 64},
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exact ordered complete capture set", result.stderr)

    def test_supplement_anchors_are_mandatory_and_exact(self) -> None:
        for mode in ("missing", "wrong"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="wric-cross-supp-anchor-") as name:
                data = SyntheticEvidence(Path(name))
                options = ({"omit_supplement_anchor": "phase8"} if mode == "missing" else {"supplement_anchor_override": {"phase8": "0" * 64}})
                result = self.run_case(data, **options)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("supplement", result.stderr.lower())

    def test_fabricated_supplement_row_is_rejected_when_resealed_and_reanchored(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-fabricated-") as name:
            data = SyntheticEvidence(Path(name))
            data.fabricate_supplement_row("phase7")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("row set mismatch", result.stderr)

    def test_supplemental_run_rows_are_bound_to_sealed_row_contents(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-supp-row-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase8-generation",
                lambda row: row["row_id"].endswith("w1") and row["trial_index"] == "1",
                {"input_sha256": "f" * 64},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("sealed manifest row", result.stderr)

    def test_phase0_timeout_count_comes_from_sealed_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-timeout-count-") as name:
            data = SyntheticEvidence(Path(name))
            data.append_timeout_trial()
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("records 1 observations", result.stderr)

    def test_phase9_ledger_anchor_and_deep_failure_propagate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-ledger-") as name:
            result = self.run_case(SyntheticEvidence(Path(name)), ledger_anchor="0" * 64)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("external anchor", result.stderr)
            self.assertFalse(result.phase9_calls)
        with tempfile.TemporaryDirectory(prefix="wric-cross-deep-") as name:
            result = self.run_case(
                SyntheticEvidence(Path(name)),
                phase9_failure="synthetic deep Phase-9 failure",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("synthetic deep Phase-9 failure", result.stderr)
            self.assertEqual(len(result.phase9_calls), 1)

    def test_forged_final_real_stderr_and_output_are_rejected(self) -> None:
        for mode in ("exit", "stderr", "output"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="wric-cross-real-") as name:
                data = SyntheticEvidence(Path(name))
                if mode == "exit":
                    data.mutate(
                        "final-real",
                        lambda row: row["trial_index"] == "1",
                        {"exit_code": "0"},
                    )
                elif mode == "stderr":
                    stderr = next((data.root / "final-real" / "reports").glob("*.err"))
                    stderr.write_text("forged\n" + cross.REAL_REFUSAL_SUFFIX, encoding="utf-8")
                else:
                    report = next((data.root / "final-real" / "reports").glob("*.out"))
                    (data.root / "final-real" / "outputs" / f"{report.stem}.pb.gz").write_bytes(b"forged")
                result = self.run_case(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(
                    "refusal" in result.stderr or "derived output" in result.stderr,
                    result.stderr,
                )

    def test_group_complete_repeated_inputs_and_group_confinement(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-split-") as name:
            data = SyntheticEvidence(Path(name))
            data.split_raw("phase1")
            result = self.run_case(data)
            self.assertEqual(result.returncode, 0, result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-cross-group-") as name:
            data = SyntheticEvidence(Path(name))
            data.append_disallowed_group_row("phase3")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("outside the explicitly expected sealed run groups", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-duplicate-") as name:
            data = SyntheticEvidence(Path(name))
            data.duplicate_across_repeated_inputs("phase1")
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("duplicate row/trial", result.stderr)

    def test_historical_admission_schema_is_all_absent_or_complete(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wric-cross-historical-") as name:
            data = SyntheticEvidence(Path(name))
            data.remove_columns("phase1", set(cross.ADMISSION_FIELDS))
            result = self.run_case(data)
            self.assertEqual(result.returncode, 0, result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-historical-na-") as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase1",
                lambda row: True,
                {field: "NA" for field in cross.ADMISSION_FIELDS},
            )
            result = self.run_case(data)
            self.assertEqual(result.returncode, 0, result.stderr)
        with tempfile.TemporaryDirectory(
            prefix="wric-cross-historical-partial-header-"
        ) as name:
            data = SyntheticEvidence(Path(name))
            data.remove_column("phase1", cross.ADMISSION_FIELD)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("partial exact-candidate admission schema", result.stderr)
        with tempfile.TemporaryDirectory(
            prefix="wric-cross-historical-mixed-na-"
        ) as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase1",
                lambda row: (
                    row["row_id"] == cross.SMALL_EXACT.format(1)
                    and row["trial_index"] == "1"
                ),
                {cross.ADMISSION_FIELD: "NA"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("mixes NA and recorded values", result.stderr)
        with tempfile.TemporaryDirectory(
            prefix="wric-cross-historical-malformed-"
        ) as name:
            data = SyntheticEvidence(Path(name))
            data.mutate(
                "phase1",
                lambda row: (
                    row["row_id"] == cross.SMALL_EXACT.format(1)
                    and row["trial_index"] == "1"
                ),
                {"exact_candidate_admission_batches": "malformed"},
            )
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("admission batches is not a nonnegative integer", result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-final-schema-") as name:
            data = SyntheticEvidence(Path(name))
            data.remove_column("final-primary", cross.ADMISSION_FIELD)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(cross.ADMISSION_FIELD, result.stderr)
        with tempfile.TemporaryDirectory(prefix="wric-cross-phase6-schema-") as name:
            data = SyntheticEvidence(Path(name))
            field = "exact_candidate_admission_batches"
            data.remove_column("phase6", field)
            result = self.run_case(data)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(field, result.stderr)


if __name__ == "__main__":
    unittest.main()
