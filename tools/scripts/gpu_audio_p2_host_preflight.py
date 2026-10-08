#!/usr/bin/env python3
"""Collect a fail-closed Apple-Silicon P2 host admission receipt.

This is an admission probe, not a performance benchmark.  It refuses to
authorize a campaign when any host observation is unavailable, stale, or over
the conservative quiet-window thresholds.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import sys
from typing import Any, Callable

SCHEMA = "pulp.gpu-audio.p2.host-preflight.v1"
REPO_ROOT = Path(__file__).resolve().parents[2]
HOST_VITALS = REPO_ROOT / "tools/scripts/host_vitals.sh"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _git_head() -> str:
    result = subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=False, encoding="utf-8")
    head = result.stdout.strip()
    if result.returncode or not re.fullmatch(r"[0-9a-f]{40}", head):
        raise RuntimeError("unable to establish immutable source revision")
    dirty = subprocess.run(["git", "-C", str(REPO_ROOT), "status", "--porcelain",
                            "--untracked-files=no"], capture_output=True, text=True,
                           check=False, encoding="utf-8")
    if dirty.returncode or dirty.stdout:
        raise RuntimeError("source tree has tracked modifications")
    return head


def _run(command: list[str], runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
         timeout: int = 10) -> dict[str, Any]:
    try:
        result = runner(command, capture_output=True, text=True, check=False,
                        encoding="utf-8", timeout=timeout)
    except (FileNotFoundError, OSError) as exc:
        # A missing observation tool is an admission failure, not a producer
        # crash. Preserve the command and error so the receipt remains
        # auditable while collect() applies its fail-closed checks.
        message = f"{type(exc).__name__}: {exc}"
        return {"argv": command, "returncode": 127, "stdout": "",
                "stderr": message, "stdout_sha256": _sha256(b""),
                "stderr_sha256": _sha256(message.encode())}
    except subprocess.TimeoutExpired as exc:
        message = f"TimeoutExpired: {exc}"
        return {"argv": command, "returncode": 124, "stdout": "",
                "stderr": message, "stdout_sha256": _sha256(b""),
                "stderr_sha256": _sha256(message.encode())}
    return {"argv": command, "returncode": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr,
            "stdout_sha256": _sha256(result.stdout.encode()),
            "stderr_sha256": _sha256(result.stderr.encode())}


def _parse_processes(raw: str) -> list[dict[str, Any]]:
    rows = []
    for line in raw.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) != 3:
            continue
        try:
            rows.append({"pid": int(fields[0]), "cpu_pct": float(fields[1]),
                         "command": fields[2]})
        except ValueError:
            continue
    return rows


def _parse_gpu(raw: str) -> tuple[str, int | None]:
    if not raw.strip():
        return "unavailable", None
    busy = re.findall(r"\bbusy\s+(\d+)", raw, flags=re.IGNORECASE)
    if not busy:
        return "unknown", None
    # IORegistry's ``busy`` field is a registry bookkeeping value.  It is not
    # authenticated evidence that the accelerator execution queues are idle.
    # Preserve the parsed value for diagnostics, but never admit a campaign
    # from it.
    return "unknown", sum(int(value) for value in busy)


def collect(*, runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
            source_revision: str | None = None) -> dict[str, Any]:
    """Collect observations and return a receipt, including blocked status."""
    source = source_revision or _git_head()
    if not re.fullmatch(r"[0-9a-f]{40}", source):
        raise RuntimeError("source_revision is not an immutable commit SHA")
    observations = [
        _run([str(HOST_VITALS), "--json"], runner),
        _run(["ps", "-axo", "pid=,pcpu=,comm="], runner),
        _run(["pmset", "-g", "therm"], runner),
        _run(["ioreg", "-r", "-c", "IOAccelerator", "-l"], runner),
    ]
    reasons: list[str] = []
    try:
        vitals = json.loads(observations[0]["stdout"])
    except (json.JSONDecodeError, TypeError):
        vitals = {}
        reasons.append("host_vitals_invalid")
    if not isinstance(vitals, dict):
        vitals = {}
        if "host_vitals_invalid" not in reasons:
            reasons.append("host_vitals_invalid")
    if observations[0]["returncode"] != 0:
        reasons.append("host_vitals_unavailable")
    processes = _parse_processes(observations[1]["stdout"])
    ncpu = int(vitals.get("ncpu", 0)) if str(vitals.get("ncpu", "")).isdigit() else 0
    try:
        load1 = float(vitals.get("load1"))
    except (TypeError, ValueError):
        load1 = None
    if vitals.get("level") != "green":
        reasons.append("host_vitals_not_green")
    if ncpu <= 0 or load1 is None:
        reasons.append("load_unknown")
    elif not math.isfinite(load1):
        reasons.append("load_unknown")
    elif load1 > ncpu * 0.5:
        reasons.append("load_above_quiet_threshold")
    if observations[1]["returncode"] != 0 or not processes:
        reasons.append("process_observation_unavailable")
    high_cpu = [p for p in processes if not math.isfinite(p["cpu_pct"]) or
                p["cpu_pct"] >= 25.0]
    if high_cpu:
        reasons.append("contending_processes_present")
    windows = [p for p in processes if p["command"].endswith("/WindowServer") or
               p["command"] == "WindowServer"]
    if not windows:
        reasons.append("ui_observation_unavailable")
    elif any(p["cpu_pct"] >= 20.0 for p in windows):
        reasons.append("ui_contention_present")
    thermal_text = observations[2]["stdout"] + observations[2]["stderr"]
    thermal_state = "nominal" if (observations[2]["returncode"] == 0 and
                                   "No thermal warning level" in thermal_text and
                                   "No performance warning level" in thermal_text) else "unknown"
    if thermal_state == "unknown":
        reasons.append("thermal_observation_unknown")
    gpu_status, gpu_busy = _parse_gpu(observations[3]["stdout"])
    if observations[3]["returncode"] != 0 or gpu_status != "passed":
        reasons.append("gpu_observation_unavailable")
    elif gpu_busy != 0:
        reasons.append("gpu_work_queues_busy")
    raw_hash = _sha256(_canonical(observations))
    sampled_at = datetime.now(timezone.utc).isoformat()
    passed = not reasons
    return {
        "schema": SCHEMA, "status": "passed" if passed else "blocked",
        "source_revision": source, "host_id": platform.node(),
        "machine_arch": platform.machine(), "os": platform.platform(),
        "sampled_at": sampled_at, "quiet_host": passed,
        "host_vitals_level": vitals.get("level", "unknown"),
        "load1": load1, "ncpu": ncpu,
        "gpu_contention": "gpu_work_queues_busy" in reasons,
        "gpu_observation_status": gpu_status, "gpu_busy_work_queues": gpu_busy,
        "ui_contention": "ui_contention_present" in reasons,
        "thermal_state": thermal_state, "reasons": reasons,
        "raw_observations_sha256": raw_hash, "observations": observations,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def main(args: argparse.Namespace) -> int:
    try:
        receipt = collect()
    except RuntimeError as exc:
        print(f"gpu_audio_p2_host_preflight: {exc}", file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"schema": SCHEMA, "status": receipt["status"],
                      "host_id": receipt["host_id"], "reasons": receipt["reasons"]}, sort_keys=True))
    return 0 if receipt["status"] == "passed" else 2


if __name__ == "__main__":
    raise SystemExit(main(parse_args(sys.argv[1:])))
