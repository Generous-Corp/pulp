#!/usr/bin/env python3
"""Tests for tools/scripts/sdk_consumer_update.py pure logic.

The clone / branch / push / gh-pr side effects need network + repo access and are
exercised by running the tool; here we lock down the pin detection + rewrite, the
per-repo update planning, the buildable-consumer filter, and the publish runbook
— all without touching the network.

Run:
    python3 tools/scripts/test_sdk_consumer_update.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/scripts/sdk_consumer_update.py"


def _load():
    spec = importlib.util.spec_from_file_location("sdk_consumer_update", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["sdk_consumer_update"] = mod
    spec.loader.exec_module(mod)
    return mod


MOD = _load()


class DetectAndRewrite(unittest.TestCase):
    def test_pulp_toml(self):
        text = 'name = "demo"\nsdk_version = "0.638.1"\n'
        self.assertEqual(MOD.detect_pins(text, "pulp.toml"),
                         [("pulp.toml sdk_version", "0.638.1")])
        out, ch = MOD.rewrite_pins(text, "pulp.toml", "0.640.0")
        self.assertIn('sdk_version = "0.640.0"', out)
        self.assertEqual(ch, [("pulp.toml sdk_version", "0.638.1", "0.640.0")])

    def test_cmake_find_package_and_fetchcontent(self):
        text = ("find_package(Pulp 0.638.1 REQUIRED CONFIG)\n"
                "FetchContent_Declare(pulp GIT_TAG v0.638.1)\n")
        self.assertEqual(
            MOD.detect_pins(text, "CMakeLists.txt"),
            [("find_package(Pulp)", "0.638.1"), ("FetchContent GIT_TAG", "0.638.1")])
        out, ch = MOD.rewrite_pins(text, "CMakeLists.txt", "0.640.0")
        self.assertIn("find_package(Pulp 0.640.0 REQUIRED CONFIG)", out)
        self.assertIn("GIT_TAG v0.640.0", out)  # the v prefix is preserved
        self.assertEqual(len(ch), 2)

    def test_floating_latest_is_untouched(self):
        text = 'sdk_version = "latest"\n'
        out, ch = MOD.rewrite_pins(text, "pulp.toml", "0.640.0")
        self.assertEqual(out, text)
        self.assertEqual(ch, [])

    def test_already_at_target_is_no_change(self):
        text = 'sdk_version = "0.640.0"\n'
        out, ch = MOD.rewrite_pins(text, "pulp.toml", "0.640.0")
        self.assertEqual(out, text)   # text is unchanged
        self.assertEqual(ch, [])       # and no change is reported

    def test_non_semver_target_rejected(self):
        with self.assertRaises(ValueError):
            MOD.rewrite_pins('sdk_version = "0.1.0"\n', "pulp.toml", "latest")

    def test_wrong_file_no_match(self):
        # A find_package pin only matches in CMakeLists.txt, not pulp.toml.
        text = "find_package(Pulp 0.638.1)\n"
        self.assertEqual(MOD.detect_pins(text, "pulp.toml"), [])


class RepoPlanApply(unittest.TestCase):
    def _checkout(self) -> pathlib.Path:
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        (d / "pulp.toml").write_text('sdk_version = "0.638.1"\n')
        (d / "CMakeLists.txt").write_text("find_package(Pulp 0.638.1 CONFIG)\n")
        return d

    def test_plan_reports_changes_without_writing(self):
        d = self._checkout()
        plan = MOD.plan_repo_update(d, "0.640.0")
        self.assertIn("pulp.toml", plan)
        self.assertIn("CMakeLists.txt", plan)
        self.assertTrue(plan["pulp.toml"]["changes"])
        # Nothing was written.
        self.assertIn("0.638.1", (d / "pulp.toml").read_text())

    def test_apply_writes_both_files(self):
        d = self._checkout()
        changed = MOD.apply_repo_update(d, "0.640.0")
        self.assertEqual(set(changed), {"pulp.toml", "CMakeLists.txt"})
        self.assertIn("0.640.0", (d / "pulp.toml").read_text())
        self.assertIn("find_package(Pulp 0.640.0", (d / "CMakeLists.txt").read_text())

    def test_apply_noop_when_already_current(self):
        d = self._checkout()
        MOD.apply_repo_update(d, "0.640.0")            # first bump
        again = MOD.apply_repo_update(d, "0.640.0")    # already there
        self.assertEqual(again, [])

    def test_plan_empty_for_no_pin(self):
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        (d / "pulp.toml").write_text('sdk_version = "latest"\n')  # floating, no concrete pin
        plan = MOD.plan_repo_update(d, "0.640.0")
        # 'latest' is detected as present-but-not-a-semver: no changes.
        self.assertFalse(any(v["changes"] for v in plan.values()))


class BuildableFilter(unittest.TestCase):
    CONSUMERS = {"repos": [
        {"repo": "o/pulp-example-plugins", "status": {"state": "merged"}},
        {"repo": "o/pulp-spectral-lab", "status": {"state": "not-applicable"}},
        {"repo": "o/pulp-gpu-nam", "status": {"state": "build-pass"}},
    ]}

    def test_excludes_not_applicable_mirrors(self):
        names = [MOD.short_name(e["repo"])
                 for e in MOD.buildable_consumers(self.CONSUMERS, None)]
        self.assertIn("pulp-example-plugins", names)
        self.assertIn("pulp-gpu-nam", names)
        self.assertNotIn("pulp-spectral-lab", names)

    def test_only_filter(self):
        names = [MOD.short_name(e["repo"])
                 for e in MOD.buildable_consumers(self.CONSUMERS, {"pulp-gpu-nam"})]
        self.assertEqual(names, ["pulp-gpu-nam"])


class PublishRunbook(unittest.TestCase):
    CONSUMERS = {"repos": [
        {"repo": "o/pulp-gpu-nam",
         "latest_release": {"tag": "v1.2.4", "package_assets": ["GpuNam-1.2.4.pkg"]},
         "status": {"state": "build-pass"}},
        {"repo": "o/pulp-spectral-lab",
         "latest_release": {"tag": "v1.0.2", "package_assets": ["SpectralLab-1.0.2.pkg"]},
         "status": {"state": "not-applicable"}},
        {"repo": "o/pulp-view-embed",
         "latest_release": {"tag": "v0.1.0", "package_assets": []},
         "status": {"state": "build-pass"}},
    ]}

    def test_lists_packaged_repos_only(self):
        out = MOD.publish_runbook(self.CONSUMERS, "0.640.0", None)
        self.assertIn("pulp-gpu-nam", out)
        self.assertIn("GpuNam-1.2.4.pkg", out)
        self.assertIn("0.640.0", out)
        # No package_assets → not in the runbook.
        self.assertNotIn("pulp-view-embed", out)

    def test_mirror_repo_flagged(self):
        out = MOD.publish_runbook(self.CONSUMERS, "0.640.0", None)
        self.assertIn("README/PKG-only release mirror", out)

    def test_no_packaged_matches(self):
        out = MOD.publish_runbook({"repos": []}, "0.640.0", None)
        self.assertIn("No packaged consumers matched", out)


class UpdateCli(unittest.TestCase):
    def _run(self, argv):
        old = sys.argv
        sys.argv = ["sdk_consumer_update.py", *argv]
        try:
            return MOD.main(argv)
        finally:
            sys.argv = old

    def test_update_rejects_non_semver(self):
        # Registry present or not, a bad --to fails fast before any work.
        with mock.patch.object(MOD, "load_yaml", return_value={"repos": []}), \
             mock.patch.object(pathlib.Path, "exists", return_value=True):
            self.assertEqual(self._run(["update", "--to", "latest"]), 2)

    def test_update_dryrun_reports_without_opening_prs(self):
        # Mock clone to a prebuilt checkout so no network is touched, and assert
        # the dry-run path never calls open_update_pr.
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        (d / "pulp-gpu-nam").mkdir()
        (d / "pulp-gpu-nam" / "pulp.toml").write_text('sdk_version = "0.638.1"\n')
        consumers = {"repos": [{"repo": "o/pulp-gpu-nam",
                                "status": {"state": "build-pass"}}]}

        def fake_clone(repo, dest):
            return True, "cloned"

        args = mock.Mock(to="0.640.0", only=None, open_prs=False, workdir=d)
        with mock.patch.object(MOD, "clone", side_effect=fake_clone), \
             mock.patch.object(MOD, "open_update_pr") as opened:
            rc = MOD.cmd_update(args, consumers)
        self.assertEqual(rc, 0)
        opened.assert_not_called()          # dry-run must not open a PR
        # And the dry-run left the pin untouched on disk.
        self.assertIn("0.638.1", (d / "pulp-gpu-nam" / "pulp.toml").read_text())


if __name__ == "__main__":
    unittest.main()
