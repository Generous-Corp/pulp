#!/usr/bin/env python3
"""Contract test for codecov.yml component classification (#1055).

The 22 components in `codecov.yml` slice the single-upload coverage
report along the (subsystem × platform × surface) axes documented in
`docs/guides/coverage.md`. Codecov's path matcher will silently
double-count any first-party file matched by more than one component,
inflating the line totals on the dashboard for every overlapping
bucket.

A 2026-04-30 audit on `origin/main` showed ~110 first-party files
falling into 20 silent classification buckets — the platform
subsystems (`audio`, `midi`, `view`, `render`, `platform`, `canvas`)
matched both their own `core/<sub>/**` glob AND the platform components
(`apple`, `android`, `linux`, `windows`), and the `tools` component
greedily matched the `cli` surface too. Two `android`/`render`
components also listed overlapping patterns within their own paths
list, causing 3-way self-overlap (`platform,android,android`).

This test enumerates first-party files via `git ls-files`, applies
codecov's glob semantics to the active component set, and fails if any
file matches more than one component (or zero components, except for
files in the documented allowlist below). It would re-fail the moment
the next contributor reintroduces an overlapping pattern.

Run:
    python3 tools/scripts/test_codecov_components.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
CODECOV = REPO_ROOT / "codecov.yml"


# Patterns that codecov.yml's top-level `ignore:` block excludes from
# coverage entirely. Any file matched by these is not "first-party" for
# the purposes of component classification.
def _load_codecov_doc() -> dict:
    with CODECOV.open("r", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def _glob_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate a codecov glob (with `**`) into a regex.

    Codecov uses ant-style globs:
      - `**` matches any sequence of path segments (zero or more), including
        crossing `/` boundaries.
      - `*` matches anything except `/`.
      - other characters are literal.

    Trailing `/**` makes the pattern match the directory itself plus
    everything beneath it. Patterns without a trailing `/**` only match
    files at the matched depth.
    """
    # Tokenise — handle `**` before `*` so we don't mis-translate it.
    out: list[str] = ["^"]
    i = 0
    while i < len(pattern):
        ch = pattern[i]
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
            continue
        if ch == "*":
            out.append("[^/]*")
            i += 1
            continue
        out.append(re.escape(ch))
        i += 1
    out.append("$")
    return re.compile("".join(out))


class _Matcher:
    """Codecov-style include/exclude matcher for one paths list."""

    def __init__(self, patterns: list[str]) -> None:
        self.includes: list[re.Pattern[str]] = []
        self.excludes: list[re.Pattern[str]] = []
        for pat in patterns:
            if pat.startswith("!"):
                self.excludes.append(_glob_to_regex(pat[1:]))
            else:
                self.includes.append(_glob_to_regex(pat))

    def matches(self, path: str) -> bool:
        if not any(rx.match(path) for rx in self.includes):
            return False
        if any(rx.match(path) for rx in self.excludes):
            return False
        return True


def _first_party_files() -> list[str]:
    """All git-tracked files minus the codecov.yml `ignore:` set."""
    doc = _load_codecov_doc()
    ignore_patterns = [_glob_to_regex(p) for p in doc.get("ignore", [])]

    proc = subprocess.run(
        ["git", "ls-files"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    paths = proc.stdout.splitlines()

    def is_first_party(path: str) -> bool:
        # Drop anything matched by codecov ignore patterns.
        if any(rx.match(path) for rx in ignore_patterns):
            return False
        # Coverage components only target compiled/source surfaces under
        # `core/`, `apple/`, `android/`, `inspect/`, `ship/`, and `tools/`.
        # Anything else (docs, .agents/, ci/, README, top-level configs,
        # planning/, etc.) is intentionally outside the partition.
        first_party_roots = (
            "core/",
            "apple/",
            "android/",
            "inspect/",
            "ship/",
            "tools/",
        )
        return path.startswith(first_party_roots)

    return [p for p in paths if is_first_party(p)]


# Files that intentionally live under more than one component, or
# under zero components. Empty by design after #1055 — the partition is
# meant to be exhaustive AND mutually exclusive over the first-party
# surfaces. Add an entry only with a documented reason; CI will reject
# silent additions because the structural test below diff-checks the
# actual partition outcome.
ALLOWED_MULTI_COMPONENT: dict[str, str] = {}
ALLOWED_UNCLASSIFIED: dict[str, str] = {}


class CodecovComponentClassification(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        cls.doc = _load_codecov_doc()
        cls.components = cls.doc["component_management"]["individual_components"]
        cls.matchers = {
            entry["component_id"]: _Matcher(entry["paths"])
            for entry in cls.components
        }
        cls.first_party = _first_party_files()

    def _classify(self, path: str) -> list[str]:
        return [cid for cid, matcher in self.matchers.items() if matcher.matches(path)]

    def test_every_first_party_file_matches_exactly_one_component(self):
        multi: dict[str, list[str]] = {}
        unclassified: list[str] = []
        for path in self.first_party:
            hits = self._classify(path)
            if len(hits) == 0 and path not in ALLOWED_UNCLASSIFIED:
                unclassified.append(path)
            elif len(hits) > 1 and path not in ALLOWED_MULTI_COMPONENT:
                multi[path] = hits

        msg_parts = []
        if multi:
            sample = list(multi.items())[:10]
            msg_parts.append(
                "Files matching more than one Codecov component "
                f"({len(multi)} total) — Codecov double-counts these on "
                "the dashboard. First 10:\n"
                + "\n".join(f"  {path}: {hits}" for path, hits in sample)
            )
        if unclassified:
            sample = unclassified[:10]
            msg_parts.append(
                "First-party files matching no Codecov component "
                f"({len(unclassified)} total) — they will be invisible to "
                "the slicing dashboard. First 10:\n"
                + "\n".join(f"  {path}" for path in sample)
            )
        self.assertFalse(msg_parts, "\n\n".join(msg_parts))

    def test_no_component_double_matches_a_path_via_overlapping_patterns(self):
        # The Codex-flagged 3-way matches (`platform,android,android`,
        # `render,android,android`) were caused by ONE component listing
        # overlapping include patterns in its own paths list, e.g.
        # `core/render/platform/android/**` AND `core/**/android/**`
        # both matching the same file. Detect that explicitly so a
        # future edit can't reintroduce the 3-way self-overlap.
        for entry in self.components:
            cid = entry["component_id"]
            includes = [p for p in entry["paths"] if not p.startswith("!")]
            if len(includes) <= 1:
                continue
            include_matchers = [_glob_to_regex(p) for p in includes]
            for path in self.first_party:
                hits = sum(1 for rx in include_matchers if rx.match(path))
                if hits > 1:
                    self.fail(
                        f"{cid} component lists overlapping include patterns "
                        f"that both match {path!r}; dedupe the paths list. "
                        f"include patterns = {includes}"
                    )

    def test_glob_translator_handles_codecov_semantics(self):
        # Sanity check on the helper — `**` crosses `/`, `*` does not.
        self.assertTrue(_glob_to_regex("core/audio/**").match("core/audio/x.cpp"))
        self.assertTrue(
            _glob_to_regex("core/audio/**").match("core/audio/platform/win/d.cpp")
        )
        self.assertFalse(_glob_to_regex("core/audio/*").match("core/audio/x/y.cpp"))
        self.assertTrue(_glob_to_regex("core/**/android/**").match(
            "core/render/platform/android/x.cpp"
        ))


if __name__ == "__main__":
    unittest.main()
