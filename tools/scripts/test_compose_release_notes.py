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

        self.assertIn("### ✨ New", notes)
        self.assertNotIn("## Highlights", notes)  # no wrapper heading anymore
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

        self.assertIn("### ⚠️ Breaking changes", notes)
        self.assertIn(
            "- Move custom presets into the project store. "
            "([#13](https://github.com/example/repo/pull/13))",
            notes,
        )
        # A breaking fix! lives in the breaking section, not under Fixed.
        self.assertNotIn("### 🐛 Fixed", notes)

    def test_chore_entries_are_folded_and_bump_commits_are_skipped(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "ci")
        commit(self.repo, "ci: retry release upload")
        commit(self.repo, "chore: bump versions")
        merge_pr(self.repo, "ci", 14, "ci: retry release upload")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("<details><summary>🔧 Under the hood</summary>", notes)
        self.assertIn("- Retry release upload", notes)  # jargon-stripped
        self.assertNotIn("- ci: retry release upload", notes)
        self.assertNotIn("chore: bump versions", notes)

    def test_post_tag_changelog_regen_commits_are_skipped(self) -> None:
        commit(self.repo, "docs: regenerate changelog for v0.1.0 [skip ci]")
        git(self.repo, "checkout", "-q", "-b", "feature-after-regen")
        commit(self.repo, "fix(ui): keep toolbar visible")
        merge_pr(self.repo, "feature-after-regen", 15, "fix(ui): keep toolbar visible")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("### 🐛 Fixed", notes)
        self.assertIn("- Keep toolbar visible", notes)  # jargon-stripped
        self.assertNotIn("- fix(ui): keep toolbar visible", notes)
        self.assertNotIn("regenerate changelog", notes)

    def test_squash_style_subject_keeps_pr_link(self) -> None:
        commit(self.repo, "feat(api): expose transport sync (#16)")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("### ✨ New", notes)
        self.assertIn(
            "- Expose transport sync "
            "([#16](https://github.com/example/repo/pull/16))",
            notes,
        )
        self.assertNotIn("feat(api):", notes)  # prefix stripped
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

    def test_patch_tier_emits_light_body_and_strips_jargon(self) -> None:
        # v0.1.0 -> v0.1.1 is a patch delta; auto tier keeps only the user-facing
        # sections (no folded "Under the hood") and strips the commit prefix.
        git(self.repo, "checkout", "-q", "-b", "fixbranch")
        commit(self.repo, "fix(audio): correct gain ramp")
        merge_pr(self.repo, "fixbranch", 21, "fix(audio): correct gain ramp")
        git(self.repo, "tag", "v0.1.1")

        notes = self.compose("v0.1.1")

        self.assertNotIn("## Highlights", notes)
        self.assertIn("### 🐛 Fixed", notes)
        self.assertIn("- Correct gain ramp", notes)  # jargon-stripped
        self.assertNotIn("fix(audio):", notes)

    def test_minor_tier_emits_user_facing_section(self) -> None:
        # v0.1.0 -> v0.2.0 is a minor delta; auto tier keeps the full shape.
        git(self.repo, "checkout", "-q", "-b", "featbranch")
        commit(self.repo, "feat(view): add scope")
        merge_pr(self.repo, "featbranch", 22, "feat(view): add scope")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        self.assertIn("### ✨ New", notes)
        self.assertIn("- Add scope", notes)

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

        self.assertIn("### ⚠️ Breaking changes", notes)
        self.assertNotIn("## Highlights", notes)

    def test_explicit_tier_override_forces_under_the_hood_fold_on_patch_tag(self) -> None:
        # A patch tag rendered as `minor` keeps the folded "Under the hood"
        # section (patch would have dropped it).
        git(self.repo, "checkout", "-q", "-b", "ovr")
        commit(self.repo, "fix(ui): tweak spacing")
        merge_pr(self.repo, "ovr", 24, "fix(ui): tweak spacing")
        git(self.repo, "checkout", "-q", "main")
        git(self.repo, "checkout", "-q", "-b", "ovr-chore")
        commit(self.repo, "ci: bump runner image")
        merge_pr(self.repo, "ovr-chore", 25, "ci: bump runner image")
        git(self.repo, "tag", "v0.1.1")

        notes = self.compose_tier("v0.1.1", "minor")

        self.assertIn("### 🐛 Fixed", notes)
        self.assertIn("<details><summary>🔧 Under the hood</summary>", notes)

    def test_repeated_titles_coalesce_into_one_line(self) -> None:
        # Many near-identical mechanical PRs fold into a single line.
        for i, n in enumerate((30, 31, 32, 33)):
            git(self.repo, "checkout", "-q", "main")
            git(self.repo, "checkout", "-q", "-b", f"reg{n}")
            commit(self.repo, f"refactor: split WidgetBridge registrar part {i}")
            merge_pr(self.repo, f"reg{n}", n, f"refactor: split WidgetBridge registrar part {i}")
        git(self.repo, "tag", "v0.2.0")

        notes = self.compose("v0.2.0")

        # One coalesced line, not four; and it names the shared lead phrase.
        self.assertIn("— and 3 more", notes)
        self.assertIn("Split WidgetBridge", notes)
        self.assertEqual(notes.count("Split WidgetBridge"), 1)

    def test_humanize_strips_prefix_and_pr_number(self) -> None:
        self.assertEqual(crn.humanize("feat(view): add scope"), "Add scope")
        self.assertEqual(crn.humanize("fix!: drop legacy path"), "Drop legacy path")
        self.assertEqual(crn.humanize("refactor: split bridge (#123)"), "Split bridge")
        # An already-human Release-Note line is left essentially intact.
        self.assertEqual(
            crn.humanize("Add mixer controls to the editor."),
            "Add mixer controls to the editor.",
        )

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
