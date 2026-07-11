#!/usr/bin/env python3
"""Fixture tests for the PR queue release-tag report."""

from __future__ import annotations

import json
import unittest

from gate_test_support import GateFixtureTestCase, VBC, _git, _run


REPORT = VBC.with_name("pr_release_tag_report.py")


class PrReleaseTagReportTests(GateFixtureTestCase):
    def _tag_base_versions(self) -> None:
        _git(self.tmp, "-c", "tag.gpgSign=false", "tag", "v0.1.0")
        _git(self.tmp, "-c", "tag.gpgSign=false", "tag", "plugin-v0.1.0")

    def _run_report(
        self,
        output_format: str = "json",
        guard_head: str | None = None,
        pr_title: str | None = "Test PR",
    ) -> tuple[int, str]:
        cmd = [
            "python3",
            str(REPORT),
            "--repo-root",
            str(self.tmp),
            "--base",
            "origin/main",
            "--head",
            "HEAD",
            "--config",
            str(self.tmp / "tools/scripts/versioning.json"),
            "--format",
            output_format,
        ]
        if guard_head:
            cmd.extend(["--guard-head", guard_head])
        if pr_title is not None:
            cmd.extend(["--pr-title", pr_title])
        return _run(cmd, cwd=self.tmp)

    def test_reports_sdk_tag_and_plugin_noop(self) -> None:
        self._tag_base_versions()
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump versions")

        code, out = self._run_report()
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "v0.1.1")
        self.assertEqual(report["sdk"]["should_tag"], 1)
        self.assertEqual(report["plugin"]["expected_tag"], "")
        self.assertEqual(report["plugin"]["should_tag"], 0)

    def test_squash_range_release_skip_suppresses_plugin_tag(self) -> None:
        self._tag_base_versions()
        plugin = self.tmp / ".claude-plugin/plugin.json"
        data = json.loads(plugin.read_text())
        data["version"] = "0.1.1"
        plugin.write_text(json.dumps(data, indent=2) + "\n")
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "chore: bump versions",
            "-m",
            'Release: skip reason="intentional dry run"',
        )
        self.f.write("docs/follow-up.md", "Unrelated follow-up.\n")
        self.f.commit("docs: add follow-up note")

        code, out = self._run_report()
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["plugin"]["expected_tag"], "")
        self.assertIn("squash message carries Release: skip", report["plugin"]["reason"])

    def test_tip_release_skip_suppresses_earlier_bump_prediction(self) -> None:
        self._tag_base_versions()
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump versions")
        self.f.write("docs/release-note.md", "Deferred release.\n")
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "docs: defer release",
            "-m",
            'Release: skip reason="wait for coordinated launch"',
        )

        code, out = self._run_report()
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "")
        self.assertIn("squash message carries Release: skip", report["sdk"]["reason"])

    def test_embedded_squash_skip_remains_sticky_after_later_push(self) -> None:
        self._tag_base_versions()
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "Release maintenance",
            "-m",
            "* chore: bump versions\n\n"
            'Release: skip reason="coordinated release"\n\n'
            "---------\n\n"
            "Co-authored-by: Test User <test@example.com>",
        )
        self.f.write("docs/later.md", "Later unrelated push.\n")
        self.f.commit("docs: add later note")

        code, out = self._run_report(pr_title="Document later work")
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "")
        self.assertIn("sticky skip", report["sdk"]["reason"])

    def test_synthetic_merge_uses_pr_tip_for_guard_prediction(self) -> None:
        self._tag_base_versions()
        _git(self.tmp, "checkout", "-q", "-b", "topic")
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "chore: bump versions",
            "-m",
            'Version-Bump: skip reason="coordinated release"',
        )
        _git(self.tmp, "checkout", "-q", "-b", "synthetic", "origin/main")
        _git(self.tmp, "merge", "--no-ff", "-m", "Merge topic", "topic")

        code, out = self._run_report(guard_head="topic")
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "")
        self.assertIn("Version-Bump: skip", report["sdk"]["reason"])

    def test_one_commit_uses_commit_subject_not_revert_pr_title(self) -> None:
        self._tag_base_versions()
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("fix: release the repair")

        code, out = self._run_report(pr_title='Revert "release the repair"')
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "v0.1.1")

    def test_multi_commit_uses_revert_pr_title_not_tip_subject(self) -> None:
        self._tag_base_versions()
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("fix: release the repair")
        self.f.write("docs/follow-up.md", "Follow-up.\n")
        self.f.commit("docs: explain the repair")

        code, out = self._run_report(pr_title='Revert "release the repair"')
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "")
        self.assertIn("squash subject is a revert", report["sdk"]["reason"])

    def test_multi_commit_ignores_revert_tip_when_pr_title_is_not_revert(self) -> None:
        self._tag_base_versions()
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("fix: release the repair")
        self.f.write("docs/follow-up.md", "Follow-up.\n")
        self.f.commit('Revert "defer release"')

        code, out = self._run_report(pr_title="fix: restore the release")
        self.assertEqual(code, 0, msg=out)
        report = {item["surface"]: item for item in json.loads(out)}
        self.assertEqual(report["sdk"]["expected_tag"], "v0.1.1")

    def test_markdown_labels_prediction_as_post_merge(self) -> None:
        self._tag_base_versions()
        code, out = self._run_report("markdown")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("## Expected release tags", out)
        self.assertIn("Queue prediction", out)
        self.assertIn("Actual signed tags are created after merge", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
