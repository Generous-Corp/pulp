#!/usr/bin/env python3
"""Run the paced GPU-audio lead sweep and retain honest screening evidence.

The registered paced probe is a useful diagnostic, but it does not expose a
staged provider or GPU timestamps.  This driver therefore writes a separate
``pulp.gpu-audio.p4.lead-screening.v1`` JSONL stream.  It records the callback
envelope that the probe measures and emits explicit unavailable observations
for worker, encode, submit, completion, and GPU boundaries.  The stream is
not accepted by ``gpu_audio_p4_evidence.py`` and always leaves the verdict
unassigned.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
from typing import Any


SCHEMA = "pulp.gpu-audio.p4.lead-screening.v1"
LEADS = (1, 2, 4, 8)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unavailable(*, observer: str, api_source: str) -> dict[str, Any]:
    """Return a complete unavailable timing observation.

    The P4 timing contract forbids using zero for an unobservable boundary.
    Keeping all provenance keys present makes the screening artifact easy to
    promote only after a future benchmark supplies those measurements.
    """

    return {
        "value_ns": None,
        "availability": "unavailable",
        "clock_domain": "steady_clock",
        "observer": observer,
        "api_source": api_source,
        "relation": "unavailable",
        "start_clock_domain": "steady_clock",
        "end_clock_domain": "steady_clock",
        "start_observer": observer,
        "end_observer": observer,
        "callback_mode": "sleep_until_non_rt",
        "event_pump_strategy": "provider_worker",
        "timestamp_scope": "unavailable",
        "correlation_method": "unavailable",
        "uncertainty_ns": None,
        "instrumentation_overhead_ns": None,
        "instrumentation_control": "unavailable",
    }


def direct_callback(begin: int, end: int) -> dict[str, Any]:
    if begin < 0 or end < begin:
        raise ValueError("callback envelope is not monotonic")
    return {
        "value_ns": end - begin,
        "availability": "available",
        "clock_domain": "steady_clock",
        "observer": "callback_driver",
        "api_source": "std::chrono::steady_clock",
        "relation": "direct",
        "start_clock_domain": "steady_clock",
        "end_clock_domain": "steady_clock",
        "start_observer": "callback_driver",
        "end_observer": "callback_driver",
        "callback_mode": "sleep_until_non_rt",
        "event_pump_strategy": "provider_worker",
        "timestamp_scope": "external_callback_envelope",
        "correlation_method": "not_required",
        "uncertainty_ns": None,
        "instrumentation_overhead_ns": None,
        "instrumentation_control": "probe_clock_only_unbounded",
        "uncertainty_status": "unavailable",
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--frames", type=int, default=128, choices=(32, 64, 128))
    parser.add_argument("--blocks", type=int, default=1024)
    parser.add_argument("--warmup", type=int, default=16)
    parser.add_argument("--wake-on-write", action="store_true")
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    return parser.parse_args(argv)


def git_revision() -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"], check=True, capture_output=True, text=True
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    revision = result.stdout.strip()
    return revision if len(revision) == 40 else None


def write_trial_logs(directory: Path, trial: dict[str, Any]) -> None:
    """Persist the exact argv and child streams beside each probe receipt."""

    (directory / "command.json").write_text(
        json.dumps({"argv": trial["command"], "returncode": trial["returncode"]}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    (directory / "probe.stdout").write_text(trial["stdout"], encoding="utf-8")
    (directory / "probe.stderr").write_text(trial["stderr"], encoding="utf-8")


def run_trial(args: argparse.Namespace, lead: int, directory: Path) -> dict[str, Any]:
    command = [
        str(args.probe),
        f"--frames={args.frames}",
        f"--lead={lead}",
        f"--blocks={args.blocks}",
        f"--warmup={args.warmup}",
        f"--output-dir={directory}",
    ]
    if args.wake_on_write:
        command.append("--wake-on-write")
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=args.timeout_seconds
    )
    receipt_path = directory / "receipt.json"
    blocks_path = directory / "blocks.csv"
    if not receipt_path.is_file() or not blocks_path.is_file():
        raise RuntimeError(
            f"probe lead {lead} did not produce receipt.json and blocks.csv "
            f"(exit {completed.returncode})"
        )
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid receipt for lead {lead}: {error}") from error
    if receipt.get("schema") != "pulp.gpu-audio-paced-convolution.v1":
        raise RuntimeError(f"unexpected probe schema for lead {lead}")

    with blocks_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    measured = [row for row in rows if row.get("measured") == "1"]
    blocks: list[dict[str, Any]] = []
    for ordinal, row in enumerate(measured):
        try:
            begin = int(row["callback_begin_ns"])
            end = int(row["callback_end_ns"])
            sequence = int(row["callback_sequence"])
            miss_delta = int(row["miss_counter_delta"])
            error = float(row["max_absolute_error"])
        except (KeyError, TypeError, ValueError) as exc:
            raise RuntimeError(f"invalid block row for lead {lead}: {exc}") from exc
        callback = direct_callback(begin, end)
        timings = {
            "callback_cpu": callback,
            "worker_pack_copy": unavailable(observer="worker", api_source="not_observed"),
            "encode_cpu": unavailable(observer="worker", api_source="not_observed"),
            "submit_cpu": unavailable(observer="worker", api_source="not_observed"),
            "event_processing_cpu": unavailable(observer="worker", api_source="not_observed"),
            "retirement_cpu": unavailable(observer="worker", api_source="not_observed"),
            "worker_other_cpu": unavailable(observer="worker", api_source="not_observed"),
            "worker_end_to_end": unavailable(observer="worker", api_source="not_observed"),
            "submit_to_completion": unavailable(observer="worker", api_source="not_observed"),
            "gpu_elapsed": unavailable(observer="gpu", api_source="dawn_timestamps"),
            "completion_to_retirement_observed": unavailable(observer="worker", api_source="not_observed"),
            "retirement_to_result_visible": unavailable(observer="callback_driver", api_source="not_observed"),
            "publish_to_consumable": unavailable(observer="callback_driver", api_source="not_observed"),
        }
        blocks.append(
            {
                "record_kind": "block",
                "schema": SCHEMA,
                "block_ordinal": ordinal,
                "sequence": sequence,
                "lead_blocks": lead,
                "engine_id": None,
                "generation": None,
                "identity_status": "unavailable",
                # The origin/main paced probe does not expose one terminal or
                # delivery disposition per block.  Do not infer GPU delivery
                # from the aggregate miss counter.
                "gpu_terminal": "unavailable",
                "delivery": "unavailable",
                "deadline_miss": miss_delta > 0,
                "watchdog_expiry": False,
                "late_completion": False,
                "resync_drop": False,
                "miss_counter_delta": miss_delta,
                "oracle_max_absolute_error": error,
                "timings": timings,
                "transfers": {
                    "write_buffer_calls": 0,
                    "write_buffer_bytes": 0,
                    "output_copy_calls": 0,
                    "output_copy_bytes": 0,
                    "map_async_calls": 0,
                    "mapped_readback_memcpy_calls": 0,
                    "mapped_readback_memcpy_bytes": 0,
                },
            }
        )
    return {
        "lead": lead,
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "receipt": receipt,
        "receipt_sha256": sha256(receipt_path),
        "blocks_sha256": sha256(blocks_path),
        "blocks": blocks,
    }


def run(args: argparse.Namespace) -> int:
    if not args.probe.is_file() or not os.access(args.probe, os.X_OK):
        raise RuntimeError(f"probe is not an executable file: {args.probe}")
    if args.blocks < 1 or args.warmup < 0:
        raise ValueError("blocks must be positive and warmup must be non-negative")
    if args.output_dir.exists():
        raise RuntimeError(f"output directory must not already exist: {args.output_dir}")
    args.output_dir.mkdir(parents=True)
    probe_sha256 = sha256(args.probe)
    trials: list[dict[str, Any]] = []
    try:
        for lead in LEADS:
            trial_dir = args.output_dir / f"lead-{lead}"
            # The paced probe insists that its output directory is new and
            # creates it itself.  Keep only the campaign root under our
            # control; pre-creating this child makes every real trial fail
            # closed with "output directory must be new".
            trial = run_trial(args, lead, trial_dir)
            write_trial_logs(trial_dir, trial)
            trial["command_sha256"] = sha256(trial_dir / "command.json")
            trial["stdout_sha256"] = sha256(trial_dir / "probe.stdout")
            trial["stderr_sha256"] = sha256(trial_dir / "probe.stderr")
            trials.append(trial)
    except Exception:
        shutil.rmtree(args.output_dir, ignore_errors=True)
        raise

    manifest = {
        "schema": SCHEMA,
        "record_kind": "manifest",
        "campaign": "screening",
        "campaign_id": args.output_dir.name,
        "performance_verdict": "unassigned",
        "acceptance_status": "screening_only",
        "source_revision": git_revision(),
        "probe_sha256": probe_sha256,
        "probe_path": args.probe.name,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "machine_id": platform.node() or "unavailable",
        "machine_model": platform.machine() or "unavailable",
        "os_version": platform.platform(),
        "frames": args.frames,
        "sample_rate_hz": 48000,
        "channels": 2,
        "ir_frames": 257,
        "warmup_blocks": args.warmup,
        "measured_blocks": args.blocks,
        "leads_blocks": list(LEADS),
        "wake_on_write": args.wake_on_write,
        "staged_async_present": False,
        "gpu_timestamps": "unavailable",
        "instrumentation_limit": "paced probe exposes callback envelope only",
        "trials": [
            {
                "lead": trial["lead"],
                "returncode": trial["returncode"],
                "receipt_sha256": trial["receipt_sha256"],
                "blocks_sha256": trial["blocks_sha256"],
                "command_sha256": trial["command_sha256"],
                "stdout_sha256": trial["stdout_sha256"],
                "stderr_sha256": trial["stderr_sha256"],
                "measured_gpu_deliveries": trial["receipt"].get("measured_gpu_deliveries"),
                "measured_cpu_fallbacks": trial["receipt"].get("measured_cpu_fallbacks"),
            }
            for trial in trials
        ],
    }
    raw_path = args.output_dir / "lead-screening.jsonl"
    with raw_path.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(manifest, sort_keys=True) + "\n")
        for index, trial in enumerate(trials, 1):
            begin = {
                "schema": SCHEMA,
                "record_kind": "trial_begin",
                "trial_id": index,
                "pair_id": trial["lead"],
                "path": "shared_async",
                "lead_blocks": trial["lead"],
                "performance_verdict": "unassigned",
            }
            stream.write(json.dumps(begin, sort_keys=True) + "\n")
            for block in trial["blocks"]:
                block.update({"trial_id": index, "pair_id": trial["lead"], "path": "shared_async"})
                stream.write(json.dumps(block, sort_keys=True) + "\n")
            end = {
                "schema": SCHEMA,
                "record_kind": "trial_end",
                "trial_id": index,
                "pair_id": trial["lead"],
                "path": "shared_async",
                "block_count": len(trial["blocks"]),
                "receipt_sha256": trial["receipt_sha256"],
                "blocks_sha256": trial["blocks_sha256"],
                "performance_verdict": "unassigned",
                "ui_frame_p99": unavailable(observer="ui", api_source="not_observed"),
                "duration": unavailable(observer="campaign", api_source="not_observed"),
            }
            stream.write(json.dumps(end, sort_keys=True) + "\n")
    manifest["raw_sha256"] = sha256(raw_path)
    (args.output_dir / "campaign.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps({"schema": SCHEMA, "status": "completed", "performance_verdict": "unassigned", "output_dir": str(args.output_dir), "raw_sha256": manifest["raw_sha256"]}))
    return 0


def main(argv: list[str] | None = None) -> int:
    try:
        return run(parse_args(argv if argv is not None else sys.argv[1:]))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"gpu_audio_p4_lead_campaign: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
