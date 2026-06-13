"""iOS Simulator validation video proof commands."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import time
import uuid

from video_artifacts import resolve_ffmpeg_path


DEFAULT_SIMULATOR_VIDEO_ROOT = (
    Path.home() / "Library" / "Application Support" / "Pulp" / "desktop-automation" / "runs" / "ios-simulator"
)


def _now_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")


def _print_lines(lines: list[str], *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def _json_tail(text: str, limit: int = 4000) -> str:
    return text[-limit:] if len(text) > limit else text


def _simctl_json(
    args: list[str],
    *,
    xcrun_path: str,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn([xcrun_path, "simctl", *args], capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or f"simctl exited {result.returncode}").strip()
        raise RuntimeError(detail)
    try:
        return json.loads(result.stdout or "{}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"simctl returned invalid JSON: {exc}") from exc


def booted_simulators(
    *,
    xcrun_path: str = "xcrun",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[dict]:
    payload = _simctl_json(["list", "devices", "booted", "--json"], xcrun_path=xcrun_path, run_fn=run_fn)
    devices: list[dict] = []
    for runtime, runtime_devices in payload.get("devices", {}).items():
        for device in runtime_devices or []:
            if device.get("state") == "Booted":
                item = dict(device)
                item["runtime"] = runtime
                devices.append(item)
    return devices


def simulator_video_doctor_payload(
    *,
    device: str | None = None,
    which_fn: Callable[[str], str | None] = shutil.which,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    xcrun_path = which_fn("xcrun")
    checks: list[dict] = []
    remediations: list[dict] = []
    checks.append(
        {
            "name": "xcrun",
            "ok": bool(xcrun_path),
            "detail": xcrun_path or "xcrun not found on PATH",
        }
    )
    ffmpeg_path = None
    try:
        ffmpeg_path = resolve_ffmpeg_path(which_fn=which_fn, tool_dir=Path(__file__).resolve().parent)
        checks.append({"name": "ffmpeg", "ok": True, "detail": ffmpeg_path})
    except RuntimeError as exc:
        checks.append({"name": "ffmpeg", "ok": False, "detail": str(exc)})
    booted: list[dict] = []
    if xcrun_path:
        try:
            booted = booted_simulators(xcrun_path=xcrun_path, run_fn=run_fn)
            checks.append(
                {
                    "name": "simctl_booted",
                    "ok": True,
                    "detail": f"{len(booted)} booted simulator(s)",
                }
            )
        except RuntimeError as exc:
            checks.append({"name": "simctl_booted", "ok": False, "detail": str(exc)})
    selected = _select_simulator(booted, device=device)
    checks.append(
        {
            "name": "booted_device",
            "ok": selected is not None,
            "detail": _simulator_label(selected) if selected else "no matching booted simulator",
        }
    )
    if not xcrun_path:
        remediations.append(
            {
                "check": "xcrun",
                "detail": "Install Xcode command line tools or run from an Xcode-enabled shell.",
                "command": "xcode-select --install",
            }
        )
    if not ffmpeg_path:
        remediations.append(
            {
                "check": "ffmpeg",
                "detail": "Install the local video tooling before recording simulator proof MP4s.",
                "command": "npm --prefix tools/local-ci install",
            }
        )
    if selected is None:
        remediations.append(
            {
                "check": "booted_device",
                "detail": "Boot an iOS Simulator before recording.",
                "command": "open -a Simulator",
            }
        )
    return {
        "kind": "simulator-video-doctor",
        "target": "ios-simulator",
        "device": device,
        "ok": bool(xcrun_path) and bool(ffmpeg_path) and selected is not None,
        "checks": checks,
        "booted_devices": booted,
        "remediations": remediations,
    }


def _select_simulator(devices: list[dict], *, device: str | None = None) -> dict | None:
    if not devices:
        return None
    if not device:
        return devices[0]
    for item in devices:
        if device in {str(item.get("udid")), str(item.get("name"))}:
            return item
    return None


def _simulator_label(device: dict | None) -> str:
    if not device:
        return ""
    return f"{device.get('name', 'Simulator')} ({device.get('udid', 'unknown')})"


def _bundle_id_for_app(app_path: Path) -> str | None:
    info = app_path / "Info.plist"
    if not info.exists():
        return None
    try:
        with info.open("rb") as handle:
            payload = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException):
        return None
    value = payload.get("CFBundleIdentifier")
    return str(value) if value else None


def simulator_video_run_dir(*, label: str | None = None, output: str | None = None) -> Path:
    if output:
        return Path(output).expanduser()
    safe_label = "".join(ch if ch.isalnum() or ch in ("-", "_") else "-" for ch in (label or "simulator-video")).strip("-")
    return DEFAULT_SIMULATOR_VIDEO_ROOT / f"{_now_stamp()}-{uuid.uuid4().hex[:8]}-{safe_label or 'simulator-video'}"


def _run_simctl(
    simctl_args: list[str],
    *,
    xcrun_path: str,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    return run_fn([xcrun_path, "simctl", *simctl_args], capture_output=True, text=True, timeout=timeout)


def record_ios_simulator_video(
    *,
    device_udid: str,
    output_path: Path,
    duration_secs: float,
    video_fps: float = 10.0,
    xcrun_path: str = "xcrun",
    ffmpeg_path: str = "ffmpeg",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    time_sleep_fn: Callable[[float], None] = time.sleep,
    monotonic_fn: Callable[[], float] = time.monotonic,
) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    frames_dir = output_path.parent / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)
    fps = max(1.0, float(video_fps or 10.0))
    duration = max(0.1, float(duration_secs or 0.1))
    frame_count = max(1, int(round(duration * fps)))
    interval = 1.0 / fps
    screenshot_commands: list[dict] = []
    captured = 0
    next_frame_at = monotonic_fn()
    for index in range(1, frame_count + 1):
        frame_path = frames_dir / f"frame-{index:06d}.png"
        command = [xcrun_path, "simctl", "io", device_udid, "screenshot", "--type=png", str(frame_path)]
        result = run_fn(command, capture_output=True, text=True, timeout=10)
        screenshot_commands.append(
            {
                "command": command,
                "returncode": result.returncode,
                "stdout_tail": _json_tail(result.stdout or ""),
                "stderr_tail": _json_tail(result.stderr or ""),
            }
        )
        if result.returncode != 0:
            break
        if frame_path.exists() and frame_path.stat().st_size > 0:
            captured += 1
        next_frame_at += interval
        sleep_for = next_frame_at - monotonic_fn()
        if sleep_for > 0 and index < frame_count:
            time_sleep_fn(sleep_for)
    frame_pattern = str(frames_dir / "frame-%06d.png")
    encode_command = [
        ffmpeg_path,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-framerate",
        f"{fps:g}",
        "-i",
        frame_pattern,
        "-vf",
        f"fps={fps:g},format=yuv420p",
        "-c:v",
        "libx264",
        "-movflags",
        "+faststart",
        str(output_path),
    ]
    encode = run_fn(encode_command, capture_output=True, text=True, timeout=max(30, int(duration) + 30))
    return {
        "command": encode_command,
        "returncode": encode.returncode,
        "stdout_tail": _json_tail(encode.stdout or ""),
        "stderr_tail": _json_tail(encode.stderr or ""),
        "frame_count": captured,
        "requested_frame_count": frame_count,
        "frames_dir": str(frames_dir),
        "frame_pattern": frame_pattern,
        "video_fps": fps,
        "screenshot_commands": screenshot_commands[-5:],
    }


def cmd_simulator_video_doctor(
    args: argparse.Namespace,
    *,
    print_fn: Callable[[str], None] = print,
    which_fn: Callable[[str], str | None] = shutil.which,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> int:
    payload = simulator_video_doctor_payload(
        device=getattr(args, "device", None) or None,
        which_fn=which_fn,
        run_fn=run_fn,
    )
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        lines = ["iOS Simulator video doctor:"]
        for check in payload["checks"]:
            status = "PASS" if check.get("ok") else "FAIL"
            lines.append(f"  {status} {check['name']}: {check.get('detail', '')}")
        for remediation in payload.get("remediations", []):
            lines.append(f"  remediation[{remediation['check']}]: {remediation['detail']}")
            lines.append(f"    {remediation['command']}")
        _print_lines(lines, print_fn=print_fn)
    return 0 if payload.get("ok") else 1


def cmd_simulator_video(
    args: argparse.Namespace,
    *,
    print_fn: Callable[[str], None] = print,
    which_fn: Callable[[str], str | None] = shutil.which,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    time_sleep_fn: Callable[[float], None] = time.sleep,
) -> int:
    xcrun_path = which_fn("xcrun")
    if not xcrun_path:
        print_fn("Error: xcrun not found; install Xcode command line tools before recording simulator video.")
        return 1
    try:
        devices = booted_simulators(xcrun_path=xcrun_path, run_fn=run_fn)
    except RuntimeError as exc:
        print_fn(f"Error: unable to query booted simulators: {exc}")
        return 1
    selected = _select_simulator(devices, device=getattr(args, "device", None) or None)
    if not selected:
        print_fn("Error: no matching booted iOS Simulator found. Boot one with Simulator.app before recording.")
        return 1

    run_dir = simulator_video_run_dir(label=getattr(args, "label", None), output=getattr(args, "output", None))
    video_dir = run_dir / "video"
    video_path = video_dir / "proof.mp4"
    app_path_text = getattr(args, "app", None)
    app_path = Path(app_path_text).expanduser() if app_path_text else None
    bundle_id = getattr(args, "bundle_id", None) or (_bundle_id_for_app(app_path) if app_path else None)
    commands: list[dict] = []

    if app_path:
        if not app_path.exists():
            print_fn(f"Error: simulator app path does not exist: {app_path}")
            return 1
        install = _run_simctl(["install", selected["udid"], str(app_path)], xcrun_path=xcrun_path, run_fn=run_fn, timeout=120)
        commands.append({"step": "install", "command": [xcrun_path, "simctl", "install", selected["udid"], str(app_path)], "returncode": install.returncode})
        if install.returncode != 0:
            print_fn(f"Error: simctl install failed: {(install.stderr or install.stdout).strip()}")
            return 1

    if bundle_id:
        launch = _run_simctl(["launch", selected["udid"], bundle_id], xcrun_path=xcrun_path, run_fn=run_fn, timeout=60)
        commands.append({"step": "launch", "command": [xcrun_path, "simctl", "launch", selected["udid"], bundle_id], "returncode": launch.returncode})
        if launch.returncode != 0:
            print_fn(f"Error: simctl launch failed: {(launch.stderr or launch.stdout).strip()}")
            return 1

    duration = float(getattr(args, "duration", 8.0) or 8.0)
    video_fps = max(1.0, float(getattr(args, "video_fps", 10.0) or 10.0))
    try:
        ffmpeg_path = resolve_ffmpeg_path(which_fn=which_fn, tool_dir=Path(__file__).resolve().parent)
    except RuntimeError as exc:
        print_fn(f"Error: {exc}")
        return 1
    record = record_ios_simulator_video(
        device_udid=selected["udid"],
        output_path=video_path,
        duration_secs=duration,
        video_fps=video_fps,
        xcrun_path=xcrun_path,
        ffmpeg_path=ffmpeg_path,
        run_fn=run_fn,
        time_sleep_fn=time_sleep_fn,
    )
    commands.append({"step": "record-video", **record})
    if not video_path.exists() or video_path.stat().st_size <= 0:
        print_fn(f"Error: simulator video was not written: {video_path}")
        return 1

    manifest = {
        "schema": "pulp.simulator-video-proof.v1",
        "kind": "simulator-video-proof",
        "target": "ios-simulator",
        "action": "video",
        "label": getattr(args, "label", None) or "ios-simulator-video",
        "run_status": "pass",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "simulator": {
            "name": selected.get("name"),
            "udid": selected.get("udid"),
            "runtime": selected.get("runtime"),
            "state": selected.get("state"),
        },
        "app": {"path": str(app_path) if app_path else None, "bundle_id": bundle_id},
        "video": {
            "path": str(video_path),
            "duration_secs": duration,
            "fps": video_fps,
            "size_bytes": video_path.stat().st_size,
            "recorder": "xcrun simctl io screenshot + ffmpeg",
            "template": "mobile-simulator",
        },
        "artifacts": {"video": str(video_path), "bundle_dir": str(run_dir), "manifest": str(run_dir / "manifest.json")},
        "commands": commands,
    }
    manifest_path = run_dir / "manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    if getattr(args, "json", False):
        print_fn(json.dumps({"manifest": str(manifest_path), "video": str(video_path), "run_dir": str(run_dir)}, indent=2))
    else:
        _print_lines(
            [
                "iOS Simulator video proof recorded:",
                f"  simulator: {_simulator_label(selected)}",
                f"  video: {video_path}",
                f"  manifest: {manifest_path}",
            ],
            print_fn=print_fn,
        )
    return 0


def cmd_simulator(
    args: argparse.Namespace,
    *,
    commands: dict[str, Callable[[argparse.Namespace], int]],
    print_fn: Callable[[str], None] = print,
) -> int:
    handler = commands.get(getattr(args, "simulator_command", None))
    if handler is None:
        print_fn("Error: simulator subcommand required (video-doctor, video)")
        return 1
    return handler(args)
