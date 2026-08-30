#!/usr/bin/env python3
"""Positive, blocked, and planted-negative tests for seven A3 v2 roles."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import plistlib
import py_compile
import signal
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import test_gpu_first_visible_a3_acceptance as fixture  # noqa: E402
import gpu_first_visible_a3_role_producer as producer_support  # noqa: E402
import gpu_first_visible_a3_build_verifier as build_verifier  # noqa: E402

PRODUCERS = {
    "pulp-standalone": SCRIPT_DIR / "gpu_first_visible_a3_standalone_producer.py",
    "forge-modular-standalone": SCRIPT_DIR / "gpu_first_visible_a3_forge_producer.py",
    "forge-modular-auv2-logic": SCRIPT_DIR / "gpu_first_visible_a3_auv2_logic_producer.py",
    "forge-modular-vst3-reaper": SCRIPT_DIR / "gpu_first_visible_a3_vst3_reaper_producer.py",
    "forge-modular-clap-reaper": SCRIPT_DIR / "gpu_first_visible_a3_clap_reaper_producer.py",
    "headless-reference": SCRIPT_DIR / "gpu_first_visible_a3_headless_producer.py",
    "constrained-adapter": SCRIPT_DIR / "gpu_first_visible_a3_constrained_adapter_producer.py",
}


def assert_entrypoints_ignore_role_support_bytecode() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-a3-role-entrypoint-pyc-") as temporary:
        root = Path(temporary)
        support = root / "gpu_first_visible_a3_role_producer.py"
        safe = b"def main_entry(role):\n    return 0\n"
        malicious = b'raise SystemExit("planted role-support bytecode executed")\n'
        malicious += b"#" * (len(safe) - len(malicious)) if len(safe) >= len(malicious) else b""
        if len(malicious) > len(safe):
            safe += b"#" * (len(malicious) - len(safe))
        support.write_bytes(malicious)
        timestamp = int(support.stat().st_mtime)
        cache = Path(importlib.util.cache_from_source(str(support)))
        cache.parent.mkdir(parents=True)
        py_compile.compile(
            str(support), cfile=str(cache), doraise=True,
            invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP,
        )
        support.write_bytes(safe)
        os.utime(support, (timestamp, timestamp))
        environment = dict(os.environ)
        environment["PULP_A3_ROLE_PRODUCER_SUPPORT"] = str(support)
        for role, entrypoint in PRODUCERS.items():
            completed = subprocess.run(
                [sys.executable, str(entrypoint)], env=environment,
                text=True, capture_output=True, check=False,
            )
            assert completed.returncode == 0, (role, completed.stdout, completed.stderr)
PREFIX = {
    "pulp-standalone": "PULP_A3_STANDALONE",
    "forge-modular-standalone": "PULP_A3_FORGE",
    "forge-modular-auv2-logic": "PULP_A3_LOGIC",
    "forge-modular-vst3-reaper": "PULP_A3_REAPER_VST3",
    "forge-modular-clap-reaper": "PULP_A3_REAPER_CLAP",
    "headless-reference": "PULP_A3_HEADLESS",
    "constrained-adapter": "PULP_A3_CONSTRAINED",
}
SOURCE_ROLE = {
    "pulp-standalone": "standalone",
    "forge-modular-standalone": "forge",
    "forge-modular-auv2-logic": "daw",
    "forge-modular-vst3-reaper": "daw",
    "forge-modular-clap-reaper": "daw",
    "headless-reference": "headless-constrained",
    "constrained-adapter": "standalone",
}
FORGE_ROLES = frozenset(role for role in PRODUCERS if role.startswith("forge-modular-"))
REAPER_ROLES = frozenset({"forge-modular-vst3-reaper", "forge-modular-clap-reaper"})


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_driver(path: Path) -> None:
    source = r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--request", required=True, type=Path)
parser.add_argument("--receipt", required=True, type=Path)
args = parser.parse_args()
request = json.loads(args.request.read_text())
mutation = os.environ.get("PULP_A3_TEST_ROLE_MUTATION", "")
source = Path(os.environ["PULP_A3_TEST_ROLE_FIXTURE"])
root = Path(request["artifact_directory"])
root.mkdir(parents=True, exist_ok=True)
role = request["role"]
source_role = {
    "pulp-standalone": "standalone",
    "forge-modular-standalone": "forge",
    "forge-modular-auv2-logic": "daw",
    "forge-modular-vst3-reaper": "daw",
    "forge-modular-clap-reaper": "daw",
    "headless-reference": "headless-constrained",
    "constrained-adapter": "standalone",
}[role]

artifacts = {name: None for name in (
    "health_result", "raw_cold", "raw_warm", "trace"
)}
outcome = "inconclusive" if mutation == "driver-nonpass" else "pass"
reason = "external UI driver is unavailable" if outcome != "pass" else None
dependencies = ["ui-driver:unavailable"] if outcome != "pass" else []

if outcome == "pass":
    sources = {
        "health_result": source / f"{source_role}-health.json",
        "raw_cold": source / f"{source_role}-cold.json",
        "raw_warm": source / f"{source_role}-warm.json",
        "trace": source / f"{source_role}-trace.pftrace",
    }
    for name, source_path in sources.items():
        destination = root / f"{name}{source_path.suffix}"
        shutil.copyfile(source_path, destination)
        if name in {"raw_cold", "raw_warm"}:
            payload = json.loads(destination.read_text())
            payload["identity"] = request["identity"]
            if name == "raw_warm":
                cold_path = root / artifacts["raw_cold"]["path"]
                cold_samples = json.loads(cold_path.read_text())["samples"]
                for index, sample in enumerate(payload["samples"]):
                    sample["process_id"] = cold_samples[index]["process_id"]
            destination.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        artifacts[name] = {
            "path": destination.relative_to(root).as_posix(),
            "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
        }

    def mutate_json(name, callback):
        path = root / artifacts[name]["path"]
        payload = json.loads(path.read_text())
        callback(payload)
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        artifacts[name]["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()

    if mutation == "nine-cold":
        mutate_json("raw_cold", lambda payload: payload["samples"].pop())
    elif mutation == "visible-no-present":
        mutate_json(
            "health_result",
            lambda payload: payload["startup"]["trials"][0].__setitem__(
                "present_ms", None,
            ),
        )
    elif mutation == "headless-present":
        mutate_json(
            "health_result",
            lambda payload: payload["startup"]["trials"][0].__setitem__(
                "present_ms", 1,
            ),
        )
    elif mutation == "fresh-process-reuse":
        def reuse_prior_process(payload):
            payload["samples"][0]["cache_provenance"] = "explicit-cache-reset"
            payload["samples"][1]["process_id"] = payload["samples"][0]["process_id"]
        mutate_json("raw_cold", reuse_prior_process)
        mutate_json(
            "raw_warm",
            lambda payload: payload["samples"][1].__setitem__(
                "process_id", payload["samples"][0]["process_id"],
            ),
        )
    elif mutation == "snapshot-mutation":
        snapshot = root.parent / "identity" / "product.bin"
        snapshot.chmod(0o755)
        snapshot.write_bytes(b"mutated snapshot")
    elif mutation == "bundle-mutation":
        resource = (
            Path(os.environ["PULP_A3_REAPER_PLUGIN_BUNDLE"])
            / "Contents" / "Resources" / "changed.txt"
        )
        resource.parent.mkdir(parents=True, exist_ok=True)
        resource.write_text("changed during lifecycle\n")
    elif mutation == "forge-bundle-mutation":
        resource = (
            Path(os.environ["PULP_A3_FORGE_APP_BUNDLE"])
            / "Contents" / "Resources" / "changed.txt"
        )
        resource.parent.mkdir(parents=True, exist_ok=True)
        resource.write_text("changed during lifecycle\n")
    elif mutation == "pulp-source-mutation":
        (Path(os.environ["PULP_A3_PULP_ROOT"]) / "untracked-runtime-input.txt").write_text(
            "must not be ignored\n"
        )

cold = json.loads((root / artifacts["raw_cold"]["path"]).read_text())["samples"] if outcome == "pass" else []
warm = json.loads((root / artifacts["raw_warm"]["path"]).read_text())["samples"] if outcome == "pass" else []
lifecycle = []
trace_host_pid = None
hosts = {}
if outcome == "pass":
    challenge = request["liveness_challenge"]
    directory = Path(challenge["directory"])
    for row in cold + warm:
        if row["process_id"] not in hosts:
            hosts[row["process_id"]] = subprocess.Popen(
                [request["host"]["runtime_path"]], stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
    try:
        for index, row in enumerate(cold + warm):
            warm_row = index >= 10
            host_process = hosts[row["process_id"]]
            challenged_pid = os.getpid() if mutation == "liveness-stale-host" and index == 0 else host_process.pid
            challenge_path = directory / f"challenge-{index:02}.json"
            temporary = challenge_path.with_suffix(".tmp")
            temporary.write_text(json.dumps({
                "schema": challenge["schema"],
                "version": challenge["version"],
                "attempt_nonce": challenge["attempt_nonce"],
                "challenge_nonce": challenge["challenge_nonce"],
                "sequence": index,
                "process_id": row["process_id"],
                "host_pid": challenged_pid,
            }, indent=2, sort_keys=True) + "\n")
            os.replace(temporary, challenge_path)
            ack_path = directory / f"ack-{index:02}.json"
            deadline = time.monotonic() + 3
            while not ack_path.is_file() and time.monotonic() < deadline:
                time.sleep(0.01)
            if not ack_path.is_file():
                raise RuntimeError("producer did not acknowledge the live host")
            ack = json.loads(ack_path.read_text())
            lifecycle.append({
                "sequence": index,
                "cache_state": "warm" if warm_row else "cold",
                "lifecycle_id": row["lifecycle_id"],
                "process_id": row["process_id"],
                "host_pid": host_process.pid,
                "process_start_identity": ack["process_start_identity"],
                "executable_sha256": ack["executable_sha256"],
                "cache_boundary": row["cache_provenance"],
                "prior_lifecycle_id": cold[index - 10]["lifecycle_id"] if warm_row else None,
                "prior_process_id": row["process_id"] if warm_row else None,
                "endpoint_observed": True,
                "native_presented": request["role"] != "headless-reference",
            })
        trace_host_pid = hosts[cold[0]["process_id"]].pid
        trace_pid_path = Path(os.environ["PULP_A3_TEST_TRACE_PROCESS_PID_FILE"])
        trace_pid_path.write_text(
            str(trace_host_pid) + "\n"
        )
        trace_evidence_id = hashlib.sha256((
            "pulp-a3-live-trace-v1\0" + request["attempt_nonce"] + "\0"
            + challenge["challenge_nonce"] + "\0" + cold[0]["process_id"]
        ).encode()).hexdigest()[:32]
        if mutation != "trace-lifetime-id":
            mutate_json(
                "health_result",
                lambda payload: payload["startup"]["correlation"].__setitem__(
                    "gpu_evidence_id", trace_evidence_id,
                ),
            )
        health_payload = json.loads(
            (root / artifacts["health_result"]["path"]).read_text()
        )
        trace_pid_path.with_suffix(".evidence-id").write_text(
            health_payload["startup"]["correlation"]["gpu_evidence_id"] + "\n"
        )
    finally:
        for index, process in enumerate(hosts.values()):
            if mutation == "detached-host" and index == 0:
                Path(os.environ["PULP_A3_TEST_DETACHED_HOST_MARKER"]).write_text(
                    str(process.pid) + "\n"
                )
                continue
            try:
                os.killpg(process.pid, 15)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, 9)
                process.wait(timeout=2)
if mutation == "product-mutation":
    Path(request["product"]["runtime_path"]).write_bytes(b"mutated")
if mutation == "lifecycle" and lifecycle:
    lifecycle[10]["prior_lifecycle_id"] = None
elif mutation == "unknown-predecessor" and lifecycle:
    lifecycle[10]["prior_lifecycle_id"] = "unobserved-lifecycle"
elif mutation == "prior-process" and lifecycle:
    lifecycle[10]["prior_process_id"] = "unrelated-process"
elif mutation == "lifecycle-raw-mismatch" and lifecycle:
    lifecycle[-1]["lifecycle_id"] = "unrelated-lifecycle"

receipt = {
    "schema": "pulp.gpu-first-visible-role-driver-receipt.v1",
    "version": 1,
    "attempt_nonce": request["attempt_nonce"],
    "role": request["role"],
    "outcome": outcome,
    "reason": reason,
    "dependencies": dependencies,
    "identity": request["identity"],
    "measurement_endpoint": request["measurement_endpoint"],
    "product_sha256": request["product"]["sha256"],
    "host_sha256": request["host"]["sha256"],
    "driver_sha256": request["driver_sha256"],
    "trace_host_pid": trace_host_pid,
    "lifecycle_provenance": lifecycle,
    "artifacts": artifacts,
}
args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
if mutation == "driver-exit":
    raise SystemExit(1)
raise SystemExit({"pass": 0, "inconclusive": 2}[outcome])
'''
    path.write_text(source, encoding="utf-8")
    path.chmod(0o755)


def write_build_driver(path: Path) -> None:
    source = (
        r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, plistlib, socket
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--request", required=True, type=Path)
parser.add_argument("--receipt", required=True, type=Path)
args = parser.parse_args()
request = json.loads(args.request.read_text())
output = Path(request["output_directory"])
output.mkdir(parents=True)
identity = request["identity"]
product_identity = {
    key: identity[key] for key in (
        "pulp_revision", "forge_revision", "build_id", "product_id",
        "product_name", "plugin_format",
    )
}
marker_payload = json.dumps({
    "schema": "pulp.gpu-first-visible-embedded-build.v1",
    "version": 1,
    "product_identity": product_identity,
}, sort_keys=True, separators=(",", ":")).encode()
marker = (
    b"\0PULP_A3_BUILD_IDENTITY_V1:"
    + f"{len(marker_payload):08x}".encode() + b":" + marker_payload
    + b":END_PULP_A3_BUILD_IDENTITY\0"
)
role = request["role"]
if role in {"forge-modular-vst3-reaper", "forge-modular-clap-reaper"}:
    bundle = output / ("plugin.vst3" if identity["plugin_format"] == "vst3" else "plugin.clap")
    product = bundle / "Contents" / "MacOS" / "plugin-product"
elif role == "forge-modular-standalone":
    bundle = output / "Forge.app"
    product = bundle / "Contents" / "MacOS" / "Forge FX"
else:
    bundle = None
    product = output / f"{role}-product"
product.parent.mkdir(parents=True, exist_ok=True)
template = (
    Path(request["source_roots"]["pulp"]["path"])
    / "tools" / "testing" / "a3" / "host-template"
)
if "source-build-read-measured" in request["attempt_nonce"]:
    measured = Path(__PULP_A3_FORBIDDEN_ROOT__) / f"{role}-product"
    product.write_bytes(measured.read_bytes())
else:
    product.write_bytes(template.read_bytes() + marker)
if "source-build-network" in request["attempt_nonce"]:
    socket.create_connection(("1.1.1.1", 80), timeout=1).close()
if "source-build-mismatch" in request["attempt_nonce"]:
    product.write_bytes(b"source-built bytes differ")
product.chmod(0o755)
if role == "forge-modular-standalone":
    (bundle / "Contents" / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleExecutable": product.name,
        "CFBundleIdentifier": identity["product_id"],
        "CFBundleName": identity["product_name"],
    }, fmt=plistlib.FMT_BINARY, sort_keys=True))
if "source-build-extra-output" in request["attempt_nonce"]:
    (output / "unrelated-build-log.txt").write_text("must not be retained\n")

def tree_digest(root):
    entries = []
    for current, _, files in os.walk(root):
        for filename in files:
            member = Path(current) / filename
            entries.append((
                member.relative_to(root).as_posix(), member.read_bytes(),
                member.stat().st_mode & 0o777,
            ))
    digest = hashlib.sha256(b"pulp-directory-tree-v1\0")
    for relative, data, mode in sorted(entries):
        encoded = relative.encode()
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        digest.update(mode.to_bytes(4, "big"))
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()

receipt = {
    "schema": "pulp.gpu-first-visible-source-build-receipt.v1",
    "version": 1,
    "attempt_nonce": request["attempt_nonce"],
    "role": role,
    "outcome": "pass",
    "reason": None,
    "identity": identity,
    "source_revisions": {
        owner: value["revision"] for owner, value in request["source_roots"].items()
    },
    "build_command": ["fixture-source-build", role],
    "builder_id": "pulp-a3-source-build-fixture",
    "build_started_utc": "2026-08-29T06:00:00Z",
    "build_finished_utc": "2026-08-29T06:01:00Z",
    "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    "product_path": product.relative_to(output).as_posix(),
    "product_sha256": hashlib.sha256(product.read_bytes()).hexdigest(),
    "bundle_path": bundle.relative_to(output).as_posix() if bundle else None,
    "bundle_tree_sha256": tree_digest(bundle) if bundle else None,
}
if "source-build-output-symlink" in request["attempt_nonce"]:
    real_output = output.with_name("output-real")
    output.rename(real_output)
    output.symlink_to(real_output, target_is_directory=True)
args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
'''
    )
    path.write_text(
        source.replace("__PULP_A3_FORBIDDEN_ROOT__", repr(str(path.parent))),
        encoding="utf-8",
    )
    path.chmod(0o755)


def write_smoke(path: Path) -> None:
    path.write_text(
        "#!/bin/sh\n"
        "case \"${PULP_A3_TEST_REAPER_SMOKE:-pass}\" in\n"
        "  pass) exit 0 ;; skip) exit 2 ;; inconclusive) exit 3 ;; *) exit 1 ;;\n"
        "esac\n",
        encoding="utf-8",
    )
    path.chmod(0o755)
    (path.parent / "insert_and_float.lua").write_text("-- fixture\n", encoding="utf-8")


def write_host_template(path: Path) -> None:
    source = path.with_suffix(".c")
    source.write_text(
        "#include <signal.h>\n#include <unistd.h>\n"
        "int main(void) { for (;;) pause(); }\n",
        encoding="utf-8",
    )
    compiler = shutil.which("cc")
    assert compiler is not None
    subprocess.run([compiler, str(source), "-o", str(path)], check=True)
    path.chmod(0o755)


def write_analyzer(path: Path) -> None:
    path.write_text(
        r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, shutil, subprocess, sys, tarfile
from pathlib import Path

if len(sys.argv) > 1 and sys.argv[1] == "prepare":
    prepare = argparse.ArgumentParser()
    prepare.add_argument("prepare")
    prepare.add_argument("--workspace", required=True, type=Path)
    prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--receipt", required=True, type=Path)
    prepared = prepare.parse_args()
    prepared.workspace.mkdir(parents=True)
    shutil.copyfile(Path(__file__), prepared.output)
    prepared.output.chmod(0o500)
    root = Path(os.environ["PULP_A3_PULP_ROOT"])
    executable = Path(sys.executable).resolve()
    retained_tools = prepared.workspace / "retained-tools"
    retained_tools.mkdir()
    retained_executable = retained_tools / executable.name
    shutil.copyfile(executable, retained_executable)
    retained_executable.chmod(0o500)
    tool = {
        "command_path": str(executable),
        "resolved_path": str(executable),
        "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "version": "Python fixture toolchain",
        "retained_path": str(retained_executable),
        "retained_sha256": hashlib.sha256(retained_executable.read_bytes()).hexdigest(),
    }
    prefixes = (
        "experimental/pulp-rs", ".agents/skills/trace-sql",
        "tools/packages/tool-registry.json",
        "tools/scripts/release_product_matrix.json",
        "tools/import-design/browser_capture/runtime_manifest.txt",
    )
    files = subprocess.run(
        ["/usr/bin/git", "-C", str(root), "ls-tree", "-r", "--name-only",
         os.environ["PULP_A3_PULP_REVISION"], "--", *prefixes],
        check=True, text=True, stdout=subprocess.PIPE,
    ).stdout.splitlines()
    source_files = {
        relative: hashlib.sha256(subprocess.run(
            ["/usr/bin/git", "-C", str(root), "show",
             os.environ["PULP_A3_PULP_REVISION"] + ":" + relative],
            check=True, stdout=subprocess.PIPE,
        ).stdout).hexdigest()
        for relative in files
    }
    source_snapshot = prepared.workspace / "source-snapshot.tar"
    with tarfile.open(source_snapshot, "w") as archive:
        for relative in files:
            archive.add(root / relative, arcname=relative)
    mutation = next(
        (name for name in ("prepared-analyzer-source", "prepared-analyzer-target")
         if name in str(prepared.workspace)),
        "",
    )
    if mutation == "prepared-analyzer-source":
        source_files["experimental/pulp-rs/Cargo.lock"] = "0" * 64
    payload = {
        "schema": "pulp.gpu-first-visible-prepared-trace-analyzer.v1",
        "version": 1,
        "pulp_revision": os.environ["PULP_A3_PULP_REVISION"],
        "source_files": source_files,
        "source_snapshot_sha256": hashlib.sha256(source_snapshot.read_bytes()).hexdigest(),
        "cargo": tool,
        "rustc": tool,
        "cargo_home_mode": "fresh-config-free-linked-locked-cache",
        "target_directory_fresh": mutation != "prepared-analyzer-target",
        "analyzer_sha256": hashlib.sha256(prepared.output.read_bytes()).hexdigest(),
    }
    prepared.receipt.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    raise SystemExit(0)

parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("question")
parser.add_argument("--trace", required=True, type=Path)
parser.add_argument("--json", action="store_true")
args = parser.parse_args()
trace = args.trace.read_bytes()
if trace == b"not-a-perfetto-trace":
    payload = {
        "schema": "pulp.trace-gpu-analysis.v1",
        "question": "gpu-startup",
        "verdict": "unavailable",
        "capture_complete": False,
        "evidence_ids": [],
    }
    exit_code = 2
else:
    evidence_id = Path(
        os.environ["PULP_A3_TEST_TRACE_PROCESS_PID_FILE"]
    ).with_suffix(".evidence-id").read_text().strip()
    mutation = os.environ.get("PULP_A3_TEST_ROLE_MUTATION")
    if mutation == "trace-id":
        evidence_id = "f" * 32
    if mutation == "analyzer-artifact-mutation":
        health = args.trace.parent / "health_result.json"
        health.write_text("{}\n")
    process_pid = int(
        Path(os.environ["PULP_A3_TEST_TRACE_PROCESS_PID_FILE"]).read_text().strip()
    )
    payload = {
        "schema": "pulp.trace-gpu-analysis.v1",
        "question": "gpu-startup",
        "verdict": "fail" if mutation == "analyzer-fail" else "unverified",
        "capture_complete": True,
        "evidence_ids": [evidence_id],
        "category_scope": {
            "evidence_id": evidence_id,
            "process_upid": 7,
            "process_pid": (
                process_pid + 1000 if mutation == "analyzer-process" else process_pid
            ),
        },
    }
    if mutation == "analyzer-scope":
        payload["category_scope"] = {"evidence_id": evidence_id}
    exit_code = 1 if mutation == "analyzer-fail" else 2
print(json.dumps(payload, sort_keys=True))
raise SystemExit(exit_code)
''',
        encoding="utf-8",
    )
    path.chmod(0o755)


def make_pulp_root(
    path: Path, smoke: Path, driver: Path, build_driver: Path,
    host_template: Path, analyzer: Path,
) -> str:
    path.mkdir()
    sources = {
        "tools/scripts/gpu_first_visible_a3_role_producer.py": (
            SCRIPT_DIR / "gpu_first_visible_a3_role_producer.py"
        ),
        "tools/testing/daw-smoke/reaper_smoke.py": smoke,
        "tools/testing/daw-smoke/insert_and_float.lua": (
            smoke.parent / "insert_and_float.lua"
        ),
        "tools/testing/a3/role-driver.py": driver,
        "tools/testing/a3/source-build-driver.py": build_driver,
        "tools/testing/a3/host-template": host_template,
        "tools/scripts/gpu_first_visible_a3_trace_analyzer.py": analyzer,
        "tools/scripts/gpu_first_visible_a3_build_verifier.py": (
            SCRIPT_DIR / "gpu_first_visible_a3_build_verifier.py"
        ),
    }
    for relative, source in sources.items():
        destination = path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for relative in (
        "experimental/pulp-rs/Cargo.toml",
        "experimental/pulp-rs/Cargo.lock",
    ):
        destination = path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(f"fixture:{relative}\n", encoding="utf-8")
    subprocess.run(["git", "init", "-q"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.email", "a3@example.invalid"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.name", "A3 Test"], cwd=path, check=True)
    subprocess.run(["git", "add", "."], cwd=path, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "fixture"], cwd=path, check=True)
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()


def make_git_root(path: Path) -> str:
    path.mkdir()
    subprocess.run(["git", "init", "-q"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.email", "a3@example.invalid"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.name", "A3 Test"], cwd=path, check=True)
    (path / "source.txt").write_text("forge source\n", encoding="utf-8")
    subprocess.run(["git", "add", "source.txt"], cwd=path, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "fixture"], cwd=path, check=True)
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()


def process_is_live(pid: int) -> bool:
    completed = subprocess.run(
        ["ps", "-p", str(pid), "-o", "stat="], text=True,
        capture_output=True, check=False,
    )
    state = completed.stdout.strip()
    return bool(state) and not state.startswith("Z")


def assert_signal_cleanup(root: Path) -> None:
    marker = root / "signal-child-pids.json"
    child = root / "signal-child.py"
    child.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, pathlib, subprocess, sys, time\n"
        "grandchild = subprocess.Popen(\n"
        "    [sys.executable, '-c', 'import time; time.sleep(60)'])\n"
        "pathlib.Path(os.environ['PULP_A3_SIGNAL_MARKER']).write_text(\n"
        "    json.dumps([os.getpid(), grandchild.pid]) + '\\n')\n"
        "time.sleep(60)\n",
        encoding="utf-8",
    )
    child.chmod(0o755)
    helper = r'''
import os, pathlib, sys
sys.path.insert(0, sys.argv[1])
import gpu_first_visible_a3_role_producer as producer
producer.install_signal_handlers()
root = pathlib.Path(sys.argv[2])
producer.bounded_run(
    [sys.executable, sys.argv[3]], cwd=root, environment=dict(os.environ),
    timeout_seconds=60, stdout_path=root / "signal.stdout.log",
    stderr_path=root / "signal.stderr.log",
)
'''
    environment = dict(os.environ)
    environment["PULP_A3_SIGNAL_MARKER"] = str(marker)
    process = subprocess.Popen(
        [sys.executable, "-c", helper, str(SCRIPT_DIR), str(root), str(child)],
        env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + 5
    while (
        not marker.is_file()
        and process.poll() is None
        and time.monotonic() < deadline
    ):
        time.sleep(0.02)
    assert marker.is_file(), process.communicate(timeout=2)
    child_pids = json.loads(marker.read_text(encoding="utf-8"))
    process.send_signal(signal.SIGTERM)
    stdout, stderr = process.communicate(timeout=8)
    assert process.returncode == 128 + signal.SIGTERM, (process.returncode, stdout, stderr)
    deadline = time.monotonic() + 3
    while any(process_is_live(pid) for pid in child_pids) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not any(process_is_live(pid) for pid in child_pids), child_pids


def assert_success_cleanup(root: Path) -> None:
    marker = root / "success-child-pids.json"
    child = root / "success-child.py"
    child.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, pathlib, subprocess, sys\n"
        "grandchild = subprocess.Popen(\n"
        "    [sys.executable, '-c', 'import time; time.sleep(60)'],\n"
        "    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,\n"
        "    stderr=subprocess.DEVNULL)\n"
        "pathlib.Path(os.environ['PULP_A3_SUCCESS_MARKER']).write_text(\n"
        "    json.dumps([os.getpid(), grandchild.pid]) + '\\n')\n",
        encoding="utf-8",
    )
    child.chmod(0o755)
    environment = dict(os.environ)
    environment["PULP_A3_SUCCESS_MARKER"] = str(marker)
    exit_code = producer_support.bounded_run(
        [sys.executable, str(child)], cwd=root, environment=environment,
        timeout_seconds=5, stdout_path=root / "success.stdout.log",
        stderr_path=root / "success.stderr.log",
    )
    assert exit_code == 0
    child_pids = json.loads(marker.read_text(encoding="utf-8"))
    deadline = time.monotonic() + 3
    while any(process_is_live(pid) for pid in child_pids) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not any(process_is_live(pid) for pid in child_pids), child_pids


def run_role(
    root: Path, evidence: Path, role: str, *, mutation: str = "",
    smoke: str = "pass", omit_driver: bool = False,
) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
    pulp_root = root / "pulp-root"
    if mutation == "source-build-source-overlap":
        exclusion = pulp_root / ".git" / "info" / "exclude"
        with exclusion.open("a", encoding="utf-8") as handle:
            handle.write("\n/a3-overlap-negative/\n")
        run_dir = pulp_root / "a3-overlap-negative"
    else:
        run_dir = root / f"run-{role}-{mutation or smoke}"
    artifact_directory = run_dir / "adapter-output" / "artifacts"
    artifact_directory.mkdir(parents=True)
    source_receipt = json.loads((evidence / "fixture-receipt.json").read_text())
    campaign = next(
        item for item in source_receipt["campaigns"]
        if item["role"] == SOURCE_ROLE[role]
    )
    identity = dict(campaign["identity"])
    identity["plugin_format"] = next(iter(producer_support.FORMAT_BY_ROLE[role]))
    identity["pulp_revision"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=pulp_root, text=True,
    ).strip()
    forge_root = root / "forge-root"
    if role in FORGE_ROLES:
        identity["forge_revision"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=forge_root, text=True,
        ).strip()
    else:
        identity["forge_revision"] = None
    request = {
        "schema": "pulp.gpu-first-visible-campaign-request.v1",
        "version": 1,
        "attempt_nonce": f"attempt-{role}-{mutation or smoke}",
        "role": role,
        "identity": identity,
        "measurement_endpoint": producer_support.ENDPOINT_BY_ROLE[role],
        "cold_trial_count": 10,
        "warm_trial_count": 10,
        "cold_cache_provenance": ["fresh-process", "explicit-cache-reset"],
        "warm_cache_provenance": ["same-process-editor-reopen"],
        "require_controls": False,
        "budget": source_receipt["budget"],
        "artifact_directory": str(artifact_directory),
    }
    request_path = run_dir / "request.json"
    receipt_path = artifact_directory / "producer-receipt.json"
    write_json(request_path, request)
    case_name = mutation or smoke
    if role in REAPER_ROLES and mutation != "daw-product-outside-bundle":
        suffix = ".vst3" if identity["plugin_format"] == "vst3" else ".clap"
        bundle = root / f"plugin-{case_name}{suffix}"
        (bundle / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        product = bundle / "Contents" / "MacOS" / "plugin-product"
    elif role in REAPER_ROLES:
        suffix = ".vst3" if identity["plugin_format"] == "vst3" else ".clap"
        bundle = root / f"plugin-{case_name}{suffix}"
        (bundle / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        bundled_product = bundle / "Contents" / "MacOS" / "bundled-product"
        bundled_product.write_bytes(b"actual bundled product")
        bundled_product.chmod(0o755)
        product = root / "plugin-product-outside-bundle"
    elif role == "forge-modular-standalone":
        bundle = root / f"Forge-{case_name}.app"
        (bundle / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        product = bundle / "Contents" / "MacOS" / "Forge FX"
    else:
        product = root / f"{role}-product"
    if role in REAPER_ROLES:
        host = root / "daw-host"
    elif mutation in {"forge-separate-host", "single-executable-host"}:
        host = root / f"{role}-host-outside-bundle"
    else:
        host = product
    embedded_identity = {
        key: identity[key] for key in (
            "pulp_revision", "forge_revision", "build_id", "product_id",
            "product_name", "plugin_format",
        )
    }
    if mutation == "embedded-build-identity":
        embedded_identity["pulp_revision"] = "0" * 40
    host_template = pulp_root / "tools/testing/a3/host-template"
    product.write_bytes(
        host_template.read_bytes() + build_verifier.encode_marker(embedded_identity)
    )
    if host != product:
        host.write_bytes(host_template.read_bytes())
    product.chmod(0o755)
    host.chmod(0o755)
    if role == "forge-modular-standalone":
        plist_payload = {
            "CFBundleExecutable": product.name,
            "CFBundleIdentifier": identity["product_id"],
            "CFBundleName": identity["product_name"],
        }
        if mutation == "forge-plist-executable":
            plist_payload["CFBundleExecutable"] = "Unrelated Executable"
        (bundle / "Contents" / "Info.plist").write_bytes(
            plistlib.dumps(plist_payload, fmt=plistlib.FMT_BINARY, sort_keys=True)
        )
    if role in REAPER_ROLES and mutation == "daw-extra-executable":
        extra_executable = bundle / "Contents" / "MacOS" / "other-product"
        extra_executable.write_bytes(b"unrelated executable")
        extra_executable.chmod(0o755)
    driver = pulp_root / "tools/testing/a3/role-driver.py"
    analyzer = pulp_root / "tools/scripts/gpu_first_visible_a3_trace_analyzer.py"
    health = json.loads((evidence / f"{SOURCE_ROLE[role]}-health.json").read_text(encoding="utf-8"))
    gpu_evidence_id = health["startup"]["correlation"]["gpu_evidence_id"]
    provenance = root / f"{role}-{case_name}-build-provenance.receipt"
    bundle_digest = (
        producer_support.directory_digest(bundle, f"{role} fixture bundle")
        if role in REAPER_ROLES or role == "forge-modular-standalone" else None
    )
    provenance_payload = {
        "schema": "pulp.gpu-first-visible-local-build-provenance.v1",
        "version": 1,
        "provenance_kind": "local-clean-exact-head-build-receipt",
        "product_identity": {
            key: identity[key] for key in (
                "pulp_revision", "forge_revision", "build_id", "product_id",
                "product_name", "plugin_format",
            )
        },
        "source_revisions": {
            "pulp": identity["pulp_revision"],
            "forge": identity["forge_revision"],
        },
        "source_worktree_status": {
            "pulp": "clean",
            "forge": "clean" if identity["forge_revision"] is not None else "not-applicable",
        },
        "product_sha256": digest(product),
        "bundle_tree_sha256": bundle_digest,
        "driver_sha256": digest(driver),
        "trace_analyzer_sha256": digest(analyzer),
        "build_command": ["cmake", "--build", "build-a3-release", "--target", role],
        "builder_id": "pulp-a3-fixture",
        "build_started_utc": "2026-08-29T06:00:00Z",
        "build_finished_utc": "2026-08-29T06:01:00Z",
    }
    if mutation == "build-provenance":
        provenance_payload["source_revisions"]["pulp"] = "0" * 40
    if mutation == "driver-provenance":
        provenance_payload["driver_sha256"] = "0" * 64
    write_json(provenance, provenance_payload)
    attestation = {
        "schema": "pulp.gpu-first-visible-product-build-attestation.v1",
        "version": 1,
        "product_identity": {
            key: identity[key] for key in (
                "pulp_revision", "forge_revision", "build_id", "product_id",
                "product_name", "plugin_format",
            )
        },
        "product_sha256": digest(product),
        "bundle_tree_sha256": bundle_digest,
        "driver_sha256": digest(driver),
        "trace_analyzer_sha256": digest(analyzer),
        "provenance_kind": "local-clean-exact-head-build-receipt",
        "provenance_receipt_sha256": digest(provenance),
    }
    if mutation == "build-attestation":
        attestation["product_sha256"] = "0" * 64
    attestation_path = root / f"{role}-{case_name}-build-attestation.json"
    write_json(attestation_path, attestation)
    detached_host_marker = root / f"{role}-{case_name}-detached-host.pid"
    trace_process_pid_file = root / f"{role}-{case_name}-trace-host.pid"
    environment = dict(os.environ)
    environment.update({
        "PULP_A3_ROLE_PRODUCER_SUPPORT": str(
            SCRIPT_DIR / "gpu_first_visible_a3_role_producer.py"
        ),
        "PULP_A3_PULP_ROOT": str(pulp_root),
        "PULP_A3_TRACE_ANALYZER": str(analyzer),
        "PULP_A3_BUILD_VERIFIER": str(
            pulp_root / "tools/scripts/gpu_first_visible_a3_build_verifier.py"
        ),
        "PULP_A3_TEST_ROLE_FIXTURE": str(evidence),
        "PULP_A3_TEST_ROLE_MUTATION": mutation,
        "PULP_A3_TEST_GPU_EVIDENCE_ID": gpu_evidence_id,
        "PULP_A3_TEST_TRACE_PROCESS_PID_FILE": str(trace_process_pid_file),
        "PULP_A3_TEST_DETACHED_HOST_MARKER": str(detached_host_marker),
        "PULP_A3_TEST_REAPER_SMOKE": smoke,
        f"{PREFIX[role]}_PRODUCT_BIN": str(product),
        f"{PREFIX[role]}_HOST_BIN": str(host),
        f"{PREFIX[role]}_BUILD_ATTESTATION": str(attestation_path),
        f"{PREFIX[role]}_BUILD_PROVENANCE": str(provenance),
        f"{PREFIX[role]}_BUILD_DRIVER": str(
            pulp_root / "tools/testing/a3/source-build-driver.py"
        ),
        f"{PREFIX[role]}_BUILD_DRIVER_SOURCE_OWNER": "pulp",
        f"{PREFIX[role]}_BUILD_DRIVER_SOURCE_PATH": (
            "tools/testing/a3/source-build-driver.py"
        ),
        f"{PREFIX[role]}_DRIVER_SOURCE_OWNER": "pulp",
        f"{PREFIX[role]}_DRIVER_SOURCE_PATH": "tools/testing/a3/role-driver.py",
    })
    if not omit_driver:
        environment[f"{PREFIX[role]}_DRIVER"] = str(driver)
    if role in REAPER_ROLES:
        smoke_lua = (
            root / "unrelated.lua"
            if mutation == "reaper-lua-mismatch"
            else pulp_root / "tools/testing/daw-smoke/insert_and_float.lua"
        )
        if mutation == "reaper-lua-mismatch":
            smoke_lua.write_text("-- unrelated fixture\n", encoding="utf-8")
        environment.update({
            "PULP_A3_REAPER_PLUGIN_BUNDLE": str(bundle),
            "PULP_A3_REAPER_SMOKE": str(
                pulp_root / "tools/testing/daw-smoke/reaper_smoke.py"
            ),
            "PULP_A3_REAPER_SMOKE_LUA": str(smoke_lua),
        })
    if role in FORGE_ROLES:
        environment["PULP_A3_FORGE_ROOT"] = str(forge_root)
    if role == "forge-modular-standalone":
        environment["PULP_A3_FORGE_APP_BUNDLE"] = str(bundle)
    untracked_forge_source = forge_root / "untracked-runtime-input.txt"
    untracked_pulp_source = pulp_root / "untracked-runtime-input.txt"
    if role in FORGE_ROLES and mutation == "forge-untracked-source":
        untracked_forge_source.write_text("must not be ignored\n", encoding="utf-8")
    completed = subprocess.run(
        [str(PRODUCERS[role]), "--request", str(request_path),
         "--receipt", str(receipt_path)],
        env=environment, text=True, capture_output=True, check=False,
    )
    if detached_host_marker.is_file():
        detached_pid = int(detached_host_marker.read_text(encoding="utf-8"))
        try:
            os.killpg(detached_pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    untracked_forge_source.unlink(missing_ok=True)
    untracked_pulp_source.unlink(missing_ok=True)
    receipt = json.loads(receipt_path.read_text()) if receipt_path.is_file() else {}
    return completed, receipt


def main() -> int:
    assert_entrypoints_ignore_role_support_bytecode()
    with tempfile.TemporaryDirectory(prefix="pulp-a3-role-producers-") as temporary:
        root = Path(temporary)
        evidence = root / "fixture"
        evidence.mkdir()
        source_receipt = fixture.make_fixture(evidence)
        write_json(evidence / "fixture-receipt.json", source_receipt)
        write_driver(root / "role-driver.py")
        write_build_driver(root / "source-build-driver.py")
        write_host_template(root / "host-template")
        write_smoke(root / "reaper-smoke")
        write_analyzer(root / "trace-analyzer")
        make_pulp_root(
            root / "pulp-root", root / "reaper-smoke", root / "role-driver.py",
            root / "source-build-driver.py", root / "host-template",
            root / "trace-analyzer",
        )
        make_git_root(root / "forge-root")
        assert_signal_cleanup(root)
        assert_success_cleanup(root)

        for role in PRODUCERS:
            completed, receipt = run_role(root, evidence, role)
            if role == "forge-modular-auv2-logic":
                assert completed.returncode == 2
                assert receipt["outcome"] == "inconclusive"
                assert receipt["dependencies"] == ["host:logic-pro"]
                continue
            if role == "constrained-adapter":
                assert completed.returncode == 2
                assert receipt["outcome"] == "inconclusive"
                assert receipt["dependencies"] == ["product-policy:required-coverage"]
                continue
            assert completed.returncode == 0, (role, completed.stdout, completed.stderr, receipt)
            assert receipt["outcome"] == "pass"
            assert all(receipt["artifacts"].values())
            host_archive = root / f"run-{role}-pass" / receipt["artifacts"]["host_artifact"]["path"]
            assert digest(host_archive) == receipt["artifacts"]["host_artifact"]["sha256"]
            if role in REAPER_ROLES:
                with tarfile.open(host_archive, "r:") as archive:
                    names = set(archive.getnames())
                assert "preflight/receipt.json" in names
                assert "preflight/source-snapshot.tar" in names
            with tarfile.open(host_archive, "r:") as archive:
                names = set(archive.getnames())
            assert "preflight/source-build-pulp-source.tar" in names
            if role in FORGE_ROLES:
                assert "preflight/source-build-forge-source.tar" in names

        negatives = [
            ("pulp-standalone", "nine-cold", "pass", "20 lifecycle"),
            ("pulp-standalone", "visible-no-present", "pass", "native presentation"),
            ("headless-reference", "headless-present", "pass", "cannot claim"),
            ("pulp-standalone", "trace-id", "pass", "pinned trace replay"),
            (
                "pulp-standalone", "trace-lifetime-id", "pass",
                "live-host challenge",
            ),
            ("pulp-standalone", "analyzer-fail", "pass", "pinned trace replay"),
            ("pulp-standalone", "analyzer-scope", "pass", "pinned trace replay"),
            ("pulp-standalone", "analyzer-process", "pass", "pinned trace replay"),
            (
                "pulp-standalone", "prepared-analyzer-source", "pass",
                "not sealed to the requested source/toolchain",
            ),
            (
                "pulp-standalone", "prepared-analyzer-target", "pass",
                "not sealed to the requested source/toolchain",
            ),
            (
                "pulp-standalone", "analyzer-artifact-mutation", "pass",
                "role-driver health_result changed",
            ),
            (
                "pulp-standalone", "fresh-process-reuse", "pass",
                "reuses an earlier process identity",
            ),
            ("pulp-standalone", "lifecycle", "pass", "reopen predecessor"),
            ("pulp-standalone", "unknown-predecessor", "pass", "observed lifecycle"),
            (
                "pulp-standalone", "prior-process", "pass",
                "same-process reopen predecessor",
            ),
            ("pulp-standalone", "lifecycle-raw-mismatch", "pass", "raw observation"),
            ("pulp-standalone", "product-mutation", "pass", "changed during"),
            ("pulp-standalone", "snapshot-mutation", "pass", "changed during"),
            (
                "pulp-standalone", "build-attestation", "pass",
                "does not bind the requested source/build identity",
            ),
            (
                "pulp-standalone", "build-provenance", "pass",
                "does not bind the requested source, product, and driver",
            ),
            (
                "pulp-standalone", "driver-provenance", "pass",
                "does not bind the requested source, product, and driver",
            ),
            (
                "pulp-standalone", "embedded-build-identity", "pass",
                "independent source build differs",
            ),
            (
                "pulp-standalone", "source-build-mismatch", "pass",
                "independent source build differs",
            ),
            (
                "pulp-standalone", "source-build-read-measured", "pass",
                "omitted its receipt",
            ),
            (
                "pulp-standalone", "source-build-network", "pass",
                "omitted its receipt",
            ),
            (
                "pulp-standalone", "source-build-output-symlink", "pass",
                "output root is not a fresh directory",
            ),
            (
                "pulp-standalone", "source-build-source-overlap", "pass",
                "source roots overlap the measured artifact directory",
            ),
            (
                "forge-modular-standalone", "source-build-extra-output", "pass",
                "retained unrelated output",
            ),
            (
                "pulp-standalone", "liveness-stale-host", "pass",
                "wrong live executable",
            ),
            (
                "pulp-standalone", "detached-host", "pass",
                "owned host process IDs remain live",
            ),
            (
                "pulp-standalone", "pulp-source-mutation", "pass",
                "tracked or untracked changes",
            ),
            ("pulp-standalone", "driver-exit", "pass", "exit code disagrees"),
            ("pulp-standalone", "single-executable-host", "pass", "same executable"),
            ("forge-modular-vst3-reaper", "daw-product-outside-bundle", "pass", "sole executable"),
            ("forge-modular-vst3-reaper", "daw-extra-executable", "pass", "sole executable"),
            ("forge-modular-vst3-reaper", "bundle-mutation", "pass", "changed during the campaign"),
            (
                "forge-modular-vst3-reaper", "reaper-lua-mismatch", "pass",
                "helper used by the smoke harness",
            ),
            ("forge-modular-standalone", "forge-separate-host", "pass", "same executable"),
            (
                "forge-modular-standalone", "forge-bundle-mutation", "pass",
                "changed during the campaign",
            ),
            (
                "forge-modular-standalone", "forge-untracked-source", "pass",
                "tracked or untracked changes",
            ),
            (
                "forge-modular-standalone", "forge-plist-executable", "pass",
                "Info.plist does not bind",
            ),
        ]
        for role, mutation, smoke, needle in negatives:
            completed, receipt = run_role(root, evidence, role, mutation=mutation, smoke=smoke)
            assert completed.returncode == 1, (role, mutation, completed.stderr, receipt)
            assert receipt["outcome"] == "fail"
            assert needle in receipt["reason"], (mutation, receipt["reason"])

        completed, receipt = run_role(
            root, evidence, "pulp-standalone", mutation="missing-driver", omit_driver=True,
        )
        assert completed.returncode == 2
        assert receipt["outcome"] == "inconclusive"
        assert receipt["dependencies"] == ["role-driver:pulp-standalone"]

        completed, receipt = run_role(root, evidence, "pulp-standalone", mutation="driver-nonpass")
        assert completed.returncode == 2
        assert receipt["outcome"] == "inconclusive"
        assert receipt["dependencies"] == ["ui-driver:unavailable"]

        completed, receipt = run_role(root, evidence, "forge-modular-vst3-reaper", smoke="skip")
        assert completed.returncode == 3
        assert receipt["outcome"] == "skip"
        assert receipt["dependencies"] == ["reaper:editor-open-smoke"]

        print(
            "gpu-first-visible-a3-role-producers: "
            "positive=5 blocked=2 planted_negatives=39 cleanup_controls=2"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
