"""macOS desktop video recording core.

Screen-records a window two ways: a CPU `screencapture -l` frame sequence
(occlusion-safe stills) and a live AVFoundation display capture (captures GPU /
CAMetalLayer motion). Picks AVFoundation devices, builds the ffmpeg command
(Retina-crop aware), probes encoders, and computes poster visual stats.
"""

from __future__ import annotations

from collections.abc import Callable
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import threading
import time
from typing import Any
import zlib


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
    # AVFoundation screen capture produces native pixels; CGWindow bounds are
    # logical points. On a Retina display (scale 2.0) an unscaled crop grabs the
    # wrong region (top-left quadrant) — so scale the crop rect by the window's
    # screen backing scale factor reported by the probe.
    scale = float(window.get("scale") or 1.0)
    if scale <= 0:
        scale = 1.0

    def _even(value: float) -> int:
        pixels = int(round(value))
        return pixels - 1 if pixels % 2 else pixels

    crop_x = max(0, int(round(bounds["x"] * scale)))
    crop_y = max(0, int(round(bounds["y"] * scale)))
    crop_w = max(2, _even(bounds["width"] * scale))
    crop_h = max(2, _even(bounds["height"] * scale))
    video_filter = f"crop={crop_w}:{crop_h}:{crop_x}:{crop_y},fps={fps}"
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
    activate_fn: Callable[[], None] | None = None,
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
            activate_fn=activate_fn,
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
        activate_fn=activate_fn,
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
    activate_fn: Callable[[], None] | None = None,
    activate_interval_secs: float = 1.0,
) -> dict:
    frames_dir = output_path.parent / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)
    stop_event = threading.Event()
    state = {"frames": 0, "errors": [], "capture_scope": "window", "activations": 0}
    window_id = int(window["windowId"])
    bounds = macos_window_video_bounds(window)
    started_at = time.monotonic()
    interval = 1.0 / max(1.0, fps)

    def capture_loop() -> None:
        next_frame_at = time.monotonic()
        deadline = started_at + duration_secs
        # macOS pauses an occluded window's render loop (CVDisplayLink / Core
        # Animation throttle), so a background window's backing surface stops
        # updating and `screencapture -l` returns identical frames even while the
        # app is "animating". Keep the target window frontmost during capture so
        # its render loop keeps running. Re-raise on an interval because focus can
        # drift back to the terminal driving the proof. Discrete events (clicks)
        # still repaint an occluded window, which is why those were captured even
        # before this activation step existed.
        last_activate_at = float("-inf")
        while not stop_event.is_set() and time.monotonic() < deadline:
            if activate_fn is not None and (time.monotonic() - last_activate_at) >= activate_interval_secs:
                try:
                    activate_fn()
                    state["activations"] = int(state.get("activations", 0)) + 1
                except Exception as exc:  # best-effort; never abort capture on raise failure
                    state["errors"].append(f"window activate failed: {exc}")
                last_activate_at = time.monotonic()
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


def _macos_screencapture_failure_detail(errors: list[str] | tuple[str, ...] | None) -> str:
    detail = "; ".join(str(error) for error in (errors or []) if str(error)) or "no frames captured"
    lower_detail = detail.lower()
    if (
        "could not create image from display" in lower_detail
        or "could not create image from window" in lower_detail
        or "screen recording" in lower_detail
    ):
        detail += (
            "; screen capture is not available to this process. "
            "Re-run with --run-in-terminal after granting Terminal.app Screen Recording permission."
        )
    return detail


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
        detail = _macos_screencapture_failure_detail(state.get("errors"))
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


