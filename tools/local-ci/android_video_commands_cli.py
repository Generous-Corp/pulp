"""Android emulator validation video proof commands."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from typing import Any
import uuid

from mobile_video_composition import compose_mobile_video_proof


DEFAULT_ANDROID_VIDEO_ROOT = (
    Path.home() / "Library" / "Application Support" / "Pulp" / "desktop-automation" / "runs" / "android-emulator"
)
ANDROID_PROOF_NOTES = [
    "Watch for the adb device identity, app launch or current emulator state, and the timed Android action.",
    "Android proofs use public adb actions: activity/package launch, deep links, and coordinate taps.",
]


def _now_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")


def _print_lines(lines: list[str], *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def _json_tail(text: str, limit: int = 4000) -> str:
    return text[-limit:] if len(text) > limit else text


def _candidate_adb_paths() -> list[Path]:
    candidates: list[Path] = []
    for env_name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        root = os.environ.get(env_name)
        if root:
            candidates.append(Path(root).expanduser() / "platform-tools" / "adb")
    candidates.append(Path.home() / "Library" / "Android" / "sdk" / "platform-tools" / "adb")
    candidates.append(Path.home() / "Android" / "Sdk" / "platform-tools" / "adb")
    return candidates


def resolve_adb_path(*, which_fn: Callable[[str], str | None] = shutil.which) -> str | None:
    found = which_fn("adb")
    if found:
        return found
    for candidate in _candidate_adb_paths():
        if candidate.exists():
            return str(candidate)
    return None


def parse_adb_devices(output: str) -> list[dict]:
    devices: list[dict] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("List of devices"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        serial, state = parts[0], parts[1]
        item = {"serial": serial, "state": state}
        qualifiers = dict(part.split(":", 1) for part in parts[2:] if ":" in part)
        item.update(qualifiers)
        devices.append(item)
    return devices


def connected_android_devices(
    *,
    adb_path: str = "adb",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[dict]:
    result = run_fn([adb_path, "devices", "-l"], capture_output=True, text=True, timeout=15)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or f"adb exited {result.returncode}").strip()
        raise RuntimeError(detail)
    return parse_adb_devices(result.stdout or "")


def _select_device(devices: list[dict], *, device: str | None = None) -> dict | None:
    usable = [item for item in devices if item.get("state") == "device"]
    if not usable:
        return None
    if not device:
        return usable[0]
    for item in usable:
        if device in {str(item.get("serial")), str(item.get("model")), str(item.get("device"))}:
            return item
    return None


def _device_label(device: dict | None) -> str:
    if not device:
        return ""
    model = device.get("model") or device.get("device") or "Android"
    return f"{model} ({device.get('serial', 'unknown')})"


def android_video_doctor_payload(
    *,
    device: str | None = None,
    which_fn: Callable[[str], str | None] = shutil.which,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    adb_path = resolve_adb_path(which_fn=which_fn)
    checks: list[dict] = [
        {
            "name": "adb",
            "ok": bool(adb_path),
            "detail": adb_path or "adb not found on PATH or under the Android SDK",
        }
    ]
    remediations: list[dict] = []
    devices: list[dict] = []
    selected: dict | None = None

    if adb_path:
        try:
            devices = connected_android_devices(adb_path=adb_path, run_fn=run_fn)
            checks.append(
                {
                    "name": "adb_devices",
                    "ok": True,
                    "detail": f"{len(devices)} device(s) reported by adb",
                }
            )
        except RuntimeError as exc:
            checks.append({"name": "adb_devices", "ok": False, "detail": str(exc)})
        selected = _select_device(devices, device=device)

    checks.append(
        {
            "name": "connected_device",
            "ok": selected is not None,
            "detail": _device_label(selected) if selected else "no matching adb device in state=device",
        }
    )
    if selected and adb_path:
        screenrecord = run_fn(
            [adb_path, "-s", str(selected["serial"]), "shell", "command", "-v", "screenrecord"],
            capture_output=True,
            text=True,
            timeout=15,
        )
        checks.append(
            {
                "name": "screenrecord",
                "ok": screenrecord.returncode == 0,
                "detail": (screenrecord.stdout or screenrecord.stderr or "screenrecord unavailable").strip(),
            }
        )

    if not adb_path:
        remediations.append(
            {
                "check": "adb",
                "detail": "Install Android platform-tools or set ANDROID_HOME/ANDROID_SDK_ROOT.",
                "command": "pulp doctor android",
            }
        )
    if selected is None:
        remediations.append(
            {
                "check": "connected_device",
                "detail": "Start an Android emulator or connect a device with adb enabled.",
                "command": "pulp doctor android",
            }
        )
    if any(check["name"] == "screenrecord" and not check["ok"] for check in checks):
        remediations.append(
            {
                "check": "screenrecord",
                "detail": "Use an emulator/device image that provides Android's screenrecord utility.",
                "command": "adb shell command -v screenrecord",
            }
        )
    return {
        "kind": "android-video-doctor",
        "target": "android-emulator",
        "device": device,
        "ok": all(check.get("ok") for check in checks),
        "checks": checks,
        "devices": devices,
        "selected_device": selected,
        "remediations": remediations,
    }


def android_video_run_dir(*, label: str | None = None, output: str | None = None) -> Path:
    if output:
        return Path(output).expanduser()
    safe_label = "".join(ch if ch.isalnum() or ch in ("-", "_") else "-" for ch in (label or "android-video")).strip("-")
    return DEFAULT_ANDROID_VIDEO_ROOT / f"{_now_stamp()}-{uuid.uuid4().hex[:8]}-{safe_label or 'android-video'}"


def _run_adb(
    adb_args: list[str],
    *,
    adb_path: str,
    serial: str,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    return run_fn([adb_path, "-s", serial, *adb_args], capture_output=True, text=True, timeout=timeout)


def _activity_component(package_name: str, activity: str) -> str:
    if "/" in activity:
        return activity
    if activity.startswith("."):
        return f"{package_name}/{activity}"
    return f"{package_name}/{activity}"


def _android_action_marker(*, kind: str, label: str, command: list[str], at_secs: float) -> dict:
    return {
        "kind": kind,
        "label": label,
        "at_secs": at_secs,
        "command": command,
    }


def parse_tap(value: str) -> tuple[int, int]:
    raw = (value or "").strip()
    if "," in raw:
        parts = [part.strip() for part in raw.split(",", 1)]
    else:
        parts = raw.split()
    if len(parts) != 2:
        raise ValueError("tap must be formatted as x,y")
    try:
        x = int(parts[0])
        y = int(parts[1])
    except ValueError as exc:
        raise ValueError("tap coordinates must be integers") from exc
    if x < 0 or y < 0:
        raise ValueError("tap coordinates must be non-negative")
    return x, y


def record_android_emulator_video(
    *,
    adb_path: str,
    serial: str,
    output_path: Path,
    duration_secs: float,
    action_command: list[str] | None = None,
    action_after_secs: float = 0.5,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    popen_fn: Callable[..., Any] = subprocess.Popen,
    time_sleep_fn: Callable[[float], None] = time.sleep,
    monotonic_fn: Callable[[], float] = time.monotonic,
) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    time_limit = max(1, int(round(float(duration_secs or 1.0))))
    remote_path = f"/sdcard/pulp-video-proof-{uuid.uuid4().hex[:8]}.mp4"
    command = [adb_path, "-s", serial, "shell", "screenrecord", "--time-limit", str(time_limit), remote_path]
    started = monotonic_fn()
    proc = popen_fn(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    action_result: dict | None = None
    if action_command is not None:
        delay = max(0.0, min(float(action_after_secs or 0.0), max(0.0, time_limit - 0.1)))
        if delay > 0:
            time_sleep_fn(delay)
        action = run_fn(action_command, capture_output=True, text=True, timeout=30)
        action_result = {
            "command": action_command,
            "returncode": action.returncode,
            "at_secs": max(0.0, monotonic_fn() - started),
            "stdout_tail": _json_tail(action.stdout or ""),
            "stderr_tail": _json_tail(action.stderr or ""),
        }
    stdout, stderr = proc.communicate(timeout=time_limit + 30)
    pull = _run_adb(["pull", remote_path, str(output_path)], adb_path=adb_path, serial=serial, run_fn=run_fn, timeout=60)
    cleanup = _run_adb(["shell", "rm", "-f", remote_path], adb_path=adb_path, serial=serial, run_fn=run_fn, timeout=15)
    return {
        "command": command,
        "returncode": proc.returncode,
        "stdout_tail": _json_tail(stdout or ""),
        "stderr_tail": _json_tail(stderr or ""),
        "remote_path": remote_path,
        "time_limit_secs": time_limit,
        "action": action_result,
        "pull": {
            "command": [adb_path, "-s", serial, "pull", remote_path, str(output_path)],
            "returncode": pull.returncode,
            "stdout_tail": _json_tail(pull.stdout or ""),
            "stderr_tail": _json_tail(pull.stderr or ""),
        },
        "cleanup": {
            "command": [adb_path, "-s", serial, "shell", "rm", "-f", remote_path],
            "returncode": cleanup.returncode,
        },
    }


def cmd_android_video_doctor(
    args: argparse.Namespace,
    *,
    print_fn: Callable[[str], None] = print,
    which_fn: Callable[[str], str | None] = shutil.which,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> int:
    payload = android_video_doctor_payload(
        device=getattr(args, "device", None) or None,
        which_fn=which_fn,
        run_fn=run_fn,
    )
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        lines = ["Android emulator video doctor:"]
        for check in payload["checks"]:
            status = "PASS" if check.get("ok") else "FAIL"
            lines.append(f"  {status} {check['name']}: {check.get('detail', '')}")
        for remediation in payload.get("remediations", []):
            lines.append(f"  remediation[{remediation['check']}]: {remediation['detail']}")
            lines.append(f"    {remediation['command']}")
        _print_lines(lines, print_fn=print_fn)
    return 0 if payload.get("ok") else 1


def cmd_android_video(
    args: argparse.Namespace,
    *,
    print_fn: Callable[[str], None] = print,
    which_fn: Callable[[str], str | None] = shutil.which,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    popen_fn: Callable[..., Any] = subprocess.Popen,
    time_sleep_fn: Callable[[float], None] = time.sleep,
    compose_mobile_video_proof_fn: Callable[..., dict] = compose_mobile_video_proof,
) -> int:
    adb_path = resolve_adb_path(which_fn=which_fn)
    if not adb_path:
        print_fn("Error: adb not found; run `pulp doctor android` before recording Android proof video.")
        return 1
    try:
        devices = connected_android_devices(adb_path=adb_path, run_fn=run_fn)
    except RuntimeError as exc:
        print_fn(f"Error: unable to query adb devices: {exc}")
        return 1
    selected = _select_device(devices, device=getattr(args, "device", None) or None)
    if not selected:
        print_fn("Error: no matching Android emulator/device in adb state=device.")
        return 1
    serial = str(selected["serial"])
    run_dir = android_video_run_dir(label=getattr(args, "label", None), output=getattr(args, "output", None))
    video_dir = run_dir / "video"
    video_path = video_dir / "proof.mp4"
    commands: list[dict] = []

    apk_text = getattr(args, "apk", None) or None
    apk_path = Path(apk_text).expanduser() if apk_text else None
    if apk_path:
        if not apk_path.exists():
            print_fn(f"Error: Android APK path does not exist: {apk_path}")
            return 1
        install = _run_adb(["install", "-r", str(apk_path)], adb_path=adb_path, serial=serial, run_fn=run_fn, timeout=180)
        commands.append({"step": "install", "command": [adb_path, "-s", serial, "install", "-r", str(apk_path)], "returncode": install.returncode})
        if install.returncode != 0:
            print_fn(f"Error: adb install failed: {(install.stderr or install.stdout).strip()}")
            return 1

    package_name = getattr(args, "package", None) or None
    activity = getattr(args, "activity", None) or None
    if package_name and activity:
        component = _activity_component(package_name, activity)
        launch_args = ["shell", "am", "start", "-n", component]
        launch = _run_adb(launch_args, adb_path=adb_path, serial=serial, run_fn=run_fn, timeout=60)
        commands.append({"step": "launch", "command": [adb_path, "-s", serial, *launch_args], "returncode": launch.returncode})
        if launch.returncode != 0:
            print_fn(f"Error: adb activity launch failed: {(launch.stderr or launch.stdout).strip()}")
            return 1
    elif package_name:
        launch_args = ["shell", "monkey", "-p", package_name, "1"]
        launch = _run_adb(launch_args, adb_path=adb_path, serial=serial, run_fn=run_fn, timeout=60)
        commands.append({"step": "launch", "command": [adb_path, "-s", serial, *launch_args], "returncode": launch.returncode})
        if launch.returncode != 0:
            print_fn(f"Error: adb package launch failed: {(launch.stderr or launch.stdout).strip()}")
            return 1

    duration = float(getattr(args, "duration", 8.0) or 8.0)
    action_after_secs = max(0.0, float(getattr(args, "action_after", 0.5) or 0.0))
    open_url = getattr(args, "open_url", None) or None
    tap_text = getattr(args, "tap", None) or None
    if open_url and tap_text:
        print_fn("Error: choose either --open-url or --tap for the timed Android action, not both.")
        return 1
    tap_xy: tuple[int, int] | None = None
    if tap_text:
        try:
            tap_xy = parse_tap(tap_text)
        except ValueError as exc:
            print_fn(f"Error: invalid --tap value: {exc}")
            return 1
    action_kind = "open-url" if open_url else "tap" if tap_xy else None
    action_label = getattr(args, "action_label", None) or (
        f"open-url: {open_url}" if open_url else f"tap {tap_xy[0]},{tap_xy[1]}" if tap_xy else None
    )
    action_command = None
    if open_url:
        action_command = [adb_path, "-s", serial, "shell", "am", "start", "-a", "android.intent.action.VIEW", "-d", open_url]
    elif tap_xy:
        action_command = [adb_path, "-s", serial, "shell", "input", "tap", str(tap_xy[0]), str(tap_xy[1])]
    record = record_android_emulator_video(
        adb_path=adb_path,
        serial=serial,
        output_path=video_path,
        duration_secs=duration,
        action_command=action_command,
        action_after_secs=action_after_secs,
        run_fn=run_fn,
        popen_fn=popen_fn,
        time_sleep_fn=time_sleep_fn,
    )
    commands.append({"step": "record-video", **record})
    if record.get("returncode") != 0:
        print_fn(f"Error: adb screenrecord failed: {(record.get('stderr_tail') or record.get('stdout_tail') or '').strip()}")
        return 1
    if (record.get("pull") or {}).get("returncode") != 0:
        print_fn(f"Error: adb pull failed: {((record.get('pull') or {}).get('stderr_tail') or (record.get('pull') or {}).get('stdout_tail') or '').strip()}")
        return 1
    if record.get("action"):
        commands.append({"step": "action", **record["action"]})
        if record["action"].get("returncode") != 0:
            print_fn(f"Error: Android action failed: {(record['action'].get('stderr_tail') or record['action'].get('stdout_tail') or '').strip()}")
            return 1
    if not video_path.exists() or video_path.stat().st_size <= 0:
        print_fn(f"Error: Android video was not written: {video_path}")
        return 1

    manifest = {
        "schema": "pulp.android-video-proof.v1",
        "kind": "android-video-proof",
        "target": "android-emulator",
        "action": "video",
        "label": getattr(args, "label", None) or "android-emulator-video",
        "run_status": "pass",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "android": {
            "serial": selected.get("serial"),
            "state": selected.get("state"),
            "model": selected.get("model"),
            "device": selected.get("device"),
        },
        "app": {
            "apk": str(apk_path) if apk_path else None,
            "package": package_name,
            "activity": activity,
        },
        "interaction": (
            {"mode": "open-url", "url": open_url, "label": action_label}
            if open_url
            else {"mode": "tap", "x": tap_xy[0], "y": tap_xy[1], "label": action_label}
            if tap_xy
            else None
        ),
        "android_action": (
            {
                "kind": "open-url",
                "url": open_url,
                "label": action_label,
                "after_secs": action_after_secs,
            }
            if open_url
            else {
                "kind": "tap",
                "x": tap_xy[0],
                "y": tap_xy[1],
                "label": action_label,
                "after_secs": action_after_secs,
            }
            if tap_xy
            else None
        ),
        "video": {
            "path": str(video_path),
            "duration_secs": max(1, int(round(duration))),
            "size_bytes": video_path.stat().st_size,
            "recorder": "adb shell screenrecord",
            "template": "mobile-emulator",
        },
        "artifacts": {"video": str(video_path), "bundle_dir": str(run_dir), "manifest": str(run_dir / "manifest.json")},
        "commands": commands,
    }
    if action_kind:
        manifest["video_proof_notes"] = ANDROID_PROOF_NOTES
        manifest["video_proof_composition"] = {
            "template": "mobile-emulator",
            "action_marker": _android_action_marker(
                kind=action_kind,
                label=action_label or action_kind,
                command=action_command or [],
                at_secs=float((record.get("action") or {}).get("at_secs") or action_after_secs),
            ),
            "context": (
                {"target": "android-emulator", "action": "open-url", "url": open_url}
                if open_url
                else {"target": "android-emulator", "action": "tap", "x": tap_xy[0], "y": tap_xy[1]}
            ),
            "notes": ANDROID_PROOF_NOTES,
        }
    manifest_path = run_dir / "manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    composition_payload = None
    if getattr(args, "compose_video_proof", False):
        try:
            composition_payload = compose_mobile_video_proof_fn(
                manifest_path,
                template="mobile-emulator",
                title=getattr(args, "video_title", None) or None,
                notes=getattr(args, "video_note", None) or [],
                video_attachment_budget_mb=float(getattr(args, "video_attachment_budget_mb", 100.0) or 100.0),
                small_video=bool(getattr(args, "small_video", False)),
                small_video_budget_mb=float(getattr(args, "small_video_budget_mb", 10.0) or 10.0),
                tool_dir=Path(__file__).resolve().parent,
                run_fn=run_fn,
            )
        except Exception as exc:
            print_fn(f"Error: Android video composition failed: {exc}")
            return 1
    if getattr(args, "json", False):
        payload = {"manifest": str(manifest_path), "video": str(video_path), "run_dir": str(run_dir)}
        if composition_payload is not None:
            payload["composition"] = composition_payload
        print_fn(json.dumps(payload, indent=2))
    else:
        _print_lines(
            [
                "Android emulator video proof recorded:",
                f"  device: {_device_label(selected)}",
                f"  video: {video_path}",
                f"  manifest: {manifest_path}",
            ],
            print_fn=print_fn,
        )
        if composition_payload is not None:
            print_fn(f"  video_composed: {composition_payload['artifacts'].get('video_composed')}")
            print_fn(f"  video_issue: {composition_payload['artifacts'].get('video_issue')}")
    return 0
