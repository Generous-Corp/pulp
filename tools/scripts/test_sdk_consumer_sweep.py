#!/usr/bin/env python3
"""Tests for tools/scripts/sdk_consumer_sweep.py pure helpers.

The clone/configure/build orchestration needs network + a real SDK and is
exercised by running the tool; here we lock down the decision logic that decides
WHAT builds, WHICH binaries get measured, and HOW the result is reported —
without touching the network or a compiler.

Run:
    python3 tools/scripts/test_sdk_consumer_sweep.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/scripts/sdk_consumer_sweep.py"


def _load():
    spec = importlib.util.spec_from_file_location("sdk_consumer_sweep", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["sdk_consumer_sweep"] = mod  # dataclass needs the module registered
    spec.loader.exec_module(mod)
    return mod


MOD = _load()

# A minimal registry + recipes shaped like the real files.
CONSUMERS = {
    "repos": [
        {"repo": "owner/pulp-example-plugins",
         "status": {"state": "merged"}},
        {"repo": "owner/pulp-gpu-nam",
         "status": {"state": "build-pass"}},
        {"repo": "owner/pulp-spectral-lab",
         "status": {"state": "not-applicable",
                    "next_action": "README-only mirror."}},
    ]
}
RECIPES = {
    "defaults": {"configure_flags": []},
    "repos": {
        "pulp-gpu-nam": {"configure_flags": ["-DGPU_NAM_USE_INSTALLED_PULP=ON"]},
        "pulp-tempo-sampler": {"skip": "explicit skip reason"},
    },
}


class BuildPlans(unittest.TestCase):
    def test_not_applicable_is_skipped_with_next_action_reason(self):
        plans = MOD.build_plans(CONSUMERS, RECIPES, only=None)
        by = {p.name: p for p in plans}
        self.assertEqual(by["pulp-spectral-lab"].skip_reason, "README-only mirror.")
        # A buildable repo has no skip reason.
        self.assertIsNone(by["pulp-example-plugins"].skip_reason)

    def test_recipe_flags_applied(self):
        plans = MOD.build_plans(CONSUMERS, RECIPES, only=None)
        by = {p.name: p for p in plans}
        self.assertEqual(by["pulp-gpu-nam"].configure_flags,
                         ["-DGPU_NAM_USE_INSTALLED_PULP=ON"])
        self.assertEqual(by["pulp-example-plugins"].configure_flags, [])

    def test_only_filters_by_short_or_full_name(self):
        p1 = MOD.build_plans(CONSUMERS, RECIPES, only=["pulp-gpu-nam"])
        self.assertEqual([p.name for p in p1], ["pulp-gpu-nam"])
        p2 = MOD.build_plans(CONSUMERS, RECIPES, only=["owner/pulp-example-plugins"])
        self.assertEqual([p.name for p in p2], ["pulp-example-plugins"])

    def test_explicit_recipe_skip_wins_even_when_state_buildable(self):
        consumers = {"repos": [
            {"repo": "owner/pulp-tempo-sampler", "status": {"state": "merged"}}]}
        plans = MOD.build_plans(consumers, RECIPES, only=None)
        self.assertEqual(plans[0].skip_reason, "explicit skip reason")


class ArtifactDiscovery(unittest.TestCase):
    def _tree(self) -> pathlib.Path:
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        # A shipped Mach-O plugin binary.
        (d / "AU/Gain.component/Contents/MacOS").mkdir(parents=True)
        (d / "AU/Gain.component/Contents/MacOS/Gain").write_bytes(
            b"\xcf\xfa\xed\xfe" + b"\x00" * 64)
        # An intermediate static archive — must NOT be measured.
        (d / "CMakeFiles").mkdir()
        (d / "CMakeFiles/libfoo.a").write_bytes(b"!<arch>\n" + b"\x00" * 64)
        # A vendored dependency binary under _deps — must be skipped.
        (d / "_deps/dep-build").mkdir(parents=True)
        (d / "_deps/dep-build/dep").write_bytes(b"\xcf\xfa\xed\xfe" + b"\x00" * 64)
        # A plain text file — not a binary.
        (d / "notes.txt").write_text("hello")
        return d

    def test_discovers_only_shipped_binaries(self):
        d = self._tree()
        arts = MOD.discover_artifacts(d)
        names = {p.name for p in arts}
        self.assertIn("Gain", names)
        self.assertNotIn("libfoo.a", names)   # ar archive excluded
        self.assertNotIn("dep", names)         # _deps excluded
        self.assertNotIn("notes.txt", names)   # non-binary excluded


class RepoFloor(unittest.TestCase):
    def test_floor_is_max_over_artifacts(self):
        from unittest import mock
        paths = [pathlib.Path("/a"), pathlib.Path("/b"), pathlib.Path("/c")]

        def fake(p):
            return {"/a": ("macho", "11.0"),
                    "/b": ("macho", "13.3"),
                    "/c": ("macho", None)}[str(p)]

        with mock.patch.object(MOD, "measure_artifact", side_effect=fake):
            floor, details = MOD.repo_floor(paths)
        self.assertEqual(floor, "13.3")           # max(11.0, 13.3)
        self.assertEqual(len(details), 3)

    def test_floor_none_when_nothing_measurable(self):
        from unittest import mock
        with mock.patch.object(MOD, "measure_artifact",
                               side_effect=lambda p: (None, None)):
            floor, details = MOD.repo_floor([pathlib.Path("/x")])
        self.assertIsNone(floor)


class SdkFloor(unittest.TestCase):
    def test_reads_shipped_min_os_json(self):
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        pkg = d / "lib/cmake/Pulp"
        pkg.mkdir(parents=True)
        (pkg / "min_os.json").write_text(
            '{"platforms": {"macos-arm64": {"floor": "13.3"},'
            ' "linux-x64": {"floor": "2.34"},'
            ' "windows-x64": {"floor": "10.0"}}}')
        expected = {"darwin": "13.3"}.get(
            sys.platform, "2.34" if sys.platform.startswith("linux") else "10.0")
        self.assertEqual(MOD.sdk_floor(d), expected)


class Report(unittest.TestCase):
    def test_match_and_drift_are_flagged(self):
        results = [
            {"name": "ok-repo", "clone": True, "configure": True, "build": True,
             "floor": "13.3", "artifacts": [], "notes": []},
            {"name": "drift-repo", "clone": True, "configure": True, "build": True,
             "floor": "26.0", "artifacts": [], "notes": []},
            {"name": "broke-repo", "clone": True, "configure": True, "build": False,
             "floor": None, "artifacts": [], "notes": ["build failed"]},
        ]
        skipped = [MOD.Plan(repo="o/mirror", name="mirror", skip_reason="README-only")]
        txt = MOD.format_report(results, skipped, sdk="13.3")
        self.assertIn("match", txt)
        self.assertIn("DRIFT", txt)
        self.assertIn("build✗", txt)
        self.assertIn("mirror", txt)
        self.assertIn("README-only", txt)


class BoundedJobs(unittest.TestCase):
    def test_explicit_wins(self):
        self.assertEqual(MOD.bounded_jobs(4), 4)

    def test_default_is_capped(self):
        self.assertLessEqual(MOD.bounded_jobs(None), 8)
        self.assertGreaterEqual(MOD.bounded_jobs(None), 1)


if __name__ == "__main__":
    unittest.main()
