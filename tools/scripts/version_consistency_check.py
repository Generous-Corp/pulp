#!/usr/bin/env python3
"""Verify the four version-bearing files agree on the top SDK / plugin version.

Pulp ships an SDK version in CMakeLists.txt (`project(Pulp VERSION X.Y.Z)`)
and a plugin version in `.claude-plugin/plugin.json` + `marketplace.json`.
CHANGELOG.md carries the same SDK version in its newest `## [X.Y.Z]` heading
once the release lands. If a PR's CHANGELOG entry advertises a version that
isn't reflected in CMakeLists.txt, the next auto-release tag won't fire and
the entry sits orphaned (which is what triggered Codex P2 on PR #2331:
CHANGELOG said `[0.138.0]` while CMakeLists was still at `0.137.1`).

This script reads all four files and verifies:
  1. CMakeLists.txt's `project(Pulp VERSION X.Y.Z)` line is present.
  2. The plugin.json + marketplace.json plugin entries match each other.
  3. The CHANGELOG's newest `## [X.Y.Z]` entry matches CMakeLists.txt.

Exit code 0 → all in sync. Exit code 1 → drift detected, with diagnostic.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def read_cmake_version() -> str:
    text = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", text)
    if not m:
        raise RuntimeError("CMakeLists.txt: no VERSION line found")
    return m.group(1)


def read_plugin_versions() -> tuple[str, str, str]:
    """Returns (plugin.json::version, marketplace.json::version,
    marketplace.json::plugins[0].version). The marketplace top-level
    `version` and the nested `plugins[0].version` BOTH exist in
    Pulp's manifest — the version-bump tooling and cmd_version.cpp
    each consume one, so they must agree. Codex P2 on PR #2341
    flagged the gap where this script only read the top-level and
    a future bump could leave `plugins[0].version` stale."""
    plugin = json.loads(
        (REPO_ROOT / ".claude-plugin" / "plugin.json").read_text(encoding="utf-8")
    )
    market = json.loads(
        (REPO_ROOT / ".claude-plugin" / "marketplace.json").read_text(encoding="utf-8")
    )
    plugin_version = plugin.get("version") or ""
    market_top = market.get("version") or ""
    market_nested = (market.get("plugins") or [{}])[0].get("version") or ""
    return plugin_version, market_top, market_nested


def read_top_changelog_version() -> str:
    text = (REPO_ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    for line in text.splitlines():
        m = re.match(r"## \[(\d+\.\d+\.\d+)\]", line)
        if m:
            return m.group(1)
    raise RuntimeError("CHANGELOG.md: no '## [X.Y.Z]' heading found")


def main() -> int:
    cmake_version = read_cmake_version()
    plugin_version, market_top, market_nested = read_plugin_versions()
    changelog_version = read_top_changelog_version()

    failures = []
    if plugin_version != market_top:
        failures.append(
            f"plugin.json ({plugin_version}) != marketplace.json top-level"
            f" version ({market_top})"
        )
    if plugin_version != market_nested:
        failures.append(
            f"plugin.json ({plugin_version}) != marketplace.json"
            f" plugins[0].version ({market_nested})"
        )
    if market_top != market_nested:
        failures.append(
            f"marketplace.json top-level version ({market_top}) !="
            f" plugins[0].version ({market_nested})"
        )
    # We only flag the "CHANGELOG advertises an unreleased version" direction:
    # CHANGELOG top > CMakeLists. The reverse — CMakeLists ahead of CHANGELOG —
    # is the harmless window between a merge bumping the SDK and auto-release
    # regenerating the CHANGELOG. That window closes on its own.
    def _ver_tuple(v: str) -> tuple[int, ...]:
        return tuple(int(p) for p in v.split("."))

    if changelog_version != cmake_version:
        cl = _ver_tuple(changelog_version)
        cm = _ver_tuple(cmake_version)
        if cl > cm:
            failures.append(
                f"CHANGELOG top entry [{changelog_version}] advertises a version"
                f" higher than CMakeLists VERSION {cmake_version}. auto-release tags"
                " from CMakeLists VERSION, so the CHANGELOG entry would sit orphaned"
                " until the next legit bump. This is the drift Codex P2 on #2331"
                " flagged: a 'chore: bump versions' commit that silently no-op'd on"
                " CMakeLists and only mutated CHANGELOG."
            )
        # cmake_version > changelog_version is the post-bump pre-regen window;
        # no failure — the auto-release-bot CHANGELOG regen will close it.

    if failures:
        print("Version drift detected:")
        for line in failures:
            print(f"  - {line}")
        print(f"\nCMakeLists VERSION:            {cmake_version}")
        print(f"plugin.json version:           {plugin_version}")
        print(f"marketplace top version:       {market_top}")
        print(f"marketplace plugins[0].version: {market_nested}")
        print(f"CHANGELOG top entry:           {changelog_version}")
        return 1

    print(f"version consistency OK: SDK {cmake_version}, plugin {plugin_version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
