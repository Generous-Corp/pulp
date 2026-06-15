"""Linux remote desktop doctor check builders."""

from __future__ import annotations

from collections.abc import Callable
import subprocess

import linux_target


def linux_remote_doctor_checks(
    *,
    host: str,
    probe_linux_launch_backend_fn: Callable[[str], dict],
    probe_linux_remote_tooling_fn: Callable[[str], dict],
    desktop_check_fn: Callable[..., dict],
) -> list[dict]:
    checks: list[dict] = []
    try:
        backend = probe_linux_launch_backend_fn(host)
        if backend.get("mode") == "xvfb":
            detail = backend.get("path") or "xvfb-run"
        elif backend.get("mode") == "display":
            detail = f"existing display {backend.get('display') or ':0'}"
        else:
            detail = "missing; install xvfb and xauth (for example: sudo apt-get install xvfb xauth)"
        checks.append(desktop_check_fn("launch_backend", backend.get("mode") != "missing", detail))
    except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
        checks.append(desktop_check_fn("launch_backend", False, str(exc)))
    try:
        tooling = probe_linux_remote_tooling_fn(host)
        for tool_name, spec in linux_target.LINUX_REQUIRED_REMOTE_TOOLS.items():
            checks.append(
                desktop_check_fn(
                    spec["display_name"],
                    bool(tooling.get(f"{tool_name}_found")),
                    linux_target.linux_tooling_detail(tooling, tool_name, missing_hint=f"missing; {spec['package_hint']}"),
                )
            )
        for tool_name, spec in linux_target.LINUX_OPTIONAL_REMOTE_TOOLS.items():
            checks.append(
                desktop_check_fn(
                    spec["display_name"],
                    bool(tooling.get(f"{tool_name}_found")),
                    linux_target.linux_tooling_detail(tooling, tool_name, missing_hint=f"missing; {spec['package_hint']}"),
                    required=False,
                )
            )
    except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
        checks.append(desktop_check_fn("remote_tooling", False, str(exc)))
    return checks
