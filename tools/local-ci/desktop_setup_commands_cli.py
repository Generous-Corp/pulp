"""Desktop automation setup and health command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
from pathlib import Path
import subprocess

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_target_command_context,
    print_desktop_command_lines,
)


def _desktop_install_lines(
    *,
    target_name: str,
    target: dict,
    artifact_root: Path,
    remote_bootstrap_ready: bool,
    remote_tooling_ready: bool,
    tooling_installed: list[str],
    tooling_probe: dict | None,
    repo_checkout_probe: dict | None,
    contract: dict,
    windows_tooling_detail_fn: Callable[..., str],
) -> list[str]:
    lines = [
        f"Desktop target `{target_name}` prepared.",
        f"  adapter: {target['adapter']}",
        f"  bootstrap: {target['bootstrap']}",
        f"  artifact_root: {artifact_root}",
    ]
    if target["target_type"] != "ssh":
        return lines + ["  remote bootstrap: not required for local target"]

    if remote_bootstrap_ready:
        lines.append("  remote bootstrap: ready")
    else:
        lines.append("  remote bootstrap: pending; target profile recorded locally")
    if target["adapter"] == "windows-session-agent":
        if remote_tooling_ready:
            git_detail = windows_tooling_detail_fn(tooling_probe or {}, "git") if tooling_probe else "git ready"
            lines.append(f"  remote tooling: ready ({git_detail})")
        else:
            lines.append("  remote tooling: pending; run `pulp ci-local desktop doctor windows` for remediation")
        if tooling_installed:
            lines.append(f"  remote tooling installed: {', '.join(tooling_installed)}")
        if repo_checkout_probe and repo_checkout_probe.get("repo_path"):
            lines.append(f"  remote repo checkout: {repo_checkout_probe['repo_path']}")
    if contract.get("task_name"):
        lines.append(f"  task_name: {contract['task_name']}")
    if contract.get("remote_root"):
        lines.append(f"  remote_root: {contract['remote_root']}")
    return lines


def _desktop_doctor_payload(args: argparse.Namespace, *, target: dict, checks: list[dict], all_ok: bool) -> dict:
    return {
        "target": args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "ok": all_ok,
        "checks": checks,
    }


def _desktop_doctor_lines(args: argparse.Namespace, *, target: dict, checks: list[dict]) -> list[str]:
    lines = [
        f"Desktop doctor for `{args.target}`",
        f"  adapter: {target['adapter']}",
        f"  bootstrap: {target['bootstrap']}",
    ]
    for check in checks:
        if check["ok"]:
            status = "PASS"
        elif not check.get("required", True):
            status = "WARN"
        else:
            status = "FAIL"
        lines.append(f"  {status:4s}  {check['name']}: {check['detail']}")
    return lines


def cmd_desktop_install(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    check_writable_dir_fn: Callable[[Path], tuple[bool, str]],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    ensure_host_reachable_fn: Callable[[str, dict, dict], str | None],
    bootstrap_windows_session_agent_fn: Callable[[str, dict], dict],
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    subprocess_run_fn: Callable[..., subprocess.CompletedProcess],
    root_path: Path,
    new_install_job_id_fn: Callable[[], str],
    sync_job_bundle_to_ssh_host_fn: Callable[[str, dict], tuple[str, str]],
    ensure_windows_remote_tooling_fn: Callable[[str], dict],
    windows_remote_tooling_ready_fn: Callable[[dict], bool],
    ensure_windows_remote_repo_checkout_fn: Callable[..., dict],
    git_origin_clone_url_fn: Callable[[Path], str],
    windows_repo_checkout_ready_fn: Callable[[dict], bool],
    update_target_repo_path_fn: Callable[[dict, str, str], None],
    save_config_fn: Callable[[dict], None],
    now_iso_fn: Callable[[], str],
    desktop_target_receipt_path_fn: Callable[[str], Path],
    atomic_write_text_fn: Callable[[Path, str], None],
    windows_tooling_detail_fn: Callable[..., str],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, target, status = load_desktop_target_command_context(
        args.target,
        load_config_fn=load_config_fn,
        resolve_desktop_target_fn=resolve_desktop_target_fn,
        print_fn=print_fn,
    )
    if status is not None:
        return status

    artifact_root = Path(config["desktop_automation"]["artifact_root"])
    ok, detail = check_writable_dir_fn(artifact_root)
    if not ok:
        print_fn(f"Error: desktop artifact root is not writable: {detail}")
        return 1

    contract = desktop_target_contract_fn(args.target, target)
    remote_bootstrap_ready = target["target_type"] != "ssh"
    remote_tooling_ready = target["target_type"] != "ssh"
    remote_repo_checkout_ready = target["target_type"] != "ssh"
    tooling_probe = None
    tooling_installed: list[str] = []
    repo_checkout_probe = None
    if target["target_type"] == "ssh" and target["adapter"] == "windows-session-agent":
        host = ensure_host_reachable_fn(args.target, target, config.get("defaults", {}))
        if host:
            try:
                bootstrap_result = bootstrap_windows_session_agent_fn(host, contract)
                probe = probe_windows_session_agent_fn(host, contract)
                remote_bootstrap_ready = bool(
                    probe.get("task_present")
                    and probe.get("agent_root_exists")
                    and probe.get("jobs_dir_exists")
                    and probe.get("results_dir_exists")
                    and probe.get("script_exists")
                )
                contract = {
                    **contract,
                    "remote_root": bootstrap_result.get("remote_root", contract.get("remote_root")),
                    "script_path": bootstrap_result.get("script_path", contract.get("script_path")),
                }
                install_bundle_sha = subprocess_run_fn(
                    ["git", "rev-parse", "HEAD"],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                    check=True,
                ).stdout.strip()
                install_bundle_job = {"id": new_install_job_id_fn(), "sha": install_bundle_sha}
                install_bundle_name, install_bundle_ref = sync_job_bundle_to_ssh_host_fn(host, install_bundle_job)
                tooling_result = ensure_windows_remote_tooling_fn(host)
                tooling_probe = tooling_result["probe"]
                tooling_installed = tooling_result["installed"]
                remote_tooling_ready = windows_remote_tooling_ready_fn(tooling_probe)
                repo_checkout_probe = ensure_windows_remote_repo_checkout_fn(
                    host,
                    target.get("repo_path"),
                    remote_url=git_origin_clone_url_fn(root_path),
                    bundle_name=install_bundle_name,
                    bundle_ref=install_bundle_ref,
                )
                remote_repo_checkout_ready = windows_repo_checkout_ready_fn(repo_checkout_probe)
                effective_repo_path = repo_checkout_probe.get("repo_path")
                if effective_repo_path and effective_repo_path != target.get("repo_path"):
                    update_target_repo_path_fn(config, args.target, effective_repo_path)
                    save_config_fn(config)
                    target = resolve_desktop_target_fn(config, args.target)
            except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                remote_bootstrap_ready = False
                remote_tooling_ready = False
                remote_repo_checkout_ready = False
                print_fn(f"Warning: remote bootstrap did not complete for `{args.target}`: {exc}")
        else:
            remote_bootstrap_ready = False
            remote_tooling_ready = False
            remote_repo_checkout_ready = False

    receipt = {
        "target": args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "target_type": target["target_type"],
        "host": target.get("host"),
        "repo_path": target.get("repo_path"),
        "artifact_root": str(artifact_root),
        "capability_tier": target.get("capability_tier", "v1"),
        "installed_at": now_iso_fn(),
        "remote_bootstrap_ready": remote_bootstrap_ready,
        "remote_tooling_ready": remote_tooling_ready,
        "remote_repo_checkout_ready": remote_repo_checkout_ready,
        "tooling_probe": tooling_probe,
        "repo_checkout_probe": repo_checkout_probe,
        "contract": contract,
    }
    atomic_write_text_fn(
        desktop_target_receipt_path_fn(args.target),
        json.dumps(receipt, indent=2) + "\n",
    )

    print_desktop_command_lines(
        _desktop_install_lines(
            target_name=args.target,
            target=target,
            artifact_root=artifact_root,
            remote_bootstrap_ready=remote_bootstrap_ready,
            remote_tooling_ready=remote_tooling_ready,
            tooling_installed=tooling_installed,
            tooling_probe=tooling_probe,
            repo_checkout_probe=repo_checkout_probe,
            contract=contract,
            windows_tooling_detail_fn=windows_tooling_detail_fn,
        ),
        print_fn=print_fn,
    )
    return 0


def cmd_desktop_doctor(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, target, status = load_desktop_target_command_context(
        args.target,
        load_config_fn=load_config_fn,
        resolve_desktop_target_fn=resolve_desktop_target_fn,
        print_fn=print_fn,
    )
    if status is not None:
        return status

    checks = desktop_doctor_checks_fn(config, args.target)
    all_ok = True
    for check in checks:
        if check.get("required", True):
            all_ok = all_ok and check["ok"]
    emit_desktop_command_result(
        payload=_desktop_doctor_payload(args, target=target, checks=checks, all_ok=all_ok),
        json_output=getattr(args, "json", False),
        text_lines=_desktop_doctor_lines(args, target=target, checks=checks),
        print_fn=print_fn,
    )
    return 0 if all_ok else 1
