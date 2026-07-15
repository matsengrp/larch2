#!/usr/bin/env python3
"""Regenerate the two deterministic Phase-7 lazy-chart fixtures.

The high-compression tree and alignment are generated from the recipe frozen
in ``doc/WRIC-CHART-PARALLELIZATION-PLAN.md``.  The dense-favoring fixture is
an exact copy of the pre-existing five-tree smoke input.  Regeneration is
deliberately pinned to the frozen Phase-0 semantic oracle: both the source
conversion and a final-input round trip must produce byte-identical protobuf
and canonical semantic output on two independent runs.

This tool does not create or seal the Phase-7 supplemental workload manifest.
That remains a separate quiet-host evidence step after Phase 0 is sealed.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from typing import NoReturn, Sequence
import zlib


ORACLE_SHA256 = (
    "7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c"
)
DENSE_SOURCE = Path("data/test_5_trees/tree_0.pb.gz")
DENSE_SOURCE_SHA256 = (
    "e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6"
)

HIGH_TAXA = 512
HIGH_GROUP_SIZE = 64
HIGH_SITE_COUNT = 2048
HIGH_ACTIVE_PATTERN_COUNT = 2046
NUCLEOTIDES = "ACGT"

OUTPUTS = {
    "high_pb": Path("test/wric_lazy_high_compression.pb.gz"),
    "high_ref": Path("test/wric_lazy_high_compression.ref"),
    "dense_pb": Path("test/wric_lazy_dense_favoring.pb.gz"),
    "dense_ref": Path("test/wric_lazy_dense_favoring.ref"),
}


class FixtureError(RuntimeError):
    pass


def fail(message: str) -> NoReturn:
    raise FixtureError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_oracle(path: Path) -> Path:
    oracle = path.resolve()
    if not oracle.is_file() or not os.access(oracle, os.X_OK):
        fail(f"frozen oracle is not executable: {oracle}")
    actual = sha256_file(oracle)
    if actual != ORACLE_SHA256:
        fail(
            "frozen oracle SHA-256 mismatch: "
            f"expected {ORACLE_SHA256}, got {actual}"
        )
    return oracle


def balanced_newick(names: Sequence[str]) -> bytes:
    level = list(names)
    if not level or len(level) & (len(level) - 1):
        fail("balanced fixture requires a non-empty power-of-two taxon count")
    while len(level) != 1:
        level = [
            f"({level[index]},{level[index + 1]})"
            for index in range(0, len(level), 2)
        ]
    return (level[0] + ";\n").encode("ascii")


def high_compression_sources() -> dict[str, bytes]:
    if HIGH_TAXA != 8 * HIGH_GROUP_SIZE:
        fail("high-compression recipe must contain eight equal macro-groups")

    fasta = bytearray()
    distinct_columns: set[bytes] = set()
    invariant_columns = 0
    sequences: list[bytes] = []
    for taxon in range(HIGH_TAXA):
        group = taxon // HIGH_GROUP_SIZE
        digit_index = group if group < 4 else group - 4
        sequence = bytearray()
        for position in range(HIGH_SITE_COUNT):
            value = (
                position % 256
                if group < 4
                else (position // 8) % 256
            )
            digit = value // (4**digit_index) % 4
            sequence.append(ord(NUCLEOTIDES[digit]))
        sequence_bytes = bytes(sequence)
        sequences.append(sequence_bytes)
        fasta.extend(f">T{taxon:04d}\n".encode("ascii"))
        fasta.extend(sequence_bytes)
        fasta.extend(b"\n")

    for position in range(HIGH_SITE_COUNT):
        column = bytes(sequence[position] for sequence in sequences)
        distinct_columns.add(column)
        if len(set(column)) == 1:
            invariant_columns += 1
    active_patterns = len(distinct_columns) - invariant_columns
    if len(distinct_columns) != HIGH_SITE_COUNT:
        fail("high-compression recipe unexpectedly produced duplicate columns")
    if invariant_columns != 2 or active_patterns != HIGH_ACTIVE_PATTERN_COUNT:
        fail(
            "high-compression recipe pattern contract changed: "
            f"invariant={invariant_columns}, active={active_patterns}"
        )

    names = [f"T{taxon:04d}" for taxon in range(HIGH_TAXA)]
    return {
        "fasta": bytes(fasta),
        "newick": balanced_newick(names),
        "reference": b"A" * HIGH_SITE_COUNT + b"\n",
    }


def deterministic_gzip(payload: bytes) -> bytes:
    compressor = zlib.compressobj(
        level=9,
        method=zlib.DEFLATED,
        wbits=-zlib.MAX_WBITS,
        memLevel=9,
        strategy=zlib.Z_DEFAULT_STRATEGY,
    )
    body = compressor.compress(payload) + compressor.flush()
    header = b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff"
    trailer = struct.pack("<II", zlib.crc32(payload), len(payload) & 0xFFFFFFFF)
    result = header + body + trailer
    if gzip.decompress(result) != payload:
        fail("internal deterministic-gzip round trip failed")
    return result


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while offset < len(data) and shift <= 63:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
    fail("malformed protobuf varint while extracting reference sequence")


def extract_reference_sequence(encoded_dag: bytes) -> bytes:
    data = (
        gzip.decompress(encoded_dag)
        if encoded_dag[:2] == b"\x1f\x8b"
        else encoded_dag
    )
    references: list[bytes] = []
    offset = 0
    while offset < len(data):
        key, offset = read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 7
        if wire_type == 0:
            _, offset = read_varint(data, offset)
        elif wire_type == 1:
            offset += 8
        elif wire_type == 2:
            length, offset = read_varint(data, offset)
            end = offset + length
            if end > len(data):
                fail("truncated protobuf field while extracting reference sequence")
            if field_number == 4:
                references.append(data[offset:end])
            offset = end
        elif wire_type == 5:
            offset += 4
        else:
            fail(f"unsupported protobuf wire type {wire_type}")
        if offset > len(data):
            fail("truncated protobuf while extracting reference sequence")
    if len(references) != 1 or not references[0]:
        fail(f"expected one non-empty DAG reference sequence, got {len(references)}")
    return references[0]


def run_oracle(
    oracle: Path,
    arguments: Sequence[str],
    *,
    stdout_path: Path,
    stderr_path: Path,
) -> None:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "TZ": "UTC"})
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        completed = subprocess.run(
            [str(oracle), *arguments],
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            env=environment,
            check=False,
        )
    if completed.returncode != 0:
        detail = stderr_path.read_text(encoding="utf-8", errors="replace")
        fail(f"frozen oracle exited {completed.returncode}: {detail.rstrip()}")


def require_equal(first: Path, second: Path, label: str) -> None:
    if first.read_bytes() != second.read_bytes():
        fail(f"two frozen-oracle runs produced different {label}")


def source_conversion(
    oracle: Path, temporary: Path, sources: dict[str, bytes]
) -> tuple[bytes, bytes, dict[str, str]]:
    source_paths = {
        "fasta": temporary / "high.fa",
        "newick": temporary / "high.nwk",
        "reference": temporary / "high.ref",
    }
    for name, path in source_paths.items():
        path.write_bytes(sources[name])

    outputs: list[Path] = []
    canonicals: list[Path] = []
    for repetition in (1, 2):
        output = temporary / f"high-source-{repetition}.pb"
        canonical = temporary / f"high-source-{repetition}.canonical.json"
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
            stdout_path=temporary / f"high-source-{repetition}.stdout",
            stderr_path=temporary / f"high-source-{repetition}.stderr",
        )
        outputs.append(output)
        canonicals.append(canonical)
    require_equal(outputs[0], outputs[1], "high-compression source PB")
    require_equal(
        canonicals[0], canonicals[1], "high-compression source canonical JSON"
    )
    return (
        outputs[0].read_bytes(),
        canonicals[0].read_bytes(),
        {name: sha256_bytes(payload) for name, payload in sources.items()},
    )


def audit_final_input(
    oracle: Path, temporary: Path, name: str, encoded: bytes
) -> dict[str, object]:
    input_path = temporary / f"{name}.pb.gz"
    input_path.write_bytes(encoded)
    outputs: list[Path] = []
    canonicals: list[Path] = []
    for repetition in (1, 2):
        output = temporary / f"{name}-roundtrip-{repetition}.pb"
        canonical = temporary / f"{name}-roundtrip-{repetition}.canonical.json"
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
            stdout_path=temporary / f"{name}-roundtrip-{repetition}.stdout",
            stderr_path=temporary / f"{name}-roundtrip-{repetition}.stderr",
        )
        outputs.append(output)
        canonicals.append(canonical)
    require_equal(outputs[0], outputs[1], f"{name} round-trip PB")
    require_equal(canonicals[0], canonicals[1], f"{name} canonical JSON")
    canonical = json.loads(canonicals[0].read_text(encoding="utf-8"))
    return {
        "canonical_json_sha256": sha256_file(canonicals[0]),
        "canonical_semantic_sha256": canonical["semantic_sha256"],
        "clade_count": canonical["clade_count"],
        "parsimony_min": canonical["parsimony_min"],
        "production_count": canonical["production_count"],
        "roundtrip_pb_sha256": sha256_file(outputs[0]),
    }


def publish_or_check(
    repo_root: Path, payloads: dict[str, bytes], *, check: bool
) -> None:
    for name, relative in OUTPUTS.items():
        target = repo_root / relative
        expected = payloads[name]
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
    dense_source = repo_root / DENSE_SOURCE
    if not dense_source.is_file():
        fail(f"dense fixture source is missing: {DENSE_SOURCE}")
    dense_pb = dense_source.read_bytes()
    actual_dense_sha = sha256_bytes(dense_pb)
    if actual_dense_sha != DENSE_SOURCE_SHA256:
        fail(
            "dense fixture source SHA-256 mismatch: "
            f"expected {DENSE_SOURCE_SHA256}, got {actual_dense_sha}"
        )
    dense_reference = extract_reference_sequence(dense_pb) + b"\n"

    sources = high_compression_sources()
    with tempfile.TemporaryDirectory(prefix="wric-lazy-fixtures-") as temp_name:
        temporary = Path(temp_name)
        high_raw, source_canonical, source_hashes = source_conversion(
            oracle, temporary, sources
        )
        if extract_reference_sequence(high_raw) + b"\n" != sources["reference"]:
            fail("high-compression PB reference differs from generated reference")
        high_pb = deterministic_gzip(high_raw)
        if deterministic_gzip(high_raw) != high_pb:
            fail("deterministic gzip emitted different bytes on repetition")

        high_audit = audit_final_input(
            oracle, temporary, "high-compression", high_pb
        )
        dense_audit = audit_final_input(
            oracle, temporary, "dense-favoring", dense_pb
        )
        if sha256_bytes(source_canonical) != high_audit["canonical_json_sha256"]:
            fail("high-compression source and final-input canonical JSON differ")

    payloads = {
        "high_pb": high_pb,
        "high_ref": sources["reference"],
        "dense_pb": dense_pb,
        "dense_ref": dense_reference,
    }
    publish_or_check(repo_root, payloads, check=check)

    return {
        "schema": "wric_lazy_fixture_regeneration",
        "schema_version": 1,
        "mode": "check" if check else "write",
        "frozen_oracle_sha256": ORACLE_SHA256,
        "high_compression_recipe": {
            "taxa": HIGH_TAXA,
            "macro_groups": 8,
            "taxa_per_macro_group": HIGH_GROUP_SIZE,
            "sites": HIGH_SITE_COUNT,
            "active_patterns": HIGH_ACTIVE_PATTERN_COUNT,
            "source_sha256": source_hashes,
        },
        "fixtures": {
            name: {
                "path": str(OUTPUTS[name]),
                "sha256": sha256_bytes(payload),
                "size_bytes": len(payload),
            }
            for name, payload in payloads.items()
        },
        "oracle_audits": {
            "high_compression": high_audit,
            "dense_favoring": dense_audit,
        },
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
            # Linked worktrees contain a .git file, so exists() covers both.
            fail(f"not a repository worktree: {repo_root}")
        report = regenerate(
            checked_oracle(arguments.dagutil), repo_root, check=arguments.check
        )
        json.dump(report, sys.stdout, sort_keys=True, indent=2)
        sys.stdout.write("\n")
        return 0
    except FixtureError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
