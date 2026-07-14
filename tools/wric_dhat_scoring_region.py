#!/usr/bin/env python3
"""Validate and compare one caller-owned WRIC DHAT scoring region.

The analyzer deliberately relies on the frozen-reference parser for the DHAT
v2, command, hash, and product-counter contracts.  Unlike that parser, it does
not classify allocation owners.  It counts every DHAT ``tbk`` whose stack has
exactly one supplied ``*_into`` scorer frame at one of the supplied source
lines.  This makes caller-owned workspace growth part of the current count
while excluding result promotion and other work outside that structural seam.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shlex
from types import SimpleNamespace
import sys
import tempfile
from typing import Any, NoReturn, Sequence

# Keep a source-tree invocation (including CTest) artifact-free.
sys.dont_write_bytecode = True
import wric_dhat_allocation_reference as reference


ContractError = reference.ContractError

FROZEN_REFERENCE_REPORT_SHA256 = (
    "d54bc1fc4879c1c97321dd582cbfc2773c2b0343a672d093bc7706a1074c0522"
)
FROZEN_REFERENCE_BINARY_SHA256 = (
    "7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c"
)
FROZEN_REFERENCE_FIXTURE_SHA256 = (
    "e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6"
)
FROZEN_REFERENCE_RUN_COUNT = 3
REQUIRED_REDUCTION_PERCENT = 80


def contract_error(message: str) -> NoReturn:
    raise ContractError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require_object(value: Any, description: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        contract_error(f"{description} must be a JSON object")
    return value


def require_exact(value: Any, expected: Any, description: str) -> None:
    if value != expected:
        contract_error(f"{description}: expected {expected!r}, got {value!r}")


def require_positive_integer(value: Any, description: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        contract_error(f"{description} must be a positive integer")
    return value


def load_frozen_reference(
    path: Path,
    expected_sha256: str,
    current_fixture_sha256: str,
    expected_binary_sha256: str = FROZEN_REFERENCE_BINARY_SHA256,
    expected_fixture_sha256: str = FROZEN_REFERENCE_FIXTURE_SHA256,
) -> dict[str, Any]:
    report_sha256 = reference.validate_file_hash(
        path, expected_sha256, "frozen allocation-reference report"
    )
    try:
        data = json.loads(path.read_bytes())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ContractError(
            f"frozen allocation-reference report is not valid JSON: {error}"
        ) from error
    report = require_object(data, "frozen allocation-reference report")
    require_exact(report.get("format_version"), 1, "frozen report format_version")
    require_exact(report.get("status"), "ok", "frozen report status")
    require_exact(
        report.get("allocation_metric"),
        "DHAT tbk (total allocated blocks/allocation calls)",
        "frozen report allocation metric",
    )

    reference_section = require_object(
        report.get("reference"), "frozen report reference"
    )
    require_exact(
        reference_section.get("expected_run_count"),
        FROZEN_REFERENCE_RUN_COUNT,
        "frozen report expected run count",
    )
    binary = require_object(reference_section.get("binary"), "frozen report binary")
    fixture = require_object(reference_section.get("fixture"), "frozen report fixture")
    require_exact(
        binary.get("sha256"),
        expected_binary_sha256,
        "frozen report binary SHA-256",
    )
    require_exact(
        fixture.get("sha256"),
        expected_fixture_sha256,
        "frozen report fixture SHA-256",
    )
    require_exact(
        fixture.get("sha256"),
        current_fixture_sha256,
        "frozen/current fixture identity",
    )

    consensus = require_object(report.get("consensus"), "frozen report consensus")
    require_exact(
        consensus.get("identical_across_runs"),
        True,
        "frozen report consensus identity",
    )
    require_exact(
        consensus.get("run_count"),
        FROZEN_REFERENCE_RUN_COUNT,
        "frozen report consensus run count",
    )
    included = require_positive_integer(
        consensus.get("included_allocations"),
        "frozen report included allocations",
    )
    candidates = require_positive_integer(
        consensus.get("candidates"), "frozen report candidates"
    )
    total = require_positive_integer(
        consensus.get("total_allocations"), "frozen report total allocations"
    )
    excluded_total = require_positive_integer(
        consensus.get("excluded_allocations_total"),
        "frozen report excluded allocations",
    )
    if included + excluded_total != total:
        contract_error(
            "frozen report consensus accounting is inconsistent: "
            "included plus excluded must equal total"
        )

    runs = report.get("runs")
    if not isinstance(runs, list) or len(runs) != FROZEN_REFERENCE_RUN_COUNT:
        contract_error(
            "frozen report runs must contain exactly "
            f"{FROZEN_REFERENCE_RUN_COUNT} entries"
        )
    product_counters = require_object(
        reference_section.get("product_counters"),
        "frozen report product counters",
    )
    names: set[str] = set()
    for index, raw_run in enumerate(runs):
        run = require_object(raw_run, f"frozen report runs[{index}]")
        name = run.get("name")
        if not isinstance(name, str) or not name or name in names:
            contract_error(f"frozen report runs[{index}] has an invalid or duplicate name")
        names.add(name)
        require_exact(
            run.get("included_allocations"),
            included,
            f"frozen report {name} included allocations",
        )
        require_exact(
            run.get("total_allocations"),
            total,
            f"frozen report {name} total allocations",
        )
        require_exact(
            run.get("excluded_allocations_total"),
            excluded_total,
            f"frozen report {name} excluded allocations",
        )
        require_exact(
            run.get("product_counters"),
            product_counters,
            f"frozen report {name} product counters",
        )

    return {
        "path": str(path),
        "sha256": report_sha256,
        "included_allocations": included,
        "candidates": candidates,
        "run_count": FROZEN_REFERENCE_RUN_COUNT,
        "binary_sha256": expected_binary_sha256,
        "fixture_sha256": expected_fixture_sha256,
    }


def scorer_stem(symbol: str) -> str:
    open_parenthesis = symbol.find("(")
    if open_parenthesis <= 0 or not symbol.endswith(")"):
        contract_error("scorer symbol must be a complete demangled function symbol")
    stem = symbol[:open_parenthesis]
    if not stem.rsplit("::", 1)[-1].endswith("_into"):
        contract_error("scorer symbol must identify a structurally output-neutral *_into seam")
    return stem


def classify_region(
    data: dict[str, Any], scorer_symbol: str, scorer_lines: frozenset[int]
) -> dict[str, Any]:
    stem = scorer_stem(scorer_symbol)
    frames: list[str] = data["ftbl"]
    allocations_by_line: Counter[int] = Counter()
    points_by_line: Counter[int] = Counter()

    for point_index, point in enumerate(data["pps"]):
        description = f"DHAT scorer allocation point pps[{point_index}]"
        scorer_frames: list[tuple[int, str, int]] = []
        for stack_position, frame_index in enumerate(point["fs"]):
            frame = frames[frame_index]
            identity = reference.project_frame(frame, description)
            if stem not in reference.normalize_frame(frame):
                continue
            if identity is None:
                contract_error(
                    f"{description}: scorer-like frame lacks an exact "
                    f"chart_spr_search.hpp identity: {frame!r}"
                )
            symbol, line = identity
            scorer_frames.append((stack_position, symbol, line))

        if not scorer_frames:
            continue
        if len(scorer_frames) != 1:
            contract_error(
                f"{description}: ambiguous scorer-stack resolution; "
                f"found {len(scorer_frames)} scorer frames"
            )

        _, actual_symbol, actual_line = scorer_frames[0]
        if actual_symbol != scorer_symbol:
            contract_error(
                f"{description}: scorer symbol mismatch; expected "
                f"{scorer_symbol!r}, got {actual_symbol!r}"
            )
        if actual_line not in scorer_lines:
            contract_error(
                f"{description}: scorer line chart_spr_search.hpp:{actual_line} "
                f"is outside supplied lines {sorted(scorer_lines)}"
            )
        allocations_by_line[actual_line] += point["tbk"]
        points_by_line[actual_line] += 1

    observed_lines = frozenset(points_by_line)
    if observed_lines != scorer_lines:
        contract_error(
            "supplied scorer lines were not observed exactly; "
            f"expected {sorted(scorer_lines)}, got {sorted(observed_lines)}"
        )

    return {
        "allocations": sum(allocations_by_line.values()),
        "allocation_points": sum(points_by_line.values()),
        "allocations_by_line": {
            str(line): allocations_by_line[line] for line in sorted(allocations_by_line)
        },
        "allocation_points_by_line": {
            str(line): points_by_line[line] for line in sorted(points_by_line)
        },
    }


def require_equal(actual: int, expected: int, description: str) -> None:
    if actual != expected:
        contract_error(f"{description}: expected {expected}, got {actual}")


def build_comparison(
    current_allocations: int,
    current_candidates: int,
    frozen_allocations: int,
    frozen_candidates: int,
    reduction_percent: int,
) -> dict[str, Any]:
    remaining_percent = 100 - reduction_percent
    left = current_allocations * frozen_candidates * 100
    right = frozen_allocations * current_candidates * remaining_percent
    comparison = {
        "integer_arithmetic": "Python arbitrary precision; multiply before comparison",
        "formula": (
            "current_allocations * frozen_candidates * 100 <= "
            "frozen_allocations * current_candidates * (100 - reduction_percent)"
        ),
        "current_allocations": current_allocations,
        "current_candidates": current_candidates,
        "frozen_included_allocations": frozen_allocations,
        "frozen_candidates": frozen_candidates,
        "required_reduction_percent": reduction_percent,
        "left_widened_integer": left,
        "right_widened_integer": right,
        "passes": left <= right,
    }
    if left > right:
        contract_error(
            "allocation reduction threshold failed: "
            f"left widened integer {left} exceeds right {right}"
        )
    return comparison


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dhat", required=True)
    parser.add_argument("--dhat-sha256", required=True)
    parser.add_argument("--pid", required=True, type=reference.parse_positive)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--binary-command-path")
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--fixture-sha256", required=True)
    parser.add_argument("--fixture-command-path")
    parser.add_argument("--stdout", required=True)
    parser.add_argument("--stdout-sha256", required=True)
    parser.add_argument("--output-command-path", required=True)
    parser.add_argument("--expected-active-patterns", required=True, type=reference.parse_positive)
    parser.add_argument("--expected-candidates", required=True, type=reference.parse_positive)
    parser.add_argument("--expected-local-rows", required=True, type=reference.parse_positive)
    parser.add_argument("--expected-batches", required=True, type=reference.parse_positive)
    parser.add_argument(
        "--expected-candidate-failures",
        required=True,
        type=reference.parse_nonnegative,
    )
    parser.add_argument("--scorer-symbol", required=True)
    parser.add_argument(
        "--scorer-line",
        required=True,
        action="append",
        type=reference.parse_positive,
    )
    parser.add_argument(
        "--expected-region-allocations",
        required=True,
        type=reference.parse_nonnegative,
    )
    parser.add_argument(
        "--expected-region-points", required=True, type=reference.parse_positive
    )
    parser.add_argument(
        "--expected-whole-process-allocations",
        required=True,
        type=reference.parse_positive,
    )
    parser.add_argument("--frozen-reference-report", required=True)
    args = parser.parse_args(argv)
    if len(set(args.scorer_line)) != len(args.scorer_line):
        parser.error("--scorer-line values must be unique")
    return args


def run(
    args: argparse.Namespace,
    frozen_report_sha256: str = FROZEN_REFERENCE_REPORT_SHA256,
    frozen_binary_sha256: str = FROZEN_REFERENCE_BINARY_SHA256,
    frozen_fixture_sha256: str = FROZEN_REFERENCE_FIXTURE_SHA256,
) -> dict[str, Any]:
    dhat_path = Path(args.dhat)
    binary_path = Path(args.binary)
    fixture_path = Path(args.fixture)
    stdout_path = Path(args.stdout)

    hashes = {
        "dhat": reference.validate_file_hash(dhat_path, args.dhat_sha256, "DHAT"),
        "binary": reference.validate_file_hash(
            binary_path, args.binary_sha256, "profiled binary"
        ),
        "fixture": reference.validate_file_hash(
            fixture_path, args.fixture_sha256, "profiled fixture"
        ),
        "stdout": reference.validate_file_hash(
            stdout_path, args.stdout_sha256, "companion stdout"
        ),
    }
    frozen_reference = load_frozen_reference(
        Path(args.frozen_reference_report),
        frozen_report_sha256,
        hashes["fixture"],
        frozen_binary_sha256,
        frozen_fixture_sha256,
    )

    data = reference.load_dhat(dhat_path)
    reference.validate_dhat_schema(data, args.pid, "current")
    binary_command_path = args.binary_command_path or args.binary
    fixture_command_path = args.fixture_command_path or args.fixture
    command_argv = reference.validate_command(
        data["cmd"],
        reference.expected_command(
            binary_command_path,
            fixture_command_path,
            args.output_command_path,
            args.expected_candidates,
        ),
        "current",
    )
    product_counters = reference.validate_product_stdout(
        stdout_path,
        args.expected_active_patterns,
        args.expected_candidates,
        args.expected_local_rows,
        args.expected_batches,
        args.expected_candidate_failures,
        "current",
    )

    scorer_lines = frozenset(args.scorer_line)
    region = classify_region(data, args.scorer_symbol, scorer_lines)
    whole_process_allocations = sum(point["tbk"] for point in data["pps"])
    require_equal(
        region["allocations"],
        args.expected_region_allocations,
        "scoring-region allocations",
    )
    require_equal(
        region["allocation_points"],
        args.expected_region_points,
        "scoring-region allocation points",
    )
    require_equal(
        whole_process_allocations,
        args.expected_whole_process_allocations,
        "whole-process allocations",
    )
    comparison = build_comparison(
        region["allocations"],
        args.expected_candidates,
        frozen_reference["included_allocations"],
        frozen_reference["candidates"],
        REQUIRED_REDUCTION_PERCENT,
    )

    return {
        "format_version": 1,
        "status": "ok",
        "allocation_metric": "DHAT tbk (total allocated blocks/allocation calls)",
        "inputs": {
            "dhat": {"path": str(dhat_path), "sha256": hashes["dhat"]},
            "binary": {
                "path": str(binary_path),
                "command_path": binary_command_path,
                "sha256": hashes["binary"],
            },
            "fixture": {
                "path": str(fixture_path),
                "command_path": fixture_command_path,
                "sha256": hashes["fixture"],
            },
            "stdout": {"path": str(stdout_path), "sha256": hashes["stdout"]},
            "frozen_reference_report": frozen_reference,
            "pid": args.pid,
            "output_command_path": args.output_command_path,
            "command_argv": command_argv,
            "product_counters": product_counters,
        },
        "region_contract": {
            "scorer_symbol": args.scorer_symbol,
            "scorer_lines": sorted(scorer_lines),
            "stack_rule": (
                "count all tbk from points with exactly one exact scorer frame"
            ),
        },
        "measurement": {
            **region,
            "candidates": args.expected_candidates,
            "whole_process_allocations": whole_process_allocations,
            "whole_process_allocation_points": len(data["pps"]),
        },
        "comparison": comparison,
    }


def expect_contract_error(action: Any, text: str) -> None:
    try:
        action()
    except ContractError as error:
        if text not in str(error):
            raise AssertionError(
                f"expected ContractError containing {text!r}, got {str(error)!r}"
            ) from error
    else:
        raise AssertionError(f"expected ContractError containing {text!r}")


def self_test() -> None:
    symbol = "example::score_into(int)"
    with tempfile.TemporaryDirectory(prefix="wric-dhat-region-") as temporary:
        root = Path(temporary)
        binary = root / "binary"
        fixture = root / "fixture.pb"
        stdout = root / "stdout"
        dhat = root / "profile.dhat"
        binary.write_bytes(b"binary")
        fixture.write_bytes(b"fixture")
        stdout_text = """\
  active_patterns: 2
  configured_max_candidates: 1
  candidates_generated: 1
  candidates_scored: 1
  local_rows_recomputed: 3
  candidate_batches_scored: 1
      candidates_generated: 1
      candidates_scored: 1
      candidate_score_failures: 0
    local_candidate_scores: 1
    local_rows_recomputed: 3
    candidate_batches_scored: 1
  chart_workers_requested: 1
  chart_workers_resolved: 1
  local_score_workers: 1
  effective_pattern_batch_size: 2
  effective_candidate_batch_size: 1
  requested_max_iterations: 1
  iterations: 1
    local_score_parallel_batches: 0
    local_score_worker_tasks: 0
