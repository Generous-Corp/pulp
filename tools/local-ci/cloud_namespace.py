"""Namespace CLI wrappers for local CI cloud workflows."""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parents[2]


def nsc_run(args: list[str], *, capture_output: bool = True) -> subprocess.CompletedProcess | None:
    try:
        return subprocess.run(
            ["nsc", *args],
            cwd=ROOT,
            capture_output=capture_output,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return None


def nsc_available(*, nsc_run_fn: Callable[..., subprocess.CompletedProcess | None] = nsc_run) -> bool:
    result = nsc_run_fn(["version"])
    return bool(result and result.returncode == 0)


def nsc_version(*, nsc_run_fn: Callable[..., subprocess.CompletedProcess | None] = nsc_run) -> str | None:
    result = nsc_run_fn(["version"])
    if not result or result.returncode != 0:
        return None
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return lines[0] if lines else None


def nsc_logged_in(*, nsc_run_fn: Callable[..., subprocess.CompletedProcess | None] = nsc_run) -> bool:
    result = nsc_run_fn(["auth", "check-login"])
    return bool(result and result.returncode == 0)


def parse_colon_separated_fields(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        fields[key.strip()] = value.strip()
    return fields


def nsc_workspace_info(
    *,
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None] = nsc_run,
) -> dict[str, str] | None:
    result = nsc_run_fn(["workspace", "describe"])
    if not result or result.returncode != 0:
        return None
    fields = parse_colon_separated_fields(result.stdout)
    return fields or None


def nsc_instance_history(
    max_entries: int = 100,
    *,
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None] = nsc_run,
) -> list[dict]:
    result = nsc_run_fn(["instance", "history", "--all", "-o", "json", "--max_entries", str(max_entries)])
    if not result or result.returncode != 0:
        return []
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return []
    return payload if isinstance(payload, list) else []


def namespace_instances_for_run(
    repository: str,
    run_id: int,
    *,
    nsc_instance_history_fn: Callable[[], list[dict]] = nsc_instance_history,
    normalize_namespace_instance_fn: Callable[[dict], dict],
) -> list[dict]:
    matched: list[dict] = []
    for raw_instance in nsc_instance_history_fn():
        github_workflow = raw_instance.get("github_workflow") or {}
        if github_workflow.get("repository") != repository:
            continue
        if str(github_workflow.get("run_id") or "") != str(run_id):
            continue
        matched.append(normalize_namespace_instance_fn(raw_instance))
    matched.sort(key=lambda item: (item.get("created_at", ""), item.get("cluster_id", "")))
    return matched


def print_namespace_setup_help() -> None:
    print("Recommended Namespace setup:")
    print("  1. Install the `nsc` CLI")
    print("  2. Run `nsc login`")
    print("  3. Verify with `nsc workspace describe`")
    print("  4. Configure a Namespace runner selector/profile for the workflow you want to route")


def cmd_cloud_namespace_doctor(
    _args: argparse.Namespace,
    *,
    nsc_version_fn: Callable[[], str | None] = nsc_version,
    nsc_logged_in_fn: Callable[[], bool] = nsc_logged_in,
    nsc_workspace_info_fn: Callable[[], dict[str, str] | None] = nsc_workspace_info,
    print_namespace_setup_help_fn: Callable[[], None] = print_namespace_setup_help,
) -> int:
    version = nsc_version_fn()
    if not version:
        print("Namespace CLI: missing")
        print_namespace_setup_help_fn()
        return 1

    print(f"Namespace CLI: ok ({version})")
    if not nsc_logged_in_fn():
        print("Namespace login: missing")
        print("Run: nsc login")
        return 1

    print("Namespace login: ok")
    workspace = nsc_workspace_info_fn()
    if workspace:
        name = workspace.get("Name")
        if name:
            print(f"Workspace: {name}")
        tenant = workspace.get("Tenant ID")
        if tenant:
            print(f"Tenant ID: {tenant}")
        registry = workspace.get("Registry URL")
        if registry:
            print(f"Registry URL: {registry}")
    else:
        print("Workspace: unavailable")
    return 0


def cmd_cloud_namespace_setup(
    _args: argparse.Namespace,
    *,
    nsc_available_fn: Callable[[], bool] = nsc_available,
    nsc_logged_in_fn: Callable[[], bool] = nsc_logged_in,
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None] = nsc_run,
    cmd_cloud_namespace_doctor_fn: Callable[[argparse.Namespace], int] = cmd_cloud_namespace_doctor,
    print_namespace_setup_help_fn: Callable[[], None] = print_namespace_setup_help,
) -> int:
    if not nsc_available_fn():
        print("Namespace CLI: missing")
        print_namespace_setup_help_fn()
        return 1

    if not nsc_logged_in_fn():
        print("Namespace login: starting `nsc login`...")
        login_result = nsc_run_fn(["login"], capture_output=False)
        if not login_result or login_result.returncode != 0:
            print("Namespace login: failed")
            return 1

    return cmd_cloud_namespace_doctor_fn(argparse.Namespace())
