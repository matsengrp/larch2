#!/usr/bin/env python3
"""Materialize and audit narrowly approved WRIC capture-harness fixes.

The product checkout remains pristine.  This tool reads its exact tracked
benchmark harness, applies (or proves already present) a Git-derived allowlist
of score-domain and per-trial canonical-digest hunks plus one fixed,
hash-allowlisted summary-key correction, and publishes a separate immutable
harness and canonical metadata record without replacing any existing path.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import secrets
import stat
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn, cast


SCHEMA = "wric.historical_harness_compat"
SCHEMA_VERSION = 1
HARNESS_RELATIVE_PATH = "tools/wric_spr_search_benchmark.sh"
SCORE_DOMAIN_SOURCE_REVISION = "00fb50d3dd158b9db8c7adff9c160f163c132f9f"
PHASE78_REFERENCE_REVISION = "38e9a281396e5263647ba68724414848841525d7"
TIMED_TRIAL_SOURCE_REVISION = "3ac59125484790deed7fc21f0ef9572f0781164a"

PHASE78_REFERENCE_SHA256 = (
    "13721168b95d7af9c9c77d13f68d91fc92c7a6b33479ca85619e24a87255eec9"
)
SCORE_DOMAIN_SOURCE_SHA256 = (
    "fc241df4186b6b214de7808cb187d4f86f330360a295352ae692a76961e31453"
)
TIMED_TRIAL_SOURCE_SHA256 = (
    "ed089af213f7f6773a252908dc3111a4d86bf01c85f12ad9d3a846ec92b3770f"
)
PHASE15_SUMMARY_ROW_ID_RESULT_SHA256 = (
    "17e11516a0b3056a445595607a939648f5290c8aaefd43f4bd2b71fbda4c347b"
)
PHASE6_SUMMARY_ROW_ID_RESULT_SHA256 = (
    "f7d3ff47140d94d3924b2fb78744d399eacaf0956d478e9f04664cd3f9c1c7f3"
)
PHASE78_SUMMARY_ROW_ID_RESULT_SHA256 = (
    "ff7f7d2904752c7198f05e13d1eafffaa087ccba9ef70221c5084a91325ef821"
)

_SUMMARY_ROW_ID_OLD = (
    b"# Aggregate measured rows by fixture/method/requested worker. wall_clock_s is\n"
    b"# the median; explicit aggregate columns retain the maximums and trial count.\n"
    b"awk -F '\\t' -v OFS='\\t' '\n"
    b"  NR==1 {for(i=1;i<=NF;i++){h[$i]=i; name[i]=$i} nfields=NF;\n"
    b'    print $0,"trial_count","wall_clock_max_s","user_cpu_median_s","system_cpu_median_s","max_rss_max_kb","peak_sampled_rss_max_kb"; next}\n'
    b"  {\n"
    b'    g=$h["fixture"] SUBSEP $h["method"] SUBSEP $h["requested_workers"]\n'
)
_SUMMARY_ROW_ID_NEW = (
    b"# Aggregate measured trials by manifest row ID. A row ID is the sealed workload\n"
    b"# and policy identity; wall_clock_s is the median, while explicit aggregate\n"
    b"# columns retain the maximums and trial count.\n"
    b"awk -F '\\t' -v OFS='\\t' '\n"
    b"  NR==1 {for(i=1;i<=NF;i++){h[$i]=i; name[i]=$i} nfields=NF;\n"
    b'    print $0,"trial_count","wall_clock_max_s","user_cpu_median_s","system_cpu_median_s","max_rss_max_kb","peak_sampled_rss_max_kb"; next}\n'
    b"  {\n"
    b'    g=$h["row_id"]\n'
)
SUMMARY_ROW_ID_OLD_SHA256 = (
    "3bdb20c44e249748e439a19057a419a159a1e052360c18fb9c8ec328c13cb2a2"
)
SUMMARY_ROW_ID_NEW_SHA256 = (
    "16ad3c0eb900b052420b475fff07f51e8ce0fcebc124311812028d8d22e42f4d"
)

_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_REVISION_RE = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
_GIT = "/usr/bin/git"
_HARNESS_MODE = 0o555
_METADATA_MODE = 0o444
_MAX_HARNESS_BYTES = 2 * 1024 * 1024
_MAX_METADATA_BYTES = 512 * 1024


class CompatibilityError(RuntimeError):
    """A closed compatibility or provenance contract was violated."""


@dataclass(frozen=True)
class Variant:
    name: str
    base_sha256: str
    base_blob: str
    result_sha256: str
    summary_only: bool = False


@dataclass(frozen=True)
class Hunk:
    category: str
    index: int
    old: bytes
    new: bytes

    @property
    def name(self) -> str:
        return f"{self.category}-{self.index:02d}"


_VARIANTS: dict[str, Variant] = {
    "9cdcd0c0eaa9cfed37ceed3a34c8cc557637988bc0834a17465a752d8560ba32": Variant(
        name="phase1-5",
        base_sha256=(
            "9cdcd0c0eaa9cfed37ceed3a34c8cc557637988bc0834a17465a752d8560ba32"
        ),
        base_blob="9678ff856c745852882ea6cb5e38d85c47d50693",
        result_sha256=PHASE15_SUMMARY_ROW_ID_RESULT_SHA256,
    ),
    "e9460139457ac406522a81f6206182c30a3c4d38d9f2877f2785c02ec1648f14": Variant(
        name="phase6",
        base_sha256=(
            "e9460139457ac406522a81f6206182c30a3c4d38d9f2877f2785c02ec1648f14"
        ),
        base_blob="623181bc826d33ddb7bd748dc2aa5ba2ff0a632d",
        result_sha256=PHASE6_SUMMARY_ROW_ID_RESULT_SHA256,
    ),
    PHASE78_REFERENCE_SHA256: Variant(
        name="phase7-8",
        base_sha256=PHASE78_REFERENCE_SHA256,
        base_blob="c791f4fbd7f807e8d15cb7474062b8ee319dc323",
        result_sha256=PHASE78_SUMMARY_ROW_ID_RESULT_SHA256,
    ),
    TIMED_TRIAL_SOURCE_SHA256: Variant(
        name="timed-trial-current",
        base_sha256=TIMED_TRIAL_SOURCE_SHA256,
        base_blob="fa04eeb645b47b7db21fa313bf278110bfebeeb6",
        result_sha256=PHASE78_SUMMARY_ROW_ID_RESULT_SHA256,
        summary_only=True,
    ),
}

_APPROVED_PRODUCT_REVISIONS: dict[str, str] = {
    "208ce23f0c005d3702d114f535fe21564b3b79b6": "phase1-5",
    "0c4623ba1793395ae8f5c3df2a2524a27d89bc80": "phase1-5",
    "7d294d68eaadc8c55b92be4f5589278c8a2f78f2": "phase1-5",
    "cbf92b62284b2a93e506f59187ac94a5336b0be3": "phase1-5",
    "3a10e9cc45050f7a6f846f9f1adb8d5f4157f9a5": "phase1-5",
    "870c298ff1c0c21901bdf79d341bf97d121f389c": "phase6",
    "38e9a281396e5263647ba68724414848841525d7": "phase7-8",
    "6c8d0c7651c2aa2e5c396d0f57c2e4e18c322310": "phase7-8",
    "a9db72e60f153a95362db544107373817a58a258": "timed-trial-current",
    "4b5f0efb4b8355916375f4639fff0e0a3abdc03c": "timed-trial-current",
}


def _fail(message: str) -> NoReturn:
    raise CompatibilityError(message)


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _canonical_json_bytes(value: object) -> bytes:
    return (
        json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
        + "\n"
    ).encode("ascii")


def _strict_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            _fail(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> NoReturn:
    _fail(f"non-finite JSON number: {value}")


def _parse_canonical_json(payload: bytes, label: str) -> Mapping[str, object]:
    try:
        decoded = payload.decode("utf-8")
        value: object = json.loads(
            decoded,
            object_pairs_hook=_strict_object,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        _fail(f"{label} is not strict UTF-8 JSON: {error}")
    if not isinstance(value, dict):
        _fail(f"{label} top level is not an object")
    mapping = cast(dict[str, object], value)
    if _canonical_json_bytes(mapping) != payload:
        _fail(f"{label} is not canonical JSON")
    return mapping


def _git(
    root: Path,
    arguments: Sequence[str],
    *,
    input_bytes: bytes | None = None,
    allow_failure: bool = False,
) -> bytes:
    environment = {
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    result = subprocess.run(
        [_GIT, "-C", os.fspath(root), *arguments],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        env=environment,
    )
    if result.returncode != 0 and not allow_failure:
        detail = result.stderr.decode("utf-8", "replace").strip()
        _fail(f"Git command failed ({' '.join(arguments)}): {detail}")
    return result.stdout


def _require_hash(value: str, label: str) -> None:
    if _SHA256_RE.fullmatch(value) is None:
        _fail(f"{label} is not a lowercase SHA-256 digest")


def _require_revision(value: str) -> None:
    if _REVISION_RE.fullmatch(value) is None:
        _fail("expected product revision must be one full lowercase object ID")


def _require_canonical_directory(path: Path, label: str) -> Path:
    raw = os.fspath(path)
    if not path.is_absolute() or os.path.normpath(raw) != raw:
        _fail(f"{label} must be a normalized absolute path")
    try:
        resolved = path.resolve(strict=True)
        info = path.lstat()
    except OSError as error:
        _fail(f"cannot inspect {label}: {error}")
    if resolved != path or not stat.S_ISDIR(info.st_mode):
        _fail(f"{label} must be a real canonical directory without symlink aliases")
    return path


def _require_canonical_target(path: Path, label: str, *, exists: bool) -> Path:
    raw = os.fspath(path)
    if not path.is_absolute() or os.path.normpath(raw) != raw:
        _fail(f"{label} must be a normalized absolute path")
    if path.name in ("", ".", "..") or "\n" in path.name or "\t" in path.name:
        _fail(f"{label} has an unsafe final component")
    parent = _require_canonical_directory(path.parent, f"{label} parent")
    canonical = parent / path.name
    if canonical != path:
        _fail(f"{label} is not canonical")
    occupied = os.path.lexists(path)
    if exists and not occupied:
        _fail(f"{label} does not exist")
    if not exists and occupied:
        _fail(f"{label} is already occupied")
    if exists:
        try:
            if path.resolve(strict=True) != path:
                _fail(f"{label} is a symlink or alias")
        except OSError as error:
            _fail(f"cannot resolve {label}: {error}")
    return path


def _stat_signature(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_uid,
        info.st_gid,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def _read_regular_file(path: Path, label: str, mode: int, limit: int) -> bytes:
    _require_canonical_target(path, label, exists=True)
    parent_fd = os.open(
        path.parent,
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
    )
    descriptor = -1
    try:
        before = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            _fail(f"{label} must be a singly linked regular file")
        if stat.S_IMODE(before.st_mode) != mode:
            _fail(f"{label} mode must be {mode:04o}")
        if before.st_size <= 0 or before.st_size > limit:
            _fail(f"{label} size is outside its closed bound")
        descriptor = os.open(
            path.name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_fd,
        )
        opened = os.fstat(descriptor)
        if _stat_signature(opened) != _stat_signature(before):
            _fail(f"{label} changed before it was opened")
        chunks: list[bytes] = []
        remaining = limit + 1
        while remaining > 0:
            chunk = os.read(descriptor, min(65536, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        payload = b"".join(chunks)
        after_open = os.fstat(descriptor)
        after_path = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        if (
            _stat_signature(after_open) != _stat_signature(opened)
            or _stat_signature(after_path) != _stat_signature(opened)
            or len(payload) != opened.st_size
            or len(payload) > limit
        ):
            _fail(f"{label} changed while it was read")
        return payload
    except OSError as error:
        _fail(f"cannot safely read {label}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        os.close(parent_fd)


def _git_harness(root: Path, revision: str) -> tuple[bytes, str, str]:
    listing = _git(root, ["ls-tree", revision, "--", HARNESS_RELATIVE_PATH])
    try:
        text = listing.decode("ascii")
    except UnicodeDecodeError as error:
        _fail(f"tracked harness tree entry is not ASCII: {error}")
    match = re.fullmatch(
        rf"(100755) blob ([0-9a-f]{{40}}|[0-9a-f]{{64}})\t{re.escape(HARNESS_RELATIVE_PATH)}\n",
        text,
    )
    if match is None:
        _fail(f"revision {revision} lacks the exact executable harness tree entry")
    mode, blob = match.groups()
    payload = _git(root, ["cat-file", "blob", blob])
    return payload, blob, mode


def _validate_product(
    product_repo_root: Path, expected_product_revision: str
) -> tuple[Path, bytes, str, Variant]:
    root = _require_canonical_directory(product_repo_root, "product repository root")
    _require_revision(expected_product_revision)
    top = _git(root, ["rev-parse", "--show-toplevel"]).decode("utf-8").rstrip("\n")
    if top != os.fspath(root):
        _fail("product repository root is not the canonical Git toplevel")
    head = _git(root, ["rev-parse", "HEAD"]).decode("ascii").strip()
    if head != expected_product_revision:
        _fail(f"product HEAD differs from expected full revision: {head}")
    verified = _git(
        root, ["rev-parse", "--verify", f"{expected_product_revision}^{{commit}}"]
    ).decode("ascii").strip()
    if verified != expected_product_revision:
        _fail("expected product revision does not resolve to itself as a commit")
    expected_variant = _APPROVED_PRODUCT_REVISIONS.get(expected_product_revision)
    if expected_variant is None:
        _fail("expected product revision is not an approved capture checkpoint")
    if _git(root, ["status", "--porcelain=v1", "--untracked-files=all"]):
        _fail("product repository must be completely clean")

    payload, blob, git_mode = _git_harness(root, expected_product_revision)
    digest = _sha256(payload)
    variant = _VARIANTS.get(digest)
    if variant is None:
        _fail(f"unknown historical base harness SHA-256: {digest}")
    if variant.name != expected_variant:
        _fail(
            "approved product revision has the wrong historical harness variant: "
            f"expected {expected_variant}, found {variant.name}"
        )
    if blob != variant.base_blob or git_mode != "100755":
        _fail("historical harness blob or Git mode differs from its approved identity")

    source = root / HARNESS_RELATIVE_PATH
    source_bytes = _read_regular_file(
        source, "tracked product harness", 0o755, _MAX_HARNESS_BYTES
    )
    if source_bytes != payload:
        _fail("working-tree harness bytes differ from the exact Git blob")
    final_head = _git(root, ["rev-parse", "HEAD"]).decode("ascii").strip()
    final_status = _git(root, ["status", "--porcelain=v1", "--untracked-files=all"])
    if final_head != expected_product_revision or final_status:
        _fail("product repository changed during validation")
    return root, payload, blob, variant


def _unified_hunks(old: bytes, new: bytes, category: str) -> tuple[Hunk, ...]:
    try:
        old_lines = old.decode("utf-8").splitlines(keepends=True)
        new_lines = new.decode("utf-8").splitlines(keepends=True)
    except UnicodeDecodeError as error:
        _fail(f"approved harness source is not UTF-8: {error}")
    diff = list(
        difflib.unified_diff(old_lines, new_lines, n=3, lineterm="")
    )[2:]
    raw_hunks: list[tuple[bytes, bytes]] = []
    old_part: list[str] | None = None
    new_part: list[str] | None = None
    for line in diff:
        if line.startswith("@@"):
            if old_part is not None and new_part is not None:
                raw_hunks.append(
                    ("".join(old_part).encode(), "".join(new_part).encode())
                )
            old_part = []
            new_part = []
        elif line.startswith(" "):
            if old_part is None or new_part is None:
                _fail("malformed derived unified diff context")
            old_part.append(line[1:])
            new_part.append(line[1:])
        elif line.startswith("-"):
            if old_part is None:
                _fail("malformed derived unified diff deletion")
            old_part.append(line[1:])
        elif line.startswith("+"):
            if new_part is None:
                _fail("malformed derived unified diff insertion")
            new_part.append(line[1:])
        elif line.startswith("\\"):
            _fail("approved harness source unexpectedly lacks a final newline")
        else:
            _fail("malformed derived unified diff")
    if old_part is not None and new_part is not None:
        raw_hunks.append(("".join(old_part).encode(), "".join(new_part).encode()))
    if not raw_hunks or any(not old_bytes or old_bytes == new_bytes for old_bytes, new_bytes in raw_hunks):
        _fail(f"derived {category} transformation is empty or ambiguous")
    return tuple(
        Hunk(category=category, index=index, old=old_bytes, new=new_bytes)
        for index, (old_bytes, new_bytes) in enumerate(raw_hunks, start=1)
    )


def _derive_hunks(root: Path) -> tuple[Hunk, ...]:
    phase78, phase78_blob, mode = _git_harness(root, PHASE78_REFERENCE_REVISION)
    score, score_blob, score_mode = _git_harness(root, SCORE_DOMAIN_SOURCE_REVISION)
    timed, timed_blob, timed_mode = _git_harness(root, TIMED_TRIAL_SOURCE_REVISION)
    if (
        _sha256(phase78) != PHASE78_REFERENCE_SHA256
        or phase78_blob != "c791f4fbd7f807e8d15cb7474062b8ee319dc323"
        or mode != "100755"
    ):
        _fail("Phase-7/8 transformation reference changed")
    if (
        _sha256(score) != SCORE_DOMAIN_SOURCE_SHA256
        or score_blob != "f00e191348c9c1eb37abf05acea434e8ddd2c945"
        or score_mode != "100755"
    ):
        _fail("00fb50d score-domain source changed")
    if (
        _sha256(timed) != TIMED_TRIAL_SOURCE_SHA256
        or timed_blob != "fa04eeb645b47b7db21fa313bf278110bfebeeb6"
        or timed_mode != "100755"
    ):
        _fail("timed-trial transformation source changed")
    score_hunks = _unified_hunks(phase78, score, "score-domain")
    timed_hunks = _unified_hunks(score, timed, "timed-trial-canonical")
    if len(score_hunks) != 2 or len(timed_hunks) != 14:
        _fail("approved transformation hunk cardinality changed")
    if (
        _sha256(_SUMMARY_ROW_ID_OLD) != SUMMARY_ROW_ID_OLD_SHA256
        or _sha256(_SUMMARY_ROW_ID_NEW) != SUMMARY_ROW_ID_NEW_SHA256
    ):
        _fail("fixed summary row-ID correction changed")
    summary_hunk = Hunk(
        category="summary-row-id",
        index=1,
        old=_SUMMARY_ROW_ID_OLD,
        new=_SUMMARY_ROW_ID_NEW,
    )
    return (*score_hunks, *timed_hunks, summary_hunk)


def _spec_document(hunks: Sequence[Hunk]) -> Mapping[str, object]:
    return {
        "algorithm": (
            "python-difflib-unified-diff-context3-plus-fixed-summary-row-id-"
            "exact-once-v2"
        ),
        "phase78_reference_revision": PHASE78_REFERENCE_REVISION,
        "phase78_reference_sha256": PHASE78_REFERENCE_SHA256,
        "score_domain_source_revision": SCORE_DOMAIN_SOURCE_REVISION,
        "score_domain_source_sha256": SCORE_DOMAIN_SOURCE_SHA256,
        "timed_trial_source_revision": TIMED_TRIAL_SOURCE_REVISION,
        "timed_trial_source_sha256": TIMED_TRIAL_SOURCE_SHA256,
        "summary_aggregation_correction": {
            "kind": "fixed_allowlisted_exact_once",
            "scope": "post_trial_summary_grouping",
            "old_key": ["fixture", "method", "requested_workers"],
            "new_key": ["row_id"],
            "old_sha256": SUMMARY_ROW_ID_OLD_SHA256,
            "new_sha256": SUMMARY_ROW_ID_NEW_SHA256,
        },
        "hunks": [
            {
                "name": hunk.name,
                "old_sha256": _sha256(hunk.old),
                "old_size": len(hunk.old),
                "new_sha256": _sha256(hunk.new),
                "new_size": len(hunk.new),
            }
            for hunk in hunks
        ],
    }


def _apply_transformations(
    base: bytes, hunks: Sequence[Hunk], variant: Variant
) -> tuple[bytes, list[Mapping[str, object]]]:
    result = base
    proof: list[Mapping[str, object]] = []
    for hunk in hunks:
        old_occurrences = result.count(hunk.old)
        new_occurrences_before = result.count(hunk.new)
        if old_occurrences != 1 or new_occurrences_before != 0:
            _fail(
                f"{hunk.name} has wrong context or was already patched: "
                f"old={old_occurrences}, new={new_occurrences_before}"
            )
        result = result.replace(hunk.old, hunk.new, 1)
        old_after = result.count(hunk.old)
        new_after = result.count(hunk.new)
        if old_after != 0 or new_after != 1:
            _fail(f"{hunk.name} exact replacement proof failed")
        proof.append(
            {
                "name": hunk.name,
                "old_sha256": _sha256(hunk.old),
                "new_sha256": _sha256(hunk.new),
                "old_occurrences_before": old_occurrences,
                "new_occurrences_before": new_occurrences_before,
                "old_occurrences_after": old_after,
                "new_occurrences_after": new_after,
            }
        )
    if _sha256(result) != variant.result_sha256:
        _fail(f"transformed {variant.name} result hash differs from its allowlist")
    return result, proof


def _variant_hunks(
    hunks: Sequence[Hunk], variant: Variant
) -> tuple[Hunk, ...]:
    """Select the exact allowlisted transformation profile for one base."""

    if not variant.summary_only:
        return tuple(hunks)
    selected = tuple(hunk for hunk in hunks if hunk.category == "summary-row-id")
    if len(selected) != 1 or selected[0].name != "summary-row-id-01":
        _fail("summary-only transformation profile is not exactly one known hunk")
    return selected


def _tool_provenance() -> Mapping[str, object]:
    tool = Path(__file__).resolve(strict=True)
    if tool != Path(__file__).absolute():
        _fail("compatibility tool path is a symlink or alias")
    info = tool.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        _fail("compatibility tool must be a singly linked regular file")
    tool_mode = stat.S_IMODE(info.st_mode)
    tool_bytes = _read_regular_file(
        tool, "compatibility tool", tool_mode, _MAX_HARNESS_BYTES
    )
    root_text = _git(tool.parent, ["rev-parse", "--show-toplevel"]).decode("utf-8").strip()
    root = Path(root_text)
    if root.resolve(strict=True) != root:
        _fail("compatibility tool Git root is not canonical")
    try:
        relative = tool.relative_to(root).as_posix()
    except ValueError:
        _fail("compatibility tool is outside its Git root")
    revision = _git(root, ["rev-parse", "HEAD"]).decode("ascii").strip()
    return {
        "path": relative,
        "revision": revision,
        "sha256": _sha256(tool_bytes),
        "mode": f"{tool_mode:04o}",
    }


def _metadata_document(
    *,
    root: Path,
    revision: str,
    source_blob: str,
    variant: Variant,
    harness: Path,
    result: bytes,
    hunks: Sequence[Hunk],
    proof: Sequence[Mapping[str, object]],
) -> Mapping[str, object]:
    spec = _spec_document(hunks)
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "variant": variant.name,
        "product": {
            "repo_root": os.fspath(root),
            "revision": revision,
        },
        "base_harness": {
            "path": HARNESS_RELATIVE_PATH,
            "absolute_path": os.fspath(root / HARNESS_RELATIVE_PATH),
            "git_blob": source_blob,
            "git_mode": "100755",
            "filesystem_mode": "0755",
            "sha256": variant.base_sha256,
        },
        "transformation": {
            "spec_sha256": _sha256(_canonical_json_bytes(spec)),
            "spec": spec,
            "proof": list(proof),
            "approved_child_argv_change": {
                "flag": "--chart-spr-canonical-result",
                "value_placeholder": "@search-canonical-result",
                "scope": "one_unique_path_per_timed_chart_invocation",
            },
            "child_runner_logic_unchanged": True,
            "timing_boundary_unchanged": True,
            "deferred_validation_only": True,
            "summary_aggregation_logic_unchanged_except_group_key": True,
        },
        "result_harness": {
            "path": os.fspath(harness),
            "sha256": _sha256(result),
            "mode": "0555",
        },
        "metadata": {
            "path": os.fspath(harness.with_name(harness.name + ".metadata.json")),
            "mode": "0444",
        },
        "tool": _tool_provenance(),
    }


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _require_ignored_destination(root: Path, path: Path) -> None:
    if not _is_within(path, root):
        return
    relative = path.relative_to(root).as_posix()
    environment = {
        "HOME": "/nonexistent",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    result = subprocess.run(
        [_GIT, "-C", os.fspath(root), "check-ignore", "--quiet", "--", relative],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
        env=environment,
    )
    if result.returncode != 0:
        _fail("destination inside product root must be ignored so the root stays clean")


def _write_all(descriptor: int, payload: bytes) -> None:
    view = memoryview(payload)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            _fail("short write while publishing compatibility artifact")
        view = view[written:]


def _publish_no_replace(path: Path, payload: bytes, mode: int) -> tuple[int, int]:
    parent_fd = os.open(
        path.parent,
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
    )
    temporary = f".{path.name}.tmp.{os.getpid()}.{secrets.token_hex(16)}"
    descriptor = -1
    published = False
    completed = False
    identity: tuple[int, int] | None = None
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            mode,
            dir_fd=parent_fd,
        )
        _write_all(descriptor, payload)
        os.fchmod(descriptor, mode)
        os.fsync(descriptor)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            _fail("temporary publication file is not a private regular file")
        identity = (info.st_dev, info.st_ino)
        os.close(descriptor)
        descriptor = -1
        os.link(
            temporary,
            path.name,
            src_dir_fd=parent_fd,
            dst_dir_fd=parent_fd,
            follow_symlinks=False,
        )
        published = True
        os.unlink(temporary, dir_fd=parent_fd)
        os.fsync(parent_fd)
        final = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        if (
            not stat.S_ISREG(final.st_mode)
            or final.st_nlink != 1
            or stat.S_IMODE(final.st_mode) != mode
            or final.st_size != len(payload)
        ):
            _fail("published compatibility artifact failed its inode proof")
        completed = True
        return final.st_dev, final.st_ino
    except FileExistsError:
        _fail(f"refusing to replace occupied path: {path}")
    except OSError as error:
        _fail(f"failed publishing {path}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(temporary, dir_fd=parent_fd)
        except FileNotFoundError:
            pass
        if published and not completed and identity is not None:
            try:
                final = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
                if (final.st_dev, final.st_ino) == identity:
                    os.unlink(path.name, dir_fd=parent_fd)
            except FileNotFoundError:
                pass
        if not completed:
            os.fsync(parent_fd)
        os.close(parent_fd)


def _unlink_if_owned(path: Path, identity: tuple[int, int]) -> None:
    descriptor = os.open(
        path.parent,
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
    )
    try:
        try:
            info = os.stat(path.name, dir_fd=descriptor, follow_symlinks=False)
        except FileNotFoundError:
            return
        if (info.st_dev, info.st_ino) != identity or not stat.S_ISREG(info.st_mode):
            return
        os.unlink(path.name, dir_fd=descriptor)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def materialize_historical_harness(
    harness: Path,
    product_repo_root: Path,
    expected_product_revision: str,
) -> Mapping[str, object]:
    """Create a no-replace historical compatibility harness and metadata pair."""

    output = _require_canonical_target(harness, "materialized harness", exists=False)
    metadata = output.with_name(output.name + ".metadata.json")
    _require_canonical_target(metadata, "materialized harness metadata", exists=False)
    root, base, source_blob, variant = _validate_product(
        product_repo_root, expected_product_revision
    )
    _require_ignored_destination(root, output)
    _require_ignored_destination(root, metadata)
    if output == root / HARNESS_RELATIVE_PATH:
        _fail("materialized harness must not alias the tracked product harness")
    hunks = _variant_hunks(_derive_hunks(root), variant)
    result, proof = _apply_transformations(base, hunks, variant)
    document = _metadata_document(
        root=root,
        revision=expected_product_revision,
        source_blob=source_blob,
        variant=variant,
        harness=output,
        result=result,
        hunks=hunks,
        proof=proof,
    )
    metadata_bytes = _canonical_json_bytes(document)
    harness_identity: tuple[int, int] | None = None
    metadata_identity: tuple[int, int] | None = None
    harness_sha256 = _sha256(result)
    metadata_sha256 = _sha256(metadata_bytes)
    try:
        harness_identity = _publish_no_replace(output, result, _HARNESS_MODE)
        metadata_identity = _publish_no_replace(metadata, metadata_bytes, _METADATA_MODE)
        audit_materialized_harness(
            output,
            metadata,
            harness_sha256,
            metadata_sha256,
            root,
            expected_product_revision,
        )
    except CompatibilityError:
        if metadata_identity is not None:
            _unlink_if_owned(metadata, metadata_identity)
        if harness_identity is not None:
            _unlink_if_owned(output, harness_identity)
        raise
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": "materialized",
        "kind": "create",
        "variant": variant.name,
        "product_revision": expected_product_revision,
        "harness": os.fspath(output),
        "metadata": os.fspath(metadata),
        "harness_sha256": harness_sha256,
        "metadata_sha256": metadata_sha256,
    }


def audit_materialized_harness(
    harness: Path,
    metadata: Path,
    expected_harness_sha256: str,
    expected_metadata_sha256: str,
    product_repo_root: Path,
    expected_product_revision: str,
) -> Mapping[str, object]:
    """Re-derive and audit an immutable historical compatibility harness."""

    _require_hash(expected_harness_sha256, "expected harness SHA-256")
    _require_hash(expected_metadata_sha256, "expected metadata SHA-256")
    output = _require_canonical_target(harness, "materialized harness", exists=True)
    metadata_path = _require_canonical_target(
        metadata, "materialized harness metadata", exists=True
    )
    derived_metadata = output.with_name(output.name + ".metadata.json")
    if metadata_path != derived_metadata:
        _fail("metadata path is not HARNESS.name + '.metadata.json'")
    root, base, source_blob, variant = _validate_product(
        product_repo_root, expected_product_revision
    )
    harness_bytes = _read_regular_file(
        output, "materialized harness", _HARNESS_MODE, _MAX_HARNESS_BYTES
    )
    metadata_bytes = _read_regular_file(
        metadata_path, "materialized harness metadata", _METADATA_MODE, _MAX_METADATA_BYTES
    )
    actual_harness_sha256 = _sha256(harness_bytes)
    actual_metadata_sha256 = _sha256(metadata_bytes)
    if actual_harness_sha256 != expected_harness_sha256:
        _fail("materialized harness differs from its external SHA-256 anchor")
    if actual_metadata_sha256 != expected_metadata_sha256:
        _fail("materialized metadata differs from its external SHA-256 anchor")

    hunks = _variant_hunks(_derive_hunks(root), variant)
    expected_harness, proof = _apply_transformations(base, hunks, variant)
    if harness_bytes != expected_harness:
        _fail("materialized harness differs from its exact Git-derived bytes")
    expected_document = _metadata_document(
        root=root,
        revision=expected_product_revision,
        source_blob=source_blob,
        variant=variant,
        harness=output,
        result=expected_harness,
        hunks=hunks,
        proof=proof,
    )
    actual_document = _parse_canonical_json(metadata_bytes, "materialized metadata")
    if actual_document != expected_document:
        _fail("materialized metadata differs from its fully re-derived contract")
    spec = cast(Mapping[str, object], expected_document["transformation"])
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": "ok",
        "kind": "audit",
        "variant": variant.name,
        "product_repo_root": os.fspath(root),
        "product_revision": expected_product_revision,
        "harness": os.fspath(output),
        "metadata": os.fspath(metadata_path),
        "harness_sha256": actual_harness_sha256,
        "metadata_sha256": actual_metadata_sha256,
        "transformation_spec_sha256": spec["spec_sha256"],
    }


def _add_product_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--product-repo-root", required=True, type=Path)
    parser.add_argument("--expected-product-revision", required=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser(
        "create", aliases=["materialize"], help="create a no-replace compatibility pair"
    )
    create.add_argument("--harness", required=True, type=Path)
    _add_product_arguments(create)
    audit = subparsers.add_parser("audit", help="audit an existing compatibility pair")
    audit.add_argument("--harness", required=True, type=Path)
    audit.add_argument("--metadata", required=True, type=Path)
    audit.add_argument("--expected-harness-sha256", required=True)
    audit.add_argument("--expected-metadata-sha256", required=True)
    _add_product_arguments(audit)
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    parsed = _parser().parse_args(arguments)
    try:
        if parsed.command in ("create", "materialize"):
            result = materialize_historical_harness(
                parsed.harness,
                parsed.product_repo_root,
                parsed.expected_product_revision,
            )
        else:
            result = audit_materialized_harness(
                parsed.harness,
                parsed.metadata,
                parsed.expected_harness_sha256,
                parsed.expected_metadata_sha256,
                parsed.product_repo_root,
                parsed.expected_product_revision,
            )
    except CompatibilityError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(_canonical_json_bytes(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
