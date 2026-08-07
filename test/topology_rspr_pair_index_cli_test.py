#!/usr/bin/env python3.14

import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile


def manifest(bundle: pathlib.Path) -> dict[str, str]:
    rows = (bundle / "manifest.tsv").read_text(encoding="ascii").splitlines()
    assert rows[0] == "key\tvalue"
    return dict(row.split("\t") for row in rows[1:])


def tree_rows(bundle: pathlib.Path) -> list[list[str]]:
    rows = (bundle / "trees.tsv").read_text(encoding="ascii").splitlines()
    return [row.split("\t") for row in rows[1:]]


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invocation(executable: pathlib.Path, bundle: pathlib.Path,
               output: pathlib.Path, score_min: int = 10,
               score_max: int = 11, count: int = 2) -> list[str]:
    identity = manifest(bundle)
    return [
        str(executable),
        "--parent", str(bundle),
        "--output", str(output),
        "--score-min", str(score_min),
        "--score-max", str(score_max),
        "--expected-tree-count", str(count),
        "--expected-analysis-policy", identity["analysis_policy_hash"],
        "--expected-landscape-semantics", identity["landscape_semantics_hash"],
        "--expected-artifact-provenance", identity["artifact_provenance_id"],
        "--expected-trees-sha256", sha256(bundle / "trees.tsv"),
    ]


