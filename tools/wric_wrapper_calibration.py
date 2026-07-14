#!/usr/bin/env python3
"""Produce the canonical process-wrapper overhead calibration artifact.

The controller is deliberately narrow: one fixed CPU-bound workload, one
physical-core cpuset, one process wrapper, and a closed JSON evidence schema.
It creates ``wrapper-calibration.json`` only after every arm and both <=2%
median gates pass.  Failed or interfered runs never leave an apparent result.
"""

from __future__ import annotations

import argparse
import ctypes
from decimal import Decimal, ROUND_HALF_EVEN
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import stat
import subprocess
import tempfile
import threading
import time
from typing import Mapping, Sequence


SCHEMA = "wric_process_wrapper_calibration"
SCHEMA_VERSION = 3
CONTROLLER_SCHEMA = "wric_wrapper_calibration_controller"
CONTROLLER_VERSION = 3
WORKLOAD_SCHEMA = "wric_wrapper_calibration_cpu_workload"
WORKLOAD_VERSION = 1
WORKLOAD_VERSION_TEXT = "wric-wrapper-calibration-workload-v1"
WORKLOAD_SOURCE_SHA256 = (
    "66244128374664bdc6868df078410608fbc5f9a0dd47c71bdc7f7534b81da8d2"
)
WORKLOAD_STDOUT_TEXT = (
    "schema=wric-wrapper-calibration-workload-v1\n"
    "threads=8\n"
    "iterations_per_thread=2250000000\n"
    "affinity_cpu_count=8\n"
    "checksum=8745913267985538626\n"
)
WORKLOAD_STDOUT_BYTES = 137
WORKLOAD_STDOUT_SHA256 = (
    "6c20ef62a67ad417071deb005e0a49bc4e7e806a18dd9578a1bc8c4ac32480b6"
)
FROZEN_REVISION = "408434ecfd096af484ecbbd3deeb67511151cd76"
AFFINITY = (0, 2, 4, 6, 8, 10, 12, 14)
AFFINITY_TEXT = "0,2,4,6,8,10,12,14"
UNSELECTED_SMT = (1, 3, 5, 7, 9, 11, 13, 15)
TIMEOUT_SECONDS = 600
RSS_LIMIT_BYTES = 16 * 1024**3
QUIET_THRESHOLD_PPM = 200_000
PREFLIGHT_SECONDS = 5
LIVE_GUARD_POLL_MS = 250
LIVE_GUARD_MAX_GAP_NS = 1_000_000_000
RATIO_THRESHOLD = Decimal("1.02")
SATURATION_MIN = Decimal("7.2")
SATURATION_MAX = Decimal("8.8")
MIN_ARM_WALL_NS = 5_000_000_000
MIN_ARM_CPU_US = 36_000_000
INNER_OUTER_WALL_EARLY_TOLERANCE_NS = 5_000_000
INNER_OUTER_WALL_MAX_DELTA_NS = 250_000_000
INNER_OUTER_CPU_EARLY_TOLERANCE_US = 10_000
INNER_OUTER_CPU_MAX_DELTA_US = 500_000
INNER_OUTER_RSS_MAX_DELTA_KB = 64 * 1024
RATIO_PLACES = Decimal("0.000000000001")
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
ENVIRONMENT = {"LC_ALL": "C", "PATH": "/usr/bin:/bin", "TZ": "Europe/Sofia"}
SYMBOLIC_ARGV = ["@workload"]
SYMBOLIC_REDIRECTIONS = {"stderr": "@stderr", "stdout": "@stdout"}
PR_SET_CHILD_SUBREAPER = 36
PR_GET_CHILD_SUBREAPER = 37

INPUT_ROLES = ("controller", "runner", "workload", "workload_source")
SNAPSHOT_MODES = {
    "controller": 0o555,
    "runner": 0o555,
    "workload": 0o555,
    "workload_source": 0o444,
}
SEALED_LAUNCH_SCHEMA = "wric_wrapper_calibration_sealed_launch"
SEALED_LAUNCH_VERSION = 1

PROCESS_METRICS_V2_FIELDS = {
    "schema_version", "outcome", "exit_code", "term_signal", "timed_out",
    "runner_exit_code", "wall_seconds", "user_seconds", "system_seconds",
    "max_rss_kb", "peak_sampled_rss_kb", "peak_sampled_swap_kb",
    "rss_kb_unit", "proc_status_samples", "proc_rss_samples",
    "proc_swap_samples", "proc_group_samples", "peak_sampled_process_count",
    "subreaper_enabled", "descendants_reaped", "post_leader_descendants",
    "descendant_cleanup_kill_sent", "live_descendants_at_return",
    "process_group_alive_at_return", "wait4_echild_at_return",
    "monitor_error", "monitor_error_count", "wait4_collected", "wait_errno",
    "child_error_stage", "child_error_errno", "core_dumped",
    "timeout_term_sent", "timeout_kill_sent", "rss_limit_bytes",
    "rss_limit_enabled", "rss_limit_observed", "rss_limit_exceeded",
    "rss_limit_trigger_bytes", "rss_limit_term_sent", "rss_limit_kill_sent",
}


class CalibrationError(RuntimeError):
    pass


def fail(message: str) -> "NoReturn":
    raise CalibrationError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_identity(info: os.stat_result, digest: str) -> dict[str, object]:
    return {
        "ctime_ns": info.st_ctime_ns,
        "device": info.st_dev,
        "inode": info.st_ino,
        "mode": stat.S_IMODE(info.st_mode),
        "mtime_ns": info.st_mtime_ns,
        "sha256": digest,
        "size_bytes": info.st_size,
    }


def directory_identity(info: os.stat_result) -> dict[str, int]:
    return {
        "ctime_ns": info.st_ctime_ns,
        "device": info.st_dev,
        "inode": info.st_ino,
        "mode": stat.S_IMODE(info.st_mode),
        "mtime_ns": info.st_mtime_ns,
    }


def directory_inode_identity(info: os.stat_result) -> dict[str, int]:
    return {
        "device": info.st_dev,
        "inode": info.st_ino,
        "mode": stat.S_IMODE(info.st_mode),
    }


def read_stable_fd(descriptor: int, label: str) -> tuple[bytes, dict[str, object]]:
    before = os.fstat(descriptor)
    if not stat.S_ISREG(before.st_mode):
        fail(f"{label} descriptor is not a regular file")
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks: list[bytes] = []
    while True:
        chunk = os.read(descriptor, 1024 * 1024)
        if not chunk:
            break
        chunks.append(chunk)
    os.lseek(descriptor, 0, os.SEEK_SET)
    after = os.fstat(descriptor)
    stable_fields = (
        "st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns", "st_ctime_ns"
    )
    if any(getattr(before, key) != getattr(after, key) for key in stable_fields):
        fail(f"{label} descriptor identity changed while it was read")
    data = b"".join(chunks)
    if len(data) != after.st_size:
        fail(f"{label} descriptor read did not consume its exact size")
    return data, file_identity(after, sha256_bytes(data))


def require_fd_path_identity(
    descriptor: int,
    path: Path,
    expected: Mapping[str, object],
    label: str,
) -> None:
    try:
        fd_info = os.fstat(descriptor)
        path_info = path.stat(follow_symlinks=False)
    except OSError as error:
        fail(f"cannot stat {label} descriptor/path binding: {error}")
    fields = {
        "device": "st_dev",
        "inode": "st_ino",
        "mode": None,
        "size_bytes": "st_size",
        "mtime_ns": "st_mtime_ns",
        "ctime_ns": "st_ctime_ns",
    }
    for key, attribute in fields.items():
        fd_value = stat.S_IMODE(fd_info.st_mode) if attribute is None else getattr(fd_info, attribute)
        path_value = stat.S_IMODE(path_info.st_mode) if attribute is None else getattr(path_info, attribute)
        if fd_value != int(expected[key]) or path_value != int(expected[key]):
            fail(f"{label} descriptor/path identity changed")
    if not stat.S_ISREG(fd_info.st_mode) or not stat.S_ISREG(path_info.st_mode):
        fail(f"{label} descriptor/path is not a regular file")


def read_directory_binding(
    path: Path, descriptor: int, label: str
) -> dict[str, int]:
    try:
        fd_info = os.fstat(descriptor)
        path_info = path.stat(follow_symlinks=False)
    except OSError as error:
        fail(f"cannot stat {label} directory binding: {error}")
    if not stat.S_ISDIR(fd_info.st_mode) or not stat.S_ISDIR(path_info.st_mode):
        fail(f"{label} binding is not a directory")
    fd_identity = directory_identity(fd_info)
    path_identity = directory_identity(path_info)
    if fd_identity != path_identity:
        fail(f"{label} directory FD no longer names its lexical path")
    return fd_identity


def require_directory_binding(
    path: Path,
    descriptor: int,
    expected: Mapping[str, object],
    label: str,
) -> None:
    if read_directory_binding(path, descriptor, label) != expected:
        fail(f"{label} directory identity/mode changed")


def read_directory_inode_binding(
    path: Path, descriptor: int, label: str
) -> dict[str, int]:
    try:
        fd_info = os.fstat(descriptor)
        path_info = path.stat(follow_symlinks=False)
    except OSError as error:
        fail(f"cannot stat {label} directory binding: {error}")
    if not stat.S_ISDIR(fd_info.st_mode) or not stat.S_ISDIR(path_info.st_mode):
        fail(f"{label} binding is not a directory")
    fd_identity = directory_inode_identity(fd_info)
    path_identity = directory_inode_identity(path_info)
    if fd_identity != path_identity:
        fail(f"{label} directory FD no longer names its lexical path")
    return fd_identity


def require_directory_inode_binding(
    path: Path,
    descriptor: int,
    expected: Mapping[str, object],
    label: str,
) -> None:
    if read_directory_inode_binding(path, descriptor, label) != expected:
        fail(f"{label} directory inode/mode changed")


def read_stable_input(path: Path, label: str) -> tuple[bytes, dict[str, object]]:
    """Read one lexical regular file through an identity-stable descriptor."""

    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        fail(f"cannot open {label} without following links: {error}")
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            fail(f"{label} descriptor is not a regular file")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        data = b"".join(chunks)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    try:
        path_after = path.stat(follow_symlinks=False)
    except OSError as error:
        fail(f"cannot restat {label} after its stable read: {error}")
    stable_fields = (
        "st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns", "st_ctime_ns"
    )
    if any(getattr(before, key) != getattr(after, key) for key in stable_fields) or any(
        getattr(after, key) != getattr(path_after, key) for key in stable_fields
    ):
        fail(f"{label} identity changed while it was read")
    if len(data) != after.st_size:
        fail(f"{label} stable read did not consume its exact size")
    return data, file_identity(after, sha256_bytes(data))


def require_stat_identity(
    path: Path, expected: Mapping[str, object], label: str
) -> None:
    """Cheaply prove that a private snapshot path still names the same inode."""

    try:
        observed = path.stat(follow_symlinks=False)
    except OSError as error:
        fail(f"cannot stat {label}: {error}")
    if not stat.S_ISREG(observed.st_mode) or any(
        (
            observed.st_dev != int(expected["device"]),
            observed.st_ino != int(expected["inode"]),
            stat.S_IMODE(observed.st_mode) != int(expected["mode"]),
            observed.st_size != int(expected["size_bytes"]),
            observed.st_mtime_ns != int(expected["mtime_ns"]),
            observed.st_ctime_ns != int(expected["ctime_ns"]),
        )
    ):
        fail(f"{label} private snapshot identity changed between arms")


