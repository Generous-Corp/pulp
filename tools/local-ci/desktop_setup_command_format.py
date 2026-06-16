"""Desktop setup and doctor command output formatting."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path


def desktop_install_lines(
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


def desktop_doctor_payload(args: argparse.Namespace, *, target: dict, checks: list[dict], all_ok: bool) -> dict:
    return {
        "target": args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "ok": all_ok,
        "checks": checks,
    }


def desktop_doctor_lines(args: argparse.Namespace, *, target: dict, checks: list[dict]) -> list[str]:
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
