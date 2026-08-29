#!/usr/bin/env python3
"""Replay one A3 trace-producer overhead sample from trace facts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import subprocess
from pathlib import Path
from typing import Any

ANALYSIS_SCHEMA = "pulp.gpu-first-visible-trace-producer-replay.v1"
REQUEST_KEYS = {
    "evidence_id", "host_pid", "process_start_identity", "executable_sha256",
    "audio_thread_tids", "session_config_sha256", "ring_bytes",
    "collection_challenge_nonce",
}
SHA256_LENGTH = 64
PINNED_PROCESSOR_VERSION = "v57.2"
PINNED_PROCESSOR_SHA256 = {
    ("Darwin", "arm64"): "98a41b80e9f60da0373d64aff6455681f8c26b7c391ae5736324a5b11e3dacc2",
    ("Darwin", "aarch64"): "98a41b80e9f60da0373d64aff6455681f8c26b7c391ae5736324a5b11e3dacc2",
    ("Darwin", "x86_64"): "c0f61397901da47cbe1bb9a0843624f7c2038ac92176ce15e3736ce9aa0afef0",
    ("Linux", "x86_64"): "55ba613fc6d4f71df81eee2dbfc293020063655c241b3e314bff75345b802684",
    ("Linux", "amd64"): "55ba613fc6d4f71df81eee2dbfc293020063655c241b3e314bff75345b802684",
    ("Linux", "aarch64"): "1dcc1d9aaff2eb92e8bc58f1957e4e445600294bd61dbc09345c1018c5ff0868",
    ("Linux", "arm64"): "1dcc1d9aaff2eb92e8bc58f1957e4e445600294bd61dbc09345c1018c5ff0868",
}


class TraceReplayError(ValueError):
    """The trace cannot prove the requested producer sample."""


def exact_keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise TraceReplayError(f"{label} has the wrong fields")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tids_digest(tids: list[int]) -> str:
    return sha256_bytes(
        json.dumps(tids, separators=(",", ":")).encode("utf-8")
    )


def validate_request(value: Any) -> dict[str, Any]:
    exact_keys(value, REQUEST_KEYS, "trace replay request")
    assert isinstance(value, dict)
    tids = value["audio_thread_tids"]
    if (
        not isinstance(value["evidence_id"], str)
        or len(value["evidence_id"]) != 32
        or any(character not in "0123456789abcdef" for character in value["evidence_id"])
        or not isinstance(value["host_pid"], int)
        or isinstance(value["host_pid"], bool)
        or value["host_pid"] <= 0
        or not isinstance(value["process_start_identity"], str)
        or not value["process_start_identity"]
        or not isinstance(value["executable_sha256"], str)
        or len(value["executable_sha256"]) != SHA256_LENGTH
        or not isinstance(value["session_config_sha256"], str)
        or len(value["session_config_sha256"]) != SHA256_LENGTH
        or value["ring_bytes"] != 128 * 1024 * 1024
        or not isinstance(value["collection_challenge_nonce"], str)
        or len(value["collection_challenge_nonce"]) != 32
        or any(
            character not in "0123456789abcdef"
            for character in value["collection_challenge_nonce"]
        )
        or not isinstance(tids, list)
        or not tids
        or any(not isinstance(item, int) or isinstance(item, bool) or item <= 0 for item in tids)
        or tids != sorted(set(tids))
    ):
        raise TraceReplayError("trace replay request is invalid")
    for field in ("executable_sha256", "session_config_sha256"):
        if any(character not in "0123456789abcdef" for character in value[field]):
            raise TraceReplayError("trace replay request digest is invalid")
    return value


def result(
    *, trace_format: str, request: dict[str, Any], producer_events: int,
    foreign_producer_events: int, producer_pids: list[int], producer_tids: list[int],
    producer_upids: list[int], session_events: int, session_upids: list[int],
    xrun_events: int, incomplete_slices: int, data_loss_count: int,
    no_flush_count: int, input_to_present_events: dict[str, int],
    input_to_present_pids: list[int], input_to_present_tids: list[int],
    input_to_present_upids: list[int],
) -> dict[str, Any]:
    audio_tids = set(request["audio_thread_tids"])
    audio_events = sum(tid in audio_tids for tid in producer_tids)
    audio_stage_events = sum(tid in audio_tids for tid in input_to_present_tids)
    payload = {
        "schema": ANALYSIS_SCHEMA,
        "version": 1,
        "trace_format": trace_format,
        "evidence_id": request["evidence_id"],
        "host_pid": request["host_pid"],
        "process_start_identity": request["process_start_identity"],
        "executable_sha256": request["executable_sha256"],
        "session_config_sha256": request["session_config_sha256"],
        "ring_bytes": request["ring_bytes"],
        "producer_events": producer_events,
        "foreign_producer_events": foreign_producer_events,
        "producer_pids": sorted(set(producer_pids)),
        "producer_tids": sorted(set(producer_tids)),
        "producer_upids": sorted(set(producer_upids)),
        "session_events": session_events,
        "session_upids": sorted(set(session_upids)),
        "xrun_events": xrun_events,
        "audio_thread_producer_events": audio_events,
        "capture_integrity": {
            "incomplete_slices": incomplete_slices,
            "data_loss_count": data_loss_count,
            "no_flush_count": no_flush_count,
        },
        "input_to_present_events": input_to_present_events,
        "input_to_present_pids": sorted(set(input_to_present_pids)),
        "input_to_present_tids": sorted(set(input_to_present_tids)),
        "input_to_present_upids": sorted(set(input_to_present_upids)),
        "audio_thread_input_to_present_events": audio_stage_events,
    }
    if (
        producer_events < 1
        or foreign_producer_events != 0
        or payload["producer_pids"] != [request["host_pid"]]
        or len(payload["producer_upids"]) != 1
        or session_events != 1
        or payload["session_upids"] != payload["producer_upids"]
        or xrun_events != 0
        or audio_events != 0
        or incomplete_slices != 0
        or data_loss_count != 0
        or no_flush_count != 0
        or set(input_to_present_events) != {"gpu_acquire", "gpu_submit", "gpu_present"}
        or any(value < 1 for value in input_to_present_events.values())
        or payload["input_to_present_pids"] != [request["host_pid"]]
        or payload["input_to_present_upids"] != payload["producer_upids"]
        or audio_stage_events != 0
    ):
        raise TraceReplayError("trace does not prove the bounded producer session")
    return payload


def analyze_chrome_json(data: bytes, request: dict[str, Any]) -> dict[str, Any]:
    try:
        document = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise TraceReplayError("trace is not valid Chrome trace JSON") from error
    if not isinstance(document, dict) or set(document) != {"traceEvents"}:
        raise TraceReplayError("Chrome trace JSON has the wrong root fields")
    events = document["traceEvents"]
    if not isinstance(events, list) or not events:
        raise TraceReplayError("Chrome trace JSON has no events")
    producer_events = 0
    foreign_events = 0
    producer_pids: list[int] = []
    producer_tids: list[int] = []
    session_events = 0
    producer_upids: list[int] = []
    session_upids: list[int] = []
    xrun_events = 0
    input_to_present_events = {
        "gpu_acquire": 0, "gpu_submit": 0, "gpu_present": 0,
    }
    stage_pids: list[int] = []
    stage_tids: list[int] = []
    stage_upids: list[int] = []
    expected_tids_digest = tids_digest(request["audio_thread_tids"])
    for index, event in enumerate(events):
        if not isinstance(event, dict):
            raise TraceReplayError(f"trace event {index} is not an object")
        name = event.get("name")
        category = event.get("cat")
        args = event.get("args", {})
        if not isinstance(args, dict):
            raise TraceReplayError(f"trace event {index} args are invalid")
        if category == "gpu" and name == "gpu_health_transition_first_visible":
            pid = event.get("pid")
            tid = event.get("tid")
            if (
                not isinstance(pid, int) or isinstance(pid, bool)
                or not isinstance(tid, int) or isinstance(tid, bool)
            ):
                raise TraceReplayError("producer event lacks numeric PID/TID")
            producer_pids.append(pid)
            producer_tids.append(tid)
            producer_upids.append(pid)
            if args.get("gpu_evidence_id") == request["evidence_id"]:
                producer_events += 1
            else:
                foreign_events += 1
        if category == "dsp" and isinstance(name, str) and (
            name.startswith("xrun") or name.startswith("deadline_miss")
        ):
            xrun_events += 1
        if category == "render" and name in input_to_present_events:
            pid = event.get("pid")
            tid = event.get("tid")
            if (
                not isinstance(pid, int) or isinstance(pid, bool)
                or not isinstance(tid, int) or isinstance(tid, bool)
            ):
                raise TraceReplayError("input-to-present event lacks numeric PID/TID")
            input_to_present_events[name] += 1
            stage_pids.append(pid)
            stage_tids.append(tid)
            stage_upids.append(pid)
        if category == "metadata" and name == "pulp_a3_trace_session":
            session_matches = (
                event.get("pid") == request["host_pid"]
                and args.get("gpu_evidence_id") == request["evidence_id"]
                and args.get("process_start_identity") == request["process_start_identity"]
                and args.get("executable_sha256") == request["executable_sha256"]
                and args.get("session_config_sha256") == request["session_config_sha256"]
                and args.get("collection_challenge_nonce")
                == request["collection_challenge_nonce"]
                and args.get("audio_thread_tids_sha256") == expected_tids_digest
                and args.get("ring_bytes") == request["ring_bytes"]
                and args.get("session_active") is True
            )
            if session_matches:
                session_events += 1
                session_upids.append(event["pid"])
    return result(
        trace_format="chrome-json", request=request,
        producer_events=producer_events, foreign_producer_events=foreign_events,
        producer_pids=producer_pids, producer_tids=producer_tids,
        producer_upids=producer_upids, session_events=session_events,
        session_upids=session_upids, xrun_events=xrun_events,
        incomplete_slices=0, data_loss_count=0, no_flush_count=0,
        input_to_present_events=input_to_present_events,
        input_to_present_pids=stage_pids, input_to_present_tids=stage_tids,
        input_to_present_upids=stage_upids,
    )


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def parse_integer_list(value: str) -> list[int]:
    if value in {"", "[NULL]"}:
        return []
    try:
        return [int(item) for item in value.split(",")]
    except ValueError as error:
        raise TraceReplayError("trace processor returned an invalid integer list") from error


def analyze_proto(
    trace: Path, request: dict[str, Any], trace_processor: Path,
) -> dict[str, Any]:
    tids_hash = tids_digest(request["audio_thread_tids"])
    evidence = sql_literal(request["evidence_id"])
    start = sql_literal(request["process_start_identity"])
    executable = sql_literal(request["executable_sha256"])
    config = sql_literal(request["session_config_sha256"])
    tids = sql_literal(tids_hash)
    query = f"""
