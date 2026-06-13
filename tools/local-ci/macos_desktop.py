"""macOS desktop/window helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
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
) -> list[str]:
    bounds = macos_window_video_bounds(window)
    crop = f"crop={bounds['width']}:{bounds['height']}:{bounds['x']}:{bounds['y']}"
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
        input_device,
        "-t",
        str(duration_secs),
        "-vf",
        crop,
        "-an",
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
    fallback_to_frame_sequence: bool = True,
    startup_grace_secs: float = 0.25,
) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        input_device = input_device_fn(ffmpeg_path=ffmpeg_path, run_fn=run_fn)
        command = macos_window_video_command(
            window,
            output_path,
            duration_secs=duration_secs,
            fps=fps,
            ffmpeg_path=ffmpeg_path,
            input_device=input_device,
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
    state = {"frames": 0, "errors": []}
    window_id = int(window["windowId"])
    started_at = time.monotonic()
    interval = 1.0 / max(1.0, fps)

    def capture_loop() -> None:
        next_frame_at = time.monotonic()
        deadline = started_at + duration_secs
        while not stop_event.is_set() and time.monotonic() < deadline:
            frame_index = int(state["frames"]) + 1
            frame_path = frames_dir / f"frame-{frame_index:06d}.png"
            result = run_fn(
                ["screencapture", "-x", "-l", str(window_id), str(frame_path)],
                capture_output=True,
                text=True,
            )
            if result.returncode == 0 and frame_path.exists():
                state["frames"] = frame_index
            else:
                detail = result.stderr.strip() or result.stdout.strip() or f"screencapture exited {result.returncode}"
                state["errors"].append(detail)
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
        "bounds": macos_window_video_bounds(window),
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
        first_frame = next(iter(sorted(recording["frames_dir"].glob("frame-*.png"))), None)
        if first_frame is not None:
            poster_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(first_frame, poster_path)
        metadata["poster"] = {
            "path": str(poster_path),
            "exists": poster_path.exists(),
        }
    write_desktop_video_metadata_fn(metadata_path, metadata)
    if result.returncode != 0 or not output_path.exists():
        detail = (result.stderr or result.stdout or f"ffmpeg exited {result.returncode}").strip()
        raise RuntimeError(f"Video proof recording failed: {detail[-1000:]}")
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
    )
    metadata["mode"] = recording.get("mode")
    metadata["requested_fps"] = fps
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

    write_desktop_video_metadata_fn(metadata_path, metadata)
    if not output_path.exists() or (proc.returncode != 0 and not terminated):
        detail = (stderr or stdout or f"ffmpeg exited {proc.returncode}").strip()
        raise RuntimeError(f"Video proof recording failed: {detail[-1000:]}")
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
) -> dict:
    result = run_fn(
        ["osascript", "-e", f'tell application id "{bundle_id}" to activate'],
        capture_output=True,
        text=True,
    )
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
) -> None:
    run_fn(
        ["osascript", "-e", f'tell application id "{bundle_id}" to quit'],
        capture_output=True,
        text=True,
        check=False,
    )