"""
        stdout.write_text(stdout_text, encoding="utf-8")

        def point(tbk: int, frames: list[int]) -> dict[str, Any]:
            return {
                "tb": tbk,
                "tbk": tbk,
                "tl": tbk,
                "mb": 0,
                "mbk": 0,
                "gb": 0,
                "gbk": 0,
                "eb": 0,
                "ebk": 0,
                "rb": 0,
                "wb": 0,
                "fs": frames,
            }

        command = reference.expected_command(
            "/command/binary", "/command/fixture.pb", "/command/output.pb", 1
        )
        profile = {
            "dhatFileVersion": 2,
            "mode": "heap",
            "verb": "Allocated",
            "bklt": True,
            "bkacc": True,
            "tu": "instrs",
            "Mtu": "Minstr",
            "tuth": 500,
            "cmd": shlex.join(command),
            "pid": 7,
            "te": 2,
            "tg": 1,
            "pps": [point(3, [1, 2, 3]), point(7, [1, 3])],
            "ftbl": [
                "[root]",
                "0x1: operator new(unsigned long)",
                f"0x2: {symbol} (chart_spr_search.hpp:10)",
                "0x3: main",
            ],
        }

        def write_profile(value: dict[str, Any]) -> str:
            encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
            dhat.write_bytes(encoded)
            return sha256_bytes(encoded)

        frozen_reference_path = root / "frozen-reference.json"
        current_fixture_hash = sha256_bytes(b"fixture")
        frozen_binary_hash = sha256_bytes(b"frozen-binary")
        frozen_product_counters = {
            "active_patterns": 2,
            "candidates_generated": 1,
            "candidates_scored": 1,
            "local_rows_recomputed": 3,
            "candidate_batches_scored": 1,
            "candidate_score_failures": 0,
        }
        frozen_report = {
            "format_version": 1,
            "status": "ok",
            "allocation_metric": "DHAT tbk (total allocated blocks/allocation calls)",
            "reference": {
                "expected_run_count": FROZEN_REFERENCE_RUN_COUNT,
                "binary": {"sha256": frozen_binary_hash},
                "fixture": {"sha256": current_fixture_hash},
                "product_counters": frozen_product_counters,
            },
            "consensus": {
                "identical_across_runs": True,
                "run_count": FROZEN_REFERENCE_RUN_COUNT,
                "included_allocations": 20,
                "excluded_allocations_total": 5,
                "total_allocations": 25,
                "candidates": 1,
            },
            "runs": [
                {
                    "name": f"run{index}",
                    "included_allocations": 20,
                    "excluded_allocations_total": 5,
                    "total_allocations": 25,
                    "product_counters": frozen_product_counters,
                }
                for index in range(1, FROZEN_REFERENCE_RUN_COUNT + 1)
            ],
        }

        def write_frozen_report(value: dict[str, Any]) -> str:
            encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
            frozen_reference_path.write_bytes(encoded)
            return sha256_bytes(encoded)

        frozen_report_hash = write_frozen_report(frozen_report)

        args = SimpleNamespace(
            dhat=str(dhat),
            dhat_sha256=write_profile(profile),
            pid=7,
            binary=str(binary),
            binary_sha256=sha256_bytes(b"binary"),
            binary_command_path="/command/binary",
            fixture=str(fixture),
            fixture_sha256=current_fixture_hash,
            fixture_command_path="/command/fixture.pb",
            stdout=str(stdout),
            stdout_sha256=sha256_bytes(stdout_text.encode()),
            output_command_path="/command/output.pb",
            expected_active_patterns=2,
            expected_candidates=1,
            expected_local_rows=3,
            expected_batches=1,
            expected_candidate_failures=0,
            scorer_symbol=symbol,
            scorer_line=[10],
            expected_region_allocations=3,
            expected_region_points=1,
            expected_whole_process_allocations=10,
            frozen_reference_report=str(frozen_reference_path),
        )

        def run_test() -> dict[str, Any]:
            return run(
                args,
                frozen_report_hash,
                frozen_binary_hash,
                current_fixture_hash,
            )

        first = run_test()
        second = run_test()
        assert json.dumps(first, sort_keys=True) == json.dumps(second, sort_keys=True)
        assert first["measurement"]["allocations"] == 3
        assert first["comparison"]["passes"] is True

        original_hash = args.dhat_sha256
        args.dhat_sha256 = "0" * 64
        expect_contract_error(run_test, "SHA-256 mismatch")
        args.dhat_sha256 = original_hash

        original_binary_hash = args.binary_sha256
        args.binary_sha256 = "0" * 64
        expect_contract_error(run_test, "SHA-256 mismatch")
        args.binary_sha256 = original_binary_hash

        original_fixture_hash = args.fixture_sha256
        args.fixture_sha256 = "0" * 64
        expect_contract_error(run_test, "SHA-256 mismatch")
        args.fixture_sha256 = original_fixture_hash

        original_stdout_hash = args.stdout_sha256
        args.stdout_sha256 = "0" * 64
        expect_contract_error(run_test, "SHA-256 mismatch")
        args.stdout_sha256 = original_stdout_hash

        original_frozen_hash = frozen_report_hash
        frozen_report_hash = "0" * 64
        expect_contract_error(run_test, "SHA-256 mismatch")
        frozen_report_hash = original_frozen_hash

        invalid_schema = json.loads(json.dumps(profile))
        invalid_schema["mode"] = "copy"
        args.dhat_sha256 = write_profile(invalid_schema)
        expect_contract_error(run_test, "mode")

        wrong_command = json.loads(json.dumps(profile))
        wrong_command["cmd"] += " --unexpected"
        args.dhat_sha256 = write_profile(wrong_command)
        expect_contract_error(run_test, "command mismatch")

        changed_stdout = stdout_text.replace("  active_patterns: 2", "  active_patterns: 3")
        stdout.write_text(changed_stdout, encoding="utf-8")
        args.stdout_sha256 = sha256_bytes(changed_stdout.encode())
        args.dhat_sha256 = write_profile(profile)
        expect_contract_error(run_test, "active_patterns")
        stdout.write_text(stdout_text, encoding="utf-8")
        args.stdout_sha256 = original_stdout_hash

        ambiguous = json.loads(json.dumps(profile))
        ambiguous["pps"][0]["fs"].insert(2, 2)
        args.dhat_sha256 = write_profile(ambiguous)
        expect_contract_error(run_test, "ambiguous scorer-stack resolution")

        foreign_source = json.loads(json.dumps(profile))
        foreign_source["ftbl"].append(f"0x4: {symbol} (other.hpp:10)")
        foreign_source["pps"][0]["fs"].insert(2, 4)
        args.dhat_sha256 = write_profile(foreign_source)
        expect_contract_error(run_test, "lacks an exact chart_spr_search.hpp")

        truncated = json.loads(json.dumps(profile))
        truncated["ftbl"].append(f"0x4: {scorer_stem(symbol)}")
        truncated["pps"][0]["fs"].insert(2, 4)
        args.dhat_sha256 = write_profile(truncated)
        expect_contract_error(run_test, "lacks an exact chart_spr_search.hpp")

        wrong_symbol = json.loads(json.dumps(profile))
        wrong_symbol["ftbl"][2] = (
            "0x2: example::score_into(long) (chart_spr_search.hpp:10)"
        )
        args.dhat_sha256 = write_profile(wrong_symbol)
        expect_contract_error(run_test, "scorer symbol mismatch")

        wrong_line = json.loads(json.dumps(profile))
        wrong_line["ftbl"][2] = f"0x2: {symbol} (chart_spr_search.hpp:11)"
        args.dhat_sha256 = write_profile(wrong_line)
        expect_contract_error(run_test, "outside supplied lines")

        args.dhat_sha256 = write_profile(profile)
        args.expected_region_allocations = 4
        expect_contract_error(run_test, "scoring-region allocations")
        args.expected_region_allocations = 3
        args.expected_region_points = 2
        expect_contract_error(run_test, "scoring-region allocation points")
        args.expected_region_points = 1
        args.expected_whole_process_allocations = 11
        expect_contract_error(run_test, "whole-process allocations")
        args.expected_whole_process_allocations = 10

        failing_reference = json.loads(json.dumps(frozen_report))
        failing_reference["consensus"]["included_allocations"] = 10
        failing_reference["consensus"]["total_allocations"] = 15
        for run_record in failing_reference["runs"]:
            run_record["included_allocations"] = 10
            run_record["total_allocations"] = 15
        frozen_report_hash = write_frozen_report(failing_reference)
        expect_contract_error(run_test, "allocation reduction threshold failed")

        malformed_reference = json.loads(json.dumps(frozen_report))
        malformed_reference["consensus"]["run_count"] = 2
        frozen_report_hash = write_frozen_report(malformed_reference)
        expect_contract_error(run_test, "consensus run count")


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments == ["self-test"]:
        try:
            self_test()
        except (AssertionError, ContractError) as error:
            print(f"wric_dhat_scoring_region self-test FAIL: {error}", file=sys.stderr)
            return 1
        print("wric_dhat_scoring_region self-test PASS")
        return 0

    args = parse_arguments(arguments)
    try:
        result = run(args)
    except ContractError as error:
        json.dump(
            {"format_version": 1, "status": "error", "error": str(error)},
            sys.stderr,
            sort_keys=True,
        )
        sys.stderr.write("\n")
        return 1
    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
