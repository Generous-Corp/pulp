"""SSH remote desktop doctor dispatcher."""

from __future__ import annotations

from collections.abc import Callable

from desktop_doctor_remote_linux import linux_remote_doctor_checks
from desktop_doctor_remote_windows import windows_session_doctor_checks


def ssh_desktop_doctor_checks(
    *,
    target_name: str,
    target: dict,
    contract: dict,
    receipt: dict | None,
    ssh_reachable_fn: Callable[[str, int], bool],
    ssh_failure_detail_fn: Callable[[str, int], str],
    probe_linux_launch_backend_fn: Callable[[str], dict],
    probe_linux_remote_tooling_fn: Callable[[str], dict],
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    probe_windows_repo_checkout_fn: Callable[[str, str | None], dict],
    desktop_check_fn: Callable[..., dict],
) -> list[dict]:
    checks: list[dict] = []
    host = target.get("host")
    adapter = target["adapter"]
    checks.append(desktop_check_fn("host", bool(host), host or "missing"))
    ssh_ok = False
    if host:
        ssh_ok = ssh_reachable_fn(host, 5)
        ssh_detail = host if ssh_ok else ssh_failure_detail_fn(host, 5)
        checks.append(desktop_check_fn("ssh", ssh_ok, ssh_detail))
        if ssh_ok and adapter == "linux-xvfb":
            checks.extend(
                linux_remote_doctor_checks(
                    host=host,
                    probe_linux_launch_backend_fn=probe_linux_launch_backend_fn,
                    probe_linux_remote_tooling_fn=probe_linux_remote_tooling_fn,
                    desktop_check_fn=desktop_check_fn,
                )
            )
        if ssh_ok and adapter == "windows-session-agent":
            checks.extend(
                windows_session_doctor_checks(
                    target_name=target_name,
                    target=target,
                    contract=contract,
                    receipt=receipt,
                    host=host,
                    probe_windows_session_agent_fn=probe_windows_session_agent_fn,
                    probe_windows_remote_tooling_fn=probe_windows_remote_tooling_fn,
                    probe_windows_repo_checkout_fn=probe_windows_repo_checkout_fn,
                    desktop_check_fn=desktop_check_fn,
                )
            )
    checks.append(desktop_check_fn("bootstrap", True, target.get("bootstrap", "manual")))
    return checks
