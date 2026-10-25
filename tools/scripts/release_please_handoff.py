#!/usr/bin/env python3
"""Hand a tagged release back to release-please.

Both release-please configurations set `skip-github-release`: auto-release.yml
tags, after its own pre-tag checks. release-please still tracks a merged
release PR by its labels, and while one carries the pending labels it refuses
to open the next release PR ("There are untagged, merged release PRs
outstanding - aborting"). Its own release step clears them by swapping the
pending labels for `autorelease: tagged`; with that step skipped, the tagger
owns the swap.

This performs it for every merged release PR whose tag now exists, reading the
version from the PR's merge commit (the manifest release-please wrote) and the
tag name from the same configuration release-please read. A PR whose tag does
not exist yet keeps its labels: auto-release.yml can withhold an SDK tag (the
Windows pre-tag check) and cut it on a later push, and the swap follows then.
The release liveness alarm (pending_intent_liveness.py) reports a merged
release PR left pending too long.

    python3 tools/scripts/release_please_handoff.py --repo-name OWNER/REPO [--dry-run]

Runs `gh api` with the caller's GH_TOKEN. Exit 0 when every PR was handled or
left for a later tag; 1 when a GitHub call failed.
"""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
import urllib.parse
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ROOT_PACKAGE = "."
DEFAULT_RELEASE_LABEL = "autorelease: tagged"


class GitHubError(RuntimeError):
    pass


@dataclass(frozen=True)
class Surface:
    name: str
    config_path: str
    manifest_path: str
    labels: tuple[str, ...]
    release_labels: tuple[str, ...]
    tag_prefix: str


def _split(value: str | None) -> tuple[str, ...]:
    return tuple(part.strip() for part in (value or "").split(",") if part.strip())


def tag_prefix(config: dict) -> str:
    """The tag prefix release-please composes for the root package."""
    package = config.get("packages", {}).get(ROOT_PACKAGE, {})

    def option(key: str, default):
        return package.get(key, config.get(key, default))

    prefix = ""
    if option("include-component-in-tag", True) and option("component", ""):
        prefix = option("component", "") + option("tag-separator", "-")
    if option("include-v-in-tag", True):
        prefix += "v"
    return prefix


def load_surfaces(repo: Path, versioning: Path) -> list[Surface]:
    data = json.loads(versioning.read_text(encoding="utf-8"))
    surfaces = []
    for name, entry in data["surfaces"].items():
        mapping = entry["release_please"]
        config = json.loads((repo / mapping["config"]).read_text(encoding="utf-8"))
        surfaces.append(Surface(
            name=name,
            config_path=mapping["config"],
            manifest_path=mapping["manifest"],
            labels=_split(config.get("label")) or ("autorelease: pending",),
            release_labels=_split(config.get("release-label")) or (DEFAULT_RELEASE_LABEL,),
            tag_prefix=tag_prefix(config),
        ))
    return surfaces


def plan_handoffs(
    pulls: list[dict],
    surfaces: list[Surface],
    manifest_version: Callable[[Surface, str], str | None],
    tag_exists: Callable[[str], bool],
) -> list[tuple[dict, Surface, str]]:
    """(pull, surface, tag) for each merged pending release PR whose tag exists."""
    ready = []
    for pull in pulls:
        if not pull.get("merged_at") or not pull.get("merge_commit_sha"):
            continue
        names = {label["name"] for label in pull.get("labels", [])}
        for surface in surfaces:
            if not set(surface.labels) <= names:
                continue
            version = manifest_version(surface, pull["merge_commit_sha"])
            if not version:
                continue
            tag = surface.tag_prefix + version
            if tag_exists(tag):
                ready.append((pull, surface, tag))
    return ready


class GitHub:
    def __init__(self, repo_name: str) -> None:
        self.repo_name = repo_name

    def _api(self, *args: str, allow_404: bool = False):
        proc = subprocess.run(
            ["gh", "api", *args], capture_output=True, text=True, encoding="utf-8"
        )
        if proc.returncode != 0:
            if allow_404 and "HTTP 404" in proc.stderr:
                return None
            raise GitHubError(f"gh api {' '.join(args)}: {proc.stderr.strip()}")
        return json.loads(proc.stdout) if proc.stdout.strip() else None

    def pending_pulls(self, labels: tuple[str, ...]) -> list[dict]:
        issues = self._api(
            "-X", "GET", f"repos/{self.repo_name}/issues",
            "-f", "state=closed", "-f", f"labels={','.join(labels)}",
            "-f", "per_page=30",
        ) or []
        pulls = []
        for issue in issues:
            if "pull_request" in issue:
                pulls.append(self._api(f"repos/{self.repo_name}/pulls/{issue['number']}"))
        return pulls

    def manifest_version(self, surface: Surface, ref: str) -> str | None:
        content = self._api(
            f"repos/{self.repo_name}/contents/{surface.manifest_path}?ref={ref}",
            allow_404=True,
        )
        if not content:
            return None
        manifest = json.loads(base64.b64decode(content["content"]).decode("utf-8"))
        return manifest.get(ROOT_PACKAGE)

    def tag_exists(self, tag: str) -> bool:
        return self._api(f"repos/{self.repo_name}/git/ref/tags/{urllib.parse.quote(tag)}", allow_404=True) is not None

    def swap_labels(self, number: int, remove: tuple[str, ...], add: tuple[str, ...]) -> None:
        for label in remove:
            self._api(
                "-X", "DELETE",
                f"repos/{self.repo_name}/issues/{number}/labels/{urllib.parse.quote(label, safe='')}",
                allow_404=True,
            )
        args = ["-X", "POST", f"repos/{self.repo_name}/issues/{number}/labels"]
        for label in add:
            args += ["-f", f"labels[]={label}"]
        self._api(*args)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-name", required=True, help="OWNER/REPO")
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    surfaces = load_surfaces(repo, args.config or repo / "tools/scripts/versioning.json")
    github = GitHub(args.repo_name)
    try:
        pulls: dict[int, dict] = {}
        for surface in surfaces:
            for pull in github.pending_pulls(surface.labels):
                pulls[pull["number"]] = pull
        ready = plan_handoffs(
            list(pulls.values()), surfaces, github.manifest_version, github.tag_exists
        )
        for pull, surface, tag in ready:
            print(f"release-please-handoff: #{pull['number']} ({surface.name}) is tagged "
                  f"{tag}; marking it {', '.join(surface.release_labels)}")
            if not args.dry_run:
                github.swap_labels(pull["number"], surface.labels, surface.release_labels)
        if not ready:
            print("release-please-handoff: no merged release PR is waiting on a cut tag")
    except GitHubError as error:
        print(f"release-please-handoff: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
