"""Facade dependency bindings for SSH bundle host and probe helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


def target_name_for_ssh_host(bindings: dict, config: dict, host: str) -> str | None:
    for name, target_cfg in config.get("targets", {}).items():
        if name == host or target_cfg.get("host") == host:
            return name
    return None


def ssh_host_uses_windows_shell(bindings: dict, config: dict, host: str) -> bool:
    target_name = _binding(bindings, "target_name_for_ssh_host")(config, host)
    if target_name:
        target_cfg = dict(config.get("targets", {}).get(target_name, {}))
        repo_path = str(target_cfg.get("repo_path") or "")
        if target_name.lower().startswith("win") or "\\" in repo_path:
            return True
    return host.lower().startswith("win")


def probe_uploaded_bundle_size(bindings: dict, host: str, remote_name: str, *, config: dict) -> int | None:
    if _binding(bindings, "ssh_host_uses_windows_shell")(config, host):
        cmd = [
            "ssh",
            "-o",
            "BatchMode=yes",
            host,
            f"cmd /V:OFF /C if exist %USERPROFILE%\\{remote_name} for %I in (%USERPROFILE%\\{remote_name}) do @echo %~zI",
        ]
    else:
        cmd = [
            "ssh",
            "-o",
            "BatchMode=yes",
            host,
            f"sh -lc 'f=\"$HOME/{remote_name}\"; if [ -f \"$f\" ]; then wc -c < \"$f\"; fi'",
        ]
    subprocess_module = _binding(bindings, "subprocess")
    timeout_expired_type = getattr(subprocess_module, "TimeoutExpired", TimeoutError)
    try:
        result = subprocess_module.run(cmd, capture_output=True, text=True, timeout=15)
    except timeout_expired_type:
        return None
    if result.returncode != 0:
        return None
    output = (result.stdout or "").strip().splitlines()
    if not output:
        return None
    value = output[-1].strip()
    try:
        return int(value)
    except ValueError:
        return None
