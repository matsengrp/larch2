#!/usr/bin/env python3
"""Validate and summarize the frozen Phase-2 WRIC DHAT reference.

This parser intentionally recognizes one narrow profiling contract.  It fails
closed when the DHAT schema, command line, product counters, scorer frames, or
allocation-owner identities differ from that contract.  Allocation counts are
DHAT ``tbk`` (total blocks), not bytes or live blocks.

Each --run has seven values:

    NAME DHAT DHAT_SHA256 STDOUT STDOUT_SHA256 OUTPUT_PATH PID

OUTPUT_PATH is the exact ``-o`` argument recorded in the DHAT command.  The
output file does not need to remain present after the profile is captured.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn, Sequence


OUTPUT_SCORES_VECTOR = "output_scores_vector"
PREPARED_SCORE_CANDIDATE_COPY = "prepared_score_candidate_copy"
RETURNED_SCORE_CANDIDATE_COPY = "returned_score_candidate_copy"
INCLUDED = "included"

EXCLUDED_CATEGORIES = (
    OUTPUT_SCORES_VECTOR,
    PREPARED_SCORE_CANDIDATE_COPY,
    RETURNED_SCORE_CANDIDATE_COPY,
)

SCORER_SYMBOL = (
    "larch::chart_spr_search_detail::score_candidates_locally_all_cache("
    "larch::chart_spr_search_state const&, std::vector<"
    "larch::grammar_spr_candidate, std::allocator<"
    "larch::grammar_spr_candidate> > const&, "
    "larch::local_spr_score_options const&, unsigned long)"
)
SCORER_NAME = "score_candidates_locally_all_cache"
SCORER_LINES = (3391, 3402)


# These are the only allocation-owner identities admitted below the frozen
# score_candidates_locally_all_cache stack.  A line and its complete demangled
# symbol must both match.  This makes a source shift or a new allocation site a
# deliberate reference-contract update rather than an accidental inclusion.
OWNER_SYMBOLS = {
    779: (
        "larch::chart_spr_candidate_score::chart_spr_candidate_score("
        "larch::chart_spr_candidate_score const&)"
    ),
    2168: (
        "larch::chart_spr_search_detail::"
        "validate_overlay_delta_production_partition("
        "larch::spr_overlay_delta const&, "
        "larch::overlay_grammar_production const&, unsigned int)"
    ),
    2188: "append_temp_production_index",
    2194: "append_temp_production_index",
    2241: "validate_reachable_base_production",
    2253: (
        "larch::chart_spr_search_detail::mark_overlay_delta_affected("
        "larch::spr_overlay_delta const&, std::vector<bool, "
        "std::allocator<bool> >&, std::vector<bool, std::allocator<bool> >&, "
        "std::vector<larch::overlay_clade_ref, "
        "std::allocator<larch::overlay_clade_ref> >&, "
        "larch::overlay_clade_ref)"
    ),
    2258: "mark_overlay_delta_affected",
    2296: (
        "larch::chart_spr_search_detail::build_overlay_delta_temp_indices("
        "larch::spr_overlay_delta&)"
    ),
    2297: (
        "larch::chart_spr_search_detail::build_overlay_delta_temp_indices("
        "larch::spr_overlay_delta&)"
    ),
    2298: (
        "larch::chart_spr_search_detail::build_overlay_delta_temp_indices("
        "larch::spr_overlay_delta&)"
    ),
    2299: (
        "larch::chart_spr_search_detail::build_overlay_delta_temp_indices("
        "larch::spr_overlay_delta&)"
    ),
    2309: (
        "larch::chart_spr_search_detail::build_overlay_delta_temp_indices("
        "larch::spr_overlay_delta&)"
    ),
    2325: (
        "larch::chart_spr_search_detail::compute_overlay_delta_reachability("
        "larch::spr_overlay_delta&, larch::local_spr_score_options const&)"
    ),
    2326: (
        "larch::chart_spr_search_detail::compute_overlay_delta_reachability("
        "larch::spr_overlay_delta&, larch::local_spr_score_options const&)"
    ),
    2331: (
        "larch::chart_spr_search_detail::compute_overlay_delta_reachability("
        "larch::spr_overlay_delta&, larch::local_spr_score_options const&)"
    ),
    2361: (
        "larch::chart_spr_search_detail::compute_overlay_delta_reachability("
        "larch::spr_overlay_delta&, larch::local_spr_score_options const&)"
    ),
    2375: (
        "larch::chart_spr_search_detail::compute_overlay_delta_reachability("
        "larch::spr_overlay_delta&, larch::local_spr_score_options const&)"
    ),
    2404: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2405: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2454: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2455: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2456: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2458: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2463: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2467: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2470: (
        "larch::chart_spr_search_detail::"
        "compute_overlay_delta_affected_order(larch::spr_overlay_delta&)"
    ),
    2514: (
        "larch::build_spr_overlay_delta(larch::clade_grammar const&, "
        "larch::grammar_spr_candidate const&, "
        "larch::local_spr_score_options const&)"
    ),
    2515: (
        "larch::build_spr_overlay_delta(larch::clade_grammar const&, "
        "larch::grammar_spr_candidate const&, "
        "larch::local_spr_score_options const&)"
    ),
    2522: (
        "larch::build_spr_overlay_delta(larch::clade_grammar const&, "
        "larch::grammar_spr_candidate const&, "
        "larch::local_spr_score_options const&)"
    ),
    2525: (
        "larch::build_spr_overlay_delta(larch::clade_grammar const&, "
        "larch::grammar_spr_candidate const&, "
        "larch::local_spr_score_options const&)"
    ),
    2545: (
        "larch::build_spr_overlay_delta(larch::clade_grammar const&, "
        "larch::grammar_spr_candidate const&, "
        "larch::local_spr_score_options const&)"
    ),
    2643: "recompute_overlay_delta_row<larch::overlay_row_provider>",
    2645: "recompute_overlay_delta_row<larch::overlay_row_provider>",
    2711: "build_local_overlay_chart_rows_into",
    2945: (
        "larch::chart_spr_search_detail::prepare_local_candidate_score("
        "larch::chart_spr_search_state const&, "
        "larch::grammar_spr_candidate const&, "
        "larch::local_spr_score_options const&, "
        "larch::chart_spr_search_counters*)"
    ),
    3004: (
        "larch::chart_spr_search_detail::"
        "accumulate_prepared_local_candidate_patterns("
        "larch::chart_spr_search_state const&, "
        "larch::chart_spr_search_detail::prepared_local_candidate_score&, "
        "unsigned long, std::vector<larch::pattern_chart_cache_entry, "
        "std::allocator<larch::pattern_chart_cache_entry> > const&, "
        "larch::local_spr_score_options const&, "
        "larch::chart_spr_search_counters*, "
        "larch::chart_spr_local_score_scratch&)"
    ),
    3019: (
        "larch::chart_spr_search_detail::"
        "accumulate_prepared_local_candidate_patterns("
        "larch::chart_spr_search_state const&, "
        "larch::chart_spr_search_detail::prepared_local_candidate_score&, "
        "unsigned long, std::vector<larch::pattern_chart_cache_entry, "
        "std::allocator<larch::pattern_chart_cache_entry> > const&, "
        "larch::local_spr_score_options const&, "
        "larch::chart_spr_search_counters*, "
        "larch::chart_spr_local_score_scratch&)"
    ),
    3023: (
        "larch::chart_spr_search_detail::"
        "accumulate_prepared_local_candidate_patterns("
        "larch::chart_spr_search_state const&, "
        "larch::chart_spr_search_detail::prepared_local_candidate_score&, "
        "unsigned long, std::vector<larch::pattern_chart_cache_entry, "
        "std::allocator<larch::pattern_chart_cache_entry> > const&, "
        "larch::local_spr_score_options const&, "
        "larch::chart_spr_search_counters*, "
        "larch::chart_spr_local_score_scratch&)"
    ),
    3037: (
        "larch::chart_spr_search_detail::finish_prepared_local_candidate_score("
        "larch::chart_spr_search_state const&, "
        "larch::chart_spr_search_detail::prepared_local_candidate_score&)"
    ),
    3391: SCORER_SYMBOL,
}

OWNER_CATEGORIES = {
    779: RETURNED_SCORE_CANDIDATE_COPY,
    2945: PREPARED_SCORE_CANDIDATE_COPY,
    3391: OUTPUT_SCORES_VECTOR,
}

DHAT_TOP_LEVEL_KEYS = {
    "dhatFileVersion",
    "mode",
    "verb",
    "bklt",
    "bkacc",
    "tu",
    "Mtu",
    "tuth",
    "cmd",
    "pid",
    "te",
    "tg",
    "pps",
    "ftbl",
}
PPS_INTEGER_KEYS = {
    "tb",
    "tbk",
    "tl",
    "mb",
    "mbk",
    "gb",
    "gbk",
    "eb",
    "ebk",
    "rb",
    "wb",
}
PPS_REQUIRED_KEYS = PPS_INTEGER_KEYS | {"fs"}
PPS_ALLOWED_KEYSETS = (
    frozenset(PPS_REQUIRED_KEYS),
    frozenset(PPS_REQUIRED_KEYS | {"acc"}),
)

ADDRESS_PREFIX_RE = re.compile(r"^0x[0-9A-Fa-f]+: ")
PROJECT_FRAME_RE = re.compile(
    r"^(?P<symbol>.+) \(chart_spr_search\.hpp:(?P<line>[0-9]+)\)$"
)
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
RUN_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


FROZEN_COMMAND_BEFORE_OUTPUT = (
    "--force-no-vcf",
    "--validate",
    "--wric-polytomy-mode",
    "expand-bounded",
    "--wric-polytomy-max-shapes",
    "1",
    "--wric-lazy-chart",
    "off",
    "--chart-spr-search",
    "--chart-spr-max-iterations",
    "1",
    "--chart-spr-max-candidates",
)
FROZEN_COMMAND_AFTER_CANDIDATES = (
    "--chart-spr-top-k-exact",
    "0",
    "--chart-spr-candidate-selection",
    "lower-bound-top-k",
    "--chart-spr-candidate-source",
    "grammar",
    "--chart-spr-acceptance",
    "lower-bound",
    "--chart-spr-workers",
    "1",
    "--chart-spr-memory-budget",
    "12884901888",
    "--seed",
    "1",
    "-o",
)


class ContractError(RuntimeError):
    """A frozen-reference invariant was not satisfied."""


@dataclass(frozen=True)
class RunSpec:
    name: str
    dhat_path: Path
    dhat_sha256: str
    stdout_path: Path
    stdout_sha256: str
    output_command_path: str
    pid: int


def contract_error(message: str) -> NoReturn:
    raise ContractError(message)


def parse_nonnegative(value: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"expected a decimal integer: {value}") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError(f"expected a nonnegative integer: {value}")
    return parsed


def parse_positive(value: str) -> int:
    parsed = parse_nonnegative(value)
    if parsed == 0:
        raise argparse.ArgumentTypeError(f"expected a positive integer: {value}")
    return parsed


def normalize_sha256(value: str, description: str) -> str:
    if SHA256_RE.fullmatch(value) is None:
        contract_error(f"{description}: expected exactly 64 hexadecimal SHA-256 digits")
    return value.lower()


def sha256_file(path: Path, description: str) -> str:
    if not path.is_file():
        contract_error(f"{description}: not a regular readable file: {path}")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        contract_error(f"{description}: cannot read {path}: {error}")
    return digest.hexdigest()


def validate_file_hash(path: Path, expected: str, description: str) -> str:
    expected = normalize_sha256(expected, f"{description} expected hash")
    actual = sha256_file(path, description)
    if actual != expected:
        contract_error(
            f"{description}: SHA-256 mismatch for {path}: "
            f"expected {expected}, got {actual}"
        )
    return actual


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            contract_error(f"DHAT JSON contains duplicate object key {key!r}")
        result[key] = value
    return result


def reject_json_constant(value: str) -> NoReturn:
    contract_error(f"DHAT JSON contains non-standard numeric constant {value!r}")


def load_dhat(path: Path) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        contract_error(f"DHAT JSON cannot be read as UTF-8 from {path}: {error}")
    try:
        result = json.loads(
            text,
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_json_constant,
        )
    except ContractError:
        raise
    except (json.JSONDecodeError, TypeError, ValueError) as error:
        contract_error(f"invalid DHAT JSON in {path}: {error}")
    if type(result) is not dict:
        contract_error(f"DHAT JSON root in {path} must be an object")
    return result


def require_exact_int(value: Any, description: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        contract_error(f"{description} must be an integer >= {minimum}")
    return value


def validate_dhat_schema(data: dict[str, Any], expected_pid: int, run: str) -> None:
    keys = set(data)
    if keys != DHAT_TOP_LEVEL_KEYS:
        missing = sorted(DHAT_TOP_LEVEL_KEYS - keys)
        unknown = sorted(keys - DHAT_TOP_LEVEL_KEYS)
        contract_error(
            f"{run}: DHAT v2 top-level schema mismatch; "
            f"missing={missing}, unknown={unknown}"
        )

    exact_values = {
        "dhatFileVersion": 2,
        "mode": "heap",
        "verb": "Allocated",
        "bklt": True,
        "bkacc": True,
        "tu": "instrs",
        "Mtu": "Minstr",
        "tuth": 500,
    }
    for key, expected in exact_values.items():
        value = data[key]
        if type(value) is not type(expected) or value != expected:
            contract_error(
                f"{run}: DHAT field {key!r} must be {expected!r}, got {value!r}"
            )

    if type(data["cmd"]) is not str or not data["cmd"]:
        contract_error(f"{run}: DHAT field 'cmd' must be a nonempty string")
    pid = require_exact_int(data["pid"], f"{run}: DHAT pid", minimum=1)
    if pid != expected_pid:
        contract_error(f"{run}: DHAT pid mismatch: expected {expected_pid}, got {pid}")
    elapsed = require_exact_int(data["te"], f"{run}: DHAT te", minimum=1)
    useful = require_exact_int(data["tg"], f"{run}: DHAT tg", minimum=1)
    if useful > elapsed:
        contract_error(f"{run}: DHAT tg ({useful}) exceeds te ({elapsed})")

    ftbl = data["ftbl"]
    if type(ftbl) is not list or not ftbl:
        contract_error(f"{run}: DHAT ftbl must be a nonempty list")
    for index, frame in enumerate(ftbl):
        if type(frame) is not str or not frame:
            contract_error(f"{run}: DHAT ftbl[{index}] must be a nonempty string")
    if ftbl[0] != "[root]":
        contract_error(f"{run}: DHAT ftbl[0] must be '[root]'")

    pps = data["pps"]
    if type(pps) is not list or not pps:
        contract_error(f"{run}: DHAT pps must be a nonempty list")
    for pp_index, point in enumerate(pps):
        prefix = f"{run}: DHAT pps[{pp_index}]"
        if type(point) is not dict:
            contract_error(f"{prefix} must be an object")
        point_keys = frozenset(point)
        if point_keys not in PPS_ALLOWED_KEYSETS:
            missing = sorted(PPS_REQUIRED_KEYS - point_keys)
            unknown = sorted(point_keys - (PPS_REQUIRED_KEYS | {"acc"}))
            contract_error(
                f"{prefix} schema mismatch; missing={missing}, unknown={unknown}"
            )
        for key in PPS_INTEGER_KEYS:
            minimum = 1 if key in {"tb", "tbk", "tl"} else 0
            require_exact_int(point[key], f"{prefix}.{key}", minimum=minimum)
        frames = point["fs"]
        if type(frames) is not list or not frames:
            contract_error(f"{prefix}.fs must be a nonempty list")
        for frame_position, frame_index in enumerate(frames):
            frame_index = require_exact_int(
                frame_index,
                f"{prefix}.fs[{frame_position}]",
            )
            if frame_index >= len(ftbl):
                contract_error(
                    f"{prefix}.fs[{frame_position}]={frame_index} is outside ftbl"
                )
        if "acc" in point:
            accesses = point["acc"]
            if type(accesses) is not list or not accesses:
                contract_error(f"{prefix}.acc must be a nonempty integer list")
            if any(type(value) is not int for value in accesses):
                contract_error(f"{prefix}.acc must contain only integers")


def expected_command(
    binary_command_path: str,
    fixture_command_path: str,
    output_command_path: str,
    expected_candidates: int,
) -> list[str]:
    return [
        binary_command_path,
        "--dag-pb",
        fixture_command_path,
        *FROZEN_COMMAND_BEFORE_OUTPUT,
        str(expected_candidates),
        *FROZEN_COMMAND_AFTER_CANDIDATES,
        output_command_path,
    ]


def validate_command(command: str, expected: list[str], run: str) -> list[str]:
    try:
        actual = shlex.split(command, posix=True)
    except ValueError as error:
        contract_error(f"{run}: DHAT command cannot be tokenized: {error}")
    if actual != expected:
        contract_error(
            f"{run}: frozen command mismatch; "
            f"expected argv={expected!r}, got argv={actual!r}"
        )
    return actual


def unique_stdout_integer(text: str, indent: int, key: str, run: str) -> int:
    expression = re.compile(rf"(?m)^{' ' * indent}{re.escape(key)}: ([0-9]+)$")
    matches = expression.findall(text)
    if len(matches) != 1:
        contract_error(
            f"{run}: stdout must contain exactly one {key!r} integer at "
            f"indentation {indent}; found {len(matches)}"
        )
    return int(matches[0], 10)


def require_counter(actual: int, expected: int, description: str, run: str) -> None:
    if actual != expected:
        contract_error(f"{run}: {description}: expected {expected}, got {actual}")


def validate_product_stdout(
    path: Path,
    expected_active_patterns: int,
    expected_candidates: int,
    expected_local_rows: int,
    expected_batches: int,
    expected_failures: int,
    run: str,
) -> dict[str, int]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        contract_error(f"{run}: companion stdout is not readable UTF-8: {error}")

    expected_fields = {
        (2, "active_patterns"): expected_active_patterns,
        (2, "configured_max_candidates"): expected_candidates,
        (2, "candidates_generated"): expected_candidates,
        (2, "candidates_scored"): expected_candidates,
        (2, "local_rows_recomputed"): expected_local_rows,
        (2, "candidate_batches_scored"): expected_batches,
        (6, "candidates_generated"): expected_candidates,
        (6, "candidates_scored"): expected_candidates,
        (6, "candidate_score_failures"): expected_failures,
        (4, "local_candidate_scores"): expected_candidates,
        (4, "local_rows_recomputed"): expected_local_rows,
        (4, "candidate_batches_scored"): expected_batches,
    }
    for (indent, key), expected in expected_fields.items():
        require_counter(
            unique_stdout_integer(text, indent, key, run),
            expected,
            f"stdout {'iteration ' if indent == 6 else ''}{key}",
            run,
        )

    frozen_metadata = {
        (2, "chart_workers_requested"): 1,
        (2, "chart_workers_resolved"): 1,
        (2, "local_score_workers"): 1,
        (2, "effective_pattern_batch_size"): expected_active_patterns,
        (2, "effective_candidate_batch_size"): 1,
        (2, "requested_max_iterations"): 1,
        (2, "iterations"): 1,
        (4, "local_score_parallel_batches"): 0,
        (4, "local_score_worker_tasks"): 0,
    }
    for (indent, key), expected in frozen_metadata.items():
        require_counter(
            unique_stdout_integer(text, indent, key, run),
            expected,
            f"stdout frozen metadata {key}",
            run,
        )

    if expected_failures != 0:
        contract_error(
            f"{run}: frozen allocation reference requires zero candidate failures"
        )

    return {
        "active_patterns": expected_active_patterns,
        "candidates_generated": expected_candidates,
        "candidates_scored": expected_candidates,
        "local_rows_recomputed": expected_local_rows,
        "candidate_batches_scored": expected_batches,
        "candidate_score_failures": expected_failures,
    }


def normalize_frame(frame: str) -> str:
    return ADDRESS_PREFIX_RE.sub("", frame, count=1)


def project_frame(frame: str, description: str) -> tuple[str, int] | None:
    normalized = normalize_frame(frame)
    if "chart_spr_search.hpp" not in normalized:
        return None
    match = PROJECT_FRAME_RE.fullmatch(normalized)
    if match is None:
        contract_error(f"{description}: malformed project frame {frame!r}")
    return match.group("symbol"), int(match.group("line"), 10)


def classify_scorer_allocations(data: dict[str, Any], run: str) -> dict[str, Any]:
    frames: list[str] = data["ftbl"]
    allocation_calls_by_line: Counter[int] = Counter()
    allocation_points_by_line: Counter[int] = Counter()
    category_calls: Counter[str] = Counter()
    category_points: Counter[str] = Counter()
    scorer_points = 0

    for pp_index, point in enumerate(data["pps"]):
        stack = [frames[index] for index in point["fs"]]
        scorer_positions = [
            position
            for position, frame in enumerate(stack)
            if SCORER_NAME in normalize_frame(frame)
        ]
        if not scorer_positions:
            continue
        description = f"{run}: scorer allocation point pps[{pp_index}]"
        if len(scorer_positions) != 1:
            contract_error(
                f"{description}: ambiguous scorer-stack resolution; "
                f"found {len(scorer_positions)} scorer frames"
            )

        scorer_position = scorer_positions[0]
        scorer_identity = project_frame(stack[scorer_position], description)
        if scorer_identity is None:
            contract_error(f"{description}: scorer frame lacks a project line identity")
        scorer_symbol, scorer_line = scorer_identity
        if scorer_symbol != SCORER_SYMBOL or scorer_line not in SCORER_LINES:
            contract_error(
                f"{description}: unrecognized scorer frame identity "
                f"{scorer_symbol!r} at chart_spr_search.hpp:{scorer_line}"
            )

        project_positions: list[tuple[int, str, int]] = []
        for position, frame in enumerate(stack):
            identity = project_frame(frame, description)
            if identity is not None:
                symbol, line = identity
                project_positions.append((position, symbol, line))
        if not project_positions:
            contract_error(f"{description}: no allocation-owner project frame")

        owner_position, owner_symbol, owner_line = project_positions[0]
        if owner_position > scorer_position:
            contract_error(
                f"{description}: allocation owner appears outside the scorer frame"
            )
        expected_symbol = OWNER_SYMBOLS.get(owner_line)
        if expected_symbol is None:
            contract_error(
                f"{description}: unclassified allocation owner at "
                f"chart_spr_search.hpp:{owner_line}: {owner_symbol!r}"
            )
        if owner_symbol != expected_symbol:
            contract_error(
                f"{description}: owner symbol mismatch at "
                f"chart_spr_search.hpp:{owner_line}; expected {expected_symbol!r}, "
                f"got {owner_symbol!r}"
            )

        category = OWNER_CATEGORIES.get(owner_line, INCLUDED)
        allocation_calls = point["tbk"]
        allocation_calls_by_line[owner_line] += allocation_calls
        allocation_points_by_line[owner_line] += 1
        category_calls[category] += allocation_calls
        category_points[category] += 1
        scorer_points += 1

    if scorer_points == 0:
        contract_error(f"{run}: no scorer-stack DHAT allocation points found")
    missing_owner_lines = sorted(set(OWNER_SYMBOLS) - set(allocation_calls_by_line))
    if missing_owner_lines:
        contract_error(
            f"{run}: frozen owner identities have no allocation point: "
            f"{missing_owner_lines}"
        )

    total_calls = sum(allocation_calls_by_line.values())
    excluded = {category: category_calls[category] for category in EXCLUDED_CATEGORIES}
    excluded_total = sum(excluded.values())
    included_calls = category_calls[INCLUDED]
    if included_calls + excluded_total != total_calls:
        contract_error(f"{run}: internal allocation classification accounting error")

    return {
        "scorer_allocation_points": scorer_points,
        "total_allocations": total_calls,
        "excluded_allocations": excluded,
        "excluded_allocations_total": excluded_total,
        "included_allocations": included_calls,
        "owner_allocations_by_line": {
            str(line): allocation_calls_by_line[line]
            for line in sorted(allocation_calls_by_line)
        },
        "owner_allocation_points_by_line": {
            str(line): allocation_points_by_line[line]
            for line in sorted(allocation_points_by_line)
        },
    }


def parse_run_specs(raw_specs: list[list[str]], expected_count: int) -> list[RunSpec]:
    if len(raw_specs) != expected_count:
        contract_error(
            f"expected exactly {expected_count} --run specifications, got {len(raw_specs)}"
        )
    specs: list[RunSpec] = []
    names: set[str] = set()
    dhat_paths: set[Path] = set()
    stdout_paths: set[Path] = set()
    for raw in raw_specs:
        name, dhat, dhat_hash, stdout, stdout_hash, output_path, pid_text = raw
        if RUN_NAME_RE.fullmatch(name) is None:
            contract_error(f"invalid run name {name!r}")
        if name in names:
            contract_error(f"duplicate run name {name!r}")
        try:
            pid = int(pid_text, 10)
        except ValueError as error:
            raise ContractError(f"{name}: invalid decimal pid {pid_text!r}") from error
        if pid <= 0:
            contract_error(f"{name}: pid must be positive")
        dhat_path = Path(dhat)
        stdout_path = Path(stdout)
        if dhat_path in dhat_paths:
            contract_error(f"{name}: duplicate DHAT path {dhat_path}")
        if stdout_path in stdout_paths:
            contract_error(f"{name}: duplicate stdout path {stdout_path}")
        specs.append(
            RunSpec(
                name=name,
                dhat_path=dhat_path,
                dhat_sha256=normalize_sha256(dhat_hash, f"{name} DHAT hash"),
                stdout_path=stdout_path,
                stdout_sha256=normalize_sha256(stdout_hash, f"{name} stdout hash"),
                output_command_path=output_path,
                pid=pid,
            )
        )
        names.add(name)
        dhat_paths.add(dhat_path)
        stdout_paths.add(stdout_path)
    return specs


def validate_expected_totals(summary: dict[str, Any], args: argparse.Namespace, run: str) -> None:
    expected = {
        "total_allocations": args.expected_total_allocations,
        "included_allocations": args.expected_included_allocations,
    }
    for key, value in expected.items():
        if summary[key] != value:
            contract_error(f"{run}: {key}: expected {value}, got {summary[key]}")

    expected_excluded = {
        OUTPUT_SCORES_VECTOR: args.expected_output_scores_vector_allocations,
        PREPARED_SCORE_CANDIDATE_COPY: (
            args.expected_prepared_score_candidate_copy_allocations
        ),
        RETURNED_SCORE_CANDIDATE_COPY: (
            args.expected_returned_score_candidate_copy_allocations
        ),
    }
    if summary["excluded_allocations"] != expected_excluded:
        contract_error(
            f"{run}: excluded allocation totals: expected {expected_excluded}, "
            f"got {summary['excluded_allocations']}"
        )
    if (
        args.expected_included_allocations + sum(expected_excluded.values())
        != args.expected_total_allocations
    ):
        contract_error(
            "expected totals are arithmetically inconsistent: included + excluded "
            "must equal total"
        )


def consensus_key(run: dict[str, Any]) -> tuple[Any, ...]:
    return (
        run["scorer_allocation_points"],
        run["total_allocations"],
        tuple(sorted(run["excluded_allocations"].items())),
        run["excluded_allocations_total"],
        run["included_allocations"],
        tuple(sorted(run["owner_allocations_by_line"].items())),
        tuple(sorted(run["owner_allocation_points_by_line"].items())),
        tuple(sorted(run["product_counters"].items())),
    )


def build_comparison(
    reference_included: int,
    reference_candidates: int,
    reduction_percent: int,
    current_included: int | None,
    current_candidates: int | None,
) -> dict[str, Any]:
    remaining_percent = 100 - reduction_percent
    result: dict[str, Any] = {
        "metric": "included DHAT tbk allocation calls per candidate",
        "normalization_formula": "included_allocations * 1000 / candidates",
        "required_reduction_percent": reduction_percent,
        "integer_arithmetic": "arbitrary precision; multiply before comparison; no division",
        "widened_integer_formula": (
            "current_included_allocations * reference_candidates * 100 <= "
            "reference_included_allocations * current_candidates * "
            f"{remaining_percent}"
        ),
        "reference_substitution": (
            f"current_included_allocations * {reference_candidates} * 100 <= "
            f"{reference_included} * current_candidates * {remaining_percent}"
        ),
    }
    if current_included is not None and current_candidates is not None:
        left = current_included * reference_candidates * 100
        right = reference_included * current_candidates * remaining_percent
        result["current_comparison"] = {
            "current_included_allocations": current_included,
            "current_candidates": current_candidates,
            "left_widened_integer": left,
            "right_widened_integer": right,
            "passes": left <= right,
        }
    return result


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--binary", required=True, help="frozen executable to hash")
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument(
        "--binary-command-path",
        help="exact argv[0] in DHAT cmd (defaults to --binary spelling)",
    )
    parser.add_argument("--fixture", required=True, help="frozen input fixture to hash")
    parser.add_argument("--fixture-sha256", required=True)
    parser.add_argument(
        "--fixture-command-path",
        help="exact --dag-pb value in DHAT cmd (defaults to --fixture spelling)",
    )
    parser.add_argument("--expected-run-count", required=True, type=parse_positive)
    parser.add_argument(
        "--run",
        action="append",
        nargs=7,
        required=True,
        metavar="FIELD",
        help=(
            "repeat NAME DHAT DHAT_SHA256 STDOUT STDOUT_SHA256 OUTPUT_PATH PID"
        ),
    )
    parser.add_argument("--expected-active-patterns", required=True, type=parse_positive)
    parser.add_argument("--expected-candidates", required=True, type=parse_positive)
    parser.add_argument("--expected-local-rows", required=True, type=parse_positive)
    parser.add_argument("--expected-batches", required=True, type=parse_positive)
    parser.add_argument("--expected-candidate-failures", required=True, type=parse_nonnegative)
    parser.add_argument("--expected-total-allocations", required=True, type=parse_positive)
    parser.add_argument("--expected-included-allocations", required=True, type=parse_positive)
    parser.add_argument(
        "--expected-output-scores-vector-allocations",
        required=True,
        type=parse_nonnegative,
    )
    parser.add_argument(
        "--expected-prepared-score-candidate-copy-allocations",
        required=True,
        type=parse_nonnegative,
    )
    parser.add_argument(
        "--expected-returned-score-candidate-copy-allocations",
        required=True,
        type=parse_nonnegative,
    )
    parser.add_argument(
        "--required-reduction-percent",
        type=parse_nonnegative,
        default=80,
    )
    parser.add_argument("--current-included-allocations", type=parse_nonnegative)
    parser.add_argument("--current-candidates", type=parse_positive)
    args = parser.parse_args(argv)
    if args.required_reduction_percent > 100:
        parser.error("--required-reduction-percent must be between 0 and 100")
    if (args.current_included_allocations is None) != (args.current_candidates is None):
        parser.error(
            "--current-included-allocations and --current-candidates must be supplied together"
        )
    return args


def run(args: argparse.Namespace) -> dict[str, Any]:
    binary_path = Path(args.binary)
    fixture_path = Path(args.fixture)
    binary_sha256 = validate_file_hash(
        binary_path, args.binary_sha256, "frozen binary"
    )
    fixture_sha256 = validate_file_hash(
        fixture_path, args.fixture_sha256, "frozen fixture"
    )
    binary_command_path = args.binary_command_path or args.binary
    fixture_command_path = args.fixture_command_path or args.fixture
    run_specs = parse_run_specs(args.run, args.expected_run_count)

    run_results: list[dict[str, Any]] = []
    for spec in run_specs:
        dhat_sha256 = validate_file_hash(
            spec.dhat_path, spec.dhat_sha256, f"{spec.name} DHAT"
        )
        stdout_sha256 = validate_file_hash(
            spec.stdout_path, spec.stdout_sha256, f"{spec.name} stdout"
        )
        data = load_dhat(spec.dhat_path)
        validate_dhat_schema(data, spec.pid, spec.name)
        command_argv = validate_command(
            data["cmd"],
            expected_command(
                binary_command_path,
                fixture_command_path,
                spec.output_command_path,
                args.expected_candidates,
            ),
            spec.name,
        )
        product_counters = validate_product_stdout(
            spec.stdout_path,
            args.expected_active_patterns,
            args.expected_candidates,
            args.expected_local_rows,
            args.expected_batches,
            args.expected_candidate_failures,
            spec.name,
        )
        summary = classify_scorer_allocations(data, spec.name)
        validate_expected_totals(summary, args, spec.name)
        run_results.append(
            {
                "name": spec.name,
                "dhat_path": str(spec.dhat_path),
                "dhat_sha256": dhat_sha256,
                "stdout_path": str(spec.stdout_path),
                "stdout_sha256": stdout_sha256,
                "pid": spec.pid,
                "output_command_path": spec.output_command_path,
                "command_argv": command_argv,
                "product_counters": product_counters,
                **summary,
            }
        )

    first_key = consensus_key(run_results[0])
    disagreeing = [
        result["name"] for result in run_results[1:] if consensus_key(result) != first_key
    ]
    if disagreeing:
        contract_error(
            "frozen runs do not reproduce identical allocation/counter accounting: "
            + ", ".join(disagreeing)
        )

    first = run_results[0]
    exclusion_contract = {
        category: {
            "line": line,
            "symbol": OWNER_SYMBOLS[line],
        }
        for line, category in sorted(OWNER_CATEGORIES.items())
    }
    return {
        "format_version": 1,
        "status": "ok",
        "allocation_metric": "DHAT tbk (total allocated blocks/allocation calls)",
        "reference": {
            "binary": {
                "path": str(binary_path),
                "command_path": binary_command_path,
                "sha256": binary_sha256,
            },
            "fixture": {
                "path": str(fixture_path),
                "command_path": fixture_command_path,
                "sha256": fixture_sha256,
            },
            "expected_run_count": args.expected_run_count,
            "product_counters": first["product_counters"],
        },
        "classification_contract": {
            "scorer_symbol": SCORER_SYMBOL,
            "scorer_lines": list(SCORER_LINES),
            "allocation_owner_rule": (
                "first chart_spr_search.hpp frame at or below the scorer frame"
            ),
            "recognized_owner_identity_count": len(OWNER_SYMBOLS),
            "excluded_owner_identities": exclusion_contract,
        },
        "runs": run_results,
        "consensus": {
            "identical_across_runs": True,
            "run_count": len(run_results),
            "scorer_allocation_points": first["scorer_allocation_points"],
            "total_allocations": first["total_allocations"],
            "excluded_allocations": first["excluded_allocations"],
            "excluded_allocations_total": first["excluded_allocations_total"],
            "included_allocations": first["included_allocations"],
            "candidates": args.expected_candidates,
        },
        "comparison": build_comparison(
            first["included_allocations"],
            args.expected_candidates,
            args.required_reduction_percent,
            args.current_included_allocations,
            args.current_candidates,
        ),
    }


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_arguments(sys.argv[1:] if argv is None else argv)
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
