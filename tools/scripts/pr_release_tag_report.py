#!/usr/bin/env python3
"""Report the release tags a PR head is expected to create after merge."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
from pathlib import Path

from auto_release_decision import decide
from gate_common import (
    git_commit_trailers,
    git_range_trailers,
    repo_root as discover_repo_root,
)
from version_bump_surfaces import load_config, version_at_base


TAG_SPECS = {
    "sdk": ("refs/tags/v[0-9]*", "v"),
    "plugin": ("refs/tags/plugin-v[0-9]*", "plugin-v"),
}


def _latest_tag(pattern: str) -> str:
    result = subprocess.run(
        [
            "git",
            "for-each-ref",
            "--sort=-version:refname",
            "--format=%(refname:short)",
            "--count=1",
            pattern,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def _find_bump_commit(head: str, version_file, current: str | None) -> str:
    if not current:
        return ""
    result = subprocess.run(
        ["git", "log", "--format=%H", head, "--", version_file.path],
        check=True,
        capture_output=True,
        text=True,
    )
    for sha in result.stdout.splitlines():
        at_commit = version_at_base(sha, version_file)
        at_parent = version_at_base(f"{sha}^", version_file)
        if at_commit == current and at_commit != at_parent:
            return sha
    return ""


def _has_release_skip(sha: str) -> bool:
    if not sha:
        return False
    parsed_skip = any(
        re.match(r"^\s*skip\b", value, re.IGNORECASE)
        for value in git_commit_trailers(sha).get("release", [])
    )
    if parsed_skip:
        return True
    body = subprocess.run(
        ["git", "show", "-s", "--format=%B", sha],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return bool(
        re.search(r"(?im)^[ \t]*release:[ \t]+skip(?:[ \t]|$)", body)
    )


def _resolve_commit(ref: str) -> str:
    result = subprocess.run(
        [
            "git",
            "rev-parse",
            "--verify",
            "--quiet",
            "--end-of-options",
            f"{ref}^{{commit}}",
        ],
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def _auto_release_guard_reason(
    base: str,
    head: str,
    pr_title: str | None,
) -> str:
    base_sha = _resolve_commit(base)
    if not base_sha:
        return f"auto-release guard could not resolve {base}"
    sha = _resolve_commit(head)
    if not sha:
        return f"auto-release guard could not resolve {head}"

    count_result = subprocess.run(
        ["git", "rev-list", "--count", f"{base_sha}..{sha}"],
        capture_output=True,
        text=True,
    )
    if count_result.returncode != 0:
        return f"auto-release guard could not inspect {base}..{head}"
    commit_count = int(count_result.stdout.strip())
    if commit_count == 0:
        return ""

    if commit_count == 1:
        subject = subprocess.run(
            ["git", "show", "-s", "--format=%s", sha],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        trailers = git_commit_trailers(sha)
    else:
        if pr_title is None:
            return "multi-commit squash subject unavailable without --pr-title"
        subject = pr_title.strip()
        trailers = git_range_trailers(base_sha, sha)

    if any(
        re.match(r"^\s*skip\b", value, re.IGNORECASE)
        for value in trailers.get("release", [])
    ):
        return "squash message carries Release: skip"
    for value in trailers.get("version-bump", []):
        match = re.match(r"^\s*skip\b(.*)$", value, re.IGNORECASE)
        if match and re.search(r'reason\s*=\s*"[^"]+"', match.group(1)):
            return "squash message carries top-level Version-Bump: skip"
    if subject.lower().startswith("revert"):
        return "squash subject is a revert"
    for value in trailers.get("revert-of", []):
        match = re.search(r"\b[0-9a-fA-F]{7,64}\b", value)
        if match and _resolve_commit(match.group(0)):
            return "squash message has a resolved Revert-Of target"
    return ""


def build_report(
    config,
    base: str,
    head: str,
    guard_head: str | None = None,
    pr_title: str | None = None,
) -> list[dict]:
    report: list[dict] = []
    guard_reason = _auto_release_guard_reason(
        base,
        guard_head or head,
        pr_title,
    )
    for surface in config.surfaces:
        if surface.name not in TAG_SPECS:
            raise ValueError(
                f"no release-tag mapping configured for surface {surface.name!r}"
            )
        tag_pattern, tag_prefix = TAG_SPECS[surface.name]
        version_file = surface.version_files[0]
        base_version = version_at_base(base, version_file)
        head_version = version_at_base(head, version_file)
        latest_tag = _latest_tag(tag_pattern)
        tag_version = (
            version_at_base(latest_tag, version_file) if latest_tag else None
        )
        bump_commit = _find_bump_commit(head, version_file, head_version)
        if guard_reason:
            decision = {
                "should_tag": 0,
                "reason": f"auto-release guard: {guard_reason}",
            }
        else:
            decision = decide(
                head_version,
                tag_version,
                _has_release_skip(bump_commit),
                surface.name,
            )
        expected_tag = (
            f"{tag_prefix}{head_version}" if decision["should_tag"] else ""
        )
        report.append(
            {
                "surface": surface.name,
                "label": surface.label,
                "base_version": base_version,
                "head_version": head_version,
                "latest_tag": latest_tag,
                "expected_tag": expected_tag,
                "should_tag": decision["should_tag"],
                "reason": decision["reason"],
            }
        )
    return report


def render_markdown(report: list[dict]) -> str:
    lines = [
        "## Expected release tags",
        "",
        "| Surface | Base | PR head | Latest tag | Queue prediction |",
        "| --- | --- | --- | --- | --- |",
    ]
    for item in report:
        prediction = (
            f"`{item['expected_tag']}`" if item["expected_tag"] else "No new tag"
        )
        lines.append(
            "| {label} | `{base}` | `{head}` | `{latest}` | {prediction} |".format(
                label=item["label"],
                base=item["base_version"] or "missing",
                head=item["head_version"] or "missing",
                latest=item["latest_tag"] or "none",
                prediction=prediction,
            )
        )
    lines.extend(
        [
            "",
            "This is the queue-time prediction from the fetched tag state, "
            "auto-release squash-message guard, and sticky `Release: skip` metadata. "
            "Actual signed tags are created after merge by `auto-release.yml`.",
        ]
    )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", default="HEAD")
    parser.add_argument(
        "--guard-head",
        default=None,
        help="PR tip ref used for auto-release guard prediction (defaults to --head)",
    )
    parser.add_argument(
        "--pr-title",
        default=None,
        help="PR title used as the squash subject for a multi-commit PR",
    )
    parser.add_argument("--config", default="tools/scripts/versioning.json")
    parser.add_argument("--repo-root")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    args = parser.parse_args(argv)

    root = (
        Path(args.repo_root).resolve()
        if args.repo_root
        else discover_repo_root().resolve()
    )
    os.chdir(root)
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = root / config_path
    report = build_report(
        load_config(config_path),
        args.base,
        args.head,
        args.guard_head,
        args.pr_title,
    )
    if args.format == "json":
        print(json.dumps(report, sort_keys=True))
    else:
        print(render_markdown(report), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
