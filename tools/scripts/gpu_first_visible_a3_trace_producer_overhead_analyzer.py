#!/usr/bin/env python3
"""Replay one A3 trace-producer overhead sample from trace facts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Any

ANALYSIS_SCHEMA = "pulp.gpu-first-visible-trace-producer-replay.v1"
REQUEST_KEYS = {
    "evidence_id", "host_pid", "process_start_identity", "executable_sha256",
    "audio_thread_tids", "session_config_sha256", "ring_bytes",
}
SHA256_LENGTH = 64


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
    session_events: int, xrun_events: int,
) -> dict[str, Any]:
    audio_tids = set(request["audio_thread_tids"])
    audio_events = sum(tid in audio_tids for tid in producer_tids)
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
        "session_events": session_events,
        "xrun_events": xrun_events,
        "audio_thread_producer_events": audio_events,
    }
    if (
        producer_events < 1
        or foreign_producer_events != 0
        or payload["producer_pids"] != [request["host_pid"]]
        or session_events != 1
        or xrun_events != 0
        or audio_events != 0
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
    xrun_events = 0
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
            if args.get("gpu_evidence_id") == request["evidence_id"]:
                producer_events += 1
            else:
                foreign_events += 1
        if category == "dsp" and isinstance(name, str) and (
            name.startswith("xrun") or name.startswith("deadline_miss")
        ):
            xrun_events += 1
        if category == "metadata" and name == "pulp_a3_trace_session":
            session_matches = (
                event.get("pid") == request["host_pid"]
                and args.get("gpu_evidence_id") == request["evidence_id"]
                and args.get("process_start_identity") == request["process_start_identity"]
                and args.get("executable_sha256") == request["executable_sha256"]
                and args.get("session_config_sha256") == request["session_config_sha256"]
                and args.get("audio_thread_tids_sha256") == expected_tids_digest
                and args.get("ring_bytes") == request["ring_bytes"]
                and args.get("session_active") is True
            )
            if session_matches:
                session_events += 1
    return result(
        trace_format="chrome-json", request=request,
        producer_events=producer_events, foreign_producer_events=foreign_events,
        producer_pids=producer_pids, producer_tids=producer_tids,
        session_events=session_events, xrun_events=xrun_events,
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
  SELECT process.pid AS pid, thread.tid AS tid,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.gpu_evidence_id') AS STRING) AS evidence_id
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread ON thread_track.utid = thread.utid
  JOIN process ON thread.upid = process.upid
  WHERE slice.category = 'gpu' AND slice.name = 'gpu_health_transition_first_visible'
), sessions AS (
  SELECT process.pid AS pid,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.gpu_evidence_id') AS STRING) AS evidence_id,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.process_start_identity') AS STRING) AS process_start,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.executable_sha256') AS STRING) AS executable_sha,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.session_config_sha256') AS STRING) AS config_sha,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.audio_thread_tids_sha256') AS STRING) AS tids_sha,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.ring_bytes') AS INT) AS ring_bytes,
         CAST(EXTRACT_ARG(slice.arg_set_id, 'debug.session_active') AS INT) AS active
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread ON thread_track.utid = thread.utid
  JOIN process ON thread.upid = process.upid
  WHERE slice.category = 'metadata' AND slice.name = 'pulp_a3_trace_session'
), xruns AS (
  SELECT 1 FROM slice WHERE category = 'dsp'
    AND (name GLOB 'xrun*' OR name GLOB 'deadline_miss*')
)
SELECT
  (SELECT COUNT(*) FROM producer WHERE evidence_id = {evidence}) AS producer_events,
  (SELECT COUNT(*) FROM producer WHERE evidence_id IS NULL OR evidence_id != {evidence}) AS foreign_events,
  (SELECT GROUP_CONCAT(DISTINCT pid) FROM producer WHERE evidence_id = {evidence}) AS producer_pids,
  (SELECT GROUP_CONCAT(DISTINCT tid) FROM producer WHERE evidence_id = {evidence}) AS producer_tids,
  (SELECT COUNT(*) FROM sessions WHERE pid = {request['host_pid']}
    AND evidence_id = {evidence} AND process_start = {start}
    AND executable_sha = {executable} AND config_sha = {config}
    AND tids_sha = {tids} AND ring_bytes = {request['ring_bytes']} AND active = 1) AS session_events,
  (SELECT COUNT(*) FROM xruns) AS xrun_events;
"""
    completed = subprocess.run(
        [str(trace_processor), "--query-string", query, str(trace)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=60, check=False,
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
            session_events=int(row["session_events"]),
            xrun_events=int(row["xrun_events"]),
        )
    except (KeyError, ValueError) as error:
        raise TraceReplayError("trace processor output is not the closed replay shape") from error


def analyze_trace(
    trace: Path, request_value: Any, trace_processor: Path | None = None,
) -> dict[str, Any]:
    request = validate_request(request_value)
    data = trace.read_bytes()
    if not data:
        raise TraceReplayError("active trace is empty")
    if data.lstrip().startswith(b"{"):
        return analyze_chrome_json(data, request)
    if trace_processor is None:
        raise TraceReplayError("binary Perfetto trace requires the pinned trace processor")
    return analyze_proto(trace, request, trace_processor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--trace-processor", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        request = json.loads(args.request.read_text(encoding="utf-8"))
        payload = analyze_trace(args.trace, request, args.trace_processor)
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