def run(command: list[str], success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if success and result.returncode != 0:
        raise AssertionError(f"command failed: {result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError("command unexpectedly succeeded")
    return result


def parse_results(path: pathlib.Path) -> dict[str, str]:
    rows = path.read_text(encoding="ascii").splitlines()
    assert rows[0] == "key\tvalue"
    assert rows[1:] == sorted(rows[1:])
    return dict(row.split("\t") for row in rows[1:])


def check_valid(executable: pathlib.Path, bundle: pathlib.Path,
                root: pathlib.Path) -> None:
    first = root / "first"
    second = root / "second"
    run(invocation(executable, bundle, first), True)
    run(invocation(executable, bundle, second), True)

    expected_members = {"RESULTS.tsv", "SHA256SUMS", "endpoint_pairs.tsv"}
    assert {item.name for item in first.iterdir()} == expected_members
    for name in expected_members:
        assert (first / name).read_bytes() == (second / name).read_bytes()

    topology_ids = sorted(row[0] for row in tree_rows(bundle))
    edge_rows = f"{topology_ids[0]}\t{topology_ids[1]}\n"
    endpoint_bytes = (
        "topology_low\ttopology_high\n" + edge_rows
    ).encode("ascii")
    assert (first / "endpoint_pairs.tsv").read_bytes() == endpoint_bytes
    edge_ledger = hashlib.sha256(
        b"topology-endpoint-pair.edge-set.v1\n" + edge_rows.encode("ascii")
    ).hexdigest()

    identity = manifest(bundle)
    results = parse_results(first / "RESULTS.tsv")
    expected_keys = {
        "accepted_edge_count", "algorithm", "analysis_policy_hash",
        "candidate_bucket_count", "cut_count", "endpoint_pair_ledger_sha256",
        "exact_signature_class_count", "fingerprint_collision_bucket_count",
        "landscape_semantics_hash", "largest_exact_signature_class_size",
        "move_relation", "move_scope", "pair_coverage_method",
        "parent_artifact_provenance_id", "parent_tree_table_sha256",
        "score_max", "score_min", "tree_count", "unordered_pair_denominator",
    }
    assert set(results) == expected_keys
    assert results["accepted_edge_count"] == "1"
    assert results["algorithm"] == "collision_safe_common_prune_signature_index_v1"
    assert results["analysis_policy_hash"] == identity["analysis_policy_hash"]
    assert results["endpoint_pair_ledger_sha256"] == edge_ledger
    assert results["fingerprint_collision_bucket_count"] == "0"
    assert results["landscape_semantics_hash"] == identity["landscape_semantics_hash"]
    assert results["move_relation"] == "rooted_common_prune_reduction_hard_rspr_v1"
    assert results["move_scope"] == "hard_rspr_induced_selected_view_v1"
    assert results["pair_coverage_method"] == "complete_exact_signature_equivalence_partition"
    assert results["parent_artifact_provenance_id"] == identity["artifact_provenance_id"]
    assert results["parent_tree_table_sha256"] == sha256(bundle / "trees.tsv")
    assert results["score_min"] == "10"
    assert results["score_max"] == "11"
    assert results["tree_count"] == "2"
    assert results["unordered_pair_denominator"] == "1"

    expected_sums = (
        f"{sha256(first / 'RESULTS.tsv')}\tRESULTS.tsv\n"
        f"{sha256(first / 'endpoint_pairs.tsv')}\tendpoint_pairs.tsv\n"
    )
    assert (first / "SHA256SUMS").read_text(encoding="ascii") == expected_sums


def check_identity_reject(executable: pathlib.Path, bundle: pathlib.Path,
                          root: pathlib.Path) -> None:
    options = [
        "--expected-analysis-policy",
        "--expected-landscape-semantics",
        "--expected-artifact-provenance",
        "--expected-trees-sha256",
    ]
    for ordinal, option in enumerate(options):
        output = root / f"bad-{ordinal}"
        command = invocation(executable, bundle, output)
        command[command.index(option) + 1] = "0" * 64
        result = run(command, False)
        assert "mismatch" in result.stderr
        assert not output.exists()


def check_selection_reject(executable: pathlib.Path, bundle: pathlib.Path,
                           root: pathlib.Path) -> None:
    wrong_count = root / "wrong-count"
    result = run(invocation(executable, bundle, wrong_count, count=1), False)
    assert "selected tree count mismatch" in result.stderr
    assert not wrong_count.exists()

    empty = root / "empty"
    result = run(invocation(executable, bundle, empty, 12, 12, 2), False)
    assert "selected tree count mismatch" in result.stderr
    assert not empty.exists()

    reversed_output = root / "reversed"
    result = run(invocation(executable, bundle, reversed_output, 11, 10, 2), False)
    assert "score interval is reversed" in result.stderr
    assert not reversed_output.exists()


def check_no_replace(executable: pathlib.Path, bundle: pathlib.Path,
                     root: pathlib.Path) -> None:
    output = root / "existing"
    output.mkdir()
    sentinel = output / "sentinel"
    sentinel.write_text("preserve\n", encoding="ascii")
    result = run(invocation(executable, bundle, output), False)
    assert "output path already exists" in result.stderr
    assert sentinel.read_text(encoding="ascii") == "preserve\n"
    assert {item.name for item in output.iterdir()} == {"sentinel"}


def check_publication(executable: pathlib.Path, bundle: pathlib.Path,
                      root: pathlib.Path) -> None:
    output = root / "published"
    run(invocation(executable, bundle, output), True)
    expected_members = {"RESULTS.tsv", "SHA256SUMS", "endpoint_pairs.tsv"}
    assert output.exists()
    assert output.is_dir()
    assert {item.name for item in output.iterdir()} == expected_members

    if output.is_symlink():
        target_text = os.readlink(output)
        target = pathlib.Path(target_text)
        assert not target.is_absolute()
        assert target.parent == pathlib.Path(".")
        assert target.name.startswith(".published.staging-")
        staging = output.parent / target
        assert staging.is_dir()
        assert not staging.is_symlink()
        assert {item.name for item in staging.iterdir()} == expected_members
        for name in expected_members:
            assert (output / name).read_bytes() == (staging / name).read_bytes()
    else:
        assert not list(root.glob(".published.staging-*"))

    sums = (output / "SHA256SUMS").read_text(encoding="ascii").splitlines()
    assert sums == [
        f"{sha256(output / 'RESULTS.tsv')}\tRESULTS.tsv",
        f"{sha256(output / 'endpoint_pairs.tsv')}\tendpoint_pairs.tsv",
    ]


def check_v1_reject(executable: pathlib.Path, bundle: pathlib.Path,
                    root: pathlib.Path) -> None:
    output = root / "v1"
    result = run(invocation(executable, bundle, output), False)
    assert "parent must be topology-landscape-neutral-v2" in result.stderr
    assert not output.exists()


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit("usage: TEST EXECUTABLE V2_FIXTURE V1_FIXTURE MODE")
    executable = pathlib.Path(sys.argv[1])
    v2 = pathlib.Path(sys.argv[2])
    v1 = pathlib.Path(sys.argv[3])
    mode = sys.argv[4]
    with tempfile.TemporaryDirectory(prefix="larch-rspr-pair-index-") as temp:
        root = pathlib.Path(temp)
        if mode == "valid":
            check_valid(executable, v2, root)
        elif mode == "identity_reject":
            check_identity_reject(executable, v2, root)
        elif mode == "selection_reject":
            check_selection_reject(executable, v2, root)
        elif mode == "no_replace":
            check_no_replace(executable, v2, root)
        elif mode == "publication":
            check_publication(executable, v2, root)
        elif mode == "v1_reject":
            check_v1_reject(executable, v1, root)
        else:
            raise AssertionError(f"unknown mode: {mode}")


if __name__ == "__main__":
    main()
