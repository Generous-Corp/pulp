"""macOS desktop/window helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import math
import os
import struct
import json
import plistlib
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import tempfile
import threading
import time
from typing import Any
import zlib


def detect_macos_app_bundle(command: str | None) -> Path | None:
    if not command:
        return None
    args = shlex.split(command)
    if not args:
        return None
    exec_path = Path(args[0]).expanduser()
    candidates = [exec_path, *exec_path.parents]
    for candidate in candidates:
        if candidate.suffix == ".app":
            return candidate
    return None


def macos_bundle_id_for_app_path(app_path: Path) -> str | None:
    info_plist = app_path / "Contents" / "Info.plist"
    if not info_plist.exists():
        return None
    try:
        payload = plistlib.loads(info_plist.read_bytes())
    except (plistlib.InvalidFileException, OSError):
        return None
    bundle_id = payload.get("CFBundleIdentifier")
    return bundle_id if isinstance(bundle_id, str) and bundle_id else None


def macos_window_probe_path(script_dir: Path) -> Path:
    return script_dir / "macos_window_probe.swift"


def macos_window_info_for_pid(
    pid: int,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["swift", str(probe_path_fn()), "window-info", "--pid", str(pid)],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def macos_window_info_for_bundle_id(
    bundle_id: str,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["swift", str(probe_path_fn()), "window-info", "--bundle-id", bundle_id],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def macos_accessibility_trusted(
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> bool:
    result = run_fn(
        ["swift", str(probe_path_fn()), "accessibility-trusted"],
        capture_output=True,
        text=True,
        check=True,
    )
    payload = json.loads(result.stdout)
    return bool(payload.get("trusted"))


def wait_for_macos_window(
    pid: int,
    timeout_secs: float,
    *,
    macos_window_info_for_pid_fn: Callable[[int], dict],
    time_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> dict:
    deadline = time_fn() + timeout_secs
    last_error = ""
    while time_fn() < deadline:
        try:
            payload = macos_window_info_for_pid_fn(pid)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            last_error = str(exc)
            sleep_fn(0.2)
            continue
        windows = payload.get("windows", [])
        if windows:
            return windows[0]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for a visible window for pid {pid}")


def wait_for_macos_bundle_window(
    bundle_id: str,
    timeout_secs: float,
    *,
    macos_window_info_for_bundle_id_fn: Callable[[str], dict],
    activate_macos_bundle_id_fn: Callable[[str], dict],
    time_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> tuple[int, dict]:
    deadline = time_fn() + timeout_secs
    last_error = ""
    while time_fn() < deadline:
        try:
            payload = macos_window_info_for_bundle_id_fn(bundle_id)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            last_error = str(exc)
            sleep_fn(0.2)
            continue
        windows = payload.get("windows", [])
        pid = payload.get("pid")
        if windows and isinstance(pid, int):
            return pid, windows[0]
        activation_payload = activate_macos_bundle_id_fn(bundle_id)
        if activation_payload.get("stderr"):
            last_error = activation_payload["stderr"]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for a visible window for bundle id {bundle_id}")


def wait_for_macos_bundle_window_title(
    bundle_id: str,
    title_contains: str,
    timeout_secs: float,
    *,
    macos_window_info_for_bundle_id_fn: Callable[[str], dict],
    activate_macos_bundle_id_fn: Callable[[str], dict],
    time_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> tuple[int, dict]:
    deadline = time_fn() + timeout_secs
    last_error = ""
    while time_fn() < deadline:
        try:
            payload = macos_window_info_for_bundle_id_fn(bundle_id)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            last_error = str(exc)
            sleep_fn(0.2)
            continue
        windows = payload.get("windows", [])
        pid = payload.get("pid")
        for window in windows:
            title = str(window.get("title") or "")
            if title_contains in title and isinstance(pid, int):
                return pid, window
        activation_payload = activate_macos_bundle_id_fn(bundle_id)
        if activation_payload.get("stderr"):
            last_error = activation_payload["stderr"]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for Terminal window titled `{title_contains}`")


def _window_area(window: dict) -> float:
    bounds = window.get("bounds") if isinstance(window.get("bounds"), dict) else {}
    try:
        return float(bounds.get("width") or 0.0) * float(bounds.get("height") or 0.0)
    except (TypeError, ValueError):
        return 0.0


def _window_bounds(window: dict) -> tuple[float, float]:
    bounds = window.get("bounds") if isinstance(window.get("bounds"), dict) else {}
    try:
        return float(bounds.get("width") or 0.0), float(bounds.get("height") or 0.0)
    except (TypeError, ValueError):
        return 0.0, 0.0


def _likely_floating_editor_window(window: dict) -> bool:
    width, height = _window_bounds(window)
    return 320.0 <= width <= 560.0 and 240.0 <= height <= 560.0


def wait_for_macos_bundle_secondary_window(
    bundle_id: str,
    timeout_secs: float,
    *,
    macos_window_info_for_bundle_id_fn: Callable[[str], dict],
    activate_macos_bundle_id_fn: Callable[[str], dict],
    time_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> tuple[int, dict]:
    deadline = time_fn() + timeout_secs
    last_error = ""
    while time_fn() < deadline:
        try:
            payload = macos_window_info_for_bundle_id_fn(bundle_id)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            last_error = str(exc)
            sleep_fn(0.2)
            continue
        windows = payload.get("windows", [])
        pid = payload.get("pid")
        if len(windows) > 1 and isinstance(pid, int):
            primary_id = windows[0].get("windowId")
            candidates = [
                window
                for window in windows[1:]
                if window.get("windowId") != primary_id and _window_area(window) > 0
            ]
            if candidates:
                preferred = [window for window in candidates if _likely_floating_editor_window(window)]
                return pid, max(preferred or candidates, key=_window_area)
        activation_payload = activate_macos_bundle_id_fn(bundle_id)
        if activation_payload.get("stderr"):
            last_error = activation_payload["stderr"]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for a secondary window for bundle id {bundle_id}")


def terminal_proof_shell_script(
    *,
    cwd: Path,
    command_args: list[str],
    title: str,
    stdout_path: Path,
    stderr_path: Path,
    returncode_path: Path,
    keepalive_secs: float,
) -> str:
    command = " ".join(shlex.quote(part) for part in command_args)
    return "\n".join(
        [
            f"cd {shlex.quote(str(cwd))}",
            f"printf '\\033]0;%s\\007' {shlex.quote(title)}",
            f"exec > >(tee -a {shlex.quote(str(stdout_path))}) 2> >(tee -a {shlex.quote(str(stderr_path))} >&2)",
            "printf '%s\\n' 'Pulp validation video proof'",
            f"printf '%s\\n' {shlex.quote('$ ' + command)}",
            f"{command} &",
            "pulp_child_pid=$!",
            f"sleep {max(0.5, keepalive_secs):.3f}",
            "if kill -0 \"$pulp_child_pid\" >/dev/null 2>&1; then",
            "  printf '\\n%s\\n' '[pulp video proof] stopping command after capture window'",
            "  kill -INT \"$pulp_child_pid\" >/dev/null 2>&1 || true",
            "  for _pulp_wait in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do",
            "    kill -0 \"$pulp_child_pid\" >/dev/null 2>&1 || break",
            "    sleep 0.1",
            "  done",
            "  if kill -0 \"$pulp_child_pid\" >/dev/null 2>&1; then",
            "    kill -TERM \"$pulp_child_pid\" >/dev/null 2>&1 || true",
            "    sleep 0.5",
            "  fi",
            "  if kill -0 \"$pulp_child_pid\" >/dev/null 2>&1; then",
            "    kill -KILL \"$pulp_child_pid\" >/dev/null 2>&1 || true",
            "  fi",
            "fi",
            "wait \"$pulp_child_pid\"",
            "pulp_rc=$?",
            f"printf '%s\\n' \"$pulp_rc\" > {shlex.quote(str(returncode_path))}",
            "printf '\\n[pulp video proof] command exit: %s\\n' \"$pulp_rc\"",
        ]
    )


def launch_macos_terminal_proof_command(
    command_args: list[str],
    *,
    cwd: Path,
    title: str,
    stdout_path: Path,
    stderr_path: Path,
    returncode_path: Path,
    keepalive_secs: float,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    returncode_path.parent.mkdir(parents=True, exist_ok=True)
    shell_script = terminal_proof_shell_script(
        cwd=cwd,
        command_args=command_args,
        title=title,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        returncode_path=returncode_path,
        keepalive_secs=keepalive_secs,
    )
    result = run_fn(
        ["osascript", "-e", f'tell application "Terminal" to do script {json.dumps(shell_script)}'],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"osascript exited {result.returncode}"
        raise RuntimeError(f"Could not launch Terminal proof command: {detail}")
    return {
        "bundle_id": "com.apple.Terminal",
        "title": title,
        "command": command_args,
        "cwd": str(cwd),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "returncode": str(returncode_path),
        "keepalive_secs": keepalive_secs,
        "osascript_stdout": result.stdout.strip(),
        "osascript_stderr": result.stderr.strip(),
    }


def close_macos_terminal_windows_with_title(
    title_contains: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    sleep_fn: Callable[[float], None] = time.sleep,
    attempts: int = 5,
) -> dict:
    script = "\n".join(
        [
            'tell application "Terminal"',
            "    set closedCount to 0",
            "    set otherCount to 0",
            "    set otherProofCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            close w",
            "            set closedCount to closedCount + 1",
            '        else if windowName contains "Pulp Video Proof" then',
            "            set otherProofCount to otherProofCount + 1",
            "        else",
            "            set otherCount to otherCount + 1",
            "        end if",
            "    end repeat",
            "    if closedCount > 0 and otherCount = 0 then",
            "        quit",
            "    end if",
            "    return closedCount",
            "end tell",
        ]
    )
    result = subprocess.CompletedProcess(["osascript", "-e", script], 1, "", "")
    closed_count = 0
    terminated_terminal = False
    terminate_returncode: int | None = None
    for attempt in range(max(1, attempts)):
        if attempt:
            sleep_fn(0.2)
        result = run_fn(["osascript", "-e", script], capture_output=True, text=True)
        if result.returncode == 0:
            try:
                attempt_closed_count = int((result.stdout or "0").strip())
            except ValueError:
                attempt_closed_count = 0
            closed_count += attempt_closed_count
            if attempt_closed_count == 0:
                break
    if result.returncode != 0:
        state_script = "\n".join(
            [
                'tell application "System Events"',
                '    if not (exists process "Terminal") then',
                '        return "0\t0\t0"',
                "    end if",
                '    set terminalPid to unix id of process "Terminal"',
                "end tell",
                'tell application "Terminal"',
                "    set proofCount to 0",
                "    set otherProofCount to 0",
                "    set otherCount to 0",
                "    repeat with w in (every window)",
                "        set windowName to name of w",
                f"        if windowName contains {json.dumps(title_contains)} then",
                "            set proofCount to proofCount + 1",
                '        else if windowName contains "Pulp Video Proof" then',
                "            set otherProofCount to otherProofCount + 1",
                "        else",
                "            set otherCount to otherCount + 1",
                "        end if",
                "    end repeat",
                '    return (terminalPid as text) & "\t" & (proofCount as text) & "\t" & (otherCount as text)',
                "end tell",
            ]
        )
        state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
        if state_result.returncode == 0:
            state_fields = (state_result.stdout or "").strip().split("\t")
            if len(state_fields) == 3:
                try:
                    terminal_pid = int(state_fields[0])
                    proof_count = int(state_fields[1])
                    other_count = int(state_fields[2])
                except ValueError:
                    terminal_pid = 0
                    proof_count = 0
                    other_count = 0
                if terminal_pid > 0 and proof_count > 0 and other_count == 0:
                    terminate_result = run_fn(["kill", "-TERM", str(terminal_pid)], capture_output=True, text=True)
                    terminate_returncode = terminate_result.returncode
                    terminated_terminal = terminate_result.returncode == 0
    return {
        "title_contains": title_contains,
        "closed_count": closed_count,
        "terminated_terminal": terminated_terminal,
        "terminate_returncode": terminate_returncode,
        "returncode": result.returncode,
        "stdout": result.stdout.strip(),
        "stderr": result.stderr.strip(),
    }


def capture_macos_window(
    window_id: int,
    output_path: Path,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    sleep_fn: Callable[[float], None] = time.sleep,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    last_error = ""
    for attempt in range(5):
        result = run_fn(
            ["screencapture", "-x", "-l", str(window_id), str(output_path)],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0 and output_path.exists():
            return
        last_error = result.stderr.strip() or result.stdout.strip() or f"screencapture exited {result.returncode}"
        if attempt < 4:
            sleep_fn(0.2)
    fallback_result = run_fn(
        ["screencapture", "-x", str(output_path)],
        capture_output=True,
        text=True,
    )
    if fallback_result.returncode == 0 and output_path.exists():
        return
    raise RuntimeError(f"Could not capture macOS window {window_id}: {last_error}")


def probe_macos_screencapture(
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="pulp-screencapture-probe-") as tmp:
        output_path = Path(tmp) / "probe.png"
        result = run_fn(["screencapture", "-x", str(output_path)], capture_output=True, text=True)
        if result.returncode == 0 and output_path.exists():
            return True, "ok"
        detail = result.stderr.strip() or result.stdout.strip() or f"screencapture exited {result.returncode}"
        return False, detail


def macos_window_video_bounds(window: dict) -> dict[str, int]:
    bounds = window.get("bounds") if isinstance(window.get("bounds"), dict) else {}
    x = max(0, int(round(float(bounds.get("x", 0.0) or 0.0))))
    y = max(0, int(round(float(bounds.get("y", 0.0) or 0.0))))
    width = max(2, int(round(float(bounds.get("width", 0.0) or 0.0))))
    height = max(2, int(round(float(bounds.get("height", 0.0) or 0.0))))
    if width % 2:
        width -= 1
    if height % 2:
        height -= 1
    return {"x": x, "y": y, "width": width, "height": height}


def macos_window_video_command(
    window: dict,
    output_path: Path,
    *,
    duration_secs: float,
    fps: float,
    ffmpeg_path: str = "ffmpeg",
    input_device: str = "1:",
    audio_source: str = "none",
    audio_device: str | None = None,
) -> list[str]:
    bounds = macos_window_video_bounds(window)
    video_filter = f"crop={bounds['width']}:{bounds['height']}:{bounds['x']}:{bounds['y']},fps={fps}"
    input_spec = input_device
    audio_args = ["-an"]
    if audio_source == "system":
        if not audio_device:
            raise RuntimeError(
                "Video audio source `system` requires an AVFoundation audio device; "
                "pass --video-audio-device <index-or-name> or set PULP_VIDEO_AUDIO_DEVICE."
            )
        input_spec = f"{input_device}{audio_device}"
        audio_args = ["-c:a", "aac", "-b:a", "128k", "-shortest"]
    elif audio_source != "none":
        raise RuntimeError(f"Unsupported video audio source `{audio_source}`.")
    return [
        ffmpeg_path,
        "-y",
        "-f",
        "avfoundation",
        "-framerate",
        str(fps),
        "-pixel_format",
        "nv12",
        "-capture_cursor",
        "1",
        "-i",
        input_spec,
        "-t",
        str(duration_secs),
        "-vf",
        video_filter,
        *audio_args,
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "23",
        "-pix_fmt",
        "yuv420p",
        "-frames:v",
        str(max(1, int(round(duration_secs * fps)))),
        "-movflags",
        "+faststart",
        str(output_path),
    ]


def macos_avfoundation_screen_input_device(
    *,
    ffmpeg_path: str = "ffmpeg",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    try:
        result = run_fn(
            [ffmpeg_path, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(f"{ffmpeg_path} not found on PATH; install ffmpeg or disable --record-video.") from exc
    output = "\n".join([result.stderr or "", result.stdout or ""])
    for line in output.splitlines():
        match = re.search(r"\[(\d+)\]\s+Capture screen 0\b", line, flags=re.IGNORECASE)
        if match:
            return f"{match.group(1)}:"
    raise RuntimeError("Could not find AVFoundation device `Capture screen 0` in ffmpeg device list.")


def macos_avfoundation_device_listing(
    *,
    ffmpeg_path: str = "ffmpeg",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    try:
        result = run_fn(
            [ffmpeg_path, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(f"{ffmpeg_path} not found on PATH; install ffmpeg or disable --record-video.") from exc
    return "\n".join([result.stderr or "", result.stdout or ""])


def macos_avfoundation_audio_devices_from_listing(output: str) -> list[dict[str, str]]:
    devices: list[dict[str, str]] = []
    in_audio = False
    for line in output.splitlines():
        if "AVFoundation video devices" in line:
            in_audio = False
            continue
        if "AVFoundation audio devices" in line:
            in_audio = True
            continue
        if not in_audio:
            continue
        match = re.search(r"\[(\d+)\]\s+(.+?)\s*$", line)
        if match:
            devices.append({"index": match.group(1), "name": match.group(2).strip()})
    return devices


def macos_avfoundation_audio_input_device(
    explicit_device: str | None = None,
    *,
    env: dict[str, str] | None = None,
) -> str | None:
    device = explicit_device or (env or os.environ).get("PULP_VIDEO_AUDIO_DEVICE")
    if device is None:
        return None
    device = str(device).strip()
    if not device:
        return None
    if device.startswith(":"):
        device = device[1:]
    return device


def macos_avfoundation_audio_device_detail(
    explicit_device: str | None = None,
    *,
    env: dict[str, str] | None = None,
    ffmpeg_path: str = "ffmpeg",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> tuple[bool, str]:
    device = macos_avfoundation_audio_input_device(explicit_device, env=env)
    if not device:
        return (
            False,
            "No AVFoundation audio device configured; pass --video-audio-device <index-or-name> or set PULP_VIDEO_AUDIO_DEVICE.",
        )
    output = macos_avfoundation_device_listing(ffmpeg_path=ffmpeg_path, run_fn=run_fn)
    devices = macos_avfoundation_audio_devices_from_listing(output)
    for item in devices:
        if device == item["index"] or device == item["name"]:
            return True, f"{item['name']} ({item['index']})"
    available = ", ".join(f"{item['name']} ({item['index']})" for item in devices) or "none listed"
    return False, f"AVFoundation audio device `{device}` not found; available audio devices: {available}"


def ffmpeg_encoder_identity(
    ffmpeg_path: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    command = [ffmpeg_path, "-hide_banner", "-version"]
    try:
        result = run_fn(command, capture_output=True, text=True)
    except OSError as exc:
        return {"path": ffmpeg_path, "command": command, "ok": False, "error": str(exc)}
    output = "\n".join(part for part in [result.stdout, result.stderr] if part)
    first_line = output.splitlines()[0] if output.splitlines() else ""
    return {
        "path": ffmpeg_path,
        "command": command,
        "ok": result.returncode == 0,
        "returncode": result.returncode,
        "version": first_line,
    }


def png_visual_stats(path: Path) -> dict:
    """Return simple RGB stats for non-interlaced 8-bit PNGs."""
    try:
        payload = path.read_bytes()
        if not payload.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("not a png")
        offset = 8
        width = height = bit_depth = color_type = None
        compressed = bytearray()
        while offset + 8 <= len(payload):
            length = struct.unpack(">I", payload[offset : offset + 4])[0]
            chunk_type = payload[offset + 4 : offset + 8]
            chunk_data = payload[offset + 8 : offset + 8 + length]
            offset += 12 + length
            if chunk_type == b"IHDR":
                width, height, bit_depth, color_type, _compression, _filter, interlace = struct.unpack(">IIBBBBB", chunk_data)
                if interlace != 0:
                    raise ValueError("interlaced png unsupported")
            elif chunk_type == b"IDAT":
                compressed.extend(chunk_data)
            elif chunk_type == b"IEND":
                break
        if width is None or height is None or bit_depth != 8 or color_type not in {0, 2, 6}:
            raise ValueError("unsupported png format")
        channels = {0: 1, 2: 3, 6: 4}[color_type]
        stride = width * channels
        raw = zlib.decompress(bytes(compressed))
        rows: list[bytearray] = []
        pos = 0
        previous = bytearray(stride)
        for _row in range(height):
            filter_type = raw[pos]
            pos += 1
            scanline = bytearray(raw[pos : pos + stride])
            pos += stride
            for i, value in enumerate(scanline):
                left = scanline[i - channels] if i >= channels else 0
                up = previous[i]
                up_left = previous[i - channels] if i >= channels else 0
                if filter_type == 1:
                    scanline[i] = (value + left) & 0xFF
                elif filter_type == 2:
                    scanline[i] = (value + up) & 0xFF
                elif filter_type == 3:
                    scanline[i] = (value + ((left + up) // 2)) & 0xFF
                elif filter_type == 4:
                    p = left + up - up_left
                    pa = abs(p - left)
                    pb = abs(p - up)
                    pc = abs(p - up_left)
                    predictor = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
                    scanline[i] = (value + predictor) & 0xFF
                elif filter_type != 0:
                    raise ValueError(f"unsupported png filter {filter_type}")
            rows.append(scanline)
            previous = scanline

        count = width * height
        sums = [0.0, 0.0, 0.0]
        sums_sq = [0.0, 0.0, 0.0]
        for row in rows:
            for x in range(width):
                base = x * channels
                if color_type == 0:
                    rgb = (row[base], row[base], row[base])
                else:
                    rgb = (row[base], row[base + 1], row[base + 2])
                for channel, value in enumerate(rgb):
                    sums[channel] += value
                    sums_sq[channel] += value * value
        mean = [value / count for value in sums]
        stddev = [math.sqrt(max(0.0, (sums_sq[i] / count) - (mean[i] * mean[i]))) for i in range(3)]
        return {
            "ok": True,
            "width": width,
            "height": height,
            "mean": [round(value, 2) for value in mean],
            "stddev": [round(value, 2) for value in stddev],
            "appears_blank": max(mean) < 2.0 and max(stddev) < 2.0,
        }
    except (OSError, ValueError, zlib.error, struct.error, IndexError) as exc:
        return {"ok": False, "error": str(exc)}


def annotate_poster_visual_check(metadata: dict, poster_path: Path | None) -> str | None:
    if poster_path is None:
        return None
    poster = metadata.setdefault("poster", {"path": str(poster_path), "exists": poster_path.exists()})
    if not poster_path.exists():
        return None
    visual = png_visual_stats(poster_path)
    poster["visual"] = visual
    if visual.get("ok") and visual.get("appears_blank"):
        return f"Video proof recording failed: poster appears blank ({poster_path}); wake the display and retry capture."
    return None


def start_macos_window_video_recording(
    window: dict,
    output_path: Path,
    *,
    duration_secs: float,
    fps: float,
    popen_fn: Callable[..., Any] = subprocess.Popen,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ffmpeg_path: str = "ffmpeg",
    input_device_fn: Callable[..., str] = macos_avfoundation_screen_input_device,
    audio_input_device_fn: Callable[..., str | None] = macos_avfoundation_audio_input_device,
    fallback_to_frame_sequence: bool = True,
    startup_grace_secs: float = 0.25,
    prefer_frame_sequence: bool = False,
    audio_source: str = "none",
    audio_device: str | None = None,
) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if audio_source not in {"none", "system"}:
        raise RuntimeError(f"Unsupported video audio source `{audio_source}`.")
    if audio_source == "system" and prefer_frame_sequence:
        raise RuntimeError("--video-audio system requires ffmpeg/AVFoundation capture and cannot use frame-sequence fallback.")
    if prefer_frame_sequence:
        return start_macos_window_frame_sequence_recording(
            window,
            output_path,
            duration_secs=duration_secs,
            fps=fps,
            run_fn=run_fn,
            ffmpeg_path=ffmpeg_path,
            fallback_reason="window-id frame capture preferred",
        )
    try:
        input_device = input_device_fn(ffmpeg_path=ffmpeg_path, run_fn=run_fn)
        resolved_audio_device = audio_input_device_fn(audio_device) if audio_source == "system" else None
        command = macos_window_video_command(
            window,
            output_path,
            duration_secs=duration_secs,
            fps=fps,
            ffmpeg_path=ffmpeg_path,
            input_device=input_device,
            audio_source=audio_source,
            audio_device=resolved_audio_device,
        )
        proc = popen_fn(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if startup_grace_secs > 0:
            time.sleep(startup_grace_secs)
        if proc.poll() is None:
            return {
                "mode": "ffmpeg-avfoundation",
                "process": proc,
                "command": command,
                "ffmpeg_path": ffmpeg_path,
                "run_fn": run_fn,
                "path": str(output_path),
                "duration_secs": duration_secs,
                "requested_fps": fps,
                "audio_source": audio_source,
                "audio_device": resolved_audio_device,
                "bounds": macos_window_video_bounds(window),
                "window_id": int(window["windowId"]),
                "started_at": time.monotonic(),
            }
        stdout, stderr = proc.communicate()
        if not fallback_to_frame_sequence:
            detail = (stderr or stdout or f"ffmpeg exited {proc.returncode}").strip()
            raise RuntimeError(f"Video proof recording failed: {detail[-1000:]}")
        fallback_reason = (stderr or stdout or f"ffmpeg exited {proc.returncode}").strip()
    except (OSError, RuntimeError) as exc:
        if audio_source == "system":
            raise
        if not fallback_to_frame_sequence:
            raise
        fallback_reason = str(exc)

    return start_macos_window_frame_sequence_recording(
        window,
        output_path,
        duration_secs=duration_secs,
        fps=fps,
        run_fn=run_fn,
        ffmpeg_path=ffmpeg_path,
        fallback_reason=fallback_reason,
    )


def start_macos_window_frame_sequence_recording(
    window: dict,
    output_path: Path,
    *,
    duration_secs: float,
    fps: float,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ffmpeg_path: str = "ffmpeg",
    fallback_reason: str | None = None,
) -> dict:
    frames_dir = output_path.parent / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)
    stop_event = threading.Event()
    state = {"frames": 0, "errors": [], "capture_scope": "window"}
    window_id = int(window["windowId"])
    bounds = macos_window_video_bounds(window)
    started_at = time.monotonic()
    interval = 1.0 / max(1.0, fps)

    def capture_loop() -> None:
        next_frame_at = time.monotonic()
        deadline = started_at + duration_secs
        while not stop_event.is_set() and time.monotonic() < deadline:
            frame_index = int(state["frames"]) + 1
            frame_path = frames_dir / f"frame-{frame_index:06d}.png"
            last_error = ""
            captured = False
            for attempt in range(5):
                result = run_fn(
                    ["screencapture", "-x", "-l", str(window_id), str(frame_path)],
                    capture_output=True,
                    text=True,
                )
                if result.returncode == 0 and frame_path.exists():
                    captured = True
                    break
                last_error = result.stderr.strip() or result.stdout.strip() or f"screencapture exited {result.returncode}"
                if attempt < 4:
                    time.sleep(0.2)
            if not captured:
                screen_result = run_fn(
                    ["screencapture", "-x", str(frame_path)],
                    capture_output=True,
                    text=True,
                )
                if screen_result.returncode == 0 and frame_path.exists():
                    captured = True
                    state["capture_scope"] = "screen-crop"
                    state["errors"].append(f"window capture failed; using full-screen crop fallback: {last_error}")
                else:
                    last_error = screen_result.stderr.strip() or screen_result.stdout.strip() or last_error
            if captured:
                state["frames"] = frame_index
            else:
                state["errors"].append(last_error or "screencapture failed")
            next_frame_at += interval
            sleep_for = next_frame_at - time.monotonic()
            if sleep_for > 0:
                stop_event.wait(sleep_for)

    thread = threading.Thread(target=capture_loop, name="pulp-desktop-video-capture", daemon=True)
    thread.start()
    return {
        "mode": "screencapture-sequence",
        "thread": thread,
        "stop_event": stop_event,
        "state": state,
        "frames_dir": frames_dir,
        "ffmpeg_path": ffmpeg_path,
        "run_fn": run_fn,
        "path": str(output_path),
        "duration_secs": duration_secs,
        "requested_fps": fps,
        "bounds": bounds,
        "window_id": window_id,
        "started_at": started_at,
        "fallback_reason": fallback_reason,
    }


def stop_macos_window_video_recording(
    recording: dict,
    *,
    output_path: Path,
    metadata_path: Path,
    poster_path: Path | None = None,
    duration_secs: float,
    fps: float,
    attachment_budget_bytes: int,
    desktop_video_metadata_fn: Callable[..., dict],
    write_desktop_video_metadata_fn: Callable[[Path, dict], None],
    wait_timeout_secs: float = 5.0,
) -> dict:
    if recording.get("mode") == "ffmpeg-avfoundation":
        return stop_macos_window_ffmpeg_recording(
            recording,
            output_path=output_path,
            metadata_path=metadata_path,
            poster_path=poster_path,
            duration_secs=duration_secs,
            fps=fps,
            attachment_budget_bytes=attachment_budget_bytes,
            desktop_video_metadata_fn=desktop_video_metadata_fn,
            write_desktop_video_metadata_fn=write_desktop_video_metadata_fn,
            wait_timeout_secs=wait_timeout_secs,
        )

    stop_event = recording["stop_event"]
    thread = recording["thread"]
    elapsed_secs = max(0.0, time.monotonic() - float(recording.get("started_at") or time.monotonic()))
    remaining_secs = max(0.0, duration_secs - elapsed_secs)
    if remaining_secs > 0:
        thread.join(timeout=remaining_secs + wait_timeout_secs)
    if thread.is_alive():
        stop_event.set()
        thread.join(timeout=wait_timeout_secs)
    if thread.is_alive():
        raise RuntimeError("Video proof recording failed: screencapture thread did not stop.")
    state = recording["state"]
    frame_count = int(state.get("frames") or 0)
    elapsed_secs = max(0.001, time.monotonic() - float(recording["started_at"]))
    actual_fps = max(1.0, frame_count / elapsed_secs)
    if frame_count <= 0:
        detail = "; ".join(state.get("errors") or []) or "no frames captured"
        raise RuntimeError(f"Video proof recording failed: {detail}")

    frame_pattern = str(recording["frames_dir"] / "frame-%06d.png")
    command = [
        recording["ffmpeg_path"],
        "-hide_banner",
        "-y",
        "-framerate",
        f"{actual_fps:.3f}",
        "-i",
        frame_pattern,
    ]
    if state.get("capture_scope") == "screen-crop":
        bounds = recording.get("bounds") or {}
        command.extend(
            [
                "-vf",
                (
                    f"crop={bounds.get('width')}:{bounds.get('height')}:{bounds.get('x')}:{bounds.get('y')},"
                    "scale=trunc(iw/2)*2:trunc(ih/2)*2"
                ),
            ]
        )
    else:
        command.extend(["-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2"])
    command.extend(
        [
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "23",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            str(output_path),
        ]
    )
    encoder = ffmpeg_encoder_identity(recording["ffmpeg_path"], run_fn=recording["run_fn"])
    result = recording["run_fn"](command, capture_output=True, text=True)
    metadata = desktop_video_metadata_fn(
        output_path,
        duration_secs=duration_secs,
        fps=actual_fps,
        attachment_budget_bytes=attachment_budget_bytes,
        bounds=recording.get("bounds"),
        command=command,
        encoder=encoder,
    )
    metadata["mode"] = recording.get("mode")
    metadata["frame_capture_scope"] = state.get("capture_scope")
    metadata["requested_fps"] = fps
    metadata["frame_count"] = frame_count
    metadata["returncode"] = result.returncode
    if recording.get("fallback_reason"):
        metadata["fallback_reason"] = recording["fallback_reason"]
    if result.stdout:
        metadata["stdout_tail"] = result.stdout[-4000:]
    if result.stderr:
        metadata["stderr_tail"] = result.stderr[-4000:]
    if state.get("errors"):
        metadata["capture_errors"] = state["errors"][-10:]
    if poster_path is not None:
        if state.get("capture_scope") == "screen-crop" and result.returncode == 0 and output_path.exists():
            poster_path.parent.mkdir(parents=True, exist_ok=True)
            poster_result = recording["run_fn"](
                [
                    recording["ffmpeg_path"],
                    "-hide_banner",
                    "-y",
                    "-i",
                    str(output_path),
                    "-frames:v",
                    "1",
                    str(poster_path),
                ],
                capture_output=True,
                text=True,
            )
            metadata["poster_command"] = poster_result.args
            metadata["poster_returncode"] = poster_result.returncode
        else:
            first_frame = next(iter(sorted(recording["frames_dir"].glob("frame-*.png"))), None)
            if first_frame is not None:
                poster_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(first_frame, poster_path)
        metadata["poster"] = {
            "path": str(poster_path),
            "exists": poster_path.exists(),
        }
    blank_error = annotate_poster_visual_check(metadata, poster_path)
    write_desktop_video_metadata_fn(metadata_path, metadata)
    if result.returncode != 0 or not output_path.exists():
        detail = (result.stderr or result.stdout or f"ffmpeg exited {result.returncode}").strip()
        raise RuntimeError(f"Video proof recording failed: {detail[-1000:]}")
    if blank_error:
        raise RuntimeError(blank_error)
    return metadata


def stop_macos_window_ffmpeg_recording(
    recording: dict,
    *,
    output_path: Path,
    metadata_path: Path,
    poster_path: Path | None,
    duration_secs: float,
    fps: float,
    attachment_budget_bytes: int,
    desktop_video_metadata_fn: Callable[..., dict],
    write_desktop_video_metadata_fn: Callable[[Path, dict], None],
    wait_timeout_secs: float,
) -> dict:
    proc = recording["process"]
    terminated = False
    elapsed_secs = max(0.0, time.monotonic() - float(recording.get("started_at") or time.monotonic()))
    remaining_secs = max(0.0, duration_secs - elapsed_secs)
    try:
        stdout, stderr = proc.communicate(timeout=max(wait_timeout_secs, remaining_secs + 2.0))
    except subprocess.TimeoutExpired:
        if proc.poll() is None:
            proc.terminate()
            terminated = True
        try:
            stdout, stderr = proc.communicate(timeout=wait_timeout_secs)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()

    encoder = ffmpeg_encoder_identity(recording["ffmpeg_path"], run_fn=recording["run_fn"])
    metadata = desktop_video_metadata_fn(
        output_path,
        duration_secs=duration_secs,
        fps=fps,
        attachment_budget_bytes=attachment_budget_bytes,
        bounds=recording.get("bounds"),
        command=recording.get("command"),
        encoder=encoder,
        has_audio=recording.get("audio_source") == "system",
        audio_source=recording.get("audio_source") or "none",
    )
    metadata["mode"] = recording.get("mode")
    metadata["requested_fps"] = fps
    if recording.get("audio_device"):
        metadata["audio_device"] = recording["audio_device"]
    metadata["returncode"] = proc.returncode
    metadata["terminated"] = terminated
    if stdout:
        metadata["stdout_tail"] = stdout[-4000:]
    if stderr:
        metadata["stderr_tail"] = stderr[-4000:]
    if poster_path is not None and output_path.exists():
        poster_path.parent.mkdir(parents=True, exist_ok=True)
        poster_command = [
            recording["ffmpeg_path"],
            "-hide_banner",
            "-y",
            "-i",
            str(output_path),
            "-frames:v",
            "1",
            str(poster_path),
        ]
        poster_result = recording["run_fn"](poster_command, capture_output=True, text=True)
        metadata["poster"] = {
            "path": str(poster_path),
            "exists": poster_path.exists(),
            "command": poster_command,
            "returncode": poster_result.returncode,
        }
    elif poster_path is not None:
        metadata["poster"] = {"path": str(poster_path), "exists": False}

    blank_error = annotate_poster_visual_check(metadata, poster_path)
    write_desktop_video_metadata_fn(metadata_path, metadata)
    if not output_path.exists() or proc.returncode != 0:
        detail = (stderr or stdout or f"ffmpeg exited {proc.returncode}").strip()
        raise RuntimeError(f"Video proof recording failed: {detail[-1000:]}")
    if blank_error:
        raise RuntimeError(blank_error)
    return metadata


def activate_macos_pid(
    pid: int,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["swift", str(probe_path_fn()), "activate", "--pid", str(pid)],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def activate_macos_bundle_id(
    bundle_id: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    timeout_secs: float = 5.0,
) -> dict:
    cmd = ["osascript", "-e", f'tell application id "{bundle_id}" to activate']
    try:
        result = run_fn(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_secs,
        )
    except subprocess.TimeoutExpired as exc:
        stderr = (exc.stderr or "").strip() if isinstance(exc.stderr, str) else ""
        return {
            "activated": False,
            "bundle_id": bundle_id,
            "stdout": (exc.stdout or "").strip() if isinstance(exc.stdout, str) else "",
            "stderr": stderr or f"activation timed out after {timeout_secs:.1f}s",
            "returncode": None,
            "timed_out": True,
        }
    return {
        "activated": result.returncode == 0,
        "bundle_id": bundle_id,
        "stdout": result.stdout.strip(),
        "stderr": result.stderr.strip(),
        "returncode": result.returncode,
    }


def dispatch_macos_click(
    screen_x: float,
    screen_y: float,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        [
            "swift",
            str(probe_path_fn()),
            "click",
            "--x",
            str(screen_x),
            "--y",
            str(screen_y),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def terminate_process(proc: subprocess.Popen, timeout_secs: float = 5.0) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout_secs)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout_secs)


def quit_macos_bundle_id(
    bundle_id: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    timeout_secs: float = 5.0,
) -> None:
    try:
        run_fn(
            ["osascript", "-e", f'tell application id "{bundle_id}" to quit'],
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout_secs,
        )
    except subprocess.TimeoutExpired:
        return
