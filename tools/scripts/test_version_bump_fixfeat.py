#!/usr/bin/env python3
"""Fixture tests for the version_bump --require-bump-for-fix-feat check.

The PR-title/commit-subject fix/feat-needs-bump guard. These exercise the
helpers (`_is_fix_or_feat_title`, `_range_has_bump_commit`,
`_range_fix_feat_subjects`,
`_range_has_version_bump_skip_trailer`, `check_fix_feat_requires_bump`)
that remain in `version_bump_check.py` alongside `main()`.

Runs standalone (`python3 tools/scripts/test_version_bump_fixfeat.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

import json
import os
import subprocess
import unittest

from gate_test_support import GateFixtureTestCase, VBC, _run, _git


class VersionBumpFixFeatTests(GateFixtureTestCase):
    """--require-bump-for-fix-feat (issue #1009) fixtures."""

    def _run_vbc_fixfeat(
        self,
        title: str | None,
        extra: list[str] | None = None,
    ) -> tuple[int, str]:
        """Helper: invoke version_bump_check.py with --require-bump-for-fix-feat
        and an explicit PR title (or omitted to test the env var path)."""
        cmd = [
            "python3", str(VBC),
            "--base", "origin/main",
            "--config", str(self.tmp / "tools/scripts/versioning.json"),
            "--mode=report",
            "--require-bump-for-fix-feat",
            *(extra or []),
        ]
        if title is not None:
            cmd.extend(["--pr-title", title])
        return _run(cmd, cwd=self.tmp)

    def test_fixfeat_with_bump_commit_passes(self) -> None:
        """A `fix:` PR that includes a `chore: bump versions` commit in
        the diff range should pass the fix/feat-needs-bump check."""
        # Touch a real source file so the diff range is non-empty.
        self.f.write(
            "core/runtime/include/pulp/runtime/foo.hpp",
            "#pragma once\nint foo();\nint bar();\n",
        )
        self.f.commit("fix(runtime): wire bar() into foo() codepath")

        # Apply a bump and create the canonical bump commit subject.
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump versions")

        code, out = self._run_vbc_fixfeat(
            "fix(view): on(id,'click',fn) auto-wires View::on_click",
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("fix/feat-needs-bump", out)
        self.assertIn("OK", out)

    def test_fixfeat_with_legacy_bump_commit_prefix_passes(self) -> None:
        """The legacy scoped bump marker remains accepted."""
        self.f.write(
            "core/runtime/include/pulp/runtime/foo.hpp",
            "#pragma once\nint foo();\nint bar();\n",
        )
        self.f.commit("fix(runtime): wire bar() into foo() codepath")

        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore(versions): bump SDK")

        code, out = self._run_vbc_fixfeat(
            "fix(cli): refresh explicit upgrade discovery",
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("fix/feat-needs-bump", out)
        self.assertIn("OK", out)

    def test_fixfeat_with_near_miss_bump_subject_fails(self) -> None:
        """Only the precise bump-marker subjects count.

        `chore: bump SDK to vX.Y.Z` sounds reasonable to a human, but it
        is not the release-guard marker and must fail loudly.
        """
        self.f.write(
            "core/runtime/include/pulp/runtime/foo.hpp",
            "#pragma once\nint foo();\nint bar();\n",
        )
        self.f.commit("fix(runtime): wire bar() into foo() codepath")

        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump SDK to v0.1.1")

        code, out = self._run_vbc_fixfeat(
            "fix(cli): refresh explicit upgrade discovery",
        )
        self.assertEqual(code, 1, msg=out)
        self.assertIn("NO commit with subject `chore: bump versions`", out)
        self.assertIn("chore: bump SDK to vX.Y.Z", out)

    def test_fixfeat_without_bump_commit_fails(self) -> None:
        """A `fix:` PR that lacks a bump commit AND lacks a skip trailer
        must fail the fix/feat-needs-bump check. This is the structural
        fix for issue #1009 — PR #1008 merged in this exact state."""
        # Source change that wouldn't otherwise demand a minor bump
        # (internal-only); the per-surface verdict is patch-suggested
        # which is advisory, so the only thing failing here is the new
        # fix/feat check.
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 2; }\n",
        )
        self.f.commit("fix(runtime): off-by-one in foo()")

        code, out = self._run_vbc_fixfeat(
            "fix(view): on(id,'click',fn) auto-wires View::on_click",
        )
        self.assertEqual(code, 1, msg=out)
        self.assertIn("fix/feat-needs-bump", out)
        self.assertIn("NO commit with subject `chore: bump versions`", out)
        # Suggestion text must include the trailer escape hatch.
        self.assertIn("Version-Bump: skip", out)
        self.assertIn("issue #1009", out)

    def test_plain_pr_title_cannot_hide_fix_commit_subject(self) -> None:
        """The gate must classify the subject GitHub can actually squash.

        A one-commit PR can have a plain-language PR title while the repository's
        ``COMMIT_OR_PR_TITLE`` policy lands the commit's ``fix:`` subject. A
        scoped surface skip still must not bypass the global release guard.
        """
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 9; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "fix(runtime): clear transient worker latch",
            "-m",
            'Version-Bump: sdk=skip reason="internal idle-state repair"',
        )

        code, out = self._run_vbc_fixfeat(
            "Fix worker-pool reheat latch after gate collision",
        )
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )
        self.assertIn("NO commit with subject `chore: bump versions`", out)

    def test_plain_squash_subject_recovers_embedded_fix_signal(self) -> None:
        """COMMIT_MESSAGES source subjects remain visible after squash."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 91; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "Repair worker latch (#123)",
            "-m",
            "* fix(runtime): clear transient worker latch\n\n"
            "Implementation details.\n\n"
            "---------\n\n"
            "Co-authored-by: Test User <test@example.com>",
        )

        code, out = self._run_vbc_fixfeat("Repair worker latch")
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "0",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        result = json.loads(out)
        self.assertEqual(result["surfaces"], ["sdk"])
        self.assertEqual(result["levels"], {"sdk": "patch"})

        _git(self.tmp, "revert", "--no-edit", "HEAD")
        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "0",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 1, msg=out)

    def test_squash_source_range_preserves_signal_file_attribution(self) -> None:
        """A plugin fix cannot inherit an unrelated SDK source commit's files."""
        _git(self.tmp, "checkout", "-q", "-b", "source-pr")
        plugin = self.tmp / ".claude-plugin/plugin.json"
        plugin_data = json.loads(plugin.read_text())
        plugin_data["description"] = "Repair command metadata"
        plugin.write_text(json.dumps(plugin_data, indent=2) + "\n")
        self.f.commit("fix(plugin): repair command metadata")
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 92; }\n",
        )
        self.f.commit("chore(runtime): unrelated maintenance")

        _git(self.tmp, "checkout", "-q", "main")
        _git(self.tmp, "merge", "--squash", "source-pr")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "Maintenance bundle (#124)",
            "-m",
            "* fix(plugin): repair command metadata\n\n"
            "* chore(runtime): unrelated maintenance\n\n"
            "---------\n\n"
            "Co-authored-by: Test User <test@example.com>",
        )

        command = [
            "python3",
            str(VBC),
            "classify-unreleased-range",
            "origin/main",
            "HEAD",
            "0",
            "0",
            str(self.tmp / "tools/scripts/versioning.json"),
            "-",
            "-",
            "origin/main",
            "source-pr",
        ]
        code, out = _run(command, cwd=self.tmp)
        self.assertEqual(code, 0, msg=out)
        result = json.loads(out)
        self.assertEqual(result["surfaces"], ["plugin"])

        command[6] = "1"
        code, out = _run(command, cwd=self.tmp)
        self.assertEqual(code, 1, msg=out)

    def test_plain_pr_title_with_fix_subject_and_bump_passes(self) -> None:
        """A commit-derived release signal accepts the normal bump marker."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 10; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")

        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump versions")

        code, out = self._run_vbc_fixfeat(
            "Fix worker-pool reheat latch after gate collision",
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )
        self.assertIn("found `chore: bump versions`", out)

    def test_classify_range_subcommand_reports_live_signal(self) -> None:
        """The post-merge backstop can reuse the range classifier."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 101; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-range",
                "origin/main",
                "HEAD",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertEqual(out.strip(), "fix(runtime): clear transient worker latch")

    def test_classify_range_honors_global_skip_anywhere_in_range(self) -> None:
        """The post-merge detector shares the PR gate's range-wide opt-out."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 102; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "fix(runtime): clear transient worker latch",
            "-m",
            'Version-Bump: skip reason="no SDK consumer impact"',
        )
        self.f.write("docs/follow-up.md", "Follow-up.\n")
        self.f.commit("docs: add follow-up")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-range",
                "origin/main",
                "HEAD",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 1, msg=out)

    def test_unreleased_range_tracks_sdk_when_plugin_will_tag(self) -> None:
        """A plugin release cannot cover an SDK fix in the same push."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 103; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "1",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        result = json.loads(out)
        self.assertEqual(result["subject"], "fix(runtime): clear transient worker latch")
        self.assertEqual(result["surfaces"], ["sdk"])

    def test_unreleased_public_header_fix_records_minor_heuristic(self) -> None:
        """Recovery cannot downgrade a public API diff to subject-level patch."""
        self.f.write(
            "core/runtime/include/pulp/runtime/foo.hpp",
            "#pragma once\nint foo();\nint worker_state();\n",
        )
        self.f.commit("fix(runtime): expose corrected worker state")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "0",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertEqual(json.loads(out)["levels"], {"sdk": "minor"})

    def test_unreleased_range_maps_fix_merge_subject_to_net_paths(self) -> None:
        """A merge-title signal inherits only the merge's introduced paths."""
        _git(self.tmp, "checkout", "-q", "-b", "topic")
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 104; }\n",
        )
        self.f.commit("refactor(runtime): adjust worker latch")
        _git(self.tmp, "checkout", "-q", "main")
        _git(
            self.tmp,
            "merge",
            "--no-ff",
            "-m",
            "fix(runtime): merge worker latch repair",
            "topic",
        )
        plugin = self.tmp / ".claude-plugin/plugin.json"
        plugin_data = json.loads(plugin.read_text())
        plugin_data["description"] = "Unrelated metadata follow-up"
        plugin.write_text(json.dumps(plugin_data, indent=2) + "\n")
        self.f.commit("chore(plugin): update unrelated metadata")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "0",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        result = json.loads(out)
        self.assertEqual(result["subject"], "fix(runtime): merge worker latch repair")
        self.assertEqual(result["surfaces"], ["sdk"])

    def test_sticky_skip_boundary_covers_only_earlier_sdk_fix(self) -> None:
        """A skipped bump covers earlier work but not a later fix."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 204; }\n",
        )
        self.f.commit("fix(runtime): repair worker latch")
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
            'Release: skip reason="coordinated release"',
        )
        bump_sha = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.f.write("docs/follow-up.md", "Follow-up.\n")
        self.f.commit("docs: add follow-up")

        common = [
            "python3",
            str(VBC),
            "classify-unreleased-range",
            "origin/main",
            "HEAD",
            "0",
            "0",
            str(self.tmp / "tools/scripts/versioning.json"),
            bump_sha,
            "-",
        ]
        code, out = _run(common, cwd=self.tmp)
        self.assertEqual(code, 1, msg=out)

        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 205; }\n",
        )
        self.f.commit("fix(runtime): repair later worker race")
        code, out = _run(common, cwd=self.tmp)
        self.assertEqual(code, 0, msg=out)
        result = json.loads(out)
        self.assertEqual(result["subject"], "fix(runtime): repair later worker race")
        self.assertEqual(result["surfaces"], ["sdk"])
        self.assertEqual(result["levels"], {"sdk": "patch"})

        later_fix_sha = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.f.write("docs/after-fix.md", "Another unrelated push.\n")
        self.f.commit("docs: add another follow-up")
        later_push = list(common)
        later_push[3] = later_fix_sha
        code, out = _run(later_push, cwd=self.tmp)
        self.assertEqual(code, 1, msg=out)

    def test_recovery_level_ignores_numeric_override_before_boundary(self) -> None:
        """A covered patch override cannot lower a later uncovered feature."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 214; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "feat(runtime): add covered worker mode",
            "-m",
            "Version-Bump: sdk=patch",
        )
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
            'Release: skip reason="coordinated release"',
        )
        bump_sha = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 215; }\n",
        )
        self.f.commit("feat(runtime): add uncovered worker mode")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "0",
                str(self.tmp / "tools/scripts/versioning.json"),
                bump_sha,
                "-",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertEqual(json.loads(out)["levels"], {"sdk": "minor"})

    def test_unreleased_range_unions_surfaces_across_live_signals(self) -> None:
        """One tracker recovery command covers every stranded surface."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 105; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        plugin = self.tmp / ".claude-plugin/plugin.json"
        plugin_data = json.loads(plugin.read_text())
        plugin_data["description"] = "Repair command metadata"
        plugin.write_text(json.dumps(plugin_data, indent=2) + "\n")
        self.f.commit("fix(plugin): repair command metadata")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "classify-unreleased-range",
                "origin/main",
                "HEAD",
                "0",
                "0",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertEqual(json.loads(out)["surfaces"], ["sdk", "plugin"])

    def test_unresolved_revert_of_fails_closed_for_fix_subject(self) -> None:
        """Stale revert metadata cannot suppress a real fix signal."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 11; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "fix(runtime): undo transient worker latch",
            "-m",
            "Revert-Of: deadbeef",
        )

        code, out = self._run_vbc_fixfeat("Undo transient worker latch")
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): undo transient worker latch'",
            out,
        )

    def test_revert_of_trailer_cancels_its_target_signal(self) -> None:
        """A valid Revert-Of trailer cancels the target, not only itself."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 111; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        fix_sha = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        _git(self.tmp, "revert", "--no-commit", fix_sha)
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "chore(runtime): undo transient worker latch",
            "-m",
            f"Revert-Of: {fix_sha}",
        )

        code, out = self._run_vbc_fixfeat("Undo transient worker latch")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump required", out)

    def test_later_revert_cancels_fix_commit_signal(self) -> None:
        """A fully reverted fix has no net user-facing release signal."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 12; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        _git(self.tmp, "revert", "--no-edit", "HEAD")

        code, out = self._run_vbc_fixfeat("Undo transient worker latch")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump required", out)

    def test_later_same_path_edit_does_not_revive_reverted_fix(self) -> None:
        """A later edit to the same file is not evidence the fix survived."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 120; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        _git(self.tmp, "revert", "--no-edit", "HEAD")

        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 1; }\n// Document the fallback state.\n",
        )
        self.f.commit("docs(runtime): explain fallback state")

        code, out = self._run_vbc_fixfeat("Document worker fallback state")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump required", out)

    def test_later_rename_preserves_live_fix_commit_signal(self) -> None:
        """Renaming a fixed file must not hide the commit's release signal."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 121; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        _git(
            self.tmp,
            "mv",
            "core/runtime/src/foo.cpp",
            "core/runtime/src/worker.cpp",
        )
        self.f.commit("refactor(runtime): rename worker implementation")

        code, out = self._run_vbc_fixfeat("Rename worker implementation")
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )

    def test_revert_of_revert_restores_fix_commit_signal(self) -> None:
        """Reapplying a reverted fix makes its release signal live again."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 13; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        _git(self.tmp, "revert", "--no-edit", "HEAD")
        _git(self.tmp, "revert", "--no-edit", "HEAD")

        code, out = self._run_vbc_fixfeat("Reapply transient worker latch")
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )

    def test_reapply_restores_fix_signal_from_before_pr_range(self) -> None:
        """A cross-PR reapply carries the original fix's release signal."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 131; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")
        _git(self.tmp, "revert", "--no-edit", "HEAD")
        _git(
            self.tmp,
            "update-ref",
            "refs/remotes/origin/main",
            "HEAD",
        )

        _git(self.tmp, "revert", "--no-edit", "HEAD")

        code, out = self._run_vbc_fixfeat("Reapply transient worker latch")
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )

    def test_reverted_merge_cancels_child_fix_subject(self) -> None:
        """Reverting a merge removes its child fix from the net signal."""
        _git(self.tmp, "checkout", "-q", "-b", "topic")
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 15; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")

        _git(self.tmp, "checkout", "-q", "main")
        _git(self.tmp, "merge", "--no-ff", "-m", "Merge topic", "topic")
        _git(self.tmp, "revert", "-m", "1", "--no-edit", "HEAD")
        self.f.write("test/test_foo.cpp", "int main() { return 2; }\n")
        self.f.commit("test: retain unrelated harness update")

        code, out = self._run_vbc_fixfeat("Undo transient worker merge")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump required", out)

    def test_second_parent_merge_revert_keeps_topic_fix_signal(self) -> None:
        """Revert -m 2 removes mainline work while retaining the topic fix."""
        _git(self.tmp, "checkout", "-q", "-b", "topic")
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 16; }\n",
        )
        self.f.commit("fix(runtime): clear transient worker latch")

        _git(self.tmp, "checkout", "-q", "main")
        self.f.write("docs/mainline-note.md", "Mainline-only note.\n")
        self.f.commit("docs: add mainline-only note")
        _git(self.tmp, "merge", "--no-ff", "-m", "Merge topic", "topic")
        _git(self.tmp, "revert", "-m", "2", "--no-edit", "HEAD")

        code, out = self._run_vbc_fixfeat("Retain topic worker repair")
        self.assertEqual(code, 1, msg=out)
        self.assertIn(
            "commit subject 'fix(runtime): clear transient worker latch'",
            out,
        )

    def test_fixfeat_without_bump_but_with_skip_trailer_passes(self) -> None:
        """A `fix:` PR that lacks a bump commit but carries a top-level
        `Version-Bump: skip reason="..."` trailer on the tip commit
        is honored as an explicit bypass."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 3; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'fix(runtime): cosmetic\n\n'
             'Version-Bump: skip reason="reverted in next PR, no consumer impact"')

        code, out = self._run_vbc_fixfeat(
            "fix(runtime): cosmetic",
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("fix/feat-needs-bump", out)
        self.assertIn("bypass honored", out)

    def test_fixfeat_skip_trailer_requires_reason(self) -> None:
        """A bare `Version-Bump: skip` (no reason) must NOT bypass the
        check. Empty-reason bypasses are rejected per the documented
        bypass grammar."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 4; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             "fix(runtime): cosmetic\n\nVersion-Bump: skip")

        code, out = self._run_vbc_fixfeat("fix(runtime): cosmetic")
        self.assertEqual(code, 1, msg=out)
        self.assertIn("NO commit with subject `chore: bump versions`", out)

    def test_chore_title_does_not_require_bump(self) -> None:
        """`chore:` PRs (e.g. the catch-up bump PR itself) must not
        trigger the fix/feat check — the chore PR IS the bump."""
        # Simulate the catch-up bump itself: a chore PR that bumps
        # CMakeLists.txt with no other source touches.
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump versions [skip ci]")

        code, out = self._run_vbc_fixfeat(
            "chore: bump versions to v0.66.0 (catch-up after #1008)",
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("not a `fix:` or `feat:`", out)

    def test_docs_title_does_not_require_bump(self) -> None:
        """`docs:` / `test:` / `refactor:` titles are not user-facing
        release events; the fix/feat check must not demand a bump."""
        for prefix in ("docs", "test", "refactor", "perf", "build", "ci", "style"):
            with self.subTest(prefix=prefix):
                code, out = self._run_vbc_fixfeat(
                    f"{prefix}: tighten widget regression test",
                )
                self.assertEqual(code, 0, msg=out)
                self.assertIn("not a `fix:` or `feat:`", out)

    def test_feat_without_bump_fails_same_as_fix(self) -> None:
        """Both `fix:` and `feat:` are user-facing — feat must fail too."""
        self.f.write(
            "core/runtime/include/pulp/runtime/baz.hpp",
            "#pragma once\nint baz();\n",
        )
        self.f.commit("feat(runtime): add baz()")

        code, out = self._run_vbc_fixfeat(
            "feat(runtime): add baz()",
        )
        self.assertEqual(code, 1, msg=out)
        self.assertIn("user-facing", out)

    def test_breaking_fix_with_bang_is_treated_as_fix(self) -> None:
        """`fix!:` / `feat!:` (Conventional Commits BREAKING marker) is
        still a fix/feat — the check must apply."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 5; }\n",
        )
        self.f.commit("fix!: replace foo() return semantics")

        code, out = self._run_vbc_fixfeat(
            "fix!: replace foo() return semantics",
        )
        self.assertEqual(code, 1, msg=out)

    def test_empty_pr_title_skips_check(self) -> None:
        """Defensive: when the PR title isn't supplied (push event,
        workflow_dispatch), the check should advisory-skip rather than
        false-fail. The per-surface verdict pipeline is unaffected."""
        # Empty source change so the per-surface verdict says "none".
        # An empty title means we can't classify the PR — skip.
        code, out = self._run_vbc_fixfeat("")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("PR title not provided", out)

    def test_pr_title_via_env_var(self) -> None:
        """The flag should pick up GITHUB_PR_TITLE from the environment
        if --pr-title isn't passed. This is how version-skill-check.yml
        wires it."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 6; }\n",
        )
        self.f.commit("fix(runtime): another cosmetic")

        env = os.environ.copy()
        env["GITHUB_PR_TITLE"] = "fix(runtime): another cosmetic"
        result = subprocess.run(
            [
                "python3", str(VBC),
                "--base", "origin/main",
                "--config", str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=report",
                "--require-bump-for-fix-feat",
            ],
            cwd=self.tmp, capture_output=True, text=True, env=env,
        )
        out = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, msg=out)
        self.assertIn("user-facing", out)

    def test_fixfeat_check_does_not_run_without_flag(self) -> None:
        """Sanity: without the new flag, the existing report-mode
        behavior is unchanged — internal-only diffs that would fail
        the new check still pass the old one."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 7; }\n",
        )
        self.f.commit("fix(runtime): edge case")
        code, out = self.f.run_vbc()  # no --require-bump-for-fix-feat
        self.assertEqual(code, 0, msg=out)
        self.assertNotIn("fix/feat-needs-bump", out)

    def test_hint_mode_with_flag_never_fails(self) -> None:
        """Even when the new flag is set, --mode=hint preserves its
        "always exit 0" contract. Only report/apply hard-fail."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 8; }\n",
        )
        self.f.commit("fix(runtime): another")
        code, out = self._run_vbc_fixfeat(
            "fix(runtime): another",
            extra=["--mode=hint"],
        )
        # We override --mode after the helper appended --mode=report —
        # argparse takes the LAST value for repeated flags, so the
        # added --mode=hint wins.
        self.assertEqual(code, 0, msg=out)
        # Body should still surface the violation note.
        self.assertIn("fix/feat-needs-bump", out)

    def test_fixfeat_check_helper_unit_cases(self) -> None:
        """Direct coverage for the helper that classifies titles. Keeps
        the regex grammar from silently regressing under future
        refactors."""
        vbc = self._import_gate_module("version_bump_check")

        positives = [
            "fix: tweak something",
            "fix(view): tweak something",
            "fix!: API broke",
            "fix(view)!: API broke in view",
            "feat: new widget",
            "feat(audio): new oscillator",
            "feat!: redo public API",
        ]
        negatives = [
            "chore: bump versions",
            "docs: update guide",
            "test: tighten",
            "refactor(view): rename helper",
            "perf(audio): faster path",
            "build: cmake bump",
            "ci: add lint job",
            "style(view): reformat",
            "Revert \"feat(view): add foo\"",
            "WIP: not yet",
            "",
            "feat without colon",
            "fixed it",  # not "fix:" — must not match
        ]
        for t in positives:
            self.assertTrue(
                vbc._is_fix_or_feat_title(t),
                msg=f"expected {t!r} to be classified as fix/feat",
            )
        for t in negatives:
            self.assertFalse(
                vbc._is_fix_or_feat_title(t),
                msg=f"expected {t!r} NOT to be classified as fix/feat",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
