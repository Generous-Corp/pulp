#!/usr/bin/env python3
"""Tests for release_please_handoff.py.

Load-bearing behavior: a merged release PR is handed back to release-please
(its pending labels swapped for `autorelease: tagged`) only once the tag for
the version in its merge commit's manifest exists; a withheld tag keeps the
labels; and the tag names come out exactly as auto-release.yml cuts them
(`vX.Y.Z`, `plugin-vX.Y.Z`).
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import release_please_handoff as handoff  # noqa: E402

REPO = HERE.parent.parent


def pull(number: int, labels: list[str], merged: bool = True) -> dict:
    return {
        "number": number,
        "merged_at": "2026-10-24T12:00:00Z" if merged else None,
        "merge_commit_sha": f"{number:040d}" if merged else None,
        "labels": [{"name": name} for name in labels],
    }


class TagNames(unittest.TestCase):
    def test_the_shipped_configs_name_the_existing_tag_series(self) -> None:
        surfaces = {s.name: s for s in handoff.load_surfaces(
            REPO, REPO / "tools/scripts/versioning.json")}
        self.assertEqual(surfaces["sdk"].tag_prefix, "v")
        self.assertEqual(surfaces["plugin"].tag_prefix, "plugin-v")
        self.assertEqual(surfaces["sdk"].labels, ("autorelease: pending", "release: sdk"))
        self.assertEqual(surfaces["plugin"].release_labels, ("autorelease: tagged",))

    def test_package_options_override_top_level_ones(self) -> None:
        config = {"include-component-in-tag": True,
                  "packages": {".": {"component": "x", "include-v-in-tag": False}}}
        self.assertEqual(handoff.tag_prefix(config), "x-")
        self.assertEqual(handoff.tag_prefix({"include-component-in-tag": False,
                                             "packages": {".": {"component": "x"}}}), "v")


class PlanHandoffs(unittest.TestCase):
    SDK = handoff.Surface("sdk", "c", "m", ("autorelease: pending", "release: sdk"),
                          ("autorelease: tagged",), "v")
    PLUGIN = handoff.Surface("plugin", "c", "m", ("autorelease: pending", "release: plugin"),
                             ("autorelease: tagged",), "plugin-v")

    def plan(self, pulls, tags, versions=None):
        versions = versions or {"sdk": "1.11.0", "plugin": "0.492.0"}
        return handoff.plan_handoffs(
            pulls, [self.SDK, self.PLUGIN],
            lambda surface, _ref: versions.get(surface.name),
            lambda tag: tag in tags,
        )

    def test_a_tagged_release_pr_is_handed_back(self) -> None:
        ready = self.plan([pull(7, ["autorelease: pending", "release: sdk"])], {"v1.11.0"})
        self.assertEqual([(p["number"], s.name, t) for p, s, t in ready], [(7, "sdk", "v1.11.0")])

    def test_a_withheld_tag_keeps_the_labels(self) -> None:
        self.assertEqual(self.plan([pull(7, ["autorelease: pending", "release: sdk"])], set()), [])

    def test_each_surface_matches_only_its_own_labels(self) -> None:
        ready = self.plan([pull(8, ["autorelease: pending", "release: plugin"])],
                          {"v1.11.0", "plugin-v0.492.0"})
        self.assertEqual([(s.name, t) for _p, s, t in ready], [("plugin", "plugin-v0.492.0")])

    def test_an_open_or_unlabelled_pr_is_ignored(self) -> None:
        pulls = [pull(9, ["autorelease: pending", "release: sdk"], merged=False),
                 pull(10, ["autorelease: pending"])]
        self.assertEqual(self.plan(pulls, {"v1.11.0", "plugin-v0.492.0"}), [])


if __name__ == "__main__":
    unittest.main()
