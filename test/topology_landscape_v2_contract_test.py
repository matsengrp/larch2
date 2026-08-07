#!/usr/bin/env python3.14
"""Coherent neutral-v2 mutations for an exchange validator."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import tempfile


PAYLOAD_ROLES = {
    "analysis_policy.tsv": "identity",
    "artifact_provenance.tsv": "identity",
    "completeness.tsv": "claim",
    "grammar_topology_provenance.tsv": "table",
    "landscape_semantics.tsv": "identity",
    "move_witnesses.tsv": "table",
    "score_census.tsv": "claim",
    "search_run.tsv": "identity",
    "topology_edges.tsv": "table",
    "trees.tsv": "table",
}
DATA_FILES = sorted({
    "completeness.tsv", "grammar_topology_provenance.tsv",
    "move_witnesses.tsv", "score_census.tsv", "topology_edges.tsv", "trees.tsv",
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
    target = key + "\t"
    found = False
    for index, row in enumerate(rows):
        if row.startswith(target):
            rows[index] = target + value
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


def update_manifest_identity(root: pathlib.Path, key: str, domain: bytes,
                             filename: str) -> str:
    value = identity(domain, (root / filename).read_bytes())
    replace_key(root / "manifest.tsv", key, value)
    return value


def rebuild_content_ledgers(root: pathlib.Path) -> None:
    output_index = b"".join(
        name.encode() + b"\t" + digest((root / name).read_bytes()).encode() + b"\n"
        for name in DATA_FILES)
    output_hash = identity(b"topology-landscape.output-data.v2\n", output_index)
    replace_key(root / "artifact_provenance.tsv", "output_data_sha256", output_hash)
    update_manifest_identity(
        root, "artifact_provenance_id",
        b"topology-landscape.artifact-provenance.v2\n",
        "artifact_provenance.tsv")

    file_rows = ["filename\trole\trow_count\tsha256"]
    for name in sorted(PAYLOAD_ROLES):
        value = (root / name).read_bytes()
        file_rows.append(
            f"{name}\t{PAYLOAD_ROLES[name]}\t{value.count(bytes([10])) - 1}\t{digest(value)}")
    write_lines(root / "files.tsv", file_rows)
    replace_key(root / "manifest.tsv", "files_row_count", str(len(PAYLOAD_ROLES)))
    replace_key(root / "manifest.tsv", "files_sha256",
                digest((root / "files.tsv").read_bytes()))
    checksum = [
        f"{digest((root / name).read_bytes())}\t{name}"
        for name in sorted(set(PAYLOAD_ROLES) | {"files.tsv", "manifest.tsv"})
    ]
    write_lines(root / "SHA256SUMS", checksum)


def relink_all_identities(root: pathlib.Path) -> None:
    landscape = update_manifest_identity(
        root, "landscape_semantics_hash",
        b"topology-landscape.landscape-semantics.v2\n",
        "landscape_semantics.tsv")
    replace_key(root / "analysis_policy.tsv", "landscape_semantics_hash", landscape)
    replace_key(root / "search_run.tsv", "landscape_semantics_hash", landscape)
    tree_rows = lines(root / "trees.tsv")
    for index in range(1, len(tree_rows)):
        cells = tree_rows[index].split("\t")
        cells[1] = landscape
        cells[10] = landscape
        tree_rows[index] = "\t".join(cells)
    write_lines(root / "trees.tsv", tree_rows)

    analysis = update_manifest_identity(
        root, "analysis_policy_hash",
        b"topology-landscape.analysis-policy.v2\n", "analysis_policy.tsv")
    completeness = lines(root / "completeness.tsv")
    for index in range(1, len(completeness)):
        cells = completeness[index].split("\t")
        if cells[0] not in {"score_census", "trace_event"}:
            cells[2] = analysis
        elif cells[0] == "score_census":
            cells[2] = landscape
        completeness[index] = "\t".join(cells)
    write_lines(root / "completeness.tsv", completeness)

    search = update_manifest_identity(
        root, "search_run_id", b"topology-landscape.search-run.v2\n",
        "search_run.tsv")
    replace_cell(root / "completeness.tsv", 0, "trace_event", 2, search)
    rebuild_content_ledgers(root)


def mutate(root: pathlib.Path, mode: str) -> tuple[str, ...]:
    if mode == "ordinal":
        replace_cell(root / "score_census.tsv", 0, "1", 0, "3")
        rebuild_content_ledgers(root)
        return ("grammar_mapping",)
    if mode == "census_score":
        replace_cell(root / "score_census.tsv", 0, "1", 1, "10")
        rebuild_content_ledgers(root)
        return ("grammar_mapping",)
    if mode == "selected_score":
        tree = root / "trees.tsv"
        rows = lines(tree)
        cells = rows[2].split("\t")
        for column in (8, 11, 13, 16, 18, 20):
            cells[column] = "10"
        cells[9] = "0"
        rows[2] = "\t".join(cells)
        write_lines(tree, rows)
        replace_cell(root / "grammar_topology_provenance.tsv", 1, "1", 3, "10")
        rebuild_content_ledgers(root)
        return ("grammar_mapping",)
    if mode == "interval":
        replace_key(root / "analysis_policy.tsv", "score_view",
                    "absolute_score_interval_inclusive_v1:10:10")
        relink_all_identities(root)
        return ("grammar_mapping",)
    if mode == "coverage":
        replace_cell(root / "trees.tsv", 0,
                     "501054b49975d5f2a5f8bf6fc1f29bd6ec85c852798621ae50cb9f384c653833",
                     12, "not_checked")
        rebuild_content_ledgers(root)
        return ("score_mismatch", "topology_identity")
    if mode == "oracle_name":
        replace_cell(root / "trees.tsv", 0,
                     "501054b49975d5f2a5f8bf6fc1f29bd6ec85c852798621ae50cb9f384c653833",
                     15, "not_applicable")
        rebuild_content_ledgers(root)
        return ("score_mismatch", "topology_identity")
    if mode == "oracle_equal":
        replace_cell(root / "trees.tsv", 0,
                     "501054b49975d5f2a5f8bf6fc1f29bd6ec85c852798621ae50cb9f384c653833",
                     15, "synthetic_canonical_tree_sankoff_v1")
        rebuild_content_ledgers(root)
        return ("score_mismatch", "topology_identity")
    if mode == "both_na_valid":
        rows = lines(root / "trees.tsv")
        for index in range(1, len(rows)):
            cells = rows[index].split("\t")
            for score_column, status_column, oracle_column in (
                    (13, 14, 15), (20, 21, 22)):
                cells[score_column] = "-"
                cells[status_column] = "not_applicable"
                cells[oracle_column] = "not_applicable"
            rows[index] = "\t".join(cells)
        write_lines(root / "trees.tsv", rows)
        claims = lines(root / "completeness.tsv")
        targets = {"selected_grammar_sankoff_verification",
                   "tree_native_sankoff_verification"}
        for index in range(1, len(claims)):
            cells = claims[index].split("\t")
            if cells[0] in targets:
                cells[1] = "unknown"
                cells[3] = cells[4] = cells[6] = cells[7] = "-"
                cells[5] = "0"
                cells[8] = "oracle_verification_not_applicable"
                claims[index] = "\t".join(cells)
        write_lines(root / "completeness.tsv", claims)
        rebuild_content_ledgers(root)
        return ()
    if mode == "audit_digest":
        replace_cell(root / "grammar_topology_provenance.tsv", 1, "0", 8, "bad")
        rebuild_content_ledgers(root)
        return ("grammar_mapping",)
    if mode == "wrong_domain":
        tree_rows = lines(root / "trees.tsv")
        cells = tree_rows[2].split("\t")
        old = cells[0]
        wrong = hashlib.sha256(
            b"topology-landscape.rooted-labelled-topology.v2\n" +
            cells[2].encode()).hexdigest()
        cells[0] = wrong
        tree_rows[2] = "\t".join(cells)
        write_lines(root / "trees.tsv", tree_rows)
        replace_cell(root / "grammar_topology_provenance.tsv", 2, old, 2, wrong)
        rebuild_content_ledgers(root)
        return ("topology_identity",)
    if mode == "range":
        replace_cell(root / "score_census.tsv", 0, "2", 1,
                     "9223372036854775808")
        rebuild_content_ledgers(root)
        return ("grammar_mapping",)
    if mode == "empty_view":
        replace_key(root / "analysis_policy.tsv", "score_view",
                    "absolute_score_interval_inclusive_v1:13:14")
        relink_all_identities(root)
        return ("grammar_mapping", "topology_identity")
    if mode == "view_scope":
        replace_cell(root / "trees.tsv", 0,
                     "501054b49975d5f2a5f8bf6fc1f29bd6ec85c852798621ae50cb9f384c653833",
                     1, "0" * 64)
        rebuild_content_ledgers(root)
        return ("topology_identity",)
    if mode == "completeness":
        replace_cell(root / "completeness.tsv", 0, "canonical_topology", 4, "3")
        rebuild_content_ledgers(root)
        return ("completeness",)
    if mode == "digest_namespace":
        old = "501054b49975d5f2a5f8bf6fc1f29bd6ec85c852798621ae50cb9f384c653833"
        wrong = "601054b49975d5f2a5f8bf6fc1f29bd6ec85c852798621ae50cb9f384c653833"
        replace_cell(root / "trees.tsv", 0, old, 0, wrong)
        replace_cell(root / "grammar_topology_provenance.tsv", 2, old, 2, wrong)
        rebuild_content_ledgers(root)
        return ("topology_identity",)
    if mode == "cross_link":
        replace_key(root / "manifest.tsv", "analysis_policy_hash", "0" * 64)
        checksum = [
            f"{digest((root / name).read_bytes())}\t{name}"
            for name in sorted(set(PAYLOAD_ROLES) | {"files.tsv", "manifest.tsv"})
        ]
        write_lines(root / "SHA256SUMS", checksum)
        return ("identity_mismatch",)
    raise AssertionError(mode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("validator", type=pathlib.Path)
    parser.add_argument("fixture", type=pathlib.Path)
    parser.add_argument("style", choices=("larch", "tric"))
    parser.add_argument("mode", choices=(
        "valid", "ordinal", "census_score", "interval", "coverage", "oracle_name",
        "oracle_equal", "both_na_valid", "selected_score", "audit_digest",
        "wrong_domain", "range", "empty_view", "rewrite", "rewrite_no_replace",
        "view_scope", "completeness", "digest_namespace", "cross_link"))
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="topology-landscape-v2-test-") as temp:
        root = pathlib.Path(temp) / "bundle"
        shutil.copytree(arguments.fixture, root)
        if arguments.mode in {"rewrite", "rewrite_no_replace"}:
            if arguments.style != "larch":
                return 2
            output = pathlib.Path(temp) / "rewritten"
            if arguments.mode == "rewrite_no_replace":
                output.mkdir()
                sentinel = output / "sentinel"
                sentinel.write_text("unchanged\n", encoding="ascii")
            result = subprocess.run(
                [str(arguments.validator), "rewrite-canonical", str(root),
                 str(output)], check=False, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if arguments.mode == "rewrite_no_replace":
                return 0 if (result.returncode != 0 and sentinel.exists() and
                             sentinel.read_text(encoding="ascii") == "unchanged\n"
                             and "output_exists" in result.stderr) else 1
            if result.returncode != 0:
                print(result.stdout + result.stderr)
                return 1
            expected = {path.name for path in root.iterdir()}
            return 0 if ({path.name for path in output.iterdir()} == expected and
                         all((root / name).read_bytes() ==
                             (output / name).read_bytes()
                             for name in expected)) else 1
        accepting = arguments.mode in {"valid", "both_na_valid"}
        expected: tuple[str, ...] = ()
        if arguments.mode != "valid":
            expected = mutate(root, arguments.mode)
        command = ([str(arguments.validator), "validate", str(root)]
                   if arguments.style == "larch"
                   else [str(arguments.validator), str(root)])
        result = subprocess.run(command, check=False, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if accepting:
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
