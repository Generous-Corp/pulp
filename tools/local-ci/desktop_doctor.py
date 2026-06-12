"""Desktop automation doctor and optional probe helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from desktop_doctor_checks import (
    macos_local_doctor_checks,
    optional_desktop_doctor_checks,
)
from desktop_doctor_optional import (
    desktop_capabilities_for,
    desktop_optional_capabilities,
    probe_webdriver_endpoint,
    webdriver_status_url,
)
import linux_target
import windows_target


def desktop_check(name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return {"name": name, "ok": ok, "detail": detail, "required": required}


def check_writable_dir(path: Path) -> tuple[bool, str]:
    probe = None
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / f".write-check-{uuid.uuid4().hex}"
        probe.write_text("ok\n")
        return True, str(path)
    except OSError as exc:
        return False, str(exc)
    finally:
        if probe is not None:
            try:
                probe.unlink(missing_ok=True)
            except OSError:
                pass


def desktop_doctor_checks(
    config: dict,
    target_name: str,
    *,
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    desktop_receipt_for_fn: Callable[[str], dict | None],
    macos_accessibility_trusted_fn: Callable[[], bool],
    ssh_reachable_fn: Callable[[str, int], bool],
    ssh_failure_detail_fn: Callable[[str, int], str],
    probe_linux_launch_backend_fn: Callable[[str], dict],
    probe_linux_remote_tooling_fn: Callable[[str], dict],
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    probe_windows_repo_checkout_fn: Callable[[str, str | None], dict],
    platform: str | None = None,
    which_fn: Callable[[str], str | None] | None = None,
    probe_webdriver_endpoint_fn: Callable[..., dict] | None = None,
) -> list[dict]:
    platform = platform or sys.platform
    which_fn = which_fn or shutil.which
    desktop_cfg = config["desktop_automation"]
    target = resolve_desktop_target_fn(config, target_name)
    contract = desktop_target_contract_fn(target_name, target)
    checks: list[dict] = []

    ok, detail = check_writable_dir(Path(desktop_cfg["artifact_root"]))
    checks.append(desktop_check("artifact_root", ok, detail))

    receipt = desktop_receipt_for_fn(target_name)
    checks.append(
        desktop_check(
            "receipt",
            receipt is not None,
            "installed" if receipt else f"not installed; run `pulp ci-local desktop install {target_name}`",
        )
    )

    adapter = target["adapter"]
    if adapter == "macos-local":
        checks.extend(
            macos_local_doctor_checks(
                platform=platform,
                which_fn=which_fn,
                macos_accessibility_trusted_fn=macos_accessibility_trusted_fn,
                desktop_check_fn=desktop_check,
            )
        )
    elif target["target_type"] == "ssh":
        host = target.get("host")
        checks.append(desktop_check("host", bool(host), host or "missing"))
        ssh_ok = False
        if host:
            ssh_ok = ssh_reachable_fn(host, 5)
            ssh_detail = host if ssh_ok else ssh_failure_detail_fn(host, 5)
            checks.append(desktop_check("ssh", ssh_ok, ssh_detail))
            if ssh_ok and adapter == "linux-xvfb":
                try:
                    backend = probe_linux_launch_backend_fn(host)
                    if backend.get("mode") == "xvfb":
                        detail = backend.get("path") or "xvfb-run"
                    elif backend.get("mode") == "display":
                        detail = f"existing display {backend.get('display') or ':0'}"
                    else:
                        detail = "missing; install xvfb and xauth (for example: sudo apt-get install xvfb xauth)"
                    checks.append(desktop_check("launch_backend", backend.get("mode") != "missing", detail))
                except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                    checks.append(desktop_check("launch_backend", False, str(exc)))
                try:
                    tooling = probe_linux_remote_tooling_fn(host)
                    for tool_name, spec in linux_target.LINUX_REQUIRED_REMOTE_TOOLS.items():
                        checks.append(
                            desktop_check(
                                spec["display_name"],
                                bool(tooling.get(f"{tool_name}_found")),
                                linux_target.linux_tooling_detail(tooling, tool_name, missing_hint=f"missing; {spec['package_hint']}"),
                            )
                        )
                    for tool_name, spec in linux_target.LINUX_OPTIONAL_REMOTE_TOOLS.items():
                        checks.append(
                            desktop_check(
                                spec["display_name"],
                                bool(tooling.get(f"{tool_name}_found")),
                                linux_target.linux_tooling_detail(tooling, tool_name, missing_hint=f"missing; {spec['package_hint']}"),
                                required=False,
                            )
                        )
                except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                    checks.append(desktop_check("remote_tooling", False, str(exc)))
            if ssh_ok and adapter == "windows-session-agent":
                bootstrap_required = bool(receipt and receipt.get("remote_bootstrap_ready"))
                checks.append(desktop_check("task_name", bool(contract.get("task_name")), contract.get("task_name") or "missing", required=False))
                try:
                    probe = probe_windows_session_agent_fn(host, contract)
                    checks.append(
                        desktop_check(
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
                    checks.append(desktop_check("interactive_user", bool(desktop_session_user), session_detail, required=False))
                    checks.append(desktop_check("agent_root", bool(probe.get("agent_root_exists")), probe.get("remote_root") or contract.get("remote_root") or "missing", required=bootstrap_required))
                    checks.append(desktop_check("jobs_dir", bool(probe.get("jobs_dir_exists")), probe.get("jobs_dir") or "missing", required=bootstrap_required))
                    checks.append(desktop_check("results_dir", bool(probe.get("results_dir_exists")), probe.get("results_dir") or "missing", required=bootstrap_required))
                    checks.append(desktop_check("script_path", bool(probe.get("script_exists")), probe.get("script_path") or contract.get("script_path") or "missing", required=bootstrap_required))
                    tooling = probe_windows_remote_tooling_fn(host)
                    checks.append(
                        desktop_check(
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
                        desktop_check(
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
                        desktop_check(
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
                        desktop_check(
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
                            desktop_check(
                                "repo_checkout",
                                repo_ready,
                                repo_detail,
                                required=bootstrap_required,
                            )
                        )
                    except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                        checks.append(desktop_check("repo_checkout", False, str(exc), required=bootstrap_required))
                except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                    checks.append(desktop_check("scheduled_task", False, str(exc), required=bootstrap_required))
        checks.append(desktop_check("bootstrap", True, target.get("bootstrap", "manual")))
    else:
        checks.append(desktop_check("adapter", adapter != "unknown", adapter))

    probe_webdriver_endpoint_fn = probe_webdriver_endpoint_fn or probe_webdriver_endpoint
    checks.extend(
        optional_desktop_doctor_checks(
            target,
            which_fn=which_fn,
            probe_webdriver_endpoint_fn=probe_webdriver_endpoint_fn,
            desktop_check_fn=desktop_check,
        )
    )

    return checks
