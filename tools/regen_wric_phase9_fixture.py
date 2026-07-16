#!/usr/bin/env python3
"""Regenerate the deterministic Phase-9 three-accept chart-SPR fixture.

The topology is the serialized equivalent of the three-disjoint-quartet test
fixture in ``test/chart_spr_search_test.cpp``.  Every alignment column favors
all three intended quartet repairs by one Fitch step, while exposing the same
state set at each quartet root before and after the repair.  The base layer is
the Cartesian product of the 12 ordered pairs of distinct nucleotides across
the three quartets.  Three balanced supplemental layers make one quartet
constant and keep the other two preferred.  Together they yield 2,592 exact
active patterns.  The constant quartet contributes no competing topology
signal, so the added work preserves the independent three-move objective.

Regeneration is pinned to the frozen Phase-0 semantic oracle.  Two independent
source conversions and two final-input round trips must produce byte-identical
protobuf and canonical semantic output.

This tool deliberately does not seal the Phase-9 supplemental workload
manifest: timing evidence is a separate quiet-host acceptance step.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import NoReturn, Sequence

sys.dont_write_bytecode = True

from regen_wric_lazy_fixtures import (
    FixtureError,
    checked_oracle,
    deterministic_gzip,
    extract_reference_sequence,
    require_equal,
    run_oracle,
    sha256_bytes,
    sha256_file,
)


NUCLEOTIDES = "ACGT"
TAXA = tuple("ABCDEFGHIJKL")
QUARTETS = (("A", "B", "C", "D"), ("E", "F", "G", "H"),
            ("I", "J", "K", "L"))
ORDERED_DISTINCT_PAIRS = tuple(
    (left, right)
    for left in NUCLEOTIDES
    for right in NUCLEOTIDES
    if left != right
)
PREFERRED_QUARTET_CODES = tuple(
    (left, left, right, right)
    for left, right in ORDERED_DISTINCT_PAIRS
)
NEUTRAL_QUARTET_CODES = (("A", "A", "A", "A"), ("C", "C", "C", "C"))
BASE_PATTERN_COUNT = len(PREFERRED_QUARTET_CODES) ** len(QUARTETS)
SUPPLEMENTAL_PATTERN_COUNT = (
    len(QUARTETS)
    * len(NEUTRAL_QUARTET_CODES)
    * len(PREFERRED_QUARTET_CODES) ** (len(QUARTETS) - 1)
)
ACTIVE_PATTERN_COUNT = BASE_PATTERN_COUNT + SUPPLEMENTAL_PATTERN_COUNT
OUTPUT_PB = Path("test/wric_chart_three_accepts.pb.gz")
OUTPUT_REF = Path("test/wric_chart_three_accepts.ref")
PHASE9_ORACLE_SHA256 = (
    "7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c"
)


def fail(message: str) -> NoReturn:
    raise FixtureError(message)


def checked_phase9_oracle(path: Path) -> Path:
    oracle = checked_oracle(path)
    actual = sha256_file(oracle)
    if actual != PHASE9_ORACLE_SHA256:
        fail(
            "phase-9 frozen oracle SHA-256 mismatch: "
            f"expected {PHASE9_ORACLE_SHA256}, got {actual}"
        )
    return oracle


def fitch_combine(
    left: frozenset[str], right: frozenset[str]
) -> tuple[frozenset[str], int]:
    intersection = left & right
    return (intersection, 0) if intersection else (left | right, 1)


def quartet_fitch_contract(
    code: tuple[str, str, str, str]
) -> tuple[int, int, frozenset[str], frozenset[str]]:
    states = tuple(frozenset((state,)) for state in code)
    wrong_first, wrong_first_cost = fitch_combine(states[0], states[2])
    wrong_second, wrong_second_cost = fitch_combine(states[1], states[3])
    wrong_root, wrong_root_cost = fitch_combine(wrong_first, wrong_second)
    wrong_cost = wrong_first_cost + wrong_second_cost + wrong_root_cost

    correct_first, correct_first_cost = fitch_combine(states[0], states[1])
    correct_second, correct_second_cost = fitch_combine(states[2], states[3])
    correct_root, correct_root_cost = fitch_combine(
        correct_first, correct_second
    )
    correct_cost = correct_first_cost + correct_second_cost + correct_root_cost
    return wrong_cost, correct_cost, wrong_root, correct_root


def assert_quartet_signal_contract(code: tuple[str, str, str, str]) -> None:
    wrong_cost, correct_cost, wrong_root, correct_root = (
        quartet_fitch_contract(code)
    )

    if wrong_cost != 2 or correct_cost != 1 or wrong_root != correct_root:
        fail(
            "phase-9 quartet signal no longer has the 2-to-1 Fitch repair "
            f"contract for {''.join(code)}"
        )


def assert_quartet_neutral_contract(code: tuple[str, str, str, str]) -> None:
    wrong_cost, correct_cost, wrong_root, correct_root = (
        quartet_fitch_contract(code)
    )
    if wrong_cost != correct_cost or wrong_root != correct_root:
        fail(
            "phase-9 neutral quartet changes the repair objective for "
            f"{''.join(code)}"
        )


def fixture_quartet_codes():
    yield from itertools.product(
        PREFERRED_QUARTET_CODES, repeat=len(QUARTETS)
    )
    for neutral_index in range(len(QUARTETS)):
        pools = [PREFERRED_QUARTET_CODES] * len(QUARTETS)
        pools[neutral_index] = NEUTRAL_QUARTET_CODES
        yield from itertools.product(*pools)


def fixture_sources() -> dict[str, bytes]:
    if len(ORDERED_DISTINCT_PAIRS) != 12 or ACTIVE_PATTERN_COUNT != 2592:
        fail("phase-9 ordered-pair pattern contract changed")
    for code in PREFERRED_QUARTET_CODES:
        assert_quartet_signal_contract(code)
    for code in NEUTRAL_QUARTET_CODES:
        assert_quartet_neutral_contract(code)

    sequences = {taxon: bytearray() for taxon in TAXA}
    columns: set[bytes] = set()
    for quartet_codes in fixture_quartet_codes():
        state_by_taxon: dict[str, str] = {}
        for quartet, code in zip(QUARTETS, quartet_codes, strict=True):
            for taxon, state in zip(quartet, code, strict=True):
                state_by_taxon[taxon] = state
        column = bytes(ord(state_by_taxon[taxon]) for taxon in TAXA)
        if len(set(column)) == 1:
            fail("phase-9 recipe unexpectedly produced an invariant column")
        if column in columns:
            fail("phase-9 recipe unexpectedly produced a duplicate column")
        columns.add(column)
        for taxon, state in zip(TAXA, column, strict=True):
            sequences[taxon].append(state)

    if len(columns) != ACTIVE_PATTERN_COUNT:
        fail(
            "phase-9 active-pattern recipe changed: "
            f"expected {ACTIVE_PATTERN_COUNT}, got {len(columns)}"
        )

    fasta = bytearray()
    for taxon in TAXA:
        sequence = bytes(sequences[taxon])
        if len(sequence) != ACTIVE_PATTERN_COUNT:
            fail(f"phase-9 sequence length changed for taxon {taxon}")
        fasta.extend(f">{taxon}\n".encode("ascii"))
        fasta.extend(sequence)
        fasta.extend(b"\n")

    # Three disjoint misplaced quartet cherries, joined by the same ladder as
    # make_three_misplaced_groups_tree().
    newick = b"((((A,C),(B,D)),((E,G),(F,H))),((I,K),(J,L)));\n"
    reference = b"A" * ACTIVE_PATTERN_COUNT + b"\n"
    return {"fasta": bytes(fasta), "newick": newick, "reference": reference}


def source_conversion(
    oracle: Path, temporary: Path, sources: dict[str, bytes]
) -> tuple[bytes, bytes, dict[str, str]]:
    source_paths = {
        "fasta": temporary / "phase9.fa",
        "newick": temporary / "phase9.nwk",
        "reference": temporary / "phase9.ref",
    }
    for name, path in source_paths.items():
        path.write_bytes(sources[name])

    outputs: list[Path] = []
    canonicals: list[Path] = []
    for repetition in (1, 2):
        output = temporary / f"phase9-source-{repetition}.pb"
        canonical = temporary / f"phase9-source-{repetition}.canonical.json"
        run_oracle(
            oracle,
            [
                "--fasta",
                str(source_paths["fasta"]),
                "--newick",
                str(source_paths["newick"]),
                "--refseq",
                str(source_paths["reference"]),
                "--force-no-vcf",
                "--validate",
                "--canonical-dag-result",
                str(canonical),
                "-o",
                str(output),
            ],
            stdout_path=temporary / f"phase9-source-{repetition}.stdout",
            stderr_path=temporary / f"phase9-source-{repetition}.stderr",
        )
        outputs.append(output)
        canonicals.append(canonical)
    require_equal(outputs[0], outputs[1], "phase-9 source PB")
    require_equal(canonicals[0], canonicals[1], "phase-9 source canonical JSON")
    return (
        outputs[0].read_bytes(),
        canonicals[0].read_bytes(),
        {name: sha256_bytes(payload) for name, payload in sources.items()},
    )


def audit_final_input(
    oracle: Path, temporary: Path, encoded: bytes
) -> dict[str, object]:
    input_path = temporary / "phase9.pb.gz"
    input_path.write_bytes(encoded)
    outputs: list[Path] = []
    canonicals: list[Path] = []
    for repetition in (1, 2):
        output = temporary / f"phase9-roundtrip-{repetition}.pb"
        canonical = temporary / f"phase9-roundtrip-{repetition}.canonical.json"
        run_oracle(
            oracle,
            [
                "--dag-pb",
                str(input_path),
                "--force-no-vcf",
                "--validate",
                "--canonical-dag-result",
                str(canonical),
                "-o",
                str(output),
            ],
            stdout_path=temporary / f"phase9-roundtrip-{repetition}.stdout",
            stderr_path=temporary / f"phase9-roundtrip-{repetition}.stderr",
        )
        outputs.append(output)
        canonicals.append(canonical)
    require_equal(outputs[0], outputs[1], "phase-9 round-trip PB")
    require_equal(canonicals[0], canonicals[1], "phase-9 canonical JSON")
    canonical = json.loads(canonicals[0].read_text(encoding="utf-8"))
    return {
        "canonical_json_sha256": sha256_file(canonicals[0]),
        "canonical_semantic_sha256": canonical["semantic_sha256"],
        "clade_count": canonical["clade_count"],
        "parsimony_min": canonical["parsimony_min"],
        "production_count": canonical["production_count"],
        "roundtrip_pb_sha256": sha256_file(outputs[0]),
    }


def pattern_audit(
    oracle: Path, temporary: Path, encoded: bytes
) -> dict[str, int]:
    input_path = temporary / "phase9-pattern-audit.pb.gz"
    input_path.write_bytes(encoded)
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "TZ": "UTC"})
    completed = subprocess.run(
        [
            str(oracle),
            "--dag-pb",
            str(input_path),
            "--force-no-vcf",
            "--validate",
            "--chart-pattern-info",
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace")
        fail(f"frozen pattern audit exited {completed.returncode}: {detail.rstrip()}")
    report = completed.stdout.decode("utf-8", errors="strict")
    wanted = {"total_sites", "taxa", "exact_patterns", "invariant_sites",
              "variable_sites"}
    parsed: dict[str, int] = {}
    for line in report.splitlines():
        stripped = line.strip()
        if ":" not in stripped:
            continue
        name, value = stripped.split(":", 1)
        if name in wanted:
            try:
                parsed[name] = int(value.strip())
            except ValueError:
                fail(f"non-integer frozen pattern-audit field {name}: {value}")
    if set(parsed) != wanted:
        fail(f"frozen pattern audit omitted fields: {sorted(wanted - set(parsed))}")
    expected = {
        "total_sites": ACTIVE_PATTERN_COUNT,
        "taxa": len(TAXA),
        "exact_patterns": ACTIVE_PATTERN_COUNT,
        "invariant_sites": 0,
        "variable_sites": ACTIVE_PATTERN_COUNT,
    }
    if parsed != expected:
        fail(f"frozen pattern audit changed: expected {expected}, got {parsed}")
    return parsed


def publish_or_check(
    repo_root: Path, encoded: bytes, reference: bytes, *, check: bool
) -> None:
    for relative, expected in ((OUTPUT_PB, encoded), (OUTPUT_REF, reference)):
        target = repo_root / relative
        if check:
            if not target.is_file():
                fail(f"generated fixture is missing: {relative}")
            if target.read_bytes() != expected:
                fail(f"generated fixture is stale: {relative}")
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        staged = target.with_name(target.name + ".tmp")
        staged.write_bytes(expected)
        os.chmod(staged, 0o644)
        os.replace(staged, target)


def regenerate(oracle: Path, repo_root: Path, *, check: bool) -> dict[str, object]:
    sources = fixture_sources()
    with tempfile.TemporaryDirectory(prefix="wric-phase9-fixture-") as temp_name:
        temporary = Path(temp_name)
        raw, source_canonical, source_hashes = source_conversion(
            oracle, temporary, sources
        )
        if extract_reference_sequence(raw) + b"\n" != sources["reference"]:
            fail("phase-9 PB reference differs from generated reference")
        encoded = deterministic_gzip(raw)
        if deterministic_gzip(raw) != encoded:
            fail("deterministic gzip emitted different phase-9 bytes")
        final_audit = audit_final_input(oracle, temporary, encoded)
        if sha256_bytes(source_canonical) != final_audit["canonical_json_sha256"]:
            fail("phase-9 source and final-input canonical JSON differ")
        patterns = pattern_audit(oracle, temporary, encoded)

    publish_or_check(
        repo_root, encoded, sources["reference"], check=check
    )
    return {
        "schema": "wric_phase9_fixture_regeneration",
        "schema_version": 1,
        "mode": "check" if check else "write",
        "frozen_oracle_sha256": sha256_file(oracle),
        "recipe": {
            "taxa": len(TAXA),
            "quartets": [list(quartet) for quartet in QUARTETS],
            "ordered_distinct_pairs_per_quartet": len(ORDERED_DISTINCT_PAIRS),
            "preferred_base_patterns": BASE_PATTERN_COUNT,
            "neutral_codes_per_supplemental_layer": len(
                NEUTRAL_QUARTET_CODES
            ),
            "balanced_supplemental_patterns": SUPPLEMENTAL_PATTERN_COUNT,
            "sites": ACTIVE_PATTERN_COUNT,
            "active_patterns": ACTIVE_PATTERN_COUNT,
            "source_sha256": source_hashes,
        },
        "fixtures": {
            "protobuf": {
                "path": str(OUTPUT_PB),
                "sha256": sha256_bytes(encoded),
                "size_bytes": len(encoded),
            },
            "reference": {
                "path": str(OUTPUT_REF),
                "sha256": sha256_bytes(sources["reference"]),
                "size_bytes": len(sources["reference"]),
            },
        },
        "pattern_audit": patterns,
        "oracle_audit": final_audit,
    }


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dagutil",
        required=True,
        type=Path,
        help="frozen Phase-0 dagutil (must match the pinned SHA-256)",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify tracked fixture bytes instead of replacing them",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    try:
        arguments = parse_args(argv)
        repo_root = arguments.repo_root.resolve()
        if not (repo_root / ".git").exists():
            fail(f"not a repository worktree: {repo_root}")
        report = regenerate(
            checked_phase9_oracle(arguments.dagutil),
            repo_root,
            check=arguments.check,
        )
        json.dump(report, sys.stdout, sort_keys=True, indent=2)
        sys.stdout.write("\n")
        return 0
    except FixtureError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
