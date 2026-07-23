#!/usr/bin/env python3
"""Run, seal, and later audit one provenance-bound WRIC benchmark capture.

The benchmark harness is intentionally invoked here rather than asking an
operator to seal an already-existing directory.  That lets this tool bind the
exact executable identities, repositories, build cache, affinity, environment,
manifest chain, and argv that existed on both sides of the measured command.
The completed directory is then closed with the generic v2 evidence ledger.

The detached ledger digest printed by ``capture`` is an external trust anchor.
``audit`` requires that digest plus the independently expected run label and
repository revisions.  It re-audits the byte closure and all live provenance;
metadata is never allowed to authenticate itself.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from types import ModuleType
from typing import Any, Mapping, NoReturn, Sequence


SCHEMA = "wric.benchmark_capture"
SCHEMA_VERSION = 2
METADATA_NAME = "wric-benchmark-run-metadata.json"
HARNESS_STDOUT_NAME = "wric-benchmark-harness.stdout"
HARNESS_STDERR_NAME = "wric-benchmark-harness.stderr"
PHASE9_INNER_NAME = "benchmark"
PHASE9_SEAL_STDOUT_NAME = "wric-phase9-seal.stdout"
PHASE9_SEAL_STDERR_NAME = "wric-phase9-seal.stderr"
PHASE9_AUDIT_STDOUT_NAME = "wric-phase9-audit.stdout"
PHASE9_AUDIT_STDERR_NAME = "wric-phase9-audit.stderr"
PHASE9_METADATA_NAME = "phase9-run-metadata.json"
PHASE9_LEDGER_NAME = "phase9-run-artifacts.tsv"
PHASE9_LEDGER_SEAL_NAME = PHASE9_LEDGER_NAME + ".sha256"
LEDGER_NAME = "wric-evidence-run-ledger.tsv"
LEDGER_SEAL_NAME = LEDGER_NAME + ".sha256"
COMPAT_AUDITOR_NAME = "wric_historical_harness_compat.py"

EXPECTED_COMPILER = Path("/home/ogi-agent/install/gcc-trunk/bin/g++-trunk")
EXPECTED_TOOLCHAIN = Path("/home/ogi-agent/install/gcc-trunk")
EXPECTED_COMPILER_LINK_COUNT = 4
EXPECTED_BUILD_TYPE = "RelWithDebInfo"
EXPECTED_RELWITHDEBINFO_FLAGS = "-O2 -g -DNDEBUG"
EXPECTED_EFFECTIVE_CXX_FLAGS = "-O2 -g -DNDEBUG -std=c++26 -freflection"
TIMED_TRIAL_DIGEST_FIX_REVISION = "3ac59125484790deed7fc21f0ef9572f0781164a"
SUMMARY_COMPAT_PRODUCT_REVISION = "a9db72e60f153a95362db544107373817a58a258"
SUMMARY_COMPAT_VARIANT = "timed-trial-current"
SUMMARY_COMPAT_HARNESS_SHA256 = (
    "ff7f7d2904752c7198f05e13d1eafffaa087ccba9ef70221c5084a91325ef821"
)
SUMMARY_COMPAT_TRACKED_HARNESS_SHA256 = (
    "ed089af213f7f6773a252908dc3111a4d86bf01c85f12ad9d3a846ec92b3770f"
)
BINARY_PROVENANCE_LIMIT = (
    "binary hashes bind the measured executables, but no reproducible-build "
    "attestation proves derivation from HEAD"
)
PHYSICAL_AFFINITY = "0,2,4,6,8,10,12,14"
SMT_AFFINITY = "0-15"
PHASE0_CAPTURE_METADATA_NAME = "capture-metadata.txt"
PHASE0_CALIBRATION_RELATIVE = "bootstrap-phase0/wrapper-calibration.json"
PHASE0_CALIBRATION_SCHEMA = "wric_process_wrapper_calibration"
PHASE0_CALIBRATION_SCHEMA_VERSION = 3
SYS_CPU_ROOT = Path("/sys/devices/system/cpu")
PROC_CPUINFO = Path("/proc/cpuinfo")
PHASE9_PYTHON_PREFIX = (
    sys.executable,
    "-E",
    "-s",
    "-S",
    "-B",
    "-X",
    "pycache_prefix=/dev/null",
)


def component(
    group: str,
    workers: str,
    repetitions: str,
    supplements: Sequence[str] = (),
    affinity: str = "P",
    extras: Sequence[str] = (),
) -> dict[str, object]:
    return {
        "affinity": affinity,
        "extras": list(extras),
        "group": group,
        "repetitions": repetitions,
        "supplements": list(supplements),
        "workers": workers,
    }


RUN_COMPONENTS: Mapping[str, Sequence[Mapping[str, object]]] = {
    "phase1": (
        component("p0-medium-dense-physical", "1", "5"),
        component("p0-small-dense-physical", "1", "5"),
        component("p0-small-exact1-physical", "1", "5"),
        component("p0-medium-cache-physical", "1", "5"),
        component("p0-medium-lazy-physical", "1", "5"),
    ),
    "phase2": (
        component("p0-medium-dense-physical", "1", "5"),
        component("p0-small-exact1-physical", "1", "5"),
    ),
    "phase3": (
        component("p0-small-dense-physical", "1,8", "5"),
        component("p0-medium-dense-physical", "1", "5"),
    ),
    "phase4": (
        component("p0-medium-dense-physical", "1,2,4,8", "5"),
        component("p0-medium-cache-physical", "1,2,4,8", "5"),
    ),
    "phase5": (component("p0-medium-exact1-physical", "1,8", "5"),),
    "phase6": (
        component("p0-medium-exact1-physical", "1,2,4,8", "3"),
        component("p0-primary-physical", "1,2,4,8", "3"),
        component("p0-stress-physical", "1,2,4,8", "3"),
    ),
    "phase7-high": (
        component("phase7-lazy", "1,8", "5", ("phase7-lazy",)),
    ),
    "phase7-small": (
        component("p0-small-dense-physical", "1,8", "5"),
        component(
            "phase7-lazy-small-on",
            "1,8",
            "5",
            ("phase7-lazy-completion",),
        ),
    ),
    "phase7-auto": (
        component("p0-medium-dense-physical", "1,8", "5"),
        component("p0-medium-lazy-physical", "1,8", "5"),
        component(
            "phase7-lazy-medium-auto",
            "1,8",
            "5",
            ("phase7-lazy-completion",),
        ),
        component("phase7-lazy", "1,8", "5", ("phase7-lazy",)),
    ),
    "phase8-generation": (
        component("phase8-generation", "1,8", "5", ("phase8-generation",)),
    ),
    "phase8-generation-retry1": (
        component("phase8-generation", "1,8", "5", ("phase8-generation",)),
    ),
    "phase8-end-to-end": (
        component("phase8-generation", "1,8", "5", ("phase8-generation",)),
    ),
    "final-phase6-exact1": (
        component("p0-medium-exact1-physical", "1,2,4,8", "3"),
    ),
    "final-scaling": (component("p0-primary-physical", "1,2,4,8", "3"),),
    "final-primary": (
        component(
            "p0-primary-physical",
            "1,8",
            "5",
            extras=(
                "--require-wall-ratio",
                "chart_spr_grammar_exact@8=1.0",
            ),
        ),
    ),
    "final-smt": (
        component("p0-primary-smt", "1,2,4,8,16,auto", "3", affinity="S"),
    ),
    "final-small-auto": (
        component("p0-small-auto", "1,auto", "5", affinity="U"),
    ),
    "final-unpinned-auto": (
        component(
            "p0-primary-smt",
            "auto",
            "5",
            affinity="U",
            extras=(
                "--require-wall-ratio",
                "chart_spr_grammar_exact@auto=1.0",
            ),
        ),
    ),
    "final-default-auto": (
        component(
            "p0-primary-smt",
            "default,auto",
            "5",
            affinity="U",
            extras=(
                "--require-wall-ratio",
                "chart_spr_grammar_exact@default=1.0",
                "--require-worker-policy",
                "default=automatic_default",
            ),
        ),
    ),
    "final-stress": (
        component("p0-stress-physical", "1,2,4,8", "3"),
    ),
    "final-real": (component("real-bounded", "1,8", "3"),),
    "phase9": (
        component(
            "phase9-local-commit",
            "1,8",
            "3",
            ("phase9-local-commit",),
        ),
    ),
}

HISTORICAL_RUN_REVISIONS: Mapping[str, str] = {
    "phase1": "208ce23f0c005d3702d114f535fe21564b3b79b6",
    "phase2": "0c4623ba1793395ae8f5c3df2a2524a27d89bc80",
    "phase5": "3a10e9cc45050f7a6f846f9f1adb8d5f4157f9a5",
    "phase6": "870c298ff1c0c21901bdf79d341bf97d121f389c",
    "phase8-end-to-end": "38e9a281396e5263647ba68724414848841525d7",
    "phase8-generation": "94a63238d25a8e3262428419d53f8f0986e8879b",
    "phase8-generation-retry1": "07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c",
}

# Later optimizations are allowed to discharge still-open earlier-phase
# performance gates, but every such retry must measure the same immutable
# current product as final acceptance.  The original Phase-3/4/7 captures stay
# preserved under their earlier capture-tool revisions; this revision admits
# only the additive Q retries below.
CURRENT_PRODUCT_REVISION = "a9db72e60f153a95362db544107373817a58a258"
CURRENT_PRODUCT_RUN_REVISIONS: Mapping[str, str] = {
    "phase3": CURRENT_PRODUCT_REVISION,
    "phase4": CURRENT_PRODUCT_REVISION,
    "phase7-high": CURRENT_PRODUCT_REVISION,
    "phase7-small": CURRENT_PRODUCT_REVISION,
    "phase7-auto": CURRENT_PRODUCT_REVISION,
    "final-phase6-exact1": CURRENT_PRODUCT_REVISION,
    "final-scaling": CURRENT_PRODUCT_REVISION,
    "final-primary": CURRENT_PRODUCT_REVISION,
    "final-smt": CURRENT_PRODUCT_REVISION,
    "final-small-auto": CURRENT_PRODUCT_REVISION,
    "final-unpinned-auto": CURRENT_PRODUCT_REVISION,
    "final-stress": CURRENT_PRODUCT_REVISION,
    "final-real": CURRENT_PRODUCT_REVISION,
    "phase9": CURRENT_PRODUCT_REVISION,
}
DEFERRED_DEFAULT_RUN_LABEL = "final-default-auto"
# The paired default/explicit-auto evidence must come from the later, separately
# pinned default-promotion product.  Until that immutable revision exists, fail
# closed instead of treating the absent mapping as permission to use any HEAD.

SAFE_HARNESS_ENVIRONMENT = {
    "HOME": "/nonexistent",
    "LANG": "C",
    "LC_ALL": "C",
    "PATH": "/usr/bin:/bin",
    "TMPDIR": "/tmp",
    "TZ": "Europe/Sofia",
}
BENCHMARK_ENVIRONMENT_EXACT = frozenset(
    {
        "ASAN_OPTIONS",
        "BLIS_NUM_THREADS",
        "CPUPROFILE",
        "DAGUTIL",
        "GLIBC_TUNABLES",
        "GOMP_CPU_AFFINITY",
        "HEAPPROFILE",
        "KMP_AFFINITY",
        "KMP_HW_SUBSET",
        "LARCH2",
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "LSAN_OPTIONS",
        "MALLOC_ARENA_MAX",
        "MKL_NUM_THREADS",
        "NUMEXPR_NUM_THREADS",
        "OMP_DYNAMIC",
        "OMP_NUM_THREADS",
        "OPENBLAS_NUM_THREADS",
        "RAYON_NUM_THREADS",
        "TBB_NUM_THREADS",
        "TSAN_OPTIONS",
        "UBSAN_OPTIONS",
        "VECLIB_MAXIMUM_THREADS",
        "WRIC_PROCESS_METRICS",
        "WRIC_REPO_ROOT",
    }
)
BENCHMARK_ENVIRONMENT_PREFIXES = (
    "WRIC_SPR_SEARCH_",
    "OMP_",
    "OPENBLAS_",
    "MKL_",
    "KMP_",
    "MALLOC_",
)

SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
REVISION_RE = re.compile(r"[0-9a-f]{40,64}\Z")
RUN_LABEL_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
SAFE_COMPONENT_RE = re.compile(r"[A-Za-z0-9_.-]+\Z")
CMAKE_ENTRY_RE = re.compile(r"([A-Za-z0-9_.+-]+):([A-Za-z]+)=(.*)\Z")
MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_ARTIFACT_LEDGER_BYTES = 64 * 1024 * 1024
MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
CHUNK_SIZE = 1024 * 1024

FILE_FLAGS = (
    os.O_RDONLY
    | getattr(os, "O_NOFOLLOW", 0)
    | getattr(os, "O_CLOEXEC", 0)
)
DIRECTORY_FLAGS = (
    os.O_RDONLY
    | getattr(os, "O_DIRECTORY", 0)
    | getattr(os, "O_NOFOLLOW", 0)
    | getattr(os, "O_CLOEXEC", 0)
)

GIT_CONFIG_OVERRIDES = (
    "core.fsmonitor=false",
    "core.untrackedCache=false",
    "core.filemode=true",
    "core.trustctime=true",
    "core.checkStat=default",
    "core.ignoreStat=false",
)

TOP_LEVEL_KEYS = frozenset(
    {
        "affinity",
        "binary_provenance_limit",
        "capture_dir",
        "capture_outputs",
        "cmake_contract",
        "compiler_version",
        "dagutil_flags",
        "environment",
        "effective_build_commands",
        "harness_argv",
        "harness_argv_sha256",
        "harness_configuration",
        "harness_execution",
        "harness_provenance",
        "host_contract",
        "tracked_git_blobs",
        "phase0_chain",
        "postprocessor",
        "repositories",
        "run_label",
        "schema",
        "schema_version",
        "tools",
        "umask",
        "working_directory",
    }
)
TOOL_KEYS = frozenset(
    {
        "benchmark_harness",
        "capture_wrapper",
        "cmake_cache",
        "compiler",
        "dagutil_compile_recipes",
        "dagutil_compile_flags",
        "dagutil_link_command",
        "frozen_larch2",
        "frozen_oracle_dagutil",
        "frozen_process_metrics",
        "generic_ledger",
        "larch_compile_recipes",
        "larch_compile_flags",
        "larch_link_command",
        "product_dagutil",
    }
)
NONEXECUTABLE_TOOL_ROLES = frozenset(
    {
        "cmake_cache",
        "dagutil_compile_recipes",
        "dagutil_compile_flags",
        "dagutil_link_command",
        "larch_compile_recipes",
        "larch_compile_flags",
        "larch_link_command",
    }
)
FILE_SNAPSHOT_KEYS = frozenset(
    {
        "bytes",
        "ctime_ns",
        "device",
        "inode",
        "mode",
        "mtime_ns",
        "nlink",
        "path",
        "sha256",
    }
)
REPOSITORY_KEYS = frozenset({"capture_tool", "product"})
REPOSITORY_RECORD_KEYS = frozenset({"path", "expected_revision", "pre", "post"})
REPOSITORY_STATE_KEYS = frozenset(
    {
        "head",
        "object_format",
        "porcelain_v2_z_base64",
        "porcelain_v2_z_bytes",
        "porcelain_v2_z_sha256",
        "tracked_files_v_z_bytes",
        "tracked_files_v_z_sha256",
        "tracked_worktree_bytes",
        "tracked_worktree_files",
        "tracked_worktree_observation_sha256",
        "toplevel",
    }
)
TRACKED_BLOB_KEYS = frozenset(
    {"capture_wrapper", "generic_ledger", "product_harness"}
)
TRACKED_BLOB_RECORD_KEYS = frozenset(
    {
        "blob_bytes",
        "blob_sha256",
        "mode",
        "object_id",
        "relative",
        "repository",
        "working_file",
    }
)
OBSERVATION_KEYS = frozenset(
    {
        "argv",
        "returncode",
        "stderr_bytes",
        "stderr_sha256",
        "stdout_bytes",
        "stdout_sha256",
    }
)


class CaptureError(RuntimeError):
    """A stable, user-facing provenance or capture failure."""


def fail(message: str) -> NoReturn:
    raise CaptureError(message)


def require_run_revision(run_label: str, product_revision: str) -> None:
    if run_label == DEFERRED_DEFAULT_RUN_LABEL:
        fail(
            f"{run_label} is disabled until the default-promotion product "
            "revision is committed and pinned"
        )
    historical = HISTORICAL_RUN_REVISIONS.get(run_label)
    if historical is not None and product_revision != historical:
        fail(
            f"{run_label} requires exact historical product revision "
            f"{historical}, not {product_revision}"
        )
    current = CURRENT_PRODUCT_RUN_REVISIONS.get(run_label)
    if current is not None and product_revision != current:
        fail(
            f"{run_label} requires exact current-product revision "
            f"{current}, not {product_revision}"
        )
    if (
        historical is None
        and current is None
        and run_label in RUN_COMPONENTS
    ):
        fail(
            f"{run_label} has no pinned product revision"
        )


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def stable_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def directory_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def canonical_existing_path(value: str | Path, label: str) -> Path:
    text = os.fspath(value)
    if not os.path.isabs(text) or os.path.abspath(text) != text:
        fail(f"{label} is not an absolute, lexically canonical path: {text!r}")
    path = Path(text)
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        fail(f"{label} is missing: {path}: {error.strerror or error}")
    if resolved != path:
        fail(f"{label} contains a symlink or lexical alias: {path}")
    return path


def canonical_directory(value: str | Path, label: str) -> Path:
    path = canonical_existing_path(value, label)
    try:
        info = path.lstat()
    except OSError as error:
        fail(f"cannot inspect {label}: {error.strerror or error}")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        fail(f"{label} is not an exact non-symlink directory: {path}")
    return path


def canonical_absent_capture(value: str | Path) -> tuple[Path, Path, tuple[int, ...]]:
    text = os.fspath(value)
    if not os.path.isabs(text) or os.path.abspath(text) != text:
        fail(f"capture directory is not an absolute, lexically canonical path: {text!r}")
    path = Path(text)
    if SAFE_COMPONENT_RE.fullmatch(path.name) is None or path.name in (".", ".."):
        fail("capture directory has an unsafe final path component")
    parent = canonical_directory(path.parent, "capture parent directory")
    if os.path.lexists(path):
        fail(f"capture directory already exists: {path}")
    return path, parent, directory_signature(parent.stat(follow_symlinks=False))


def require_capture_parent_unchanged(
    parent: Path, expected: tuple[int, ...], *, allow_mtime_change: bool
) -> None:
    try:
        current = directory_signature(parent.lstat())
    except OSError as error:
        fail(f"capture parent directory changed: {error.strerror or error}")
    if allow_mtime_change:
        # Creating the capture changes only the parent's mtime/ctime.  Its
        # identity, kind, and permission bits must remain exact.  The link
        # count normally increments because the harness creates one child
        # directory and therefore cannot be part of the invariant.
        stable_fields = (0, 1, 2)
        if any(current[index] != expected[index] for index in stable_fields):
            fail("capture parent directory identity changed during the run")
    elif current != expected:
        fail("capture parent directory changed before the harness was launched")


def snapshot_directory(path: Path, label: str) -> dict[str, object]:
    directory = canonical_directory(path, label)
    info = directory.lstat()
    return {
        "device": info.st_dev,
        "inode": info.st_ino,
        "mode": stat.S_IMODE(info.st_mode),
        "nlink": info.st_nlink,
        "path": os.fspath(directory),
    }


def inspect_file(
    value: str | Path,
    label: str,
    *,
    executable: bool,
    expected_nlink: int = 1,
    collect_payload: bool,
    maximum_bytes: int | None = None,
) -> tuple[dict[str, object], bytes | None]:
    path = canonical_existing_path(value, label)
    descriptor: int | None = None
    try:
        lexical = path.lstat()
        if stat.S_ISLNK(lexical.st_mode) or not stat.S_ISREG(lexical.st_mode):
            fail(f"{label} is not an exact regular file: {path}")
        if lexical.st_nlink != expected_nlink:
            fail(
                f"{label} has link count {lexical.st_nlink}; expected "
                f"{expected_nlink}: {path}"
            )
        mode = stat.S_IMODE(lexical.st_mode)
        if executable and mode & 0o111 == 0:
            fail(f"{label} is not executable: {path}")
        descriptor = os.open(path, FILE_FLAGS)
        before = os.fstat(descriptor)
        if stable_signature(before) != stable_signature(lexical):
            fail(f"{label} changed while opening: {path}")
        digest = hashlib.sha256()
        total = 0
        chunks: list[bytes] | None = [] if collect_payload else None
        while True:
            block = os.read(descriptor, CHUNK_SIZE)
            if not block:
                break
            digest.update(block)
            total += len(block)
            if maximum_bytes is not None and total > maximum_bytes:
                fail(f"{label} exceeds the maximum supported size")
            if chunks is not None:
                chunks.append(block)
        after = os.fstat(descriptor)
        if stable_signature(after) != stable_signature(before) or total != after.st_size:
            fail(f"{label} changed while hashing: {path}")
        lexical_after = path.lstat()
        if stable_signature(lexical_after) != stable_signature(after):
            fail(f"{label} path identity changed while hashing: {path}")
        snapshot = {
            "bytes": total,
            "ctime_ns": after.st_ctime_ns,
            "device": after.st_dev,
            "inode": after.st_ino,
            "mode": stat.S_IMODE(after.st_mode),
            "mtime_ns": after.st_mtime_ns,
            "nlink": after.st_nlink,
            "path": os.fspath(path),
            "sha256": digest.hexdigest(),
        }
        return snapshot, (b"".join(chunks) if chunks is not None else None)
    except CaptureError:
        raise
    except OSError as error:
        fail(f"cannot snapshot {label}: {path}: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)


def snapshot_file(
    value: str | Path,
    label: str,
    *,
    executable: bool,
    expected_nlink: int = 1,
) -> dict[str, object]:
    snapshot, _ = inspect_file(
        value,
        label,
        executable=executable,
        expected_nlink=expected_nlink,
        collect_payload=False,
    )
    return snapshot


def read_snapshotted_file(
    value: str | Path,
    label: str,
    *,
    executable: bool = False,
    expected_nlink: int = 1,
    maximum_bytes: int | None = None,
) -> tuple[dict[str, object], bytes]:
    snapshot, payload = inspect_file(
        value,
        label,
        executable=executable,
        expected_nlink=expected_nlink,
        collect_payload=True,
        maximum_bytes=maximum_bytes,
    )
    assert payload is not None
    return snapshot, payload


def require_exact_keys(value: Mapping[str, object], keys: frozenset[str], label: str) -> None:
    if set(value) != keys:
        fail(
            f"{label} has the wrong key set: missing={sorted(keys - set(value))}, "
            f"extra={sorted(set(value) - keys)}"
        )


def require_snapshot_shape(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, dict):
        fail(f"{label} is not an object")
    require_exact_keys(value, FILE_SNAPSHOT_KEYS, label)
    if not isinstance(value["path"], str) or not isinstance(value["sha256"], str):
        fail(f"{label} has invalid path or SHA-256 fields")
    if SHA256_RE.fullmatch(value["sha256"]) is None:
        fail(f"{label} SHA-256 is not canonical")
    for key in FILE_SNAPSHOT_KEYS - {"path", "sha256"}:
        if isinstance(value[key], bool) or not isinstance(value[key], int) or value[key] < 0:
            fail(f"{label} {key} is not a nonnegative integer")
    return value


def require_snapshot_unchanged(
    expected: Mapping[str, object], label: str, *, executable: bool
) -> dict[str, object]:
    expected = require_snapshot_shape(expected, label)
    expected_nlink = expected["nlink"]
    assert isinstance(expected_nlink, int)
    observed = snapshot_file(
        str(expected["path"]),
        label,
        executable=executable,
        expected_nlink=expected_nlink,
    )
    if observed != dict(expected):
        fail(f"{label} identity or bytes changed")
    return observed


def git_run(root: Path, arguments: Sequence[str], label: str) -> bytes:
    environment = {
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    try:
        completed = subprocess.run(
            [
                "/usr/bin/git",
                *(
                    token
                    for setting in GIT_CONFIG_OVERRIDES
                    for token in ("-c", setting)
                ),
                "-C",
                os.fspath(root),
                *arguments,
            ],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
    except OSError as error:
        fail(f"cannot execute git for {label}: {error.strerror or error}")
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout)[-4096:].decode(
            "utf-8", errors="backslashreplace"
        )
        fail(f"git {label} failed: {detail.strip()}")
    return completed.stdout


def git_text(root: Path, arguments: Sequence[str], label: str) -> str:
    payload = git_run(root, arguments, label)
    try:
        return payload.decode("ascii")
    except UnicodeDecodeError:
        fail(f"git {label} output is not ASCII")


def git_is_ancestor(root: Path, ancestor: str, descendant: str) -> bool:
    environment = {
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    completed = subprocess.run(
        [
            "/usr/bin/git",
            *(
                token
                for setting in GIT_CONFIG_OVERRIDES
                for token in ("-c", setting)
            ),
            "-C",
            os.fspath(root),
            "merge-base",
            "--is-ancestor",
            ancestor,
            descendant,
        ],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    if completed.returncode == 0 and not completed.stdout and not completed.stderr:
        return True
    if completed.returncode == 1 and not completed.stdout and not completed.stderr:
        return False
    if completed.returncode == 128 and (
        b"Not a valid commit name" in completed.stderr
        or b"Not a valid object name" in completed.stderr
    ):
        # A repository that does not contain the reviewed fix object cannot
        # prove an exact tracked harness is safe.  The caller rejects this
        # branch and requires the externally audited compatibility artifact.
        return False
    detail = (completed.stderr or completed.stdout).decode(
        "utf-8", errors="backslashreplace"
    )
    fail(f"git score-domain ancestry check failed: {detail.strip()}")


def require_timed_trial_digest_fix(
    product_root: Path, product_revision: str, harness: Path
) -> None:
    if not git_is_ancestor(
        product_root, TIMED_TRIAL_DIGEST_FIX_REVISION, product_revision
    ):
        fail(
            "pre-timed-trial-digest product revisions require an externally anchored "
            "materialized compatibility harness"
        )
    _, payload = read_snapshotted_file(
        harness,
        "tracked product harness score-domain source",
        executable=True,
        maximum_bytes=MAX_JSON_BYTES,
    )
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"cannot inspect tracked product harness score-domain fix: {error}")
    required_fragments = (
        'reported=${field[${raw_index[best_reported_objective]}]}',
        '[[ "$method" != sample_explore_merge ]] || reported=$final',
        '[[ "$expected_final" == - || "$reported" == "$expected_final" ]]',
        'reported_initial=$(awk',
        '"$reported_initial" "$initial" >"$curve"',
        '"${MANIFEST_TIMED_WORKER_ARGS[@]}" "${canonical_args[@]}" -o "$pb")',
        '"${commit_args[@]}" --seed "$seed" "${canonical_args[@]}" -o "$pb")',
        'DEFERRED_SEARCH_SHA[$result_key]=$timed_search',
        'search_sha=${DEFERRED_SEARCH_SHA[$result_key]:--}',
        '"$search_sha" != "${canonical_companion_search[$row_id]:--}"',
    )
    for fragment in required_fragments:
        if text.count(fragment) != 1:
            fail(
                "tracked product harness does not retain the exact timed-trial "
                f"fix fragment: {fragment!r}"
            )


def validate_git_toplevel(value: str | Path, label: str) -> Path:
    root = canonical_directory(value, label)
    top_text = git_text(root, ("rev-parse", "--show-toplevel"), label).strip()
    top = canonical_directory(top_text, f"Git {label} toplevel")
    if top != root:
        fail(f"{label} is not the exact Git toplevel: {root} != {top}")
    return root


def parse_head_tree_records(
    payload: bytes, object_format: str, label: str
) -> list[tuple[bytes, str, str]]:
    object_id_bytes = 40 if object_format == "sha1" else 64
    result: list[tuple[bytes, str, str]] = []
    paths: set[bytes] = set()
    for raw_record in payload.split(b"\0"):
        if not raw_record:
            continue
        try:
            prefix, relative = raw_record.split(b"\t", 1)
            mode_bytes, kind, object_id = prefix.split(b" ")
            mode = mode_bytes.decode("ascii")
            object_id_text = object_id.decode("ascii")
        except (UnicodeDecodeError, ValueError):
            fail(f"{label} HEAD tree contains a malformed record")
        if (
            mode not in ("100644", "100755")
            or kind != b"blob"
            or len(object_id_text) != object_id_bytes
            or re.fullmatch(r"[0-9a-f]+", object_id_text) is None
            or not relative
            or relative in paths
        ):
            fail(f"{label} HEAD tree contains an unsupported or duplicate entry")
        paths.add(relative)
        result.append((relative, mode, object_id_text))
    return result


def parse_index_stage_records(
    payload: bytes, object_format: str, label: str
) -> list[tuple[bytes, str, str]]:
    object_id_bytes = 40 if object_format == "sha1" else 64
    result: list[tuple[bytes, str, str]] = []
    paths: set[bytes] = set()
    for raw_record in payload.split(b"\0"):
        if not raw_record:
            continue
        try:
            prefix, relative = raw_record.split(b"\t", 1)
            mode_bytes, object_id, stage = prefix.split(b" ")
            mode = mode_bytes.decode("ascii")
            object_id_text = object_id.decode("ascii")
        except (UnicodeDecodeError, ValueError):
            fail(f"{label} index contains a malformed stage record")
        if (
            mode not in ("100644", "100755")
            or stage != b"0"
            or len(object_id_text) != object_id_bytes
            or re.fullmatch(r"[0-9a-f]+", object_id_text) is None
            or not relative
            or relative in paths
        ):
            fail(f"{label} index contains an unsupported or duplicate stage entry")
        paths.add(relative)
        result.append((relative, mode, object_id_text))
    return result


def canonical_tracked_path(root: Path, relative_bytes: bytes, label: str) -> Path:
    relative = os.fsdecode(relative_bytes)
    pure = PurePosixPath(relative)
    if (
        not relative
        or pure.is_absolute()
        or pure.as_posix() != relative
        or any(component in ("", ".", "..") for component in pure.parts)
    ):
        fail(f"{label} contains an unsafe tracked path")
    path = canonical_existing_path(
        root / Path(*pure.parts), f"{label} tracked file {relative!r}"
    )
    try:
        path.relative_to(root)
    except ValueError:
        fail(f"{label} tracked file escapes its repository: {relative!r}")
    return path


def raw_tracked_worktree_observation(
    root: Path, object_format: str, label: str
) -> dict[str, object]:
    if object_format not in ("sha1", "sha256"):
        fail(f"{label} uses an unsupported Git object format: {object_format!r}")
    tree_payload = git_run(
        root,
        ("ls-tree", "-r", "-z", "--full-tree", "HEAD"),
        f"{label} recursive HEAD tree",
    )
    index_payload = git_run(
        root,
        ("ls-files", "--stage", "-z"),
        f"{label} stage-0 index",
    )
    tree = parse_head_tree_records(tree_payload, object_format, label)
    index = parse_index_stage_records(index_payload, object_format, label)
    if tree != index:
        fail(f"{label} index paths, modes, or object IDs differ from HEAD")

    observations: list[dict[str, object]] = []
    total_bytes = 0
    for relative_bytes, git_mode, expected_object_id in tree:
        path = canonical_tracked_path(root, relative_bytes, label)
        snapshot, payload = read_snapshotted_file(
            path,
            f"{label} tracked file {os.fsdecode(relative_bytes)!r}",
            executable=git_mode == "100755",
        )
        expected_mode = 0o755 if git_mode == "100755" else 0o644
        if snapshot["mode"] != expected_mode:
            fail(
                f"{label} tracked file mode differs from the exact HEAD checkout "
                f"contract: {os.fsdecode(relative_bytes)!r}"
            )
        constructor = hashlib.new(object_format)
        constructor.update(f"blob {len(payload)}\0".encode("ascii"))
        constructor.update(payload)
        observed_object_id = constructor.hexdigest()
        if observed_object_id != expected_object_id:
            fail(
                f"{label} tracked file raw working bytes differ from HEAD: "
                f"{os.fsdecode(relative_bytes)!r}"
            )
        observations.append(
            {
                "git_mode": git_mode,
                "object_id": expected_object_id,
                "path_base64": base64.b64encode(relative_bytes).decode("ascii"),
                "snapshot": snapshot,
            }
        )
        total_bytes += len(payload)

    if (
        git_run(
            root,
            ("ls-tree", "-r", "-z", "--full-tree", "HEAD"),
            f"{label} final recursive HEAD tree",
        )
        != tree_payload
        or git_run(
            root,
            ("ls-files", "--stage", "-z"),
            f"{label} final stage-0 index",
        )
        != index_payload
    ):
        fail(f"{label} HEAD tree or index changed during raw verification")
    return {
        "tracked_worktree_bytes": total_bytes,
        "tracked_worktree_files": len(observations),
        "tracked_worktree_observation_sha256": sha256_bytes(
            canonical_json_bytes(observations)
        ),
    }


def repository_state(
    root: Path,
    expected_revision: str,
    label: str,
    *,
    require_clean: bool,
) -> dict[str, object]:
    if REVISION_RE.fullmatch(expected_revision) is None:
        fail(f"{label} expected revision is not a full canonical object ID")
    root = validate_git_toplevel(root, label)
    head = git_text(root, ("rev-parse", "--verify", "HEAD^{commit}"), f"{label} HEAD").strip()
    if head != expected_revision:
        fail(f"{label} HEAD differs from expected revision: {head} != {expected_revision}")
    object_format = git_text(
        root, ("rev-parse", "--show-object-format"), f"{label} object format"
    ).strip()
    if object_format not in ("sha1", "sha256"):
        fail(f"{label} uses an unsupported Git object format: {object_format!r}")
    porcelain = git_run(
        root,
        ("status", "--porcelain=v2", "-z", "--untracked-files=all"),
        f"{label} porcelain-v2 clean-status",
    )
    if require_clean and porcelain:
        first = porcelain.split(b"\0", 1)[0].decode("utf-8", errors="backslashreplace")
        fail(f"{label} is not completely clean (first porcelain-v2 record: {first})")
    tracked_files = git_run(
        root,
        ("ls-files", "-v", "-f", "-z"),
        f"{label} tracked-file flags",
    )
    for record in tracked_files.split(b"\0"):
        if not record:
            continue
        if len(record) < 3 or record[1:2] != b" " or record[:1] != b"H":
            detail = record[:256].decode("utf-8", errors="backslashreplace")
            fail(
                f"{label} has a forbidden tracked-file index flag "
                f"(assume-unchanged/skip-worktree or non-stage-0 state): {detail}"
            )
    tracked_worktree = raw_tracked_worktree_observation(
        root, object_format, label
    )
    final_head = git_text(
        root, ("rev-parse", "--verify", "HEAD^{commit}"), f"final {label} HEAD"
    ).strip()
    final_porcelain = git_run(
        root,
        ("status", "--porcelain=v2", "-z", "--untracked-files=all"),
        f"final {label} porcelain-v2 clean-status",
    )
    if final_head != head or final_porcelain != porcelain:
        fail(f"{label} HEAD or clean status changed during raw verification")
    return {
        "head": head,
        "object_format": object_format,
        "porcelain_v2_z_base64": base64.b64encode(porcelain).decode("ascii"),
        "porcelain_v2_z_bytes": len(porcelain),
        "porcelain_v2_z_sha256": sha256_bytes(porcelain),
        "tracked_files_v_z_bytes": len(tracked_files),
        "tracked_files_v_z_sha256": sha256_bytes(tracked_files),
        "toplevel": os.fspath(root),
    } | tracked_worktree


def require_tracked_file(root: Path, path: Path, label: str) -> None:
    try:
        relative = path.relative_to(root).as_posix()
    except ValueError:
        fail(f"{label} is outside its capture-tool repository: {path}")
    output = git_run(
        root,
        ("ls-files", "--error-unmatch", "--", relative),
        f"tracked {label}",
    )
    if output != (relative + "\n").encode("utf-8"):
        fail(f"{label} is not one exact tracked repository path: {relative}")


def tracked_git_blob(root: Path, path: Path, label: str) -> dict[str, object]:
    require_tracked_file(root, path, label)
    relative = path.relative_to(root).as_posix()
    index_output = git_text(
        root,
        ("ls-files", "--stage", "--", relative),
        f"{label} index blob",
    )
    index_match = re.fullmatch(
        r"(100755) ([0-9a-f]{40,64}) 0\t([^\n]+)\n", index_output
    )
    if index_match is None or index_match.group(3) != relative:
        fail(f"{label} does not have one exact stage-0 executable Git blob")
    tree_output = git_run(
        root,
        ("ls-tree", "-z", "HEAD", "--", relative),
        f"{label} HEAD blob",
    )
    tree_match = re.fullmatch(
        rb"(100755) blob ([0-9a-f]{40,64})\t([^\0]+)\0", tree_output
    )
    if tree_match is None:
        fail(f"{label} is not one exact executable blob at HEAD")
    try:
        tree_object_id = tree_match.group(2).decode("ascii")
        tree_relative = tree_match.group(3).decode("utf-8")
    except (UnicodeDecodeError, IndexError):
        fail(f"{label} HEAD blob record is not canonical text")
    if tree_relative != relative:
        fail(f"{label} HEAD blob path differs from its lexical repository path")
    if (
        index_match.group(1) != "100755"
        or index_match.group(2) != tree_object_id
    ):
        fail(f"{label} index blob differs from HEAD")
    blob = git_run(root, ("cat-file", "blob", tree_object_id), f"{label} HEAD blob bytes")
    working = snapshot_file(path, label, executable=True, expected_nlink=1)
    if working["bytes"] != len(blob) or working["sha256"] != sha256_bytes(blob):
        fail(f"{label} working bytes differ from the exact HEAD blob")
    return {
        "blob_bytes": len(blob),
        "blob_sha256": sha256_bytes(blob),
        "mode": index_match.group(1),
        "object_id": tree_object_id,
        "relative": relative,
        "repository": os.fspath(root),
        "working_file": working,
    }


def validate_tracked_git_blob_record(
    value: object, label: str
) -> Mapping[str, object]:
    if not isinstance(value, dict):
        fail(f"{label} is not an object")
    require_exact_keys(value, TRACKED_BLOB_RECORD_KEYS, label)
    if (
        value["mode"] != "100755"
        or not isinstance(value["object_id"], str)
        or REVISION_RE.fullmatch(value["object_id"]) is None
        or not isinstance(value["relative"], str)
        or not isinstance(value["repository"], str)
        or isinstance(value["blob_bytes"], bool)
        or not isinstance(value["blob_bytes"], int)
        or value["blob_bytes"] < 0
        or not isinstance(value["blob_sha256"], str)
        or SHA256_RE.fullmatch(value["blob_sha256"]) is None
    ):
        fail(f"{label} fields are invalid")
    working = require_snapshot_shape(value["working_file"], f"{label} working file")
    if (
        working["bytes"] != value["blob_bytes"]
        or working["sha256"] != value["blob_sha256"]
    ):
        fail(f"{label} working snapshot differs from its HEAD blob bytes")
    return value


def load_python_module(
    path: Path, role: str, expected_snapshot: Mapping[str, object]
) -> ModuleType:
    expected = require_snapshot_shape(expected_snapshot, role)
    observed, payload = read_snapshotted_file(
        path,
        role,
        executable=True,
        expected_nlink=1,
        maximum_bytes=MAX_JSON_BYTES,
    )
    if observed != dict(expected):
        fail(f"{role} differs from its already-proved HEAD snapshot before execution")
    name = f"_wric_{path.stem}_{os.getpid()}_{id(path)}"
    module = ModuleType(name)
    module.__file__ = os.fspath(path)
    sys.modules[name] = module
    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        source = payload.decode("utf-8")
        code = compile(source, os.fspath(path), "exec", dont_inherit=True)
        exec(code, module.__dict__)
    except UnicodeDecodeError as error:
        fail(f"cannot load {role}: source is not UTF-8: {error}")
    except Exception as error:
        fail(f"cannot load {role}: {error}")
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode
    return module


def canonical_affinity(cpus: set[int]) -> str:
    if not cpus or any(cpu < 0 for cpu in cpus):
        fail("process affinity is empty or invalid")
    values = sorted(cpus)
    ranges: list[str] = []
    start = previous = values[0]
    for cpu in values[1:]:
        if cpu == previous + 1:
            previous = cpu
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = cpu
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(ranges)


def current_affinity() -> str:
    try:
        return canonical_affinity(set(os.sched_getaffinity(0)))
    except OSError as error:
        fail(f"cannot read process affinity: {error.strerror or error}")


def require_affinity(value: str) -> str:
    live = current_affinity()
    if value != live:
        fail(f"requested affinity is not the exact live canonical CPU list: {value!r} != {live!r}")
    return live


def parse_cpu_list(value: str, label: str) -> set[int]:
    if not value or re.fullmatch(r"[0-9]+(?:-[0-9]+)?(?:,[0-9]+(?:-[0-9]+)?)*", value) is None:
        fail(f"{label} is not a canonical CPU list: {value!r}")
    result: set[int] = set()
    for item in value.split(","):
        fields = item.split("-", 1)
        start = int(fields[0])
        end = int(fields[-1])
        if end < start:
            fail(f"{label} has a descending CPU range: {item!r}")
        for cpu in range(start, end + 1):
            if cpu in result:
                fail(f"{label} duplicates CPU {cpu}")
            result.add(cpu)
    if canonical_affinity(result) != value:
        fail(f"{label} is not the unique canonical CPU-list spelling")
    return result


def read_virtual_ascii(path: Path, label: str, maximum_bytes: int = 1024 * 1024) -> str:
    descriptor: int | None = None
    try:
        lexical = path.lstat()
        if stat.S_ISLNK(lexical.st_mode) or not stat.S_ISREG(lexical.st_mode):
            fail(f"{label} is not an exact regular virtual file: {path}")
        descriptor = os.open(path, FILE_FLAGS)
        before = os.fstat(descriptor)
        if (
            before.st_dev != lexical.st_dev
            or before.st_ino != lexical.st_ino
            or before.st_mode != lexical.st_mode
        ):
            fail(f"{label} changed while opening")
        chunks: list[bytes] = []
        total = 0
        while True:
            block = os.read(descriptor, 64 * 1024)
            if not block:
                break
            total += len(block)
            if total > maximum_bytes:
                fail(f"{label} exceeds the maximum supported size")
            chunks.append(block)
        after = os.fstat(descriptor)
        if (
            after.st_dev != before.st_dev
            or after.st_ino != before.st_ino
            or after.st_mode != before.st_mode
        ):
            fail(f"{label} changed while reading")
        return b"".join(chunks).decode("ascii")
    except CaptureError:
        raise
    except (OSError, UnicodeDecodeError) as error:
        fail(f"cannot read {label}: {error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)


def host_topology_observation() -> dict[str, object]:
    physical = parse_cpu_list(PHYSICAL_AFFINITY, "physical affinity contract")
    smt = parse_cpu_list(SMT_AFFINITY, "SMT affinity contract")
    online_text = read_virtual_ascii(SYS_CPU_ROOT / "online", "online CPU list").strip()
    online = parse_cpu_list(online_text, "online CPU list")
    if online != smt:
        fail(
            "live online CPUs differ from the sealed-host SMT contract: "
            f"{canonical_affinity(online)} != {SMT_AFFINITY}"
        )
    cpuinfo = read_virtual_ascii(PROC_CPUINFO, "CPU information")
    model_names = {
        line.split(":", 1)[1].strip()
        for line in cpuinfo.splitlines()
        if line.startswith("model name") and ":" in line
    }
    if len(model_names) != 1 or not next(iter(model_names), ""):
        fail("live CPU information does not contain one exact nonempty model name")
    model_name = next(iter(model_names))
    logical: list[dict[str, int | str]] = []
    identities: dict[tuple[int, int], set[int]] = {}
    for cpu in sorted(smt):
        topology = SYS_CPU_ROOT / f"cpu{cpu}" / "topology"
        try:
            package_id = int(
                read_virtual_ascii(
                    topology / "physical_package_id", f"CPU {cpu} package ID"
                ).strip()
            )
            core_id = int(
                read_virtual_ascii(topology / "core_id", f"CPU {cpu} core ID").strip()
            )
        except ValueError:
            fail(f"CPU {cpu} topology contains a non-integer package/core ID")
        siblings_text = read_virtual_ascii(
            topology / "thread_siblings_list", f"CPU {cpu} thread siblings"
        ).strip()
        siblings = parse_cpu_list(siblings_text, f"CPU {cpu} thread siblings")
        if cpu not in siblings or not siblings <= smt:
            fail(f"CPU {cpu} has thread siblings outside the sealed-host SMT set")
        identity = (package_id, core_id)
        identities.setdefault(identity, set()).add(cpu)
        logical.append(
            {
                "core_id": core_id,
                "cpu": cpu,
                "package_id": package_id,
                "thread_siblings": canonical_affinity(siblings),
            }
        )
    if len(identities) != 8 or any(len(cpus) != 2 for cpus in identities.values()):
        fail("sealed-host topology is not exactly eight two-thread physical cores")
    for record in logical:
        identity = (int(record["package_id"]), int(record["core_id"]))
        if record["thread_siblings"] != canonical_affinity(identities[identity]):
            fail(f"CPU {record['cpu']} thread-sibling topology is inconsistent")
    physical_identities = {
        (int(record["package_id"]), int(record["core_id"]))
        for record in logical
        if int(record["cpu"]) in physical
    }
    if len(physical) != 8 or physical_identities != set(identities):
        fail("physical affinity does not select one CPU from every distinct core")
    packages = {int(record["package_id"]) for record in logical}
    if len(packages) != 1:
        fail("sealed-host topology is not exactly one CPU package")
    topology_text = "1 socket; 8 cores/socket; 2 threads/core; 16 logical CPUs"
    return {
        "cpu_model": model_name,
        "cpu_topology": topology_text,
        "logical_cpus": logical,
        "online_cpus": canonical_affinity(online),
        "physical_affinity": PHYSICAL_AFFINITY,
        "smt_affinity": SMT_AFFINITY,
    }


def parse_capture_metadata_host(path: Path) -> dict[str, str]:
    _, payload = read_snapshotted_file(
        path, "Phase-0 capture metadata", maximum_bytes=MAX_JSON_BYTES
    )
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        fail("Phase-0 capture metadata is not UTF-8")
    wanted = {
        "cpu_model",
        "cpu_topology",
        "online_cpus",
        "physical_core_cpu_list",
    }
    result: dict[str, str] = {}
    for line in text.splitlines():
        if ": " not in line:
            continue
        key, value = line.split(": ", 1)
        if key not in wanted:
            continue
        if key in result or not value:
            fail(f"Phase-0 capture metadata has duplicate/empty host field {key}")
        result[key] = value
    if set(result) != wanted:
        fail(
            "Phase-0 capture metadata lacks its exact host fields: "
            f"{sorted(wanted - set(result))}"
        )
    return result


def phase0_host_contract(
    phase0_root: Path,
    base_path: Path,
    artifact_entries: Mapping[str, str],
) -> dict[str, object]:
    capture_metadata = canonical_existing_path(
        base_path.parent / PHASE0_CAPTURE_METADATA_NAME,
        "Phase-0 capture metadata",
    )
    calibration = canonical_existing_path(
        base_path.parent / PHASE0_CALIBRATION_RELATIVE,
        "Phase-0 wrapper calibration",
    )
    for path, label in (
        (capture_metadata, "Phase-0 capture metadata"),
        (calibration, "Phase-0 wrapper calibration"),
    ):
        recorded = artifact_digest_for(artifact_entries, phase0_root, path, label)
        observed = snapshot_file(path, label, executable=False)["sha256"]
        if recorded != observed:
            fail(f"{label} differs from the Phase-0 artifact ledger")
    metadata_host = parse_capture_metadata_host(capture_metadata)
    _, calibration_payload = read_snapshotted_file(
        calibration, "Phase-0 wrapper calibration", maximum_bytes=MAX_JSON_BYTES
    )
    calibration_json = strict_json_bytes(
        calibration_payload, "Phase-0 wrapper calibration"
    )
    if (
        calibration_json.get("schema") != PHASE0_CALIBRATION_SCHEMA
        or calibration_json.get("schema_version") != PHASE0_CALIBRATION_SCHEMA_VERSION
        or calibration_json.get("affinity_cpus") != PHYSICAL_AFFINITY
    ):
        fail("Phase-0 wrapper calibration host/schema contract is invalid")
    summary = calibration_json.get("summary")
    if not isinstance(summary, dict) or summary.get("decision") != "PASS":
        fail("Phase-0 wrapper calibration did not record PASS")
    live = host_topology_observation()
    logical_value = live["logical_cpus"]
    if not isinstance(logical_value, list):
        fail("live logical CPU topology is not a list")
    physical = parse_cpu_list(PHYSICAL_AFFINITY, "physical affinity")
    expected_cores: list[dict[str, object]] = []
    for record_value in logical_value:
        if not isinstance(record_value, dict):
            fail("live logical CPU topology contains a non-object record")
        cpu = record_value.get("cpu")
        if not isinstance(cpu, int):
            fail("live logical CPU topology contains an invalid CPU ID")
        if cpu in physical:
            expected_cores.append(
                {
                    "core_id": record_value.get("core_id"),
                    "cpu": cpu,
                    "package_id": record_value.get("package_id"),
                }
            )
    if calibration_json.get("physical_cores") != expected_cores:
        fail("Phase-0 wrapper calibration physical cores differ from live topology")
    expected_metadata = {
        "cpu_model": live["cpu_model"],
        "cpu_topology": live["cpu_topology"],
        "online_cpus": live["online_cpus"],
        "physical_core_cpu_list": PHYSICAL_AFFINITY,
    }
    if metadata_host != expected_metadata:
        fail("Phase-0 capture metadata host identity differs from live topology")
    return {
        "calibration_sha256": artifact_digest_for(
            artifact_entries, phase0_root, calibration, "Phase-0 wrapper calibration"
        ),
        "capture_metadata_sha256": artifact_digest_for(
            artifact_entries, phase0_root, capture_metadata, "Phase-0 capture metadata"
        ),
        "live": live,
    }


def reject_inherited_benchmark_environment(environment: Mapping[str, str]) -> None:
    offending = sorted(
        name
        for name in environment
        if name in BENCHMARK_ENVIRONMENT_EXACT
        or any(name.startswith(prefix) for prefix in BENCHMARK_ENVIRONMENT_PREFIXES)
    )
    if offending:
        fail(
            "inherited benchmark-affecting environment is forbidden; invoke with "
            f"those variables unset: {offending}"
        )


def harness_environment(phase0_root: Path) -> dict[str, str]:
    return SAFE_HARNESS_ENVIRONMENT | {"WRIC_REPO_ROOT": os.fspath(phase0_root)}


def command_observation(
    argv: Sequence[str], environment: Mapping[str, str], label: str
) -> dict[str, object]:
    try:
        completed = subprocess.run(
            list(argv),
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=dict(environment),
        )
    except OSError as error:
        fail(f"cannot execute {label}: {error.strerror or error}")
    if len(completed.stdout) > MAX_COMMAND_OUTPUT_BYTES or len(completed.stderr) > MAX_COMMAND_OUTPUT_BYTES:
        fail(f"{label} output exceeds the maximum supported size")
    if completed.returncode != 0:
        fail(f"{label} failed with status {completed.returncode}")
    return {
        "argv": list(argv),
        "returncode": completed.returncode,
        "stderr_bytes": len(completed.stderr),
        "stderr_sha256": sha256_bytes(completed.stderr),
        "stdout_bytes": len(completed.stdout),
        "stdout_sha256": sha256_bytes(completed.stdout),
    }


def run_captured_command(
    argv: Sequence[str],
    environment: Mapping[str, str],
    working_directory: Path,
    label: str,
) -> tuple[int, bytes, bytes]:
    try:
        completed = subprocess.run(
            list(argv),
            check=False,
            cwd=working_directory,
            env=dict(environment),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        fail(f"cannot execute {label}: {error.strerror or error}")
    if (
        len(completed.stdout) > MAX_COMMAND_OUTPUT_BYTES
        or len(completed.stderr) > MAX_COMMAND_OUTPUT_BYTES
    ):
        fail(f"{label} console output exceeds the maximum supported size")
    return completed.returncode, completed.stdout, completed.stderr


def strict_json_bytes(payload: bytes, label: str) -> dict[str, object]:
    def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in items:
            if key in result:
                fail(f"{label} duplicates JSON key: {key}")
            result[key] = value
        return result

    def constant(value: str) -> NoReturn:
        fail(f"{label} contains non-finite JSON number: {value}")

    try:
        value = json.loads(payload, object_pairs_hook=pairs, parse_constant=constant)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        fail(f"{label} is invalid JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{label} root is not an object")
    return value


def stream_observation(payload: bytes) -> dict[str, object]:
    return {"bytes": len(payload), "sha256": sha256_bytes(payload)}


def compiler_version_observation(
    compiler: Path, environment: Mapping[str, str]
) -> dict[str, object]:
    argv = [os.fspath(compiler), "--version"]
    try:
        completed = subprocess.run(
            argv,
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=dict(environment),
        )
    except OSError as error:
        fail(f"cannot execute configured compiler --version: {error.strerror or error}")
    if completed.returncode != 0:
        fail(f"configured compiler --version failed with status {completed.returncode}")
    if len(completed.stdout) > MAX_COMMAND_OUTPUT_BYTES or len(completed.stderr) > MAX_COMMAND_OUTPUT_BYTES:
        fail("configured compiler --version output exceeds the maximum supported size")
    try:
        stdout_text = completed.stdout.decode("utf-8")
        stderr_text = completed.stderr.decode("utf-8")
    except UnicodeDecodeError:
        fail("configured compiler --version output is not UTF-8")
    if "g++-trunk (GCC)" not in stdout_text:
        fail("configured compiler --version does not identify gcc-trunk")
    return {
        "argv": argv,
        "returncode": completed.returncode,
        "stderr_bytes": len(completed.stderr),
        "stderr_sha256": sha256_bytes(completed.stderr),
        "stdout_text": stdout_text,
        "stderr_text": stderr_text,
        "stdout_bytes": len(completed.stdout),
        "stdout_sha256": sha256_bytes(completed.stdout),
    }


def parse_cmake_cache(path: Path, product_root: Path) -> dict[str, str]:
    snapshot, payload = read_snapshotted_file(
        path, "CMake cache", maximum_bytes=MAX_JSON_BYTES
    )
    cache_bytes = snapshot["bytes"]
    if not isinstance(cache_bytes, int) or cache_bytes > MAX_JSON_BYTES:
        fail("CMake cache exceeds the maximum supported size")
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"cannot read CMake cache as UTF-8: {error}")
    if not payload.endswith(b"\n") or b"\x00" in payload or b"\r" in payload:
        fail("CMake cache is not canonical newline-delimited text")
    entries: dict[str, tuple[str, str]] = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        match = CMAKE_ENTRY_RE.fullmatch(line)
        if match is None:
            fail(f"CMake cache line {number} is malformed")
        key, kind, value = match.groups()
        if key in entries:
            fail(f"CMake cache duplicates key: {key}")
        entries[key] = (kind, value)
    required = {
        "CMAKE_BUILD_TYPE": ("STRING", EXPECTED_BUILD_TYPE),
        "CMAKE_CXX_FLAGS": ("STRING", ""),
        "CMAKE_CXX_FLAGS_RELWITHDEBINFO": ("STRING", EXPECTED_RELWITHDEBINFO_FLAGS),
        "CMAKE_EXE_LINKER_FLAGS": ("STRING", ""),
        "CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO": ("STRING", ""),
        "CMAKE_SHARED_LINKER_FLAGS": ("STRING", ""),
        "CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO": ("STRING", ""),
        "CMAKE_STATIC_LINKER_FLAGS": ("STRING", ""),
        "CMAKE_STATIC_LINKER_FLAGS_RELWITHDEBINFO": ("STRING", ""),
        "CMAKE_HOME_DIRECTORY": ("INTERNAL", os.fspath(product_root)),
        "ENABLE_ASAN": ("BOOL", "OFF"),
        "ENABLE_TSAN": ("BOOL", "OFF"),
        "ENABLE_VULKAN": ("BOOL", "OFF"),
    }
    for key, expected in required.items():
        if entries.get(key) != expected:
            fail(f"CMake cache {key} is not the exact required value: {entries.get(key)!r}")
    compiler_entry = entries.get("CMAKE_CXX_COMPILER")
    if compiler_entry not in {
        ("FILEPATH", os.fspath(EXPECTED_COMPILER)),
        ("STRING", os.fspath(EXPECTED_COMPILER)),
    }:
        fail(
            "CMake cache CMAKE_CXX_COMPILER is not the closed exact STRING/FILEPATH "
            f"contract: {compiler_entry!r}"
        )
    toolchain_entry = entries.get("GCC_TOOLCHAIN")
    if toolchain_entry not in {
        ("PATH", ""),
        ("PATH", os.fspath(EXPECTED_TOOLCHAIN)),
    }:
        fail(
            "CMake cache GCC_TOOLCHAIN is not the closed empty/exact-path "
            f"contract: {toolchain_entry!r}"
        )
    forbidden_nonempty = (
        "CMAKE_CXX_COMPILER_LAUNCHER",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO",
    )
    for key in forbidden_nonempty:
        entry = entries.get(key)
        if entry is not None and entry[1] not in ("", "OFF"):
            fail(f"CMake cache {key} enables a forbidden build override: {entry!r}")
    assert compiler_entry is not None
    return (
        {key: value for key, (_, value) in required.items()}
        | {
            "CMAKE_CXX_COMPILER": compiler_entry[1],
            "CMAKE_CXX_COMPILER_KIND": compiler_entry[0],
            "GCC_TOOLCHAIN": toolchain_entry[1],
        }
    )


def target_compile_sources(
    product_root: Path, target: str
) -> list[tuple[str, Path]]:
    if target == "dagutil":
        return [
            (
                "CMakeFiles/dagutil.dir/tools/dagutil.cpp.o",
                product_root / "tools/dagutil.cpp",
            )
        ]
    if target != "larch":
        fail(f"unsupported effective compile-recipe target: {target}")
    relatives = [
        "src/protobuf.cpp",
        "src/protobuf_encode.cpp",
        "src/pickle_reader.cpp",
    ]
    if (product_root / "src/chart_scheduler.cpp").is_file():
        relatives.append("src/chart_scheduler.cpp")
    relatives.extend(
        ["src/chart_spr_search.cpp", "src/chart_bnb_trim_apply.cpp"]
    )
    return [
        (f"CMakeFiles/larch.dir/{relative}.o", product_root / relative)
        for relative in relatives
    ]


def validate_target_compile_recipes(
    path: Path, product_root: Path, target: str
) -> str:
    role = f"{target} compile recipes"
    _, payload = read_snapshotted_file(
        path, role, maximum_bytes=1024 * 1024
    )
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        fail(f"{role} are not UTF-8")
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        fail(f"{role} are not canonical generated Make text")
    sources = target_compile_sources(product_root, target)
    target_prefix = re.escape(f"CMakeFiles/{target}.dir/")
    declared_objects = re.findall(
        rf"^({target_prefix}[^\n:]+\.cpp\.o): "
        rf"CMakeFiles/{re.escape(target)}\.dir/flags\.make$",
        text,
        flags=re.MULTILINE,
    )
    expected_objects = [object_path for object_path, _ in sources]
    if declared_objects != expected_objects:
        fail(
            f"{role} declare a non-exact source/object sequence: "
            f"{declared_objects!r}"
        )
    expected_recipe_lines: list[str] = []
    for progress, (object_path, source_path) in enumerate(sources, 1):
        dependency = (
            f"{object_path}: CMakeFiles/{target}.dir/flags.make\n"
            f"{object_path}: {source_path}\n"
            f"{object_path}: CMakeFiles/{target}.dir/compiler_depend.ts\n"
        )
        echo = (
            "\t@$(CMAKE_COMMAND) -E cmake_echo_color \"--switch=$(COLOR)\" "
            f"--green --progress-dir={product_root}/build/CMakeFiles "
            f"--progress-num=$(CMAKE_PROGRESS_{progress}) "
            f'"Building CXX object {object_path}"\n'
        )
        compile_command = (
            f"\t{EXPECTED_COMPILER} $(CXX_DEFINES) $(CXX_INCLUDES) "
            f"$(CXX_FLAGS) -MD -MT {object_path} -MF {object_path}.d "
            f"-o {object_path} -c {source_path}\n\n"
        )
        fragment = dependency + echo + compile_command
        if text.count(fragment) != 1:
            fail(
                f"{role} do not contain one exact launcher-free recipe for "
                f"{object_path}"
            )
        intermediate = object_path.removesuffix(".o")
        expected_recipe_lines.extend(
            [
                echo.rstrip("\n"),
                compile_command.splitlines()[0],
                (
                    "\t@$(CMAKE_COMMAND) -E cmake_echo_color "
                    '"--switch=$(COLOR)" --green '
                    f'"Preprocessing CXX source to {intermediate}.i"'
                ),
                (
                    f"\t{EXPECTED_COMPILER} $(CXX_DEFINES) $(CXX_INCLUDES) "
                    f"$(CXX_FLAGS) -E {source_path} > {intermediate}.i"
                ),
                (
                    "\t@$(CMAKE_COMMAND) -E cmake_echo_color "
                    '"--switch=$(COLOR)" --green '
                    f'"Compiling CXX source to assembly {intermediate}.s"'
                ),
                (
                    f"\t{EXPECTED_COMPILER} $(CXX_DEFINES) $(CXX_INCLUDES) "
                    f"$(CXX_FLAGS) -S {source_path} -o {intermediate}.s"
                ),
            ]
        )
    link_progress = len(sources) + 1
    link_description = (
        "Linking CXX executable bin/dagutil"
        if target == "dagutil"
        else "Linking CXX static library liblarch.a"
    )
    expected_recipe_lines.append(
        "\t@$(CMAKE_COMMAND) -E cmake_echo_color \"--switch=$(COLOR)\" "
        f"--green --bold --progress-dir={product_root}/build/CMakeFiles "
        f"--progress-num=$(CMAKE_PROGRESS_{link_progress}) "
        f'"{link_description}"'
    )
    if target == "larch":
        expected_recipe_lines.append(
            "\t$(CMAKE_COMMAND) -P CMakeFiles/larch.dir/cmake_clean_target.cmake"
        )
    expected_recipe_lines.extend(
        [
            f"\t$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/{target}.dir/link.txt --verbose=$(VERBOSE)",
            f"\t$(CMAKE_COMMAND) -P CMakeFiles/{target}.dir/cmake_clean.cmake",
            (
                f"\tcd {product_root}/build && $(CMAKE_COMMAND) -E cmake_depends "
                f'"Unix Makefiles" {product_root} {product_root} '
                f"{product_root}/build {product_root}/build "
                f"{product_root}/build/CMakeFiles/{target}.dir/DependInfo.cmake "
                f'"--color=$(COLOR)" {target}'
            ),
        ]
    )
    actual_recipe_lines = [line for line in text.splitlines() if line.startswith("\t")]
    if actual_recipe_lines != expected_recipe_lines:
        fail(
            f"{role} contain an extra, missing, reordered, or modified target recipe"
        )
    return sha256_bytes(payload)


def validate_effective_build_commands(
    tool_paths: Mapping[str, Path], product_root: Path
) -> dict[str, str]:
    expected_includes = (
        f"-I{product_root / 'include'} -I{product_root / 'build/generated'}"
    )
    result: dict[str, str] = {}
    for target in ("dagutil", "larch"):
        role = f"{target}_compile_recipes"
        result[role] = validate_target_compile_recipes(
            tool_paths[role], product_root, target
        )
    for role in ("dagutil_compile_flags", "larch_compile_flags"):
        _, payload = read_snapshotted_file(
            tool_paths[role], role.replace("_", " "), maximum_bytes=64 * 1024
        )
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError:
            fail(f"{role.replace('_', ' ')} is not UTF-8")
        expected_text = (
            "# CMAKE generated file: DO NOT EDIT!\n"
            '# Generated by "Unix Makefiles" Generator, CMake Version 4.3\n'
            "\n"
            f"# compile CXX with {EXPECTED_COMPILER}\n"
            "CXX_DEFINES = \n"
            "\n"
            f"CXX_INCLUDES = {expected_includes}\n"
            "\n"
            f"CXX_FLAGS = {EXPECTED_EFFECTIVE_CXX_FLAGS}\n"
            "\n"
        )
        if text != expected_text:
            fail(
                f"{role.replace('_', ' ')} is not the exact generated "
                "RelWithDebInfo C++26 compile contract"
            )
        result[role] = sha256_bytes(payload)
    for role in ("dagutil_link_command", "larch_link_command"):
        _, payload = read_snapshotted_file(
            tool_paths[role], role.replace("_", " "), maximum_bytes=64 * 1024
        )
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError:
            fail(f"{role.replace('_', ' ')} is not UTF-8")
        if not text.endswith("\n") or "\r" in text or "\x00" in text:
            fail(f"{role.replace('_', ' ')} is not canonical command text")
        if role == "dagutil_link_command":
            lines = text.splitlines()
            if len(lines) != 1:
                fail("dagutil link command is not exactly one command")
            tokens = shlex.split(lines[0], posix=True)
            expected_tokens = [
                os.fspath(EXPECTED_COMPILER),
                "-O2",
                "-g",
                "-DNDEBUG",
                "-static-libstdc++",
                "-static-libgcc",
                "-Wl,--dependency-file=CMakeFiles/dagutil.dir/link.d",
                "CMakeFiles/dagutil.dir/tools/dagutil.cpp.o",
                "-o",
                "bin/dagutil",
                "liblarch.a",
                "/usr/lib/libz.so",
            ]
            if tokens != expected_tokens:
                fail(
                    "dagutil link command token sequence is not the exact "
                    f"RelWithDebInfo target contract: {tokens!r}"
                )
        else:
            lines = text.splitlines()
            if len(lines) != 2:
                fail("larch link command is not exactly two archive commands")
            archive = shlex.split(lines[0], posix=True)
            ranlib = shlex.split(lines[1], posix=True)
            archive_tool = archive[0] if archive else ""
            ranlib_tool = ranlib[0] if ranlib else ""
            if (archive_tool, ranlib_tool) not in {
                ("/bin/ar", "/bin/ranlib"),
                ("/usr/bin/ar", "/usr/bin/ranlib"),
            }:
                fail("larch link command does not use one closed system archive pair")
            objects = [
                object_path
                for object_path, _ in target_compile_sources(product_root, "larch")
            ]
            if archive != [archive_tool, "qc", "liblarch.a", *objects]:
                fail("larch archive command object sequence is not exact")
            if ranlib != [ranlib_tool, "liblarch.a"]:
                fail("larch ranlib command token sequence is not exact")
        result[role] = sha256_bytes(payload)
    return result


def required_option(arguments: Sequence[str], option: str) -> str:
    indexes = [index for index, token in enumerate(arguments) if token == option]
    if len(indexes) != 1:
        fail(f"harness argv must contain {option} exactly once as a separate token")
    index = indexes[0]
    if index + 1 >= len(arguments) or arguments[index + 1].startswith("--"):
        fail(f"harness argv {option} lacks one exact value token")
    if any(token.startswith(option + "=") for token in arguments):
        fail(f"harness argv must not use the {option}=VALUE alias")
    return arguments[index + 1]


def repeated_option(arguments: Sequence[str], option: str) -> list[str]:
    if any(token.startswith(option + "=") for token in arguments):
        fail(f"harness argv must not use the {option}=VALUE alias")
    values: list[str] = []
    for index, token in enumerate(arguments):
        if token != option:
            continue
        if index + 1 >= len(arguments) or arguments[index + 1].startswith("--"):
            fail(f"harness argv {option} lacks one exact value token")
        values.append(arguments[index + 1])
    return values


def canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def argv_digest(argv: Sequence[str]) -> str:
    return sha256_bytes(canonical_json_bytes(list(argv)))


def parse_manifest_preamble(path: Path, label: str) -> tuple[dict[str, str], bytes]:
    _, payload = read_snapshotted_file(
        path, label, maximum_bytes=MAX_JSON_BYTES
    )
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        fail(f"{label} is not canonical newline-delimited UTF-8")
    try:
        lines = payload.decode("utf-8").splitlines()
    except UnicodeDecodeError:
        fail(f"{label} is not UTF-8")
    preamble: dict[str, str] = {}
    for line in lines:
        if not line.startswith("# "):
            break
        body = line[2:]
        if "=" not in body:
            fail(f"{label} has a malformed preamble line")
        key, value = body.split("=", 1)
        if not key or not value or key in preamble:
            fail(f"{label} has an empty or duplicate preamble field: {key!r}")
        preamble[key] = value
    required = {
        "schema",
        "schema_version",
        "kind",
        "manifest_id",
        "parent_sha256",
        "repo_revision",
        "merge_base",
        "frozen_larch2_uri",
        "frozen_larch2_sha256",
        "frozen_oracle_dagutil_uri",
        "frozen_oracle_dagutil_sha256",
        "commands_uri",
        "commands_sha256",
    }
    if set(preamble) != required:
        fail(
            f"{label} preamble key set is not exact: missing={sorted(required - set(preamble))}, "
            f"extra={sorted(set(preamble) - required)}"
        )
    if preamble["schema"] != "wric_chart_parallelization_workloads" or preamble["schema_version"] != "1":
        fail(f"{label} has an unsupported schema")
    for key in ("frozen_larch2_sha256", "frozen_oracle_dagutil_sha256", "commands_sha256"):
        if SHA256_RE.fullmatch(preamble[key]) is None:
            fail(f"{label} {key} is not canonical SHA-256")
    for key in ("repo_revision", "merge_base"):
        if REVISION_RE.fullmatch(preamble[key]) is None:
            fail(f"{label} {key} is not a full canonical revision")
    return preamble, payload


def require_sealed_file_mode(
    snapshot: Mapping[str, object], label: str
) -> None:
    # Legacy Phase-0 evidence was finalized in owner-writable worktrees.  Its
    # exact mode is still bound into each repeated snapshot, while write access
    # by group/other (and therefore a wider mutation surface) is forbidden.
    mode = snapshot.get("mode")
    if not isinstance(mode, int) or isinstance(mode, bool):
        fail(f"{label} snapshot lacks a valid permission mode")
    if mode & 0o022:
        fail(f"{label} is group/other-writable: mode {mode:04o}")


def verify_detached_sha(path: Path, expected_sha256: str, label: str) -> dict[str, object]:
    if SHA256_RE.fullmatch(expected_sha256) is None:
        fail(f"expected {label} SHA-256 is not canonical")
    manifest, _ = read_snapshotted_file(path, label)
    require_sealed_file_mode(manifest, label)
    if manifest["sha256"] != expected_sha256:
        fail(f"{label} differs from its external SHA-256 anchor")
    seal_path = canonical_existing_path(os.fspath(path) + ".sha256", f"{label} detached seal")
    seal, seal_payload = read_snapshotted_file(
        seal_path, f"{label} detached seal", maximum_bytes=1024
    )
    require_sealed_file_mode(seal, f"{label} detached seal")
    expected = f"{expected_sha256}  {path.name}\n".encode("ascii")
    if seal_payload != expected:
        fail(f"{label} detached seal is not exact GNU sha256sum format")
    return {"file": manifest, "seal": seal}


def repo_uri_path(root: Path, uri: str, label: str) -> Path:
    if not uri.startswith("repo://"):
        fail(f"{label} URI is not repo:// confined: {uri!r}")
    relative = uri.removeprefix("repo://")
    pure = PurePosixPath(relative)
    if not relative or pure.is_absolute() or pure.as_posix() != relative or ".." in pure.parts:
        fail(f"{label} URI is not normalized: {uri!r}")
    path = canonical_existing_path(root / Path(*pure.parts), label)
    try:
        path.relative_to(root)
    except ValueError:
        fail(f"{label} escapes the Phase-0 base root")
    return path


def read_artifact_ledger(
    phase0_root: Path,
    path: Path,
    expected_sha256: str,
) -> tuple[dict[str, str], dict[str, object], dict[str, object]]:
    files = verify_detached_sha(path, expected_sha256, "Phase-0 artifact ledger")
    try:
        ledger_snapshot, payload = read_snapshotted_file(
            path,
            "Phase-0 artifact ledger",
            maximum_bytes=MAX_ARTIFACT_LEDGER_BYTES,
        )
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"cannot read Phase-0 artifact ledger: {error}")
    if ledger_snapshot != files["file"]:
        fail("Phase-0 artifact ledger changed between verification and parsing")
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        fail("Phase-0 artifact ledger is not canonical newline-delimited UTF-8")
    reader = csv.reader(text.splitlines(), dialect="excel-tab")
    rows = list(reader)
    if not rows or rows[0] != ["sha256", "uri"]:
        fail("Phase-0 artifact ledger header is invalid")
    entries: dict[str, str] = {}
    order: list[str] = []
    identities: set[tuple[int, int]] = set()
    observations: list[dict[str, object]] = []
    ledger_seal = path.with_name(path.name + ".sha256")
    for number, row in enumerate(rows[1:], 2):
        if len(row) != 2 or SHA256_RE.fullmatch(row[0]) is None:
            fail(f"Phase-0 artifact ledger row {number} is malformed")
        digest, uri = row
        if uri in entries:
            fail(f"Phase-0 artifact ledger duplicates URI: {uri}")
        member = repo_uri_path(
            phase0_root, uri, f"Phase-0 artifact row {number}"
        )
        if member in (path, ledger_seal):
            fail("Phase-0 artifact ledger contains itself or its detached seal")
        snapshot = snapshot_file(
            member, f"Phase-0 artifact row {number}", executable=False
        )
        require_sealed_file_mode(snapshot, f"Phase-0 artifact row {number}")
        if snapshot["sha256"] != digest:
            fail(f"Phase-0 artifact row {number} hash mismatch: {uri}")
        device = snapshot["device"]
        inode = snapshot["inode"]
        assert isinstance(device, int) and isinstance(inode, int)
        identity = (device, inode)
        if identity in identities:
            fail(f"Phase-0 artifact ledger aliases one inode more than once: {uri}")
        identities.add(identity)
        observations.append({"snapshot": snapshot, "uri": uri})
        entries[uri] = digest
        order.append(uri)
    if order != sorted(order):
        fail("Phase-0 artifact ledger URIs are not canonically sorted")
    closure = {
        "member_count": len(observations),
        "observation_sha256": sha256_bytes(canonical_json_bytes(observations)),
    }
    return entries, files, closure


def artifact_digest_for(
    entries: Mapping[str, str], phase0_root: Path, path: Path, label: str
) -> str:
    try:
        relative = path.relative_to(phase0_root).as_posix()
    except ValueError:
        fail(f"{label} is outside the Phase-0 base root")
    uri = "repo://" + relative
    digest = entries.get(uri)
    if digest is None:
        fail(f"Phase-0 artifact ledger omits {label}: {uri}")
    return digest


def manifest_chain(
    phase0_root: Path,
    base_path: Path,
    base_sha256: str,
    supplement_paths: Sequence[Path],
    supplement_sha256s: Sequence[str],
    artifact_sha256: str,
) -> tuple[dict[str, object], dict[str, Path], dict[str, str]]:
    if len(supplement_paths) != len(supplement_sha256s):
        fail("supplement manifest paths and external SHA-256 anchors differ in count")
    base_files = verify_detached_sha(base_path, base_sha256, "base workload manifest")
    base, _ = parse_manifest_preamble(base_path, "base workload manifest")
    if base["kind"] != "base" or base["parent_sha256"] != "-":
        fail("base workload manifest is not an exact root manifest")
    if validate_git_toplevel(phase0_root, "Phase-0 base root") != phase0_root:
        fail("Phase-0 base root is not an exact Git toplevel")
    phase0_head = git_text(
        phase0_root, ("rev-parse", "--verify", "HEAD^{commit}"), "Phase-0 base HEAD"
    ).strip()
    if phase0_head != base["repo_revision"]:
        fail("Phase-0 base root HEAD differs from the base manifest revision")
    artifact_path = canonical_existing_path(
        base_path.parent / "phase0-artifacts.tsv", "Phase-0 artifact ledger"
    )
    entries, artifact_files, artifact_closure = read_artifact_ledger(
        phase0_root, artifact_path, artifact_sha256
    )
    if artifact_digest_for(entries, phase0_root, base_path, "base workload manifest") != base_sha256:
        fail("Phase-0 artifact ledger base-manifest hash differs from its external anchor")
    host_contract = phase0_host_contract(phase0_root, base_path, entries)

    larch2 = repo_uri_path(
        phase0_root, base["frozen_larch2_uri"], "frozen Phase-0 larch2"
    )
    oracle = repo_uri_path(
        phase0_root,
        base["frozen_oracle_dagutil_uri"],
        "frozen Phase-0 oracle dagutil",
    )
    process_metrics = canonical_existing_path(
        base_path.parent / "bin" / "wric-process-metrics",
        "frozen Phase-0 process-metrics",
    )
    expected_roles = {
        "frozen_larch2": base["frozen_larch2_sha256"],
        "frozen_oracle_dagutil": base["frozen_oracle_dagutil_sha256"],
        "frozen_process_metrics": artifact_digest_for(
            entries, phase0_root, process_metrics, "frozen Phase-0 process-metrics"
        ),
    }
    for label, path in (
        ("frozen Phase-0 larch2", larch2),
        ("frozen Phase-0 oracle dagutil", oracle),
    ):
        expected = expected_roles[
            "frozen_larch2" if path == larch2 else "frozen_oracle_dagutil"
        ]
        if artifact_digest_for(entries, phase0_root, path, label) != expected:
            fail(f"Phase-0 artifact ledger differs from the base-manifest {label} hash")

    supplements: list[dict[str, object]] = []
    for index, (path, expected_sha) in enumerate(
        zip(supplement_paths, supplement_sha256s, strict=True), 1
    ):
        files = verify_detached_sha(path, expected_sha, f"supplement manifest {index}")
        preamble, _ = parse_manifest_preamble(path, f"supplement manifest {index}")
        if preamble["kind"] != "supplement" or preamble["parent_sha256"] != base_sha256:
            fail(f"supplement manifest {index} is not an append-only child of the base")
        for key in (
            "schema",
            "schema_version",
            "frozen_larch2_sha256",
            "frozen_oracle_dagutil_sha256",
        ):
            if preamble[key] != base[key]:
                fail(f"supplement manifest {index} changes base field {key}")
        supplements.append(
            {
                "expected_sha256": expected_sha,
                "files": files,
                "path": os.fspath(path),
                "preamble": preamble,
            }
        )
    chain = {
        "artifact_ledger": {
            "closure": artifact_closure,
            "expected_sha256": artifact_sha256,
            "files": artifact_files,
            "path": os.fspath(artifact_path),
        },
        "base_manifest": {
            "expected_sha256": base_sha256,
            "files": base_files,
            "path": os.fspath(base_path),
            "preamble": base,
        },
        "phase0_root": snapshot_directory(phase0_root, "Phase-0 base root"),
        "phase0_revision": phase0_head,
        "host_contract": host_contract,
        "supplement_manifests": supplements,
    }
    paths = {
        "frozen_larch2": larch2,
        "frozen_oracle_dagutil": oracle,
        "frozen_process_metrics": process_metrics,
    }
    return chain, paths, expected_roles


def supplement_manifest_ids(chain: Mapping[str, object]) -> list[str]:
    records = chain.get("supplement_manifests")
    if not isinstance(records, list):
        fail("manifest chain supplement records are invalid")
    result: list[str] = []
    for index, record in enumerate(records, 1):
        if not isinstance(record, dict) or not isinstance(record.get("preamble"), dict):
            fail(f"manifest chain supplement {index} is invalid")
        manifest_id = record["preamble"].get("manifest_id")
        if not isinstance(manifest_id, str) or RUN_LABEL_RE.fullmatch(manifest_id) is None:
            fail(f"manifest chain supplement {index} manifest_id is invalid")
        result.append(manifest_id)
    return result


def build_tool_paths(
    product_root: Path,
    capture_tool_root: Path,
    frozen_paths: Mapping[str, Path],
    benchmark_harness: Path,
) -> dict[str, Path]:
    return {
        "benchmark_harness": benchmark_harness,
        "capture_wrapper": capture_tool_root / "tools/wric_benchmark_capture.py",
        "cmake_cache": product_root / "build/CMakeCache.txt",
        "compiler": EXPECTED_COMPILER,
        "dagutil_compile_recipes": product_root
        / "build/CMakeFiles/dagutil.dir/build.make",
        "dagutil_compile_flags": product_root / "build/CMakeFiles/dagutil.dir/flags.make",
        "dagutil_link_command": product_root / "build/CMakeFiles/dagutil.dir/link.txt",
        "frozen_larch2": frozen_paths["frozen_larch2"],
        "frozen_oracle_dagutil": frozen_paths["frozen_oracle_dagutil"],
        "frozen_process_metrics": frozen_paths["frozen_process_metrics"],
        "generic_ledger": capture_tool_root / "tools/wric_evidence_run_ledger.py",
        "larch_compile_recipes": product_root
        / "build/CMakeFiles/larch.dir/build.make",
        "larch_compile_flags": product_root / "build/CMakeFiles/larch.dir/flags.make",
        "larch_link_command": product_root / "build/CMakeFiles/larch.dir/link.txt",
        "product_dagutil": product_root / "build/bin/dagutil",
    }


def harness_provenance(
    harness: Path,
    expected_harness_sha256: str,
    expected_metadata_sha256: str,
    product_root: Path,
    product_revision: str,
    capture_tool_root: Path,
) -> dict[str, object]:
    if SHA256_RE.fullmatch(expected_harness_sha256) is None:
        fail("expected benchmark harness SHA-256 is not canonical")
    harness_snapshot = snapshot_file(
        harness, "benchmark harness", executable=True, expected_nlink=1
    )
    if harness_snapshot["sha256"] != expected_harness_sha256:
        fail("benchmark harness differs from its external SHA-256 anchor")
    exact_product_harness = product_root / "tools/wric_spr_search_benchmark.sh"
    if harness == exact_product_harness:
        if expected_metadata_sha256 != "-":
            fail("exact tracked product harness must use metadata SHA-256 '-' ")
        require_timed_trial_digest_fix(product_root, product_revision, harness)
        blob = tracked_git_blob(product_root, harness, "product benchmark harness")
        if blob["working_file"] != harness_snapshot:
            fail("product benchmark harness snapshot differs from its HEAD blob check")
        return {
            "audit_result": None,
            "auditor": None,
            "expected_harness_sha256": expected_harness_sha256,
            "expected_metadata_sha256": "-",
            "kind": "exact_product_tracked",
            "metadata": None,
            "product_git_blob": blob,
        }

    if SHA256_RE.fullmatch(expected_metadata_sha256) is None:
        fail(
            "materialized compatibility harness requires a canonical external "
            "metadata SHA-256 anchor"
        )
    metadata_path = canonical_existing_path(
        os.fspath(harness) + ".metadata.json",
        "materialized compatibility harness metadata",
    )
    metadata_snapshot = snapshot_file(
        metadata_path,
        "materialized compatibility harness metadata",
        executable=False,
        expected_nlink=1,
    )
    if metadata_snapshot["sha256"] != expected_metadata_sha256:
        fail("compatibility harness metadata differs from its external SHA-256 anchor")
    auditor_path = canonical_existing_path(
        capture_tool_root / "tools" / COMPAT_AUDITOR_NAME,
        "historical harness compatibility auditor",
    )
    auditor_snapshot = snapshot_file(
        auditor_path,
        "historical harness compatibility auditor",
        executable=True,
        expected_nlink=1,
    )
    auditor_blob = tracked_git_blob(
        capture_tool_root,
        auditor_path,
        "historical harness compatibility auditor",
    )
    if auditor_blob["working_file"] != auditor_snapshot:
        fail("historical harness compatibility auditor changed during its HEAD check")
    module = load_python_module(
        auditor_path,
        "historical harness compatibility auditor",
        auditor_snapshot,
    )
    if (
        getattr(module, "SCHEMA", None) != "wric.historical_harness_compat"
        or getattr(module, "SCHEMA_VERSION", None) != 1
    ):
        fail("historical harness compatibility auditor schema is unsupported")
    audit_function = getattr(module, "audit_materialized_harness", None)
    if not callable(audit_function):
        fail("historical harness compatibility auditor lacks its required API")
    try:
        result = audit_function(
            harness,
            metadata_path,
            expected_harness_sha256,
            expected_metadata_sha256,
            product_root,
            product_revision,
        )
    except Exception as error:
        fail(f"materialized compatibility harness audit failed: {error}")
    if not isinstance(result, Mapping):
        fail("materialized compatibility harness audit returned a non-mapping result")
    try:
        normalized_result = json.loads(
            json.dumps(result, sort_keys=True, separators=(",", ":"), allow_nan=False)
        )
    except (TypeError, ValueError, json.JSONDecodeError) as error:
        fail(f"materialized compatibility harness audit result is not canonical JSON: {error}")
    if not isinstance(normalized_result, dict):
        fail("materialized compatibility harness audit result is not a JSON object")
    return {
        "audit_result": normalized_result,
        "auditor": {"file": auditor_snapshot, "git_blob": auditor_blob},
        "expected_harness_sha256": expected_harness_sha256,
        "expected_metadata_sha256": expected_metadata_sha256,
        "kind": "materialized_compatibility",
        "metadata": metadata_snapshot,
        "product_git_blob": None,
    }


def validate_harness_provenance_shape(value: object) -> Mapping[str, object]:
    if not isinstance(value, dict):
        fail("run metadata harness provenance is not an object")
    keys = frozenset(
        {
            "audit_result",
            "auditor",
            "expected_harness_sha256",
            "expected_metadata_sha256",
            "kind",
            "metadata",
            "product_git_blob",
        }
    )
    require_exact_keys(value, keys, "run metadata harness provenance")
    kind = value["kind"]
    if kind == "exact_product_tracked":
        if (
            value["audit_result"] is not None
            or value["auditor"] is not None
            or value["metadata"] is not None
            or not isinstance(value["product_git_blob"], dict)
            or value["expected_metadata_sha256"] != "-"
        ):
            fail("exact-product harness provenance has compatibility-only fields")
        validate_tracked_git_blob_record(
            value["product_git_blob"], "product benchmark harness Git blob"
        )
    elif kind == "materialized_compatibility":
        if (
            not isinstance(value["audit_result"], dict)
            or not isinstance(value["auditor"], dict)
            or not isinstance(value["metadata"], dict)
            or value["product_git_blob"] is not None
        ):
            fail("materialized harness provenance lacks its strict auditor evidence")
        require_snapshot_shape(value["metadata"], "compatibility harness metadata")
        auditor = value["auditor"]
        assert isinstance(auditor, dict)
        if set(auditor) != {"file", "git_blob"}:
            fail("materialized harness auditor evidence key set is invalid")
        auditor_file = require_snapshot_shape(
            auditor["file"], "compatibility harness auditor"
        )
        auditor_blob = validate_tracked_git_blob_record(
            auditor["git_blob"], "compatibility harness auditor Git blob"
        )
        if auditor_blob["working_file"] != auditor_file:
            fail("compatibility harness auditor snapshot differs from its HEAD blob")
    else:
        fail("run metadata harness provenance kind is unsupported")
    expected = value["expected_harness_sha256"]
    if not isinstance(expected, str) or SHA256_RE.fullmatch(expected) is None:
        fail("run metadata expected harness SHA-256 is invalid")
    return value


def require_run_harness_policy(
    run_label: str,
    product_revision: str,
    product_root: Path,
    harness: Path,
    provenance: Mapping[str, object],
) -> None:
    """Require Q captures to use the one approved summary-key route."""

    if product_revision != SUMMARY_COMPAT_PRODUCT_REVISION:
        return
    if run_label not in CURRENT_PRODUCT_RUN_REVISIONS:
        fail("summary-compat product revision has an unapproved run label")
    kind = provenance.get("kind")
    expected_harness = product_root / "tools/wric_spr_search_benchmark.sh"
    if run_label == "phase9":
        if (
            kind != "exact_product_tracked"
            or harness != expected_harness
            or provenance.get("expected_harness_sha256")
            != SUMMARY_COMPAT_TRACKED_HARNESS_SHA256
            or provenance.get("expected_metadata_sha256") != "-"
        ):
            fail("Product-Q Phase-9 capture requires its exact tracked harness")
        return

    if (
        kind != "materialized_compatibility"
        or provenance.get("expected_harness_sha256")
        != SUMMARY_COMPAT_HARNESS_SHA256
    ):
        fail(
            "Product-Q non-Phase9 capture requires the approved summary-row-ID "
            "compatibility harness"
        )
    metadata = require_snapshot_shape(
        provenance.get("metadata"), "Product-Q compatibility harness metadata"
    )
    expected_metadata_sha256 = provenance.get("expected_metadata_sha256")
    metadata_path = Path(os.fspath(harness) + ".metadata.json")
    if (
        not isinstance(expected_metadata_sha256, str)
        or SHA256_RE.fullmatch(expected_metadata_sha256) is None
        or metadata.get("sha256") != expected_metadata_sha256
        or metadata.get("path") != os.fspath(metadata_path)
        or metadata.get("mode") != 0o444
    ):
        fail("Product-Q compatibility metadata is not fully externally bound")
    audit_result = provenance.get("audit_result")
    expected_audit_keys = {
        "harness",
        "harness_sha256",
        "kind",
        "metadata",
        "metadata_sha256",
        "product_repo_root",
        "product_revision",
        "schema",
        "schema_version",
        "status",
        "transformation_spec_sha256",
        "variant",
    }
    if not isinstance(audit_result, dict) or set(audit_result) != expected_audit_keys:
        fail("Product-Q compatibility audit result has an invalid key set")
    if (
        audit_result.get("schema") != "wric.historical_harness_compat"
        or audit_result.get("schema_version") != 1
        or audit_result.get("status") != "ok"
        or audit_result.get("kind") != "audit"
        or audit_result.get("variant") != SUMMARY_COMPAT_VARIANT
        or audit_result.get("product_repo_root") != os.fspath(product_root)
        or audit_result.get("product_revision") != product_revision
        or audit_result.get("harness") != os.fspath(harness)
        or audit_result.get("metadata") != os.fspath(metadata_path)
        or audit_result.get("harness_sha256") != SUMMARY_COMPAT_HARNESS_SHA256
        or audit_result.get("metadata_sha256") != expected_metadata_sha256
        or not isinstance(audit_result.get("transformation_spec_sha256"), str)
        or SHA256_RE.fullmatch(
            str(audit_result.get("transformation_spec_sha256"))
        )
        is None
    ):
        fail("Product-Q compatibility audit result is not the exact approved route")


def require_outer_phase9_layout(outer: Path) -> Path:
    try:
        names = sorted(os.listdir(outer))
    except OSError as error:
        fail(f"cannot inspect Phase-9 outer capture: {error.strerror or error}")
    if names != [PHASE9_INNER_NAME]:
        fail(
            "Phase-9 harness must create only the fixed inner benchmark "
            f"directory before specialized sealing: {names}"
        )
    return canonical_directory(outer / PHASE9_INNER_NAME, "Phase-9 inner benchmark")


def phase9_paths(product_root: Path) -> tuple[Path, Path]:
    return (
        canonical_existing_path(
            product_root / "tools/wric_phase9_acceptance.py",
            "Phase-9 acceptance postprocessor",
        ),
        canonical_existing_path(
            product_root / "build/bin/larch2", "Phase-9 working larch2"
        ),
    )


def phase9_seal_argv(
    phase9_tool: Path,
    inner: Path,
    base_manifest: Path,
    base_sha256: str,
    supplement: Path,
    phase0_root: Path,
    product_root: Path,
    product_revision: str,
    working_larch2: Path,
    product_dagutil: Path,
    benchmark_harness: Path,
    affinity: str,
) -> list[str]:
    return [
        *PHASE9_PYTHON_PREFIX,
        os.fspath(phase9_tool),
        "seal-run",
        "--benchmark-dir",
        os.fspath(inner),
        "--base-manifest",
        os.fspath(base_manifest),
        "--expected-parent-sha256",
        base_sha256,
        "--supplement",
        os.fspath(supplement),
        "--base-repo-root",
        os.fspath(phase0_root),
        "--working-repo-root",
        os.fspath(product_root),
        "--working-revision",
        product_revision,
        "--working-larch2",
        os.fspath(working_larch2),
        "--working-dagutil",
        os.fspath(product_dagutil),
        "--benchmark-harness",
        os.fspath(benchmark_harness),
        "--affinity-cpus",
        affinity,
        "--warmups",
        "1",
        "--full-canonical",
    ]


def phase9_audit_argv(
    phase9_tool: Path,
    inner: Path,
    base_manifest: Path,
    base_sha256: str,
    supplement: Path,
    phase0_root: Path,
    product_root: Path,
    ledger_sha256: str,
) -> list[str]:
    return [
        *PHASE9_PYTHON_PREFIX,
        os.fspath(phase9_tool),
        "evaluate",
        "--benchmark-dir",
        os.fspath(inner),
        "--base-manifest",
        os.fspath(base_manifest),
        "--expected-parent-sha256",
        base_sha256,
        "--supplement",
        os.fspath(supplement),
        "--base-repo-root",
        os.fspath(phase0_root),
        "--working-repo-root",
        os.fspath(product_root),
        "--expected-run-ledger-sha256",
        ledger_sha256,
    ]


def phase9_read_only_audit(
    argv: Sequence[str],
    environment: Mapping[str, str],
    product_root: Path,
) -> tuple[dict[str, object], bytes, bytes]:
    status, stdout, stderr = run_captured_command(
        argv, environment, product_root, "Phase-9 read-only acceptance audit"
    )
    result = strict_json_bytes(stdout, "Phase-9 read-only acceptance result")
    if status != 0 or result.get("status") != "pass" or stderr:
        fail(
            "Phase-9 read-only acceptance audit did not pass exactly: "
            f"status={status}, result_status={result.get('status')!r}"
        )
    return result, stdout, stderr


def phase9_seal_and_audit(
    outer: Path,
    product_root: Path,
    product_revision: str,
    phase0_root: Path,
    base_manifest: Path,
    base_sha256: str,
    supplements: Sequence[Path],
    tool_paths: Mapping[str, Path],
    harness_provenance_record: Mapping[str, object],
    affinity: str,
    environment: Mapping[str, str],
) -> tuple[dict[str, object], dict[str, bytes]]:
    if affinity != PHYSICAL_AFFINITY:
        fail(f"Phase-9 requires exact physical-core affinity {PHYSICAL_AFFINITY}")
    if len(supplements) != 1:
        fail("Phase-9 requires exactly one Phase-9 supplement")
    if harness_provenance_record.get("kind") != "exact_product_tracked":
        fail("Phase-9 requires the exact tracked current product harness")
    inner = require_outer_phase9_layout(outer)
    phase9_tool, working_larch2 = phase9_paths(product_root)
    phase9_tool_snapshot = snapshot_file(
        phase9_tool, "Phase-9 acceptance postprocessor", executable=True
    )
    phase9_tool_blob = tracked_git_blob(
        product_root, phase9_tool, "Phase-9 acceptance postprocessor"
    )
    if phase9_tool_blob["working_file"] != phase9_tool_snapshot:
        fail("Phase-9 postprocessor snapshot differs from its HEAD blob check")
    working_larch2_snapshot = snapshot_file(
        working_larch2, "Phase-9 working larch2", executable=True
    )
    seal_argv = phase9_seal_argv(
        phase9_tool,
        inner,
        base_manifest,
        base_sha256,
        supplements[0],
        phase0_root,
        product_root,
        product_revision,
        working_larch2,
        tool_paths["product_dagutil"],
        tool_paths["benchmark_harness"],
        affinity,
    )
    require_snapshot_unchanged(
        phase9_tool_snapshot,
        "Phase-9 acceptance postprocessor immediately before seal-run",
        executable=True,
    )
    status, seal_stdout, seal_stderr = run_captured_command(
        seal_argv, environment, product_root, "Phase-9 seal-run postprocessor"
    )
    require_snapshot_unchanged(
        phase9_tool_snapshot,
        "Phase-9 acceptance postprocessor immediately after seal-run",
        executable=True,
    )
    seal_result = strict_json_bytes(seal_stdout, "Phase-9 seal-run result")
    expected_result_keys = {
        "artifact_count",
        "benchmark_dir",
        "ledger_sha256",
        "metadata_sha256",
        "schema",
        "schema_version",
        "status",
    }
    if set(seal_result) != expected_result_keys:
        fail("Phase-9 seal-run result has an unexpected key set")
    ledger_sha = seal_result.get("ledger_sha256")
    metadata_sha = seal_result.get("metadata_sha256")
    artifact_count = seal_result.get("artifact_count")
    if (
        status != 0
        or seal_stderr
        or seal_result.get("status") != "sealed"
        or seal_result.get("benchmark_dir") != os.fspath(inner)
        or not isinstance(ledger_sha, str)
        or SHA256_RE.fullmatch(ledger_sha) is None
        or not isinstance(metadata_sha, str)
        or SHA256_RE.fullmatch(metadata_sha) is None
        or isinstance(artifact_count, bool)
        or not isinstance(artifact_count, int)
        or artifact_count <= 0
    ):
        fail("Phase-9 seal-run did not return one exact successful seal result")
    phase9_metadata = snapshot_file(
        inner / PHASE9_METADATA_NAME, "Phase-9 run metadata", executable=False
    )
    phase9_ledger = snapshot_file(
        inner / PHASE9_LEDGER_NAME, "Phase-9 run ledger", executable=False
    )
    phase9_ledger_seal, phase9_ledger_seal_payload = read_snapshotted_file(
        inner / PHASE9_LEDGER_SEAL_NAME,
        "Phase-9 run ledger detached seal",
        maximum_bytes=1024,
    )
    if phase9_metadata["sha256"] != metadata_sha or phase9_ledger["sha256"] != ledger_sha:
        fail("Phase-9 seal-run result hashes differ from its published artifacts")
    expected_seal = f"{ledger_sha}  {PHASE9_LEDGER_NAME}\n".encode("ascii")
    if phase9_ledger_seal_payload != expected_seal:
        fail("Phase-9 run ledger detached seal is not exact")
    audit_argv = phase9_audit_argv(
        phase9_tool,
        inner,
        base_manifest,
        base_sha256,
        supplements[0],
        phase0_root,
        product_root,
        ledger_sha,
    )
    require_snapshot_unchanged(
        phase9_tool_snapshot,
        "Phase-9 acceptance postprocessor immediately before read-only audit",
        executable=True,
    )
    audit_result, audit_stdout, audit_stderr = phase9_read_only_audit(
        audit_argv, environment, product_root
    )
    require_snapshot_unchanged(
        phase9_tool_snapshot, "Phase-9 acceptance postprocessor", executable=True
    )
    require_snapshot_unchanged(
        working_larch2_snapshot, "Phase-9 working larch2", executable=True
    )
    if sorted(os.listdir(outer)) != [PHASE9_INNER_NAME]:
        fail("Phase-9 postprocessor wrote outside the fixed inner benchmark directory")
    record: dict[str, object] = {
        "audit_argv": audit_argv,
        "audit_argv_sha256": argv_digest(audit_argv),
        "audit_result": audit_result,
        "audit_stderr": stream_observation(audit_stderr),
        "audit_stdout": stream_observation(audit_stdout),
        "inner_directory": PHASE9_INNER_NAME,
        "kind": "phase9",
        "ledger": phase9_ledger,
        "ledger_seal": phase9_ledger_seal,
        "ledger_sha256": ledger_sha,
        "metadata": phase9_metadata,
        "metadata_sha256": metadata_sha,
        "phase9_tool": phase9_tool_snapshot,
        "phase9_tool_git_blob": phase9_tool_blob,
        "seal_argv": seal_argv,
        "seal_argv_sha256": argv_digest(seal_argv),
        "seal_result": seal_result,
        "seal_stderr": stream_observation(seal_stderr),
        "seal_stdout": stream_observation(seal_stdout),
        "working_larch2": working_larch2_snapshot,
    }
    streams = {
        PHASE9_AUDIT_STDERR_NAME: audit_stderr,
        PHASE9_AUDIT_STDOUT_NAME: audit_stdout,
        PHASE9_SEAL_STDERR_NAME: seal_stderr,
        PHASE9_SEAL_STDOUT_NAME: seal_stdout,
    }
    return record, streams


def audit_phase9_postprocessor(
    outer: Path,
    record_value: object,
    expected_ledger_sha256: str,
    product_root: Path,
    product_revision: str,
    phase0_root: Path,
    base_manifest: Path,
    base_sha256: str,
    supplements: Sequence[Path],
    tool_paths: Mapping[str, Path],
    affinity: str,
    environment: Mapping[str, str],
) -> None:
    record = validate_postprocessor_shape(record_value)
    if record["kind"] != "phase9":
        fail("--phase9-mode requires a sealed Phase-9 postprocessor record")
    if SHA256_RE.fullmatch(expected_ledger_sha256) is None:
        fail("expected Phase-9 ledger SHA-256 is not canonical")
    if record["ledger_sha256"] != expected_ledger_sha256:
        fail("Phase-9 run ledger differs from its external expected anchor")
    if len(supplements) != 1 or affinity != PHYSICAL_AFFINITY:
        fail("Phase-9 live supplement or affinity contract is invalid")
    inner = canonical_directory(
        outer / PHASE9_INNER_NAME, "Phase-9 inner benchmark"
    )
    phase9_tool, working_larch2 = phase9_paths(product_root)
    expected_paths = {
        "ledger": inner / PHASE9_LEDGER_NAME,
        "ledger_seal": inner / PHASE9_LEDGER_SEAL_NAME,
        "metadata": inner / PHASE9_METADATA_NAME,
        "phase9_tool": phase9_tool,
        "working_larch2": working_larch2,
    }
    for name, path in expected_paths.items():
        snapshot = require_snapshot_shape(record[name], f"Phase-9 postprocessor {name}")
        if snapshot["path"] != os.fspath(path):
            fail(f"Phase-9 postprocessor {name} path is not exact")
        require_snapshot_unchanged(
            snapshot,
            f"Phase-9 postprocessor {name}",
            executable=name in ("phase9_tool", "working_larch2"),
        )
    if tracked_git_blob(
        product_root, phase9_tool, "Phase-9 acceptance postprocessor"
    ) != record["phase9_tool_git_blob"]:
        fail("Phase-9 acceptance postprocessor Git blob changed")
    ledger_snapshot = require_snapshot_shape(record["ledger"], "Phase-9 run ledger")
    metadata_snapshot = require_snapshot_shape(record["metadata"], "Phase-9 run metadata")
    if (
        ledger_snapshot["sha256"] != expected_ledger_sha256
        or metadata_snapshot["sha256"] != record["metadata_sha256"]
    ):
        fail("Phase-9 postprocessor artifact hashes are inconsistent")
    expected_seal = (
        f"{expected_ledger_sha256}  {PHASE9_LEDGER_NAME}\n".encode("ascii")
    )
    _, live_seal_payload = read_snapshotted_file(
        inner / PHASE9_LEDGER_SEAL_NAME,
        "Phase-9 run ledger detached seal",
        maximum_bytes=1024,
    )
    if live_seal_payload != expected_seal:
        fail("Phase-9 run ledger detached seal is not exact")
    expected_seal_argv = phase9_seal_argv(
        phase9_tool,
        inner,
        base_manifest,
        base_sha256,
        supplements[0],
        phase0_root,
        product_root,
        product_revision,
        working_larch2,
        tool_paths["product_dagutil"],
        tool_paths["benchmark_harness"],
        affinity,
    )
    if (
        record["seal_argv"] != expected_seal_argv
        or record["seal_argv_sha256"] != argv_digest(expected_seal_argv)
    ):
        fail("Phase-9 seal-run argv is not exactly derivable")
    seal_result = record["seal_result"]
    if (
        not isinstance(seal_result, dict)
        or seal_result.get("status") != "sealed"
        or seal_result.get("benchmark_dir") != os.fspath(inner)
        or seal_result.get("ledger_sha256") != expected_ledger_sha256
        or seal_result.get("metadata_sha256") != record["metadata_sha256"]
    ):
        fail("Phase-9 recorded seal-run result is invalid")
    expected_audit_argv = phase9_audit_argv(
        phase9_tool,
        inner,
        base_manifest,
        base_sha256,
        supplements[0],
        phase0_root,
        product_root,
        expected_ledger_sha256,
    )
    if (
        record["audit_argv"] != expected_audit_argv
        or record["audit_argv_sha256"] != argv_digest(expected_audit_argv)
    ):
        fail("Phase-9 read-only audit argv is not exactly derivable")
    logs = record["logs"]
    assert isinstance(logs, dict)
    observations = {
        PHASE9_AUDIT_STDERR_NAME: record["audit_stderr"],
        PHASE9_AUDIT_STDOUT_NAME: record["audit_stdout"],
        PHASE9_SEAL_STDERR_NAME: record["seal_stderr"],
        PHASE9_SEAL_STDOUT_NAME: record["seal_stdout"],
    }
    for name, snapshot_value in logs.items():
        snapshot = require_snapshot_shape(
            snapshot_value, f"Phase-9 postprocessor log {name}"
        )
        path = outer / name
        if snapshot["path"] != os.fspath(path):
            fail(f"Phase-9 postprocessor log path is not exact: {name}")
        require_snapshot_unchanged(
            snapshot, f"Phase-9 postprocessor log {name}", executable=False
        )
        reread_snapshot, payload = read_snapshotted_file(
            path, f"Phase-9 postprocessor log {name}", maximum_bytes=MAX_COMMAND_OUTPUT_BYTES
        )
        if reread_snapshot != snapshot:
            fail(f"Phase-9 postprocessor log changed while reading: {name}")
        if stream_observation(payload) != observations[name]:
            fail(f"Phase-9 postprocessor log observation differs: {name}")
    _, seal_stdout_payload = read_snapshotted_file(
        outer / PHASE9_SEAL_STDOUT_NAME,
        "recorded Phase-9 seal-run stdout",
        maximum_bytes=MAX_COMMAND_OUTPUT_BYTES,
    )
    seal_stdout_result = strict_json_bytes(
        seal_stdout_payload, "recorded Phase-9 seal-run stdout"
    )
    if seal_stdout_result != seal_result:
        fail("recorded Phase-9 seal-run stdout differs from its result")
    phase9_tool_snapshot = require_snapshot_shape(
        record["phase9_tool"], "Phase-9 acceptance postprocessor"
    )
    require_snapshot_unchanged(
        phase9_tool_snapshot,
        "Phase-9 acceptance postprocessor immediately before audit rerun",
        executable=True,
    )
    audit_result, audit_stdout, audit_stderr = phase9_read_only_audit(
        expected_audit_argv, environment, product_root
    )
    require_snapshot_unchanged(
        phase9_tool_snapshot,
        "Phase-9 acceptance postprocessor immediately after audit rerun",
        executable=True,
    )
    if (
        audit_result != record["audit_result"]
        or stream_observation(audit_stdout) != record["audit_stdout"]
        or stream_observation(audit_stderr) != record["audit_stderr"]
    ):
        fail("rerun Phase-9 read-only audit differs from the sealed result")


def require_phase9_record_unchanged(record_value: object) -> None:
    record = validate_postprocessor_shape(record_value)
    if record["kind"] != "phase9":
        fail("Phase-9 capture lacks its specialized postprocessor record")
    for name in ("ledger", "ledger_seal", "metadata", "phase9_tool", "working_larch2"):
        snapshot = require_snapshot_shape(record[name], f"Phase-9 postprocessor {name}")
        require_snapshot_unchanged(
            snapshot,
            f"Phase-9 postprocessor {name}",
            executable=name in ("phase9_tool", "working_larch2"),
        )
    logs = record["logs"]
    assert isinstance(logs, dict)
    for name, snapshot_value in logs.items():
        snapshot = require_snapshot_shape(
            snapshot_value, f"Phase-9 postprocessor log {name}"
        )
        require_snapshot_unchanged(
            snapshot, f"Phase-9 postprocessor log {name}", executable=False
        )


def snapshot_tools(paths: Mapping[str, Path]) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for role in sorted(paths):
        result[role] = snapshot_file(
            paths[role],
            role.replace("_", " "),
            executable=role not in NONEXECUTABLE_TOOL_ROLES,
            expected_nlink=(EXPECTED_COMPILER_LINK_COUNT if role == "compiler" else 1),
        )
    executable_identities: set[tuple[int, int]] = set()
    for role, snapshot in result.items():
        if role in NONEXECUTABLE_TOOL_ROLES:
            continue
        device = snapshot["device"]
        inode = snapshot["inode"]
        if not isinstance(device, int) or not isinstance(inode, int):
            fail(f"{role} snapshot has invalid filesystem identity")
        executable_identities.add((device, inode))
    if len(executable_identities) != len(result) - len(NONEXECUTABLE_TOOL_ROLES):
        fail("benchmark executable/tool roles contain a hard-linked identity alias")
    return result


def validate_harness_arguments(
    arguments: Sequence[str],
    benchmark_out_dir: Path,
    tool_paths: Mapping[str, Path],
    base_manifest: Path,
    supplements: Sequence[Path],
    supplement_ids: Sequence[str],
    run_label: str,
    affinity_cpus: str,
) -> dict[str, object]:
    if not arguments:
        fail("capture requires the exact benchmark harness argv after --")
    values = {
        "out_dir": required_option(arguments, "--out-dir"),
        "product_dagutil": required_option(arguments, "--dagutil"),
        "frozen_larch2": required_option(arguments, "--larch2"),
        "frozen_process_metrics": required_option(arguments, "--process-metrics"),
        "base_manifest": required_option(arguments, "--workload-manifest"),
    }
    expected = {
        "out_dir": benchmark_out_dir,
        "product_dagutil": tool_paths["product_dagutil"],
        "frozen_larch2": tool_paths["frozen_larch2"],
        "frozen_process_metrics": tool_paths["frozen_process_metrics"],
        "base_manifest": base_manifest,
    }
    for key, expected_path in expected.items():
        if values[key] != os.fspath(expected_path):
            fail(
                f"harness argv {key} path is not exact: {values[key]!r} != "
                f"{os.fspath(expected_path)!r}"
            )
        if key != "out_dir":
            canonical_existing_path(values[key], f"harness argv {key}")
    observed_supplements = repeated_option(arguments, "--supplemental-workload-manifest")
    expected_supplements = [os.fspath(path) for path in supplements]
    if observed_supplements != expected_supplements:
        fail("harness argv supplemental manifest paths/order differ from external anchors")
    group = required_option(arguments, "--run-manifest-group")
    workers = required_option(arguments, "--workers-list")
    warmups = required_option(arguments, "--warmups")
    repetitions = required_option(arguments, "--repetitions")
    if warmups != "1":
        fail("harness argv must request exactly one warmup")
    if arguments.count("--full-canonical-correctness") != 1:
        fail("harness argv must request full canonical correctness exactly once")
    components = RUN_COMPONENTS.get(run_label)
    if components is None:
        fail(f"run label has no approved benchmark component matrix: {run_label}")
    matching = [
        item
        for item in components
        if item["group"] == group
        and item["workers"] == workers
        and item["repetitions"] == repetitions
        and item["supplements"] == list(supplement_ids)
    ]
    if len(matching) != 1:
        fail(
            "harness group/workers/repetitions/supplement tuple is not an exact "
            f"approved component for {run_label}"
        )
    spec = matching[0]
    affinity_class = spec["affinity"]
    if affinity_class == "P" and affinity_cpus != PHYSICAL_AFFINITY:
        fail(f"{run_label} requires exact physical-core affinity {PHYSICAL_AFFINITY}")
    if affinity_class in ("S", "U") and affinity_cpus != SMT_AFFINITY:
        qualifier = "unpinned sealed-host" if affinity_class == "U" else "SMT"
        fail(f"{run_label} requires exact {qualifier} affinity {SMT_AFFINITY}")
    if affinity_class not in ("P", "S", "U"):
        fail(f"{run_label} component has an invalid affinity class")
    extras = spec["extras"]
    if not isinstance(extras, list) or not all(isinstance(item, str) for item in extras):
        fail(f"{run_label} component has invalid strict-gate argv")
    expected_arguments = [
        "--dagutil",
        os.fspath(tool_paths["product_dagutil"]),
        "--larch2",
        os.fspath(tool_paths["frozen_larch2"]),
        "--process-metrics",
        os.fspath(tool_paths["frozen_process_metrics"]),
        "--workload-manifest",
        os.fspath(base_manifest),
    ]
    for path in supplements:
        expected_arguments.extend(
            ["--supplemental-workload-manifest", os.fspath(path)]
        )
    expected_arguments.extend(
        [
            "--out-dir",
            os.fspath(benchmark_out_dir),
            "--run-manifest-group",
            group,
            "--workers-list",
            workers,
            "--warmups",
            "1",
            "--repetitions",
            repetitions,
            "--full-canonical-correctness",
            *extras,
        ]
    )
    if list(arguments) != expected_arguments:
        fail(
            "harness argv is not the exact canonical approved component argv; "
            "extra, reordered, duplicated, or weakening options are forbidden"
        )
    return dict(values) | {
        "affinity_class": affinity_class,
        "full_canonical_correctness": True,
        "repetitions": repetitions,
        "run_label": run_label,
        "run_manifest_group": group,
        "supplement_manifest_ids": list(supplement_ids),
        "supplement_manifests": observed_supplements,
        "warmups": "1",
        "workers_list": workers,
    }


def ensure_running_wrapper(expected: Path) -> None:
    running = canonical_existing_path(Path(__file__), "running capture wrapper")
    if running != expected:
        fail(f"running capture wrapper is not the capture-tool repository copy: {running} != {expected}")


def render_metadata(metadata: Mapping[str, object]) -> bytes:
    return (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode("utf-8")


def exclusive_write(path: Path, payload: bytes, label: str) -> dict[str, object]:
    descriptor: int | None = None
    created = False
    try:
        descriptor = os.open(
            path,
            os.O_WRONLY
            | os.O_CREAT
            | os.O_EXCL
            | getattr(os, "O_NOFOLLOW", 0)
            | getattr(os, "O_CLOEXEC", 0),
            0o600,
        )
        created = True
        offset = 0
        while offset < len(payload):
            count = os.write(descriptor, payload[offset:])
            if count <= 0:
                fail(f"short write while publishing {label}")
            offset += count
        os.fchmod(descriptor, 0o444)
        os.fsync(descriptor)
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o444
            or info.st_size != len(payload)
        ):
            fail(f"published {label} has unsafe attributes")
    except FileExistsError:
        fail(f"{label} destination already exists: {path}")
    except CaptureError:
        raise
    except OSError as error:
        fail(f"cannot publish {label}: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if created:
            try:
                directory = os.open(path.parent, DIRECTORY_FLAGS)
                os.fsync(directory)
                os.close(directory)
            except OSError as error:
                fail(f"cannot fsync {label} parent directory: {error.strerror or error}")
    return snapshot_file(path, label, executable=False)


def strict_json_object(path: Path, label: str) -> dict[str, object]:
    snapshot, payload = read_snapshotted_file(
        path, label, maximum_bytes=MAX_JSON_BYTES
    )
    json_bytes = snapshot["bytes"]
    if not isinstance(json_bytes, int) or json_bytes > MAX_JSON_BYTES:
        fail(f"{label} exceeds the maximum supported size")

    def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in items:
            if key in result:
                fail(f"{label} duplicates JSON key: {key}")
            result[key] = value
        return result

    def constant(value: str) -> NoReturn:
        fail(f"{label} contains non-finite JSON number: {value}")

    try:
        value = json.loads(payload, object_pairs_hook=pairs, parse_constant=constant)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        fail(f"{label} is invalid JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{label} root is not an object")
    if render_metadata(value) != payload:
        fail(f"{label} is not exact canonical JSON")
    return value


def load_ledger_module(
    path: Path, expected_snapshot: Mapping[str, object]
) -> ModuleType:
    module = load_python_module(path, "generic ledger", expected_snapshot)
    if (
        getattr(module, "SCHEMA", None) != "wric.evidence_run_ledger"
        or getattr(module, "SCHEMA_VERSION", None) != 2
        or getattr(module, "LEDGER_NAME", None) != LEDGER_NAME
        or getattr(module, "SEAL_NAME", None) != LEDGER_SEAL_NAME
    ):
        fail("generic ledger is not the exact supported v2 contract")
    return module


def output_snapshot(capture: Path, name: str, label: str) -> dict[str, object]:
    if "/" in name or SAFE_COMPONENT_RE.fullmatch(name) is None:
        fail(f"unsafe capture output name: {name!r}")
    return snapshot_file(capture / name, label, executable=False)


def parse_raw_row_ids(
    path: Path, expected_snapshot: Mapping[str, object]
) -> tuple[int, list[str]]:
    snapshot, payload = read_snapshotted_file(
        path, "raw trials TSV", maximum_bytes=MAX_JSON_BYTES
    )
    if snapshot != expected_snapshot:
        fail("raw trials TSV changed between snapshot and parsing")
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"cannot read raw trials TSV: {error}")
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        fail("raw trials TSV is not canonical newline-delimited UTF-8")
    rows = list(csv.reader(text.splitlines(), dialect="excel-tab"))
    if len(rows) < 2 or not rows[0] or len(rows[0]) != len(set(rows[0])):
        fail("raw trials TSV has no rows or has an invalid header")
    try:
        row_id_index = rows[0].index("row_id")
    except ValueError:
        fail("raw trials TSV lacks row_id")
    row_ids: set[str] = set()
    for number, row in enumerate(rows[1:], 2):
        if len(row) != len(rows[0]) or not row[row_id_index]:
            fail(f"raw trials TSV row {number} has invalid width or row_id")
        row_ids.add(row[row_id_index])
    return len(rows) - 1, sorted(row_ids)


def capture_outputs(capture: Path) -> dict[str, object]:
    raw = output_snapshot(capture, "raw_trials.tsv", "raw trials TSV")
    summary = output_snapshot(capture, "summary.md", "benchmark summary")
    commands = output_snapshot(capture, "commands.sh", "benchmark commands")
    row_count, row_ids = parse_raw_row_ids(capture / "raw_trials.tsv", raw)
    return {
        "commands": commands,
        "raw_trials": raw,
        "raw_trial_rows": row_count,
        "row_ids": row_ids,
        "summary": summary,
    }


def observation_shape(value: object, label: str, *, compiler: bool) -> Mapping[str, object]:
    if not isinstance(value, dict):
        fail(f"{label} is not an object")
    keys = OBSERVATION_KEYS | ({"stdout_text", "stderr_text"} if compiler else set())
    require_exact_keys(value, frozenset(keys), label)
    if not isinstance(value["argv"], list) or not all(isinstance(item, str) for item in value["argv"]):
        fail(f"{label} argv is invalid")
    for key in ("returncode", "stderr_bytes", "stdout_bytes"):
        if isinstance(value[key], bool) or not isinstance(value[key], int) or value[key] < 0:
            fail(f"{label} {key} is invalid")
    for key in ("stderr_sha256", "stdout_sha256"):
        if not isinstance(value[key], str) or SHA256_RE.fullmatch(value[key]) is None:
            fail(f"{label} {key} is invalid")
    if compiler and (
        not isinstance(value["stdout_text"], str) or not isinstance(value["stderr_text"], str)
    ):
        fail(f"{label} version bytes are not represented as UTF-8 strings")
    return value


def validate_metadata_shape(metadata: Mapping[str, object]) -> None:
    require_exact_keys(metadata, TOP_LEVEL_KEYS, "run metadata")
    if metadata["schema"] != SCHEMA or metadata["schema_version"] != SCHEMA_VERSION:
        fail("run metadata schema is unsupported")
    if not isinstance(metadata["run_label"], str) or RUN_LABEL_RE.fullmatch(metadata["run_label"]) is None:
        fail("run metadata run_label is invalid")
    if metadata["binary_provenance_limit"] != BINARY_PROVENANCE_LIMIT:
        fail("run metadata binary provenance limitation is not honest/exact")
    tools = metadata["tools"]
    if not isinstance(tools, dict):
        fail("run metadata tools is not an object")
    require_exact_keys(tools, TOOL_KEYS, "run metadata tools")
    for role, value in tools.items():
        require_snapshot_shape(value, f"run metadata tool {role}")
    repositories = metadata["repositories"]
    if not isinstance(repositories, dict):
        fail("run metadata repositories is not an object")
    require_exact_keys(repositories, REPOSITORY_KEYS, "run metadata repositories")
    for role, value in repositories.items():
        if not isinstance(value, dict):
            fail(f"run metadata repository {role} is not an object")
        require_exact_keys(value, REPOSITORY_RECORD_KEYS, f"run metadata repository {role}")
        for position in ("pre", "post"):
            state = value[position]
            if not isinstance(state, dict):
                fail(f"run metadata repository {role} {position} is not an object")
            require_exact_keys(state, REPOSITORY_STATE_KEYS, f"run metadata repository {role} {position}")
    tracked = metadata["tracked_git_blobs"]
    if not isinstance(tracked, dict):
        fail("run metadata tracked Git blobs is not an object")
    require_exact_keys(tracked, TRACKED_BLOB_KEYS, "run metadata tracked Git blobs")
    for role, value in tracked.items():
        if role == "product_harness" and value is None:
            continue
        validate_tracked_git_blob_record(
            value, f"run metadata tracked Git blob {role}"
        )
    for tracked_role, tool_role in (
        ("capture_wrapper", "capture_wrapper"),
        ("generic_ledger", "generic_ledger"),
    ):
        record = tracked[tracked_role]
        assert isinstance(record, dict)
        if record["working_file"] != tools[tool_role]:
            fail(f"run metadata {tracked_role} HEAD/tool snapshots differ")
    product_harness_blob = tracked["product_harness"]
    if product_harness_blob is not None:
        assert isinstance(product_harness_blob, dict)
        if product_harness_blob["working_file"] != tools["benchmark_harness"]:
            fail("run metadata product harness HEAD/tool snapshots differ")
    validate_harness_provenance_shape(metadata["harness_provenance"])
    validate_postprocessor_shape(metadata["postprocessor"])
    host = metadata["host_contract"]
    phase0_chain = metadata["phase0_chain"]
    if (
        not isinstance(host, dict)
        or set(host) != {"calibration_sha256", "capture_metadata_sha256", "live"}
        or not isinstance(phase0_chain, dict)
        or phase0_chain.get("host_contract") != host
    ):
        fail("run metadata host contract is malformed or differs from Phase-0")
    for key in ("calibration_sha256", "capture_metadata_sha256"):
        if not isinstance(host[key], str) or SHA256_RE.fullmatch(host[key]) is None:
            fail(f"run metadata host contract {key} is invalid")
    if not isinstance(host["live"], dict):
        fail("run metadata live host topology is not an object")
    effective = metadata["effective_build_commands"]
    expected_effective = {
        "dagutil_compile_recipes",
        "dagutil_compile_flags",
        "dagutil_link_command",
        "larch_compile_recipes",
        "larch_compile_flags",
        "larch_link_command",
    }
    if not isinstance(effective, dict) or set(effective) != expected_effective:
        fail("run metadata effective build-command record is malformed")
    for role, digest in effective.items():
        if not isinstance(role, str) or not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            fail("run metadata effective build-command digest is invalid")
    observation_shape(metadata["dagutil_flags"], "run metadata dagutil flags", compiler=False)
    observation_shape(metadata["compiler_version"], "run metadata compiler version", compiler=True)


def validate_postprocessor_shape(value: object) -> Mapping[str, object]:
    if not isinstance(value, dict) or not isinstance(value.get("kind"), str):
        fail("run metadata postprocessor record is invalid")
    if value["kind"] == "none":
        if value != {"kind": "none"}:
            fail("non-Phase9 postprocessor record has extra fields")
        return value
    if value["kind"] != "phase9":
        fail("run metadata postprocessor kind is unsupported")
    keys = {
        "audit_argv",
        "audit_argv_sha256",
        "audit_result",
        "audit_stderr",
        "audit_stdout",
        "inner_directory",
        "kind",
        "ledger",
        "ledger_seal",
        "ledger_sha256",
        "logs",
        "metadata",
        "metadata_sha256",
        "phase9_tool",
        "phase9_tool_git_blob",
        "seal_argv",
        "seal_argv_sha256",
        "seal_result",
        "seal_stderr",
        "seal_stdout",
        "working_larch2",
    }
    if set(value) != keys:
        fail("Phase-9 postprocessor record has the wrong key set")
    if value["inner_directory"] != PHASE9_INNER_NAME:
        fail("Phase-9 postprocessor inner directory is not exact")
    for name in ("ledger", "ledger_seal", "metadata", "phase9_tool", "working_larch2"):
        require_snapshot_shape(value[name], f"Phase-9 postprocessor {name}")
    for name in ("ledger_sha256", "metadata_sha256", "seal_argv_sha256", "audit_argv_sha256"):
        digest = value[name]
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            fail(f"Phase-9 postprocessor {name} is invalid")
    for name in ("seal_argv", "audit_argv"):
        argv = value[name]
        if not isinstance(argv, list) or not argv or not all(isinstance(item, str) for item in argv):
            fail(f"Phase-9 postprocessor {name} is invalid")
    for name in ("seal_result", "audit_result"):
        if not isinstance(value[name], dict):
            fail(f"Phase-9 postprocessor {name} is not an object")
    phase9_blob = validate_tracked_git_blob_record(
        value["phase9_tool_git_blob"], "Phase-9 postprocessor Git blob"
    )
    if phase9_blob["working_file"] != value["phase9_tool"]:
        fail("Phase-9 postprocessor snapshot differs from its HEAD blob")
    for name in ("seal_stdout", "seal_stderr", "audit_stdout", "audit_stderr"):
        observation = value[name]
        if (
            not isinstance(observation, dict)
            or set(observation) != {"bytes", "sha256"}
            or isinstance(observation["bytes"], bool)
            or not isinstance(observation["bytes"], int)
            or observation["bytes"] < 0
            or not isinstance(observation["sha256"], str)
            or SHA256_RE.fullmatch(observation["sha256"]) is None
        ):
            fail(f"Phase-9 postprocessor {name} observation is invalid")
    logs = value["logs"]
    expected_logs = {
        PHASE9_AUDIT_STDERR_NAME,
        PHASE9_AUDIT_STDOUT_NAME,
        PHASE9_SEAL_STDERR_NAME,
        PHASE9_SEAL_STDOUT_NAME,
    }
    if not isinstance(logs, dict) or set(logs) != expected_logs:
        fail("Phase-9 postprocessor log closure is invalid")
    for name, snapshot in logs.items():
        require_snapshot_shape(snapshot, f"Phase-9 postprocessor log {name}")
    return value


def repository_record(
    root: Path, revision: str, label: str, pre: Mapping[str, object], post: Mapping[str, object]
) -> dict[str, object]:
    return {
        "expected_revision": revision,
        "path": os.fspath(root),
        "post": dict(post),
        "pre": dict(pre),
    }


def require_live_repositories(
    metadata: Mapping[str, object],
    product_root: Path,
    product_revision: str,
    capture_root: Path,
    capture_revision: str,
) -> dict[str, dict[str, object]]:
    repositories = metadata["repositories"]
    assert isinstance(repositories, dict)
    result: dict[str, dict[str, object]] = {}
    for role, root, revision in (
        ("product", product_root, product_revision),
        ("capture_tool", capture_root, capture_revision),
    ):
        recorded = repositories[role]
        assert isinstance(recorded, dict)
        if recorded["path"] != os.fspath(root) or recorded["expected_revision"] != revision:
            fail(f"run metadata {role} repository path/revision differs from external expectation")
        state = repository_state(root, revision, f"{role} repository", require_clean=True)
        if recorded["pre"] != state or recorded["post"] != state:
            fail(f"live {role} repository differs from recorded clean pre/post state")
        result[role] = state
    return result


def seal_with_generic_ledger(
    capture: Path,
    ledger_path: Path,
    expected_snapshot: Mapping[str, object],
) -> tuple[str, int]:
    module = load_ledger_module(ledger_path, expected_snapshot)
    try:
        result = module.seal_capture(capture)
    except Exception as error:
        ledger_error = getattr(module, "LedgerError", ())
        if ledger_error and isinstance(error, ledger_error):
            fail(f"generic ledger seal failed: {error}")
        raise
    digest = getattr(result, "ledger_sha256", None)
    count = getattr(result, "member_count", None)
    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None or not isinstance(count, int):
        fail("generic ledger returned an invalid seal result")
    return digest, count


def audit_with_generic_ledger(
    capture: Path,
    ledger_path: Path,
    expected_sha256: str,
    expected_snapshot: Mapping[str, object],
) -> int:
    module = load_ledger_module(ledger_path, expected_snapshot)
    try:
        result = module.audit_capture(capture, expected_sha256)
    except Exception as error:
        ledger_error = getattr(module, "LedgerError", ())
        if ledger_error and isinstance(error, ledger_error):
            fail(f"generic ledger audit failed: {error}")
        raise
    count = getattr(result, "member_count", None)
    if not isinstance(count, int):
        fail("generic ledger returned an invalid audit result")
    return count


def capture_run(args: argparse.Namespace) -> dict[str, object]:
    reject_inherited_benchmark_environment(os.environ)
    if RUN_LABEL_RE.fullmatch(args.run_label) is None:
        fail("run label is not a safe canonical identifier")
    require_run_revision(args.run_label, args.expected_product_revision)
    if bool(args.phase9_mode) != (args.run_label == "phase9"):
        fail("the phase9 run label and --phase9-mode must be selected together")
    capture, parent, parent_signature = canonical_absent_capture(args.capture_dir)
    product_root = validate_git_toplevel(args.product_repo_root, "product repository")
    capture_tool_root = validate_git_toplevel(
        args.capture_tool_repo_root, "capture-tool repository"
    )
    phase0_root = validate_git_toplevel(args.phase0_base_root, "Phase-0 base root")
    base_manifest = canonical_existing_path(args.base_manifest, "base workload manifest")
    supplements = [
        canonical_existing_path(path, f"supplement manifest {index}")
        for index, path in enumerate(args.supplement_manifest, 1)
    ]
    chain, frozen_paths, expected_frozen_hashes = manifest_chain(
        phase0_root,
        base_manifest,
        args.expected_base_manifest_sha256,
        supplements,
        args.expected_supplement_manifest_sha256,
        args.expected_phase0_artifact_ledger_sha256,
    )
    benchmark_harness = canonical_existing_path(
        args.benchmark_harness, "benchmark harness"
    )
    tool_paths = build_tool_paths(
        product_root, capture_tool_root, frozen_paths, benchmark_harness
    )
    ensure_running_wrapper(tool_paths["capture_wrapper"])
    require_tracked_file(capture_tool_root, tool_paths["capture_wrapper"], "capture wrapper")
    require_tracked_file(capture_tool_root, tool_paths["generic_ledger"], "generic ledger")
    harness_provenance_pre = harness_provenance(
        benchmark_harness,
        args.expected_harness_sha256,
        args.expected_harness_metadata_sha256,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
    )
    require_run_harness_policy(
        args.run_label,
        args.expected_product_revision,
        product_root,
        benchmark_harness,
        harness_provenance_pre,
    )
    tracked_blobs: dict[str, object] = {
        "capture_wrapper": tracked_git_blob(
            capture_tool_root, tool_paths["capture_wrapper"], "capture wrapper"
        ),
        "generic_ledger": tracked_git_blob(
            capture_tool_root, tool_paths["generic_ledger"], "generic ledger"
        ),
        "product_harness": harness_provenance_pre["product_git_blob"],
    }

    product_pre = repository_state(
        product_root, args.expected_product_revision, "product repository", require_clean=True
    )
    capture_tool_pre = repository_state(
        capture_tool_root,
        args.expected_capture_tool_revision,
        "capture-tool repository",
        require_clean=True,
    )
    tools_pre = snapshot_tools(tool_paths)
    for tracked_role, tool_role in (
        ("capture_wrapper", "capture_wrapper"),
        ("generic_ledger", "generic_ledger"),
    ):
        record = tracked_blobs[tracked_role]
        assert isinstance(record, dict)
        if record["working_file"] != tools_pre[tool_role]:
            fail(f"{tracked_role} changed between its HEAD and tool snapshots")
    product_harness_blob = tracked_blobs["product_harness"]
    if product_harness_blob is not None:
        assert isinstance(product_harness_blob, dict)
        if product_harness_blob["working_file"] != tools_pre["benchmark_harness"]:
            fail("product benchmark harness HEAD/tool snapshots differ")
    for role, digest in expected_frozen_hashes.items():
        if tools_pre[role]["sha256"] != digest:
            fail(f"{role} bytes differ from the externally anchored Phase-0 chain")
    cmake = parse_cmake_cache(tool_paths["cmake_cache"], product_root)
    effective_build_commands = validate_effective_build_commands(
        tool_paths, product_root
    )
    environment = harness_environment(phase0_root)
    affinity_pre = require_affinity(args.affinity_cpus)
    dagutil_flags_pre = command_observation(
        (os.fspath(tool_paths["product_dagutil"]), "--help"),
        environment,
        "product dagutil --help",
    )
    compiler_version_pre = compiler_version_observation(tool_paths["compiler"], environment)
    configuration = validate_harness_arguments(
        args.harness_argv,
        capture / PHASE9_INNER_NAME if args.phase9_mode else capture,
        tool_paths,
        base_manifest,
        supplements,
        supplement_manifest_ids(chain),
        args.run_label,
        args.affinity_cpus,
    )
    harness_argv = [os.fspath(tool_paths["benchmark_harness"]), *args.harness_argv]
    require_capture_parent_unchanged(parent, parent_signature, allow_mtime_change=False)
    require_snapshot_unchanged(
        tools_pre["benchmark_harness"],
        "benchmark harness immediately before execution",
        executable=True,
    )

    previous_umask = os.umask(0o022)
    try:
        with tempfile.TemporaryFile(mode="w+b", dir=parent) as stdout_stream, tempfile.TemporaryFile(
            mode="w+b", dir=parent
        ) as stderr_stream:
            try:
                completed = subprocess.run(
                    harness_argv,
                    check=False,
                    cwd=phase0_root,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_stream,
                    stderr=stderr_stream,
                )
            except OSError as error:
                fail(f"cannot execute benchmark harness: {error.strerror or error}")
            stdout_stream.seek(0)
            stderr_stream.seek(0)
            harness_stdout = stdout_stream.read()
            harness_stderr = stderr_stream.read()
    finally:
        os.umask(previous_umask)
    require_snapshot_unchanged(
        tools_pre["benchmark_harness"],
        "benchmark harness immediately after execution",
        executable=True,
    )
    if (
        len(harness_stdout) > MAX_COMMAND_OUTPUT_BYTES
        or len(harness_stderr) > MAX_COMMAND_OUTPUT_BYTES
    ):
        fail("benchmark harness console output exceeds the maximum supported size")
    if completed.returncode != 0:
        detail = harness_stderr[-8192:].decode("utf-8", errors="backslashreplace").strip()
        fail(
            f"benchmark harness failed with status {completed.returncode}"
            + (f": {detail}" if detail else "")
        )
    capture = canonical_directory(capture, "completed capture directory")
    require_capture_parent_unchanged(parent, parent_signature, allow_mtime_change=True)

    postprocessor: dict[str, object] = {"kind": "none"}
    phase9_streams: dict[str, bytes] = {}
    benchmark_output = capture
    if args.phase9_mode:
        benchmark_output = canonical_directory(
            capture / PHASE9_INNER_NAME, "Phase-9 inner benchmark"
        )
        postprocessor, phase9_streams = phase9_seal_and_audit(
            capture,
            product_root,
            args.expected_product_revision,
            phase0_root,
            base_manifest,
            args.expected_base_manifest_sha256,
            supplements,
            tool_paths,
            harness_provenance_pre,
            args.affinity_cpus,
            environment,
        )

    affinity_post = current_affinity()
    if affinity_post != affinity_pre:
        fail("process affinity changed while the benchmark harness ran")
    product_post = repository_state(
        product_root, args.expected_product_revision, "product repository", require_clean=True
    )
    capture_tool_post = repository_state(
        capture_tool_root,
        args.expected_capture_tool_revision,
        "capture-tool repository",
        require_clean=True,
    )
    if product_post != product_pre or capture_tool_post != capture_tool_pre:
        fail("product or capture-tool repository changed while the harness ran")
    tools_post = snapshot_tools(tool_paths)
    if tools_post != tools_pre:
        fail("a benchmark tool/build identity changed while the harness ran")
    if parse_cmake_cache(tool_paths["cmake_cache"], product_root) != cmake:
        fail("CMake build contract changed while the harness ran")
    if (
        validate_effective_build_commands(tool_paths, product_root)
        != effective_build_commands
    ):
        fail("effective product compile/link commands changed while the harness ran")
    dagutil_flags_post = command_observation(
        (os.fspath(tool_paths["product_dagutil"]), "--help"),
        environment,
        "product dagutil --help",
    )
    compiler_version_post = compiler_version_observation(tool_paths["compiler"], environment)
    if dagutil_flags_post != dagutil_flags_pre:
        fail("product dagutil flags changed while the harness ran")
    if compiler_version_post != compiler_version_pre:
        fail("configured compiler version output changed while the harness ran")
    harness_provenance_post = harness_provenance(
        benchmark_harness,
        args.expected_harness_sha256,
        args.expected_harness_metadata_sha256,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
    )
    if harness_provenance_post != harness_provenance_pre:
        fail("benchmark harness provenance changed while the harness ran")

    stdout_snapshot = exclusive_write(
        capture / HARNESS_STDOUT_NAME, harness_stdout, "benchmark harness stdout"
    )
    stderr_snapshot = exclusive_write(
        capture / HARNESS_STDERR_NAME, harness_stderr, "benchmark harness stderr"
    )
    phase9_log_snapshots: dict[str, dict[str, object]] = {}
    for name, payload in sorted(phase9_streams.items()):
        phase9_log_snapshots[name] = exclusive_write(
            capture / name, payload, f"Phase-9 postprocessor log {name}"
        )
    if args.phase9_mode:
        postprocessor["logs"] = phase9_log_snapshots
    outputs = capture_outputs(benchmark_output)
    execution = {
        "returncode": completed.returncode,
        "stderr": stderr_snapshot,
        "stderr_name": HARNESS_STDERR_NAME,
        "stdout": stdout_snapshot,
        "stdout_name": HARNESS_STDOUT_NAME,
    }
    metadata: dict[str, object] = {
        "affinity": {
            "post": affinity_post,
            "pre": affinity_pre,
            "requested": args.affinity_cpus,
        },
        "binary_provenance_limit": BINARY_PROVENANCE_LIMIT,
        "capture_dir": os.fspath(capture),
        "capture_outputs": outputs,
        "cmake_contract": cmake,
        "compiler_version": compiler_version_pre,
        "dagutil_flags": dagutil_flags_pre,
        "environment": environment,
        "effective_build_commands": effective_build_commands,
        "harness_argv": harness_argv,
        "harness_argv_sha256": argv_digest(harness_argv),
        "harness_configuration": configuration,
        "harness_execution": execution,
        "harness_provenance": harness_provenance_pre,
        "host_contract": chain["host_contract"],
        "phase0_chain": chain,
        "postprocessor": postprocessor,
        "repositories": {
            "capture_tool": repository_record(
                capture_tool_root,
                args.expected_capture_tool_revision,
                "capture-tool repository",
                capture_tool_pre,
                capture_tool_post,
            ),
            "product": repository_record(
                product_root,
                args.expected_product_revision,
                "product repository",
                product_pre,
                product_post,
            ),
        },
        "run_label": args.run_label,
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "tools": tools_pre,
        "tracked_git_blobs": tracked_blobs,
        "umask": "0022",
        "working_directory": os.fspath(phase0_root),
    }
    validate_metadata_shape(metadata)
    exclusive_write(capture / METADATA_NAME, render_metadata(metadata), "run metadata")

    # Recheck all live inputs immediately before the immutable closure is
    # published.  If any check fails, the explicit partial capture remains
    # fail-closed and cannot be silently reused.
    require_live_repositories(
        metadata,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
        args.expected_capture_tool_revision,
    )
    for role, snapshot in tools_pre.items():
        require_snapshot_unchanged(
            snapshot,
            role.replace("_", " "),
            executable=role not in NONEXECUTABLE_TOOL_ROLES,
        )
    chain_before_seal, paths_before_seal, hashes_before_seal = manifest_chain(
        phase0_root,
        base_manifest,
        args.expected_base_manifest_sha256,
        supplements,
        args.expected_supplement_manifest_sha256,
        args.expected_phase0_artifact_ledger_sha256,
    )
    if (
        chain_before_seal != chain
        or paths_before_seal != frozen_paths
        or hashes_before_seal != expected_frozen_hashes
    ):
        fail("Phase-0/manifest provenance changed before ledger sealing")
    if harness_provenance(
        benchmark_harness,
        args.expected_harness_sha256,
        args.expected_harness_metadata_sha256,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
    ) != harness_provenance_pre:
        fail("benchmark harness provenance changed before ledger sealing")
    if current_affinity() != affinity_pre:
        fail("process affinity changed before ledger sealing")
    if args.phase9_mode:
        require_phase9_record_unchanged(postprocessor)
    digest, member_count = seal_with_generic_ledger(
        capture, tool_paths["generic_ledger"], tools_pre["generic_ledger"]
    )

    # The ledger invocation is executable code too.  Emit its external anchor
    # only after one final live-provenance and ledger audit succeeds.
    require_live_repositories(
        metadata,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
        args.expected_capture_tool_revision,
    )
    for role, snapshot in tools_pre.items():
        require_snapshot_unchanged(
            snapshot,
            role.replace("_", " "),
            executable=role not in NONEXECUTABLE_TOOL_ROLES,
        )
    chain_after_seal, paths_after_seal, hashes_after_seal = manifest_chain(
        phase0_root,
        base_manifest,
        args.expected_base_manifest_sha256,
        supplements,
        args.expected_supplement_manifest_sha256,
        args.expected_phase0_artifact_ledger_sha256,
    )
    if (
        chain_after_seal != chain
        or paths_after_seal != frozen_paths
        or hashes_after_seal != expected_frozen_hashes
    ):
        fail("Phase-0/manifest provenance changed during ledger sealing")
    if harness_provenance(
        benchmark_harness,
        args.expected_harness_sha256,
        args.expected_harness_metadata_sha256,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
    ) != harness_provenance_pre:
        fail("benchmark harness provenance changed during ledger sealing")
    if current_affinity() != affinity_pre:
        fail("process affinity changed during ledger sealing")
    if args.phase9_mode:
        require_phase9_record_unchanged(postprocessor)
    audit_with_generic_ledger(
        capture,
        tool_paths["generic_ledger"],
        digest,
        tools_pre["generic_ledger"],
    )
    result_payload: dict[str, object] = {
        "capture_dir": os.fspath(capture),
        "ledger_sha256": digest,
        "member_count": member_count,
        "metadata": METADATA_NAME,
        "status": "sealed",
    }
    if args.phase9_mode:
        result_payload["phase9_ledger_sha256"] = postprocessor["ledger_sha256"]
    return result_payload


def audit_run(args: argparse.Namespace) -> dict[str, object]:
    reject_inherited_benchmark_environment(os.environ)
    if SHA256_RE.fullmatch(args.expected_ledger_sha256) is None:
        fail("expected ledger SHA-256 is not canonical lowercase hexadecimal")
    if RUN_LABEL_RE.fullmatch(args.expected_run_label) is None:
        fail("expected run label is not a safe canonical identifier")
    require_run_revision(
        args.expected_run_label, args.expected_product_revision
    )
    if bool(args.phase9_mode) != (args.expected_run_label == "phase9"):
        fail("the phase9 expected label and --phase9-mode must be selected together")
    if args.phase9_mode:
        if (
            not isinstance(args.expected_phase9_ledger_sha256, str)
            or SHA256_RE.fullmatch(args.expected_phase9_ledger_sha256) is None
        ):
            fail("Phase-9 audit requires a canonical external Phase-9 ledger SHA-256")
    elif args.expected_phase9_ledger_sha256 is not None:
        fail("non-Phase9 audit must not supply a Phase-9 ledger SHA-256")
    capture = canonical_directory(args.capture_dir, "capture directory")
    product_root = validate_git_toplevel(args.product_repo_root, "product repository")
    capture_tool_root = validate_git_toplevel(
        args.capture_tool_repo_root, "capture-tool repository"
    )
    phase0_root = validate_git_toplevel(args.phase0_base_root, "Phase-0 base root")
    capture_wrapper_path = canonical_existing_path(
        capture_tool_root / "tools/wric_benchmark_capture.py", "capture wrapper"
    )
    ledger_path = canonical_existing_path(
        capture_tool_root / "tools/wric_evidence_run_ledger.py", "generic ledger"
    )
    ensure_running_wrapper(capture_wrapper_path)
    repository_state(
        capture_tool_root,
        args.expected_capture_tool_revision,
        "capture-tool repository bootstrap",
        require_clean=True,
    )
    tracked_git_blob(
        capture_tool_root, capture_wrapper_path, "capture wrapper bootstrap"
    )
    bootstrap_ledger_blob = tracked_git_blob(
        capture_tool_root, ledger_path, "generic ledger bootstrap"
    )
    bootstrap_ledger_snapshot = bootstrap_ledger_blob["working_file"]
    assert isinstance(bootstrap_ledger_snapshot, dict)
    first_count = audit_with_generic_ledger(
        capture,
        ledger_path,
        args.expected_ledger_sha256,
        bootstrap_ledger_snapshot,
    )
    metadata_path = capture / METADATA_NAME
    metadata_snapshot = snapshot_file(metadata_path, "run metadata", executable=False)
    if metadata_snapshot["mode"] != 0o444:
        fail("run metadata is not exact mode 0444")
    metadata = strict_json_object(metadata_path, "run metadata")
    validate_metadata_shape(metadata)
    if metadata["run_label"] != args.expected_run_label:
        fail("run metadata label differs from the external expected label")
    if metadata["capture_dir"] != os.fspath(capture):
        fail("run metadata capture directory differs from the audited directory")
    if metadata["working_directory"] != os.fspath(phase0_root):
        fail("run metadata Phase-0 working directory differs from external expectation")
    expected_environment = harness_environment(phase0_root)
    if metadata["environment"] != expected_environment:
        fail("run metadata environment is not the exact env-i benchmark environment")
    if metadata["umask"] != "0022":
        fail("run metadata umask is not exact")
    affinity = metadata["affinity"]
    if not isinstance(affinity, dict) or set(affinity) != {"pre", "post", "requested"}:
        fail("run metadata affinity record is malformed")
    live_affinity = require_affinity(args.expected_affinity_cpus)
    if affinity != {"pre": live_affinity, "post": live_affinity, "requested": live_affinity}:
        fail("run metadata affinity pre/post differs from external/live affinity")

    require_live_repositories(
        metadata,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
        args.expected_capture_tool_revision,
    )
    chain = metadata["phase0_chain"]
    if not isinstance(chain, dict):
        fail("run metadata Phase-0 chain is not an object")
    base_manifest = canonical_existing_path(args.base_manifest, "base workload manifest")
    supplements = [
        canonical_existing_path(path, f"supplement manifest {index}")
        for index, path in enumerate(args.supplement_manifest, 1)
    ]
    live_chain, frozen_paths, expected_frozen_hashes = manifest_chain(
        phase0_root,
        base_manifest,
        args.expected_base_manifest_sha256,
        supplements,
        args.expected_supplement_manifest_sha256,
        args.expected_phase0_artifact_ledger_sha256,
    )
    if chain != live_chain:
        fail("live Phase-0/manifest provenance differs from run metadata")
    if metadata["host_contract"] != live_chain["host_contract"]:
        fail("live host topology differs from run metadata")
    benchmark_harness = canonical_existing_path(
        args.benchmark_harness, "benchmark harness"
    )
    tool_paths = build_tool_paths(
        product_root, capture_tool_root, frozen_paths, benchmark_harness
    )
    ensure_running_wrapper(tool_paths["capture_wrapper"])
    require_tracked_file(capture_tool_root, tool_paths["capture_wrapper"], "capture wrapper")
    require_tracked_file(capture_tool_root, tool_paths["generic_ledger"], "generic ledger")
    live_harness_provenance = harness_provenance(
        benchmark_harness,
        args.expected_harness_sha256,
        args.expected_harness_metadata_sha256,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
    )
    require_run_harness_policy(
        args.expected_run_label,
        args.expected_product_revision,
        product_root,
        benchmark_harness,
        live_harness_provenance,
    )
    if metadata["harness_provenance"] != live_harness_provenance:
        fail("live benchmark harness provenance differs from run metadata")
    live_tracked_blobs: dict[str, object] = {
        "capture_wrapper": tracked_git_blob(
            capture_tool_root, tool_paths["capture_wrapper"], "capture wrapper"
        ),
        "generic_ledger": tracked_git_blob(
            capture_tool_root, tool_paths["generic_ledger"], "generic ledger"
        ),
        "product_harness": live_harness_provenance["product_git_blob"],
    }
    if metadata["tracked_git_blobs"] != live_tracked_blobs:
        fail("live tracked tool Git blobs differ from run metadata")
    recorded_tools = metadata["tools"]
    assert isinstance(recorded_tools, dict)
    for role in sorted(TOOL_KEYS):
        snapshot = require_snapshot_shape(recorded_tools[role], f"run metadata tool {role}")
        if snapshot["path"] != os.fspath(tool_paths[role]):
            fail(f"run metadata {role} path differs from the derived exact role path")
        require_snapshot_unchanged(
            snapshot,
            role.replace("_", " "),
            executable=role not in NONEXECUTABLE_TOOL_ROLES,
        )
        if role in expected_frozen_hashes and snapshot["sha256"] != expected_frozen_hashes[role]:
            fail(f"run metadata {role} differs from the externally anchored Phase-0 hash")
    cmake = parse_cmake_cache(tool_paths["cmake_cache"], product_root)
    if metadata["cmake_contract"] != cmake:
        fail("live CMake build contract differs from run metadata")
    effective_build_commands = validate_effective_build_commands(
        tool_paths, product_root
    )
    if metadata["effective_build_commands"] != effective_build_commands:
        fail("live effective product compile/link commands differ from run metadata")
    flags = command_observation(
        (os.fspath(tool_paths["product_dagutil"]), "--help"),
        expected_environment,
        "product dagutil --help",
    )
    if metadata["dagutil_flags"] != flags:
        fail("live product dagutil flags differ from run metadata")
    compiler_version = compiler_version_observation(tool_paths["compiler"], expected_environment)
    if metadata["compiler_version"] != compiler_version:
        fail("live configured compiler version bytes differ from run metadata")

    harness_argv = metadata["harness_argv"]
    if not isinstance(harness_argv, list) or not all(isinstance(item, str) for item in harness_argv):
        fail("run metadata harness argv is invalid")
    if not harness_argv or harness_argv[0] != os.fspath(tool_paths["benchmark_harness"]):
        fail("run metadata harness argv does not start with the exact benchmark harness")
    configuration = validate_harness_arguments(
        harness_argv[1:],
        capture / PHASE9_INNER_NAME if args.phase9_mode else capture,
        tool_paths,
        base_manifest,
        supplements,
        supplement_manifest_ids(live_chain),
        args.expected_run_label,
        args.expected_affinity_cpus,
    )
    if metadata["harness_configuration"] != configuration:
        fail("run metadata harness configuration differs from its exact argv")
    if metadata["harness_argv_sha256"] != argv_digest(harness_argv):
        fail("run metadata harness argv digest is invalid")
    benchmark_output = capture
    if args.phase9_mode:
        if live_harness_provenance["kind"] != "exact_product_tracked":
            fail("Phase-9 audit requires the exact tracked current product harness")
        benchmark_output = canonical_directory(
            capture / PHASE9_INNER_NAME, "Phase-9 inner benchmark"
        )
        assert isinstance(args.expected_phase9_ledger_sha256, str)
        audit_phase9_postprocessor(
            capture,
            metadata["postprocessor"],
            args.expected_phase9_ledger_sha256,
            product_root,
            args.expected_product_revision,
            phase0_root,
            base_manifest,
            args.expected_base_manifest_sha256,
            supplements,
            tool_paths,
            args.expected_affinity_cpus,
            expected_environment,
        )
    elif metadata["postprocessor"] != {"kind": "none"}:
        fail("non-Phase9 capture unexpectedly records a postprocessor")
    execution = metadata["harness_execution"]
    if not isinstance(execution, dict) or set(execution) != {
        "returncode", "stderr", "stderr_name", "stdout", "stdout_name"
    }:
        fail("run metadata harness execution record is malformed")
    if execution["returncode"] != 0 or execution["stdout_name"] != HARNESS_STDOUT_NAME or execution["stderr_name"] != HARNESS_STDERR_NAME:
        fail("run metadata does not record one successful exact harness execution")
    for stream in ("stdout", "stderr"):
        snapshot = require_snapshot_shape(execution[stream], f"harness {stream}")
        expected_path = capture / (HARNESS_STDOUT_NAME if stream == "stdout" else HARNESS_STDERR_NAME)
        if snapshot["path"] != os.fspath(expected_path):
            fail(f"harness {stream} path is not exact")
        require_snapshot_unchanged(snapshot, f"harness {stream}", executable=False)
    outputs = capture_outputs(benchmark_output)
    if metadata["capture_outputs"] != outputs:
        fail("sealed raw/summary/commands hashes or row IDs differ from run metadata")

    # Close the audit interval around both live provenance and the capture.
    require_live_repositories(
        metadata,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
        args.expected_capture_tool_revision,
    )
    for role in sorted(TOOL_KEYS):
        snapshot = require_snapshot_shape(recorded_tools[role], f"run metadata tool {role}")
        require_snapshot_unchanged(
            snapshot,
            role.replace("_", " "),
            executable=role not in NONEXECUTABLE_TOOL_ROLES,
        )
    final_chain, final_frozen_paths, final_frozen_hashes = manifest_chain(
        phase0_root,
        base_manifest,
        args.expected_base_manifest_sha256,
        supplements,
        args.expected_supplement_manifest_sha256,
        args.expected_phase0_artifact_ledger_sha256,
    )
    if (
        final_chain != live_chain
        or final_frozen_paths != frozen_paths
        or final_frozen_hashes != expected_frozen_hashes
    ):
        fail("Phase-0/manifest provenance changed during audit")
    if harness_provenance(
        benchmark_harness,
        args.expected_harness_sha256,
        args.expected_harness_metadata_sha256,
        product_root,
        args.expected_product_revision,
        capture_tool_root,
    ) != live_harness_provenance:
        fail("benchmark harness provenance changed during audit")
    if current_affinity() != live_affinity:
        fail("process affinity changed during audit")
    if snapshot_file(metadata_path, "run metadata", executable=False) != metadata_snapshot:
        fail("run metadata changed during audit")
    generic_ledger_snapshot = require_snapshot_shape(
        recorded_tools["generic_ledger"], "run metadata tool generic_ledger"
    )
    second_count = audit_with_generic_ledger(
        capture,
        ledger_path,
        args.expected_ledger_sha256,
        generic_ledger_snapshot,
    )
    if second_count != first_count:
        fail("generic ledger member count changed during audit")
    result_payload: dict[str, object] = {
        "capture_dir": os.fspath(capture),
        "ledger_sha256": args.expected_ledger_sha256,
        "member_count": second_count,
        "run_label": args.expected_run_label,
        "status": "audited",
    }
    if args.phase9_mode:
        result_payload["phase9_ledger_sha256"] = args.expected_phase9_ledger_sha256
    return result_payload


def add_repository_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--product-repo-root", required=True)
    parser.add_argument("--expected-product-revision", required=True)
    parser.add_argument("--capture-tool-repo-root", required=True)
    parser.add_argument("--expected-capture-tool-revision", required=True)
    parser.add_argument("--benchmark-harness", required=True)
    parser.add_argument("--expected-harness-sha256", required=True)
    parser.add_argument(
        "--expected-harness-metadata-sha256",
        required=True,
        help="'-' only for a tracked product harness containing the score-domain fix",
    )
    parser.add_argument("--phase0-base-root", required=True)
    parser.add_argument("--base-manifest", required=True)
    parser.add_argument("--expected-base-manifest-sha256", required=True)
    parser.add_argument("--supplement-manifest", action="append", default=[])
    parser.add_argument(
        "--expected-supplement-manifest-sha256", action="append", default=[]
    )
    parser.add_argument("--expected-phase0-artifact-ledger-sha256", required=True)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="run/seal or audit a provenance-bound WRIC benchmark capture"
    )
    commands = result.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("capture", help="run the harness and seal its output")
    capture.add_argument("--capture-dir", required=True)
    capture.add_argument("--run-label", required=True)
    capture.add_argument("--affinity-cpus", required=True)
    capture.add_argument("--phase9-mode", action="store_true")
    add_repository_arguments(capture)
    capture.add_argument("harness_argv", nargs=argparse.REMAINDER)

    audit = commands.add_parser("audit", help="audit a sealed capture and live provenance")
    audit.add_argument("--capture-dir", required=True)
    audit.add_argument("--expected-ledger-sha256", required=True)
    audit.add_argument("--expected-run-label", required=True)
    audit.add_argument("--expected-affinity-cpus", required=True)
    audit.add_argument("--phase9-mode", action="store_true")
    audit.add_argument("--expected-phase9-ledger-sha256")
    add_repository_arguments(audit)
    return result


def run(args: argparse.Namespace) -> dict[str, object]:
    if args.command == "capture":
        if args.harness_argv and args.harness_argv[0] == "--":
            args.harness_argv = args.harness_argv[1:]
        return capture_run(args)
    if args.command == "audit":
        return audit_run(args)
    fail(f"unsupported command: {args.command}")


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        result = run(args)
    except CaptureError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"error: operating-system failure: {error.strerror or error}", file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
