#!/usr/bin/env python3
"""Tests for compose_release_notes.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock
from pathlib import Path

import compose_release_notes as crn


SCRIPT = Path(__file__).with_name("compose_release_notes.py")


def git(cwd: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def commit(cwd: Path, subject: str, body: str = "") -> None:
    path = cwd / "file.txt"
    path.write_text(path.read_text() + subject + "\n")
    git(cwd, "add", "file.txt")
    if body:
        git(cwd, "commit", "-q", "-m", subject, "-m", body)
    else:
        git(cwd, "commit", "-q", "-m", subject)


def merge_pr(cwd: Path, branch: str, pr_number: int, title: str) -> None:
    git(cwd, "checkout", "-q", "main")
    git(
        cwd,
        "merge",
        "--no-ff",
        branch,
        "-m",
        f"Merge pull request #{pr_number} from example/{branch}",
        "-m",
        title,
    )


class ComposeReleaseNotesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmpdir.name)
        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "config", "user.email", "test@example.com")
        git(self.repo, "config", "user.name", "Test User")
        (self.repo / "file.txt").write_text("initial\n")
        git(self.repo, "add", "file.txt")
        git(self.repo, "commit", "-q", "-m", "chore: initial")
        git(self.repo, "tag", "v0.1.0")

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def compose(self, tag: str) -> str:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                tag,
                "--repo-url",
                "https://github.com/example/repo",
            ],
            cwd=self.repo,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout

    def test_groups_pull_request_titles_and_uses_release_note_override(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "feature")
        commit(
            self.repo,
            "feat(ui): add mixer",
            'Release-Note: Add mixer controls to the editor.',
        )
        merge_pr(self.repo, "feature", 12, "feat(ui): add mixer")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("## Highlights", notes)
        self.assertIn("### ✨ Features", notes)
        self.assertIn(
            "- Add mixer controls to the editor. "
            "([#12](https://github.com/example/repo/pull/12))",
            notes,
        )
        self.assertNotIn("- feat(ui): add mixer", notes)

    def test_breaking_entries_render_before_highlights_and_skip_duplicate_grouping(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "breaking")
        commit(
            self.repo,
            "fix!: remove legacy preset path",
            textwrap.dedent(
                """\
                Presets now resolve from the project store.

                BREAKING CHANGE: Move custom presets into the project store.
                Release-Note: Move custom presets into the project store.
                """
            ),
        )
        merge_pr(self.repo, "breaking", 13, "fix!: remove legacy preset path")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertLess(notes.index("## ⚠️ Breaking Changes"), notes.index("## Highlights"))
        self.assertIn(
            "- Move custom presets into the project store. "
            "([#13](https://github.com/example/repo/pull/13))",
            notes,
        )
        self.assertNotIn("### 🐛 Fixes", notes)

    def test_chore_entries_are_folded_and_bump_commits_are_skipped(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "ci")
        commit(self.repo, "ci: retry release upload")
        commit(self.repo, "chore: bump versions")
        merge_pr(self.repo, "ci", 14, "ci: retry release upload")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("<details><summary>🔧 Chore & CI</summary>", notes)
        self.assertIn("- ci: retry release upload", notes)
        self.assertNotIn("chore: bump versions", notes)

    def test_post_tag_changelog_regen_commits_are_skipped(self) -> None:
        commit(self.repo, "docs: regenerate changelog for v0.1.0 [skip ci]")
        git(self.repo, "checkout", "-q", "-b", "feature-after-regen")
        commit(self.repo, "fix(ui): keep toolbar visible")
        merge_pr(self.repo, "feature-after-regen", 15, "fix(ui): keep toolbar visible")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("### 🐛 Fixes", notes)
        self.assertIn("- fix(ui): keep toolbar visible", notes)
        self.assertNotIn("regenerate changelog", notes)

    def test_squash_style_subject_keeps_pr_link(self) -> None:
        commit(self.repo, "feat(api): expose transport sync (#16)")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("### ✨ Features", notes)
        self.assertIn(
            "- feat(api): expose transport sync "
            "([#16](https://github.com/example/repo/pull/16))",
            notes,
        )
        self.assertNotIn("`", notes)

    def test_github_repo_slug_and_breaking_label_detection(self) -> None:
        self.assertEqual(
            crn.github_repo_from_url("https://github.com/example/repo"),
            "example/repo",
        )
        completed = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="bug\nbreaking\n",
            stderr="",
        )
        with mock.patch.object(crn.subprocess, "run", return_value=completed):
            self.assertTrue(crn.pr_has_breaking_label("12", "example/repo"))

    def compose_tier(self, tag: str, tier: str) -> str:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                tag,
                "--repo-url",
                "https://github.com/example/repo",
                "--tier",
                tier,
            ],
            cwd=self.repo,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout

    def test_patch_tier_emits_light_grouped_body_without_highlights_heading(self) -> None:
        # v0.1.0 -> v0.1.1 is a patch delta; auto tier must drop the Highlights H2
        # wrapper while keeping the grouped section headers + bullets.
        git(self.repo, "checkout", "-q", "-b", "fixbranch")
        commit(self.repo, "fix(audio): correct gain ramp")
        merge_pr(self.repo, "fixbranch", 21, "fix(audio): correct gain ramp")
        git(self.repo, "tag", "v0.1.1")

        notes = self.compose("v0.1.1")

        self.assertNotIn("## Highlights", notes)
        self.assertIn("### 🐛 Fixes", notes)
        self.assertIn("fix(audio): correct gain ramp", notes)

    def test_minor_tier_emits_full_highlights_heading(self) -> None:
        # v0.1.0 -> v0.2.0 is a minor delta; auto tier keeps the full shape.
        git(self.repo, "checkout", "-q", "-b", "featbranch")
        commit(self.repo, "feat(view): add scope")
        merge_pr(self.repo, "featbranch", 22, "feat(view): add scope")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("## Highlights", notes)
        self.assertIn("### ✨ Features", notes)

    def test_patch_tier_still_renders_breaking_section(self) -> None:
        # A breaking change on a patch tag must still surface the ⚠️ section first.
        git(self.repo, "checkout", "-q", "-b", "breakpatch")
        commit(
            self.repo,
            "fix!: drop legacy path",
            "BREAKING CHANGE: legacy path removed.",
        )
        merge_pr(self.repo, "breakpatch", 23, "fix!: drop legacy path")
        git(self.repo, "tag", "v0.1.1")

        notes = self.compose_tier("v0.1.1", "patch")

        self.assertIn("## ⚠️ Breaking Changes", notes)
        self.assertNotIn("## Highlights", notes)

    def test_explicit_tier_override_forces_full_body_on_patch_tag(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "ovr")
        commit(self.repo, "fix(ui): tweak spacing")
        merge_pr(self.repo, "ovr", 24, "fix(ui): tweak spacing")
        git(self.repo, "tag", "v0.1.1")

        notes = self.compose_tier("v0.1.1", "minor")

        self.assertIn("## Highlights", notes)

    def test_bump_level_classifies_semver_deltas(self) -> None:
        self.assertEqual(crn.bump_level("v0.1.1", "v0.1.0"), "patch")
        self.assertEqual(crn.bump_level("v0.2.0", "v0.1.0"), "minor")
        self.assertEqual(crn.bump_level("v1.0.0", "v0.9.3"), "major")
        # no previous tag (first-ever release) => unknown (falls back to full body)
        self.assertEqual(crn.bump_level("v0.1.0", None), "unknown")
        # non-semver tag => unknown
        self.assertEqual(crn.bump_level("nightly-2026", "v0.1.0"), "unknown")
        # identical => unknown (no delta to classify)
        self.assertEqual(crn.bump_level("v0.1.0", "v0.1.0"), "unknown")

    def test_parse_semver_variants(self) -> None:
        self.assertEqual(crn.parse_semver("v1.2.3"), (1, 2, 3))
        self.assertEqual(crn.parse_semver("1.2.3"), (1, 2, 3))
        self.assertEqual(crn.parse_semver("v1.2.3-rc1"), (1, 2, 3))
        self.assertIsNone(crn.parse_semver("nightly"))
        self.assertIsNone(crn.parse_semver(None))


if __name__ == "__main__":
    unittest.main(verbosity=2)
