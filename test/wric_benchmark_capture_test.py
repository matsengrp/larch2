#!/usr/bin/env python3
"""Focused adversarial tests for the provenance-bound benchmark controller."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Sequence
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/wric_benchmark_capture.py"
LEDGER = REPO / "tools/wric_evidence_run_ledger.py"
sys.path.insert(0, os.fspath(TOOL.parent))
import wric_benchmark_capture as capture_tool  # noqa: E402


def sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def write(path: Path, payload: str | bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload.encode("utf-8") if isinstance(payload, str) else payload)
    path.chmod(mode)


def git(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["/usr/bin/git", "-C", os.fspath(root), *arguments],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_CONFIG_NOSYSTEM": "1",
            "HOME": "/nonexistent",
            "LANG": "C",
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
        },
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr or completed.stdout)
    return completed.stdout.strip()


def initialize_git(root: Path) -> None:
    root.mkdir()
    subprocess.run(
        ["/usr/bin/git", "init", "-q", "-b", "main", os.fspath(root)],
        check=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"},
    )
    git(root, "config", "user.name", "WRIC test")
    git(root, "config", "user.email", "wric-test@example.invalid")


def commit_all(root: Path, message: str) -> str:
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", message)
    return git(root, "rev-parse", "HEAD")


class Fixture:
    def __init__(self, top: Path) -> None:
        self.top = top
        self.product = top / "product"
        self.controller = top / "controller"
        self.phase0 = top / "phase0"
        self.captures = top / "captures"
        self.captures.mkdir()
        self._make_product()
        self._make_controller()
        self._make_phase0()
        self.supplements: list[tuple[Path, str]] = []
        self._make_compatibility_harness()

    def _make_product(self) -> None:
        initialize_git(self.product)
        write(self.product / ".gitignore", "/build/\n")
        write(
            self.product / "tools/wric_spr_search_benchmark.sh",
            "#!/bin/sh\necho historical-product-harness >&2\nexit 91\n",
            0o755,
        )
        write(self.product / "tracked-sentinel", "product-clean\n")
        self.product_revision = commit_all(self.product, "fake product")
        write(
            self.product / "build/bin/dagutil",
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = --help ]; then printf 'fake dagutil flags\\n' >&2; exit 0; fi\n"
            "exit 3\n",
            0o755,
        )
        write(
            self.product / "build/CMakeCache.txt",
            "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/home/ogi-agent/install/gcc-trunk/bin/g++-trunk\n"
            "CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG\n"
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.product}\n"
            "ENABLE_ASAN:BOOL=OFF\n"
            "ENABLE_TSAN:BOOL=OFF\n"
            "GCC_TOOLCHAIN:PATH=/home/ogi-agent/install/gcc-trunk\n",
        )

    def _make_controller(self) -> None:
        initialize_git(self.controller)
        write(self.controller / ".gitignore", "__pycache__/\n*.pyc\n")
        tools = self.controller / "tools"
        tools.mkdir()
        shutil.copy2(TOOL, tools / TOOL.name)
        shutil.copy2(LEDGER, tools / LEDGER.name)
        # The production contract pins the sealed host's unpinned/SMT set to
        # 0-15.  CI may run inside a cpuset with remapped CPU numbers, so this
        # isolated controller fixture substitutes its own exact live set while
        # preserving the same closed-affinity behavior.
        copied_controller = tools / TOOL.name
        controller_text = copied_controller.read_text(encoding="utf-8")
        live_affinity = capture_tool.canonical_affinity(set(os.sched_getaffinity(0)))
        copied_controller.write_text(
            controller_text.replace(
                'SMT_AFFINITY = "0-15"', f'SMT_AFFINITY = "{live_affinity}"'
            ),
            encoding="utf-8",
        )
        (tools / TOOL.name).chmod(0o755)
        (tools / LEDGER.name).chmod(0o755)
        write(
            tools / capture_tool.COMPAT_AUDITOR_NAME,
            "#!/usr/bin/env python3\n"
            "from __future__ import annotations\n"
            "import hashlib, json\n"
            "from pathlib import Path\n"
            "SCHEMA = 'wric.historical_harness_compat'\n"
            "SCHEMA_VERSION = 1\n"
            "def _sha(path):\n"
            "    return hashlib.sha256(Path(path).read_bytes()).hexdigest()\n"
            "def audit_materialized_harness(harness, metadata, expected_harness_sha256, expected_metadata_sha256, product_repo_root, expected_product_revision):\n"
            "    if _sha(harness) != expected_harness_sha256: raise RuntimeError('harness hash')\n"
            "    if _sha(metadata) != expected_metadata_sha256: raise RuntimeError('metadata hash')\n"
            "    value=json.loads(Path(metadata).read_text(encoding='utf-8'))\n"
            "    if value != {'product_revision': expected_product_revision, 'schema': SCHEMA, 'schema_version': SCHEMA_VERSION}: raise RuntimeError('metadata contract')\n"
            "    return {'base_revision': expected_product_revision, 'status': 'audited'}\n",
            0o755,
        )
        write(self.controller / "tracked-sentinel", "controller-clean\n")
        self.controller_revision = commit_all(self.controller, "fake controller")

    def _make_phase0(self) -> None:
        initialize_git(self.phase0)
        write(self.phase0 / ".gitignore", "/build/\n")
        write(self.phase0 / "sealed-root-marker", "phase0\n")
        self.phase0_revision = commit_all(self.phase0, "fake phase0 base")
        baseline = self.phase0 / "build/baseline"
        self.larch2 = baseline / "bin/larch2"
        self.oracle = baseline / "bin/dagutil"
        self.process_metrics = baseline / "bin/wric-process-metrics"
        for path, role in (
            (self.larch2, "larch2"),
            (self.oracle, "oracle"),
            (self.process_metrics, "metrics"),
        ):
            write(path, f"#!/bin/sh\nprintf '{role}\\n' >&2\nexit 0\n", 0o555)
        commands = baseline / "commands.phase0.sh"
        write(commands, "#!/bin/sh\nexit 0\n", 0o555)
        self.base_manifest = baseline / "workloads.tsv"
        manifest = (
            "# schema=wric_chart_parallelization_workloads\n"
            "# schema_version=1\n"
            "# kind=base\n"
            "# manifest_id=test-phase0\n"
            "# parent_sha256=-\n"
            f"# repo_revision={self.phase0_revision}\n"
            f"# merge_base={self.phase0_revision}\n"
            "# frozen_larch2_uri=repo://build/baseline/bin/larch2\n"
            f"# frozen_larch2_sha256={sha256_file(self.larch2)}\n"
            "# frozen_oracle_dagutil_uri=repo://build/baseline/bin/dagutil\n"
            f"# frozen_oracle_dagutil_sha256={sha256_file(self.oracle)}\n"
            "# commands_uri=manifest://commands.phase0.sh\n"
            f"# commands_sha256={sha256_file(commands)}\n"
            "row_id\tvalue\n"
            "fixture-row\t1\n"
        )
        write(self.base_manifest, manifest, 0o444)
        self.base_sha256 = sha256_file(self.base_manifest)
        write(
            Path(os.fspath(self.base_manifest) + ".sha256"),
            f"{self.base_sha256}  {self.base_manifest.name}\n",
            0o444,
        )
        artifact_members = (
            self.base_manifest,
            self.larch2,
            self.oracle,
            self.process_metrics,
        )
        rows = sorted(
            (
                f"{sha256_file(path)}\trepo://{path.relative_to(self.phase0).as_posix()}"
                for path in artifact_members
            ),
            key=lambda line: line.split("\t", 1)[1],
        )
        self.artifact_ledger = baseline / "phase0-artifacts.tsv"
        write(self.artifact_ledger, "sha256\turi\n" + "\n".join(rows) + "\n", 0o444)
        self.artifact_sha256 = sha256_file(self.artifact_ledger)
        write(
            Path(os.fspath(self.artifact_ledger) + ".sha256"),
            f"{self.artifact_sha256}  {self.artifact_ledger.name}\n",
            0o444,
        )

    def _make_compatibility_harness(self) -> None:
        self.harness = self.top / "historical-harness-compatible.sh"
        write(
            self.harness,
            "#!/bin/sh\n"
            "set -eu\n"
            "out=\n"
            "fail=0\n"
            "drift_product=0\n"
            "drift_tool=0\n"
            "drift_controller=0\n"
            "precreate_metadata=0\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    --out-dir) out=$2; shift 2;;\n"
            "    --dagutil|--larch2|--process-metrics|--workload-manifest|--supplemental-workload-manifest|--run-manifest-group) shift 2;;\n"
            "    --fake-fail) fail=1; shift;;\n"
            "    --fake-drift-product) drift_product=1; shift;;\n"
            "    --fake-drift-tool) drift_tool=1; shift;;\n"
            "    --fake-drift-controller) drift_controller=1; shift;;\n"
            "    --fake-precreate-metadata) precreate_metadata=1; shift;;\n"
            "    *) shift;;\n"
            "  esac\n"
            "done\n"
            "[ -n \"$out\" ]\n"
            "[ ! -e \"$out\" ]\n"
            "mkdir -p \"$out/logs\" \"$out/outputs\"\n"
            f"if [ \"$precreate_metadata\" -eq 1 ]; then printf rival > \"$out/{capture_tool.METADATA_NAME}\"; fi\n"
            "if [ \"$fail\" -eq 1 ]; then printf 'intentional failure\\n' >&2; exit 7; fi\n"
            f"if [ \"$drift_product\" -eq 1 ]; then printf drift >> {self.product}/tracked-sentinel; fi\n"
            f"if [ \"$drift_tool\" -eq 1 ]; then printf drift >> {self.product}/build/bin/dagutil; fi\n"
            f"if [ \"$drift_controller\" -eq 1 ]; then printf drift >> {self.controller}/tracked-sentinel; fi\n"
            "printf 'row_id\\tstatus\\nfixture-row\\tok\\n' > \"$out/raw_trials.tsv\"\n"
            "printf '# summary\\n' > \"$out/summary.md\"\n"
            "printf '#!/bin/sh\\n' > \"$out/commands.sh\"\n"
            "printf 'harness stdout\\n'\n"
            "printf 'harness stderr\\n' >&2\n",
            0o755,
        )
        self.harness_sha256 = sha256_file(self.harness)
        self.harness_metadata = Path(os.fspath(self.harness) + ".metadata.json")
        write(
            self.harness_metadata,
            json.dumps(
                {
                    "product_revision": self.product_revision,
                    "schema": capture_tool.SCHEMA.replace(
                        "benchmark_capture", "historical_harness_compat"
                    ),
                    "schema_version": 1,
                },
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
            0o444,
        )
        self.harness_metadata_sha256 = sha256_file(self.harness_metadata)

    def add_supplement(self, name: str = "supplement") -> tuple[Path, str]:
        path = self.phase0 / f"build/{name}.tsv"
        payload = (
            "# schema=wric_chart_parallelization_workloads\n"
            "# schema_version=1\n"
            "# kind=supplement\n"
            f"# manifest_id={name}\n"
            f"# parent_sha256={self.base_sha256}\n"
            f"# repo_revision={self.product_revision}\n"
            f"# merge_base={self.phase0_revision}\n"
            "# frozen_larch2_uri=repo://build/baseline/bin/larch2\n"
            f"# frozen_larch2_sha256={sha256_file(self.larch2)}\n"
            "# frozen_oracle_dagutil_uri=repo://build/baseline/bin/dagutil\n"
            f"# frozen_oracle_dagutil_sha256={sha256_file(self.oracle)}\n"
            "# commands_uri=manifest://commands.phase0.sh\n"
            f"# commands_sha256={sha256_file(self.phase0 / 'build/baseline/commands.phase0.sh')}\n"
            "row_id\tvalue\n"
            "supplement-row\t1\n"
        )
        write(path, payload, 0o444)
        digest = sha256_file(path)
        write(
            Path(os.fspath(path) + ".sha256"),
            f"{digest}  {path.name}\n",
            0o444,
        )
        self.supplements.append((path, digest))
        return path, digest

    def set_harness_behavior(self, variable: str) -> None:
        allowed = {
            "fail",
            "drift_product",
            "drift_tool",
            "drift_controller",
            "precreate_metadata",
        }
        if variable not in allowed:
            raise AssertionError(f"unknown fake harness behavior: {variable}")
        text = self.harness.read_text(encoding="utf-8")
        before = f"{variable}=0\n"
        if text.count(before) != 1:
            raise AssertionError(f"fake harness variable is not unique: {variable}")
        self.harness.chmod(0o755)
        self.harness.write_text(text.replace(before, f"{variable}=1\n"), encoding="utf-8")
        self.harness.chmod(0o755)
        self.harness_sha256 = sha256_file(self.harness)

    @property
    def wrapper(self) -> Path:
        return self.controller / "tools" / TOOL.name

    @property
    def ledger(self) -> Path:
        return self.controller / "tools" / LEDGER.name

    @property
    def affinity(self) -> str:
        return capture_tool.canonical_affinity(set(os.sched_getaffinity(0)))

    def common(self, capture: Path) -> list[str]:
        result = [
            "--capture-dir",
            os.fspath(capture),
            "--product-repo-root",
            os.fspath(self.product),
            "--expected-product-revision",
            self.product_revision,
            "--capture-tool-repo-root",
            os.fspath(self.controller),
            "--expected-capture-tool-revision",
            self.controller_revision,
            "--benchmark-harness",
            os.fspath(self.harness),
            "--expected-harness-sha256",
            self.harness_sha256,
            "--expected-harness-metadata-sha256",
            self.harness_metadata_sha256,
            "--phase0-base-root",
            os.fspath(self.phase0),
            "--base-manifest",
            os.fspath(self.base_manifest),
            "--expected-base-manifest-sha256",
            self.base_sha256,
            "--expected-phase0-artifact-ledger-sha256",
            self.artifact_sha256,
        ]
        for path, digest in self.supplements:
            result.extend(
                [
                    "--supplement-manifest",
                    os.fspath(path),
                    "--expected-supplement-manifest-sha256",
                    digest,
                ]
            )
        return result

    def harness_arguments(self, capture: Path, extra: Sequence[str] = ()) -> list[str]:
        result = [
            "--dagutil",
            os.fspath(self.product / "build/bin/dagutil"),
            "--larch2",
            os.fspath(self.larch2),
            "--process-metrics",
            os.fspath(self.process_metrics),
            "--workload-manifest",
            os.fspath(self.base_manifest),
        ]
        for path, _ in self.supplements:
            result.extend(["--supplemental-workload-manifest", os.fspath(path)])
        result.extend(
            [
            "--out-dir",
            os.fspath(capture),
            "--run-manifest-group",
            "p0-small-auto",
            "--workers-list",
            "1,auto",
            "--warmups",
            "1",
            "--repetitions",
            "5",
            "--full-canonical-correctness",
            *extra,
            ]
        )
        return result

    def capture_command(self, capture: Path, extra: Sequence[str] = ()) -> list[str]:
        return [
            sys.executable,
            os.fspath(self.wrapper),
            "capture",
            "--run-label",
            "final-small-auto",
            "--affinity-cpus",
            self.affinity,
            *self.common(capture),
            "--",
            *self.harness_arguments(capture, extra),
        ]

    def audit_command(
        self,
        capture: Path,
        ledger_sha256: str,
        *,
        label: str = "final-small-auto",
    ) -> list[str]:
        return [
            sys.executable,
            os.fspath(self.wrapper),
            "audit",
            "--expected-ledger-sha256",
            ledger_sha256,
            "--expected-run-label",
            label,
            "--expected-affinity-cpus",
            self.affinity,
            *self.common(capture),
        ]

    @staticmethod
    def run(command: Sequence[str], *, extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        environment = {
            "HOME": "/nonexistent",
            "LANG": "C",
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
        }
        if extra_env:
            environment.update(extra_env)
        return subprocess.run(
            list(command),
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )


class Phase9Fixture(Fixture):
    def __init__(self, top: Path) -> None:
        super().__init__(top)
        self.add_supplement("phase9-local-commit")

    def _make_product(self) -> None:
        subprocess.run(
            [
                "/usr/bin/git",
                "clone",
                "-q",
                "--shared",
                os.fspath(REPO),
                os.fspath(self.product),
            ],
            check=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        )
        git(self.product, "config", "user.name", "WRIC test")
        git(self.product, "config", "user.email", "wric-test@example.invalid")
        write(self.product / "tracked-sentinel", "phase9-product-clean\n")
        self._write_phase9_harness(extra_inner=False)
        write(
            self.product / "tools/wric_phase9_acceptance.py",
            "#!/usr/bin/env python3\n"
            "from __future__ import annotations\n"
            "import argparse, hashlib, json, os\n"
            "from pathlib import Path\n"
            "META='phase9-run-metadata.json'\n"
            "LEDGER='phase9-run-artifacts.tsv'\n"
            "SEAL=LEDGER+'.sha256'\n"
            "def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()\n"
            "def publish(path, payload):\n"
            "    fd=os.open(path, os.O_WRONLY|os.O_CREAT|os.O_EXCL, 0o600)\n"
            "    try:\n"
            "        os.write(fd, payload); os.fchmod(fd, 0o444); os.fsync(fd)\n"
            "    finally: os.close(fd)\n"
            "def seal(args):\n"
            "    root=Path(args.benchmark_dir)\n"
            "    outer=root.parent\n"
            "    if sorted(p.name for p in outer.iterdir()) != ['benchmark']: raise RuntimeError('controller reordered Phase9 sealing')\n"
            "    allowed={'raw_trials.tsv','summary.md','commands.sh','logs','outputs'}\n"
            "    observed={p.name for p in root.iterdir()}\n"
            "    if observed != allowed: raise RuntimeError('extra inner file before Phase9 seal')\n"
            "    metadata=(json.dumps({'status':'sealed'},sort_keys=True,indent=2)+'\\n').encode()\n"
            "    publish(root/META, metadata)\n"
            "    members=sorted(p.name for p in root.iterdir() if p.name not in {LEDGER,SEAL})\n"
            "    ledger=('fake-phase9-ledger-v1\\n'+'\\n'.join(members)+'\\n').encode()\n"
            "    publish(root/LEDGER, ledger)\n"
            "    ledger_sha=hashlib.sha256(ledger).hexdigest()\n"
            "    publish(root/SEAL, f'{ledger_sha}  {LEDGER}\\n'.encode())\n"
            "    return {'artifact_count':len(members),'benchmark_dir':str(root),'ledger_sha256':ledger_sha,'metadata_sha256':hashlib.sha256(metadata).hexdigest(),'schema':'fake.phase9','schema_version':1,'status':'sealed'}\n"
            "def evaluate(args):\n"
            "    root=Path(args.benchmark_dir)\n"
            "    actual=digest(root/LEDGER)\n"
            "    if actual != args.expected_run_ledger_sha256: raise RuntimeError('ledger anchor')\n"
            "    if (root/SEAL).read_text() != f'{actual}  {LEDGER}\\n': raise RuntimeError('ledger seal')\n"
            "    return {'ledger_sha256':actual,'schema':'fake.phase9','schema_version':1,'status':'pass'}\n"
            "def parser():\n"
            "    p=argparse.ArgumentParser(); sub=p.add_subparsers(dest='command',required=True)\n"
            "    s=sub.add_parser('seal-run')\n"
            "    for name in ('benchmark-dir','base-manifest','expected-parent-sha256','supplement','base-repo-root','working-repo-root','working-revision','working-larch2','working-dagutil','benchmark-harness','affinity-cpus','warmups'): s.add_argument('--'+name,required=True)\n"
            "    s.add_argument('--full-canonical',action='store_true',required=True)\n"
            "    a=sub.add_parser('evaluate')\n"
            "    for name in ('benchmark-dir','base-manifest','expected-parent-sha256','supplement','base-repo-root','working-repo-root','expected-run-ledger-sha256'): a.add_argument('--'+name,required=True)\n"
            "    return p\n"
            "def main():\n"
            "    args=parser().parse_args()\n"
            "    try: result=seal(args) if args.command=='seal-run' else evaluate(args); code=0\n"
            "    except Exception as error: result={'failure':str(error),'schema':'fake.phase9','schema_version':1,'status':'fail'}; code=1\n"
            "    print(json.dumps(result,sort_keys=True,indent=2)); return code\n"
            "if __name__=='__main__': raise SystemExit(main())\n",
            0o755,
        )
        self.product_revision = commit_all(self.product, "fake Phase9 product")
        write(
            self.product / "build/bin/dagutil",
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = --help ]; then printf 'fake phase9 dagutil flags\\n' >&2; exit 0; fi\n"
            "exit 3\n",
            0o755,
        )
        write(
            self.product / "build/bin/larch2",
            "#!/bin/sh\nprintf 'fake working larch2\\n' >&2\nexit 0\n",
            0o755,
        )
        write(
            self.product / "build/CMakeCache.txt",
            "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/home/ogi-agent/install/gcc-trunk/bin/g++-trunk\n"
            "CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG\n"
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.product}\n"
            "ENABLE_ASAN:BOOL=OFF\n"
            "ENABLE_TSAN:BOOL=OFF\n"
            "GCC_TOOLCHAIN:PATH=/home/ogi-agent/install/gcc-trunk\n",
        )

    def _write_phase9_harness(self, *, extra_inner: bool) -> None:
        write(
            self.product / "tools/wric_spr_search_benchmark.sh",
            "#!/bin/sh\n"
            "set -eu\n"
            "# reported=${field[${raw_index[best_reported_objective]}]}\n"
            "# [[ \"$method\" != sample_explore_merge ]] || reported=$final\n"
            "# [[ \"$expected_final\" == - || \"$reported\" == \"$expected_final\" ]]\n"
            "# reported_initial=$(awk\n"
            "# \"$reported_initial\" \"$initial\" >\"$curve\"\n"
            "# \"${MANIFEST_TIMED_WORKER_ARGS[@]}\" \"${canonical_args[@]}\" -o \"$pb\")\n"
            "# \"${commit_args[@]}\" --seed \"$seed\" \"${canonical_args[@]}\" -o \"$pb\")\n"
            "# DEFERRED_SEARCH_SHA[$result_key]=$timed_search\n"
            "# search_sha=${DEFERRED_SEARCH_SHA[$result_key]:--}\n"
            "# \"$search_sha\" != \"${canonical_companion_search[$row_id]:--}\"\n"
            "out=\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    --out-dir) out=$2; shift 2;;\n"
            "    --dagutil|--larch2|--process-metrics|--workload-manifest|--supplemental-workload-manifest|--run-manifest-group|--workers-list|--warmups|--repetitions) shift 2;;\n"
            "    --full-canonical-correctness) shift;;\n"
            "    *) exit 81;;\n"
            "  esac\n"
            "done\n"
            "[ -n \"$out\" ] && [ ! -e \"$out\" ]\n"
            "mkdir -p \"$out/logs\" \"$out/outputs\"\n"
            "printf 'row_id\\tstatus\\nphase9-local-commit-seed1-w1\\tok\\n' > \"$out/raw_trials.tsv\"\n"
            "printf 'Configuration: workers=1 8, warmups=1, repetitions=3, fake\\n' > \"$out/summary.md\"\n"
            "printf '#!/bin/sh\\n' > \"$out/commands.sh\"\n"
            + ("printf extra > \"$out/unexpected\"\n" if extra_inner else ""),
            0o755,
        )

    def _make_controller(self) -> None:
        super()._make_controller()
        path = self.controller / "tools" / TOOL.name
        text = path.read_text(encoding="utf-8")
        live = capture_tool.canonical_affinity(set(os.sched_getaffinity(0)))
        path.write_text(
            text.replace(
                'PHYSICAL_AFFINITY = "0,2,4,6,8,10,12,14"',
                f'PHYSICAL_AFFINITY = "{live}"',
            ),
            encoding="utf-8",
        )
        path.chmod(0o755)
        self.controller_revision = commit_all(
            self.controller, "adapt Phase9 test cpuset"
        )

    def _make_compatibility_harness(self) -> None:
        self.harness = self.product / "tools/wric_spr_search_benchmark.sh"
        self.harness_sha256 = sha256_file(self.harness)
        self.harness_metadata = Path(os.fspath(self.harness) + ".metadata.json")
        self.harness_metadata_sha256 = "-"

    def set_extra_inner(self) -> None:
        self._write_phase9_harness(extra_inner=True)
        self.product_revision = commit_all(self.product, "fake extra inner output")
        self.harness_sha256 = sha256_file(self.harness)

    def harness_arguments(self, capture: Path, extra: Sequence[str] = ()) -> list[str]:
        if extra:
            raise AssertionError("Phase9 fixture has no arbitrary harness options")
        supplement = self.supplements[0][0]
        return [
            "--dagutil",
            os.fspath(self.product / "build/bin/dagutil"),
            "--larch2",
            os.fspath(self.larch2),
            "--process-metrics",
            os.fspath(self.process_metrics),
            "--workload-manifest",
            os.fspath(self.base_manifest),
            "--supplemental-workload-manifest",
            os.fspath(supplement),
            "--out-dir",
            os.fspath(capture / capture_tool.PHASE9_INNER_NAME),
            "--run-manifest-group",
            "phase9-local-commit",
            "--workers-list",
            "1,8",
            "--warmups",
            "1",
            "--repetitions",
            "3",
            "--full-canonical-correctness",
        ]

    def capture_command(self, capture: Path, extra: Sequence[str] = ()) -> list[str]:
        if extra:
            raise AssertionError("Phase9 fixture has no arbitrary harness options")
        return [
            sys.executable,
            os.fspath(self.wrapper),
            "capture",
            "--run-label",
            "phase9",
            "--affinity-cpus",
            self.affinity,
            "--phase9-mode",
            *self.common(capture),
            "--",
            *self.harness_arguments(capture),
        ]

    def audit_command(
        self,
        capture: Path,
        ledger_sha256: str,
        *,
        label: str = "phase9",
        phase9_ledger_sha256: str | None = None,
    ) -> list[str]:
        if phase9_ledger_sha256 is None:
            raise AssertionError("Phase9 audit requires its inner ledger anchor")
        return [
            sys.executable,
            os.fspath(self.wrapper),
            "audit",
            "--expected-ledger-sha256",
            ledger_sha256,
            "--expected-run-label",
            label,
            "--expected-affinity-cpus",
            self.affinity,
            "--phase9-mode",
            "--expected-phase9-ledger-sha256",
            phase9_ledger_sha256,
            *self.common(capture),
        ]


class BenchmarkCaptureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.top = Path(self.temporary.name).resolve()
        self.fixture = Fixture(self.top)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def successful_capture(self, name: str = "capture") -> tuple[Path, dict[str, object]]:
        capture = self.fixture.captures / name
        completed = self.fixture.run(self.fixture.capture_command(capture))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        result = json.loads(completed.stdout)
        self.assertEqual(result["status"], "sealed")
        return capture, result

    def test_capture_and_read_only_audit_bind_exact_provenance(self) -> None:
        root, result = self.successful_capture()
        metadata_path = root / capture_tool.METADATA_NAME
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        self.assertEqual(
            metadata["environment"],
            capture_tool.SAFE_HARNESS_ENVIRONMENT
            | {"WRIC_REPO_ROOT": os.fspath(self.fixture.phase0)},
        )
        self.assertEqual(metadata["affinity"]["pre"], self.fixture.affinity)
        self.assertEqual(metadata["affinity"]["post"], self.fixture.affinity)
        self.assertEqual(metadata["repositories"]["product"]["expected_revision"], self.fixture.product_revision)
        self.assertEqual(metadata["repositories"]["capture_tool"]["expected_revision"], self.fixture.controller_revision)
        self.assertEqual(metadata["harness_provenance"]["kind"], "materialized_compatibility")
        self.assertIsNone(metadata["tracked_git_blobs"]["product_harness"])
        self.assertEqual(metadata["tools"]["compiler"]["nlink"], 4)
        self.assertTrue(
            all(
                snapshot["nlink"] == 1
                for role, snapshot in metadata["tools"].items()
                if role != "compiler"
            )
        )
        self.assertNotEqual(
            metadata["tools"]["product_dagutil"]["inode"],
            metadata["tools"]["frozen_oracle_dagutil"]["inode"],
        )
        self.assertEqual(metadata["capture_outputs"]["row_ids"], ["fixture-row"])
        self.assertEqual(metadata["capture_outputs"]["raw_trial_rows"], 1)
        self.assertEqual(stat.S_IMODE(metadata_path.stat().st_mode), 0o444)
        ledger_text = (root / capture_tool.LEDGER_NAME).read_text(encoding="ascii")
        self.assertIn("directory\t-\t-\t0755\toutputs\n", ledger_text)
        self.assertIn(f"\t{capture_tool.METADATA_NAME}\n", ledger_text)

        audited = self.fixture.run(
            self.fixture.audit_command(root, str(result["ledger_sha256"]))
        )
        self.assertEqual(audited.returncode, 0, audited.stderr)
        self.assertEqual(json.loads(audited.stdout)["status"], "audited")

    def test_preexisting_capture_wrong_outdir_and_duplicate_role_are_rejected(self) -> None:
        occupied = self.fixture.captures / "occupied"
        occupied.mkdir()
        completed = self.fixture.run(self.fixture.capture_command(occupied))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("capture directory already exists", completed.stderr)

        wrong = self.fixture.captures / "wrong"
        command = self.fixture.capture_command(wrong)
        out_index = command.index("--out-dir") + 1
        command[out_index] = os.fspath(self.fixture.captures / "different")
        completed = self.fixture.run(command)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("harness argv out_dir path is not exact", completed.stderr)
        self.assertFalse(wrong.exists())

        duplicate = self.fixture.captures / "duplicate"
        command = self.fixture.capture_command(duplicate)
        command.extend(
            ["--dagutil", os.fspath(self.fixture.product / "build/bin/dagutil")]
        )
        completed = self.fixture.run(command)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("--dagutil exactly once", completed.stderr)
        self.assertFalse(duplicate.exists())

    def test_failed_harness_is_never_sealed(self) -> None:
        self.fixture.set_harness_behavior("fail")
        root = self.fixture.captures / "failed"
        completed = self.fixture.run(self.fixture.capture_command(root))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("benchmark harness failed with status 7", completed.stderr)
        self.assertTrue(root.is_dir())
        self.assertFalse((root / capture_tool.METADATA_NAME).exists())
        self.assertFalse((root / capture_tool.LEDGER_NAME).exists())

        rival_top = self.top / "rival-fixture"
        rival_top.mkdir()
        rival_fixture = Fixture(rival_top)
        rival_fixture.set_harness_behavior("precreate_metadata")
        rival = rival_fixture.captures / "rival-metadata"
        completed = rival_fixture.run(rival_fixture.capture_command(rival))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("run metadata destination already exists", completed.stderr)
        self.assertEqual((rival / capture_tool.METADATA_NAME).read_bytes(), b"rival")
        self.assertFalse((rival / capture_tool.LEDGER_NAME).exists())

    def test_supplement_manifest_chain_is_externally_anchored_and_label_closed(self) -> None:
        supplement, supplement_sha = self.fixture.add_supplement()
        chain, _, _ = capture_tool.manifest_chain(
            self.fixture.phase0,
            self.fixture.base_manifest,
            self.fixture.base_sha256,
            [supplement],
            [supplement_sha],
            self.fixture.artifact_sha256,
        )
        recorded = chain["supplement_manifests"]
        self.assertIsInstance(recorded, list)
        assert isinstance(recorded, list)
        self.assertEqual(len(recorded), 1)
        self.assertEqual(recorded[0]["path"], os.fspath(supplement))
        self.assertEqual(recorded[0]["expected_sha256"], supplement_sha)

        root = self.fixture.captures / "supplement-forbidden-for-label"
        completed = self.fixture.run(self.fixture.capture_command(root))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("not an exact approved component", completed.stderr)
        self.assertFalse(root.exists())

    def test_repository_and_tool_end_drift_are_rejected(self) -> None:
        for name, behavior, expected in (
            ("product-drift", "drift_product", "product repository is not completely clean"),
            ("tool-drift", "drift_tool", "benchmark tool/build identity changed"),
            ("controller-drift", "drift_controller", "capture-tool repository is not completely clean"),
        ):
            with self.subTest(behavior=behavior):
                # Each drift dirties its fixture permanently, so use an isolated
                # nested fixture for the next adversarial launch.
                nested_top = self.top / name
                nested_top.mkdir()
                fixture = Fixture(nested_top)
                fixture.set_harness_behavior(behavior)
                root = fixture.captures / "run"
                completed = fixture.run(fixture.capture_command(root))
                self.assertEqual(completed.returncode, 2)
                self.assertIn(expected, completed.stderr)
                self.assertFalse((root / capture_tool.METADATA_NAME).exists())
                self.assertFalse((root / capture_tool.LEDGER_NAME).exists())

    def test_inherited_benchmark_environment_and_path_aliases_fail_closed(self) -> None:
        root = self.fixture.captures / "environment"
        completed = self.fixture.run(
            self.fixture.capture_command(root), extra_env={"OMP_NUM_THREADS": "8"}
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("inherited benchmark-affecting environment is forbidden", completed.stderr)
        self.assertFalse(root.exists())

        alias = self.top / "harness-alias"
        alias.symlink_to(self.fixture.harness)
        root = self.fixture.captures / "alias"
        command = self.fixture.capture_command(root)
        command[command.index("--benchmark-harness") + 1] = os.fspath(alias)
        completed = self.fixture.run(command)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("symlink or lexical alias", completed.stderr)

        os.link(self.fixture.larch2, self.top / "external-larch2-link")
        root = self.fixture.captures / "hardlink"
        completed = self.fixture.run(self.fixture.capture_command(root))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("frozen larch2 has link count 2", completed.stderr)

    def test_capture_and_metadata_tamper_are_rejected_even_with_new_anchor(self) -> None:
        root, result = self.successful_capture("tamper")
        (root / "outputs").chmod(0o700)
        audited = self.fixture.run(
            self.fixture.audit_command(root, str(result["ledger_sha256"]))
        )
        self.assertEqual(audited.returncode, 2)
        self.assertIn("generic ledger audit failed", audited.stderr)
        (root / "outputs").chmod(0o755)

        for name in (capture_tool.LEDGER_NAME, capture_tool.LEDGER_SEAL_NAME):
            path = root / name
            path.chmod(0o644)
            path.unlink()
        metadata_path = root / capture_tool.METADATA_NAME
        metadata_path.chmod(0o644)
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["unexpected"] = True
        metadata_path.write_text(
            json.dumps(metadata, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
        metadata_path.chmod(0o444)
        sealed = subprocess.run(
            [
                sys.executable,
                os.fspath(self.fixture.ledger),
                "seal",
                "--capture-dir",
                os.fspath(root),
            ],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        )
        self.assertEqual(sealed.returncode, 0, sealed.stderr)
        new_anchor = json.loads(sealed.stdout)["ledger_sha256"]
        audited = self.fixture.run(self.fixture.audit_command(root, new_anchor))
        self.assertEqual(audited.returncode, 2)
        self.assertIn("run metadata has the wrong key set", audited.stderr)

    def test_external_label_revision_and_live_tool_drift_are_rejected(self) -> None:
        root, result = self.successful_capture("external")
        anchor = str(result["ledger_sha256"])
        wrong_label = self.fixture.run(
            self.fixture.audit_command(root, anchor, label="another-label")
        )
        self.assertEqual(wrong_label.returncode, 2)
        self.assertIn("differs from the external expected label", wrong_label.stderr)

        command = self.fixture.audit_command(root, anchor)
        revision_index = command.index("--expected-product-revision") + 1
        command[revision_index] = "0" * 40
        wrong_revision = self.fixture.run(command)
        self.assertEqual(wrong_revision.returncode, 2)
        self.assertIn("path/revision differs from external expectation", wrong_revision.stderr)

        with (self.fixture.product / "build/bin/dagutil").open("a", encoding="utf-8") as stream:
            stream.write("# drift\n")
        tool_drift = self.fixture.run(self.fixture.audit_command(root, anchor))
        self.assertEqual(tool_drift.returncode, 2)
        self.assertIn("product dagutil identity or bytes changed", tool_drift.stderr)

    def test_pre_score_domain_product_harness_cannot_bypass_compatibility_audit(self) -> None:
        root = self.fixture.captures / "historical-exact"
        product_harness = self.fixture.product / "tools/wric_spr_search_benchmark.sh"
        command = self.fixture.capture_command(root)
        command[command.index("--benchmark-harness") + 1] = os.fspath(product_harness)
        command[command.index("--expected-harness-sha256") + 1] = sha256_file(product_harness)
        command[command.index("--expected-harness-metadata-sha256") + 1] = "-"
        completed = self.fixture.run(command)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("pre-timed-trial-digest product revisions require", completed.stderr)
        self.assertFalse(root.exists())

        self.fixture.harness_metadata.unlink()
        root = self.fixture.captures / "missing-compat-metadata"
        completed = self.fixture.run(self.fixture.capture_command(root))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("compatibility harness metadata is missing", completed.stderr)

    def test_affinity_canonicalization_and_start_end_mock_drift(self) -> None:
        self.assertEqual(capture_tool.canonical_affinity({0, 1, 2, 4, 6, 7}), "0-2,4,6-7")
        with mock.patch(
            "wric_benchmark_capture.os.sched_getaffinity",
            side_effect=[{0, 2, 4}, {0, 2}],
        ):
            start = capture_tool.require_affinity("0,2,4")
            end = capture_tool.current_affinity()
        self.assertEqual(start, "0,2,4")
        self.assertNotEqual(start, end)

        out = self.fixture.captures / "affinity-contract"
        tool_paths = capture_tool.build_tool_paths(
            self.fixture.product,
            self.fixture.controller,
            {
                "frozen_larch2": self.fixture.larch2,
                "frozen_oracle_dagutil": self.fixture.oracle,
                "frozen_process_metrics": self.fixture.process_metrics,
            },
            self.fixture.harness,
        )
        with self.assertRaisesRegex(
            capture_tool.CaptureError, "exact unpinned sealed-host affinity 0-15"
        ):
            capture_tool.validate_harness_arguments(
                self.fixture.harness_arguments(out),
                out,
                tool_paths,
                self.fixture.base_manifest,
                [],
                [],
                "final-small-auto",
                capture_tool.PHYSICAL_AFFINITY,
            )


class Phase9BenchmarkCaptureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.top = Path(self.temporary.name).resolve()
        self.fixture = Phase9Fixture(self.top)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def successful_capture(self, name: str) -> tuple[Path, dict[str, object]]:
        root = self.fixture.captures / name
        completed = self.fixture.run(self.fixture.capture_command(root))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return root, json.loads(completed.stdout)

    @staticmethod
    def closure(root: Path) -> dict[str, tuple[str, int]]:
        result: dict[str, tuple[str, int]] = {}
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root).as_posix()
            if path.is_dir():
                result[relative] = ("directory", stat.S_IMODE(path.stat().st_mode))
            else:
                result[relative] = (sha256_file(path), stat.S_IMODE(path.stat().st_mode))
        return result

    def test_gap_free_phase9_seal_then_outer_seal_and_repeatable_audit(self) -> None:
        product_harness = self.fixture.product / "tools/wric_spr_search_benchmark.sh"
        capture_tool.require_timed_trial_digest_fix(
            self.fixture.product,
            self.fixture.product_revision,
            product_harness,
        )
        harness_text = product_harness.read_text(encoding="utf-8")
        fragment = "DEFERRED_SEARCH_SHA[$result_key]=$timed_search"
        self.assertEqual(harness_text.count(fragment), 1)
        product_harness.write_text(
            harness_text.replace(fragment, "DEFERRED_SEARCH_SHA[$result_key]=broken"),
            encoding="utf-8",
        )
        try:
            with self.assertRaisesRegex(
                capture_tool.CaptureError,
                "does not retain the exact timed-trial fix fragment",
            ):
                capture_tool.require_timed_trial_digest_fix(
                    self.fixture.product,
                    self.fixture.product_revision,
                    product_harness,
                )
        finally:
            product_harness.write_text(harness_text, encoding="utf-8")

        root, result = self.successful_capture("phase9")
        inner = root / capture_tool.PHASE9_INNER_NAME
        self.assertTrue((inner / capture_tool.PHASE9_METADATA_NAME).is_file())
        self.assertTrue((inner / capture_tool.PHASE9_LEDGER_NAME).is_file())
        metadata = json.loads(
            (root / capture_tool.METADATA_NAME).read_text(encoding="utf-8")
        )
        post = metadata["postprocessor"]
        self.assertEqual(post["kind"], "phase9")
        self.assertEqual(
            post["ledger_sha256"], result["phase9_ledger_sha256"]
        )
        self.assertEqual(
            post["seal_argv"][2:4], ["--benchmark-dir", os.fspath(inner)]
        )
        before = self.closure(root)
        command = self.fixture.audit_command(
            root,
            str(result["ledger_sha256"]),
            phase9_ledger_sha256=str(result["phase9_ledger_sha256"]),
        )
        for _ in range(2):
            audited = self.fixture.run(command)
            self.assertEqual(audited.returncode, 0, audited.stderr)
            self.assertEqual(json.loads(audited.stdout)["status"], "audited")
        self.assertEqual(self.closure(root), before)

    def test_phase9_mode_omission_wrong_inner_and_extra_inner_fail_closed(self) -> None:
        omitted_root = self.fixture.captures / "omitted"
        omitted = self.fixture.capture_command(omitted_root)
        omitted.remove("--phase9-mode")
        completed = self.fixture.run(omitted)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("phase9 run label and --phase9-mode", completed.stderr)
        self.assertFalse(omitted_root.exists())

        wrong_root = self.fixture.captures / "wrong-inner"
        wrong = self.fixture.capture_command(wrong_root)
        wrong[wrong.index("--out-dir") + 1] = os.fspath(wrong_root)
        completed = self.fixture.run(wrong)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("harness argv out_dir path is not exact", completed.stderr)
        self.assertFalse(wrong_root.exists())

        extra_top = self.top / "extra-fixture"
        extra_top.mkdir()
        extra_fixture = Phase9Fixture(extra_top)
        extra_fixture.set_extra_inner()
        extra_root = extra_fixture.captures / "extra-inner"
        completed = extra_fixture.run(extra_fixture.capture_command(extra_root))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("Phase-9 seal-run result has an unexpected key set", completed.stderr)
        self.assertFalse((extra_root / capture_tool.METADATA_NAME).exists())
        self.assertFalse((extra_root / capture_tool.LEDGER_NAME).exists())

    def test_phase9_external_anchor_and_seal_argv_reordering_are_rejected(self) -> None:
        root, result = self.successful_capture("anchor")
        wrong_anchor = self.fixture.run(
            self.fixture.audit_command(
                root,
                str(result["ledger_sha256"]),
                phase9_ledger_sha256="0" * 64,
            )
        )
        self.assertEqual(wrong_anchor.returncode, 2)
        self.assertIn("differs from its external expected anchor", wrong_anchor.stderr)

        for name in (capture_tool.LEDGER_NAME, capture_tool.LEDGER_SEAL_NAME):
            path = root / name
            path.chmod(0o644)
            path.unlink()
        metadata_path = root / capture_tool.METADATA_NAME
        metadata_path.chmod(0o644)
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        argv = metadata["postprocessor"]["seal_argv"]
        argv[2], argv[4] = argv[4], argv[2]
        metadata_path.write_text(
            json.dumps(metadata, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
        metadata_path.chmod(0o444)
        sealed = subprocess.run(
            [
                sys.executable,
                os.fspath(self.fixture.ledger),
                "seal",
                "--capture-dir",
                os.fspath(root),
            ],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        )
        self.assertEqual(sealed.returncode, 0, sealed.stderr)
        new_outer_anchor = json.loads(sealed.stdout)["ledger_sha256"]
        audited = self.fixture.run(
            self.fixture.audit_command(
                root,
                new_outer_anchor,
                phase9_ledger_sha256=str(result["phase9_ledger_sha256"]),
            )
        )
        self.assertEqual(audited.returncode, 2)
        self.assertIn("Phase-9 seal-run argv is not exactly derivable", audited.stderr)


if __name__ == "__main__":
    unittest.main()