WITH producer AS (
  SELECT process.upid AS upid, process.pid AS pid, thread.tid AS tid,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.gpu_evidence_id') AS STRING) AS evidence_id
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread ON thread_track.utid = thread.utid
  JOIN process ON thread.upid = process.upid
  WHERE slice.category = 'gpu' AND slice.name = 'gpu_health_transition_first_visible'
    AND slice.dur >= 0
), sessions AS (
  SELECT process.upid AS upid, process.pid AS pid,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.gpu_evidence_id') AS STRING) AS evidence_id,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.process_start_identity') AS STRING) AS process_start,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.executable_sha256') AS STRING) AS executable_sha,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.session_config_sha256') AS STRING) AS config_sha,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.audio_thread_tids_sha256') AS STRING) AS tids_sha,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.collection_challenge_nonce') AS STRING) AS challenge_nonce,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.ring_bytes') AS INT) AS ring_bytes,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.session_active') AS INT) AS active
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread ON thread_track.utid = thread.utid
  JOIN process ON thread.upid = process.upid
  WHERE slice.category = 'metadata' AND slice.name = 'pulp_a3_trace_session'
    AND slice.dur >= 0
), xruns AS (
  SELECT 1 FROM slice WHERE category = 'dsp' AND dur >= 0
    AND (name GLOB 'xrun*' OR name GLOB 'deadline_miss*')
), stages AS (
  SELECT process.upid AS upid, process.pid AS pid, thread.tid AS tid,
         slice.name AS name
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread ON thread_track.utid = thread.utid
  JOIN process ON thread.upid = process.upid
  WHERE slice.category = 'render' AND slice.dur >= 0
    AND slice.name IN ('gpu_acquire', 'gpu_submit', 'gpu_present')
)
SELECT
  (SELECT COUNT(*) FROM producer WHERE evidence_id = {evidence}) AS producer_events,
  (SELECT COUNT(*) FROM producer WHERE evidence_id IS NULL OR evidence_id != {evidence}) AS foreign_events,
  (SELECT GROUP_CONCAT(DISTINCT pid) FROM producer WHERE evidence_id = {evidence}) AS producer_pids,
  (SELECT GROUP_CONCAT(DISTINCT tid) FROM producer WHERE evidence_id = {evidence}) AS producer_tids,
  (SELECT GROUP_CONCAT(DISTINCT upid) FROM producer WHERE evidence_id = {evidence}) AS producer_upids,
  (SELECT COUNT(*) FROM sessions WHERE pid = {request['host_pid']}
    AND evidence_id = {evidence} AND process_start = {start}
    AND executable_sha = {executable} AND config_sha = {config}
    AND tids_sha = {tids}
    AND challenge_nonce = {sql_literal(request['collection_challenge_nonce'])}
    AND ring_bytes = {request['ring_bytes']} AND active = 1) AS session_events,
  (SELECT GROUP_CONCAT(DISTINCT upid) FROM sessions WHERE pid = {request['host_pid']}
    AND evidence_id = {evidence} AND process_start = {start}
    AND executable_sha = {executable} AND config_sha = {config}
    AND tids_sha = {tids}
    AND challenge_nonce = {sql_literal(request['collection_challenge_nonce'])}
    AND ring_bytes = {request['ring_bytes']} AND active = 1) AS session_upids,
  (SELECT COUNT(*) FROM xruns) AS xrun_events,
  (SELECT COUNT(*) FROM stages WHERE name = 'gpu_acquire') AS gpu_acquire_events,
  (SELECT COUNT(*) FROM stages WHERE name = 'gpu_submit') AS gpu_submit_events,
  (SELECT COUNT(*) FROM stages WHERE name = 'gpu_present') AS gpu_present_events,
  (SELECT GROUP_CONCAT(DISTINCT pid) FROM stages) AS stage_pids,
  (SELECT GROUP_CONCAT(DISTINCT tid) FROM stages) AS stage_tids,
  (SELECT GROUP_CONCAT(DISTINCT upid) FROM stages) AS stage_upids,
  (SELECT COUNT(*) FROM slice WHERE dur = -1) AS incomplete_slices,
  COALESCE((SELECT SUM(value) FROM stats
    WHERE severity = 'data_loss' AND value > 0), 0) AS data_loss_count,
  COALESCE((SELECT SUM(value) FROM stats WHERE value > 0
    AND (name GLOB '*no_flush*' OR name GLOB '*not_flushed*')), 0) AS no_flush_count;
