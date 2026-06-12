"""GitHub CLI/API wrappers for local CI cloud workflows."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Callable

from cloud_github_pr import (
    gh_pr_comment,
    gh_pr_create,
    gh_pr_head,
    gh_pr_list_open,
    gh_pr_merge,
)
from cloud_github_runs import (
    gh_find_dispatched_run,
    gh_run_view,
)


ROOT = Path(__file__).resolve().parents[2]


def gh_available() -> bool:
    result = subprocess.run(["gh", "auth", "status"], capture_output=True, text=True)
    return result.returncode == 0


def gh_auth_status_text() -> str:
    result = subprocess.run(["gh", "auth", "status", "-t"], capture_output=True, text=True)
    if result.returncode != 0:
        return ""
    return result.stdout


def gh_token_scopes(*, gh_auth_status_text_fn: Callable[[], str] = gh_auth_status_text) -> set[str]:
    status_text = gh_auth_status_text_fn()
    if not status_text:
        return set()
    marker = "Token scopes:"
    for raw_line in status_text.splitlines():
        line = raw_line.strip()
        if marker not in line:
            continue
        suffix = line.split(marker, 1)[1].strip()
        if suffix.startswith("'") and suffix.endswith("'"):
            suffix = suffix[1:-1]
        return {item.strip() for item in suffix.split(",") if item.strip()}
    return set()


def gh_api_json(path: str, *, fields: dict[str, str | int] | None = None) -> tuple[dict | list | None, str]:
    cmd = [
        "gh",
        "api",
        "-H",
        "Accept: application/vnd.github+json",
        "-H",
        "X-GitHub-Api-Version: 2026-03-10",
        path,
    ]
    for key, value in (fields or {}).items():
        cmd += ["-F", f"{key}={value}"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return None, detail or "gh api failed"
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None, "gh api returned invalid JSON"
    return payload, ""


def gh_repo_variables(repository: str) -> dict[str, str]:
    result = subprocess.run(
        ["gh", "variable", "list", "--repo", repository, "--json", "name,value"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return {}
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return {}
    variables: dict[str, str] = {}
    for item in payload:
        name = item.get("name")
        value = item.get("value")
        if not name or value in (None, ""):
            continue
        variables[str(name)] = str(value)
    return variables


def gh_repo_name() -> str | None:
    result = subprocess.run(
        ["gh", "repo", "view", "--json", "nameWithOwner"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout).get("nameWithOwner")
    except json.JSONDecodeError:
        return None


def gh_current_login() -> str | None:
    result = subprocess.run(
        ["gh", "api", "user", "--jq", ".login"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    login = result.stdout.strip()
    return login or None


def gh_workflow_dispatch(repository: str, workflow_file: str, ref: str, fields: dict[str, str]) -> None:
    cmd = ["gh", "workflow", "run", workflow_file, "--repo", repository, "--ref", ref]
    for key, value in fields.items():
        cmd += ["-f", f"{key}={value}"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"Failed to dispatch {workflow_file}: {detail or 'gh workflow run failed'}")
