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
        "workflows": (".github/workflows/pr-safe-linux.yml",),
    },
    "pr-safe": {
        "name": "pulp-pr-safe-build",
        "workflows": (".github/workflows/pr-safe-linux.yml",),
    },
    "release-control": {
        "name": "pulp-release-control",
        "workflows": (
            ".github/workflows/release-cli.yml",
            ".github/workflows/sign-and-release.yml",
        ),
    },
}

RELEASE_TAG_REF = re.compile(
    r"refs/tags/v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)


def valid_release_tag_ref(workflow_ref: str | None) -> bool:
    if workflow_ref is None:
        return False
    match = RELEASE_TAG_REF.fullmatch(workflow_ref)
    if match is None:
        return False
    prerelease = match.group(4)
    if prerelease is None:
        return True
    return all(
        not (identifier.isdigit() and len(identifier) > 1 and identifier[0] == "0")
        for identifier in prerelease.split(".")
    )


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
    workflow_ref: str | None = None,
) -> list[str]:
    expected_policy = POLICIES[policy]
    if policy == "release-control":
        if not valid_release_tag_ref(workflow_ref):
            return ["release-control policy requires an exact semantic-version tag ref"]
        selected_ref = workflow_ref
    else:
        if workflow_ref is not None:
            return [f"{policy} policy does not accept a workflow ref override"]
        selected_ref = "refs/heads/main"
    expected = [
        f"{repo}/{workflow}@{selected_ref}"
        for workflow in expected_policy["workflows"]
    ]
    failures = []
    group_name = group.get("name", "")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]+", group_name):
        failures.append("group name must be nonempty and shell-safe")
    elif group_name != expected_policy["name"]:
        failures.append(f"group name must be {expected_policy['name']}")
    if group.get("default") is not False:
        failures.append("group must not be the default runner group")
    if group.get("visibility") != "selected":
        failures.append("group visibility must be selected")
    if group.get("allows_public_repositories") is not True:
        failures.append("group must explicitly allow its selected public repository")
    if group.get("restricted_to_workflows") is not True:
        failures.append("group must be restricted to selected workflows")
    if group.get("selected_workflows") != expected:
        failures.append(f"group must select exactly the {policy} Pulp workflows")
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
    parser.add_argument("--workflow-ref")
    args = parser.parse_args(argv)
    owner, separator, _ = args.repo.partition("/")
    if not separator or not args.group_id.isdigit():
        print("--repo must be OWNER/REPO and --group-id must be numeric", file=sys.stderr)
        return 2
    base = f"orgs/{owner}/actions/runner-groups/{args.group_id}"
    try:
        group = api_json(args.gh, base)
        repositories = api_json(args.gh, base + "/repositories?per_page=100")
    except (RuntimeError, json.JSONDecodeError) as error:
        print(f"cannot verify runner group policy: {error}", file=sys.stderr)
        return 1
    failures = validate_policy(
        group, repositories, args.repo, args.policy, args.workflow_ref
    )
    if failures:
        for failure in failures:
            print("runner group policy error: " + failure, file=sys.stderr)
        return 1
    print(group.get("name", ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
