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


Q_PRODUCT_REVISION = "a9db72e60f153a95362db544107373817a58a258"
P_PRODUCT_REVISION = "4b5f0efb4b8355916375f4639fff0e0a3abdc03c"
REVISIONS = (
    "208ce23f0c005d3702d114f535fe21564b3b79b6",
    "870c298ff1c0c21901bdf79d341bf97d121f389c",
    "38e9a281396e5263647ba68724414848841525d7",
    Q_PRODUCT_REVISION,
    P_PRODUCT_REVISION,
)
EXPECTED_VARIANTS = (
    "phase1-5",
    "phase6",
    "phase7-8",
    "timed-trial-current",
    "timed-trial-current",
)
APPROVED_REVISIONS = {
    "208ce23f0c005d3702d114f535fe21564b3b79b6": "phase1-5",
    "0c4623ba1793395ae8f5c3df2a2524a27d89bc80": "phase1-5",
    "7d294d68eaadc8c55b92be4f5589278c8a2f78f2": "phase1-5",
    "cbf92b62284b2a93e506f59187ac94a5336b0be3": "phase1-5",
    "3a10e9cc45050f7a6f846f9f1adb8d5f4157f9a5": "phase1-5",
    "870c298ff1c0c21901bdf79d341bf97d121f389c": "phase6",
    "38e9a281396e5263647ba68724414848841525d7": "phase7-8",
    "6c8d0c7651c2aa2e5c396d0f57c2e4e18c322310": "phase7-8",
    Q_PRODUCT_REVISION: "timed-trial-current",
    P_PRODUCT_REVISION: "timed-trial-current",
}


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

    def test_all_approved_variants_are_exact_and_auditable(self) -> None:
        self.assertEqual(compat._APPROVED_PRODUCT_REVISIONS, APPROVED_REVISIONS)
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
                expected_proof_length = (
                    1 if expected_variant == "timed-trial-current" else 17
                )
                self.assertEqual(len(proof), expected_proof_length)
                if expected_variant == "timed-trial-current":
                    self.assertEqual(
                        [item["name"] for item in proof],
                        ["summary-row-id-01"],
                    )
                else:
                    self.assertEqual(
                        [item["name"] for item in proof[:2]],
                        ["score-domain-01", "score-domain-02"],
                    )
                self.assertEqual(proof[-1]["name"], "summary-row-id-01")
                correction = cast(
                    dict[str, object], spec["summary_aggregation_correction"]
                )
                self.assertEqual(
                    correction["old_key"],
                    ["fixture", "method", "requested_workers"],
                )
                self.assertEqual(correction["new_key"], ["row_id"])
                self.assertEqual(
                    correction["old_sha256"], compat.SUMMARY_ROW_ID_OLD_SHA256
                )
                self.assertEqual(
                    correction["new_sha256"], compat.SUMMARY_ROW_ID_NEW_SHA256
                )
                self.assertTrue(transformation["child_runner_logic_unchanged"])
                self.assertTrue(transformation["timing_boundary_unchanged"])
                self.assertTrue(
                    transformation[
                        "summary_aggregation_logic_unchanged_except_group_key"
                    ]
                )

    def test_score_domains_digest_flow_and_summary_correction_match_allowlist(
        self,
    ) -> None:
        product, harness, _, _ = self.create(name="semantics")
        frozen = subprocess.check_output(
            (
                "/usr/bin/git",
                "-C",
                os.fspath(product),
                "show",
                f"{compat.TIMED_TRIAL_SOURCE_REVISION}:{compat.HARNESS_RELATIVE_PATH}",
            )
        )
        self.assertEqual(frozen.count(compat._SUMMARY_ROW_ID_OLD), 1)
        self.assertEqual(frozen.count(compat._SUMMARY_ROW_ID_NEW), 0)
        expected = frozen.replace(
            compat._SUMMARY_ROW_ID_OLD, compat._SUMMARY_ROW_ID_NEW, 1
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
        self.assertIn("g=$h[\"row_id\"]", text)
        self.assertNotIn(
            'g=$h["fixture"] SUBSEP $h["method"] SUBSEP $h["requested_workers"]',
            text,
        )

    def test_current_timed_trial_product_applies_only_summary_correction(
        self,
    ) -> None:
        product, harness, metadata, result = self.create(
            P_PRODUCT_REVISION, "current-summary-only"
        )
        source, blob, mode = compat._git_harness(product, P_PRODUCT_REVISION)
        self.assertEqual(
            hashlib.sha256(source).hexdigest(),
            compat.TIMED_TRIAL_SOURCE_SHA256,
        )
        self.assertEqual(blob, "fa04eeb645b47b7db21fa313bf278110bfebeeb6")
        self.assertEqual(mode, "100755")
        self.assertEqual(source.count(compat._SUMMARY_ROW_ID_OLD), 1)
        expected = source.replace(
            compat._SUMMARY_ROW_ID_OLD, compat._SUMMARY_ROW_ID_NEW, 1
        )
        self.assertEqual(harness.read_bytes(), expected)
        self.assertEqual(
            result["harness_sha256"],
            compat.PHASE78_SUMMARY_ROW_ID_RESULT_SHA256,
        )
        document = cast(dict[str, object], json.loads(metadata.read_text()))
        transformation = cast(dict[str, object], document["transformation"])
        proof = cast(list[dict[str, object]], transformation["proof"])
        spec = cast(dict[str, object], transformation["spec"])
        hunks = cast(list[dict[str, object]], spec["hunks"])
        self.assertEqual([item["name"] for item in proof], ["summary-row-id-01"])
        self.assertEqual([item["name"] for item in hunks], ["summary-row-id-01"])

    def test_summary_aggregation_keeps_multi_policy_row_digests_separate(
        self,
    ) -> None:
        _, harness, _, _ = self.create(name="summary-behavior")
        text = harness.read_text(encoding="utf-8")
        marker = "# Aggregate measured trials by manifest row ID."
        start = text.index("awk -F '\\t' -v OFS='\\t' '\n", text.index(marker))
        program_start = start + len("awk -F '\\t' -v OFS='\\t' '\n")
        program_end = text.index(
            "\n' \"$raw_trials_tsv\" >\"$summary_tsv\"", program_start
        )
        program = text[program_start:program_end]

        columns = (
            "row_id",
            "fixture",
            "method",
            "requested_workers",
            "wall_clock_s",
            "user_cpu_s",
            "system_cpu_s",
            "max_rss_kb",
            "peak_sampled_rss_kb",
            "status",
            "validation_status",
            "canonical_digest",
        )
        lines = ["\t".join(columns)]
        policies = (
            ("phase7-lazy-off", "a" * 64),
            ("phase7-lazy-on", "b" * 64),
            ("phase7-lazy-auto", "c" * 64),
        )
        for row_id, digest in policies:
            for trial, wall in enumerate((1.0, 3.0), start=1):
                lines.append(
                    "\t".join(
                        (
                            row_id,
                            "same-fixture",
                            "chart_spr_grammar_lower_bound_heuristic",
                            "default",
                            str(wall),
                            str(float(trial)),
                            "0.1",
                            str(100 + trial),
                            str(200 + trial),
                            "ok",
                            "ok",
                            digest,
                        )
                    )
                )
        aggregated = subprocess.run(
            ("/usr/bin/awk", "-F", "\t", "-v", "OFS=\t", program),
            input="\n".join(lines) + "\n",
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout.splitlines()
        self.assertEqual(len(aggregated), 4)
        header = aggregated[0].split("\t")
        index = {name: position for position, name in enumerate(header)}
        rows = {
            fields[index["row_id"]]: fields
            for fields in (line.split("\t") for line in aggregated[1:])
        }
        self.assertEqual(set(rows), {row_id for row_id, _ in policies})
        for row_id, digest in policies:
            self.assertEqual(rows[row_id][index["canonical_digest"]], digest)
            self.assertEqual(rows[row_id][index["trial_count"]], "2")
            self.assertEqual(rows[row_id][index["wall_clock_s"]], "2.000000")

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
        with self.assertRaisesRegex(compat.CompatibilityError, "approved capture checkpoint"):
            compat.materialize_historical_harness(
                self.output("unknown"), unknown, unknown_revision
            )

        same_bytes = self.clone(REVISIONS[0], "product-unlisted-same-bytes")
        original_digest = sha256(same_bytes / compat.HARNESS_RELATIVE_PATH)
        run(
            (
                "/usr/bin/git",
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.invalid",
                "commit",
                "--quiet",
                "--allow-empty",
                "-m",
                "same harness, unlisted commit",
            ),
            cwd=same_bytes,
        )
        self.assertEqual(
            sha256(same_bytes / compat.HARNESS_RELATIVE_PATH), original_digest
        )
        unlisted_revision = run(("/usr/bin/git", "rev-parse", "HEAD"), cwd=same_bytes)
        with self.assertRaisesRegex(compat.CompatibilityError, "approved capture checkpoint"):
            compat.materialize_historical_harness(
                self.output("unlisted-same-bytes"), same_bytes, unlisted_revision
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

        summary_hunk = hunks[-1]
        self.assertEqual(summary_hunk.name, "summary-row-id-01")
        corrected, proof = compat._apply_transformations(
            template, (summary_hunk,), variant
        )
        self.assertEqual(
            hashlib.sha256(corrected).hexdigest(),
            compat.PHASE78_SUMMARY_ROW_ID_RESULT_SHA256,
        )
        self.assertEqual(proof[0]["old_occurrences_before"], 1)
        self.assertEqual(proof[0]["new_occurrences_after"], 1)
        with self.assertRaisesRegex(compat.CompatibilityError, "already patched"):
            compat._apply_transformations(corrected, (summary_hunk,), variant)

        wrong_summary_context = template.replace(
            b'g=$h["fixture"] SUBSEP $h["method"] SUBSEP $h["requested_workers"]',
            b'g=$h["fixture"] SUBSEP $h["method"]',
            1,
        )
        with self.assertRaisesRegex(compat.CompatibilityError, "wrong context"):
            compat._apply_transformations(
                wrong_summary_context, (summary_hunk,), variant
            )

        duplicated_summary_context = template + summary_hunk.old
        with self.assertRaisesRegex(
            compat.CompatibilityError, "old=2, new=0"
        ):
            compat._apply_transformations(
                duplicated_summary_context, (summary_hunk,), variant
            )

        with mock.patch.object(
            compat,
            "_SUMMARY_ROW_ID_NEW",
            compat._SUMMARY_ROW_ID_NEW + b"# unapproved\n",
        ):
            with self.assertRaisesRegex(
                compat.CompatibilityError, "fixed summary row-ID correction changed"
            ):
                compat._derive_hunks(product)

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

    def test_final_self_audit_failure_rolls_back_only_owned_inodes(self) -> None:
        revision = REVISIONS[2]
        product = self.clone(revision, "product-self-audit-rollback")
        harness = self.output("self-audit-rollback")
        metadata = harness.with_name(harness.name + ".metadata.json")
        original_unlink = compat._unlink_if_owned
        unlink_order: list[Path] = []

        def recording_unlink(path: Path, identity: tuple[int, int]) -> None:
            unlink_order.append(path)
            original_unlink(path, identity)

        with mock.patch.object(
            compat,
            "audit_materialized_harness",
            side_effect=compat.CompatibilityError("forced final self-audit failure"),
        ), mock.patch.object(
            compat, "_unlink_if_owned", side_effect=recording_unlink
        ):
            with self.assertRaisesRegex(compat.CompatibilityError, "forced final"):
                compat.materialize_historical_harness(harness, product, revision)
        self.assertEqual(unlink_order, [metadata, harness])
        self.assertFalse(os.path.lexists(metadata))
        self.assertFalse(os.path.lexists(harness))

        product2 = self.clone(revision, "product-self-audit-race")
        raced_harness = self.output("self-audit-race")
        raced_metadata = raced_harness.with_name(
            raced_harness.name + ".metadata.json"
        )

        def racing_audit(
            audited_harness: Path,
            audited_metadata: Path,
            expected_harness_sha256: str,
            expected_metadata_sha256: str,
            product_repo_root: Path,
            expected_product_revision: str,
        ) -> Mapping[str, object]:
            del (
                audited_harness,
                expected_harness_sha256,
                expected_metadata_sha256,
                product_repo_root,
                expected_product_revision,
            )
            audited_metadata.unlink()
            audited_metadata.write_bytes(b"racing foreign metadata\n")
            os.chmod(audited_metadata, 0o444)
            raise compat.CompatibilityError("forced raced final self-audit failure")

        with mock.patch.object(
            compat, "audit_materialized_harness", side_effect=racing_audit
        ):
            with self.assertRaisesRegex(compat.CompatibilityError, "forced raced"):
                compat.materialize_historical_harness(
                    raced_harness, product2, revision
                )
        self.assertFalse(os.path.lexists(raced_harness))
        self.assertEqual(raced_metadata.read_bytes(), b"racing foreign metadata\n")


if __name__ == "__main__":
    unittest.main()
