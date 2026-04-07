#!/usr/bin/env python3
"""Check package registry freshness against upstream GitHub repos.

Tier 1 checks (metadata only, no builds):
- Latest tag/release vs pinned version
- Last commit date on default branch
- Archived status
- License file hash (detect license changes)

Exits non-zero if any package has issues.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "tools" / "packages" / "registry.json"


@dataclass
class CheckResult:
    package: str
    pinned_version: str
    latest_version: str | None = None
    last_commit_date: str | None = None
    archived: bool = False
    license_changed: bool = False
    issues: list[str] | None = None

    def __post_init__(self):
        if self.issues is None:
            self.issues = []


def run_gh(args: list[str]) -> dict | list | None:
    """Run a gh api command and return parsed JSON."""
    try:
        result = subprocess.run(
            ["gh", "api", *args],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode != 0:
            return None
        return json.loads(result.stdout)
    except (subprocess.TimeoutExpired, json.JSONDecodeError):
        return None


def extract_owner_repo(url: str) -> tuple[str, str] | None:
    """Extract owner/repo from a GitHub URL."""
    url = url.rstrip("/").removesuffix(".git")
    parts = url.split("/")
    if "github.com" in parts:
        idx = parts.index("github.com")
        if idx + 2 < len(parts):
            return parts[idx + 1], parts[idx + 2]
    return None


def check_package(slug: str, pkg: dict) -> CheckResult:
    result = CheckResult(package=slug, pinned_version=pkg["version"])

    git_url = pkg.get("fetch", {}).get("git_repository", "")
    parsed = extract_owner_repo(git_url)
    if not parsed:
        result.issues.append(f"Cannot parse GitHub URL: {git_url}")
        return result

    owner, repo = parsed

    # Check repo metadata (archived status, last push)
    repo_data = run_gh([f"repos/{owner}/{repo}"])
    if repo_data is None:
        result.issues.append("Cannot reach GitHub API")
        return result

    result.archived = repo_data.get("archived", False)
    if result.archived:
        result.issues.append("Repository is archived")

    pushed_at = repo_data.get("pushed_at", "")
    result.last_commit_date = pushed_at[:10] if pushed_at else None

    # Check latest release/tag
    releases = run_gh([f"repos/{owner}/{repo}/releases?per_page=1"])
    if releases and isinstance(releases, list) and len(releases) > 0:
        result.latest_version = releases[0].get("tag_name")
    else:
        # Fall back to tags
        tags = run_gh([f"repos/{owner}/{repo}/tags?per_page=1"])
        if tags and isinstance(tags, list) and len(tags) > 0:
            result.latest_version = tags[0].get("name")

    if result.latest_version and result.latest_version != pkg["version"]:
        result.issues.append(
            f"Newer version available: {result.latest_version} (pinned: {pkg['version']})"
        )

    # Check license
    license_data = run_gh([f"repos/{owner}/{repo}/license"])
    if license_data:
        spdx = license_data.get("license", {}).get("spdx_id", "")
        if spdx and spdx != "NOASSERTION" and spdx != pkg.get("license"):
            result.license_changed = True
            result.issues.append(
                f"License mismatch: registry says {pkg['license']}, GitHub says {spdx}"
            )

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Check package registry freshness")
    parser.add_argument("--package", help="Check a single package by slug")
    parser.add_argument("--format", choices=["text", "json", "markdown"], default="text")
    args = parser.parse_args()

    registry = json.loads(REGISTRY.read_text())
    packages = registry.get("packages", {})

    if args.package:
        if args.package not in packages:
            print(f"ERROR: Package '{args.package}' not in registry", file=sys.stderr)
            return 1
        packages = {args.package: packages[args.package]}

    results: list[CheckResult] = []
    issues_found = 0

    for slug, pkg in packages.items():
        print(f"Checking {slug}...", file=sys.stderr)
        result = check_package(slug, pkg)
        results.append(result)
        issues_found += len(result.issues)

    if args.format == "json":
        out = [
            {
                "package": r.package,
                "pinned": r.pinned_version,
                "latest": r.latest_version,
                "last_commit": r.last_commit_date,
                "archived": r.archived,
                "license_changed": r.license_changed,
                "issues": r.issues,
            }
            for r in results
        ]
        print(json.dumps(out, indent=2))
    elif args.format == "markdown":
        print("| Package | Pinned | Latest | Last Commit | Issues |")
        print("|---------|--------|--------|-------------|--------|")
        for r in results:
            status = " / ".join(r.issues) if r.issues else "OK"
            print(
                f"| {r.package} | {r.pinned_version} | {r.latest_version or '?'} "
                f"| {r.last_commit_date or '?'} | {status} |"
            )
    else:
        for r in results:
            if r.issues:
                print(f"  {r.package} {'.' * max(1, 30 - len(r.package))} ISSUES")
                for issue in r.issues:
                    print(f"    - {issue}")
            else:
                print(f"  {r.package} {'.' * max(1, 30 - len(r.package))} OK")

    print(f"\n{len(results)} packages checked, {issues_found} issues.", file=sys.stderr)
    return 1 if issues_found > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
