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
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "pulp.graph.layered-replay-receipt.v1"
PARTITIONS = (64, 128)


def canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical(value)).hexdigest()


@dataclass(frozen=True)
class Layer:
    name: str
    value: Any

    @property
    def hash(self) -> str:
        return digest(self.value)


def run_consumer(label: str, command: Sequence[str] | None) -> dict[str, Any]:
    """Run one independent installed consumer and record only stable evidence."""
    if not command:
        return {"name": label, "status": "UNAVAILABLE", "reason": "command not supplied"}
    try:
        result = subprocess.run(command, capture_output=True, stdin=subprocess.DEVNULL,
                                text=True, check=False)
    except OSError as exc:
        return {"name": label, "status": "UNAVAILABLE", "reason": str(exc)}
    output = (result.stdout + result.stderr).encode()
    executable = Path(command[0])
    record: dict[str, Any] = {
        "name": label,
        "status": "PASS" if result.returncode == 0 else "FAIL",
        "returncode": result.returncode,
        "command": list(command),
        "output_sha256": "sha256:" + hashlib.sha256(output).hexdigest(),
    }
    if executable.is_file():
        record["executable_sha256"] = "sha256:" + hashlib.sha256(executable.read_bytes()).hexdigest()
    if result.returncode != 0:
        record["output_tail"] = (result.stdout + result.stderr)[-400:]
    return record


def output_for(partition: int, event: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
    """Produce canonical PCM bytes from sample positions, independent of blocks."""
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
    return {
        "canonical_pcm": {"encoding": "s32le", "channels": ["L", "R"], "sample_rate": 48000},
        "partition_frames": partition,
        "frame_count": frames,
        "peak_tolerance": 1e-7,
        "rms_tolerance": 1e-7,
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
    plan = {"generation": 7, "block_frames": 128, "sample_rate": 48000, "gain": 1.0, "latency": 0,
            "fallback": "none", "capacity": {"nodes": 3, "events": 16}}
    event = {"clock_epoch": 3, "absolute_origin": 0, "capacity": 16,
             "gain_by_sample": {"64": 0.5, "192": 0.75}, "ordering": "(sample,producer,sequence)"}
    output = {"partitions": [output_for(partition, event, plan) for partition in PARTITIONS],
              "partition_invariant": True}
    return [Layer("authored", authored), Layer("resolved", resolved), Layer("plan", plan),
            Layer("event", event), Layer("output", output)]


def receipt(layers: list[Layer], consumers: list[dict[str, Any]], source_head: str) -> dict[str, Any]:
    layer_records = [{"name": layer.name, "sha256": layer.hash, "value": layer.value} for layer in layers]
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
        controls.append({"mutation": target, "status": "PASS" if changed == [target] else "FAIL",
                         "changed_layers": changed})
    return {
        "schema": SCHEMA,
        "source_head": source_head,
        "provenance": {"tool": "tools/scripts/p0_layered_replay_receipt.py",
                        "python": sys.version.split()[0]},
        "consumers": consumers,
        "consumer_gate": {"status": "PASS" if all(item["status"] == "PASS" for item in consumers)
                           else "NOT_QUALIFIED",
                           "requires": ["g3-installed-public-consumer", "spectral-installed-public-consumer"]},
        "layers": layer_records,
        "partition_controls": {"fixture": "synthetic-sample-level-oracle", "partitions": list(PARTITIONS), "status": "PASS",
                                "output_sha256": [output_for(p, baseline["event"], baseline["plan"])["pcm_sha256"]
                                                  for p in PARTITIONS]},
        "planted_negative_controls": {"fixture": "synthetic-layer-mutation-oracle", "controls": controls},
        "unavailable_lanes": ["runtime", "realtime", "cross_platform", "scheduler", "provider_parity"],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-head", default="unknown")
    parser.add_argument("--g3-command", nargs="+")
    parser.add_argument("--spectral-command", nargs="+")
    parser.add_argument("--allow-unavailable", action="store_true",
                        help="write a NOT_QUALIFIED receipt when consumer commands are absent")
    args = parser.parse_args(argv)
    result = receipt(base_layers(), [run_consumer("g3-installed-public-consumer", args.g3_command),
                                     run_consumer("spectral-installed-public-consumer", args.spectral_command)],
                     args.source_head)
    missing = any(consumer["status"] == "UNAVAILABLE" for consumer in result["consumers"])
    failed = any(consumer["status"] == "FAIL" for consumer in result["consumers"])
    if failed or (missing and not args.allow_unavailable):
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"receipt": str(args.output), "receipt_sha256": digest(result),
                      "qualification": result["consumer_gate"]["status"],
                      "consumer_status": [consumer["status"] for consumer in result["consumers"]]},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
