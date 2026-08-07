#!/usr/bin/env python3.14
"""Coherent larch raw-v1 handoff mutations for the producer validator."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import tempfile


ROLES = {
    "provenance.tsv": "identity",
    "score_census.tsv": "claim",
    "selected_topologies.tsv": "table",
    "semantics.tsv": "identity",
}
SEMANTIC_FILES = sorted({
    "score_census.tsv", "selected_topologies.tsv", "semantics.tsv",
})


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def identity(domain: bytes, value: bytes) -> str:
    return digest(domain + value)


def lines(path: pathlib.Path) -> list[str]:
    value = path.read_text(encoding="ascii")
    assert value.endswith("\n") and not value.endswith("\n\n")
    return value[:-1].split("\n")


def write_lines(path: pathlib.Path, rows: list[str]) -> None:
    path.write_text("\n".join(rows) + "\n", encoding="ascii", newline="")


def replace_key(path: pathlib.Path, key: str, value: str) -> None:
    rows = lines(path)
    prefix = key + "\t"
    found = False
    for index, row in enumerate(rows):
        if row.startswith(prefix):
            rows[index] = prefix + value
            found = True
    assert found
    write_lines(path, rows)


def replace_cell(path: pathlib.Path, key_column: int, key: str,
                 value_column: int, value: str) -> None:
    rows = lines(path)
    found = False
    for index in range(1, len(rows)):
        cells = rows[index].split("\t")
        if cells[key_column] == key:
            cells[value_column] = value
            rows[index] = "\t".join(cells)
            found = True
    assert found
    write_lines(path, rows)


def rebuild(root: pathlib.Path) -> None:
    replace_key(root / "manifest.tsv", "semantics_hash", identity(
        b"larch.topology-score-band.semantics.v1\n",
        (root / "semantics.tsv").read_bytes()))
    replace_key(root / "manifest.tsv", "provenance_id", identity(
        b"larch.topology-score-band.provenance.v1\n",
        (root / "provenance.tsv").read_bytes()))
    semantic_index = b"".join(
        name.encode() + b"\t" + digest((root / name).read_bytes()).encode() + b"\n"
        for name in SEMANTIC_FILES)
    replace_key(root / "manifest.tsv", "semantic_data_sha256", identity(
        b"larch.topology-score-band.semantic-data.v1\n", semantic_index))
    file_rows = ["filename\trole\trow_count\tsha256"]
    for name in sorted(ROLES):
        value = (root / name).read_bytes()
        file_rows.append(
            f"{name}\t{ROLES[name]}\t{value.count(bytes([10])) - 1}\t{digest(value)}")
    write_lines(root / "files.tsv", file_rows)
    replace_key(root / "manifest.tsv", "files_row_count", str(len(ROLES)))
    replace_key(root / "manifest.tsv", "files_sha256",
                digest((root / "files.tsv").read_bytes()))
    checksum = [
        f"{digest((root / name).read_bytes())}\t{name}"
        for name in sorted(set(ROLES) | {"files.tsv", "manifest.tsv"})
    ]
    write_lines(root / "SHA256SUMS", checksum)


def rebuild_outer_checksum(root: pathlib.Path) -> None:
    checksum = [
        f"{digest((root / name).read_bytes())}\t{name}"
        for name in sorted(set(ROLES) | {"files.tsv", "manifest.tsv"})
    ]
    write_lines(root / "SHA256SUMS", checksum)


def mutate(root: pathlib.Path, mode: str) -> tuple[str, ...]:
    first = "0"
    second = "1"
    if mode == "outer_hash":
        replace_cell(root / "selected_topologies.tsv", 0, first, 2, "11")
        return ("checksum_mismatch",)
    if mode == "table_hash":
        replace_cell(root / "files.tsv", 0, "selected_topologies.tsv", 3,
                     "0" * 64)
        rebuild_outer_checksum(root)
        return ("checksum_mismatch",)
    if mode == "schema":
        replace_key(root / "manifest.tsv", "schema_name", "wrong")
        rebuild(root)
        return ("schema",)
    if mode == "fixed_token":
        replace_key(root / "semantics.tsv", "hodge_metric", "dense")
        rebuild(root)
        return ("identity_mismatch",)
    if mode == "ordinal":
        replace_cell(root / "score_census.tsv", 0, second, 0, "3")
        rebuild(root)
        return ("grammar_mapping", "selected_topology")
    if mode == "census_score":
        replace_cell(root / "score_census.tsv", 0, second, 1, "10")
        rebuild(root)
        return ("grammar_mapping",)
    if mode == "interval":
        replace_key(root / "semantics.tsv", "score_view",
                    "absolute_score_interval_inclusive_v1:10:10")
        rebuild(root)
        return ("grammar_mapping", "selected_topology")
    if mode == "coverage":
        replace_cell(root / "selected_topologies.tsv", 0, first, 3, "not_checked")
        rebuild(root)
        return ("selected_topology",)
    if mode == "false_oracle":
        replace_cell(root / "selected_topologies.tsv", 0, first, 2, "11")
        rebuild(root)
        return ("selected_topology",)
    if mode == "oracle_name":
        replace_cell(root / "selected_topologies.tsv", 0, first, 6,
                     "not_applicable")
        rebuild(root)
        return ("selected_topology",)
    if mode == "validation":
        replace_cell(root / "selected_topologies.tsv", 0, first, 21, "false")
        rebuild(root)
        return ("selected_topology",)
    if mode == "audit":
        replace_cell(root / "selected_topologies.tsv", 0, first, 24, "bad")
        rebuild(root)
        return ("selected_topology",)
    if mode == "bad_hash":
        replace_cell(root / "selected_topologies.tsv", 0, first, 11, "0" * 64)
        rebuild(root)
        return ("selected_topology",)
    if mode == "wrong_domain":
        rows = lines(root / "selected_topologies.tsv")
        cells = rows[1].split("\t")
        cells[11] = hashlib.sha256(
            b"topology-landscape.rooted-labelled-topology.v2\n" +
            cells[10].encode()).hexdigest()
        rows[1] = "\t".join(cells)
        write_lines(root / "selected_topologies.tsv", rows)
        rebuild(root)
        return ("selected_topology",)
    if mode == "taxon":
        rows = lines(root / "selected_topologies.tsv")
        cells = rows[2].split("\t")
        cells[10] = cells[10].replace("L1:C", "L1:D")
        cells[11] = hashlib.sha256(
            b"topology-landscape.rooted-labelled-topology.v1\n" +
            cells[10].encode()).hexdigest()
        rows[2] = "\t".join(cells)
        write_lines(root / "selected_topologies.tsv", rows)
        rebuild(root)
        return ("topology_identity",)
    if mode == "range":
        replace_cell(root / "score_census.tsv", 0, "2", 1,
                     "9223372036854775808")
        rebuild(root)
        return ("grammar_mapping",)
    if mode == "empty_view":
        replace_key(root / "semantics.tsv", "score_view",
                    "absolute_score_interval_inclusive_v1:13:14")
        rebuild(root)
        return ("grammar_mapping", "selected_topology")
    if mode == "duplicate":
        rows = lines(root / "selected_topologies.tsv")
        first_cells = rows[1].split("\t")
        second_cells = rows[2].split("\t")
        second_cells[10] = first_cells[10]
        second_cells[11] = first_cells[11]
        rows[2] = "\t".join(second_cells)
        write_lines(root / "selected_topologies.tsv", rows)
        rebuild(root)
        return ("selected_topology",)
    if mode == "numeric_order":
        replace_cell(root / "selected_topologies.tsv", 0, second, 0, "01")
        rebuild(root)
        return ("selected_topology",)
    raise AssertionError(mode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("validator", type=pathlib.Path)
    parser.add_argument("fixture", type=pathlib.Path)
    parser.add_argument("mode", choices=(
        "valid", "outer_hash", "table_hash", "schema", "fixed_token", "ordinal",
        "census_score", "interval", "coverage", "false_oracle", "oracle_name",
        "validation", "audit", "bad_hash", "wrong_domain", "taxon", "range",
        "empty_view", "duplicate", "numeric_order"))
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="topology-score-band-raw-test-") as temp:
        root = pathlib.Path(temp) / "raw"
        shutil.copytree(arguments.fixture, root)
        expected: tuple[str, ...] = ()
        if arguments.mode != "valid":
            expected = mutate(root, arguments.mode)
        result = subprocess.run(
            [str(arguments.validator), "validate-score-band-raw", str(root)],
            check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if arguments.mode == "valid":
            if result.returncode != 0:
                print(result.stdout + result.stderr)
                return 1
        elif result.returncode == 0 or not any(
                code in result.stderr for code in expected):
            print(f"expected rejection {expected}; exit={result.returncode}")
            print(result.stdout + result.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
