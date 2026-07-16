#!/usr/bin/env python3
"""Focused adversarial tests for the generic evidence-run ledger."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "wric_evidence_run_ledger.py"
sys.path.insert(0, os.fspath(TOOL.parent))
import wric_evidence_run_ledger as ledger  # noqa: E402


def sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


class EvidenceRunLedgerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.top = Path(self.temporary.name)
        self.counter = 0

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def capture(self, *, two_files: bool = False) -> Path:
        self.counter += 1
        root = self.top / f"capture-{self.counter}"
        root.mkdir()
        (root / "alpha.txt").write_bytes(b"alpha\n")
        (root / "alpha.txt").chmod(0o640)
        if two_files:
            nested = root / "nested"
            nested.mkdir()
            (nested / "z.bin").write_bytes(b"\x00z\xff")
            (nested / "z.bin").chmod(0o755)
        return root

    @staticmethod
    def seal(root: Path) -> ledger.Result:
        return ledger.seal_capture(root)

    @staticmethod
    def rewrite_control(path: Path, payload: bytes) -> None:
        path.chmod(0o644)
        path.write_bytes(payload)
        path.chmod(0o444)

    def rewrite_ledger(self, root: Path, payload: bytes) -> str:
        self.rewrite_control(root / ledger.LEDGER_NAME, payload)
        self.rewrite_control(root / ledger.SEAL_NAME, ledger.detached_seal(payload))
        return sha256(payload)

    @staticmethod
    def cli(*arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, os.fspath(TOOL), *arguments],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={
                "LC_ALL": "C",
                "PATH": "/usr/bin:/bin",
                "PYTHONHASHSEED": "0",
            },
        )

    def test_exact_v1_format_detached_seal_and_cli_output(self) -> None:
        root = self.capture(two_files=True)
        completed = self.cli("seal", "--capture-dir", os.fspath(root))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")

        alpha = b"alpha\n"
        nested = b"\x00z\xff"
        expected_ledger = (
            f"# schema={ledger.SCHEMA}\n"
            f"# schema_version={ledger.SCHEMA_VERSION}\n"
            "sha256\tbytes\tmode\tpath\n"
            f"{sha256(alpha)}\t{len(alpha)}\t0640\talpha.txt\n"
            f"{sha256(nested)}\t{len(nested)}\t0755\tnested/z.bin\n"
        ).encode("ascii")
        observed = (root / ledger.LEDGER_NAME).read_bytes()
        self.assertEqual(observed, expected_ledger)
        expected_sha = sha256(expected_ledger)
        self.assertEqual(
            (root / ledger.SEAL_NAME).read_bytes(),
            f"{expected_sha}  {ledger.LEDGER_NAME}\n".encode("ascii"),
        )
        for name in (ledger.LEDGER_NAME, ledger.SEAL_NAME):
            info = (root / name).stat()
            self.assertTrue(stat.S_ISREG(info.st_mode))
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o444)
            self.assertEqual(info.st_nlink, 1)
        self.assertFalse(
            any(path.name.startswith(ledger.STAGING_PREFIX) for path in root.iterdir())
        )
        self.assertEqual(
            completed.stdout,
            json.dumps(
                {
                    "ledger": ledger.LEDGER_NAME,
                    "ledger_sha256": expected_sha,
                    "member_count": 2,
                    "status": "sealed",
                },
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
        )

        audited = self.cli(
            "audit",
            "--capture-dir",
            os.fspath(root),
            "--expected-ledger-sha256",
            expected_sha,
        )
        self.assertEqual(audited.returncode, 0, audited.stderr)
        self.assertEqual(audited.stderr, "")
        self.assertEqual(
            json.loads(audited.stdout),
            {
                "ledger": ledger.LEDGER_NAME,
                "ledger_sha256": expected_sha,
                "member_count": 2,
                "status": "audited",
            },
        )

    def test_external_anchor_is_required_and_exact(self) -> None:
        root = self.capture()
        result = self.seal(root)
        ledger.audit_capture(root, result.ledger_sha256)
        for anchor, message in (
            ("A" * 64, "expected ledger SHA-256 is not canonical"),
            ("0" * 63, "expected ledger SHA-256 is not canonical"),
            ("0" * 64, "differs from the external SHA-256 anchor"),
        ):
            with self.subTest(anchor=anchor):
                with self.assertRaisesRegex(ledger.LedgerError, message):
                    ledger.audit_capture(root, anchor)

    def test_seal_is_no_replace_and_cli_failure_is_exact(self) -> None:
        root = self.capture()
        result = self.seal(root)
        before_ledger = (root / ledger.LEDGER_NAME).read_bytes()
        before_seal = (root / ledger.SEAL_NAME).read_bytes()
        completed = self.cli("seal", "--capture-dir", os.fspath(root))
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(completed.stdout, "")
        self.assertEqual(
            completed.stderr,
            "error: publication destination already exists: "
            f"{ledger.LEDGER_NAME}\n",
        )
        self.assertEqual((root / ledger.LEDGER_NAME).read_bytes(), before_ledger)
        self.assertEqual((root / ledger.SEAL_NAME).read_bytes(), before_seal)
        ledger.audit_capture(root, result.ledger_sha256)

    def test_preexisting_symlink_destination_is_never_replaced(self) -> None:
        root = self.capture()
        target = self.top / "rival"
        target.write_bytes(b"rival")
        destination = root / ledger.LEDGER_NAME
        destination.symlink_to(target)
        with self.assertRaisesRegex(
            ledger.LedgerError,
            f"publication destination already exists: {ledger.LEDGER_NAME}",
        ):
            self.seal(root)
        self.assertTrue(destination.is_symlink())
        self.assertEqual(target.read_bytes(), b"rival")
        self.assertFalse((root / ledger.SEAL_NAME).exists())

    def test_second_link_race_rolls_back_only_owned_entry(self) -> None:
        root = self.capture()
        real_link = ledger.link_no_replace
        rival = b"rival seal\n"

        def racing_link(root_descriptor: int, source: str, destination: str) -> None:
            if destination == ledger.SEAL_NAME:
                descriptor = os.open(
                    destination,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                    0o444,
                    dir_fd=root_descriptor,
                )
                try:
                    os.write(descriptor, rival)
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
            real_link(root_descriptor, source, destination)

        with mock.patch.object(ledger, "link_no_replace", side_effect=racing_link):
            with self.assertRaisesRegex(
                ledger.LedgerError,
                f"publication destination already exists: {ledger.SEAL_NAME}",
            ):
                self.seal(root)
        self.assertFalse((root / ledger.LEDGER_NAME).exists())
        self.assertEqual((root / ledger.SEAL_NAME).read_bytes(), rival)
        self.assertFalse(
            any(path.name.startswith(ledger.STAGING_PREFIX) for path in root.iterdir())
        )

        (root / ledger.SEAL_NAME).unlink()
        result = self.seal(root)
        ledger.audit_capture(root, result.ledger_sha256)

    def test_staging_residue_is_rejected_not_silently_excluded(self) -> None:
        root = self.capture()
        residue = root / f"{ledger.STAGING_PREFIX}crash"
        residue.write_bytes(b"partial")
        with self.assertRaisesRegex(
            ledger.LedgerError, "unsafe or reserved path"
        ):
            self.seal(root)

        residue.unlink()
        result = self.seal(root)
        residue.write_bytes(b"post-seal partial")
        with self.assertRaisesRegex(
            ledger.LedgerError, "unsafe or reserved path"
        ):
            ledger.audit_capture(root, result.ledger_sha256)

    def test_member_content_mode_deletion_and_extra_mutations_fail(self) -> None:
        cases = ("content", "mode", "deletion", "extra-file", "extra-directory")
        for mutation in cases:
            with self.subTest(mutation=mutation):
                root = self.capture()
                result = self.seal(root)
                member = root / "alpha.txt"
                if mutation == "content":
                    member.write_bytes(b"omega\n")
                    member.chmod(0o640)
                elif mutation == "mode":
                    member.chmod(0o600)
                elif mutation == "deletion":
                    member.unlink()
                elif mutation == "extra-file":
                    (root / "extra.txt").write_bytes(b"extra")
                else:
                    (root / "empty-extra").mkdir()
                with self.assertRaises(ledger.LedgerError):
                    ledger.audit_capture(root, result.ledger_sha256)

    def test_member_mutation_after_first_audit_scan_is_detected(self) -> None:
        root = self.capture()
        result = self.seal(root)
        member = root / "alpha.txt"
        real_scan = ledger.scan_capture_descriptor
        scan_count = 0

        def mutate_after_first_scan(
            root_descriptor: int,
            exclusions: dict[str, tuple[int, int]],
            *,
            durable: bool,
        ) -> ledger.ScanResult:
            nonlocal scan_count
            observed = real_scan(
                root_descriptor,
                exclusions,
                durable=durable,
            )
            scan_count += 1
            if scan_count == 1:
                member.write_bytes(b"omega\n")
                member.chmod(0o640)
            return observed

        with mock.patch.object(
            ledger,
            "scan_capture_descriptor",
            side_effect=mutate_after_first_scan,
        ):
            with self.assertRaisesRegex(
                ledger.LedgerError, "sealed capture member changed: alpha.txt"
            ):
                ledger.audit_capture(root, result.ledger_sha256)
        self.assertEqual(scan_count, 2)

    def test_symlink_special_and_hardlink_inputs_are_rejected(self) -> None:
        variants = ("file-symlink", "directory-symlink", "fifo", "hardlink")
        for variant in variants:
            with self.subTest(variant=variant):
                root = self.capture()
                if variant == "file-symlink":
                    (root / "link").symlink_to(root / "alpha.txt")
                elif variant == "directory-symlink":
                    real = self.top / f"real-{self.counter}"
                    real.mkdir()
                    (root / "linked-dir").symlink_to(real, target_is_directory=True)
                elif variant == "fifo":
                    os.mkfifo(root / "fifo")
                else:
                    os.link(root / "alpha.txt", root / "alias.txt")
                with self.assertRaises(ledger.LedgerError):
                    self.seal(root)

    def test_post_seal_member_and_control_hardlinks_are_rejected(self) -> None:
        root = self.capture()
        result = self.seal(root)
        outside_member = self.top / "outside-member"
        os.link(root / "alpha.txt", outside_member)
        with self.assertRaisesRegex(ledger.LedgerError, "hard-linked or aliased"):
            ledger.audit_capture(root, result.ledger_sha256)
        outside_member.unlink()

        outside_ledger = self.top / "outside-ledger"
        os.link(root / ledger.LEDGER_NAME, outside_ledger)
        with self.assertRaisesRegex(ledger.LedgerError, "single-link regular file"):
            ledger.audit_capture(root, result.ledger_sha256)

    def test_control_symlink_or_mode_mutation_is_rejected(self) -> None:
        root = self.capture()
        result = self.seal(root)
        seal = root / ledger.SEAL_NAME
        seal.chmod(0o644)
        with self.assertRaisesRegex(ledger.LedgerError, "mode-0444"):
            ledger.audit_capture(root, result.ledger_sha256)

        seal.chmod(0o444)
        ledger_path = root / ledger.LEDGER_NAME
        ledger_path.unlink()
        ledger_path.symlink_to(self.top / "elsewhere")
        with self.assertRaisesRegex(ledger.LedgerError, "single-link regular file"):
            ledger.audit_capture(root, result.ledger_sha256)

    def test_malformed_ledgers_are_rejected_even_with_matching_new_anchor(self) -> None:
        variants = (
            "preamble",
            "crlf",
            "non-ascii",
            "no-final-newline",
            "digest",
            "size",
            "mode",
            "escape",
            "self",
            "duplicate",
            "unsorted",
            "extra-field",
            "empty",
        )
        for variant in variants:
            with self.subTest(variant=variant):
                root = self.capture(two_files=True)
                self.seal(root)
                valid = (root / ledger.LEDGER_NAME).read_bytes()
                lines = valid.decode("ascii").splitlines()
                rows = lines[3:]
                if variant == "preamble":
                    lines[0] = "# schema=wrong"
                elif variant == "crlf":
                    payload = valid.replace(b"\n", b"\r\n")
                    anchor = self.rewrite_ledger(root, payload)
                    with self.assertRaisesRegex(ledger.LedgerError, "canonical newline"):
                        ledger.audit_capture(root, anchor)
                    continue
                elif variant == "non-ascii":
                    payload = valid[:-1] + b"\xff\n"
                    anchor = self.rewrite_ledger(root, payload)
                    with self.assertRaisesRegex(ledger.LedgerError, "not ASCII"):
                        ledger.audit_capture(root, anchor)
                    continue
                elif variant == "no-final-newline":
                    payload = valid.rstrip(b"\n")
                    anchor = self.rewrite_ledger(root, payload)
                    with self.assertRaisesRegex(ledger.LedgerError, "canonical newline"):
                        ledger.audit_capture(root, anchor)
                    continue
                elif variant == "digest":
                    fields = rows[0].split("\t")
                    fields[0] = "g" * 64
                    lines[3] = "\t".join(fields)
                elif variant == "size":
                    fields = rows[0].split("\t")
                    fields[1] = "06"
                    lines[3] = "\t".join(fields)
                elif variant == "mode":
                    fields = rows[0].split("\t")
                    fields[2] = "8888"
                    lines[3] = "\t".join(fields)
                elif variant == "escape":
                    fields = rows[0].split("\t")
                    fields[3] = "../escape"
                    lines[3] = "\t".join(fields)
                elif variant == "self":
                    fields = rows[0].split("\t")
                    fields[3] = ledger.LEDGER_NAME
                    lines[3] = "\t".join(fields)
                elif variant == "duplicate":
                    lines.append(rows[0])
                elif variant == "unsorted":
                    lines[3:] = reversed(rows)
                elif variant == "extra-field":
                    lines[3] += "\textra"
                elif variant == "empty":
                    lines = lines[:3]
                payload = ("\n".join(lines) + "\n").encode("ascii")
                anchor = self.rewrite_ledger(root, payload)
                with self.assertRaises(ledger.LedgerError):
                    ledger.audit_capture(root, anchor)

    def test_detached_seal_tamper_is_rejected(self) -> None:
        root = self.capture()
        result = self.seal(root)
        self.rewrite_control(root / ledger.SEAL_NAME, b"0" * 64 + b"  wrong\n")
        with self.assertRaisesRegex(ledger.LedgerError, "detached seal is not exact"):
            ledger.audit_capture(root, result.ledger_sha256)

    def test_unsafe_names_empty_captures_and_empty_directories_fail_closed(self) -> None:
        empty = self.top / "empty"
        empty.mkdir()
        with self.assertRaisesRegex(ledger.LedgerError, "no evidence files"):
            self.seal(empty)

        unsafe = self.capture()
        (unsafe / "bad name").write_bytes(b"bad")
        with self.assertRaisesRegex(ledger.LedgerError, "unsafe or reserved path"):
            self.seal(unsafe)

        empty_directory = self.capture()
        (empty_directory / "empty").mkdir()
        with self.assertRaisesRegex(ledger.LedgerError, "unbound directory closure"):
            self.seal(empty_directory)

    def test_capture_root_symlink_is_rejected(self) -> None:
        root = self.capture()
        alias = self.top / "capture-alias"
        alias.symlink_to(root, target_is_directory=True)
        with self.assertRaisesRegex(ledger.LedgerError, "non-symlink directory"):
            self.seal(alias)

    def test_seal_fsyncs_regular_files_and_directories(self) -> None:
        root = self.capture(two_files=True)
        real_fsync = os.fsync
        synced_types: list[int] = []

        def tracking_fsync(descriptor: int) -> None:
            synced_types.append(stat.S_IFMT(os.fstat(descriptor).st_mode))
            real_fsync(descriptor)

        with mock.patch(
            "wric_evidence_run_ledger.os.fsync", side_effect=tracking_fsync
        ):
            self.seal(root)
        self.assertIn(stat.S_IFREG, synced_types)
        self.assertIn(stat.S_IFDIR, synced_types)
        self.assertGreaterEqual(synced_types.count(stat.S_IFREG), 4)
        self.assertGreaterEqual(synced_types.count(stat.S_IFDIR), 4)


if __name__ == "__main__":
    unittest.main()
