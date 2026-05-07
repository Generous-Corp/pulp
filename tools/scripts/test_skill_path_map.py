#!/usr/bin/env python3
"""Contract test for ``tools/scripts/skill_path_map.json``.

Catches the silent-no-op class of bugs documented in pulp #1053: a
pattern that matches zero tracked files is a bug masquerading as
working configuration. When a contributor edits the file the pattern
*intended* to mark, ``skill_sync_check.py`` doesn't fire, the SKILL.md
goes out of date, and CI never notices.

This contract enforces:

    Every pattern in ``skill_path_map.json`` must EITHER match at least
    one tracked file in the repo, OR live under a skill entry that
    declares a ``_doc.empty-ok`` annotation explaining why the empty
    pattern is intentional (e.g. external SDK paths that are
    developer-supplied and not committed).

The matcher used here is the production gitignore-style matcher from
``skill_sync_check._glob_match`` — keep that as the single source of
truth so this test and the gate it backs can never disagree.
"""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import skill_sync_check as ssc  # noqa: E402  — path inserted above

PATH_MAP = REPO / "tools" / "scripts" / "skill_path_map.json"


def _tracked_files() -> list[str]:
    out = subprocess.check_output(
        ["git", "ls-files"], cwd=REPO, text=True
    )
    return out.splitlines()


def _load_skill_map() -> dict:
    return json.loads(PATH_MAP.read_text())


class NoSilentEmptyPatterns(unittest.TestCase):
    """Every mapped pattern must match a tracked file or be annotated."""

    def test_every_path_matches_at_least_one_tracked_file_or_has_empty_ok_annotation(
        self,
    ) -> None:
        skill_map = _load_skill_map()
        files = _tracked_files()

        offenders: list[str] = []
        for skill, entry in skill_map.get("skills", {}).items():
            if not isinstance(entry, dict):
                continue
            patterns = entry.get("paths", []) or []
            doc = entry.get("_doc") or {}
            empty_ok = isinstance(doc, dict) and bool(doc.get("empty-ok"))

            for pat in patterns:
                hits = [f for f in files if ssc._glob_match(f, pat)]
                if hits:
                    continue
                if empty_ok:
                    continue
                offenders.append(f"{skill}: {pat}")

        if offenders:
            joined = "\n  ".join(offenders)
            self.fail(
                "Silent-no-op patterns found in skill_path_map.json (see "
                "pulp #1053). Each pattern below matches zero tracked "
                "files. Either correct the pattern to point at the moved "
                "code, drop it, or — if it intentionally references an "
                "external/developer-supplied path — add a "
                "_doc.empty-ok annotation on the skill entry.\n"
                f"  {joined}"
            )

    def test_doc_empty_ok_only_used_for_external_sdk_paths(self) -> None:
        """Conservative annotation policy from pulp #1053.

        Empty patterns that aren't external SDK references are bugs,
        not intentional. Restrict the ``_doc.empty-ok`` escape hatch to
        ``external/*`` patterns so future edits can't silently re-open
        the no-op door for in-tree code.
        """
        skill_map = _load_skill_map()
        files = _tracked_files()

        offenders: list[str] = []
        for skill, entry in skill_map.get("skills", {}).items():
            if not isinstance(entry, dict):
                continue
            doc = entry.get("_doc") or {}
            if not (isinstance(doc, dict) and doc.get("empty-ok")):
                continue
            patterns = entry.get("paths", []) or []
            for pat in patterns:
                hits = [f for f in files if ssc._glob_match(f, pat)]
                if hits:
                    continue
                if not pat.startswith("external/"):
                    offenders.append(f"{skill}: {pat}")

        if offenders:
            joined = "\n  ".join(offenders)
            self.fail(
                "Non-external empty pattern relies on _doc.empty-ok "
                "annotation. Per pulp #1053, only external/*-SDK paths "
                "may be annotated as intentionally empty:\n"
                f"  {joined}"
            )


if __name__ == "__main__":
    unittest.main()
