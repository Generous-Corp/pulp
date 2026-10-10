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
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Callable

SCHEMA = "pulp.gpu-audio.p2.host-preflight.v1"
REPO_ROOT = Path(__file__).resolve().parents[2]
HOST_VITALS = REPO_ROOT / "tools/scripts/host_vitals.sh"
THERMAL_HELPER_RELATIVE = "tools/scripts/p2_thermal_state.swift"
THERMAL_HELPER = REPO_ROOT / THERMAL_HELPER_RELATIVE
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
THERMAL_SCHEMA = "pulp.gpu-audio.p2.thermal-observation.v1"
THERMAL_STATES = frozenset({"nominal", "fair", "serious", "critical", "unknown"})
THERMAL_MAX_AGE_SECONDS = 30.0
GPU_HEALTH_MAX_AGE_SECONDS = 30.0
GPU_HEALTH_SCHEMA = "pulp.gpu-health-result.v2"
UTC_MEASURED_RE = re.compile(
    r"^[0-9]{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])"
    r"T(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z$"
)
PINNED_TOOLS = {
    "ps": ("/bin/ps", "/usr/bin/ps"),
    "pmset": ("/usr/bin/pmset",),
    "ioreg": ("/usr/sbin/ioreg",),
    "swift": ("/usr/bin/swift",),
}


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _file_sha256(path: Path) -> str | None:
    try:
        return _sha256(path.read_bytes())
    except OSError:
        return None


def _tool_provenance(name: str) -> dict[str, Any]:
    path = shutil.which(name)
    resolved = None
    digest = None
    if path:
        try:
            resolved = str(Path(path).resolve(strict=True))
            digest = _file_sha256(Path(resolved))
        except OSError:
            resolved = None
    return {"name": name, "path": resolved, "sha256": digest}


def _command_provenance(command: list[str]) -> dict[str, Any]:
    """Hash the executable used for active GPU health evidence."""
    raw = command[0] if command else ""
    resolved = None
    digest = None
    try:
        candidate = Path(raw)
        if not candidate.is_absolute():
            found = shutil.which(raw)
            candidate = Path(found) if found else candidate
        resolved = str(candidate.resolve(strict=True))
        digest = _file_sha256(Path(resolved))
    except OSError:
        resolved = None
    return {"name": Path(raw).name, "argv0": raw, "path": resolved, "sha256": digest}


def _pinned_tool(name: str) -> str:
    """Resolve a system observation tool without consulting PATH."""
    for raw in PINNED_TOOLS.get(name, ()):
        candidate = Path(raw)
        try:
            if (candidate.is_file() and not candidate.is_symlink()
                    and candidate.stat().st_mode & 0o111):
                return str(candidate)
        except OSError:
            continue
    # Keep the intended absolute path in the receipt when unavailable; _run
    # records a non-zero result and collect() fails closed.
    return PINNED_TOOLS.get(name, (f"/pulp/missing/{name}",))[0]


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
        # Dawn/Metal can leave a child-side pipe open while tearing down the
        # device. A pipe-backed capture then waits until the admission timeout
        # even though the same doctor command completes immediately when its
        # output is file-backed. Keep this workaround local to the real GPU
        # observation so test doubles retain their simple CompletedProcess API.
        file_backed_gpu = (runner is subprocess.run and len(command) >= 3
                           and command[1:3] == ["doctor", "gpu"])
        if file_backed_gpu:
            with (tempfile.TemporaryFile() as stdout_file,
                  tempfile.TemporaryFile() as stderr_file):
                result = runner(command, stdout=stdout_file, stderr=stderr_file,
                                check=False, timeout=timeout)
                stdout_file.seek(0)
                stderr_file.seek(0)
                result.stdout = stdout_file.read().decode("utf-8", errors="replace")
                result.stderr = stderr_file.read().decode("utf-8", errors="replace")
        else:
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


def _gpu_health_command() -> list[str]:
    """Return the installed/source Pulp GPU doctor command.

    The IORegistry ``busy`` counter is not an execution proof.  Admission
    therefore uses the same bounded Dawn/WebGPU compute probe that Pulp uses
    for authenticated GPU health.  Discovery is deliberately local and
    fail-closed: a missing executable produces an unavailable observation.
    """
    # Do not silently use an installed CLI: it may be built from a different
    # checkout (the CLI itself reports this mismatch).  P2 evidence must come
    # from the exact source worktree whose revision is in the receipt.
    candidates = [
        REPO_ROOT / "build" / "pulp",
        REPO_ROOT / "build" / "tools" / "cli" / "pulp-cpp",
    ]
    for candidate in candidates:
        try:
            if candidate.is_file() and not candidate.is_symlink() and candidate.stat().st_mode & 0o111:
                return [str(candidate), "doctor", "gpu", "--json"]
        except OSError:
            continue
    return [str(REPO_ROOT / "build" / "pulp"), "doctor", "gpu", "--json"]