"""
    expected_digest = PINNED_PROCESSOR_SHA256.get(
        (platform.system(), platform.machine())
    )
    if expected_digest is None:
        raise TraceReplayError("no pinned trace processor covers this host platform")
    if sha256_bytes(trace_processor.read_bytes()) != expected_digest:
        raise TraceReplayError(
            f"trace processor is not Pulp's pinned {PINNED_PROCESSOR_VERSION} host binary"
        )
    completed = subprocess.run(
        [str(trace_processor), "--query-string", query, str(trace)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=60, check=False,
        env={"HOME": "/var/empty", "LC_ALL": "C", "PATH": "/usr/bin:/bin"},
    )
    if completed.returncode != 0:
        raise TraceReplayError(
            f"trace processor rejected active trace: {completed.stderr.strip()}"
        )
    rows = list(csv.DictReader(completed.stdout.splitlines()))
    if len(rows) != 1:
        raise TraceReplayError("trace processor returned the wrong result shape")
    row = rows[0]
    try:
        return result(
            trace_format="perfetto-proto", request=request,
            producer_events=int(row["producer_events"]),
            foreign_producer_events=int(row["foreign_events"]),
            producer_pids=parse_integer_list(row["producer_pids"]),
            producer_tids=parse_integer_list(row["producer_tids"]),
            producer_upids=parse_integer_list(row["producer_upids"]),
            session_events=int(row["session_events"]),
            session_upids=parse_integer_list(row["session_upids"]),
            xrun_events=int(row["xrun_events"]),
            incomplete_slices=int(row["incomplete_slices"]),
            data_loss_count=int(row["data_loss_count"]),
            no_flush_count=int(row["no_flush_count"]),
            input_to_present_events={
                "gpu_acquire": int(row["gpu_acquire_events"]),
                "gpu_submit": int(row["gpu_submit_events"]),
                "gpu_present": int(row["gpu_present_events"]),
            },
            input_to_present_pids=parse_integer_list(row["stage_pids"]),
            input_to_present_tids=parse_integer_list(row["stage_tids"]),
            input_to_present_upids=parse_integer_list(row["stage_upids"]),
        )
    except (KeyError, ValueError) as error:
        raise TraceReplayError("trace processor output is not the closed replay shape") from error


def analyze_trace(
    trace: Path, request_value: Any, trace_processor: Path | None = None,
    *, allow_fixture_chrome_json: bool = False,
) -> dict[str, Any]:
    request = validate_request(request_value)
    data = trace.read_bytes()
    if not data:
        raise TraceReplayError("active trace is empty")
    json_candidate = data[3:] if data.startswith(b"\xef\xbb\xbf") else data
    if json_candidate.lstrip().startswith((b"{", b"[")):
        if not allow_fixture_chrome_json:
            raise TraceReplayError(
                "Chrome trace JSON is a nonterminal planted-fixture format"
            )
        return analyze_chrome_json(json_candidate, request)
    if trace_processor is None:
        raise TraceReplayError("binary Perfetto trace requires the pinned trace processor")
    return analyze_proto(trace, request, trace_processor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--trace-processor", type=Path)
    parser.add_argument("--allow-fixture-chrome-json", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        request = json.loads(args.request.read_text(encoding="utf-8"))
        payload = analyze_trace(
            args.trace, request, args.trace_processor,
            allow_fixture_chrome_json=args.allow_fixture_chrome_json,
        )
    except (OSError, json.JSONDecodeError, TraceReplayError) as error:
        print(f"A3 trace producer replay: FAIL: {error}")
        return 1
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if args.output is None:
        print(encoded, end="")
    else:
        args.output.write_text(encoded, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
