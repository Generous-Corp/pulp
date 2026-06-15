"""GitHub CLI PR helpers for local CI cloud workflows."""
from __future__ import annotations

from collections.abc import Callable
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def gh_pr_create(branch: str, base: str = "main") -> int | None:
    result = subprocess.run(
        ["gh", "pr", "create", "--head", branch, "--base", base, "--fill", "--no-maintainer-edit"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        existing = subprocess.run(
            ["gh", "pr", "view", branch, "--json", "number"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if existing.returncode == 0:
            return json.loads(existing.stdout)["number"]
        print(f"  Failed to create PR: {result.stderr.strip()}")
        return None

    url = result.stdout.strip()
    try:
        return int(url.rstrip("/").split("/")[-1])
    except (ValueError, IndexError):
        return None


def gh_pr_comment(pr_number: int, body: str) -> bool:
    result = subprocess.run(
        ["gh", "pr", "comment", str(pr_number), "--body", body],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_merge(pr_number: int, method: str = "squash") -> bool:
    result = subprocess.run(
        ["gh", "pr", "merge", str(pr_number), f"--{method}", "--delete-branch"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_list_open() -> list[dict]:
    result = subprocess.run(
        ["gh", "pr", "list", "--json", "number,title,headRefName,author,createdAt,labels"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []
    return json.loads(result.stdout)


def gh_pr_head(
    pr_ref: str,
    *,
    gh_pr_list_open_fn: Callable[[], list[dict]] = gh_pr_list_open,
    print_fn: Callable[[str], None] = print,
) -> tuple[int, str, str] | None:
    if pr_ref == "latest":
        prs = gh_pr_list_open_fn()
        if not prs:
            print_fn("No open PRs found.")
            return None
        pr_ref = str(prs[0]["number"])

    result = subprocess.run(
        ["gh", "pr", "view", pr_ref, "--json", "number,headRefName,headRefOid"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print_fn(f"  Could not find PR {pr_ref}: {result.stderr.strip()}")
        return None

    data = json.loads(result.stdout)
    return data["number"], data["headRefName"], data["headRefOid"]
