"""Remote desktop doctor check builders."""
from __future__ import annotations

from collections.abc import Callable
import subprocess

import linux_target
import windows_target


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


def windows_session_doctor_checks(
    *,
    target_name: str,
    target: dict,
    contract: dict,
    receipt: dict | None,
    host: str,
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    probe_windows_repo_checkout_fn: Callable[[str, str | None], dict],
    desktop_check_fn: Callable[..., dict],
) -> list[dict]:
    checks: list[dict] = []
    bootstrap_required = bool(receipt and receipt.get("remote_bootstrap_ready"))
    checks.append(desktop_check_fn("task_name", bool(contract.get("task_name")), contract.get("task_name") or "missing", required=False))
    try:
        probe = probe_windows_session_agent_fn(host, contract)
        checks.append(
            desktop_check_fn(
                "scheduled_task",
                bool(probe.get("task_present")),
                f"{probe.get('task_name') or contract.get('task_name')} ({probe.get('task_state') or 'missing'})",
                required=bootstrap_required,
            )
        )
        desktop_session_user = windows_target.windows_desktop_session_user(probe)
        desktop_session_state = windows_target.windows_desktop_session_state(probe)
        if desktop_session_user:
            session_detail = desktop_session_user
            if desktop_session_state:
                session_detail = f"{session_detail} ({desktop_session_state})"
        else:
            session_detail = "no logged-in desktop session detected; log into the Windows desktop, then retry"
        checks.append(desktop_check_fn("interactive_user", bool(desktop_session_user), session_detail, required=False))
        checks.append(desktop_check_fn("agent_root", bool(probe.get("agent_root_exists")), probe.get("remote_root") or contract.get("remote_root") or "missing", required=bootstrap_required))
        checks.append(desktop_check_fn("jobs_dir", bool(probe.get("jobs_dir_exists")), probe.get("jobs_dir") or "missing", required=bootstrap_required))
        checks.append(desktop_check_fn("results_dir", bool(probe.get("results_dir_exists")), probe.get("results_dir") or "missing", required=bootstrap_required))
        checks.append(desktop_check_fn("script_path", bool(probe.get("script_exists")), probe.get("script_path") or contract.get("script_path") or "missing", required=bootstrap_required))
        tooling = probe_windows_remote_tooling_fn(host)
        checks.append(
            desktop_check_fn(
                "git",
                bool(tooling.get("git_found")),
                windows_target.windows_tooling_detail(
                    tooling,
                    "git",
                    missing_hint="missing; `desktop install windows` will provision Git via winget when available",
                ),
            )
        )
        checks.append(
            desktop_check_fn(
                "winget",
                bool(tooling.get("winget_found")),
                windows_target.windows_tooling_detail(
                    tooling,
                    "winget",
                    missing_hint="missing; install App Installer/winget or install Git manually",
                ),
                required=False,
            )
        )
        checks.append(
            desktop_check_fn(
                "gh",
                bool(tooling.get("gh_found")),
                windows_target.windows_tooling_detail(
                    tooling,
                    "gh",
                    missing_hint="missing; optional for remote GitHub workflows on the Windows target",
                ),
                required=False,
            )
        )
        gh_auth_ready = tooling.get("gh_auth_ready")
        if tooling.get("gh_found"):
            auth_detail = tooling.get("gh_auth_detail") or "authenticated"
        else:
            auth_detail = "not applicable until gh is installed"
        checks.append(
            desktop_check_fn(
                "gh_auth",
                bool(gh_auth_ready) if gh_auth_ready is not None else False,
                auth_detail,
                required=False,
            )
        )
        try:
            repo_probe = probe_windows_repo_checkout_fn(host, target.get("repo_path"))
            repo_ready = windows_target.windows_repo_checkout_ready(repo_probe)
            repo_detail = windows_target.windows_repo_checkout_detail(repo_probe, fallback_path=target.get("repo_path"))
            if repo_probe.get("repo_path_unsafe"):
                repo_detail = f"{repo_detail}; unsafe repo root, run `pulp ci-local desktop install {target_name}`"
            checks.append(
                desktop_check_fn(
                    "repo_checkout",
                    repo_ready,
                    repo_detail,
                    required=bootstrap_required,
                )
            )
        except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
            checks.append(desktop_check_fn("repo_checkout", False, str(exc), required=bootstrap_required))
    except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
        checks.append(desktop_check_fn("scheduled_task", False, str(exc), required=bootstrap_required))
    return checks