def write_private_snapshot(
    path: Path, data: bytes, mode: int, label: str
) -> dict[str, object]:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        offset = 0
        while offset < len(data):
            written = os.write(descriptor, data[offset:])
            if written <= 0:
                fail(f"short write while creating {label}")
            offset += written
        os.fchmod(descriptor, mode)
        os.fsync(descriptor)
    except BaseException:
        os.close(descriptor)
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        raise
    else:
        os.close(descriptor)
    observed_data, identity = read_stable_input(path, label)
    if observed_data != data or identity["mode"] != mode:
        fail(f"{label} private snapshot differs from its sealed input bytes")
    return identity


def open_bound_regular_file(
    path: Path,
    expected: Mapping[str, object],
    label: str,
    *,
    inheritable: bool,
) -> int:
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        require_fd_path_identity(descriptor, path, expected, label)
        os.set_inheritable(descriptor, inheritable)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def open_bound_directory(path: Path, label: str, *, inheritable: bool) -> int:
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        identity = read_directory_binding(path, descriptor, label)
        if identity["mode"] != 0o700:
            fail(f"{label} is not exact mode 0700")
        os.set_inheritable(descriptor, inheritable)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def open_inode_bound_directory(
    path: Path, label: str, *, inheritable: bool
) -> tuple[int, dict[str, int]]:
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        identity = read_directory_inode_binding(path, descriptor, label)
        os.set_inheritable(descriptor, inheritable)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor, identity


