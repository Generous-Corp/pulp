#!/usr/bin/env python3
"""Compose grouped release-note highlights for a tagged Pulp release."""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import subprocess
import sys
from collections import OrderedDict

from version_bump_heuristics import classify_conventional


DEFAULT_REPO_URL = "https://github.com/danielraffel/pulp"

SKIP_SUBJECT_PATTERNS = (
    re.compile(r"^chore: bump .*version", re.IGNORECASE),
    re.compile(r"^chore\(release\): ", re.IGNORECASE),
    re.compile(r"^bump .*to v?\d+\.\d+\.\d+$", re.IGNORECASE),
    re.compile(r"^docs: regenerate changelog for v\d+\.\d+\.\d+ \[skip ci\]$", re.IGNORECASE),
)

SECTION_ORDER = (
    "features",
    "fixes",
    "performance",
    "refactors",
    "docs",
    "other",
    "chore",
)

SECTION_TITLES = {
    "features": "### ✨ Features",
    "fixes": "### 🐛 Fixes",
    "performance": "### ⚡ Performance",
    "refactors": "### 🧹 Refactors",
    "docs": "### 📖 Docs",
    "other": "### 📦 Other",
    "chore": "🔧 Chore & CI",
}

TYPE_TO_SECTION = {
    "feat": "features",
    "fix": "fixes",
    "perf": "performance",
    "refactor": "refactors",
    "docs": "docs",
    "doc": "docs",
    "chore": "chore",
    "ci": "chore",
    "deps": "chore",
    "build": "chore",
    "test": "chore",
    "tests": "chore",
}


@dataclasses.dataclass(frozen=True)
class CommitMessage:
    sha: str
    subject: str
    body: str


@dataclasses.dataclass(frozen=True)
class ReleaseEntry:
    subject: str
    display: str
    sha: str
    pr_number: str | None
    section: str
    breaking: bool


def run_git(args: list[str], *, allow_failure: bool = False) -> str:
    result = subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        if allow_failure:
            return ""
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.rstrip("\n")


def commit_message(ref: str) -> CommitMessage:
    raw = run_git(["show", "-s", "--format=%H%x00%s%x00%b", ref])
    sha, subject, body = raw.split("\x00", 2)
    return CommitMessage(sha=sha.strip(), subject=subject.strip(), body=body)


def commit_parents(ref: str) -> list[str]:
    out = run_git(["show", "-s", "--format=%P", ref])
    return [p for p in out.split() if p]


def rev_list(args: list[str]) -> list[str]:
    out = run_git(["rev-list", *args])
    return [line.strip() for line in out.splitlines() if line.strip()]


def previous_tag(tag: str) -> str | None:
    out = run_git(
        ["describe", "--tags", "--match", "v*", "--abbrev=0", f"{tag}^"],
        allow_failure=True,
    )
    return out.strip() or None


_SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)")


def parse_semver(tag: str | None) -> tuple[int, int, int] | None:
    """Parse a leading ``vMAJOR.MINOR.PATCH`` from a tag; None if unparseable."""
    if not tag:
        return None
    match = _SEMVER_RE.match(tag.strip())
    if not match:
        return None
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def bump_level(tag: str, base: str | None) -> str:
    """Classify the semver delta from ``base`` to ``tag``.

    Returns ``"major"``, ``"minor"``, ``"patch"``, or ``"unknown"`` when either
    tag is unparseable or there is no previous tag (first-ever release). Unknown
    deliberately falls back to the full (minor/major) treatment so a first tag or
    an odd tag name never silently degrades to the light patch body.
    """
    cur = parse_semver(tag)
    prev = parse_semver(base)
    if cur is None or prev is None:
        return "unknown"
    if cur[0] != prev[0]:
        return "major"
    if cur[1] != prev[1]:
        return "minor"
    if cur[2] != prev[2]:
        return "patch"
    return "unknown"


def first_nonempty_line(text: str) -> str | None:
    for line in text.splitlines():
        stripped = line.strip()
        if stripped:
            return stripped
    return None


def is_skipped_subject(subject: str) -> bool:
    return any(pattern.search(subject) for pattern in SKIP_SUBJECT_PATTERNS)