def _gpu_health_identity(command: list[str], source_revision: str) -> dict[str, Any]:
    """Verify a doctor executable belongs to this checkout's exact build."""
    executable = Path(command[0])
    try:
        executable = executable.resolve(strict=True)
        root = REPO_ROOT.resolve(strict=True)
        if executable.is_symlink() or root not in executable.parents:
            return {"status": "invalid", "reason": "executable_outside_checkout"}
        manifest = None
        for parent in (executable.parent, *executable.parents):
            candidate = parent / "core/runtime/generated/Release/pulp/runtime/build_info.hpp"
            if candidate.is_file() and not candidate.is_symlink():
                manifest = candidate
                break
        if manifest is None:
            return {"status": "invalid", "reason": "build_identity_missing"}
        text = manifest.read_text(encoding="utf-8")
        match = re.search(r'kGitSha\s*=\s*"([0-9a-f]{40})"', text)
        if match is None or match.group(1) != source_revision:
            return {"status": "invalid", "reason": "build_source_revision_mismatch"}
        executable_sha = _file_sha256(executable)
        manifest_sha = _file_sha256(manifest)
        if executable_sha is None or manifest_sha is None:
            return {"status": "invalid", "reason": "build_identity_unreadable"}
        return {
            "status": "valid", "path": str(executable), "sha256": executable_sha,
            "source_revision": source_revision, "manifest_path": str(manifest),
            "manifest_sha256": manifest_sha,
        }
    except (OSError, UnicodeError):
        return {"status": "invalid", "reason": "build_identity_unreadable"}


def _parse_gpu_health(raw: str, *, now: datetime | None = None) -> tuple[str, dict[str, Any]]:
    """Accept only a fresh, machine-produced authentic compute health result."""
    now = now or datetime.now(timezone.utc)
    try:
        value = json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return "unknown", {"status": "invalid", "reason": "json_invalid"}
    if not isinstance(value, dict) or value.get("schema") != GPU_HEALTH_SCHEMA:
        return "unknown", {"status": "invalid", "reason": "schema_invalid"}
    if (value.get("version") != 2
            or not isinstance(value.get("run_id"), str) or not value["run_id"].strip()
            or value.get("render_requested") is not True
            or not isinstance(value.get("recommendations"), list)
            or not isinstance(value.get("probes"), list)
            or not value["probes"]):
        return "unknown", {"status": "invalid", "reason": "top_level_shape_invalid"}
    if value.get("verdict") != "pass" or value.get("health_state") != "healthy":
        return "unknown", {"status": "invalid", "reason": "health_not_passing"}
    sampled_at = value.get("measured_at_utc")
    if not isinstance(sampled_at, str) or UTC_MEASURED_RE.fullmatch(sampled_at) is None:
        return "unknown", {"status": "invalid", "reason": "measured_at_invalid"}
    try:
        sampled = datetime.fromisoformat(sampled_at.replace("Z", "+00:00"))
        age = (now - sampled).total_seconds()
    except (TypeError, ValueError):
        return "unknown", {"status": "invalid", "reason": "measured_at_invalid"}
    if age < -1.0 or age > GPU_HEALTH_MAX_AGE_SECONDS:
        return "unknown", {"status": "invalid", "reason": "measurement_stale", "age_seconds": age}
    probes = value.get("probes")
    if not isinstance(probes, list):
        return "unknown", {"status": "invalid", "reason": "probes_invalid"}
    computes = [probe for probe in probes
                if isinstance(probe, dict)
                and probe.get("probe_id") == "gpu-compute-magnitude"
                and probe.get("required") is True]
    if len(computes) != 1:
        return "unknown", {"status": "invalid", "reason": "compute_probe_ambiguous"}
    compute = computes[0]
    if compute.get("verdict") != "pass":
        return "unknown", {"status": "invalid", "reason": "compute_probe_missing"}
    adapter = compute.get("adapter")
    measurements = compute.get("measurements")
    if (not isinstance(adapter, dict) or adapter.get("status") != "authentic"
            or adapter.get("class") != "hardware"
            or any(not isinstance(adapter.get(key), str) or not adapter[key]
                   for key in ("name", "backend", "device"))
            or not isinstance(measurements, dict)
            or measurements.get("compute_initialized") is not True
            or measurements.get("compute_oracle_passed") is not True
            or measurements.get("device_lost") is not False):
        return "unknown", {"status": "invalid", "reason": "compute_identity_or_proof_invalid"}
    observation = {
        "status": "valid", "schema": GPU_HEALTH_SCHEMA, "run_id": value.get("run_id"),
        "measured_at_utc": sampled_at, "age_seconds": age,
        "probe_id": compute.get("probe_id"), "adapter": adapter,
        "measurements": {
            key: measurements.get(key) for key in (
                "compute_initialized", "compute_oracle_passed", "device_lost")
        },
    }
    return "passed", {"status": "valid", "observation": observation}