def verify_input_closure(
    paths: Mapping[str, Path],
    starting_data: Mapping[str, bytes],
    starting_identities: Mapping[str, Mapping[str, object]],
    snapshot_paths: Mapping[str, Path],
    snapshot_identities: Mapping[str, Mapping[str, object]],
    execution_fds: Mapping[str, int],
    execution_identities: Mapping[str, Mapping[str, object]],
    directory_bindings: Mapping[
        str, tuple[Path, int, Mapping[str, object]]
    ],
    launch_state_path: Path,
    launch_state_fd: int,
    launch_state_data: bytes,
    launch_state_identity: Mapping[str, object],
    output_parent_path: Path,
    output_parent_fd: int,
    output_parent_identity: Mapping[str, object],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for role in INPUT_ROLES:
        ending_data, ending_identity = read_stable_input(
            paths[role], f"live calibration {role}"
        )
        if (
            ending_data != starting_data[role]
            or ending_identity != starting_identities[role]
        ):
            fail(f"live calibration {role} changed after its starting snapshot")
        snapshot_data, snapshot_ending = read_stable_input(
            snapshot_paths[role], f"private calibration {role} snapshot"
        )
        if (
            snapshot_data != starting_data[role]
            or snapshot_ending != snapshot_identities[role]
            or snapshot_ending["mode"] != SNAPSHOT_MODES[role]
        ):
            fail(f"private calibration {role} snapshot changed after creation")
        result[role] = {
            "end": dict(ending_identity),
            "start": dict(starting_identities[role]),
        }
        result[f"{role}_snapshot"] = {
            "end": dict(snapshot_ending),
            "start": dict(snapshot_identities[role]),
        }
    for role in ("controller", "runner", "workload"):
        ending_data, ending_identity = read_stable_fd(
            execution_fds[role], f"calibration {role} execution descriptor"
        )
        require_fd_path_identity(
            execution_fds[role],
            snapshot_paths[role],
            execution_identities[role],
            f"calibration {role} execution descriptor",
        )
        if (
            ending_data != starting_data[role]
            or ending_identity != execution_identities[role]
            or ending_identity != snapshot_identities[role]
        ):
            fail(f"calibration {role} execution descriptor changed")
        result[f"{role}_execution"] = {
            "end": dict(ending_identity),
            "start": dict(execution_identities[role]),
        }
    for name, (path, descriptor, starting_identity) in directory_bindings.items():
        ending_identity = read_directory_binding(
            path, descriptor, f"calibration {name}"
        )
        if ending_identity != starting_identity or ending_identity["mode"] != 0o700:
            fail(f"calibration {name} changed after it was sealed")
        result[name] = {
            "end": dict(ending_identity),
            "start": dict(starting_identity),
        }
    ending_state_data, ending_state_identity = read_stable_fd(
        launch_state_fd, "sealed calibration launch state"
    )
    require_fd_path_identity(
        launch_state_fd,
        launch_state_path,
        launch_state_identity,
        "sealed calibration launch state",
    )
    if (
        ending_state_data != launch_state_data
        or ending_state_identity != launch_state_identity
        or ending_state_identity["mode"] != 0o444
    ):
        fail("sealed calibration launch state changed after controller re-exec")
    result["launch_state"] = {
        "end": dict(ending_state_identity),
        "start": dict(launch_state_identity),
    }
    ending_output_parent = read_directory_inode_binding(
        output_parent_path, output_parent_fd, "calibration output parent"
    )
    if ending_output_parent != output_parent_identity:
        fail("calibration output parent inode/mode changed")
    result["output_parent_directory"] = {
        "end": dict(ending_output_parent),
        "start": dict(output_parent_identity),
    }
    return result


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")


def median(values: Sequence[Decimal]) -> Decimal:
    if not values:
        fail("cannot take a median of no observations")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def decimal_text(value: Decimal) -> str:
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text or "0"


def ratio_text(value: Decimal) -> str:
    return format(value.quantize(RATIO_PLACES, rounding=ROUND_HALF_EVEN), ".12f")


def require_lexical_file(path: Path, label: str, *, executable: bool = False) -> None:
    absolute = path.absolute()
    try:
        resolved = path.resolve(strict=True)
        info = path.lstat()
    except FileNotFoundError:
        fail(f"{label} is missing: {path}")
    if absolute != resolved or path.is_symlink() or not path.is_file():
        fail(f"{label} is not a canonical lexical regular file: {path}")
    if executable and not (info.st_mode & 0o111):
        fail(f"{label} is not executable: {path}")


def physical_core_mapping() -> list[dict[str, int]]:
    result: list[dict[str, int]] = []
    identities: set[tuple[int, int]] = set()
    for cpu in AFFINITY:
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        try:
            package_id = int(
                (topology / "physical_package_id").read_text(encoding="ascii").strip()
            )
            core_id = int((topology / "core_id").read_text(encoding="ascii").strip())
        except (FileNotFoundError, PermissionError, ValueError) as error:
            fail(f"cannot validate physical-core identity for CPU {cpu}: {error}")
        identity = (package_id, core_id)
        if identity in identities:
            fail(f"calibration cpuset contains SMT siblings for physical core {identity}")
        identities.add(identity)
        result.append({"core_id": core_id, "cpu": cpu, "package_id": package_id})
    if len(identities) != 8:
        fail("calibration cpuset does not map to eight distinct physical cores")
    return result


def enable_child_subreaper() -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        fail(f"prctl(PR_SET_CHILD_SUBREAPER) failed: {os.strerror(error)}")
    observed = ctypes.c_int(0)
    if libc.prctl(PR_GET_CHILD_SUBREAPER, ctypes.byref(observed), 0, 0, 0) != 0:
        error = ctypes.get_errno()
        fail(f"prctl(PR_GET_CHILD_SUBREAPER) failed: {os.strerror(error)}")
    if observed.value != 1:
        fail("Linux child-subreaper state did not remain enabled")


def read_proc_identity(pid: int) -> tuple[int, int, int, int] | None:
    """Return (ppid, pgrp, session, starttime), tolerating only process exit."""

    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except (FileNotFoundError, ProcessLookupError):
        return None
    except (PermissionError, OSError) as error:
        fail(f"cannot read /proc identity for PID {pid}: {error}")
    closing = text.rfind(")")
    if closing < 0:
        fail(f"malformed /proc identity for PID {pid}")
    fields = text[closing + 2 :].split()
    if len(fields) < 20:
        fail(f"short /proc identity for PID {pid}")
    try:
        return (int(fields[1]), int(fields[2]), int(fields[3]), int(fields[19]))
    except ValueError:
        fail(f"nonnumeric /proc identity for PID {pid}")


def controller_descendants() -> dict[int, tuple[int, int, int, int]]:
    table: dict[int, tuple[int, int, int, int]] = {}
    try:
        entries = list(Path("/proc").iterdir())
    except (PermissionError, OSError) as error:
        fail(f"cannot enumerate /proc for descendant proof: {error}")
    for entry in entries:
        if not entry.name.isdigit():
            continue
        identity = read_proc_identity(int(entry.name))
        if identity is not None:
            table[int(entry.name)] = identity
    descendants: dict[int, tuple[int, int, int, int]] = {}
    parents = {os.getpid()}
    changed = True
    while changed:
        changed = False
        for pid, identity in table.items():
            if pid not in descendants and identity[0] in parents:
                descendants[pid] = identity
                parents.add(pid)
                changed = True
    return descendants


def kill_identity(pid: int, identity: tuple[int, int, int, int]) -> None:
    observed = read_proc_identity(pid)
    if (
        observed is None
        or observed[2] != identity[2]
        or observed[3] != identity[3]
    ):
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except PermissionError as error:
        fail(
            "cannot kill calibration descendant "
            f"PID {pid}/session {identity[2]}/start {identity[3]}: {error}"
        )


def reap_available_children() -> tuple[int, bool]:
    reaped = 0
    while True:
        try:
            waited, _, _ = os.wait4(-1, os.WNOHANG)
        except ChildProcessError:
            return reaped, True
        except OSError as error:
            if error.errno == errno.ECHILD:
                return reaped, True
            if error.errno == errno.EINTR:
                continue
            fail(f"wait4(-1) failed during descendant cleanup: {error}")
        if waited == 0:
            return reaped, False
        reaped += 1


def kill_reap_until_closed(label: str) -> None:
    """Kill every identity-scoped descendant and prove ECHILD plus empty /proc."""

    deadline = time.monotonic_ns() + 5_000_000_000
    observed_sessions: set[int] = set()
    while True:
        descendants = controller_descendants()
        observed_sessions.update(identity[2] for identity in descendants.values())
        for pid, identity in descendants.items():
            kill_identity(pid, identity)
        _, echild = reap_available_children()
        remaining = controller_descendants()
        if echild and not remaining:
            return
        if time.monotonic_ns() >= deadline:
            detail = sorted(
                (pid, identity[2], identity[3])
                for pid, identity in remaining.items()
            )
            fail(
                f"{label}: cannot prove closed descendant lifecycle; "
                f"remaining(pid,session,starttime)={detail}, "
                f"observed_sessions={sorted(observed_sessions)}, echild={echild}"
            )
        time.sleep(0.01)


def require_closed_lifecycle(label: str) -> None:
    descendants = controller_descendants()
    reaped, echild = reap_available_children()
    remaining = controller_descendants()
    if not descendants and not remaining and reaped == 0 and echild:
        return
    kill_reap_until_closed(label)
    fail(
        f"{label}: child returned with an open lifecycle "
        f"(descendants={sorted(descendants)}, reaped={reaped}, echild={echild})"
    )


def read_metrics(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            fail(f"invalid process-metrics line: {line!r}")
        key, value = line.split("=", 1)
        if key in values:
            fail(f"duplicate process-metrics key: {key}")
        values[key] = value
    validate_success_metrics(values)
    return values


def validate_success_metrics(values: Mapping[str, str]) -> None:
    if set(values) != PROCESS_METRICS_V2_FIELDS or any(
        type(value) is not str for value in values.values()
    ):
        fail("wrapped arm does not contain the exact 41-string-key schema-v2 mapping")
    exact = {
        "schema_version": "2", "outcome": "exited", "exit_code": "0",
        "term_signal": "0", "timed_out": "0", "runner_exit_code": "0",
        "rss_kb_unit": "1024_bytes", "subreaper_enabled": "1",
        "descendants_reaped": "0", "post_leader_descendants": "0",
        "descendant_cleanup_kill_sent": "0", "live_descendants_at_return": "0",
        "process_group_alive_at_return": "0", "wait4_echild_at_return": "1",
        "monitor_error": "0", "monitor_error_count": "0", "wait4_collected": "1",
        "wait_errno": "0", "child_error_stage": "none", "child_error_errno": "0",
        "core_dumped": "0", "timeout_term_sent": "0", "timeout_kill_sent": "0",
        "rss_limit_bytes": str(RSS_LIMIT_BYTES), "rss_limit_enabled": "1",
        "rss_limit_observed": "0", "rss_limit_exceeded": "0",
        "rss_limit_trigger_bytes": "0", "rss_limit_term_sent": "0",
        "rss_limit_kill_sent": "0", "peak_sampled_process_count": "1",
    }
    if any(values.get(key) != wanted for key, wanted in exact.items()):
        fail("wrapped arm reports a non-success, cap, timeout, or lifecycle condition")
    decimal_pattern = re.compile(r"(?:0|[1-9][0-9]*)\.[0-9]+")
    if any(not decimal_pattern.fullmatch(values[key]) for key in (
        "wall_seconds", "user_seconds", "system_seconds"
    )):
        fail("wrapped arm has a noncanonical duration")
    if Decimal(values["wall_seconds"]) <= 0 or Decimal(values["user_seconds"]) <= 0:
        fail("wrapped arm has no positive wall/user duration")
    unsigned_pattern = re.compile(r"0|[1-9][0-9]*")
    unsigned = PROCESS_METRICS_V2_FIELDS - {
        "outcome", "exit_code", "wall_seconds", "user_seconds", "system_seconds",
        "rss_kb_unit", "child_error_stage",
    }
    if any(not unsigned_pattern.fullmatch(values[key]) for key in unsigned):
        fail("wrapped arm has a noncanonical unsigned metric")
    for key in (
        "proc_status_samples", "proc_rss_samples", "proc_swap_samples",
        "proc_group_samples", "max_rss_kb", "peak_sampled_rss_kb",
    ):
        if int(values[key]) <= 0:
            fail(f"wrapped arm has no {key}")


def proc_stat_snapshot() -> dict[int, tuple[int, int]]:
    result: dict[int, tuple[int, int]] = {}
    with Path("/proc/stat").open(encoding="ascii") as stream:
        for line in stream:
            match = re.match(r"cpu([0-9]+)\s+(.+)$", line)
            if match is None:
                continue
            fields = [int(item) for item in match.group(2).split()]
            if len(fields) < 8:
                fail("/proc/stat CPU row is too short")
            idle = fields[3] + fields[4]
            total = sum(fields[:8])
            result[int(match.group(1))] = (total - idle, total)
    return result


def busy_ppm(
    before: Mapping[int, tuple[int, int]],
    after: Mapping[int, tuple[int, int]],
    cpus: Sequence[int],
) -> int:
    maximum = 0
    for cpu in cpus:
        if cpu not in before or cpu not in after:
            fail(f"logical CPU {cpu} is absent from /proc/stat")
        busy_delta = after[cpu][0] - before[cpu][0]
        total_delta = after[cpu][1] - before[cpu][1]
        if total_delta <= 0 or busy_delta < 0 or busy_delta > total_delta:
            fail(f"invalid /proc/stat delta for logical CPU {cpu}")
        maximum = max(maximum, (busy_delta * 1_000_000) // total_delta)
    return maximum


def minimum_guard_scan_count(duration_ns: int, max_gap_ns: int) -> int:
    if duration_ns <= 0 or max_gap_ns <= 0:
        fail("live interference guard has a nonpositive coverage interval")
    return max(2, (duration_ns + max_gap_ns - 1) // max_gap_ns + 1)


class InterferenceGuard:
    _comm = re.compile(r"^(?:cc1plus|ctest|ninja|make|gmake|dagutil|larch2)$")
    _command = re.compile(
        r"cmake --build|wric_spr_search_benchmark\.sh|"
        r"wric_spr_search_benchmark_harness_test\.sh|wric_process_metrics_test\.sh"
    )

    def __init__(self) -> None:
        self.stop_event = threading.Event()
        self.matches: list[str] = []
        self.failure: str | None = None
        self.scan_count = 0
        self.started_ns = 0
        self.stopped_ns = 0
        self.coverage_started_ns = 0
        self.coverage_ended_ns = 0
        self.first_heartbeat_ns = 0
        self.final_heartbeat_ns = 0
        self.max_heartbeat_gap_ns = 0
        self.thread = threading.Thread(target=self._watch, daemon=True)

    def _record_heartbeat(self) -> None:
        heartbeat = time.monotonic_ns()
        previous = self.final_heartbeat_ns
        self.scan_count += 1
        if self.first_heartbeat_ns == 0:
            self.first_heartbeat_ns = heartbeat
        elif previous > 0:
            gap = heartbeat - previous
            self.max_heartbeat_gap_ns = max(self.max_heartbeat_gap_ns, gap)
            if gap > LIVE_GUARD_MAX_GAP_NS:
                raise CalibrationError(
                    "interference watcher heartbeat gap exceeded the fixed bound: "
                    f"{gap} > {LIVE_GUARD_MAX_GAP_NS}"
                )
        self.final_heartbeat_ns = heartbeat

    def _watch(self) -> None:
        try:
            while not self.stop_event.is_set():
                found: list[str] = []
                try:
                    entries = list(Path("/proc").iterdir())
                except (PermissionError, OSError) as error:
                    raise CalibrationError(
                        f"cannot enumerate /proc during interference scan: {error}"
                    ) from error
                for entry in entries:
                    if not entry.name.isdigit() or int(entry.name) == os.getpid():
                        continue
                    try:
                        comm = (entry / "comm").read_text(
                            encoding="utf-8"
                        ).strip()
                        command = (
                            (entry / "cmdline")
                            .read_bytes()
                            .replace(b"\0", b" ")
                            .decode("utf-8", errors="strict")
                        )
                    except (FileNotFoundError, ProcessLookupError):
                        # A process disappearing between directory enumeration
                        # and the two reads is an expected, fully observed race.
                        continue
                    except (PermissionError, UnicodeDecodeError, OSError) as error:
                        raise CalibrationError(
                            f"failed interference scan for PID {entry.name}: {error}"
                        ) from error
                    if self._comm.fullmatch(comm) or self._command.search(command):
                        found.append(f"{entry.name}:{comm}:{command}")
                self._record_heartbeat()
                if found:
                    self.matches.extend(found)
                    self.stop_event.set()
                    return
                self.stop_event.wait(LIVE_GUARD_POLL_MS / 1000)
        except BaseException as error:
            self.failure = f"{type(error).__name__}: {error}"
            self.stop_event.set()

    def start(self) -> None:
        self.started_ns = time.monotonic_ns()
        self.thread.start()
        deadline = self.started_ns + LIVE_GUARD_MAX_GAP_NS
        while self.scan_count == 0 and self.failure is None:
            if time.monotonic_ns() >= deadline:
                self.failure = "interference watcher produced no initial heartbeat"
                self.stop_event.set()
                break
            time.sleep(0.01)
        self.check()
        if self.first_heartbeat_ns - self.started_ns > LIVE_GUARD_MAX_GAP_NS:
            fail("interference watcher initial heartbeat exceeded the fixed bound")
        self.coverage_started_ns = time.monotonic_ns()

    def finish_coverage(self) -> None:
        if self.coverage_started_ns <= 0 or self.coverage_ended_ns != 0:
            fail("live interference guard coverage has an invalid lifecycle")
        self.check()
        self.coverage_ended_ns = time.monotonic_ns()
        wanted_scan_count = self.scan_count + 1
        deadline = self.coverage_ended_ns + LIVE_GUARD_MAX_GAP_NS
        while self.scan_count < wanted_scan_count:
            self.check()
            if time.monotonic_ns() >= deadline:
                fail("interference watcher produced no post-coverage heartbeat")
            time.sleep(0.01)
        self.check()

    def check(self) -> None:
        if self.failure is not None:
            fail(f"live interference guard failed closed: {self.failure}")
        if self.matches:
            fail("live interference guard observed a forbidden build/benchmark process")
        if self.scan_count <= 0 or self.final_heartbeat_ns <= 0:
            fail("live interference guard lacks a positive scan/heartbeat proof")
        now = self.stopped_ns or time.monotonic_ns()
        current_gap = now - self.final_heartbeat_ns
        if current_gap > LIVE_GUARD_MAX_GAP_NS:
            fail(
                "live interference guard heartbeat is stale: "
                f"{current_gap} > {LIVE_GUARD_MAX_GAP_NS}"
            )
        if not self.stop_event.is_set() and not self.thread.is_alive():
            fail("live interference watcher terminated without a recorded failure")

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=2)
        self.stopped_ns = time.monotonic_ns()
        if self.thread.is_alive():
            fail("live interference watcher did not terminate")
        self.check()

    def evidence(self, max_unselected_smt_busy_ppm: int) -> dict[str, int]:
        if self.stopped_ns <= self.started_ns:
            fail("live interference guard was not stopped after it started")
        self.check()
        if not (
            self.first_heartbeat_ns
            <= self.coverage_started_ns
            <= self.coverage_ended_ns
            <= self.final_heartbeat_ns
        ):
            fail("live interference guard heartbeats do not enclose coverage")
        if any(
            gap > LIVE_GUARD_MAX_GAP_NS
            for gap in (
                self.first_heartbeat_ns - self.started_ns,
                self.coverage_started_ns - self.first_heartbeat_ns,
                self.final_heartbeat_ns - self.coverage_ended_ns,
                self.stopped_ns - self.final_heartbeat_ns,
            )
        ):
            fail("live interference guard boundary heartbeat exceeded its bound")
        duration_ns = self.coverage_ended_ns - self.coverage_started_ns
        minimum_scans = minimum_guard_scan_count(
            duration_ns, LIVE_GUARD_MAX_GAP_NS
        )
        if (
            self.max_heartbeat_gap_ns > LIVE_GUARD_MAX_GAP_NS
            or self.scan_count < minimum_scans
            or duration_ns > (self.scan_count - 1) * self.max_heartbeat_gap_ns
        ):
            fail(
                "live interference guard lacks continuous coverage: "
                f"gap={self.max_heartbeat_gap_ns}, scans={self.scan_count}, "
                f"required_scans={minimum_scans}"
            )
        return {
            "coverage_duration_ns": duration_ns,
            "coverage_ended_ns": self.coverage_ended_ns,
            "coverage_started_ns": self.coverage_started_ns,
            "final_heartbeat_ns": self.final_heartbeat_ns,
            "first_heartbeat_ns": self.first_heartbeat_ns,
            "forbidden_process_matches": len(self.matches),
            "max_consecutive_heartbeat_gap_ns": self.max_heartbeat_gap_ns,
            "max_consecutive_heartbeat_gap_threshold_ns": LIVE_GUARD_MAX_GAP_NS,
            "max_unselected_smt_busy_ppm": max_unselected_smt_busy_ppm,
            "minimum_scan_count": minimum_scans,
            "poll_interval_ms": LIVE_GUARD_POLL_MS,
            "scan_count": self.scan_count,
            "scan_failures": 0 if self.failure is None else 1,
            "started_ns": self.started_ns,
            "stopped_ns": self.stopped_ns,
            "threshold_ppm": QUIET_THRESHOLD_PPM,
        }


def stream_record(path: Path) -> dict[str, object]:
    return {"bytes": path.stat().st_size, "sha256": sha256_file(path)}


def status_record(status: int, usage: object) -> dict[str, object]:
    exit_code = os.waitstatus_to_exitcode(status)
    return {
        "core_dumped": bool(os.WCOREDUMP(status)) if os.WIFSIGNALED(status) else False,
        "exit_code": exit_code if os.WIFEXITED(status) else -1,
        "max_rss_kb": int(usage.ru_maxrss),
        "system_us": round(usage.ru_stime * 1_000_000),
        "term_signal": os.WTERMSIG(status) if os.WIFSIGNALED(status) else 0,
        "user_us": round(usage.ru_utime * 1_000_000),
    }


def run_wait4(
    exec_fd: int,
    argv: Sequence[str],
    stdout_path: Path,
    stderr_path: Path,
    timeout_seconds: float,
) -> tuple[int, int, dict[str, object]]:
    started = time.monotonic_ns()
    pid = -1
    try:
        pid = os.fork()
        if pid == 0:
            try:
                os.setsid()
                os.sched_setaffinity(0, set(AFFINITY))
                stdout_fd = os.open(
                    stdout_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
                )
                stderr_fd = os.open(
                    stderr_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
                )
                os.dup2(stdout_fd, 1)
                os.dup2(stderr_fd, 2)
                os.close(stdout_fd)
                os.close(stderr_fd)
                os.execve(exec_fd, list(argv), ENVIRONMENT)
            except BaseException:
                os._exit(127)
        deadline = started + timeout_seconds * 1_000_000_000
        while True:
            try:
                waited, status, usage = os.wait4(pid, os.WNOHANG)
            except ChildProcessError:
                fail("outer calibration child vanished before wait4 collection")
            except OSError as error:
                if error.errno == errno.EINTR:
                    continue
                fail(f"outer wait4 failed: {error}")
            if waited == pid:
                ended = time.monotonic_ns()
                result = (started, ended, status_record(status, usage))
                require_closed_lifecycle("completed calibration arm")
                return result
            if time.monotonic_ns() >= deadline:
                fail(f"outer controller timeout after {timeout_seconds}s: {argv[0]}")
            time.sleep(0.01)
    except BaseException:
        if pid == 0:
            os._exit(127)
        if pid > 0:
            kill_reap_until_closed("exceptional calibration arm cleanup")
        raise


def validate_wait_success(record: Mapping[str, object], label: str) -> None:
    expected_keys = {
        "core_dumped", "exit_code", "max_rss_kb", "system_us", "term_signal", "user_us"
    }
    if set(record) != expected_keys:
        fail(f"{label} wait4 mapping has a changed key set")
    if (
        type(record["core_dumped"]) is not bool
        or record["core_dumped"]
        or type(record["exit_code"]) is not int
        or record["exit_code"] != 0
        or type(record["term_signal"]) is not int
        or record["term_signal"] != 0
    ):
        fail(f"{label} did not exit successfully")
    for key in ("max_rss_kb", "system_us", "user_us"):
        if type(record[key]) is not int or int(record[key]) < 0:
            fail(f"{label} has an invalid wait4 {key}")
    if int(record["max_rss_kb"]) <= 0:
        fail(f"{label} has no wait4 max RSS")


def common_arm(
    started_ns: int,
    ended_ns: int,
    wait4: dict[str, object],
    stdout_path: Path,
    stderr_path: Path,
) -> dict[str, object]:
    wall_ns = ended_ns - started_ns
    if started_ns <= 0 or ended_ns <= started_ns:
        fail("calibration arm has an invalid monotonic interval")
    validate_wait_success(wait4, "calibration arm")
    if wall_ns < MIN_ARM_WALL_NS:
        fail("calibration arm is shorter than the fixed five-second minimum")
    stdout = stream_record(stdout_path)
    stderr = stream_record(stderr_path)
    if stdout != {
        "bytes": WORKLOAD_STDOUT_BYTES,
        "sha256": WORKLOAD_STDOUT_SHA256,
    } or stdout_path.read_text(encoding="ascii") != WORKLOAD_STDOUT_TEXT:
        fail("calibration workload stdout differs from the hard-bound v1 content")
    if stderr != {"bytes": 0, "sha256": EMPTY_SHA256}:
        fail("calibration workload wrote stderr")
    return {
        "argv": list(SYMBOLIC_ARGV),
        "outer_clock": "CLOCK_MONOTONIC",
        "outer_ended_ns": ended_ns,
        "outer_started_ns": started_ns,
        "outer_wall_ns": wall_ns,
        "redirections": dict(SYMBOLIC_REDIRECTIONS),
        "stderr": stderr,
        "stdout": stdout,
        "wait4": wait4,
    }


def run_direct(
    workload: Path, workload_fd: int, directory: Path, label: str
) -> dict[str, object]:
    stdout_path = directory / f"{label}.direct.stdout"
    stderr_path = directory / f"{label}.direct.stderr"
    started_ns, ended_ns, wait4 = run_wait4(
        workload_fd,
        [f"/proc/self/fd/{workload_fd}"],
        stdout_path,
        stderr_path,
        TIMEOUT_SECONDS,
    )
    wall_ns = ended_ns - started_ns
    result = common_arm(started_ns, ended_ns, wait4, stdout_path, stderr_path)
    cpu_us = int(wait4["user_us"]) + int(wait4["system_us"])
    cpu_per_wall = Decimal(cpu_us * 1000) / Decimal(wall_ns)
    if (
        cpu_us < MIN_ARM_CPU_US
        or cpu_per_wall < SATURATION_MIN
        or cpu_per_wall > SATURATION_MAX
    ):
        fail(
            "direct arm lacks plausible eight-core saturation: "
            f"cpu/wall={cpu_per_wall}"
        )
    return result


def run_wrapped(
    runner: Path,
    runner_fd: int,
    workload_fd: int,
    directory: Path,
    label: str,
) -> dict[str, object]:
    stdout_path = directory / f"{label}.wrapped.stdout"
    stderr_path = directory / f"{label}.wrapped.stderr"
    metrics_path = directory / f"{label}.wrapped.metrics"
    driver_stdout = directory / f"{label}.driver.stdout"
    driver_stderr = directory / f"{label}.driver.stderr"
    argv = [
        str(runner), "--timeout-seconds", str(TIMEOUT_SECONDS),
        "--rss-limit-bytes", str(RSS_LIMIT_BYTES), "--stdout", str(stdout_path),
        "--stderr", str(stderr_path), "--metrics", str(metrics_path), "--",
        f"/proc/self/fd/{workload_fd}",
    ]
    started_ns, ended_ns, wait4 = run_wait4(
        runner_fd,
        argv, driver_stdout, driver_stderr, TIMEOUT_SECONDS + 10
    )
    wall_ns = ended_ns - started_ns
    result = common_arm(started_ns, ended_ns, wait4, stdout_path, stderr_path)
    metrics = read_metrics(metrics_path)
    inner_wall_ns = Decimal(metrics["wall_seconds"]) * Decimal(1_000_000_000)
    inner_cpu_us = (
        Decimal(metrics["user_seconds"]) + Decimal(metrics["system_seconds"])
    ) * Decimal(1_000_000)
    outer_cpu_us = Decimal(
        int(wait4["user_us"]) + int(wait4["system_us"])
    )
    wall_delta_ns = Decimal(wall_ns) - inner_wall_ns
    cpu_delta_us = outer_cpu_us - inner_cpu_us
    inner_rss_kb = int(metrics["max_rss_kb"])
    outer_rss_kb = int(wait4["max_rss_kb"])
    cpu_per_wall = (inner_cpu_us * 1000) / Decimal(wall_ns)
    if (
        inner_wall_ns < MIN_ARM_WALL_NS
        or inner_cpu_us < MIN_ARM_CPU_US
        or cpu_per_wall < SATURATION_MIN
        or cpu_per_wall > SATURATION_MAX
    ):
        fail(
            "wrapped arm lacks plausible eight-core saturation: "
            f"cpu/wall={cpu_per_wall}"
        )
    if not (
        -INNER_OUTER_WALL_EARLY_TOLERANCE_NS
        <= wall_delta_ns
        <= INNER_OUTER_WALL_MAX_DELTA_NS
    ):
        fail(f"wrapped inner/outer wall clocks are not bound: delta_ns={wall_delta_ns}")
    if not (
        -INNER_OUTER_CPU_EARLY_TOLERANCE_US
        <= cpu_delta_us
        <= INNER_OUTER_CPU_MAX_DELTA_US
    ):
        fail(f"wrapped inner/outer CPU usage is not bound: delta_us={cpu_delta_us}")
    if not (
        inner_rss_kb
        <= outer_rss_kb
        <= inner_rss_kb + INNER_OUTER_RSS_MAX_DELTA_KB
    ):
        fail(
            "wrapped inner/outer max RSS is not bound: "
            f"inner={inner_rss_kb}, outer={outer_rss_kb}"
        )
    result.update(
        {
            "driver_stderr": stream_record(driver_stderr),
            "driver_stdout": stream_record(driver_stdout),
            "process_metrics": metrics,
        }
    )
    if result["driver_stdout"] != {"bytes": 0, "sha256": EMPTY_SHA256} or result[
        "driver_stderr"
    ] != {"bytes": 0, "sha256": EMPTY_SHA256}:
        fail("process wrapper wrote unexpected driver output")
    return result


def run_pair(
    index: int,
    runner: Path,
    workload: Path,
    execution_fds: Mapping[str, int],
    directory: Path,
    guard: InterferenceGuard,
    snapshot_paths: Mapping[str, Path],
    snapshot_identities: Mapping[str, Mapping[str, object]],
    directory_bindings: Mapping[
        str, tuple[Path, int, Mapping[str, object]]
    ],
    launch_state_path: Path,
    launch_state_fd: int,
    launch_state_identity: Mapping[str, object],
    output_parent_path: Path,
    output_parent_fd: int,
    output_parent_identity: Mapping[str, object],
) -> dict[str, object]:
    def verify_executable_bindings() -> None:
        for role in ("controller", "runner", "workload"):
            require_fd_path_identity(
                execution_fds[role],
                snapshot_paths[role],
                snapshot_identities[role],
                f"private calibration {role} execution descriptor",
            )
        for name, (path, descriptor, starting_identity) in directory_bindings.items():
            require_directory_binding(
                path, descriptor, starting_identity, f"calibration {name}"
            )
        require_fd_path_identity(
            launch_state_fd,
            launch_state_path,
            launch_state_identity,
            "sealed calibration launch state",
        )
        require_directory_inode_binding(
            output_parent_path,
            output_parent_fd,
            output_parent_identity,
            "calibration output parent",
        )

    order = "direct_then_wrapped" if index % 2 else "wrapped_then_direct"
    guard.check()
    verify_executable_bindings()
    if index % 2:
        direct = run_direct(
            workload, execution_fds["workload"], directory, f"pair-{index}"
        )
        guard.check()
        verify_executable_bindings()
        wrapped = run_wrapped(
            runner,
            execution_fds["runner"],
            execution_fds["workload"],
            directory,
            f"pair-{index}",
        )
    else:
        wrapped = run_wrapped(
            runner,
            execution_fds["runner"],
            execution_fds["workload"],
            directory,
            f"pair-{index}",
        )
        guard.check()
        verify_executable_bindings()
        direct = run_direct(
            workload, execution_fds["workload"], directory, f"pair-{index}"
        )
    guard.check()
    verify_executable_bindings()
    if direct["argv"] != wrapped["argv"] or direct["redirections"] != wrapped["redirections"]:
        fail("direct/wrapped workload argv or redirections differ")
    if direct["stdout"] != wrapped["stdout"]:
        fail("direct/wrapped workload output differs")
    first, second = (direct, wrapped) if index % 2 else (wrapped, direct)
    if int(first["outer_ended_ns"]) >= int(second["outer_started_ns"]):
        fail("direct/wrapped calibration arms overlap or contradict their order")
    return {"direct": direct, "index": index, "order": order, "wrapped": wrapped}


def build_summary(pairs: Sequence[Mapping[str, object]]) -> dict[str, object]:
    direct = [Decimal(int(pair["direct"]["outer_wall_ns"])) for pair in pairs]  # type: ignore[index]
    wrapped = [Decimal(int(pair["wrapped"]["outer_wall_ns"])) for pair in pairs]  # type: ignore[index]
    paired = [wrapped_value / direct_value for direct_value, wrapped_value in zip(direct, wrapped)]
    direct_median = median(direct)
    wrapped_median = median(wrapped)
    paired_median = median(paired)
    medians_ratio = wrapped_median / direct_median
    decision = "PASS" if paired_median <= RATIO_THRESHOLD and medians_ratio <= RATIO_THRESHOLD else "FAIL"
    return {
        "decision": decision,
        "direct_median_ns": decimal_text(direct_median),
        "median_paired_ratio": ratio_text(paired_median),
        "ratio_of_medians": ratio_text(medians_ratio),
        "threshold": ratio_text(RATIO_THRESHOLD),
        "wrapped_median_ns": decimal_text(wrapped_median),
    }


def write_exclusive_readonly(path: Path, data: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        raise


def write_exclusive_readonly_at(
    directory_fd: int, basename: str, data: bytes
) -> None:
    if basename != "wrapper-calibration.json" or "/" in basename:
        fail("refusing a non-canonical calibration artifact basename")
    descriptor = os.open(
        basename,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
        0o444,
        dir_fd=directory_fd,
    )
    try:
        offset = 0
        while offset < len(data):
            written = os.write(descriptor, data[offset:])
            if written <= 0:
                fail("short write while creating calibration artifact")
            offset += written
        os.fchmod(descriptor, 0o444)
        os.fsync(descriptor)
    except BaseException:
        os.close(descriptor)
        try:
            os.unlink(basename, dir_fd=directory_fd)
        except FileNotFoundError:
            pass
        raise
    else:
        os.close(descriptor)


SEALED_LAUNCH_KEYS = {
    "controller_snapshot",
    "input_snapshot_directory",
    "launch_state",
    "measured_pairs",
    "official_controller",
    "output",
    "repo_root",
    "run_directory",
    "schema",
    "schema_version",
    "stage_directory",
    "warmup_pairs",
}


def validate_output_request(output: Path, warmup_pairs: int, measured_pairs: int) -> None:
    if output.name != "wrapper-calibration.json":
        fail("output basename must be exactly wrapper-calibration.json")
    if os.path.lexists(output):
        fail(f"refusing to overwrite calibration output: {output}")
    try:
        output_parent = output.parent.resolve(strict=True)
        parent_info = output.parent.lstat()
    except OSError as error:
        fail(f"cannot validate calibration output parent: {error}")
    if (
        output != output.absolute()
        or output_parent != output.parent
        or not stat.S_ISDIR(parent_info.st_mode)
    ):
        fail("calibration output parent is not a canonical lexical directory")
    if warmup_pairs < 2 or measured_pairs < 11:
        fail("calibration requires >=2 warmup and >=11 measured pairs")


def load_sealed_launch_state(
    descriptor: int,
) -> tuple[dict[str, object], bytes, dict[str, object]]:
    data, identity = read_stable_fd(descriptor, "sealed calibration launch state")
    try:
        decoded = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"sealed calibration launch state is not canonical JSON: {error}")
    if type(decoded) is not dict or set(decoded) != SEALED_LAUNCH_KEYS:
        fail("sealed calibration launch state has a changed exact schema")
    if canonical_json(decoded) != data:
        fail("sealed calibration launch state is not in canonical byte form")
    if (
        decoded["schema"] != SEALED_LAUNCH_SCHEMA
        or type(decoded["schema_version"]) is not int
        or decoded["schema_version"] != SEALED_LAUNCH_VERSION
    ):
        fail("sealed calibration launch state schema/version changed")
    for key in SEALED_LAUNCH_KEYS - {
        "measured_pairs", "schema_version", "warmup_pairs"
    }:
        if type(decoded[key]) is not str:
            fail(f"sealed calibration launch state {key} is not a string")
    for key in ("warmup_pairs", "measured_pairs"):
        if type(decoded[key]) is not int:
            fail(f"sealed calibration launch state {key} is not an integer")
    return decoded, data, identity


def safe_remove_private_stage(
    stage: Path, stage_fd: int, expected: Mapping[str, object]
) -> None:
    try:
        fd_info = os.fstat(stage_fd)
        path_info = stage.stat(follow_symlinks=False)
    except OSError as error:
        fail(f"cannot bind private calibration stage for cleanup: {error}")
    if (
        not stat.S_ISDIR(fd_info.st_mode)
        or not stat.S_ISDIR(path_info.st_mode)
        or fd_info.st_dev != int(expected["device"])
        or fd_info.st_ino != int(expected["inode"])
        or path_info.st_dev != int(expected["device"])
        or path_info.st_ino != int(expected["inode"])
    ):
        fail("refusing to remove a replaced private calibration stage")
    shutil.rmtree(stage)
    if os.path.lexists(stage):
        fail("private calibration stage survived successful cleanup")


def run(args: argparse.Namespace) -> None:
    """Minimal stager: snapshot the live controller and replace this process."""

    output = Path(args.output).absolute()
    validate_output_request(output, args.warmup_pairs, args.measured_pairs)
    controller = Path(__file__).absolute()
    require_lexical_file(controller, "controller", executable=True)
    repo_root = controller.parent.parent
    if controller != repo_root / "tools/wric_wrapper_calibration.py":
        fail("controller is not at its exact repository-relative path")
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    enable_child_subreaper()
    require_closed_lifecycle("calibration controller stager startup")

    stage: Path | None = None
    stage_name: str | None = None
    stage_fd: int | None = None
    stage_cleanup_identity: dict[str, int] | None = None
    output_parent_fd: int | None = None
    descriptors: list[int] = []
    try:
        output_parent_fd, output_parent_identity = open_inode_bound_directory(
            output.parent, "calibration output parent", inheritable=True
        )
        descriptors.append(output_parent_fd)
        while True:
            stage_name = f".wric-wrapper-calibration-{os.urandom(12).hex()}"
            try:
                os.mkdir(stage_name, mode=0o700, dir_fd=output_parent_fd)
            except FileExistsError:
                continue
            break
        stage = output.parent / stage_name
        stage_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            stage_flags |= os.O_NOFOLLOW
        stage_fd = os.open(stage_name, stage_flags, dir_fd=output_parent_fd)
        descriptors.append(stage_fd)
        os.fchmod(stage_fd, 0o700)
        os.set_inheritable(stage_fd, True)
        stage_cleanup_identity = read_directory_binding(
            stage, stage_fd, "private calibration stage"
        )
        if stage_cleanup_identity["mode"] != 0o700:
            fail("private calibration stage is not exact mode 0700")
        inputs = stage / "inputs"
        run_directory = stage / "run"
        inputs.mkdir(mode=0o700)
        run_directory.mkdir(mode=0o700)
        controller_snapshot = stage / "wric-wrapper-calibration.py"
        launch_state_path = stage / "sealed-launch.json"

        controller_data, _ = read_stable_input(
            controller, "launch-time live calibration controller"
        )
        controller_snapshot_identity = write_private_snapshot(
            controller_snapshot,
            controller_data,
            SNAPSHOT_MODES["controller"],
            "launch-time private controller snapshot",
        )
        launch_state = {
            "controller_snapshot": str(controller_snapshot),
            "input_snapshot_directory": str(inputs),
            "launch_state": str(launch_state_path),
            "measured_pairs": args.measured_pairs,
            "official_controller": str(controller),
            "output": str(output),
            "repo_root": str(repo_root),
            "run_directory": str(run_directory),
            "schema": SEALED_LAUNCH_SCHEMA,
            "schema_version": SEALED_LAUNCH_VERSION,
            "stage_directory": str(stage),
            "warmup_pairs": args.warmup_pairs,
        }
        launch_state_identity = write_private_snapshot(
            launch_state_path,
            canonical_json(launch_state),
            0o444,
            "sealed calibration launch state",
        )
        controller_fd = open_bound_regular_file(
            controller_snapshot,
            controller_snapshot_identity,
            "launch-time controller execution descriptor",
            inheritable=True,
        )
        descriptors.append(controller_fd)
        state_fd = open_bound_regular_file(
            launch_state_path,
            launch_state_identity,
            "sealed calibration launch-state descriptor",
            inheritable=True,
        )
        descriptors.append(state_fd)
        require_directory_inode_binding(
            output.parent,
            output_parent_fd,
            output_parent_identity,
            "calibration output parent",
        )
        require_directory_binding(
            stage,
            stage_fd,
            read_directory_binding(stage, stage_fd, "private calibration stage"),
            "private calibration stage",
        )
        if set(os.listdir(stage)) != {
            "inputs", "run", "sealed-launch.json", "wric-wrapper-calibration.py"
        }:
            fail("private calibration stage has an unexpected direct entry")
        os.execve(
            controller_fd,
            [
                str(controller_snapshot),
                "_sealed-run",
                "--controller-fd", str(controller_fd),
                "--output-parent-fd", str(output_parent_fd),
                "--stage-fd", str(stage_fd),
                "--state-fd", str(state_fd),
            ],
            ENVIRONMENT,
        )
    finally:
        try:
            if (
                stage is not None
                and stage_fd is not None
                and stage_cleanup_identity is not None
                and os.path.lexists(stage)
            ):
                safe_remove_private_stage(stage, stage_fd, stage_cleanup_identity)
            elif stage_name is not None and output_parent_fd is not None:
                try:
                    os.rmdir(stage_name, dir_fd=output_parent_fd)
                except FileNotFoundError:
                    pass
        finally:
            for descriptor in reversed(descriptors):
                try:
                    os.close(descriptor)
                except OSError:
                    pass
            kill_reap_until_closed("failed calibration controller stager cleanup")


def _sealed_run_authoritative(args: argparse.Namespace) -> None:
    """Authoritative controller entered only through the sealed snapshot FD."""

    if __file__ != f"/dev/fd/{args.controller_fd}":
        fail("sealed controller was not loaded from its declared /dev/fd descriptor")
    launch, launch_data, launch_identity = load_sealed_launch_state(args.state_fd)
    paths = {
        key: Path(str(launch[key]))
        for key in (
            "controller_snapshot", "input_snapshot_directory", "launch_state",
            "official_controller", "output", "repo_root", "run_directory",
            "stage_directory",
        )
    }
    stage = paths["stage_directory"]
    output = paths["output"]
    repo_root = paths["repo_root"]
    controller = paths["official_controller"]
    snapshot_root = paths["input_snapshot_directory"]
    run_root = paths["run_directory"]
    controller_snapshot = paths["controller_snapshot"]
    launch_state_path = paths["launch_state"]
    validate_output_request(
        output, int(launch["warmup_pairs"]), int(launch["measured_pairs"])
    )
    output_parent_identity = read_directory_inode_binding(
        output.parent,
        args.output_parent_fd,
        "sealed calibration output parent",
    )
    if (
        repo_root != repo_root.absolute()
        or repo_root.resolve(strict=True) != repo_root
        or controller != repo_root / "tools/wric_wrapper_calibration.py"
        or stage != stage.absolute()
        or stage.resolve(strict=True) != stage
        or stage.parent != output.parent
        or not stage.name.startswith(".wric-wrapper-calibration-")
        or controller_snapshot != stage / "wric-wrapper-calibration.py"
        or launch_state_path != stage / "sealed-launch.json"
        or snapshot_root != stage / "inputs"
        or run_root != stage / "run"
    ):
        fail("sealed calibration launch paths are not independently derivable")
    if set(os.listdir(stage)) != {
        "inputs", "run", "sealed-launch.json", "wric-wrapper-calibration.py"
    }:
        fail("sealed calibration stage has an unexpected direct entry")
    require_lexical_file(controller, "official live controller", executable=True)
    require_lexical_file(
        controller_snapshot, "private controller snapshot", executable=True
    )
    require_fd_path_identity(
        args.state_fd, launch_state_path, launch_identity, "sealed launch state"
    )
    if launch_identity["mode"] != 0o444:
        fail("sealed calibration launch state is not exact mode 0444")
    stage_startup_identity = read_directory_binding(
        stage, args.stage_fd, "sealed calibration stage"
    )
    if stage_startup_identity["mode"] != 0o700:
        fail("sealed calibration stage is not exact mode 0700")

    execution_data, controller_execution_identity = read_stable_fd(
        args.controller_fd, "executing sealed calibration controller"
    )
    live_controller_data, live_controller_identity = read_stable_input(
        controller, "official live calibration controller"
    )
    snapshot_controller_data, snapshot_controller_identity = read_stable_input(
        controller_snapshot, "private calibration controller snapshot"
    )
    require_fd_path_identity(
        args.controller_fd,
        controller_snapshot,
        controller_execution_identity,
        "executing sealed calibration controller",
    )
    if (
        execution_data != live_controller_data
        or execution_data != snapshot_controller_data
        or controller_execution_identity != snapshot_controller_identity
        or snapshot_controller_identity["mode"] != SNAPSHOT_MODES["controller"]
    ):
        fail("executing controller bytes are not bound to live and snapshot inputs")
    for descriptor in (
        args.controller_fd,
        args.state_fd,
        args.stage_fd,
        args.output_parent_fd,
    ):
        os.set_inheritable(descriptor, False)

    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    enable_child_subreaper()
    require_closed_lifecycle("sealed calibration controller startup")
    document: dict[str, object] | None = None
    extra_fds: list[int] = []
    cleanup_error: BaseException | None = None
    collection_error: BaseException | None = None
    try:
        document = collect_sealed_calibration(
            launch,
            paths,
            launch_data,
            launch_identity,
            live_controller_data,
            live_controller_identity,
            snapshot_controller_identity,
            controller_execution_identity,
            args.controller_fd,
            args.state_fd,
            args.stage_fd,
            stage_startup_identity,
            args.output_parent_fd,
            output_parent_identity,
            extra_fds,
        )
    except BaseException as error:
        collection_error = error
    try:
        kill_reap_until_closed("final sealed calibration controller cleanup")
    except BaseException as error:
        if collection_error is None:
            collection_error = error
    for descriptor in reversed(extra_fds):
        try:
            os.close(descriptor)
        except OSError as error:
            if cleanup_error is None:
                cleanup_error = error
    try:
        safe_remove_private_stage(stage, args.stage_fd, stage_startup_identity)
    except BaseException as error:
        cleanup_error = error
    for descriptor in (args.state_fd, args.controller_fd, args.stage_fd):
        try:
            os.close(descriptor)
        except OSError as error:
            if cleanup_error is None:
                cleanup_error = error
    if collection_error is not None:
        os.close(args.output_parent_fd)
        raise collection_error
    if cleanup_error is not None:
        os.close(args.output_parent_fd)
        raise cleanup_error
    if document is None:
        os.close(args.output_parent_fd)
        fail("sealed calibration collection returned without a document")
    require_directory_inode_binding(
        output.parent,
        args.output_parent_fd,
        output_parent_identity,
        "final calibration output parent",
    )
    artifact_created = False
    artifact_data = canonical_json(document)
    try:
        write_exclusive_readonly_at(
            args.output_parent_fd, output.name, artifact_data
        )
        artifact_created = True
        require_directory_inode_binding(
            output.parent,
            args.output_parent_fd,
            output_parent_identity,
            "written calibration output parent",
        )
        fd_relative_info = os.stat(
            output.name, dir_fd=args.output_parent_fd, follow_symlinks=False
        )
        lexical_info = output.stat(follow_symlinks=False)
        if (
            not stat.S_ISREG(fd_relative_info.st_mode)
            or fd_relative_info.st_dev != lexical_info.st_dev
            or fd_relative_info.st_ino != lexical_info.st_ino
            or stat.S_IMODE(fd_relative_info.st_mode) != 0o444
            or stat.S_IMODE(lexical_info.st_mode) != 0o444
            or fd_relative_info.st_size != len(artifact_data)
            or lexical_info.st_size != len(artifact_data)
        ):
            fail("written calibration artifact lost its dirfd/lexical binding")
        artifact_flags = os.O_RDONLY | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            artifact_flags |= os.O_NOFOLLOW
        artifact_fd = os.open(
            output.name, artifact_flags, dir_fd=args.output_parent_fd
        )
        try:
            observed_data, artifact_identity = read_stable_fd(
                artifact_fd, "written calibration artifact"
            )
            require_fd_path_identity(
                artifact_fd,
                output,
                artifact_identity,
                "written calibration artifact",
            )
            if observed_data != artifact_data or artifact_identity["mode"] != 0o444:
                fail("written calibration artifact bytes/mode changed")
        finally:
            os.close(artifact_fd)
        os.fsync(args.output_parent_fd)
    except BaseException:
        if artifact_created:
            try:
                os.unlink(output.name, dir_fd=args.output_parent_fd)
                os.fsync(args.output_parent_fd)
            except FileNotFoundError:
                pass
        raise
    finally:
        os.close(args.output_parent_fd)
    print(f"PASS: canonical wrapper calibration: {output}")


def sealed_run(args: argparse.Namespace) -> None:
    """Ensure even startup-validation failures cannot leave a usable stage."""

    try:
        _sealed_run_authoritative(args)
    except BaseException:
        try:
            launch, _, _ = load_sealed_launch_state(args.state_fd)
            stage = Path(str(launch["stage_directory"]))
            output = Path(str(launch["output"]))
            if (
                stage == stage.absolute()
                and output == output.absolute()
                and stage.parent == output.parent
                and stage.name.startswith(".wric-wrapper-calibration-")
            ):
                require_directory_inode_binding(
                    output.parent,
                    args.output_parent_fd,
                    read_directory_inode_binding(
                        output.parent,
                        args.output_parent_fd,
                        "failed sealed-run output parent",
                    ),
                    "failed sealed-run output parent",
                )
                stage_identity = read_directory_binding(
                    stage, args.stage_fd, "failed sealed-run stage"
                )
                safe_remove_private_stage(stage, args.stage_fd, stage_identity)
        except BaseException:
            pass
        for descriptor in (
            args.state_fd,
            args.controller_fd,
            args.stage_fd,
            args.output_parent_fd,
        ):
            try:
                os.close(descriptor)
            except OSError:
                pass
        try:
            kill_reap_until_closed("failed sealed calibration startup cleanup")
        except BaseException:
            pass
        raise


def collect_sealed_calibration(
    launch: Mapping[str, object],
    sealed_paths: Mapping[str, Path],
    launch_data: bytes,
    launch_identity: Mapping[str, object],
    live_controller_data: bytes,
    live_controller_identity: Mapping[str, object],
    snapshot_controller_identity: Mapping[str, object],
    controller_execution_identity: Mapping[str, object],
    controller_fd: int,
    launch_state_fd: int,
    stage_fd: int,
    stage_startup_identity: Mapping[str, object],
    output_parent_fd: int,
    output_parent_identity: Mapping[str, object],
    opened_fds: list[int],
) -> dict[str, object]:
    repo_root = sealed_paths["repo_root"]
    controller = sealed_paths["official_controller"]
    output = sealed_paths["output"]
    root = sealed_paths["stage_directory"]
    snapshot_root = sealed_paths["input_snapshot_directory"]
    run_root = sealed_paths["run_directory"]
    launch_state_path = sealed_paths["launch_state"]
    controller_snapshot = sealed_paths["controller_snapshot"]
    workload_source = repo_root / "tools/wric_wrapper_calibration_workload.cpp"
    runner = repo_root / "build/bin/wric-process-metrics"
    workload = repo_root / "build/bin/wric-wrapper-calibration-workload"
    input_paths = {
        "controller": controller,
        "runner": runner,
        "workload": workload,
        "workload_source": workload_source,
    }
    for path, label, executable in (
        (controller, "controller", True),
        (workload_source, "workload source", False),
        (runner, "process wrapper", True),
        (workload, "calibration workload", True),
    ):
        require_lexical_file(path, label, executable=executable)
    starting_data: dict[str, bytes] = {"controller": live_controller_data}
    starting_identities: dict[str, dict[str, object]] = {
        "controller": dict(live_controller_identity)
    }
    for role in INPUT_ROLES[1:]:
        data, identity = read_stable_input(
            input_paths[role], f"live calibration {role}"
        )
        starting_data[role] = data
        starting_identities[role] = identity
    live_inode_roles = {
        (int(identity["device"]), int(identity["inode"]))
        for identity in starting_identities.values()
    }
    if len(live_inode_roles) != len(INPUT_ROLES):
        fail("live calibration input roles alias the same filesystem identity")
    if starting_identities["workload_source"]["sha256"] != WORKLOAD_SOURCE_SHA256:
        fail(
            "calibration workload source differs from the hard-bound v1 SHA-256"
        )
    warmup_pairs = int(launch["warmup_pairs"])
    measured_pairs = int(launch["measured_pairs"])
    available = os.sched_getaffinity(0)
    if not set(AFFINITY).issubset(available):
        fail(f"exact calibration cpuset is unavailable: {AFFINITY_TEXT}")
    physical_cores = physical_core_mapping()
    document: dict[str, object] | None = None
    if True:
        if True:
            snapshot_paths = {
                "controller": controller_snapshot,
                "runner": snapshot_root / "wric-process-metrics",
                "workload": snapshot_root / "wric-wrapper-calibration-workload",
                "workload_source": snapshot_root
                / "wric-wrapper-calibration-workload.cpp",
            }
            snapshot_identities: dict[str, dict[str, object]] = {
                "controller": dict(snapshot_controller_identity)
            }
            for role in INPUT_ROLES[1:]:
                snapshot_identities[role] = write_private_snapshot(
                    snapshot_paths[role],
                    starting_data[role],
                    SNAPSHOT_MODES[role],
                    f"private calibration {role} snapshot",
                )
            all_input_inodes = live_inode_roles | {
                (int(identity["device"]), int(identity["inode"]))
                for identity in snapshot_identities.values()
            }
            if len(all_input_inodes) != 2 * len(INPUT_ROLES):
                fail("private calibration snapshots alias an input role identity")
            runner_fd = open_bound_regular_file(
                snapshot_paths["runner"], snapshot_identities["runner"],
                "private calibration runner execution descriptor",
                inheritable=True,
            )
            opened_fds.append(runner_fd)
            workload_fd = open_bound_regular_file(
                snapshot_paths["workload"], snapshot_identities["workload"],
                "private calibration workload execution descriptor",
                inheritable=True,
            )
            opened_fds.append(workload_fd)
            input_directory_fd = open_bound_directory(
                snapshot_root,
                "private calibration input snapshot directory",
                inheritable=False,
            )
            opened_fds.append(input_directory_fd)
            if set(os.listdir(snapshot_root)) != {
                "wric-process-metrics",
                "wric-wrapper-calibration-workload",
                "wric-wrapper-calibration-workload.cpp",
            }:
                fail("private calibration input directory has unexpected entries")
            stage_identity = read_directory_binding(
                root, stage_fd, "private calibration stage"
            )
            input_directory_identity = read_directory_binding(
                snapshot_root,
                input_directory_fd,
                "private calibration input snapshot directory",
            )
            if (
                stage_identity != stage_startup_identity
                or
                stage_identity["mode"] != 0o700
                or input_directory_identity["mode"] != 0o700
            ):
                fail("private calibration directories changed or are not mode 0700")
            directory_bindings = {
                "stage_directory": (root, stage_fd, stage_startup_identity),
                "input_snapshot_directory": (
                    snapshot_root,
                    input_directory_fd,
                    input_directory_identity,
                ),
            }
            execution_fds = {
                "controller": controller_fd,
                "runner": runner_fd,
                "workload": workload_fd,
            }
            execution_identities = {
                "controller": dict(controller_execution_identity),
                "runner": dict(snapshot_identities["runner"]),
                "workload": dict(snapshot_identities["workload"]),
            }

            revision_result = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=repo_root,
                check=True,
                env=ENVIRONMENT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            require_closed_lifecycle("repository revision preflight")
            revision = revision_result.stdout.strip()
            if revision_result.stderr or revision != FROZEN_REVISION:
                fail(f"repository revision is not frozen: {revision}")
            version = subprocess.run(
                [f"/proc/self/fd/{workload_fd}", "--version"],
                env=ENVIRONMENT,
                check=True,
                pass_fds=(workload_fd,),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=10,
            )
            require_closed_lifecycle("snapshot workload version preflight")
            if version.stdout != WORKLOAD_VERSION_TEXT + "\n" or version.stderr:
                fail("calibration workload version response changed")

            preflight_before = proc_stat_snapshot()
            preflight_started = time.monotonic_ns()
            time.sleep(PREFLIGHT_SECONDS)
            preflight_duration = time.monotonic_ns() - preflight_started
            preflight_after = proc_stat_snapshot()
            preflight_max = busy_ppm(
                preflight_before, preflight_after, tuple(sorted(preflight_before))
            )
            if preflight_max > QUIET_THRESHOLD_PPM:
                fail(f"host quiet preflight failed: maximum busy ppm={preflight_max}")

            warmup_dir = run_root / "warmup"
            measured_dir = run_root / "measured"
            warmup_dir.mkdir()
            measured_dir.mkdir()
            guard = InterferenceGuard()
            try:
                guard.start()
                live_before = proc_stat_snapshot()
                warmups = [
                    run_pair(
                        index,
                        snapshot_paths["runner"],
                        snapshot_paths["workload"],
                        execution_fds,
                        warmup_dir,
                        guard,
                        snapshot_paths,
                        snapshot_identities,
                        directory_bindings,
                        launch_state_path,
                        launch_state_fd,
                        launch_identity,
                        output.parent,
                        output_parent_fd,
                        output_parent_identity,
                    )
                    for index in range(1, warmup_pairs + 1)
                ]
                measured = [
                    run_pair(
                        index,
                        snapshot_paths["runner"],
                        snapshot_paths["workload"],
                        execution_fds,
                        measured_dir,
                        guard,
                        snapshot_paths,
                        snapshot_identities,
                        directory_bindings,
                        launch_state_path,
                        launch_state_fd,
                        launch_identity,
                        output.parent,
                        output_parent_fd,
                        output_parent_identity,
                    )
                    for index in range(1, measured_pairs + 1)
                ]
                guard.finish_coverage()
                guard.stop()
            finally:
                if guard.thread.is_alive():
                    guard.stop_event.set()
                    guard.thread.join(timeout=2)
                    if guard.thread.is_alive():
                        fail("live interference watcher survived exceptional cleanup")
            live_after = proc_stat_snapshot()
            live_max = busy_ppm(live_before, live_after, UNSELECTED_SMT)
            if live_max > QUIET_THRESHOLD_PPM:
                fail(f"live unselected-SMT interference gate failed: busy ppm={live_max}")
            live_guard = guard.evidence(live_max)
            arm_sequence: list[Mapping[str, object]] = []
            for pair in [*warmups, *measured]:
                if pair["order"] == "direct_then_wrapped":
                    arm_sequence.extend((pair["direct"], pair["wrapped"]))  # type: ignore[arg-type]
                else:
                    arm_sequence.extend((pair["wrapped"], pair["direct"]))  # type: ignore[arg-type]
            for previous, current in zip(arm_sequence[1::2], arm_sequence[2::2]):
                if int(previous["outer_ended_ns"]) >= int(
                    current["outer_started_ns"]
                ):
                    fail("calibration arm intervals overlap or violate global order")
            arm_wall_sum = sum(int(arm["outer_wall_ns"]) for arm in arm_sequence)
            if (
                int(live_guard["coverage_started_ns"])
                > int(arm_sequence[0]["outer_started_ns"])
                or int(live_guard["coverage_ended_ns"])
                < int(arm_sequence[-1]["outer_ended_ns"])
                or int(live_guard["coverage_duration_ns"]) < arm_wall_sum
            ):
                fail("live interference coverage does not contain every arm")
            summary = build_summary(measured)
            if summary["decision"] != "PASS":
                fail(
                    "wrapper overhead exceeds 2%: "
                    f"paired={summary['median_paired_ratio']}, "
                    f"medians={summary['ratio_of_medians']}"
                )
            expected_stdout = measured[0]["direct"]["stdout"]  # type: ignore[index]
            for pair in [*warmups, *measured]:
                if pair["direct"]["stdout"] != expected_stdout or pair["wrapped"]["stdout"] != expected_stdout:  # type: ignore[index]
                    fail("calibration workload output was not deterministic")
            ending_revision = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=repo_root,
                check=True,
                env=ENVIRONMENT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            require_closed_lifecycle("repository revision closure")
            if ending_revision.stderr or ending_revision.stdout.strip() != revision:
                fail("repository revision changed during wrapper calibration")
            input_provenance = verify_input_closure(
                input_paths,
                starting_data,
                starting_identities,
                snapshot_paths,
                snapshot_identities,
                execution_fds,
                execution_identities,
                directory_bindings,
                launch_state_path,
                launch_state_fd,
                launch_data,
                launch_identity,
                output.parent,
                output_parent_fd,
                output_parent_identity,
            )
            document = {
                "affinity_cpus": AFFINITY_TEXT,
                "clock": "CLOCK_MONOTONIC",
                "controller": {
                    "schema": CONTROLLER_SCHEMA,
                    "sha256": starting_identities["controller"]["sha256"],
                    "uri": "repo://tools/wric_wrapper_calibration.py", "version": CONTROLLER_VERSION,
                },
                "environment": dict(ENVIRONMENT),
                "input_provenance": input_provenance,
                "live_guard": live_guard,
                "measured_pairs": measured,
                "physical_cores": physical_cores,
                "preflight": {
                    "duration_ns": preflight_duration,
                    "max_logical_cpu_busy_ppm": preflight_max,
                    "threshold_ppm": QUIET_THRESHOLD_PPM,
                },
                "repo_revision": revision,
                "rss_limit_bytes": RSS_LIMIT_BYTES,
                "runner": {
                    "process_metrics_schema_version": 2,
                    "sha256": starting_identities["runner"]["sha256"],
                    "uri": "repo://build/bin/wric-process-metrics",
                },
                "schema": SCHEMA,
                "schema_version": SCHEMA_VERSION,
                "summary": summary,
                "timeout_seconds": TIMEOUT_SECONDS,
                "warmup_pairs": warmups,
                "workload": {
                    "argv": list(SYMBOLIC_ARGV),
                    "schema": WORKLOAD_SCHEMA,
                    "sha256": starting_identities["workload"]["sha256"],
                    "source_sha256": starting_identities["workload_source"]["sha256"],
                    "source_uri": "repo://tools/wric_wrapper_calibration_workload.cpp",
                    "stderr_bytes": 0,
                    "stderr_sha256": EMPTY_SHA256,
                    "stdout_bytes": WORKLOAD_STDOUT_BYTES,
                    "stdout_sha256": WORKLOAD_STDOUT_SHA256,
                    "stdout_text": WORKLOAD_STDOUT_TEXT,
                    "uri": "repo://build/bin/wric-wrapper-calibration-workload",
                    "version": WORKLOAD_VERSION,
                    "version_text": WORKLOAD_VERSION_TEXT,
                },
            }
    if document is None:
        fail("calibration collection returned without a document")
    return document


def wait_for_probe_barrier(path: Path, label: str) -> None:
    deadline = time.monotonic_ns() + 10_000_000_000
    while not path.exists():
        if time.monotonic_ns() >= deadline:
            fail(f"timed out waiting for {label}")
        time.sleep(0.005)


def reexec_probe_stage(args: argparse.Namespace) -> None:
    """Test-only stager used to prove launch-time reads and FD-bound re-exec."""

    live = Path(__file__).absolute()
    stage = Path(args.stage).absolute()
    result = Path(args.result).absolute()
    write_exclusive_readonly(Path(args.ready).absolute(), b"ready\n")
    wait_for_probe_barrier(Path(args.go).absolute(), "probe launch barrier")
    controller_data, _ = read_stable_input(live, "probe launch-time controller")
    stage.mkdir(mode=0o700)
    snapshot = stage / "controller.py"
    identity = write_private_snapshot(
        snapshot, controller_data, 0o555, "probe private controller snapshot"
    )
    descriptor = open_bound_regular_file(
        snapshot, identity, "probe controller execution descriptor", inheritable=True
    )
    try:
        write_exclusive_readonly(Path(args.staged).absolute(), b"staged\n")
        wait_for_probe_barrier(Path(args.execute).absolute(), "probe execute barrier")
        os.execve(
            descriptor,
            [
                str(snapshot),
                "_reexec-probe-inner",
                "--controller-fd", str(descriptor),
                "--live", str(live),
                "--result", str(result),
                "--snapshot", str(snapshot),
                "--stage", str(stage),
            ],
            ENVIRONMENT,
        )
    finally:
        try:
            os.close(descriptor)
        except OSError:
            pass
        if os.path.lexists(stage):
            shutil.rmtree(stage)


def reexec_probe_inner(args: argparse.Namespace) -> None:
    descriptor = args.controller_fd
    stage = Path(args.stage).absolute()
    snapshot = Path(args.snapshot).absolute()
    live = Path(args.live).absolute()
    result = Path(args.result).absolute()
    if __file__ != f"/dev/fd/{descriptor}" or snapshot != stage / "controller.py":
        fail("probe controller was not loaded through its declared descriptor")
    try:
        execution_data, execution_identity = read_stable_fd(
            descriptor, "probe executing controller"
        )
        live_data, live_identity = read_stable_input(live, "probe live controller")
        snapshot_data, snapshot_identity = read_stable_input(
            snapshot, "probe controller snapshot"
        )
        require_fd_path_identity(
            descriptor, snapshot, execution_identity, "probe executing controller"
        )
        if (
            execution_data != live_data
            or execution_data != snapshot_data
            or execution_identity != snapshot_identity
        ):
            fail("probe execution/live/snapshot controller binding failed")
        record = {
            "executing_sha256": execution_identity["sha256"],
            "live_sha256": live_identity["sha256"],
            "snapshot_sha256": snapshot_identity["sha256"],
        }
    finally:
        os.close(descriptor)
        if os.path.lexists(stage):
            shutil.rmtree(stage)
    write_exclusive_readonly(result, canonical_json(record))


def run_reexec_probe_case(root: Path, mutation: str) -> None:
    source_data, _ = read_stable_input(
        Path(__file__).absolute(), "controller self-test source"
    )
    marker = b"\n# deterministic-live-controller-B-marker\n"
    live_a = source_data + b"\n# deterministic-loaded-controller-A-marker\n"
    live_b = source_data + marker
    replacement = source_data + b"\n# deterministic-mutated-controller-C-marker\n"
    case = root / f"reexec-{mutation}"
    case.mkdir(mode=0o700)
    live = case / "live-controller.py"
    next_live = case / "next-controller.py"
    write_private_snapshot(live, live_a, 0o555, "probe loaded controller A")
    write_private_snapshot(next_live, live_b, 0o555, "probe live controller B")
    barriers = {
        name: case / name for name in ("ready", "go", "staged", "execute")
    }
    result = case / "result.json"
    stage = case / "stage"
    process = subprocess.Popen(
        [
            str(live),
            "_reexec-probe-stage",
            "--execute", str(barriers["execute"]),
            "--go", str(barriers["go"]),
            "--ready", str(barriers["ready"]),
            "--result", str(result),
            "--stage", str(stage),
            "--staged", str(barriers["staged"]),
        ],
        env=ENVIRONMENT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    wait_for_probe_barrier(barriers["ready"], "loaded-controller barrier")
    os.replace(next_live, live)
    write_exclusive_readonly(barriers["go"], b"go\n")
    wait_for_probe_barrier(barriers["staged"], "staged-controller barrier")
    if mutation == "live":
        next_mutation = case / "mutated-live.py"
        write_private_snapshot(
            next_mutation, replacement, 0o555, "probe mutated live controller"
        )
        os.replace(next_mutation, live)
    elif mutation == "snapshot":
        next_mutation = case / "mutated-snapshot.py"
        write_private_snapshot(
            next_mutation, replacement, 0o555, "probe mutated controller snapshot"
        )
        os.replace(next_mutation, stage / "controller.py")
    elif mutation != "none":
        raise AssertionError(f"unknown re-exec probe mutation: {mutation}")
    write_exclusive_readonly(barriers["execute"], b"execute\n")
    stdout, stderr = process.communicate(timeout=10)
    if stdout:
        raise AssertionError(f"re-exec probe wrote stdout: {stdout!r}")
    expected_sha256 = sha256_bytes(live_b)
    if mutation == "none":
        if process.returncode != 0 or stderr:
            raise AssertionError(
                f"valid re-exec probe failed: rc={process.returncode}, stderr={stderr!r}"
            )
        record = json.loads(result.read_text(encoding="utf-8"))
        if record != {
            "executing_sha256": expected_sha256,
            "live_sha256": expected_sha256,
            "snapshot_sha256": expected_sha256,
        } or expected_sha256 == sha256_bytes(live_a):
            raise AssertionError("re-exec probe did not execute launch-time controller B")
    elif process.returncode == 0 or result.exists():
        raise AssertionError(f"post-stage {mutation} mutation was not rejected")
    if stage.exists():
        raise AssertionError("re-exec probe private stage survived")


def self_test(_: argparse.Namespace) -> None:
    assert decimal_text(median([Decimal(1), Decimal(3)])) == "2"
    assert decimal_text(median([Decimal(1), Decimal(2)])) == "1.5"
    pairs = [
        {"direct": {"outer_wall_ns": 100}, "wrapped": {"outer_wall_ns": 101}}
        for _ in range(11)
    ]
    summary = build_summary(pairs)
    assert summary == {
        "decision": "PASS", "direct_median_ns": "100",
        "median_paired_ratio": "1.010000000000",
        "ratio_of_medians": "1.010000000000",
        "threshold": "1.020000000000", "wrapped_median_ns": "101",
    }
    pairs[0] = {"direct": {"outer_wall_ns": 100}, "wrapped": {"outer_wall_ns": 200}}
    assert build_summary(pairs)["decision"] == "PASS"
    assert len(PROCESS_METRICS_V2_FIELDS) == 41
    assert WORKLOAD_STDOUT_BYTES == 137
    assert WORKLOAD_STDOUT_SHA256 == (
        "6c20ef62a67ad417071deb005e0a49bc4e7e806a18dd9578a1bc8c4ac32480b6"
    )
    assert len(WORKLOAD_STDOUT_TEXT.encode("ascii")) == WORKLOAD_STDOUT_BYTES
    assert hashlib.sha256(WORKLOAD_STDOUT_TEXT.encode("ascii")).hexdigest() == (
        WORKLOAD_STDOUT_SHA256
    )
    assert minimum_guard_scan_count(130_000_000_000, 1_000_000_000) == 131

    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    enable_child_subreaper()
    require_closed_lifecycle("controller self-test startup")
    with tempfile.TemporaryDirectory(
        prefix="wric-wrapper-lifecycle-self-test-"
    ) as temp:
        root = Path(temp)
        snapshot_dir = root / "snapshot-test"
        snapshot_dir.mkdir(mode=0o700)
        snapshot_path = snapshot_dir / "sealed-input"
        snapshot_identity = write_private_snapshot(
            snapshot_path, b"fixed calibration input\n", 0o555, "self-test snapshot"
        )
        require_stat_identity(snapshot_path, snapshot_identity, "self-test snapshot")
        try:
            write_private_snapshot(
                snapshot_path,
                b"replacement calibration input\n",
                0o555,
                "self-test replacement snapshot",
            )
        except FileExistsError:
            pass
        else:
            raise AssertionError("private input snapshot was overwritten")
        replaced_path = snapshot_dir / "descriptor-bound-input"
        replaced_identity = write_private_snapshot(
            replaced_path, b"descriptor-bound bytes\n", 0o555,
            "self-test descriptor-bound snapshot",
        )
        replaced_fd = open_bound_regular_file(
            replaced_path, replaced_identity,
            "self-test descriptor-bound snapshot", inheritable=False,
        )
        replacement_path = snapshot_dir / "descriptor-replacement"
        write_private_snapshot(
            replacement_path, b"different descriptor bytes\n", 0o555,
            "self-test descriptor replacement",
        )
        os.replace(replacement_path, replaced_path)
        try:
            require_fd_path_identity(
                replaced_fd, replaced_path, replaced_identity,
                "self-test replaced descriptor binding",
            )
        except CalibrationError:
            pass
        else:
            raise AssertionError("replaced execution path retained a forged FD binding")
        finally:
            os.close(replaced_fd)

        bound_directory = root / "bound-directory"
        bound_directory.mkdir(mode=0o700)
        write_private_snapshot(
            bound_directory / "child", b"unchanged child\n", 0o444,
            "self-test directory child",
        )
        bound_directory_fd = open_bound_directory(
            bound_directory, "self-test bound directory", inheritable=False
        )
        bound_directory_identity = read_directory_binding(
            bound_directory, bound_directory_fd, "self-test bound directory"
        )
        real_sleep = time.sleep
        real_sleep(0.002)
        away_directory = root / "away-directory"
        os.rename(bound_directory, away_directory)
        os.rename(away_directory, bound_directory)
        try:
            require_directory_binding(
                bound_directory, bound_directory_fd, bound_directory_identity,
                "self-test renamed directory",
            )
        except CalibrationError:
            pass
        else:
            raise AssertionError("rename-away/rename-back directory mutation was missed")
        finally:
            os.close(bound_directory_fd)

        for mutation in ("none", "live", "snapshot"):
            run_reexec_probe_case(root, mutation)

        command = [
            os.sys.executable,
            "-c",
            (
                "import os,time; child=os.fork(); "
                "(os.setsid(), time.sleep(30)) if child == 0 else "
                "(print('runner-ready', flush=True), time.sleep(30))"
            ),
        ]
        exception_stdout = root / "exception.stdout"
        python_fd = os.open(os.sys.executable, os.O_RDONLY)
        os.set_inheritable(python_fd, True)
        injected = False

        def inject_parent_exception(seconds: float) -> None:
            nonlocal injected
            if not injected:
                deadline = time.monotonic_ns() + 2_000_000_000
                while True:
                    try:
                        ready = exception_stdout.read_text(encoding="utf-8")
                    except FileNotFoundError:
                        ready = ""
                    if ready == "runner-ready\n":
                        break
                    if time.monotonic_ns() >= deadline:
                        raise AssertionError(
                            "injected-exception child never reached its fork barrier"
                        )
                    real_sleep(0.005)
                injected = True
                raise RuntimeError("deterministic injected parent exception")
            real_sleep(seconds)

        time.sleep = inject_parent_exception
        try:
            try:
                run_wait4(
                    python_fd,
                    command,
                    exception_stdout,
                    root / "exception.stderr",
                    30,
                )
            except RuntimeError as error:
                if str(error) != "deterministic injected parent exception":
                    raise
            else:
                raise AssertionError("injected parent exception did not fire")
        finally:
            time.sleep = real_sleep
        require_closed_lifecycle("injected-parent-exception lifecycle closure")
        if exception_stdout.read_text(encoding="utf-8") != "runner-ready\n":
            raise AssertionError("injected-exception regression missed its child")

        try:
            run_wait4(
                python_fd,
                command,
                root / "hang.stdout",
                root / "hang.stderr",
                0.5,
            )
        except CalibrationError as error:
            if "outer controller timeout" not in str(error):
                raise
        else:
            raise AssertionError("separate-session hanging child did not time out")
        require_closed_lifecycle("controller self-test completion")
        if (root / "hang.stdout").read_text(encoding="utf-8") != "runner-ready\n":
            raise AssertionError("hanging-runner regression never spawned its child")
        os.close(python_fd)
    print(
        "PASS: wrapper calibration controller arithmetic/schema/lifecycle self-test"
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)
    run_parser = subparsers.add_parser("run", help="run and emit one passing calibration")
    run_parser.add_argument("--output", required=True)
    run_parser.add_argument("--warmup-pairs", type=int, default=4)
    run_parser.add_argument("--measured-pairs", type=int, default=11)
    run_parser.set_defaults(function=run)
    self_parser = subparsers.add_parser("self-test", help="test pure controller invariants")
    self_parser.set_defaults(function=self_test)
    sealed_parser = subparsers.add_parser("_sealed-run", help=argparse.SUPPRESS)
    sealed_parser.add_argument("--controller-fd", type=int, required=True)
    sealed_parser.add_argument("--output-parent-fd", type=int, required=True)
    sealed_parser.add_argument("--stage-fd", type=int, required=True)
    sealed_parser.add_argument("--state-fd", type=int, required=True)
    sealed_parser.set_defaults(function=sealed_run)
    probe_stage_parser = subparsers.add_parser(
        "_reexec-probe-stage", help=argparse.SUPPRESS
    )
    for option in ("execute", "go", "ready", "result", "stage", "staged"):
        probe_stage_parser.add_argument(f"--{option}", required=True)
    probe_stage_parser.set_defaults(function=reexec_probe_stage)
    probe_inner_parser = subparsers.add_parser(
        "_reexec-probe-inner", help=argparse.SUPPRESS
    )
    probe_inner_parser.add_argument("--controller-fd", type=int, required=True)
    for option in ("live", "result", "snapshot", "stage"):
        probe_inner_parser.add_argument(f"--{option}", required=True)
    probe_inner_parser.set_defaults(function=reexec_probe_inner)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        args.function(args)
        return 0
    except (CalibrationError, OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
