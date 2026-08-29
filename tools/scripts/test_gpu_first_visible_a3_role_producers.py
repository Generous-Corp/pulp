#!/usr/bin/env python3
"""Positive and planted-negative tests for the four checked-in A3 producers."""

from __future__ import annotations

import hashlib
import json
import os
import plistlib
import signal
import shutil
import subprocess
import sys
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
    "standalone": SCRIPT_DIR / "gpu_first_visible_a3_standalone_producer.py",
    "headless-constrained": SCRIPT_DIR / "gpu_first_visible_a3_headless_producer.py",
    "daw": SCRIPT_DIR / "gpu_first_visible_a3_reaper_producer.py",
    "forge": SCRIPT_DIR / "gpu_first_visible_a3_forge_producer.py",
}
PREFIX = {
    "standalone": "PULP_A3_STANDALONE",
    "headless-constrained": "PULP_A3_HEADLESS",
    "daw": "PULP_A3_REAPER",
    "forge": "PULP_A3_FORGE",
}


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_driver(path: Path) -> None:
    source = r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, shutil, subprocess, sys
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

artifacts = {name: None for name in (
    "health_result", "raw_cold", "raw_warm", "trace"
)}
outcome = "inconclusive" if mutation == "driver-nonpass" else "pass"
reason = "external UI driver is unavailable" if outcome != "pass" else None
dependencies = ["ui-driver:unavailable"] if outcome != "pass" else []

if outcome == "pass":
    sources = {
        "health_result": source / f"{role}-health.json",
        "raw_cold": source / f"{role}-cold.json",
        "raw_warm": source / f"{role}-warm.json",
        "trace": source / f"{role}-trace.pftrace",
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
    elif mutation == "product-mutation":
        Path(request["product"]["runtime_path"]).write_bytes(b"mutated")
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
host_pid_by_process = {}
for row in cold + warm:
    host_pid_by_process.setdefault(
        row["process_id"], 2_147_000_000 + len(host_pid_by_process),
    )
if mutation == "detached-host" and cold:
    detached = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(60)"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, start_new_session=True,
    )
    host_pid_by_process[cold[0]["process_id"]] = detached.pid
    Path(os.environ["PULP_A3_TEST_DETACHED_HOST_MARKER"]).write_text(
        str(detached.pid) + "\n"
    )
lifecycle = []
for index, row in enumerate(cold + warm):
    warm_row = index >= 10
    lifecycle.append({
        "sequence": index,
        "cache_state": "warm" if warm_row else "cold",
        "lifecycle_id": row["lifecycle_id"],
        "process_id": row["process_id"],
        "host_pid": host_pid_by_process[row["process_id"]],
        "cache_boundary": row["cache_provenance"],
        "prior_lifecycle_id": cold[index - 10]["lifecycle_id"] if warm_row else None,
        "prior_process_id": row["process_id"] if warm_row else None,
        "endpoint_observed": True,
        "native_presented": request["role"] != "headless-constrained",
    })
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