def _parse_thermal(raw: str, *, now: datetime | None = None) -> tuple[str, dict[str, Any]]:
    """Parse and freshness-check the Foundation thermal observation.

    The helper is deliberately a separate source from pmset.  Missing,
    malformed, future-dated, stale, or unsupported values remain unknown and
    therefore block admission.
    """
    now = now or datetime.now(timezone.utc)
    try:
        value = json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return "unknown", {"status": "invalid", "reason": "json_invalid"}
    if not isinstance(value, dict) or value.get("schema") != THERMAL_SCHEMA:
        return "unknown", {"status": "invalid", "reason": "schema_invalid", "value": value}
    state = value.get("thermal_state")
    if state not in THERMAL_STATES:
        return "unknown", {"status": "invalid", "reason": "state_invalid", "value": value}
    if value.get("source") != "Foundation.ProcessInfo.thermalState":
        return "unknown", {"status": "invalid", "reason": "source_invalid", "value": value}
    sampled_at = value.get("sampled_at")
    try:
        sampled = datetime.fromisoformat(str(sampled_at).replace("Z", "+00:00"))
        age = (now - sampled).total_seconds()
    except (TypeError, ValueError):
        return "unknown", {"status": "invalid", "reason": "sampled_at_invalid", "value": value}
    if age < -1.0 or age > THERMAL_MAX_AGE_SECONDS:
        return "unknown", {"status": "invalid", "reason": "sample_stale", "age_seconds": age, "value": value}
    value = dict(value)
    value["age_seconds"] = age
    return state, {"status": "valid", "observation": value}


def collect(*, runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
            source_revision: str | None = None) -> dict[str, Any]:
    """Collect observations and return a receipt, including blocked status."""
    source = source_revision or _git_head()
    if not re.fullmatch(r"[0-9a-f]{40}", source):
        raise RuntimeError("source_revision is not an immutable commit SHA")
    observations = [
        _run([str(HOST_VITALS), "--json"], runner),
        _run([_pinned_tool("ps"), "-axo", "pid=,pcpu=,comm="], runner),
        _run([_pinned_tool("pmset"), "-g", "therm"], runner),
        _run([_pinned_tool("ioreg"), "-r", "-c", "IOAccelerator", "-l"], runner),
        _run([_pinned_tool("swift"), str(THERMAL_HELPER)], runner),
        _run(_gpu_health_command(), runner, timeout=30),
    ]
    gpu_health_command = observations[5]["argv"]
    gpu_health_source = _gpu_health_identity(gpu_health_command, source)
    observation_tools = {
        name: _command_provenance([observations[index]["argv"][0]])
        for name, index in (("host_vitals", 0), ("ps", 1), ("pmset", 2),
                            ("ioreg", 3), ("swift", 4))
    }
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
    thermal_state, thermal_observation = _parse_thermal(
        observations[4]["stdout"] if observations[4]["returncode"] == 0 else "")
    if observations[4]["returncode"] != 0 or thermal_state == "unknown":
        reasons.append("thermal_observation_unknown")
    elif thermal_state != "nominal":
        reasons.append("thermal_state_not_nominal")
    # Keep IORegistry as a diagnostic observation, but admit only from the
    # active Pulp GPU-health compute/readback proof.
    _, gpu_busy = _parse_gpu(observations[3]["stdout"])
    gpu_status, gpu_health_observation = _parse_gpu_health(
        observations[5]["stdout"] if observations[5]["returncode"] == 0 else "")
    if (observations[5]["returncode"] != 0 or gpu_status != "passed"
            or gpu_health_source.get("status") != "valid"):
        reasons.append("gpu_observation_unavailable")
    # A non-zero registry counter remains a conservative negative control.  It
    # can block, but a zero value can never establish the positive proof.
    if gpu_busy not in (None, 0):
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
        "gpu_health_observation": gpu_health_observation,
        "gpu_health_source": gpu_health_source,
        "ui_contention": "ui_contention_present" in reasons,
        "thermal_state": thermal_state, "thermal_observation": thermal_observation,
        "thermal_source": {
            "schema": THERMAL_SCHEMA,
            "helper": str(THERMAL_HELPER),
            "helper_sha256": _file_sha256(THERMAL_HELPER),
            "tool": observation_tools["swift"],
        },
        "observation_tools": observation_tools,
        "reasons": reasons,
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
