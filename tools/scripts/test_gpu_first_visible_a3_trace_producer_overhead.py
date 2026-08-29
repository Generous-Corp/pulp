#!/usr/bin/env python3
"""Positive and planted-negative controls for A3 trace-producer overhead."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import py_compile
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable

import gpu_first_visible_a3_trace_producer_overhead as overhead


def plant_bytecode(source: Path, malicious: bytes, mode: py_compile.PycInvalidationMode) -> None:
    """Plant bytecode that a normal sibling import would accept."""
    original = source.read_bytes()
    if mode == py_compile.PycInvalidationMode.TIMESTAMP:
        if len(malicious) > len(original):
            raise AssertionError("malicious timestamp source exceeds its safe replacement")
        malicious += b"#" * (len(original) - len(malicious))
    source.write_bytes(malicious)
    timestamp = int(source.stat().st_mtime)
    cache = Path(importlib.util.cache_from_source(str(source)))
    cache.parent.mkdir(parents=True, exist_ok=True)
    py_compile.compile(
        str(source), cfile=str(cache), doraise=True, invalidation_mode=mode,
    )
    source.write_bytes(original)
    if mode == py_compile.PycInvalidationMode.TIMESTAMP:
        os.utime(source, (timestamp, timestamp))


def assert_transitive_source_graph_ignores_bytecode() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-a3-overhead-pyc-") as temporary:
        root = Path(temporary)
        for filename in (
            "gpu_first_visible_a3_trace_producer_overhead.py",
            "gpu_first_visible_a3_trace_producer_overhead_analyzer.py",
            "gpu_first_visible_a3_role_producer.py",
        ):
            shutil.copyfile(Path(__file__).with_name(filename), root / filename)
        plant_bytecode(
            root / "gpu_first_visible_a3_trace_producer_overhead_analyzer.py",
            b'raise SystemExit("unchecked analyzer bytecode executed")\n',
            py_compile.PycInvalidationMode.UNCHECKED_HASH,
        )
        plant_bytecode(
            root / "gpu_first_visible_a3_role_producer.py",
            b'raise SystemExit("timestamp role-support bytecode executed")\n',
            py_compile.PycInvalidationMode.TIMESTAMP,
        )
        completed = subprocess.run(
            [sys.executable, str(root / "gpu_first_visible_a3_trace_producer_overhead.py"),
             "--help"],
            text=True, capture_output=True, check=False,
        )
        assert completed.returncode == 0, (completed.stdout, completed.stderr)


def write_collection_host(path: Path) -> None:
    source = path.with_suffix(".c")
    source.write_text(
        "#include <signal.h>\n#include <unistd.h>\n"
        "__attribute__((used)) static const char kTracingSentinel[] = "
        "\"PULP_TRACING_COMPILED_IN__DO_NOT_SHIP\";\n"
        "int main(void) { for (;;) pause(); }\n",
        encoding="utf-8",
    )
    compiler = shutil.which("cc")
    assert compiler is not None
    subprocess.run([compiler, str(source), "-o", str(path)], check=True)
    path.chmod(0o755)


def write_state_build_driver(path: Path, forbidden_root: Path) -> None:
    source = r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, socket, sys
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--request", required=True, type=Path)
parser.add_argument("--receipt", required=True, type=Path)
args = parser.parse_args()
request = json.loads(args.request.read_text())
output = Path(request["output_directory"])
output.mkdir()
source = (
    Path(request["source_directory"])
    / "tools" / "testing" / "a3" / "collection-host-product"
)
product = output / "product"
family = request["identity"]["build_family_id"]
if "state-build-read-measured" in family:
    product.write_bytes((Path(__FORBIDDEN_ROOT__) / "tools" / "testing" / "a3"
                         / "collection-host-product").read_bytes())
elif "state-build-network" in family:
    socket.create_connection(("1.1.1.1", 80), timeout=1).close()
elif "state-build-read-ambient" in family:
    product.write_bytes(Path(__AMBIENT_BUILD_OUTPUT__).read_bytes())
else:
    product.write_bytes(source.read_bytes())
product.chmod(0o755)
tool = Path(sys.executable).resolve()
tool_digest = hashlib.sha256(tool.read_bytes()).hexdigest()
receipt = {
    "schema": "pulp.gpu-first-visible-trace-producer-state-build-receipt.v1",
    "version": 1,
    "attempt_nonce": request["attempt_nonce"],
    "state": request["state"],
    "outcome": "pass",
    "reason": None,
    "source_revision": request["source_revision"],
    "candidate_revision": request["candidate_revision"],
    "source_tree": request["source_tree"],
    "source_tree_sha256": request["source_tree_sha256"],
    "source_archive_sha256": request["source_archive_sha256"],
    "binary_sha256": request["binary_sha256"],
    "identity": request["identity"],
    "tracing": request["tracing"],
    "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    "build_command": [
        str(tool), str(Path(__file__).resolve()), "--request", str(args.request),
        "--receipt", str(args.receipt),
    ],
    "builder_id": "pulp-a3-state-build-fixture",
    "build_started_utc": "2026-08-29T12:00:00Z",
    "build_finished_utc": "2026-08-29T12:00:01Z",
    "toolchain": [{
        "path": str(tool), "sha256": tool_digest, "version": sys.version.split()[0],
    }],
    "product_path": product.relative_to(output).as_posix(),
    "product_sha256": hashlib.sha256(product.read_bytes()).hexdigest(),
}
if "state-build-config-receipt" in family:
    receipt["tracing"]["compiled_in"] = False
args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
'''
    path.write_text(
        source.replace("__FORBIDDEN_ROOT__", repr(str(forbidden_root))).replace(
            "__AMBIENT_BUILD_OUTPUT__",
            repr(str(forbidden_root.parent / "ambient-build-output.bin")),
        ),
        encoding="utf-8",
    )
    path.chmod(0o755)


def write_collection_driver(path: Path) -> None:
    path.write_text(
        r'''#!/usr/bin/env python3
import argparse, hashlib, json, os, signal, subprocess, time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--request", required=True, type=Path)
parser.add_argument("--receipt", required=True, type=Path)
args = parser.parse_args()
request = json.loads(args.request.read_text())
root = Path(request["artifact_directory"])
challenge = request["liveness_challenge"]
challenge_root = Path(challenge["directory"])
binary = Path(request["binary"]["runtime_path"])
driver_sha = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
samples = {"warmups": [], "measured": [], "fresh_process": []}
global_index = 0
for section, count in (("warmups", 5), ("measured", 30), ("fresh_process", 20)):
    for sequence in range(count):
        host = subprocess.Popen(
            [str(binary)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, start_new_session=True,
        )
        try:
            started_text = subprocess.check_output(
                ["ps", "-p", str(host.pid), "-o", "lstart="], text=True,
            )
            process_start_identity = (
                f"pid={host.pid};started={' '.join(started_text.split())}"
            )
            evidence_fields = (
                request["attempt_nonce"], request["challenge_nonce"], request["state"],
                section, str(sequence), str(host.pid), process_start_identity,
                request["binary"]["sha256"],
            )
            evidence_id = hashlib.sha256(
                b"pulp-a3-overhead-live-evidence-v1\0"
                + b"\0".join(field.encode() for field in evidence_fields)
            ).hexdigest()[:32]
            if os.environ.get("PULP_A3_TEST_ARBITRARY_OVERHEAD_ID") == "1":
                evidence_id = f"{global_index + 1:032x}"
            started = time.monotonic_ns()
            finished = started + 10_000_000
            challenge_payload = {
                "schema": "pulp.gpu-first-visible-trace-producer-challenge.v1",
                "version": 1,
                "attempt_nonce": request["attempt_nonce"],
                "challenge_nonce": request["challenge_nonce"],
                "state": request["state"],
                "section": section,
                "sequence": sequence,
                "evidence_id": evidence_id,
                "host_pid": host.pid,
                "audio_thread_tids": [host.pid],
                "started_monotonic_ns": started,
                "finished_monotonic_ns": finished,
                "xrun_count": 0,
            }
            challenge_path = challenge_root / f"challenge-{global_index:02}.json"
            temporary = challenge_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(challenge_payload, sort_keys=True) + "\n")
            os.replace(temporary, challenge_path)
            ack_path = challenge_root / f"ack-{global_index:02}.json"
            deadline = time.monotonic() + 5
            while not ack_path.is_file() and time.monotonic() < deadline:
                time.sleep(0.005)
            if not ack_path.is_file():
                raise RuntimeError("collector did not acknowledge the live host")
            ack = json.loads(ack_path.read_text())
            metrics_path = root / f"metrics-{global_index:02}.json"
            metrics = {
                "schema": "pulp.gpu-first-visible-trace-producer-runtime-metrics.v1",
                "version": 1,
                "state": request["state"],
                "sequence": sequence,
                "evidence_id": evidence_id,
                "host_pid": host.pid,
                "process_start_identity": ack["process_start_identity"],
                "executable_sha256": request["binary"]["sha256"],
                "audio_thread_tids": challenge_payload["audio_thread_tids"],
                "started_monotonic_ns": started,
                "finished_monotonic_ns": finished,
                "xrun_count": 0,
                "collection_challenge_nonce": request["challenge_nonce"],
                "driver_sha256": driver_sha,
            }
            metrics_path.write_text(json.dumps(metrics, sort_keys=True) + "\n")
            samples[section].append({
                "sequence": sequence,
                "evidence_id": evidence_id,
                "duration_ms": 10.0,
                "runtime_metrics": {
                    "path": metrics_path.relative_to(root).as_posix(),
                    "sha256": hashlib.sha256(metrics_path.read_bytes()).hexdigest(),
                },
                "trace": None,
            })
        finally:
            try:
                os.killpg(host.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            host.wait(timeout=3)
        global_index += 1
receipt = {
    "schema": "pulp.gpu-first-visible-trace-producer-driver-receipt.v1",
    "version": 1,
    "attempt_nonce": request["attempt_nonce"],
    "challenge_nonce": request["challenge_nonce"],
    "state": request["state"],
    "outcome": "pass",
    "reason": None,
    "binary_sha256": request["binary"]["sha256"],
    "driver_sha256": driver_sha,
    "samples": samples,
}
args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
''',
        encoding="utf-8",
    )
    path.chmod(0o755)


def make_collection_source(path: Path) -> tuple[Path, Path, Path, str]:
    (path.parent / "ambient-build-output.bin").write_bytes(
        b"ambient build output must not be a state-build input"
    )
    driver = path / "tools/testing/a3/trace-producer-overhead-driver.py"
    driver.parent.mkdir(parents=True)
    write_collection_driver(driver)
    state_build_driver = (
        path / "tools/testing/a3/trace-producer-overhead-state-build.py"
    )
    write_state_build_driver(state_build_driver, path)
    product = path / "tools/testing/a3/collection-host-product"
    write_collection_host(product)
    subprocess.run(["git", "init", "-q"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.email", "a3@example.invalid"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.name", "A3 Test"], cwd=path, check=True)
    subprocess.run(["git", "add", "."], cwd=path, check=True)
    subprocess.run(["git", "commit", "-qm", "fixture"], cwd=path, check=True)
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=path, text=True,
    ).strip()
    return driver, state_build_driver, product, revision


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8",
    )


def make_fixture(
    root: Path, *, candidate_revision: str = "c" * 40,
    driver_path: str = "tools/testing/a3/trace-producer-overhead-driver.py",
    driver_sha256: str = "f" * 64,
    state_build_driver_path: str = (
        "tools/testing/a3/trace-producer-overhead-state-build.py"
    ),
    state_build_driver_sha256: str = "a" * 64,
    campaign_role: str = "standalone", campaign_id: str = "campaign-1",
    build_family_id: str = "pulp-build-1",
    product_id: str = "dev.pulp.product-1",
    product_name: str = "GPU Product 1", plugin_format: str = "standalone",
    machine_id: str = "m5-blackbook",
    candidate_binary_sha256: str = "3" * 64,
) -> tuple[dict[str, Path], dict[str, dict[str, Any]]]:
    baseline_revision = overhead.BASELINE_REVISION
    machine = {
        "machine_id": machine_id,
        "operating_system": "macOS",
        "architecture": "arm64",
    }
    workload = {
        "workload_id": "forge-first-visible-reference",
        "content_sha256": "d" * 64,
        "adapter_sha256": "e" * 64,
        "adapter_revision": candidate_revision,
        "adapter_path": driver_path,
    }
    workload["adapter_sha256"] = driver_sha256
    driver = {
        "revision": candidate_revision,
        "path": driver_path,
        "sha256": driver_sha256,
    }
    state_build_driver = {
        "revision": candidate_revision,
        "path": state_build_driver_path,
        "sha256": state_build_driver_sha256,
    }
    challenge_nonce = "0" * 32
    duration = {
        "pre-change-baseline": 10.0,
        "candidate-compile-out": 10.05,
        "candidate-compiled-in-idle": 10.1,
        "candidate-active": 10.4,
    }
    session_config_path = root / "trace-session-config.json"
    write_json(session_config_path, {
        "schema": overhead.SESSION_CONFIG_SCHEMA,
        "version": 1,
        "ring_bytes": overhead.RING_BYTES,
        "fill_policy": "ring-buffer",
        "categories": overhead.ACTIVE_TRACE_CATEGORIES,
    })
    session_config_ref = overhead.artifact_ref(session_config_path, root)
    counter = 1
    documents: dict[str, dict[str, Any]] = {}
    paths: dict[str, Path] = {}
    for state in overhead.STATES:
        active = state == "candidate-active"
        binary_sha256 = (
            candidate_binary_sha256 if state in {
                "candidate-compiled-in-idle", "candidate-active",
            } else f"{overhead.STATES.index(state) + 1:x}" * 64
        )

        def samples(section: str, count: int) -> list[dict[str, Any]]:
            nonlocal counter
            rows = []
            for sequence in range(count):
                evidence_id = f"{counter:032x}"
                host_pid = 10_000 + counter
                process_start = f"fixture-process-start-{counter}"
                audio_tids = [90_001, 90_002]
                started_ns = counter * 1_000_000_000
                finished_ns = started_ns + round(duration[state] * 1_000_000)
                metrics_path = (
                    root / "metrics" / f"{state}-{section}-{sequence}.json"
                )
                write_json(metrics_path, {
                    "schema": overhead.METRICS_SCHEMA,
                    "version": 1,
                    "state": state,
                    "sequence": sequence,
                    "evidence_id": evidence_id,
                    "host_pid": host_pid,
                    "process_start_identity": process_start,
                    "executable_sha256": binary_sha256,
                    "audio_thread_tids": audio_tids,
                    "started_monotonic_ns": started_ns,
                    "finished_monotonic_ns": finished_ns,
                    "xrun_count": 0,
                    "collection_challenge_nonce": challenge_nonce,
                    "driver_sha256": driver_sha256,
                })
                trace_ref = None
                if active:
                    trace = root / "traces" / f"{state}-{section}-{sequence}.pftrace"
                    write_json(trace, {"traceEvents": [
                        {
                            "name": "pulp_a3_trace_session",
                            "cat": "metadata",
                            "ph": "i",
                            "pid": host_pid,
                            "tid": host_pid,
                            "args": {
                                "gpu_evidence_id": evidence_id,
                                "process_start_identity": process_start,
                                "executable_sha256": binary_sha256,
                                "session_config_sha256": session_config_ref["sha256"],
                                "collection_challenge_nonce": challenge_nonce,
                                "audio_thread_tids_sha256": (
                                    overhead.trace_replay.tids_digest(audio_tids)
                                ),
                                "ring_bytes": overhead.RING_BYTES,
                                "session_active": True,
                            },
                        },
                        {
                            "name": "gpu_health_transition_first_visible",
                            "cat": "gpu",
                            "ph": "X",
                            "pid": host_pid,
                            "tid": host_pid,
                            "ts": started_ns / 1000,
                            "dur": max(1, (finished_ns - started_ns) / 1000),
                            "args": {"gpu_evidence_id": evidence_id},
                        },
                        *[
                            {
                                "name": name,
                                "cat": "render",
                                "ph": "X",
                                "pid": host_pid,
                                "tid": host_pid,
                                "ts": started_ns / 1000,
                                "dur": 1,
                                "args": {"gpu_evidence_id": evidence_id},
                            }
                            for name in ("gpu_acquire", "gpu_submit", "gpu_present")
                        ],
                        *[
                            {
                                "name": name,
                                "cat": category,
                                "ph": "X",
                                "pid": host_pid,
                                "tid": host_pid,
                                "ts": started_ns / 1000,
                                "dur": 1,
                                "args": {},
                            }
                            for category, name in (
                                ("render", "skia_begin"),
                                ("render", "view_repaint_request"),
                                ("render", "repaint_request"),
                                ("state", "editor_bridge_dispatch_json"),
                                ("state", "editor_bridge_json_parse"),
                                ("js", "frame_callback_pump"),
                                ("js", "raf_flush"),
                            )
                        ],
                        {
                            "name": "pointer_coalescer_flushes",
                            "cat": "state",
                            "ph": "C",
                            "pid": host_pid,
                            "tid": host_pid,
                            "ts": started_ns / 1000,
                            "args": {"value": 1},
                        },
                    ]})
                    trace_ref = {
                        **overhead.artifact_ref(trace, root),
                        "bytes": trace.stat().st_size,
                        "format": "chrome-json",
                    }
                rows.append({
                    "sequence": sequence,
                    "evidence_id": evidence_id,
                    "duration_ms": duration[state],
                    "runtime_metrics": overhead.artifact_ref(metrics_path, root),
                    "trace": trace_ref,
                })
                counter += 1
            return rows

        compiled, session_active, ring_bytes = overhead.STATE_TRACING[state]
        document = {
            "schema": overhead.RAW_SCHEMA,
            "version": 1,
            "state": state,
            "producer_revision": overhead.PRODUCER_REVISION,
            "producer_packages": copy.deepcopy(overhead.PRODUCER_PACKAGES),
            "source_revision": (
                baseline_revision if state == "pre-change-baseline"
                else candidate_revision
            ),
            "baseline_revision": baseline_revision,
            "candidate_revision": candidate_revision,
            "campaign_role": campaign_role,
            "campaign_id": campaign_id,
            "build_family_id": build_family_id,
            "product_id": product_id,
            "product_name": product_name,
            "plugin_format": plugin_format,
            "binary_sha256": binary_sha256,
            "machine": copy.deepcopy(machine),
            "workload": copy.deepcopy(workload),
            "measurement_driver": copy.deepcopy(driver),
            "state_build_driver": copy.deepcopy(state_build_driver),
            "trace_session_config": copy.deepcopy(session_config_ref),
            "tracing": {
                "compiled_in": compiled,
                "session_active": session_active,
                "ring_bytes": ring_bytes,
            },
            "collection": None,
            "warmups": samples("warmup", 5),
            "measured": samples("measured", 30),
            "fresh_process": samples("fresh", 20),
        }
        path = root / f"{state}.json"
        write_json(path, document)
        documents[state] = document
        paths[state] = path
    return paths, documents


def rewrite_artifact(
    root: Path, ref: dict[str, Any], mutation: Callable[[dict[str, Any]], None],
) -> None:
    path = root / ref["path"]
    payload = json.loads(path.read_text(encoding="utf-8"))
    mutation(payload)
    write_json(path, payload)
    ref["sha256"] = overhead.sha256_file(path, "mutated fixture artifact")
    if "bytes" in ref:
        ref["bytes"] = path.stat().st_size


def duplicate_process(root: Path, documents: dict[str, dict[str, Any]]) -> None:
    rows = documents["candidate-active"]["fresh_process"]
    first = json.loads((root / rows[0]["runtime_metrics"]["path"]).read_text())

    def mutate(second: dict[str, Any]) -> None:
        second["host_pid"] = first["host_pid"]
        second["process_start_identity"] = first["process_start_identity"]

    rewrite_artifact(root, rows[1]["runtime_metrics"], mutate)


def omit_active_category(
    root: Path, documents: dict[str, dict[str, Any]], category: str,
) -> None:
    reference = documents["candidate-active"]["trace_session_config"]
    path = root / reference["path"]
    payload = json.loads(path.read_text())
    payload["categories"].remove(category)
    write_json(path, payload)
    digest = overhead.sha256_file(path, "mutated trace session config")
    for document in documents.values():
        document["trace_session_config"]["sha256"] = digest


def expect_failure(
    root: Path, mutation: Callable[[dict[str, dict[str, Any]], dict[str, Path]], None],
    label: str,
) -> None:
    paths, documents = make_fixture(root)
    mutation(documents, paths)
    for state in overhead.STATES:
        write_json(paths[state], documents[state])
    try:
        receipt = overhead.build_receipt(
            paths, evidence_root=root, generated_utc="2026-08-29T12:00:00Z",
            allow_fixture_collection=True, allow_fixture_chrome_json=True,
        )
    except overhead.OverheadError:
        return
    if receipt["verdict"] == "fail":
        return
    raise AssertionError(f"planted producer-overhead negative did not fire: {label}")


def main() -> int:
    assert_transitive_source_graph_ignores_bytecode()
    with tempfile.TemporaryDirectory(prefix="pulp-a3-trace-overhead-") as temporary:
        temp_root = Path(temporary)
        paths, _ = make_fixture(temp_root / "positive")
        receipt = overhead.build_receipt(
            paths, evidence_root=temp_root / "positive",
            generated_utc="2026-08-29T12:00:00Z",
            allow_fixture_collection=True, allow_fixture_chrome_json=True,
        )
        assert receipt["verdict"] == "pass"
        receipt_path = temp_root / "positive" / "receipt.json"
        write_json(receipt_path, receipt)
        overhead.validate_receipt(
            receipt, temp_root / "positive",
            allow_fixture_collection=True, allow_fixture_chrome_json=True,
        )
        replay = receipt["replay_summary"]
        assert replay["b4ba_active_categories"] == ["js", "render", "state"]
        assert len(receipt["producer_packages"]["mac-input-to-present"]["signatures"]) == 20
        assert all(
            replay["b4ba_signature_events"][signature] == 55
            for signature in overhead.trace_replay.B4BA_REQUIRED_SIGNATURE_IDS
        )
        assert replay["b4ba_signature_events"][
            "counter:state:pointer_coalescer_flushes"
        ] == 55
        assert "counter:state:raw_drag_samples" in replay["b4ba_unobserved_signatures"]
        for kwargs, label in (
            ({}, "terminal receipt without a live collection"),
            ({"allow_fixture_collection": True}, "terminal Chrome JSON fixture"),
        ):
            try:
                overhead.build_receipt(
                    paths, evidence_root=temp_root / "positive",
                    generated_utc="2026-08-29T12:00:00Z", **kwargs,
                )
            except overhead.OverheadError:
                pass
            else:
                raise AssertionError(f"planted negative passed: {label}")

        active_row = json.loads(
            (temp_root / "positive" / receipt["raw_artifacts"]["candidate-active"]["path"])
            .read_text(encoding="utf-8")
        )["measured"][0]
        metrics = json.loads(
            (temp_root / "positive" / active_row["runtime_metrics"]["path"])
            .read_text(encoding="utf-8")
        )
        replay_request = {
            "evidence_id": metrics["evidence_id"],
            "host_pid": metrics["host_pid"],
            "process_start_identity": metrics["process_start_identity"],
            "executable_sha256": metrics["executable_sha256"],
            "audio_thread_tids": metrics["audio_thread_tids"],
            "session_config_sha256": receipt["trace_session_config"]["sha256"],
            "ring_bytes": overhead.RING_BYTES,
            "collection_challenge_nonce": metrics["collection_challenge_nonce"],
        }
        fake_marker = temp_root / "fake-processor-ran"
        fake_processor = temp_root / "fake-trace-processor"
        fake_processor.write_text(
            "#!/bin/sh\ntouch " + str(fake_marker) + "\n", encoding="utf-8",
        )
        fake_processor.chmod(0o755)
        for name, payload in (
            ("chrome-array.json", b"[]\n"),
            ("chrome-bom.json", b"\xef\xbb\xbf{\"traceEvents\":[]}\n"),
        ):
            json_trace = temp_root / name
            json_trace.write_bytes(payload)
            try:
                overhead.trace_replay.analyze_trace(
                    json_trace, replay_request, fake_processor,
                )
            except overhead.trace_replay.TraceReplayError:
                pass
            else:
                raise AssertionError(f"production replay accepted {name}")
            assert not fake_marker.exists(), f"trace processor executed for {name}"
        binary_trace = temp_root / "binary.pftrace"
        binary_trace.write_bytes(b"\x0a\x03bad")
        try:
            overhead.trace_replay.analyze_trace(
                binary_trace, replay_request, fake_processor,
            )
        except overhead.trace_replay.TraceReplayError:
            pass
        else:
            raise AssertionError("unpinned trace processor unexpectedly passed")
        assert not fake_marker.exists(), "unpinned trace processor was executed"

        valid_result = {
            "trace_format": "perfetto-proto", "request": replay_request,
            "producer_events": 1, "foreign_producer_events": 0,
            "producer_pids": [metrics["host_pid"]], "producer_tids": [metrics["host_pid"]],
            "producer_upids": [7], "session_events": 1, "session_upids": [7],
            "xrun_events": 0, "incomplete_slices": 0,
            "data_loss_count": 0, "no_flush_count": 0,
            "input_to_present_events": {
                "gpu_acquire": 1, "gpu_submit": 1, "gpu_present": 1,
            },
            "input_to_present_pids": [metrics["host_pid"]],
            "input_to_present_tids": [metrics["host_pid"]],
            "input_to_present_upids": [7],
            "b4ba_signature_events": {
                signature: (
                    1 if signature in overhead.trace_replay.B4BA_REQUIRED_SIGNATURE_IDS
                    else 0
                )
                for signature in overhead.trace_replay.B4BA_SIGNATURE_IDS
            },
            "b4ba_pids": [metrics["host_pid"]],
            "b4ba_tids": [metrics["host_pid"]],
            "b4ba_upids": [7],
        }
        for field, value in (("incomplete_slices", 1), ("session_upids", [8])):
            mutated = dict(valid_result)
            mutated[field] = value
            try:
                overhead.trace_replay.result(**mutated)
            except overhead.trace_replay.TraceReplayError:
                pass
            else:
                raise AssertionError(f"trace replay accepted planted {field}")

        def mutation_table(case_root: Path) -> list[
            Callable[[dict[str, dict[str, Any]], dict[str, Path]], None]
        ]:
            return [
                lambda docs, _: docs["candidate-active"]["measured"][0].__setitem__(
                    "duration_ms", 20.0,
                ),
                lambda docs, _: rewrite_artifact(
                    case_root,
                    docs["candidate-active"]["fresh_process"][0]["runtime_metrics"],
                    lambda value: value.__setitem__("xrun_count", 1),
                ),
                lambda docs, _: rewrite_artifact(
                    case_root, docs["candidate-active"]["measured"][0]["trace"],
                    lambda value: value["traceEvents"][1].__setitem__("tid", 90_001),
                ),
                lambda docs, _: docs["candidate-active"]["measured"][0]["trace"].__setitem__(
                    "sha256", "0" * 64,
                ),
                lambda docs, _: docs["candidate-compile-out"]["workload"].__setitem__(
                    "content_sha256", "0" * 64,
                ),
                lambda docs, _: docs["candidate-active"]["measured"][1].__setitem__(
                    "evidence_id", docs["candidate-active"]["measured"][0]["evidence_id"],
                ),
                lambda docs, _: docs["candidate-compile-out"].__setitem__(
                    "source_revision", docs["candidate-compile-out"]["baseline_revision"],
                ),
                lambda docs, _: docs["pre-change-baseline"].__setitem__(
                    "baseline_revision", "b" * 40,
                ),
                lambda docs, _: docs["candidate-active"].__setitem__(
                    "binary_sha256", "4" * 64,
                ),
                lambda docs, _: duplicate_process(case_root, docs),
                lambda docs, _: rewrite_artifact(
                    case_root, docs["candidate-active"]["measured"][1]["trace"],
                    lambda value: value["traceEvents"][1]["args"].__setitem__(
                        "gpu_evidence_id", "f" * 32,
                    ),
                ),
                lambda docs, _: rewrite_artifact(
                    case_root,
                    docs["candidate-active"]["measured"][2]["runtime_metrics"],
                    lambda value: value.__setitem__("executable_sha256", "a" * 64),
                ),
                lambda docs, _: rewrite_artifact(
                    case_root, docs["candidate-active"]["measured"][3]["trace"],
                    lambda value: value.__setitem__(
                        "traceEvents", [
                            event for event in value["traceEvents"]
                            if event.get("name") != "gpu_present"
                        ],
                    ),
                ),
                lambda docs, _: docs["candidate-active"]["producer_packages"][
                    "mac-input-to-present"
                ].__setitem__("revision", "0" * 40),
                lambda docs, _: docs["candidate-active"]["producer_packages"][
                    "mac-input-to-present"
                ]["signatures"].pop(),
                lambda docs, _: omit_active_category(case_root, docs, "state"),
            ]

        for index in range(16):
            case_root = temp_root / f"negative-{index}"
            case_root.mkdir()
            expect_failure(case_root, mutation_table(case_root)[index], str(index))

        mutated_receipt = copy.deepcopy(receipt)
        mutated_receipt["comparisons"]["candidate-active"]["pass"] = False
        try:
            overhead.validate_receipt(
                mutated_receipt, temp_root / "positive",
                allow_fixture_collection=True, allow_fixture_chrome_json=True,
            )
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("derived receipt mutation did not fire")

        collection_evidence = temp_root / "live-collection"
        collection_evidence.mkdir()
        source_root = temp_root / "collection-source"
        source_root.mkdir()
        (
            collection_driver, state_build_driver, collection_host,
            candidate_revision,
        ) = make_collection_source(source_root)
        session_config = collection_evidence / "trace-session-config.json"
        write_json(session_config, {
            "schema": overhead.SESSION_CONFIG_SCHEMA,
            "version": 1,
            "ring_bytes": overhead.RING_BYTES,
            "fill_policy": "ring-buffer",
            "categories": overhead.ACTIVE_TRACE_CATEGORIES,
        })
        driver_relative = "tools/testing/a3/trace-producer-overhead-driver.py"
        collection_request = {
            "schema": overhead.COLLECTION_REQUEST_SCHEMA,
            "version": 1,
            "state": "candidate-compiled-in-idle",
            "producer_revision": overhead.PRODUCER_REVISION,
            "producer_packages": copy.deepcopy(overhead.PRODUCER_PACKAGES),
            "source_revision": candidate_revision,
            "baseline_revision": overhead.BASELINE_REVISION,
            "candidate_revision": candidate_revision,
            "campaign_role": "standalone",
            "campaign_id": "live-collection-campaign",
            "build_family_id": "live-collection-build",
            "product_id": "dev.pulp.live-collection",
            "product_name": "Live Collection Product",
            "plugin_format": "standalone",
            "binary_sha256": overhead.sha256_file(collection_host, "collection host"),
            "machine": {
                "machine_id": "fixture-mac",
                "operating_system": "macOS",
                "architecture": "arm64",
            },
            "workload": {
                "workload_id": "live-collection-workload",
                "content_sha256": "d" * 64,
                "adapter_sha256": overhead.sha256_file(
                    collection_driver, "collection driver",
                ),
                "adapter_revision": candidate_revision,
                "adapter_path": driver_relative,
            },
            "measurement_driver": {
                "revision": candidate_revision,
                "path": driver_relative,
                "sha256": overhead.sha256_file(collection_driver, "collection driver"),
            },
            "state_build_driver": {
                "revision": candidate_revision,
                "path": "tools/testing/a3/trace-producer-overhead-state-build.py",
                "sha256": overhead.sha256_file(
                    state_build_driver, "state build driver",
                ),
            },
            "trace_session_config": overhead.artifact_ref(
                session_config, collection_evidence,
            ),
            "tracing": {
                "compiled_in": True,
                "session_active": False,
                "ring_bytes": 0,
            },
        }
        collection_request_path = collection_evidence / "request.json"
        write_json(collection_request_path, collection_request)
        foreign_driver = temp_root / "foreign-driver.py"
        foreign_driver.write_bytes(collection_driver.read_bytes())
        foreign_driver.chmod(0o755)
        try:
            overhead.collect_state(
                request_path=collection_request_path,
                evidence_root=collection_evidence, source_root=source_root,
                binary=collection_host, driver=foreign_driver,
                build_driver=state_build_driver,
                output=collection_evidence / "foreign.json", trace_processor=None,
            )
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("non-source-bound collection driver passed")
        foreign_build_driver = temp_root / "foreign-state-build-driver.py"
        foreign_build_driver.write_bytes(state_build_driver.read_bytes())
        foreign_build_driver.chmod(0o755)
        try:
            overhead.collect_state(
                request_path=collection_request_path,
                evidence_root=collection_evidence, source_root=source_root,
                binary=collection_host, driver=collection_driver,
                build_driver=foreign_build_driver,
                output=collection_evidence / "foreign-build.json",
                trace_processor=None,
            )
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("non-source-bound state build driver passed")
        for mutation in (
            "state-build-read-measured",
            "state-build-read-ambient",
            "state-build-network",
        ):
            mutation_root = temp_root / mutation
            mutation_root.mkdir()
            mutation_config = mutation_root / "trace-session-config.json"
            write_json(mutation_config, json.loads(session_config.read_text()))
            mutation_request = copy.deepcopy(collection_request)
            mutation_request["build_family_id"] = mutation
            mutation_request["trace_session_config"] = overhead.artifact_ref(
                mutation_config, mutation_root,
            )
            mutation_request_path = mutation_root / "request.json"
            write_json(mutation_request_path, mutation_request)
            try:
                overhead.collect_state(
                    request_path=mutation_request_path, evidence_root=mutation_root,
                    source_root=source_root, binary=collection_host,
                    driver=collection_driver, build_driver=state_build_driver,
                    output=mutation_root / "raw.json", trace_processor=None,
                )
            except overhead.OverheadError as error:
                assert "omitted its receipt" in str(error), error
            else:
                raise AssertionError(f"state build sandbox accepted {mutation}")
        mismatch_root = temp_root / "state-build-product-mismatch"
        mismatch_root.mkdir()
        mismatch_config = mismatch_root / "trace-session-config.json"
        write_json(mismatch_config, json.loads(session_config.read_text()))
        mismatch_product = temp_root / "mismatched-product"
        mismatch_product.write_bytes(collection_host.read_bytes() + b"mismatch")
        mismatch_product.chmod(0o755)
        mismatch_request = copy.deepcopy(collection_request)
        mismatch_request["binary_sha256"] = overhead.sha256_file(
            mismatch_product, "mismatched product",
        )
        mismatch_request["trace_session_config"] = overhead.artifact_ref(
            mismatch_config, mismatch_root,
        )
        mismatch_request_path = mismatch_root / "request.json"
        write_json(mismatch_request_path, mismatch_request)
        try:
            overhead.collect_state(
                request_path=mismatch_request_path, evidence_root=mismatch_root,
                source_root=source_root, binary=mismatch_product,
                driver=collection_driver, build_driver=state_build_driver,
                output=mismatch_root / "raw.json", trace_processor=None,
            )
        except overhead.OverheadError as error:
            assert "differs from the measured executable" in str(error), error
        else:
            raise AssertionError("non-source-built measured executable passed")
        no_sentinel = temp_root / "no-tracing-sentinel"
        no_sentinel.write_bytes(b"regular executable bytes")
        try:
            overhead.validate_binary_compile_config(
                no_sentinel, True, "planted compiled-in product",
            )
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("compiled-in product without its sentinel passed")
        collection_output = collection_evidence / "candidate-compiled-in-idle.json"
        arbitrary_root = temp_root / "arbitrary-live-id"
        arbitrary_root.mkdir()
        arbitrary_config = arbitrary_root / "trace-session-config.json"
        write_json(arbitrary_config, json.loads(session_config.read_text()))
        arbitrary_request = copy.deepcopy(collection_request)
        arbitrary_request["trace_session_config"] = overhead.artifact_ref(
            arbitrary_config, arbitrary_root,
        )
        arbitrary_request_path = arbitrary_root / "request.json"
        write_json(arbitrary_request_path, arbitrary_request)
        os.environ["PULP_A3_TEST_ARBITRARY_OVERHEAD_ID"] = "1"
        try:
            overhead.collect_state(
                request_path=arbitrary_request_path, evidence_root=arbitrary_root,
                source_root=source_root, binary=collection_host,
                driver=collection_driver, build_driver=state_build_driver,
                output=arbitrary_root / "raw.json",
                trace_processor=None,
            )
        except overhead.OverheadError as error:
            assert "challenged process lifetime" in str(error), error
        else:
            raise AssertionError("arbitrary live evidence ID unexpectedly passed")
        finally:
            os.environ.pop("PULP_A3_TEST_ARBITRARY_OVERHEAD_ID", None)
        live_raw = overhead.collect_state(
            request_path=collection_request_path, evidence_root=collection_evidence,
            source_root=source_root, binary=collection_host, driver=collection_driver,
            build_driver=state_build_driver,
            output=collection_output, trace_processor=None,
        )
        overhead.validate_raw(
            live_raw, state="candidate-compiled-in-idle",
            evidence_root=collection_evidence, trace_processor=None,
            label="live-collection", allow_fixture_collection=False,
            allow_fixture_chrome_json=False,
        )
        collection_receipt_path = (
            collection_evidence / live_raw["collection"]["path"]
        )
        original_collection_receipt = collection_receipt_path.read_bytes()
        original_collection_digest = live_raw["collection"]["sha256"]

        def expect_state_build_artifact_tamper(
            key: str, mutate: Callable[[Path], None], label: str,
        ) -> None:
            collection = json.loads(original_collection_receipt)
            ref = collection["state_build"][key]
            artifact = collection_evidence / ref["path"]
            original_artifact = artifact.read_bytes()
            original_mode = artifact.stat().st_mode & 0o777
            artifact.chmod(0o700)
            mutate(artifact)
            ref["sha256"] = overhead.sha256_file(artifact, label)
            write_json(collection_receipt_path, collection)
            live_raw["collection"]["sha256"] = overhead.sha256_file(
                collection_receipt_path, "mutated collection receipt",
            )
            try:
                overhead.validate_raw(
                    live_raw, state="candidate-compiled-in-idle",
                    evidence_root=collection_evidence, trace_processor=None,
                    label="live-collection", allow_fixture_collection=False,
                    allow_fixture_chrome_json=False,
                )
            except overhead.OverheadError:
                pass
            else:
                raise AssertionError(f"state build accepted planted {label}")
            artifact.write_bytes(original_artifact)
            artifact.chmod(original_mode)
            collection_receipt_path.write_bytes(original_collection_receipt)
            live_raw["collection"]["sha256"] = original_collection_digest

        def mutate_build_receipt(path: Path) -> None:
            payload = json.loads(path.read_text())
            payload["tracing"]["compiled_in"] = False
            write_json(path, payload)

        expect_state_build_artifact_tamper(
            "build_receipt", mutate_build_receipt, "state build config receipt",
        )
        expect_state_build_artifact_tamper(
            "source_archive", lambda path: path.write_bytes(path.read_bytes() + b"x"),
            "state source archive",
        )
        expect_state_build_artifact_tamper(
            "rebuilt_product", lambda path: path.write_bytes(path.read_bytes() + b"x"),
            "state rebuilt product",
        )
        collection_receipt = json.loads(original_collection_receipt)
        collection_receipt["state_build"]["toolchain"].pop()
        write_json(collection_receipt_path, collection_receipt)
        live_raw["collection"]["sha256"] = overhead.sha256_file(
            collection_receipt_path, "mutated collection receipt",
        )
        try:
            overhead.validate_raw(
                live_raw, state="candidate-compiled-in-idle",
                evidence_root=collection_evidence, trace_processor=None,
                label="live-collection", allow_fixture_collection=False,
                allow_fixture_chrome_json=False,
            )
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("state build accepted incomplete toolchain evidence")
        collection_receipt_path.write_bytes(original_collection_receipt)
        live_raw["collection"]["sha256"] = original_collection_digest

        collection_receipt = json.loads(original_collection_receipt)
        collection_receipt["handshake_artifacts"].pop()
        write_json(collection_receipt_path, collection_receipt)
        live_raw["collection"]["sha256"] = overhead.sha256_file(
            collection_receipt_path, "mutated collection receipt",
        )
        try:
            overhead.validate_raw(
                live_raw, state="candidate-compiled-in-idle",
                evidence_root=collection_evidence, trace_processor=None,
                label="live-collection", allow_fixture_collection=False,
                allow_fixture_chrome_json=False,
            )
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("incomplete live handshake unexpectedly passed")
        collection_receipt_path.write_bytes(original_collection_receipt)
        live_raw["collection"]["sha256"] = original_collection_digest

        print(
            "gpu-first-visible-a3-trace-producer-overhead: "
            "positive=2 planted_negatives=37"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