def parse_trailers(message: str) -> dict[str, list[str]]:
    result = subprocess.run(
        ["git", "interpret-trailers", "--parse"],
        input=message,
        capture_output=True,
        text=True,
    )
    trailers: dict[str, list[str]] = {}
    for line in result.stdout.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        trailers.setdefault(key.strip().lower(), []).append(value.strip())
    return trailers


def conventional_type(subject: str) -> str | None:
    match = re.match(r"^([a-zA-Z]+)(\([^)]*\))?!?:", subject.strip())
    if match:
        return match.group(1).lower()
    if subject.lower().startswith("feature/"):
        return "feat"
    return None


def section_for_subject(subject: str) -> str:
    kind = conventional_type(subject)
    if not kind:
        return "other"
    return TYPE_TO_SECTION.get(kind, "other")


def has_breaking_marker(messages: list[CommitMessage], title: str) -> bool:
    if classify_conventional(title) == "major":
        return True
    for message in messages:
        if classify_conventional(message.subject) == "major":
            return True
        if re.search(r"(?mi)^BREAKING(?: CHANGE)?:", message.body):
            return True
    return False


def release_note_override(messages: list[CommitMessage]) -> str | None:
    for message in messages:
        if is_skipped_subject(message.subject):
            continue
        values = [
            match.group(1)
            for match in re.finditer(r"(?mi)^Release-Note:\s*(.+)$", message.body)
        ]
        trailers = parse_trailers(f"{message.subject}\n\n{message.body}")
        values.extend(trailers.get("release-note", []))
        for value in values:
            value = value.strip()
            if value and value.lower() not in {"none", "skip"}:
                return value
    return None


def pull_request_entry(commit: CommitMessage) -> tuple[str | None, str | None]:
    match = re.match(r"^Merge pull request #(\d+)\b", commit.subject)
    if match:
        return match.group(1), first_nonempty_line(commit.body)

    squash = re.search(r"\s+\(#(\d+)\)\s*$", commit.subject)
    if squash:
        title = re.sub(r"\s+\(#\d+\)\s*$", "", commit.subject).strip()
        return squash.group(1), title or commit.subject

    return None, None


def associated_messages(commit_sha: str) -> list[CommitMessage]:
    parents = commit_parents(commit_sha)
    if len(parents) < 2:
        return [commit_message(commit_sha)]

    branch_commits = rev_list(["--reverse", f"{parents[0]}..{parents[1]}"])
    messages = [commit_message(sha) for sha in branch_commits]
    return [m for m in messages if not is_skipped_subject(m.subject)]


def github_repo_from_url(repo_url: str) -> str | None:
    match = re.match(r"https://github\.com/([^/]+/[^/]+)/?$", repo_url)
    return match.group(1) if match else None


def pr_has_breaking_label(pr_number: str, github_repo: str | None) -> bool:
    if not github_repo:
        return False
    result = subprocess.run(
        [
            "gh",
            "api",
            f"repos/{github_repo}/issues/{pr_number}/labels",
            "--jq",
            ".[].name",
        ],
        capture_output=True,
        text=True,
        env=os.environ.copy(),
    )
    if result.returncode != 0:
        print(
            f"compose_release_notes.py: warning: could not read labels for #{pr_number}",
            file=sys.stderr,
        )
        return False
    return any(line.strip().lower() == "breaking" for line in result.stdout.splitlines())


def collect_entries(
    tag: str,
    *,
    detect_github_labels: bool = False,
    github_repo: str | None = None,
) -> list[ReleaseEntry]:
    base = previous_tag(tag)
    range_spec = f"{base}..{tag}" if base else tag
    commits = rev_list(["--reverse", "--first-parent", range_spec])
    entries: list[ReleaseEntry] = []

    for sha in commits:
        merge_message = commit_message(sha)
        pr_number, pr_title = pull_request_entry(merge_message)
        messages = associated_messages(sha)
        if not messages and is_skipped_subject(merge_message.subject):
            continue

        subject = pr_title or merge_message.subject
        if is_skipped_subject(subject):
            continue

        display = release_note_override(messages) or subject
        breaking = has_breaking_marker(messages or [merge_message], subject)
        if detect_github_labels and pr_number:
            breaking = breaking or pr_has_breaking_label(pr_number, github_repo)

        entries.append(
            ReleaseEntry(
                subject=subject,
                display=display,
                sha=merge_message.sha,
                pr_number=pr_number,
                section=section_for_subject(subject),
                breaking=breaking,
            )
        )

    return entries


