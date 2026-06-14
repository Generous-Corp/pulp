"""I/O + locking utilities for local CI.

Extracted from local_ci.py to give downstream modules a thin
filesystem-and-locking seam. All helpers depend only on stdlib plus
the path helpers in `state_paths` (for the `ensure_state_dirs()` call
that's required before writing any state file).

`image_change_summary` falls back to a SHA-256 comparison when Pillow
is unavailable, so the local CI test suite stays runnable on stripped
environments (CI bot images without PIL).

`LockBusyError` lives here rather than in `state_paths` because its
only real use site is `file_lock`. Two other call sites in local_ci.py
catch it (worktree job queue + drain enforcement) — they continue to
catch it via the re-export from `local_ci.py`.
"""

from __future__ import annotations

import fcntl
import hashlib
import os
import time
import uuid
from collections import deque
from contextlib import contextmanager
from pathlib import Path

from state_paths import ensure_state_dirs


class LockBusyError(RuntimeError):
    """Raised when a non-blocking lock cannot be acquired."""


def tail_lines(path: Path, limit: int = 80) -> list[str]:
    if not path.exists():
        return []
    with path.open("r", errors="replace") as handle:
        return list(deque(handle, maxlen=limit))


def trim_line(value: str, max_len: int = 160) -> str:
    value = value.strip()
    if len(value) <= max_len:
        return value
    return "…" + value[-(max_len - 1):]


def atomic_write_text(path: Path, text: str) -> None:
    ensure_state_dirs()
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    try:
        tmp.write_text(text)
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)


def image_change_summary(before_path: Path, after_path: Path, *, diff_output_path: Path | None = None) -> dict:
    before_bytes = before_path.read_bytes()
    after_bytes = after_path.read_bytes()
    summary = {
        "changed": hashlib.sha256(before_bytes).hexdigest() != hashlib.sha256(after_bytes).hexdigest(),
        "method": "file-hash",
    }

    try:
        from PIL import Image, ImageChops

        before = Image.open(before_path).convert("RGB")
        after = Image.open(after_path).convert("RGB")
        diff = ImageChops.difference(before, after)
        if diff_output_path is not None:
            diff_output_path.parent.mkdir(parents=True, exist_ok=True)
            diff.save(diff_output_path)
        bbox = diff.getbbox()
        summary["changed"] = bbox is not None
        summary["method"] = "pixel-bbox"
        if bbox is not None:
            summary["bbox"] = {
                "left": bbox[0],
                "top": bbox[1],
                "right": bbox[2],
                "bottom": bbox[3],
            }
    except Exception:
        pass

    return summary


def design_parity_diff_summary(
    source_path: Path,
    native_path: Path,
    *,
    diff_output_path: Path,
    resized_source_output_path: Path | None = None,
    enhance_brightness: float = 3.0,
) -> dict:
    try:
        from PIL import Image, ImageChops, ImageEnhance
    except Exception as exc:
        raise RuntimeError("design parity diff generation requires Pillow") from exc

    source = Image.open(source_path).convert("RGB")
    native = Image.open(native_path).convert("RGB")
    source_size = {"width": source.size[0], "height": source.size[1]}
    native_size = {"width": native.size[0], "height": native.size[1]}
    resized = source.size != native.size
    if resized:
        resampling = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
        source_for_diff = source.resize(native.size, resampling)
    else:
        source_for_diff = source
    if resized_source_output_path is not None:
        resized_source_output_path.parent.mkdir(parents=True, exist_ok=True)
        source_for_diff.save(resized_source_output_path)
    diff = ImageChops.difference(source_for_diff, native)
    if enhance_brightness != 1.0:
        diff = ImageEnhance.Brightness(diff).enhance(enhance_brightness)
    diff_output_path.parent.mkdir(parents=True, exist_ok=True)
    diff.save(diff_output_path)
    bbox = ImageChops.difference(source_for_diff, native).getbbox()
    summary = {
        "source_image": str(source_path),
        "native_image": str(native_path),
        "diff_image": str(diff_output_path),
        "resized_source_image": str(resized_source_output_path) if resized_source_output_path else None,
        "source_size": source_size,
        "native_size": native_size,
        "resized_source": resized,
        "method": "pillow-resized-source-diff",
        "enhance_brightness": enhance_brightness,
        "changed": bbox is not None,
    }
    if bbox is not None:
        summary["bbox"] = {
            "left": bbox[0],
            "top": bbox[1],
            "right": bbox[2],
            "bottom": bbox[3],
        }
    return summary


def wait_for_path(
    path: Path,
    timeout_secs: float,
    *,
    time_fn=time.time,
    sleep_fn=time.sleep,
) -> Path:
    deadline = time_fn() + timeout_secs
    while time_fn() < deadline:
        if path.exists():
            return path
        sleep_fn(0.1)
    raise RuntimeError(f"timed out waiting for artifact `{path}`")


@contextmanager
def file_lock(path: Path, *, blocking: bool):
    ensure_state_dirs()
    handle = path.open("a+")
    mode = fcntl.LOCK_EX
    if not blocking:
        mode |= fcntl.LOCK_NB

    try:
        fcntl.flock(handle.fileno(), mode)
    except BlockingIOError as exc:
        handle.close()
        raise LockBusyError(str(path)) from exc

    try:
        yield handle
    finally:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        handle.close()
