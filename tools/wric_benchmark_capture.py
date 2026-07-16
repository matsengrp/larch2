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
import importlib.util
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from types import ModuleType
from typing import Any, Mapping, NoReturn, Sequence


SCHEMA = "wric.benchmark_capture"
SCHEMA_VERSION = 1
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
TIMED_TRIAL_DIGEST_FIX_REVISION = "3ac59125484790deed7fc21f0ef9572f0781164a"
BINARY_PROVENANCE_LIMIT = (
    "binary hashes bind the measured executables, but no reproducible-build "
    "attestation proves derivation from HEAD"
)
PHYSICAL_AFFINITY = "0,2,4,6,8,10,12,14"
SMT_AFFINITY = "0-15"


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
    "phase8-end-to-end": (
        component("phase8-generation", "1,8", "5", ("phase8-generation",)),
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
    "final-stress": (component("p0-stress-physical", "1,8", "3"),),
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
        "harness_argv",
        "harness_argv_sha256",
        "harness_configuration",
        "harness_execution",
        "harness_provenance",
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
        "frozen_larch2",
        "frozen_oracle_dagutil",
        "frozen_process_metrics",
        "generic_ledger",
        "product_dagutil",
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
        "toplevel",
    }
)
TRACKED_BLOB_KEYS = frozenset(
    {"capture_wrapper", "generic_ledger", "product_harness"}
)
TRACKED_BLOB_RECORD_KEYS = frozenset(
    {"mode", "object_id", "relative", "repository"}
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


def snapshot_file(
    value: str | Path,
    label: str,
    *,
    executable: bool,
    expected_nlink: int = 1,
) -> dict[str, object]:
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
        while True:
            block = os.read(descriptor, CHUNK_SIZE)
            if not block:
                break
            digest.update(block)
            total += len(block)
        after = os.fstat(descriptor)
        if stable_signature(after) != stable_signature(before) or total != after.st_size:
            fail(f"{label} changed while hashing: {path}")
        lexical_after = path.lstat()
        if stable_signature(lexical_after) != stable_signature(after):
            fail(f"{label} path identity changed while hashing: {path}")
        return {
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
    except CaptureError:
        raise
    except OSError as error:
        fail(f"cannot snapshot {label}: {path}: {error.strerror or error}")
    finally:
        if descriptor is not None:
            os.close(descriptor)


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
        "GIT_OPTIONAL_LOCKS": "0",
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    try:
        completed = subprocess.run(
            ["/usr/bin/git", "-C", os.fspath(root), *arguments],
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
        "GIT_OPTIONAL_LOCKS": "0",
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    completed = subprocess.run(
        [
            "/usr/bin/git",
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
    try:
        text = harness.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
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
    return {
        "head": head,
        "object_format": object_format,
        "porcelain_v2_z_base64": base64.b64encode(porcelain).decode("ascii"),
        "porcelain_v2_z_bytes": len(porcelain),
        "porcelain_v2_z_sha256": sha256_bytes(porcelain),
        "toplevel": os.fspath(root),
    }


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


def tracked_git_blob(root: Path, path: Path, label: str) -> dict[str, str]:
    require_tracked_file(root, path, label)
    relative = path.relative_to(root).as_posix()
    output = git_text(
        root,
        ("ls-files", "--stage", "--", relative),
        f"{label} Git blob",
    )
    match = re.fullmatch(r"(100755) ([0-9a-f]{40,64}) 0\t([^\n]+)\n", output)
    if match is None or match.group(3) != relative:
        fail(f"{label} does not have one exact stage-0 executable Git blob")
    return {
        "mode": match.group(1),
        "object_id": match.group(2),
        "relative": relative,
        "repository": os.fspath(root),
    }


def load_python_module(path: Path, role: str) -> ModuleType:
    snapshot_file(path, role, executable=True)
    name = f"_wric_{path.stem}_{os.getpid()}_{id(path)}"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"cannot construct the {role} module loader")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
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
    snapshot = snapshot_file(path, "CMake cache", executable=False)
    cache_bytes = snapshot["bytes"]
    if not isinstance(cache_bytes, int) or cache_bytes > MAX_JSON_BYTES:
        fail("CMake cache exceeds the maximum supported size")
    try:
        payload = path.read_bytes()
        text = payload.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
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
        "CMAKE_CXX_COMPILER": ("FILEPATH", os.fspath(EXPECTED_COMPILER)),
        "CMAKE_CXX_FLAGS_RELWITHDEBINFO": ("STRING", EXPECTED_RELWITHDEBINFO_FLAGS),
        "CMAKE_HOME_DIRECTORY": ("INTERNAL", os.fspath(product_root)),
        "ENABLE_ASAN": ("BOOL", "OFF"),
        "ENABLE_TSAN": ("BOOL", "OFF"),
        "GCC_TOOLCHAIN": ("PATH", os.fspath(EXPECTED_TOOLCHAIN)),
    }
    for key, expected in required.items():
        if entries.get(key) != expected:
            fail(f"CMake cache {key} is not the exact required value: {entries.get(key)!r}")
    return {key: value for key, (_, value) in required.items()}


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
    snapshot_file(path, label, executable=False)
    payload = path.read_bytes()
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


def verify_detached_sha(path: Path, expected_sha256: str, label: str) -> dict[str, object]:
    if SHA256_RE.fullmatch(expected_sha256) is None:
        fail(f"expected {label} SHA-256 is not canonical")
    manifest = snapshot_file(path, label, executable=False)
    if manifest["sha256"] != expected_sha256:
        fail(f"{label} differs from its external SHA-256 anchor")
    seal_path = canonical_existing_path(os.fspath(path) + ".sha256", f"{label} detached seal")
    seal = snapshot_file(seal_path, f"{label} detached seal", executable=False)
    expected = f"{expected_sha256}  {path.name}\n".encode("ascii")
    if seal_path.read_bytes() != expected:
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
) -> tuple[dict[str, str], dict[str, object]]:
    files = verify_detached_sha(path, expected_sha256, "Phase-0 artifact ledger")
    try:
        payload = path.read_bytes()
        text = payload.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        fail(f"cannot read Phase-0 artifact ledger: {error}")
    if not payload.endswith(b"\n") or b"\r" in payload or b"\x00" in payload:
        fail("Phase-0 artifact ledger is not canonical newline-delimited UTF-8")
    reader = csv.reader(text.splitlines(), dialect="excel-tab")
    rows = list(reader)
    if not rows or rows[0] != ["sha256", "uri"]:
        fail("Phase-0 artifact ledger header is invalid")
    entries: dict[str, str] = {}
    order: list[str] = []
    for number, row in enumerate(rows[1:], 2):
        if len(row) != 2 or SHA256_RE.fullmatch(row[0]) is None:
            fail(f"Phase-0 artifact ledger row {number} is malformed")
        digest, uri = row
        if uri in entries:
            fail(f"Phase-0 artifact ledger duplicates URI: {uri}")
        repo_uri_path(phase0_root, uri, f"Phase-0 artifact row {number}")
        entries[uri] = digest
        order.append(uri)
    if order != sorted(order):
        fail("Phase-0 artifact ledger URIs are not canonically sorted")
    return entries, files


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
    entries, artifact_files = read_artifact_ledger(
        phase0_root, artifact_path, artifact_sha256
    )
    if artifact_digest_for(entries, phase0_root, base_path, "base workload manifest") != base_sha256:
        fail("Phase-0 artifact ledger base-manifest hash differs from its external anchor")

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
        "frozen_larch2": frozen_paths["frozen_larch2"],
        "frozen_oracle_dagutil": frozen_paths["frozen_oracle_dagutil"],
        "frozen_process_metrics": frozen_paths["frozen_process_metrics"],
        "generic_ledger": capture_tool_root / "tools/wric_evidence_run_ledger.py",
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
    module = load_python_module(
        auditor_path, "historical harness compatibility auditor"
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
        require_snapshot_shape(auditor["file"], "compatibility harness auditor")
    else:
        fail("run metadata harness provenance kind is unsupported")
    expected = value["expected_harness_sha256"]
    if not isinstance(expected, str) or SHA256_RE.fullmatch(expected) is None:
        fail("run metadata expected harness SHA-256 is invalid")
    return value


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
    status, seal_stdout, seal_stderr = run_captured_command(
        seal_argv, environment, product_root, "Phase-9 seal-run postprocessor"
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
    phase9_ledger_seal = snapshot_file(
        inner / PHASE9_LEDGER_SEAL_NAME,
        "Phase-9 run ledger detached seal",
        executable=False,
    )
    if phase9_metadata["sha256"] != metadata_sha or phase9_ledger["sha256"] != ledger_sha:
        fail("Phase-9 seal-run result hashes differ from its published artifacts")
    expected_seal = f"{ledger_sha}  {PHASE9_LEDGER_NAME}\n".encode("ascii")
    if (inner / PHASE9_LEDGER_SEAL_NAME).read_bytes() != expected_seal:
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
    if (inner / PHASE9_LEDGER_SEAL_NAME).read_bytes() != expected_seal:
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
        payload = path.read_bytes()
        if stream_observation(payload) != observations[name]:
            fail(f"Phase-9 postprocessor log observation differs: {name}")
    seal_stdout_result = strict_json_bytes(
        (outer / PHASE9_SEAL_STDOUT_NAME).read_bytes(),
        "recorded Phase-9 seal-run stdout",
    )
    if seal_stdout_result != seal_result:
        fail("recorded Phase-9 seal-run stdout differs from its result")
    audit_result, audit_stdout, audit_stderr = phase9_read_only_audit(
        expected_audit_argv, environment, product_root
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
            executable=role != "cmake_cache",
            expected_nlink=(EXPECTED_COMPILER_LINK_COUNT if role == "compiler" else 1),
        )
    executable_identities: set[tuple[int, int]] = set()
    for role, snapshot in result.items():
        if role == "cmake_cache":
            continue
        device = snapshot["device"]
        inode = snapshot["inode"]
        if not isinstance(device, int) or not isinstance(inode, int):
            fail(f"{role} snapshot has invalid filesystem identity")
        executable_identities.add((device, inode))
    if len(executable_identities) != len(result) - 1:
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
    snapshot = snapshot_file(path, label, executable=False)
    json_bytes = snapshot["bytes"]
    if not isinstance(json_bytes, int) or json_bytes > MAX_JSON_BYTES:
        fail(f"{label} exceeds the maximum supported size")
    payload = path.read_bytes()

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


def load_ledger_module(path: Path) -> ModuleType:
    module = load_python_module(path, "generic ledger")
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


def parse_raw_row_ids(path: Path) -> tuple[int, list[str]]:
    try:
        payload = path.read_bytes()
        text = payload.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
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
    row_count, row_ids = parse_raw_row_ids(capture / "raw_trials.tsv")
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
        if not isinstance(value, dict):
            fail(f"run metadata tracked Git blob {role} is not an object")
        require_exact_keys(
            value, TRACKED_BLOB_RECORD_KEYS, f"run metadata tracked Git blob {role}"
        )
        if (
            not isinstance(value["mode"], str)
            or value["mode"] != "100755"
            or not isinstance(value["object_id"], str)
            or REVISION_RE.fullmatch(value["object_id"]) is None
            or not isinstance(value["relative"], str)
            or not isinstance(value["repository"], str)
        ):
            fail(f"run metadata tracked Git blob {role} fields are invalid")
    validate_harness_provenance_shape(metadata["harness_provenance"])
    validate_postprocessor_shape(metadata["postprocessor"])
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
    for name in ("seal_result", "audit_result", "phase9_tool_git_blob"):
        if not isinstance(value[name], dict):
            fail(f"Phase-9 postprocessor {name} is not an object")
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


def seal_with_generic_ledger(capture: Path, ledger_path: Path) -> tuple[str, int]:
    module = load_ledger_module(ledger_path)
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


def audit_with_generic_ledger(capture: Path, ledger_path: Path, expected_sha256: str) -> int:
    module = load_ledger_module(ledger_path)
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
    for role, digest in expected_frozen_hashes.items():
        if tools_pre[role]["sha256"] != digest:
            fail(f"{role} bytes differ from the externally anchored Phase-0 chain")
    cmake = parse_cmake_cache(tool_paths["cmake_cache"], product_root)
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
        "harness_argv": harness_argv,
        "harness_argv_sha256": argv_digest(harness_argv),
        "harness_configuration": configuration,
        "harness_execution": execution,
        "harness_provenance": harness_provenance_pre,
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
        require_snapshot_unchanged(snapshot, role.replace("_", " "), executable=role != "cmake_cache")
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
    digest, member_count = seal_with_generic_ledger(capture, tool_paths["generic_ledger"])

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
        require_snapshot_unchanged(snapshot, role.replace("_", " "), executable=role != "cmake_cache")
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
    audit_with_generic_ledger(capture, tool_paths["generic_ledger"], digest)
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
    ledger_path = canonical_existing_path(
        capture_tool_root / "tools/wric_evidence_run_ledger.py", "generic ledger"
    )
    first_count = audit_with_generic_ledger(capture, ledger_path, args.expected_ledger_sha256)
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
        require_snapshot_unchanged(snapshot, role.replace("_", " "), executable=role != "cmake_cache")
        if role in expected_frozen_hashes and snapshot["sha256"] != expected_frozen_hashes[role]:
            fail(f"run metadata {role} differs from the externally anchored Phase-0 hash")
    cmake = parse_cmake_cache(tool_paths["cmake_cache"], product_root)
    if metadata["cmake_contract"] != cmake:
        fail("live CMake build contract differs from run metadata")
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
        require_snapshot_unchanged(snapshot, role.replace("_", " "), executable=role != "cmake_cache")
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
    second_count = audit_with_generic_ledger(capture, ledger_path, args.expected_ledger_sha256)
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