def write_analyzer(path: Path) -> None:
    path.write_text(
        r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, shutil, sys
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
    tool = {
        "command_path": str(executable),
        "resolved_path": str(executable),
        "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "version": "Python fixture toolchain",
    }
    source_files = {
        relative: hashlib.sha256((root / relative).read_bytes()).hexdigest()
        for relative in (
            "experimental/pulp-rs/Cargo.toml",
            "experimental/pulp-rs/Cargo.lock",
        )
    }
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
    evidence_id = os.environ["PULP_A3_TEST_GPU_EVIDENCE_ID"]
    mutation = os.environ.get("PULP_A3_TEST_ROLE_MUTATION")
    if mutation == "trace-id":
        evidence_id = "f" * 32
    if mutation == "analyzer-artifact-mutation":
        health = args.trace.parent / "health_result.json"
        health.write_text("{}\n")
    process_pid = int(os.environ["PULP_A3_TEST_TRACE_PROCESS_PID"])
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


def make_pulp_root(path: Path, smoke: Path, driver: Path, analyzer: Path) -> str:
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
    run_dir = root / f"run-{role}-{mutation or smoke}"
    artifact_directory = run_dir / "adapter-output" / "artifacts"
    artifact_directory.mkdir(parents=True)
    source_receipt = json.loads((evidence / "fixture-receipt.json").read_text())
    campaign = next(item for item in source_receipt["campaigns"] if item["role"] == role)
    identity = dict(campaign["identity"])
    pulp_root = root / "pulp-root"
    identity["pulp_revision"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=pulp_root, text=True,
    ).strip()
    forge_root = root / "forge-root"
    if role == "forge":
        identity["forge_revision"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=forge_root, text=True,
        ).strip()
    request = {
        "schema": "pulp.gpu-first-visible-campaign-request.v1",
        "version": 1,
        "attempt_nonce": f"attempt-{role}",
        "role": role,
        "identity": identity,
        "measurement_endpoint": campaign["measurement_endpoint"],
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
    if role == "daw" and mutation != "daw-product-outside-bundle":
        bundle = root / f"plugin-{case_name}.vst3"
        (bundle / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        product = bundle / "Contents" / "MacOS" / "plugin-product"
    elif role == "daw":
        bundle = root / f"plugin-{case_name}.vst3"
        (bundle / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        bundled_product = bundle / "Contents" / "MacOS" / "bundled-product"
        bundled_product.write_bytes(b"actual bundled product")
        bundled_product.chmod(0o755)
        product = root / "plugin-product-outside-bundle"
    elif role == "forge":
        bundle = root / f"Forge-{case_name}.app"
        (bundle / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        product = bundle / "Contents" / "MacOS" / "Forge FX"
    else:
        product = root / f"{role}-product"
    if role == "daw":
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
    product.write_bytes(
        f"product:{role}".encode() + build_verifier.encode_marker(embedded_identity)
    )
    if host != product:
        host.write_bytes(f"host:{role}".encode())
    product.chmod(0o755)
    host.chmod(0o755)
    if role == "forge":
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
    if role == "daw" and mutation == "daw-extra-executable":
        extra_executable = bundle / "Contents" / "MacOS" / "other-product"
        extra_executable.write_bytes(b"unrelated executable")
        extra_executable.chmod(0o755)
    driver = pulp_root / "tools/testing/a3/role-driver.py"
    analyzer = pulp_root / "tools/scripts/gpu_first_visible_a3_trace_analyzer.py"
    health = json.loads((evidence / f"{role}-health.json").read_text(encoding="utf-8"))
    gpu_evidence_id = health["startup"]["correlation"]["gpu_evidence_id"]
    provenance = root / f"{role}-{case_name}-build-provenance.receipt"
    bundle_digest = (
        producer_support.directory_digest(bundle, f"{role} fixture bundle")
        if role in {"daw", "forge"} else None
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
        "PULP_A3_TEST_TRACE_PROCESS_PID": "2147000000",
        "PULP_A3_TEST_DETACHED_HOST_MARKER": str(detached_host_marker),
        "PULP_A3_TEST_REAPER_SMOKE": smoke,
        f"{PREFIX[role]}_PRODUCT_BIN": str(product),
        f"{PREFIX[role]}_HOST_BIN": str(host),
        f"{PREFIX[role]}_BUILD_ATTESTATION": str(attestation_path),
        f"{PREFIX[role]}_BUILD_PROVENANCE": str(provenance),
        f"{PREFIX[role]}_DRIVER_SOURCE_OWNER": "pulp",
        f"{PREFIX[role]}_DRIVER_SOURCE_PATH": "tools/testing/a3/role-driver.py",
    })
    if not omit_driver:
        environment[f"{PREFIX[role]}_DRIVER"] = str(driver)
    if role == "daw":
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
    if role == "forge":
        environment["PULP_A3_FORGE_ROOT"] = str(forge_root)
        environment["PULP_A3_FORGE_APP_BUNDLE"] = str(bundle)
    untracked_forge_source = forge_root / "untracked-runtime-input.txt"
    untracked_pulp_source = pulp_root / "untracked-runtime-input.txt"
    if role == "forge" and mutation == "forge-untracked-source":
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
    with tempfile.TemporaryDirectory(prefix="pulp-a3-role-producers-") as temporary:
        root = Path(temporary)
        evidence = root / "fixture"
        evidence.mkdir()
        source_receipt = fixture.make_fixture(evidence)
        write_json(evidence / "fixture-receipt.json", source_receipt)
        write_driver(root / "role-driver.py")
        write_smoke(root / "reaper-smoke")
        write_analyzer(root / "trace-analyzer")
        make_pulp_root(
            root / "pulp-root", root / "reaper-smoke", root / "role-driver.py",
            root / "trace-analyzer",
        )
        make_git_root(root / "forge-root")
        assert_signal_cleanup(root)
        assert_success_cleanup(root)

        for role in PRODUCERS:
            completed, receipt = run_role(root, evidence, role)
            assert completed.returncode == 0, (role, completed.stdout, completed.stderr, receipt)
            assert receipt["outcome"] == "pass"
            assert all(receipt["artifacts"].values())
            host_archive = root / f"run-{role}-pass" / receipt["artifacts"]["host_artifact"]["path"]
            assert digest(host_archive) == receipt["artifacts"]["host_artifact"]["sha256"]

        negatives = [
            ("standalone", "nine-cold", "pass", "20 lifecycle"),
            ("standalone", "visible-no-present", "pass", "native presentation"),
            ("headless-constrained", "headless-present", "pass", "cannot claim"),
            ("standalone", "trace-id", "pass", "pinned trace replay"),
            ("standalone", "analyzer-fail", "pass", "pinned trace replay"),
            ("standalone", "analyzer-scope", "pass", "pinned trace replay"),
            ("standalone", "analyzer-process", "pass", "pinned trace replay"),
            (
                "standalone", "prepared-analyzer-source", "pass",
                "not sealed to the requested source/toolchain",
            ),
            (
                "standalone", "prepared-analyzer-target", "pass",
                "not sealed to the requested source/toolchain",
            ),
            (
                "standalone", "analyzer-artifact-mutation", "pass",
                "role-driver health_result changed",
            ),
            (
                "standalone", "fresh-process-reuse", "pass",
                "reuses an earlier process identity",
            ),
            ("standalone", "lifecycle", "pass", "reopen predecessor"),
            ("standalone", "unknown-predecessor", "pass", "observed lifecycle"),
            (
                "standalone", "prior-process", "pass",
                "same-process reopen predecessor",
            ),
            ("standalone", "lifecycle-raw-mismatch", "pass", "raw observation"),
            ("standalone", "product-mutation", "pass", "changed during"),
            ("standalone", "snapshot-mutation", "pass", "changed during"),
            (
                "standalone", "build-attestation", "pass",
                "does not bind the requested source/build identity",
            ),
            (
                "standalone", "build-provenance", "pass",
                "does not bind the requested source, product, and driver",
            ),
            (
                "standalone", "driver-provenance", "pass",
                "does not bind the requested source, product, and driver",
            ),
            (
                "standalone", "embedded-build-identity", "pass",
                "build-verifier result does not bind the closed control",
            ),
            (
                "standalone", "detached-host", "pass",
                "owned host process IDs remain live",
            ),
            (
                "standalone", "pulp-source-mutation", "pass",
                "tracked or untracked changes",
            ),
            ("standalone", "driver-exit", "pass", "exit code disagrees"),
            ("standalone", "single-executable-host", "pass", "same executable"),
            ("daw", "daw-product-outside-bundle", "pass", "sole executable"),
            ("daw", "daw-extra-executable", "pass", "sole executable"),
            ("daw", "bundle-mutation", "pass", "changed during the campaign"),
            (
                "daw", "reaper-lua-mismatch", "pass",
                "helper used by the smoke harness",
            ),
            ("forge", "forge-separate-host", "pass", "same executable"),
            (
                "forge", "forge-bundle-mutation", "pass",
                "changed during the campaign",
            ),
            (
                "forge", "forge-untracked-source", "pass",
                "tracked or untracked changes",
            ),
            (
                "forge", "forge-plist-executable", "pass",
                "Info.plist does not bind",
            ),
        ]
        for role, mutation, smoke, needle in negatives:
            completed, receipt = run_role(root, evidence, role, mutation=mutation, smoke=smoke)
            assert completed.returncode == 1, (role, mutation, completed.stderr, receipt)
            assert receipt["outcome"] == "fail"
            assert needle in receipt["reason"], (mutation, receipt["reason"])

        completed, receipt = run_role(
            root, evidence, "standalone", mutation="missing-driver", omit_driver=True,
        )
        assert completed.returncode == 2
        assert receipt["outcome"] == "inconclusive"
        assert receipt["dependencies"] == ["role-driver:standalone"]

        completed, receipt = run_role(root, evidence, "standalone", mutation="driver-nonpass")
        assert completed.returncode == 2
        assert receipt["outcome"] == "inconclusive"
        assert receipt["dependencies"] == ["ui-driver:unavailable"]

        completed, receipt = run_role(root, evidence, "daw", smoke="skip")
        assert completed.returncode == 3
        assert receipt["outcome"] == "skip"
        assert receipt["dependencies"] == ["reaper:editor-open-smoke"]

        print(
            "gpu-first-visible-a3-role-producers: "
            "positive=4 planted_negatives=36 cleanup_controls=2"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