def entry_link(entry: ReleaseEntry, repo_url: str) -> str:
    if entry.pr_number:
        return f"[#{entry.pr_number}]({repo_url.rstrip('/')}/pull/{entry.pr_number})"
    return f"`{entry.sha[:7]}`"


def bullet_for_entry(entry: ReleaseEntry, repo_url: str) -> str:
    return f"- {entry.display} ({entry_link(entry, repo_url)})"


def render(
    entries: list[ReleaseEntry],
    *,
    repo_url: str = DEFAULT_REPO_URL,
    tier: str = "minor",
) -> str:
    """Render the grouped release-note body.

    ``tier`` (``"patch" | "minor" | "major" | "unknown"``) controls weight: a
    ``patch`` release emits a light grouped-only body (no ``## Highlights``
    wrapper heading); ``minor``/``major``/``unknown`` emit the full treatment.
    The ``## ⚠️ Breaking Changes`` section renders on **every** tier — a breaking
    change on a patch tag is exactly the case that must not be hidden.
    """
    light = tier == "patch"
    breaking = [entry for entry in entries if entry.breaking]
    grouped: OrderedDict[str, list[ReleaseEntry]] = OrderedDict(
        (section, []) for section in SECTION_ORDER
    )
    for entry in entries:
        if entry.breaking:
            continue
        grouped.setdefault(entry.section, []).append(entry)

    parts: list[str] = []
    if breaking:
        parts.append("## ⚠️ Breaking Changes")
        parts.append("")
        parts.extend(bullet_for_entry(entry, repo_url) for entry in breaking)
        parts.append("")

    nonbreaking_count = sum(len(items) for items in grouped.values())
    if nonbreaking_count:
        if not light:
            parts.append("## Highlights")
            parts.append("")
        for section, items in grouped.items():
            if not items:
                continue
            if section == "chore":
                parts.append(f"<details><summary>{SECTION_TITLES[section]}</summary>")
                parts.append("")
                parts.extend(bullet_for_entry(entry, repo_url) for entry in items)
                parts.append("")
                parts.append("</details>")
                parts.append("")
                continue
            parts.append(SECTION_TITLES[section])
            parts.append("")
            parts.extend(bullet_for_entry(entry, repo_url) for entry in items)
            parts.append("")
    elif breaking:
        if not light:
            parts.append("## Highlights")
            parts.append("")
        parts.append("No non-breaking highlights were detected for this tag.")
        parts.append("")

    if not parts:
        return "## Highlights\n\nNo release-note highlights were detected for this tag."

    return "\n".join(parts).rstrip()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compose grouped release-note highlights for a tag.",
    )
    parser.add_argument("tag", help="Release tag, for example v0.577.0")
    parser.add_argument(
        "--repo-url",
        default=DEFAULT_REPO_URL,
        help="Repository URL used for pull request links.",
    )
    parser.add_argument(
        "--github-repo",
        help="owner/repo slug for optional GitHub issue-label lookup.",
    )
    parser.add_argument(
        "--detect-github-labels",
        action="store_true",
        help="Best-effort detection of a PR label named 'breaking'.",
    )
    parser.add_argument(
        "--tier",
        choices=["auto", "patch", "minor", "major"],
        default="auto",
        help=(
            "Release weight. 'auto' (default) derives it from the semver delta "
            "between this tag and the previous v* tag: patch => light "
            "grouped-only body, minor/major => full Highlights treatment. "
            "Breaking changes always render first regardless of tier."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        github_repo = args.github_repo or github_repo_from_url(args.repo_url)
        entries = collect_entries(
            args.tag,
            detect_github_labels=args.detect_github_labels,
            github_repo=github_repo,
        )
        tier = args.tier
        if tier == "auto":
            tier = bump_level(args.tag, previous_tag(args.tag))
        print(render(entries, repo_url=args.repo_url, tier=tier))
    except Exception as exc:
        print(f"compose_release_notes.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
