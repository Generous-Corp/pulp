#!/usr/bin/env python3
"""Build a bounded, evidence-only layered graph inspection/replay receipt.

This tool deliberately composes existing public consumers.  It does not add a
runtime graph API, an event queue, or an audio scheduler.  The receipt keeps
the five identity layers independent so a planted mutation is attributed to
the layer it changes instead of being hidden behind one graph digest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "pulp.graph.layered-replay-receipt.v1"
PARTITIONS = (64, 128)
CONSUMER_TIMEOUT_SECONDS = 10
CONSUMER_PROTOCOLS = {
    "g3": ("pulp-g3-resolved-identity-consumer", "pulp.graph.g3-resolved-identity-consumer.v1"),
    "spectral": ("spectral-frame-codec-consumer", "pulp.signal.spectral-frame-codec-consumer.v1"),
}
SOURCE_HEAD_RE = re.compile(r"^[0-9a-f]{40}$")


def canonical(value: Any) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError("invalid UTF-8 in receipt value") from exc


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical(value)).hexdigest()


@dataclass(frozen=True)
class Layer:
    name: str
    value: Any

    @property
    def hash(self) -> str:
        return digest(self.value)


def _run_consumer_once(label: str, command: Sequence[str], identity: str | None,
                       protocol: str | None, consumer_kind: str,
                       partition: int | None = None) -> dict[str, Any]:
    """Run a named consumer only when its executable and protocol are explicit."""
    expected_name, expected_protocol = CONSUMER_PROTOCOLS[consumer_kind]
    record: dict[str, Any] = {"name": label, "status": "UNAVAILABLE",
                              "identity": identity, "protocol": protocol,
                              "timeout_seconds": CONSUMER_TIMEOUT_SECONDS}
    executable = Path(command[0])
    if identity != expected_name or protocol != expected_protocol:
        record["reason"] = "explicit executable identity/protocol did not match the named consumer"
        return record
    if executable.name != expected_name or not executable.is_file() or executable.is_symlink():
        record["reason"] = "executable path is not the named public consumer"
        return record
    try:
        magic = executable.read_bytes()[:4]
    except OSError as exc:
        record["reason"] = f"cannot inspect executable provenance: {exc}"
        return record
    if magic[:2] == b"#!" or magic not in (b"\x7fELF", b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                                             b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
        record["reason"] = "consumer executable is not a native installed-SDK binary"
        return record
    invoked = list(command)
    if partition is not None:
        invoked.extend(("--partition", str(partition)))
    try:
        result = subprocess.run(invoked, capture_output=True, stdin=subprocess.DEVNULL,
                                timeout=CONSUMER_TIMEOUT_SECONDS, check=False)
    except subprocess.TimeoutExpired as exc:
        record["reason"] = "consumer timed out"
        record["output_sha256"] = "sha256:" + hashlib.sha256(
            (exc.output or b"") + (exc.stderr or b"")).hexdigest()
        return record
    except OSError as exc:
        record["reason"] = str(exc)
        return record
    raw_output = result.stdout + result.stderr
    record.update({"returncode": result.returncode, "command": invoked,
                   "output_sha256": "sha256:" + hashlib.sha256(raw_output).hexdigest(),
                   "executable_sha256": "sha256:" + hashlib.sha256(executable.read_bytes()).hexdigest()})
    try:
        output = raw_output.decode("utf-8")
    except UnicodeDecodeError:
        record["reason"] = "consumer output is not valid UTF-8"
        return record
    tokens = output.split()
    if expected_protocol not in tokens:
        record["reason"] = "consumer output did not attest the required protocol as an exact token"
        return record
    if partition is not None and f"partition_frames={partition}" not in tokens:
        record["reason"] = "consumer output did not attest the requested block partition"
        return record
    record["status"] = "PASS" if result.returncode == 0 else "FAIL"
    if result.returncode != 0:
        record["output_tail"] = output[-400:]
    return record


def run_consumer(label: str, command: Sequence[str] | None, identity: str | None,
                 protocol: str | None, consumer_kind: str,
                 partitions: Sequence[int] | None = None) -> dict[str, Any]:
    """Run a named consumer, optionally once for each required block partition."""
    if not command:
        return {"name": label, "status": "UNAVAILABLE", "identity": identity,
                "protocol": protocol, "timeout_seconds": CONSUMER_TIMEOUT_SECONDS,
                "reason": "command not supplied"}
    if partitions is None:
        return _run_consumer_once(label, command, identity, protocol, consumer_kind)
    runs = [_run_consumer_once(label, command, identity, protocol, consumer_kind, partition)
            for partition in partitions]
    failed = next((run for run in runs if run["status"] == "FAIL"), None)
    unavailable = next((run for run in runs if run["status"] == "UNAVAILABLE"), None)
    status = "FAIL" if failed else ("UNAVAILABLE" if unavailable else "PASS")
    record: dict[str, Any] = {"name": label, "status": status, "identity": identity,
                              "protocol": protocol, "partitions": list(partitions),
                              "runs": runs}
    if failed:
        record["reason"] = failed.get("reason", "consumer failed")
    elif unavailable:
        record["reason"] = unavailable.get("reason", "consumer unavailable")
    else:
        record["executable_sha256"] = runs[0].get("executable_sha256")
    return record


def output_for(partition: int, event: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
    """Produce canonical PCM bytes from sample positions.

    ``partition`` is recorded for the bounded fixture; this sample-level
    oracle intentionally does not claim to exercise block boundaries.
    """
    frames = 256
    samples: list[float] = []
    gain = float(plan["gain"])
    for frame in range(frames):
        event_gain = float(event["gain_by_sample"].get(str(frame), 1.0))
        value = gain * event_gain * ((frame % 17) - 8) / 8.0
        samples.extend((value, -value))
    # Quantize explicitly to avoid a provider/partition-dependent text float.
    pcm = b"".join(int(round(max(-1.0, min(1.0, sample)) * 2147483647)).to_bytes(4, "little", signed=True)
                   for sample in samples)
    peak = max((abs(sample) for sample in samples), default=0.0)
    rms = (sum(sample * sample for sample in samples) / len(samples)) ** 0.5 if samples else 0.0
    return {
        "canonical_pcm": {"encoding": "s32le", "endianness": "little",
                          "channels": ["L", "R"], "channel_count": 2,
                          "sample_rate": 48000, "bytes_per_sample": 4},
        "partition_frames": partition,
        "frame_count": frames,
        "peak_tolerance": 1e-7,
        "rms_tolerance": 1e-7,
        "quality": {"peak": round(peak, 9), "rms": round(rms, 9),
                     "peak_tolerance": 1e-7, "rms_tolerance": 1e-7,
                     "comparison": "exact_pcm_digest_with_metric_tolerances"},
        "pcm_sha256": "sha256:" + hashlib.sha256(pcm).hexdigest(),
    }


def base_layers() -> list[Layer]:
    authored = {
        "profile": "pulp.graph.authored-io-gain.v1",
        "nodes": [
            {"id": "input", "kind": "audio_input", "channels": ["L", "R"]},
            {"id": "gain", "kind": "gain", "gain_bits": "3f800000"},
            {"id": "output", "kind": "audio_output", "channels": ["L", "R"]},
        ],
        "edges": [["input", "gain"], ["gain", "output"]],
    }
    resolved = {
        "profile": "pulp.graph.resolved-plan.v1",
        "nodes": [
            {"logical_id": "input", "implementation": "pulp.builtin.audio-input.v1", "resource": None},
            {"logical_id": "gain", "implementation": "pulp.builtin.gain.v1", "resource": None},
            {"logical_id": "output", "implementation": "pulp.builtin.audio-output.v1", "resource": None},
        ],
        "resources": [],
    }
    plan = {"generation": 7, "block_frames": 128, "sample_rate": 48000, "gain": 1.0,
            "latency": 0, "latency_samples": 0, "fallback": "none",
            "capacity": {"nodes": 3, "events": 16, "inputs": 2, "outputs": 2}}
    event = {"stream": "audio-control.v1", "clock_epoch": 3, "absolute_origin": 0,
             "capacity": {"events": 16, "bytes": 1024},
             "controls": [{"sample": 64, "gain": 0.5}, {"sample": 192, "gain": 0.75}],
             "gain_by_sample": {"64": 0.5, "192": 0.75},
             "ordering": "(absolute_sample,producer,sequence)"}
    output = {"partitions": [output_for(partition, event, plan) for partition in PARTITIONS],
              "partition_invariant": True}
    return [Layer("authored", authored), Layer("resolved", resolved), Layer("plan", plan),
            Layer("event", event), Layer("output", output)]


def receipt(layers: list[Layer], consumers: list[dict[str, Any]], source_head: str) -> dict[str, Any]:
    validated_head = None
    if SOURCE_HEAD_RE.fullmatch(source_head or ""):
        repo_root = Path(__file__).resolve().parents[2]
        checked = subprocess.run(["git", "cat-file", "-e", f"{source_head}^{{commit}}"],
                                 cwd=repo_root, capture_output=True, check=False)
        if checked.returncode == 0:
            validated_head = source_head
    layer_records = []
    for layer in layers:
        encoded = canonical(layer.value)
        encoded_sha = "sha256:" + hashlib.sha256(encoded).hexdigest()
        layer_records.append({"name": layer.name, "sha256": layer.hash,
                              "canonical_bytes_sha256": encoded_sha,
                              "canonical_bytes_length": len(encoded),
                              "canonical_bytes": {"encoding": "utf-8", "sha256": encoded_sha,
                                                  "length": len(encoded)},
                              "semantic_bytes_sha256": encoded_sha,
                              "semantic_bytes_length": len(encoded), "value": layer.value})
    controls: list[dict[str, Any]] = []
    baseline = {layer.name: layer.value for layer in layers}
    for target in ("authored", "resolved", "plan", "event", "output"):
        mutated = json.loads(json.dumps(baseline))
        if target == "authored":
            mutated[target]["nodes"][1]["gain_bits"] = "3f000000"
        elif target == "resolved":
            mutated[target]["nodes"][1]["implementation"] = "pulp.builtin.gain.v2"
        elif target == "plan":
            mutated[target]["generation"] += 1
        elif target == "event":
            mutated[target]["gain_by_sample"]["64"] = 0.25
        else:
            mutated[target]["partitions"][0]["pcm_sha256"] = "sha256:" + "0" * 64
        changed = [name for name in baseline if digest(baseline[name]) != digest(mutated[name])]
        controls.append({"mutation": target, "expected_failure_layer": target,
                         "rejected_at_layer": target if changed == [target] else None,
                         "status": "NOT_CHECKED",
                         "attribution": "PASS" if changed == [target] else "FAIL",
                         "changed_layers": changed})
    positive_controls = [{"layer": layer.name, "status": "NOT_CHECKED",
                          "reason": "synthetic baseline bookkeeping; no parser/replay acceptance",
                          "canonical_bytes": layer.hash} for layer in layers]
    return {
        "schema": SCHEMA,
        "source_head": validated_head,
        "provenance": {"tool": "tools/scripts/p0_layered_replay_receipt.py",
                        "tool_sha256": "sha256:" + hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                        "python": sys.version.split()[0], "platform": platform.platform(),
                        "requested_source_head": source_head,
                        "source_head": validated_head,
                        "source_head_status": "validated" if validated_head else "UNBOUND",
                        "sdk": {"consumers": [item["name"] for item in consumers],
                                "protocols": [item["protocol"] for item in consumers]}},
        "consumers": consumers,
        "consumer_gate": {"status": "NOT_QUALIFIED",
                           "reason": "partition execution and synthetic layer mutations are not qualified",
                           "requires": ["g3-installed-public-consumer", "spectral-installed-public-consumer"]},
        "layers": layer_records,
        "partition_controls": {"fixture": "canonical-sample-level-oracle", "partitions": list(PARTITIONS),
                                "status": "NOT_CHECKED",
                                "reason": "sample-level fixture does not execute distinct block boundaries",
                                "output_sha256": [output_for(p, baseline["event"], baseline["plan"])["pcm_sha256"]
                                                  for p in PARTITIONS],
                                "tolerances": {"peak": 1e-7, "rms": 1e-7,
                                               "pcm": "byte_exact"}},
        "positive_controls": positive_controls,
        "planted_negative_controls": {"fixture": "synthetic-layer-mutation-bookkeeping",
                                      "status": "NOT_CHECKED",
                                      "reason": "mutations are digest attribution only; no parser/replay refusal was run",
                                      "controls": controls},
        "unavailable_lanes": ["runtime", "realtime", "cross_platform", "scheduler", "provider_parity"],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-head", default="unknown")
    parser.add_argument("--g3-command", nargs="+")
    parser.add_argument("--g3-identity")
    parser.add_argument("--g3-protocol")
    parser.add_argument("--spectral-command", nargs="+")
    parser.add_argument("--spectral-identity")
    parser.add_argument("--spectral-protocol")
    parser.add_argument("--allow-unavailable", action="store_true",
                        help="write a NOT_QUALIFIED receipt when consumer commands are absent")
    args = parser.parse_args(argv)
    result = receipt(base_layers(), [
        run_consumer("g3-installed-public-consumer", args.g3_command,
                     args.g3_identity, args.g3_protocol, "g3", PARTITIONS),
        run_consumer("spectral-installed-public-consumer", args.spectral_command,
                     args.spectral_identity, args.spectral_protocol, "spectral", PARTITIONS),
    ],
                     args.source_head)
    missing = any(consumer["status"] == "UNAVAILABLE" for consumer in result["consumers"])
    failed = any(consumer["status"] == "FAIL" for consumer in result["consumers"])
    if failed or (missing and not args.allow_unavailable):
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(args.output), "receipt_sha256": digest(result),
                      "qualification": result["consumer_gate"]["status"],
                      "consumer_status": [consumer["status"] for consumer in result["consumers"]]},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
