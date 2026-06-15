"""Windows remote tooling ensure/install policy."""
from __future__ import annotations

from collections.abc import Callable


def ensure_windows_remote_tooling(
    host: str,
    *,
    install_optional: bool,
    required_tools: dict,
    optional_tools: dict,
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    install_windows_remote_tool_fn: Callable[..., None],
) -> dict:
    probe = probe_windows_remote_tooling_fn(host)
    installed: list[str] = []

    for tool_name, spec in required_tools.items():
        if probe.get(f"{tool_name}_found"):
            continue
        if not probe.get("winget_found"):
            raise RuntimeError(
                f"`{tool_name}` is missing on the Windows target and `winget` is unavailable; "
                "install it manually, then rerun `pulp ci-local desktop install windows`"
            )
        install_windows_remote_tool_fn(host, spec["winget_id"])
        installed.append(tool_name)
        probe = probe_windows_remote_tooling_fn(host)
        if not probe.get(f"{tool_name}_found"):
            raise RuntimeError(
                f"`{tool_name}` is still missing after `winget` install; "
                "verify PATH on the Windows target, then rerun `pulp ci-local desktop doctor windows`"
            )

    if install_optional:
        for tool_name, spec in optional_tools.items():
            if probe.get(f"{tool_name}_found") or not probe.get("winget_found"):
                continue
            try:
                install_windows_remote_tool_fn(host, spec["winget_id"])
                installed.append(tool_name)
                probe = probe_windows_remote_tooling_fn(host)
            except RuntimeError:
                # Optional tools are advisory. Keep the required setup path resilient.
                pass

    return {"probe": probe, "installed": installed}


__all__ = ["ensure_windows_remote_tooling"]
