"""Windows remote desktop doctor check builders."""

from __future__ import annotations

from collections.abc import Callable
import subprocess

import windows_target


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
