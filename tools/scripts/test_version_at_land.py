#!/usr/bin/env python3
"""Unit tests for version_at_land.py pure logic (aggregate_intent, plan_assignments)."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


val = _load("version_at_land")
surfaces = _load("version_bump_surfaces")

Surface = surfaces.Surface
VersionFile = surfaces.VersionFile


def trailers(*values):
    return {"version-bump": list(values)}


def surface(name):
    return Surface(name=name, label=name,
                   version_files=[VersionFile(path=f"{name}.txt", kind="raw")],
                   trigger_paths=[])


class AggregateIntentTest(unittest.TestCase):
    def test_absent_is_none(self):
        self.assertIsNone(val.aggregate_intent(trailers(), "sdk"))

    def test_single_level(self):
        self.assertEqual(val.aggregate_intent(trailers('sdk=minor reason="x"'), "sdk"), "minor")

    def test_highest_wins_across_range(self):
        t = trailers('sdk=patch reason="a"', 'sdk=major reason="b"', 'sdk=minor reason="c"')
        self.assertEqual(val.aggregate_intent(t, "sdk"), "major")

    def test_skip_is_ignored(self):
        self.assertIsNone(val.aggregate_intent(trailers('sdk=skip reason="x"'), "sdk"))

    def test_skip_does_not_beat_real_level(self):
        t = trailers('sdk=skip reason="x"', 'sdk=patch reason="y"')
        self.assertEqual(val.aggregate_intent(t, "sdk"), "patch")

    def test_other_surface_not_picked_up(self):
        self.assertIsNone(val.aggregate_intent(trailers('plugin=minor reason="x"'), "sdk"))

    def test_unknown_level_ignored(self):
        self.assertIsNone(val.aggregate_intent(trailers('sdk=bogus reason="x"'), "sdk"))


class PlanAssignmentsTest(unittest.TestCase):
    def setUp(self):
        self.config = surfaces.Config(
            surfaces=[surface("sdk"), surface("plugin")],
            generated_globs=[], trailer_version_bump="Version-Bump")
        self.versions = {"sdk": "0.599.0", "plugin": "0.308.1"}

    def plan(self, t):
        return val.plan_assignments(self.config, t, lambda s: self.versions[s.name])

    def test_single_surface_intent(self):
        p = self.plan(trailers('sdk=minor reason="x"'))
        self.assertEqual(len(p), 1)
        self.assertEqual((p[0].surface, p[0].current, p[0].assigned), ("sdk", "0.599.0", "0.600.0"))

    def test_both_surfaces_independent(self):
        p = {a.surface: a for a in self.plan(
            trailers('sdk=patch reason="x"', 'plugin=major reason="y"'))}
        self.assertEqual(p["sdk"].assigned, "0.599.1")
        self.assertEqual(p["plugin"].assigned, "1.0.0")

    def test_no_intent_no_assignment(self):
        self.assertEqual(self.plan(trailers()), [])

    def test_skip_only_surface_gets_no_assignment(self):
        self.assertEqual(self.plan(trailers('sdk=skip reason="x"')), [])

    def test_unreadable_version_is_skipped(self):
        p = val.plan_assignments(self.config, trailers('sdk=minor reason="x"'),
                                 lambda s: None)
        self.assertEqual(p, [])


if __name__ == "__main__":
    unittest.main()
