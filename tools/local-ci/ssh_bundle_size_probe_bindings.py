"""Facade dependency bindings for SSH uploaded bundle size probes."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


SSH_BUNDLE_SIZE_PROBE_EXPORTS = ("probe_uploaded_bundle_size",)


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


def install_ssh_bundle_size_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = SSH_BUNDLE_SIZE_PROBE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
