#!/usr/bin/env python3
"""Adversarial tests for the immutable historical WRIC harness materializer."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import cast
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[1]
TOOL = REPO_ROOT / "tools" / "wric_historical_harness_compat.py"
sys.path.insert(0, os.fspath(TOOL.parent))
import wric_historical_harness_compat as compat


REVISIONS = (
    "7ca527b8906d018124756182274335cbff936d72",
    "870c298ff1c0c21901bdf79d341bf97d121f389c",
    "38e9a281396e5263647ba68724414848841525d7",
)
EXPECTED_VARIANTS = ("phase1-5", "phase6", "phase7-8")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.strip()


class HistoricalHarnessCompatTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def clone(self, revision: str, name: str) -> Path:
        destination = self.root / name
        run(
            (
                "/usr/bin/git",
                "clone",
                "--quiet",
                "--shared",
                os.fspath(REPO_ROOT),
                os.fspath(destination),
            )
        )
        run(("/usr/bin/git", "checkout", "--quiet", "--detach", revision), cwd=destination)
        self.assertEqual(run(("/usr/bin/git", "status", "--porcelain=v1"), cwd=destination), "")
        return destination.resolve()

    def output(self, name: str) -> Path:
        directory = self.root / f"output-{name}"
        directory.mkdir()
        return directory / "wric_spr_search_benchmark.sh"

    def create(
        self, revision: str = REVISIONS[2], name: str = "positive"
    ) -> tuple[Path, Path, Path, Mapping[str, object]]:
        product = self.clone(revision, f"product-{name}")
        harness = self.output(name)
        result = compat.materialize_historical_harness(harness, product, revision)
        metadata = harness.with_name(harness.name + ".metadata.json")
        return product, harness, metadata, result

    def test_all_three_approved_variants_are_exact_and_auditable(self) -> None:
        for index, (revision, expected_variant) in enumerate(
            zip(REVISIONS, EXPECTED_VARIANTS, strict=True)
        ):
            with self.subTest(variant=expected_variant):
                product, harness, metadata, result = self.create(
                    revision, f"variant-{index}"
                )
                self.assertEqual(result["variant"], expected_variant)
                self.assertEqual(result["harness_sha256"], sha256(harness))
                self.assertEqual(result["metadata_sha256"], sha256(metadata))
                self.assertEqual(stat.S_IMODE(harness.stat().st_mode), 0o555)
                self.assertEqual(stat.S_IMODE(metadata.stat().st_mode), 0o444)
                self.assertEqual(harness.stat().st_nlink, 1)
                self.assertEqual(metadata.stat().st_nlink, 1)
                audited = compat.audit_materialized_harness(
                    harness,
                    metadata,
                    cast(str, result["harness_sha256"]),
                    cast(str, result["metadata_sha256"]),
                    product,
                    revision,
                )
                self.assertEqual(audited["status"], "ok")
                self.assertEqual(audited["variant"], expected_variant)

                document = cast(dict[str, object], json.loads(metadata.read_text()))
                self.assertEqual(document["schema"], compat.SCHEMA)
                self.assertEqual(document["schema_version"], compat.SCHEMA_VERSION)
                transformation = cast(dict[str, object], document["transformation"])
                spec = cast(dict[str, object], transformation["spec"])
                proof = cast(list[dict[str, object]], transformation["proof"])
                self.assertEqual(spec["score_domain_source_revision"], compat.SCORE_DOMAIN_SOURCE_REVISION)
                self.assertEqual(spec["timed_trial_source_revision"], compat.TIMED_TRIAL_SOURCE_REVISION)
                self.assertEqual(len(proof), 16)
                self.assertEqual(
                    [item["name"] for item in proof[:2]],
                    ["score-domain-01", "score-domain-02"],
                )
                self.assertTrue(transformation["child_runner_logic_unchanged"])
                self.assertTrue(transformation["timing_boundary_unchanged"])

    def test_score_domains_and_per_trial_digest_flow_match_frozen_template(self) -> None:
        product, harness, _, _ = self.create(name="semantics")
        expected = subprocess.check_output(
            (
                "/usr/bin/git",
                "-C",
                os.fspath(product),
                "show",
                f"{compat.TIMED_TRIAL_SOURCE_REVISION}:{compat.HARNESS_RELATIVE_PATH}",
            )
        )
        self.assertEqual(harness.read_bytes(), expected)
        text = expected.decode()
        self.assertIn('"$reported_initial" "$initial" >"$curve"', text)
        self.assertIn('"$reported" == "$expected_final"', text)
        self.assertIn('"${canonical_args[@]}" -o "$pb"', text)
        self.assertIn("DEFERRED_SEARCH_SHA", text)
        self.assertIn("timed/companion chart semantic mismatch", text)
        self.assertNotIn(
            "search_sha=${canonical_companion_search[$row_id]:--}", text
        )
        deferred = text.index("# No validation or semantic-capture process is allowed")
        timed_validation = text.index("elif ! timed_search=$(validate_chart_canonical_result")
        self.assertGreater(timed_validation, deferred)

    def test_cli_create_and_audit_emit_canonical_external_anchors(self) -> None:
        revision = REVISIONS[1]
        product = self.clone(revision, "product-cli")
        harness = self.output("cli")
        created = subprocess.run(
            (
                sys.executable,
                os.fspath(TOOL),
                "materialize",
                "--harness",
                os.fspath(harness),
                "--product-repo-root",
                os.fspath(product),
                "--expected-product-revision",
                revision,
            ),
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        created_document = cast(dict[str, object], json.loads(created.stdout))
        self.assertEqual(created.stdout, compat._canonical_json_bytes(created_document))
        self.assertEqual(created_document["harness_sha256"], sha256(harness))
        metadata = harness.with_name(harness.name + ".metadata.json")
        self.assertEqual(created_document["metadata_sha256"], sha256(metadata))

        audited = subprocess.run(
            (
                sys.executable,
                os.fspath(TOOL),
                "audit",
                "--harness",
                os.fspath(harness),
                "--metadata",
                os.fspath(metadata),
                "--expected-harness-sha256",
                cast(str, created_document["harness_sha256"]),
                "--expected-metadata-sha256",
                cast(str, created_document["metadata_sha256"]),
                "--product-repo-root",
                os.fspath(product),
                "--expected-product-revision",
                revision,
            ),
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        audited_document = cast(dict[str, object], json.loads(audited.stdout))
        self.assertEqual(audited.stdout, compat._canonical_json_bytes(audited_document))
        self.assertEqual(audited_document["status"], "ok")

    def test_unknown_dirty_revision_alias_and_hardlinked_source_are_rejected(self) -> None:
        revision = REVISIONS[2]

        wrong_revision = self.clone(revision, "product-wrong-revision")
        with self.assertRaisesRegex(compat.CompatibilityError, "product HEAD differs"):
            compat.materialize_historical_harness(
                self.output("wrong-revision"), wrong_revision, REVISIONS[1]
            )

        dirty = self.clone(revision, "product-dirty")
        source = dirty / compat.HARNESS_RELATIVE_PATH
        source.write_bytes(source.read_bytes() + b"# dirty\n")
        with self.assertRaisesRegex(compat.CompatibilityError, "completely clean"):
            compat.materialize_historical_harness(
                self.output("dirty"), dirty, revision
            )

        aliased = self.clone(revision, "product-aliased")
        alias = self.root / "product-root-alias"
        alias.symlink_to(aliased, target_is_directory=True)
        with self.assertRaisesRegex(compat.CompatibilityError, "symlink aliases"):
            compat.materialize_historical_harness(
                self.output("aliased"), alias, revision
            )

        hardlinked = self.clone(revision, "product-hardlinked")
        outside_link = self.root / "source-hardlink"
        os.link(hardlinked / compat.HARNESS_RELATIVE_PATH, outside_link)
        with self.assertRaisesRegex(compat.CompatibilityError, "singly linked"):
            compat.materialize_historical_harness(
                self.output("hardlinked"), hardlinked, revision
            )

        unknown = self.clone(revision, "product-unknown")
        unknown_source = unknown / compat.HARNESS_RELATIVE_PATH
        unknown_source.write_text("#!/usr/bin/env bash\necho unknown\n", encoding="utf-8")
        run(("/usr/bin/git", "add", compat.HARNESS_RELATIVE_PATH), cwd=unknown)
        run(
            (
                "/usr/bin/git",
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.invalid",
                "commit",
                "--quiet",
                "-m",
                "unknown harness",
            ),
            cwd=unknown,
        )
        unknown_revision = run(("/usr/bin/git", "rev-parse", "HEAD"), cwd=unknown)
        with self.assertRaisesRegex(compat.CompatibilityError, "unknown historical base"):
            compat.materialize_historical_harness(
                self.output("unknown"), unknown, unknown_revision
            )

    def test_wrong_context_and_already_patched_bytes_are_rejected(self) -> None:
        product = self.clone(REVISIONS[2], "product-context")
        hunks = compat._derive_hunks(product)
        template, _, _ = compat._git_harness(
            product, compat.TIMED_TRIAL_SOURCE_REVISION
        )
        variant = compat._VARIANTS[compat.PHASE78_REFERENCE_SHA256]
        with self.assertRaisesRegex(compat.CompatibilityError, "already patched"):
            compat._apply_transformations(template, hunks, variant)

        base, _, _ = compat._git_harness(product, REVISIONS[2])
        damaged = base.replace(b"write_chart_curve()", b"write_chart_curve_broken()", 1)
        with self.assertRaisesRegex(compat.CompatibilityError, "wrong context"):
            compat._apply_transformations(damaged, hunks, variant)

    def test_output_metadata_and_external_anchor_tampering_fail_closed(self) -> None:
        product, harness, metadata, result = self.create(name="tamper-harness")
        original_metadata_hash = cast(str, result["metadata_sha256"])
        os.chmod(harness, 0o755)
        harness.write_bytes(harness.read_bytes() + b"# tampered\n")
        os.chmod(harness, 0o555)
        with self.assertRaisesRegex(compat.CompatibilityError, "Git-derived bytes"):
            compat.audit_materialized_harness(
                harness,
                metadata,
                sha256(harness),
                original_metadata_hash,
                product,
                REVISIONS[2],
            )

        product2, harness2, metadata2, result2 = self.create(name="tamper-metadata")
        document = cast(dict[str, object], json.loads(metadata2.read_text()))
        document["unexpected"] = True
        os.chmod(metadata2, 0o644)
        metadata2.write_bytes(compat._canonical_json_bytes(document))
        os.chmod(metadata2, 0o444)
        with self.assertRaisesRegex(compat.CompatibilityError, "re-derived contract"):
            compat.audit_materialized_harness(
                harness2,
                metadata2,
                cast(str, result2["harness_sha256"]),
                sha256(metadata2),
                product2,
                REVISIONS[2],
            )

        product3, harness3, metadata3, result3 = self.create(name="bad-anchor")
        with self.assertRaisesRegex(compat.CompatibilityError, "external SHA-256 anchor"):
            compat.audit_materialized_harness(
                harness3,
                metadata3,
                "0" * 64,
                cast(str, result3["metadata_sha256"]),
                product3,
                REVISIONS[2],
            )

    def test_modes_hardlinks_symlinks_and_wrong_metadata_path_fail_closed(self) -> None:
        product, harness, metadata, result = self.create(name="inode")
        hardlink = self.root / "materialized-hardlink"
        os.link(harness, hardlink)
        with self.assertRaisesRegex(compat.CompatibilityError, "singly linked"):
            compat.audit_materialized_harness(
                harness,
                metadata,
                cast(str, result["harness_sha256"]),
                cast(str, result["metadata_sha256"]),
                product,
                REVISIONS[2],
            )

        product2, harness2, metadata2, result2 = self.create(name="mode")
        os.chmod(metadata2, 0o644)
        with self.assertRaisesRegex(compat.CompatibilityError, "mode must be 0444"):
            compat.audit_materialized_harness(
                harness2,
                metadata2,
                cast(str, result2["harness_sha256"]),
                cast(str, result2["metadata_sha256"]),
                product2,
                REVISIONS[2],
            )

        product3, harness3, metadata3, result3 = self.create(name="wrong-metadata")
        other = self.root / "other.metadata.json"
        other.write_bytes(metadata3.read_bytes())
        os.chmod(other, 0o444)
        with self.assertRaisesRegex(compat.CompatibilityError, "metadata path"):
            compat.audit_materialized_harness(
                harness3,
                other,
                cast(str, result3["harness_sha256"]),
                sha256(other),
                product3,
                REVISIONS[2],
            )

    def test_no_replace_special_files_and_interrupted_pair_fail_closed(self) -> None:
        revision = REVISIONS[2]
        product = self.clone(revision, "product-no-replace")

        occupied = self.output("occupied")
        occupied.write_bytes(b"foreign harness\n")
        with self.assertRaisesRegex(compat.CompatibilityError, "already occupied"):
            compat.materialize_historical_harness(occupied, product, revision)
        self.assertEqual(occupied.read_bytes(), b"foreign harness\n")

        fifo = self.output("fifo")
        os.mkfifo(fifo)
        with self.assertRaisesRegex(compat.CompatibilityError, "already occupied"):
            compat.materialize_historical_harness(fifo, product, revision)
        self.assertTrue(stat.S_ISFIFO(fifo.lstat().st_mode))

        symlink = self.output("symlink")
        symlink.symlink_to("missing-target")
        with self.assertRaisesRegex(compat.CompatibilityError, "already occupied"):
            compat.materialize_historical_harness(symlink, product, revision)
        self.assertTrue(symlink.is_symlink())

        metadata_first = self.output("metadata-first")
        metadata_path = metadata_first.with_name(metadata_first.name + ".metadata.json")
        metadata_path.write_bytes(b"foreign metadata\n")
        with self.assertRaisesRegex(compat.CompatibilityError, "already occupied"):
            compat.materialize_historical_harness(metadata_first, product, revision)
        self.assertFalse(os.path.lexists(metadata_first))
        self.assertEqual(metadata_path.read_bytes(), b"foreign metadata\n")

        raced = self.output("metadata-race")
        raced_metadata = raced.with_name(raced.name + ".metadata.json")
        original_publish = compat._publish_no_replace
        call_count = 0

        def racing_publish(path: Path, payload: bytes, mode: int) -> tuple[int, int]:
            nonlocal call_count
            call_count += 1
            if call_count == 2:
                path.write_bytes(b"racing foreign metadata\n")
                os.chmod(path, 0o444)
            return original_publish(path, payload, mode)

        with mock.patch.object(compat, "_publish_no_replace", side_effect=racing_publish):
            with self.assertRaisesRegex(compat.CompatibilityError, "refusing to replace"):
                compat.materialize_historical_harness(raced, product, revision)
        self.assertFalse(os.path.lexists(raced))
        self.assertEqual(raced_metadata.read_bytes(), b"racing foreign metadata\n")
        self.assertFalse(any(raced.parent.glob(f".{raced.name}.tmp.*")))

        product2, harness2, metadata2, result2 = self.create(name="rerun")
        original_harness = harness2.read_bytes()
        original_metadata = metadata2.read_bytes()
        with self.assertRaisesRegex(compat.CompatibilityError, "already occupied"):
            compat.materialize_historical_harness(harness2, product2, REVISIONS[2])
        self.assertEqual(harness2.read_bytes(), original_harness)
        self.assertEqual(metadata2.read_bytes(), original_metadata)
        self.assertEqual(result2["harness_sha256"], sha256(harness2))


if __name__ == "__main__":
    unittest.main()
