#!/usr/bin/env python3
"""Fail closed unless an automatic Linux runner group has its exact scope."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys


POLICIES = {
    "trusted": {
        "name": "pulp-trusted-build",
        "workflows": (
            ".github/workflows/build.yml",
            ".github/workflows/pr-safe-linux.yml",
            ".github/workflows/vellum-freeze-check.yml",
            ".github/workflows/version-skill-check.yml",
        ),
    },
    "pr-safe": {
        "name": "pulp-pr-safe-build",
        "workflows": (".github/workflows/pr-safe-linux.yml",),
    },
}


def api_json(gh: str, path: str) -> dict:
    result = subprocess.run([gh, "api", path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "GitHub API request failed")
    return json.loads(result.stdout)


def validate_policy(
    group: dict,
    repositories: dict,
    repo: str,
    policy: str = "trusted",
    *,
    group_name: str | None = None,
    workflow: str | None = None,
) -> list[str]:
    if (group_name is None) != (workflow is None):
        return ["generic group name and workflow must be supplied together"]
    if group_name is None:
        expected_policy = POLICIES[policy]
        expected_name = expected_policy["name"]
        workflows = expected_policy["workflows"]
    else:
        if not re.fullmatch(r"[A-Za-z0-9_.:-]+", group_name):
            return ["expected group name must be nonempty and shell-safe"]
        if not re.fullmatch(r"\.github/workflows/[A-Za-z0-9._-]+\.ya?ml", workflow):
            return ["expected workflow must be an exact workflow path"]
        expected_name = group_name
        workflows = (workflow,)
    expected = [
        f"{repo}/{workflow}@refs/heads/main"
        for workflow in workflows
    ]
    failures = []
    live_group_name = group.get("name", "")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]+", live_group_name):
        failures.append("group name must be nonempty and shell-safe")
    elif live_group_name != expected_name:
        failures.append(f"group name must be {expected_name}")
    if group.get("default") is not False:
        failures.append("group must not be the default runner group")
    if group.get("visibility") != "selected":
        failures.append("group visibility must be selected")
    if group.get("allows_public_repositories") is not True:
        failures.append("group must explicitly allow its selected public repository")
    if group.get("restricted_to_workflows") is not True:
        failures.append("group must be restricted to selected workflows")
    if group.get("selected_workflows") != expected:
        failures.append("group must select exactly the authorized protected workflow set")
    names = [item.get("full_name") for item in repositories.get("repositories", [])]
    if repositories.get("total_count") != 1 or names != [repo]:
        failures.append("group must contain only repository " + repo)
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gh", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--group-id", required=True)
    parser.add_argument("--policy", choices=sorted(POLICIES), default="trusted")
    parser.add_argument("--group-name")
    parser.add_argument("--workflow")
    args = parser.parse_args(argv)
    owner, separator, _ = args.repo.partition("/")
    if not separator or not args.group_id.isdigit():
        print("--repo must be OWNER/REPO and --group-id must be numeric", file=sys.stderr)
        return 2
    if (args.group_name is None) != (args.workflow is None):
        print("--group-name and --workflow must be supplied together", file=sys.stderr)
        return 2
    if args.repo == "Generous-Corp/pulp" and args.group_name is not None:
        print("Pulp must use a built-in exact runner-group policy", file=sys.stderr)
        return 2
    if args.group_name is not None and (
        not re.fullmatch(r"[A-Za-z0-9_.:-]+", args.group_name)
        or not re.fullmatch(r"\.github/workflows/[A-Za-z0-9._-]+\.ya?ml", args.workflow)
    ):
        print("generic group name or workflow is invalid", file=sys.stderr)
        return 2
    base = f"orgs/{owner}/actions/runner-groups/{args.group_id}"
    try:
        group = api_json(args.gh, base)
        repositories = api_json(args.gh, base + "/repositories?per_page=100")
    except (RuntimeError, json.JSONDecodeError) as error:
        print(f"cannot verify runner group policy: {error}", file=sys.stderr)
        return 1
    failures = validate_policy(
        group,
        repositories,
        args.repo,
        args.policy,
        group_name=args.group_name,
        workflow=args.workflow,
    )
    if failures:
        for failure in failures:
            print("runner group policy error: " + failure, file=sys.stderr)
        return 1
    print(group.get("name", ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
