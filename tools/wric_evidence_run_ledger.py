#!/usr/bin/env python3
"""Seal and audit an exact, immutable evidence-capture directory closure.

This is deliberately a small generic primitive.  It does not know about any
WRIC phase, manifest, row schema, repository, or executable.  ``seal`` binds
every regular file and descendant directory below one completed capture
directory and publishes a ledger plus detached seal without replacing an
existing directory entry.  Directory records make intentional empty output
namespaces part of the closure instead of treating them as mutable omissions.
``audit`` requires an external ledger SHA-256 anchor and verifies the exact
current typed path closure.

The publication protocol is fail-closed.  Staging files are fsynced and then
hard-linked to their final names (POSIX no-replace publication), the directory
is fsynced, and the staging links are removed.  A crash may leave an explicit
staging or partial-publication residue; a later seal/audit rejects that state
instead of silently completing or replacing it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import stat
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Mapping, NoReturn, Sequence


SCHEMA = "wric.evidence_run_ledger"
SCHEMA_VERSION = 2
LEDGER_NAME = "wric-evidence-run-ledger.tsv"
SEAL_NAME = LEDGER_NAME + ".sha256"
STAGING_PREFIX = ".wric-evidence-run-ledger.stage-"
HEADER = ("kind", "sha256", "bytes", "mode", "path")
PREAMBLE = (
    f"# schema={SCHEMA}",
    f"# schema_version={SCHEMA_VERSION}",
    "\t".join(HEADER),
)

SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
UINT_RE = re.compile(r"(?:0|[1-9][0-9]*)\Z")
MODE_RE = re.compile(r"[0-7]{4}\Z")
SAFE_COMPONENT_RE = re.compile(r"[A-Za-z0-9_.-]+\Z")
CONTROL_NAMES = frozenset((LEDGER_NAME, SEAL_NAME))
CHUNK_SIZE = 1024 * 1024
MAX_LEDGER_BYTES = 64 * 1024 * 1024

DIRECTORY_FLAGS = (
    os.O_RDONLY
    | getattr(os, "O_DIRECTORY", 0)
    | getattr(os, "O_NOFOLLOW", 0)
    | getattr(os, "O_CLOEXEC", 0)
)
FILE_FLAGS = (
    os.O_RDONLY
    | getattr(os, "O_NOFOLLOW", 0)
    | getattr(os, "O_CLOEXEC", 0)
)


class LedgerError(RuntimeError):
    """A stable, user-facing contract failure."""


@dataclass(frozen=True, slots=True)
class FileRecord:
    relative: str
    sha256: str
    size: int
    mode: int
    device: int
    inode: int


@dataclass(frozen=True, slots=True)
class DirectoryRecord:
    relative: str
    mode: int
    device: int
    inode: int


@dataclass(frozen=True, slots=True)
class LedgerEntry:
    kind: str
    sha256: str | None
    size: int | None
    mode: int
    relative: str


@dataclass(frozen=True, slots=True)
class ScanResult:
    files: Mapping[str, FileRecord]
    directories: Mapping[str, DirectoryRecord]


@dataclass(frozen=True, slots=True)
class Stage:
    name: str
    device: int
    inode: int
    sha256: str
    size: int


@dataclass(frozen=True, slots=True)
class Result:
    status: str
    ledger_sha256: str
    member_count: int


def fail(message: str) -> NoReturn:
    raise LedgerError(message)


def permission_mode(info: os.stat_result) -> int:
    return stat.S_IMODE(info.st_mode)


def identity(info: os.stat_result) -> tuple[int, int]:
    return (info.st_dev, info.st_ino)


def stable_file_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def stable_directory_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def canonical_capture_directory(path: Path) -> Path:
    absolute = Path(os.path.abspath(os.fspath(path)))
    try:
        info = absolute.lstat()
    except OSError as error:
        fail(f"cannot inspect capture directory: {error.strerror or error}")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        fail("capture directory is not an exact non-symlink directory")
    try:
        resolved = absolute.resolve(strict=True)
    except OSError as error:
        fail(f"cannot resolve capture directory: {error.strerror or error}")
    if resolved != absolute:
        fail("capture directory path contains a symlink or lexical alias")
    return absolute


def open_capture_directory(root: Path) -> tuple[int, tuple[int, int]]:
    try:
        descriptor = os.open(root, DIRECTORY_FLAGS)
    except OSError as error:
        fail(f"cannot open capture directory: {error.strerror or error}")
    info = os.fstat(descriptor)
    if not stat.S_ISDIR(info.st_mode):
        os.close(descriptor)
        fail("opened capture directory is not a directory")
    return descriptor, identity(info)


def require_root_identity(root: Path, expected: tuple[int, int]) -> None:
    try:
        info = root.lstat()
    except OSError as error:
        fail(f"capture directory changed during operation: {error.strerror or error}")
    if (
        stat.S_ISLNK(info.st_mode)
        or not stat.S_ISDIR(info.st_mode)
        or identity(info) != expected
    ):
        fail("capture directory identity changed during operation")


def fsync_directory_descriptor(descriptor: int, label: str) -> None:
    try:
        os.fsync(descriptor)
    except OSError as error:
        fail(f"cannot fsync {label}: {error.strerror or error}")


def fsync_directory_path(path: Path, label: str) -> None:
    descriptor: int | None = None
    try:
        descriptor = os.open(path, DIRECTORY_FLAGS)
        os.fsync(descriptor)
    except OSError as error:
        fail(f"cannot fsync {label}: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)


def validate_component(name: str, relative: str) -> None:
    if (
        SAFE_COMPONENT_RE.fullmatch(name) is None
        or name in (".", "..")
        or name in CONTROL_NAMES
        or name.startswith(STAGING_PREFIX)
    ):
        fail(f"capture contains an unsafe or reserved path: {relative}")


def validate_ledger_relative(relative: str) -> None:
    pure = PurePosixPath(relative)
    if (
        not relative
        or pure.is_absolute()
        or pure.as_posix() != relative
        or not pure.parts
    ):
        fail(f"ledger contains an unsafe path: {relative!r}")
    for component in pure.parts:
        if (
            SAFE_COMPONENT_RE.fullmatch(component) is None
            or component in (".", "..")
            or component in CONTROL_NAMES
            or component.startswith(STAGING_PREFIX)
        ):
            fail(f"ledger contains an unsafe path: {relative!r}")


def hash_open_regular(
    parent_descriptor: int,
    name: str,
    lexical_info: os.stat_result,
    relative: str,
    durable: bool,
) -> FileRecord:
    descriptor: int | None = None
    try:
        descriptor = os.open(name, FILE_FLAGS, dir_fd=parent_descriptor)
        before = os.fstat(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_nlink != 1
            or identity(before) != identity(lexical_info)
            or stable_file_signature(before) != stable_file_signature(lexical_info)
        ):
            fail(f"capture member changed while opening: {relative}")
        if durable:
            try:
                os.fsync(descriptor)
            except OSError as error:
                fail(
                    f"cannot fsync capture member {relative}: "
                    f"{error.strerror or error}"
                )
        digest = hashlib.sha256()
        byte_count = 0
        while True:
            block = os.read(descriptor, CHUNK_SIZE)
            if not block:
                break
            digest.update(block)
            byte_count += len(block)
        after = os.fstat(descriptor)
        if stable_file_signature(after) != stable_file_signature(before):
            fail(f"capture member changed while hashing: {relative}")
        if byte_count != after.st_size:
            fail(f"capture member size changed while hashing: {relative}")
        return FileRecord(
            relative=relative,
            sha256=digest.hexdigest(),
            size=byte_count,
            mode=permission_mode(after),
            device=after.st_dev,
            inode=after.st_ino,
        )
    except LedgerError:
        raise
    except OSError as error:
        fail(f"cannot read capture member {relative}: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)


def scan_capture_descriptor(
    root_descriptor: int,
    exclusions: Mapping[str, tuple[int, int]],
    *,
    durable: bool,
) -> ScanResult:
    files: dict[str, FileRecord] = {}
    directories: dict[str, DirectoryRecord] = {}
    inodes: set[tuple[int, int]] = set()

    def visit(directory_descriptor: int, prefix: str) -> None:
        directory_before = os.fstat(directory_descriptor)
        if not stat.S_ISDIR(directory_before.st_mode):
            fail(f"capture path ceased to be a directory: {prefix or '.'}")
        try:
            names = sorted(os.listdir(directory_descriptor))
        except OSError as error:
            fail(
                f"cannot list capture directory {prefix or '.'}: "
                f"{error.strerror or error}"
            )
        for name in names:
            relative = name if not prefix else f"{prefix}/{name}"
            try:
                lexical_info = os.stat(
                    name, dir_fd=directory_descriptor, follow_symlinks=False
                )
            except OSError as error:
                fail(f"cannot inspect capture path {relative}: {error.strerror or error}")

            if not prefix and name in exclusions:
                if (
                    not stat.S_ISREG(lexical_info.st_mode)
                    or lexical_info.st_nlink != 1
                    or identity(lexical_info) != exclusions[name]
                ):
                    fail(f"publication control file changed or is unsafe: {name}")
                continue

            validate_component(name, relative)
            if stat.S_ISDIR(lexical_info.st_mode):
                child_descriptor: int | None = None
                try:
                    child_descriptor = os.open(
                        name, DIRECTORY_FLAGS, dir_fd=directory_descriptor
                    )
                    child_info = os.fstat(child_descriptor)
                    if (
                        not stat.S_ISDIR(child_info.st_mode)
                        or identity(child_info) != identity(lexical_info)
                    ):
                        fail(f"capture directory changed while opening: {relative}")
                    visit(child_descriptor, relative)
                    child_after = os.fstat(child_descriptor)
                    if stable_directory_signature(child_after) != stable_directory_signature(
                        child_info
                    ):
                        fail(f"capture directory changed while scanning: {relative}")
                    directories[relative] = DirectoryRecord(
                        relative=relative,
                        mode=permission_mode(child_after),
                        device=child_after.st_dev,
                        inode=child_after.st_ino,
                    )
                except LedgerError:
                    raise
                except OSError as error:
                    fail(
                        f"cannot open capture directory {relative}: "
                        f"{error.strerror or error}"
                    )
                finally:
                    if child_descriptor is not None:
                        os.close(child_descriptor)
                continue

            if stat.S_ISLNK(lexical_info.st_mode) or not stat.S_ISREG(
                lexical_info.st_mode
            ):
                fail(f"capture contains a symlink or special file: {relative}")
            member_identity = identity(lexical_info)
            if lexical_info.st_nlink != 1 or member_identity in inodes:
                fail(f"capture contains a hard-linked or aliased file: {relative}")
            inodes.add(member_identity)
            record = hash_open_regular(
                directory_descriptor,
                name,
                lexical_info,
                relative,
                durable,
            )
            files[relative] = record

        if durable:
            fsync_directory_descriptor(
                directory_descriptor, f"capture directory {prefix or '.'}"
            )
        directory_after = os.fstat(directory_descriptor)
        if stable_directory_signature(directory_after) != stable_directory_signature(
            directory_before
        ):
            fail(f"capture directory changed while scanning: {prefix or '.'}")

    visit(root_descriptor, "")
    if not files:
        fail("capture directory contains no evidence files")
    return ScanResult(files=files, directories=directories)


def render_ledger(scan: ScanResult) -> bytes:
    lines = list(PREAMBLE)
    paths = sorted((*scan.files, *scan.directories))
    for relative in paths:
        if relative in scan.files:
            record = scan.files[relative]
            fields = (
                "file",
                record.sha256,
                str(record.size),
                f"{record.mode:04o}",
                relative,
            )
        else:
            directory = scan.directories[relative]
            fields = (
                "directory",
                "-",
                "-",
                f"{directory.mode:04o}",
                relative,
            )
        lines.append("\t".join(fields))
    return ("\n".join(lines) + "\n").encode("ascii")


def detached_seal(ledger_payload: bytes) -> bytes:
    digest = hashlib.sha256(ledger_payload).hexdigest()
    return f"{digest}  {LEDGER_NAME}\n".encode("ascii")


def parse_ledger(payload: bytes) -> Mapping[str, LedgerEntry]:
    if len(payload) > MAX_LEDGER_BYTES:
        fail("ledger exceeds the maximum supported size")
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        fail("ledger is not canonical newline-delimited ASCII")
    try:
        text = payload.decode("ascii")
    except UnicodeDecodeError:
        fail("ledger is not ASCII")
    lines = text[:-1].split("\n")
    if tuple(lines[: len(PREAMBLE)]) != PREAMBLE:
        fail("ledger preamble or header is not the exact v2 schema")
    entries: dict[str, LedgerEntry] = {}
    paths: list[str] = []
    for line_number, line in enumerate(lines[len(PREAMBLE) :], len(PREAMBLE) + 1):
        fields = line.split("\t")
        if len(fields) != 5:
            fail(f"ledger line {line_number} is malformed")
        kind, digest, size_text, mode_text, relative = fields
        if kind == "file":
            if SHA256_RE.fullmatch(digest) is None:
                fail(f"ledger line {line_number} has a noncanonical SHA-256")
            if UINT_RE.fullmatch(size_text) is None:
                fail(f"ledger line {line_number} has a noncanonical byte count")
            parsed_digest: str | None = digest
            parsed_size: int | None = int(size_text)
        elif kind == "directory":
            if digest != "-" or size_text != "-":
                fail(
                    f"ledger line {line_number} has noncanonical directory fields"
                )
            parsed_digest = None
            parsed_size = None
        else:
            fail(f"ledger line {line_number} has an unknown entry kind")
        if MODE_RE.fullmatch(mode_text) is None:
            fail(f"ledger line {line_number} has a noncanonical entry mode")
        validate_ledger_relative(relative)
        if relative in entries:
            fail(f"ledger duplicates path: {relative}")
        entry = LedgerEntry(
            kind=kind,
            sha256=parsed_digest,
            size=parsed_size,
            mode=int(mode_text, 8),
            relative=relative,
        )
        entries[relative] = entry
        paths.append(relative)
    if not entries:
        fail("ledger contains no evidence members")
    if paths != sorted(paths):
        fail("ledger member paths are not in canonical ASCII order")
    if not any(entry.kind == "file" for entry in entries.values()):
        fail("ledger contains no evidence files")
    for relative in paths:
        parts = PurePosixPath(relative).parts
        for length in range(1, len(parts)):
            parent = "/".join(parts[:length])
            parent_entry = entries.get(parent)
            if parent_entry is None or parent_entry.kind != "directory":
                fail(
                    "ledger directory closure omits typed parent: "
                    f"{parent} for {relative}"
                )
    return entries


def read_control_file(
    root_descriptor: int,
    name: str,
    label: str,
    *,
    maximum_size: int,
) -> tuple[bytes, tuple[int, int]]:
    descriptor: int | None = None
    try:
        lexical_info = os.stat(name, dir_fd=root_descriptor, follow_symlinks=False)
        if (
            stat.S_ISLNK(lexical_info.st_mode)
            or not stat.S_ISREG(lexical_info.st_mode)
            or lexical_info.st_nlink != 1
            or permission_mode(lexical_info) != 0o444
        ):
            fail(f"{label} is not an exact mode-0444, single-link regular file")
        descriptor = os.open(name, FILE_FLAGS, dir_fd=root_descriptor)
        before = os.fstat(descriptor)
        if stable_file_signature(before) != stable_file_signature(lexical_info):
            fail(f"{label} changed while opening")
        if before.st_size > maximum_size:
            fail(f"{label} exceeds the maximum supported size")
        chunks: list[bytes] = []
        byte_count = 0
        while True:
            block = os.read(descriptor, CHUNK_SIZE)
            if not block:
                break
            byte_count += len(block)
            if byte_count > maximum_size:
                fail(f"{label} exceeds the maximum supported size")
            chunks.append(block)
        after = os.fstat(descriptor)
        if stable_file_signature(after) != stable_file_signature(before):
            fail(f"{label} changed while reading")
        return b"".join(chunks), identity(after)
    except FileNotFoundError:
        fail(f"{label} is missing")
    except LedgerError:
        raise
    except OSError as error:
        fail(f"cannot read {label}: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)


def ensure_publication_names_absent(root_descriptor: int) -> None:
    for name in (LEDGER_NAME, SEAL_NAME):
        try:
            os.stat(name, dir_fd=root_descriptor, follow_symlinks=False)
        except FileNotFoundError:
            continue
        except OSError as error:
            fail(f"cannot inspect publication destination {name}: {error.strerror or error}")
        fail(f"publication destination already exists: {name}")


def write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            fail("short write while creating publication staging file")
        offset += written


def create_stage(root_descriptor: int, payload: bytes) -> Stage:
    descriptor: int | None = None
    name = ""
    created_identity: tuple[int, int] | None = None
    completed = False
    try:
        for _ in range(128):
            name = (
                f"{STAGING_PREFIX}{os.getpid()}-{secrets.token_hex(12)}"
            )
            try:
                descriptor = os.open(
                    name,
                    os.O_WRONLY
                    | os.O_CREAT
                    | os.O_EXCL
                    | getattr(os, "O_NOFOLLOW", 0)
                    | getattr(os, "O_CLOEXEC", 0),
                    0o600,
                    dir_fd=root_descriptor,
                )
                created_identity = identity(os.fstat(descriptor))
                break
            except FileExistsError:
                continue
        if descriptor is None:
            fail("cannot allocate a unique publication staging file")
        assert descriptor is not None
        write_all(descriptor, payload)
        os.fchmod(descriptor, 0o444)
        os.fsync(descriptor)
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_nlink != 1
            or info.st_size != len(payload)
            or permission_mode(info) != 0o444
        ):
            fail("publication staging file did not retain its exact attributes")
        stage = Stage(
            name=name,
            device=info.st_dev,
            inode=info.st_ino,
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
        )
        os.close(descriptor)
        descriptor = None
        fsync_directory_descriptor(root_descriptor, "publication staging directory")
        completed = True
        return stage
    except LedgerError:
        raise
    except OSError as error:
        fail(f"cannot create publication staging file: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if name and created_identity is not None and not completed:
            try:
                info = os.stat(name, dir_fd=root_descriptor, follow_symlinks=False)
                if stat.S_ISREG(info.st_mode) and identity(info) == created_identity:
                    os.unlink(name, dir_fd=root_descriptor)
                    os.fsync(root_descriptor)
            except OSError:
                # A residue is deliberately fail-closed by the scanner.  Never
                # remove a name whose identity no longer proves ownership.
                pass


def stage_identity(stage: Stage) -> tuple[int, int]:
    return (stage.device, stage.inode)


def validate_stage(root_descriptor: int, stage: Stage) -> None:
    payload, observed_identity = read_control_file(
        root_descriptor,
        stage.name,
        "publication staging file",
        maximum_size=max(stage.size, 1),
    )
    if (
        observed_identity != stage_identity(stage)
        or len(payload) != stage.size
        or hashlib.sha256(payload).hexdigest() != stage.sha256
    ):
        fail("publication staging file changed before publication")


def link_no_replace(root_descriptor: int, source: str, destination: str) -> None:
    """Test seam for the single POSIX no-replace publication operation."""

    try:
        os.link(
            source,
            destination,
            src_dir_fd=root_descriptor,
            dst_dir_fd=root_descriptor,
            follow_symlinks=False,
        )
    except FileExistsError:
        fail(f"publication destination already exists: {destination}")
    except OSError as error:
        fail(
            f"cannot publish {destination} without replacement: "
            f"{error.strerror or error}"
        )


def unlink_owned(
    root_descriptor: int,
    name: str,
    expected_identity: tuple[int, int],
    *,
    missing_ok: bool,
) -> None:
    try:
        info = os.stat(name, dir_fd=root_descriptor, follow_symlinks=False)
    except FileNotFoundError:
        if missing_ok:
            return
        fail(f"owned publication entry disappeared: {name}")
    except OSError as error:
        fail(f"cannot inspect owned publication entry {name}: {error.strerror or error}")
    if not stat.S_ISREG(info.st_mode) or identity(info) != expected_identity:
        fail(f"refusing to remove a replaced publication entry: {name}")
    try:
        os.unlink(name, dir_fd=root_descriptor)
    except OSError as error:
        fail(f"cannot remove owned publication entry {name}: {error.strerror or error}")


def scans_equal(left: ScanResult, right: ScanResult) -> bool:
    return (
        dict(left.directories) == dict(right.directories)
        and dict(left.files) == dict(right.files)
    )


def audit_entries(entries: Mapping[str, LedgerEntry], scan: ScanResult) -> None:
    listed = set(entries)
    actual = set(scan.files) | set(scan.directories)
    if listed != actual:
        missing = sorted(listed - actual)
        extra = sorted(actual - listed)
        fail(f"ledger is not the exact capture closure: missing={missing}, extra={extra}")
    for relative in sorted(entries):
        entry = entries[relative]
        if entry.kind == "file":
            record = scan.files.get(relative)
            changed = record is None or (
                entry.sha256 != record.sha256
                or entry.size != record.size
                or entry.mode != record.mode
            )
        else:
            directory = scan.directories.get(relative)
            changed = directory is None or entry.mode != directory.mode
        if changed:
            fail(f"sealed capture member changed: {relative}")


def safe_cleanup(
    root_descriptor: int,
    stages: Sequence[Stage],
    published: Sequence[tuple[str, Stage]],
) -> None:
    for name, stage in reversed(published):
        try:
            unlink_owned(
                root_descriptor,
                name,
                stage_identity(stage),
                missing_ok=True,
            )
        except LedgerError:
            pass
    for stage in reversed(stages):
        try:
            unlink_owned(
                root_descriptor,
                stage.name,
                stage_identity(stage),
                missing_ok=True,
            )
        except LedgerError:
            pass
    try:
        os.fsync(root_descriptor)
    except OSError:
        pass


def seal_capture(capture_directory: Path) -> Result:
    root = canonical_capture_directory(capture_directory)
    root_descriptor, root_identity = open_capture_directory(root)
    stages: list[Stage] = []
    published: list[tuple[str, Stage]] = []
    succeeded = False
    try:
        require_root_identity(root, root_identity)
        ensure_publication_names_absent(root_descriptor)
        initial = scan_capture_descriptor(root_descriptor, {}, durable=True)
        fsync_directory_path(root.parent, "capture parent directory")
        ledger_payload = render_ledger(initial)
        seal_payload = detached_seal(ledger_payload)
        ledger_stage = create_stage(root_descriptor, ledger_payload)
        stages.append(ledger_stage)
        seal_stage = create_stage(root_descriptor, seal_payload)
        stages.append(seal_stage)

        exclusions = {stage.name: stage_identity(stage) for stage in stages}
        before_publication = scan_capture_descriptor(
            root_descriptor, exclusions, durable=False
        )
        if not scans_equal(initial, before_publication):
            fail("capture closure changed before ledger publication")
        ensure_publication_names_absent(root_descriptor)
        validate_stage(root_descriptor, ledger_stage)
        validate_stage(root_descriptor, seal_stage)

        link_no_replace(root_descriptor, ledger_stage.name, LEDGER_NAME)
        published.append((LEDGER_NAME, ledger_stage))
        link_no_replace(root_descriptor, seal_stage.name, SEAL_NAME)
        published.append((SEAL_NAME, seal_stage))
        fsync_directory_descriptor(root_descriptor, "published ledger directory")

        for stage in stages:
            unlink_owned(
                root_descriptor,
                stage.name,
                stage_identity(stage),
                missing_ok=False,
            )
        fsync_directory_descriptor(root_descriptor, "staging cleanup directory")

        observed_ledger, ledger_identity = read_control_file(
            root_descriptor,
            LEDGER_NAME,
            "evidence-run ledger",
            maximum_size=MAX_LEDGER_BYTES,
        )
        observed_seal, seal_identity = read_control_file(
            root_descriptor,
            SEAL_NAME,
            "evidence-run ledger detached seal",
            maximum_size=256,
        )
        if ledger_identity != stage_identity(ledger_stage) or observed_ledger != ledger_payload:
            fail("published evidence-run ledger changed")
        if seal_identity != stage_identity(seal_stage) or observed_seal != seal_payload:
            fail("published evidence-run ledger detached seal changed")
        controls = {LEDGER_NAME: ledger_identity, SEAL_NAME: seal_identity}
        after_publication = scan_capture_descriptor(
            root_descriptor, controls, durable=False
        )
        if not scans_equal(initial, after_publication):
            fail("capture closure changed during ledger publication")
        require_root_identity(root, root_identity)
        fsync_directory_descriptor(root_descriptor, "sealed capture directory")
        fsync_directory_path(root.parent, "sealed capture parent directory")
        succeeded = True
        return Result(
            status="sealed",
            ledger_sha256=hashlib.sha256(ledger_payload).hexdigest(),
            member_count=len(initial.files),
        )
    finally:
        if not succeeded:
            safe_cleanup(root_descriptor, stages, published)
        os.close(root_descriptor)


def audit_capture(capture_directory: Path, expected_ledger_sha256: str) -> Result:
    if SHA256_RE.fullmatch(expected_ledger_sha256) is None:
        fail("expected ledger SHA-256 is not canonical lowercase hexadecimal")
    root = canonical_capture_directory(capture_directory)
    root_descriptor, root_identity = open_capture_directory(root)
    try:
        require_root_identity(root, root_identity)
        ledger_payload, ledger_identity = read_control_file(
            root_descriptor,
            LEDGER_NAME,
            "evidence-run ledger",
            maximum_size=MAX_LEDGER_BYTES,
        )
        observed_sha256 = hashlib.sha256(ledger_payload).hexdigest()
        if observed_sha256 != expected_ledger_sha256:
            fail("evidence-run ledger differs from the external SHA-256 anchor")
        seal_payload, seal_identity = read_control_file(
            root_descriptor,
            SEAL_NAME,
            "evidence-run ledger detached seal",
            maximum_size=256,
        )
        if seal_payload != detached_seal(ledger_payload):
            fail("evidence-run ledger detached seal is not exact")
        entries = parse_ledger(ledger_payload)
        scan = scan_capture_descriptor(
            root_descriptor,
            {LEDGER_NAME: ledger_identity, SEAL_NAME: seal_identity},
            durable=False,
        )
        audit_entries(entries, scan)

        ledger_again, ledger_identity_again = read_control_file(
            root_descriptor,
            LEDGER_NAME,
            "evidence-run ledger",
            maximum_size=MAX_LEDGER_BYTES,
        )
        seal_again, seal_identity_again = read_control_file(
            root_descriptor,
            SEAL_NAME,
            "evidence-run ledger detached seal",
            maximum_size=256,
        )
        if (
            ledger_again != ledger_payload
            or ledger_identity_again != ledger_identity
            or seal_again != seal_payload
            or seal_identity_again != seal_identity
        ):
            fail("ledger control files changed during audit")
        final_scan = scan_capture_descriptor(
            root_descriptor,
            {LEDGER_NAME: ledger_identity, SEAL_NAME: seal_identity},
            durable=False,
        )
        audit_entries(entries, final_scan)
        if not scans_equal(scan, final_scan):
            fail("capture closure identity changed during audit")
        require_root_identity(root, root_identity)
        return Result(
            status="audited",
            ledger_sha256=observed_sha256,
            member_count=len(scan.files),
        )
    finally:
        os.close(root_descriptor)


def result_payload(result: Result) -> str:
    return json.dumps(
        {
            "ledger": LEDGER_NAME,
            "ledger_sha256": result.ledger_sha256,
            "member_count": result.member_count,
            "status": result.status,
        },
        sort_keys=True,
        separators=(",", ":"),
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Seal or audit one exact evidence capture directory"
    )
    subparsers = result.add_subparsers(dest="command", required=True)
    seal = subparsers.add_parser("seal", help="publish a no-replace evidence ledger")
    seal.add_argument("--capture-dir", type=Path, required=True)
    audit = subparsers.add_parser("audit", help="audit an externally anchored ledger")
    audit.add_argument("--capture-dir", type=Path, required=True)
    audit.add_argument("--expected-ledger-sha256", required=True)
    return result


def run(arguments: argparse.Namespace) -> Result:
    if arguments.command == "seal":
        return seal_capture(arguments.capture_dir)
    if arguments.command == "audit":
        return audit_capture(
            arguments.capture_dir,
            arguments.expected_ledger_sha256,
        )
    fail(f"unsupported command: {arguments.command}")


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        outcome = run(arguments)
    except LedgerError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(
            f"error: operating-system failure: {error.strerror or error}",
            file=sys.stderr,
        )
        return 2
    print(result_payload(outcome))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
