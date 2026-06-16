"""Windows desktop session-agent result polling helpers."""

from __future__ import annotations

from collections.abc import Callable


def wait_for_windows_session_agent_manifest(
    *,
    host: str,
    target_name: str,
    request: dict,
    timeout_secs: float,
    settle_secs: float,
    time_fn: Callable[[], float],
    sleep_fn: Callable[[float], None],
    windows_ssh_read_json_fn: Callable[..., dict | None],
) -> dict:
    deadline = time_fn() + timeout_secs + settle_secs + 15.0
    remote_manifest: dict | None = None
    while time_fn() < deadline:
        remote_manifest = windows_ssh_read_json_fn(
            host,
            request["outputs"]["manifest"],
            timeout=15,
            optional=True,
        )
        if remote_manifest is not None:
            break
        sleep_fn(0.5)
    if remote_manifest is None:
        raise RuntimeError(f"Timed out waiting for Windows desktop agent result for `{target_name}` ({request['job_id']}).")
    return remote_manifest
