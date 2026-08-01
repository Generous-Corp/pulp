#!/usr/bin/env python3
"""Fail closed unless the Intel runner group has the exact trusted scope."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys


def api_json(gh, path):
    result = subprocess.run([gh, "api", path], capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or "GitHub API request failed"
        raise RuntimeError(detail)
    return json.loads(result.stdout)


def validate_policy(group, repositories, repo):
    expected_workflow = (
        repo + "/.github/workflows/nightly-intel.yml@refs/heads/main"
    )
    failures = []
    if group.get("default") is not False:
        failures.append("group must not be the default runner group")
    if group.get("visibility") != "selected":
        failures.append("group visibility must be selected")
    if group.get("allows_public_repositories") is not True:
        failures.append("group must explicitly allow its selected public repository")
    if group.get("restricted_to_workflows") is not True:
        failures.append("group must be restricted to selected workflows")
    if group.get("selected_workflows") != [expected_workflow]:
        failures.append("group must select only " + expected_workflow)
    names = [entry.get("full_name") for entry in repositories.get("repositories", [])]
    if repositories.get("total_count") != 1 or names != [repo]:
        failures.append("group must contain only repository " + repo)
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--gh", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--group-id", required=True)
    args = parser.parse_args(argv)
    owner, separator, _ = args.repo.partition("/")
    if not separator:
        print("--repo must be OWNER/REPO", file=sys.stderr)
        return 2
    base = "orgs/{}/actions/runner-groups/{}".format(owner, args.group_id)
    try:
        group = api_json(args.gh, base)
        repositories = api_json(args.gh, base + "/repositories?per_page=100")
    except (RuntimeError, json.JSONDecodeError) as error:
        print("cannot verify runner group policy: {}".format(error), file=sys.stderr)
        return 1
    failures = validate_policy(group, repositories, args.repo)
    if failures:
        for failure in failures:
            print("runner group policy error: " + failure, file=sys.stderr)
        return 1
    print("native Intel runner group policy: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
