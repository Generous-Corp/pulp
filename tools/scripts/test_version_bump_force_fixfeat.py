#!/usr/bin/env python3
"""Fixture tests for the `--mode=apply` force-bump reconciliation.

Issue #4679: `--mode=apply` and the CI gate
`--require-bump-for-fix-feat` disagreed when a branch's version equalled
its merge-base. The per-surface heuristic found no delta → apply wrote
nothing → CI hard-failed the `fix:` / `feat:` PR for lacking a
`chore: bump versions` commit, forcing a manual hand-bump.

`force_fix_feat_bump` (wired into `main()`'s apply branch) closes the gap: when
the title or a live commit subject is `fix:` / `feat:` and the diff touched a
versioned surface's `trigger_paths`, apply raises every touched surface to at
least the signal's minimum (patch for `fix:`, minor for `feat:`). This includes
a scoped-skip surface even when another surface was normally bumped. When NO
versioned surface is touched it emits an actionable reclassify/skip message and
exits non-zero rather than silently no-opping.

The branch==base trap is reproduced with a comment-only edit to a
trigger path: `git_diff_ignore_whitespace_nonempty` filters it out, so
the per-surface verdict is "none" even though the path was touched —
exactly the shape a same-version-base branch presents to the gate.

Runs standalone (`python3 tools/scripts/test_version_bump_force_fixfeat.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

import json
import unittest

from gate_test_support import GateFixtureTestCase, VBC, _run, _git


class VersionBumpForceFixFeatTests(GateFixtureTestCase):
    """`force_fix_feat_bump` apply-mode reconciliation fixtures (#4679)."""

    def _run_apply(
        self,
        title: str | None,
        extra: list[str] | None = None,
    ) -> tuple[int, str]:
        cmd = [
            "python3", str(VBC),
            "--base", "origin/main",
            "--config", str(self.tmp / "tools/scripts/versioning.json"),
            "--mode=apply",
            *(extra or []),
        ]
        if title is not None:
            cmd.extend(["--pr-title", title])
        return _run(cmd, cwd=self.tmp)

    def _run_report_fixfeat(self, title: str) -> tuple[int, str]:
        return _run(
            [
                "python3", str(VBC),
                "--base", "origin/main",
                "--config", str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=report",
                "--require-bump-for-fix-feat",
                "--pr-title", title,
            ],
            cwd=self.tmp,
        )

    def test_fix_branch_eq_base_forces_patch_and_report_then_passes(self) -> None:
        """Acceptance (a): a `fix:`-titled change whose branch version ==
        merge-base, touching a versioned trigger path, run through
        `--mode=apply` produces a `chore: bump versions` commit; the
        `--require-bump-for-fix-feat` report then passes with no manual
        step.

        The comment-only edit reproduces the same-version-base shape:
        the per-surface heuristic is "none" (whitespace/comment filter),
        so the pre-#4679 apply pass would have written nothing.
        """
        title = "fix(runtime): clarify foo() contract"
        # Comment-only addition to an SDK trigger path → heuristic "none".
        self.f.write(
            "core/runtime/src/foo.cpp",
            "// foo returns the canonical sentinel\nint foo() { return 1; }\n",
        )
        self.f.commit(title)

        # Sanity: plain report sees no per-surface bump (branch==base shape).
        plain_code, plain_out = self.f.run_vbc()
        self.assertEqual(plain_code, 0, msg=plain_out)
        self.assertIn("no bump needed", plain_out)

        # Apply forces the patch bump on the touched SDK surface.
        code, out = self._run_apply(title)
        self.assertEqual(code, 0, msg=out)
        self.assertIn("Edited files:", out)
        self.assertIn("CMakeLists.txt", out)
        self.assertIn(
            "VERSION 0.1.1",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

        # The caller (shipyard pr) appends the canonical bump commit.
        self.f.commit("chore: bump versions")

        # Now the CI gate passes with no manual hand-bump.
        rc, rout = self._run_report_fixfeat(title)
        self.assertEqual(rc, 0, msg=rout)
        self.assertIn("OK", rout)

    def test_feat_branch_eq_base_forces_minor(self) -> None:
        """A `feat:` title forces a minor bump (0.1.0 → 0.2.0), mirroring
        the conv-commit ceiling the per-surface pipeline would apply."""
        title = "feat(runtime): introduce foo() fast path"
        self.f.write(
            "core/runtime/src/foo.cpp",
            "// fast path note\nint foo() { return 1; }\n",
        )
        self.f.commit(title)

        code, out = self._run_apply(title)
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "VERSION 0.2.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_commit_signals_force_levels_per_surface(self) -> None:
        """An SDK fix and plugin feature retain independent bump levels."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 211; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "fix(runtime): repair worker latch",
            "-m",
            'Version-Bump: sdk=skip reason="exercise recovery"',
        )
        plugin = self.tmp / ".claude-plugin/plugin.json"
        plugin_data = json.loads(plugin.read_text())
        plugin_data["description"] = "New queue reporting"
        plugin.write_text(json.dumps(plugin_data, indent=2) + "\n")
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "feat(plugin): report queue release tags",
            "-m",
            'Version-Bump: plugin=skip reason="exercise recovery"',
        )

        code, out = self._run_apply("Repair release automation")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("VERSION 0.1.1", (self.tmp / "CMakeLists.txt").read_text())
        self.assertEqual(json.loads(plugin.read_text())["version"], "0.2.0")

    def test_numeric_trailer_remains_authoritative_during_force(self) -> None:
        """A feat signal cannot raise an explicit sdk=patch verdict."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 212; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "feat(runtime): improve worker recovery",
            "-m",
            "Version-Bump: sdk=patch",
        )

        code, out = self._run_apply("Improve worker recovery")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("VERSION 0.1.1", (self.tmp / "CMakeLists.txt").read_text())
        self.assertNotIn("VERSION 0.2.0", (self.tmp / "CMakeLists.txt").read_text())

    def test_feat_no_versioned_surface_emits_actionable_message(self) -> None:
        """Acceptance (b): a `feat:`-titled change with NO versioned
        surface touched emits an actionable reclassify/skip message and
        exits non-zero — never a silent no-op.

        `test/**` is not in any surface's trigger_paths in the fixture
        config (it is the P5 `build:`-class shape).
        """
        title = "feat: rework the test harness layout"
        self.f.write("test/test_foo.cpp", "int main() { return 1; }\n")
        self.f.commit(title)

        code, out = self._run_apply(title)
        self.assertEqual(code, 1, msg=out)
        self.assertIn("NO versioned surface", out)
        self.assertIn("Re-title the PR", out)
        self.assertIn("Version-Bump: skip", out)
        # Nothing was written.
        self.assertIn(
            "VERSION 0.1.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_no_surface_message_fires_without_require_flag(self) -> None:
        """The actionable no-surface message + non-zero exit must fire in
        apply mode even WITHOUT `--require-bump-for-fix-feat` — apply must
        not silently succeed on an unreconcilable `fix:` / `feat:`."""
        title = "fix: only touches docs and tests"
        self.f.write("test/test_foo.cpp", "int main() { return 2; }\n")
        self.f.commit(title)

        # No --require-bump-for-fix-feat flag here.
        code, out = self._run_apply(title)
        self.assertEqual(code, 1, msg=out)
        self.assertIn("NO versioned surface", out)

    def test_already_bumped_branch_is_not_double_bumped(self) -> None:
        """A `fix:` branch that already carries a real `chore: bump
        versions` commit must be left alone — the force-bump only fires
        when the range has no bump commit and no skip trailer."""
        title = "fix(runtime): genuine internal fix"
        self.f.write("core/runtime/src/foo.cpp", "int foo() { return 9; }\n")
        self.f.commit(title)

        # A real bump already landed on the branch.
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: bump versions")

        code, out = self._run_apply(title)
        self.assertEqual(code, 0, msg=out)
        # Still 0.1.1 — not double-bumped to 0.1.2.
        self.assertIn(
            "VERSION 0.1.1",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )
        self.assertNotIn("VERSION 0.1.2", (self.tmp / "CMakeLists.txt").read_text())

    def test_skip_trailer_branch_is_not_forced(self) -> None:
        """A `fix:` branch with a top-level `Version-Bump: skip
        reason="..."` trailer is an explicit opt-out — apply must honor
        it and NOT force a bump.

        Uses a comment-only edit so the per-surface heuristic is "none"
        (the branch==base shape): the ONLY thing that could write a
        version here is the force-bump, and the skip trailer must
        suppress it. (An unscoped skip trailer does not downgrade a real
        per-surface verdict — that is by design and tested elsewhere.)
        """
        title = "fix(runtime): cosmetic, reverted next PR"
        self.f.write(
            "core/runtime/src/foo.cpp",
            "// cosmetic note only\nint foo() { return 1; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp, "commit", "-q", "-m",
            'fix(runtime): cosmetic, reverted next PR\n\n'
            'Version-Bump: skip reason="reverted in the follow-up, no consumer impact"',
        )

        code, out = self._run_apply(title)
        self.assertEqual(code, 0, msg=out)
        # Untouched — the skip trailer suppressed the force-bump.
        self.assertIn(
            "VERSION 0.1.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_recovery_range_forces_past_surface_scoped_skip(self) -> None:
        """The generated stranded-release command must stage a real bump.

        A surface-scoped skip suppresses the normal heuristic but does not
        bypass the global fix/feat guard. Supplying the landed subject and the
        explicit guard flag activates the force path for that recovery range.
        """
        title = "fix(runtime): clear transient worker latch"
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 14; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            title,
            "-m",
            'Version-Bump: sdk=skip reason="internal idle-state repair"',
        )

        code, out = _run(
            [
                "python3",
                str(VBC),
                "--base",
                "HEAD^",
                "--head",
                "HEAD",
                "--config",
                str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=apply",
                "--require-bump-for-fix-feat",
                "--pr-title",
                title,
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("Edited files:", out)
        self.assertIn(
            "VERSION 0.1.1",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_plain_title_uses_commit_signal_to_force_past_scoped_skip(self) -> None:
        """Apply repairs the incident shape without rewriting the PR title."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 140; }\n",
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

        code, out = self._run_apply(
            "Fix worker-pool reheat latch after gate collision",
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("Edited files:", out)
        self.assertIn(
            "VERSION 0.1.1",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_normal_plugin_edit_does_not_mask_scoped_sdk_skip(self) -> None:
        """Force reconciliation completes every touched release surface."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 143; }\n",
        )
        plugin = self.tmp / ".claude-plugin/plugin.json"
        plugin_data = json.loads(plugin.read_text())
        plugin_data["description"] = "Updated metadata"
        plugin.write_text(json.dumps(plugin_data, indent=2) + "\n")
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

        code, out = self._run_apply("Repair worker latch metadata")
        self.assertEqual(code, 0, msg=out)
        self.assertIn("VERSION 0.1.1", (self.tmp / "CMakeLists.txt").read_text())
        self.assertIn(
            '"version": "0.2.0"',
            (self.tmp / ".claude-plugin/plugin.json").read_text(),
        )

    def test_feat_commit_signal_raises_fix_title_force_to_minor(self) -> None:
        """The strongest live signal controls the forced minimum level."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 141; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "feat(runtime): add worker recovery mode",
            "-m",
            'Version-Bump: sdk=skip reason="internal rollout"',
        )

        code, out = self._run_apply("fix(runtime): repair worker recovery")
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "VERSION 0.2.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_cross_pr_reapply_forces_original_fix_signal_bump(self) -> None:
        """Apply bumps when a reapply restores a fix from base history."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "// worker repair\nint foo() { return 1; }\n",
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

        code, out = self._run_apply("Reapply transient worker latch")
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "VERSION 0.1.1",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_cross_pr_feat_reapply_forces_minor_before_apply(self) -> None:
        """A restored feat signal cannot be stranded behind a patch write."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 145; }\n",
        )
        self.f.commit("feat(runtime): add worker recovery mode")
        _git(self.tmp, "revert", "--no-edit", "HEAD")
        _git(
            self.tmp,
            "update-ref",
            "refs/remotes/origin/main",
            "HEAD",
        )
        _git(self.tmp, "revert", "--no-edit", "HEAD")

        code, out = self._run_apply("Reapply worker recovery mode")
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "VERSION 0.2.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_recovery_range_accepts_ordinary_applied_bump(self) -> None:
        """A normal heuristic edit is also a pending bump-marker commit."""
        title = "fix(runtime): correct public worker contract"
        self.f.write(
            "core/runtime/include/pulp/runtime/foo.hpp",
            "#pragma once\nint foo();\nint worker_state();\n",
        )
        self.f.commit(title)

        code, out = _run(
            [
                "python3",
                str(VBC),
                "--base",
                "HEAD^",
                "--head",
                "HEAD",
                "--config",
                str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=apply",
                "--require-bump-for-fix-feat",
                "--pr-title",
                title,
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("applied a pending bump", out)
        self.assertIn(
            "VERSION 0.2.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_recovery_bumps_from_current_main_after_surface_advances(self) -> None:
        """Historical analysis does not reuse an obsolete version base."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 142; }\n",
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

        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.1.1")
        )
        self.f.commit("chore: advance SDK version without publishing")

        code, out = _run(
            [
                "python3",
                str(VBC),
                "--base",
                "HEAD^^",
                "--head",
                "HEAD^",
                "--apply-version-base",
                "HEAD",
                "--config",
                str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=apply",
                "--require-bump-for-fix-feat",
                "--pr-title",
                "Repair worker latch after stranded merge",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "VERSION 0.1.2",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_recovery_ignores_historical_marker_without_version_edit(self) -> None:
        """A marker-only historical commit cannot make recovery a no-op."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 144; }\n",
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
        _git(
            self.tmp,
            "commit",
            "--allow-empty",
            "-q",
            "-m",
            "chore: bump versions",
        )

        code, out = _run(
            [
                "python3",
                str(VBC),
                "--base",
                "origin/main",
                "--head",
                "HEAD",
                "--apply-version-base",
                "HEAD",
                "--recover-stranded-release",
                "--config",
                str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=apply",
                "--require-bump-for-fix-feat",
                "--pr-title",
                "Repair stranded worker latch",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("VERSION 0.1.1", (self.tmp / "CMakeLists.txt").read_text())

    def test_recovery_edits_only_uncovered_surface(self) -> None:
        """A previously released plugin surface is not bumped a second time."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 146; }\n",
        )
        plugin = self.tmp / ".claude-plugin/plugin.json"
        plugin_data = json.loads(plugin.read_text())
        plugin_data["version"] = "0.1.1"
        plugin.write_text(json.dumps(plugin_data, indent=2) + "\n")
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

        code, out = _run(
            [
                "python3",
                str(VBC),
                "--base",
                "origin/main",
                "--head",
                "HEAD",
                "--apply-version-base",
                "HEAD",
                "--recover-stranded-release",
                "--recover-surfaces",
                "sdk",
                "--config",
                str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=apply",
                "--require-bump-for-fix-feat",
                "--pr-title",
                "Repair stranded worker latch",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("VERSION 0.1.1", (self.tmp / "CMakeLists.txt").read_text())
        self.assertEqual(json.loads(plugin.read_text())["version"], "0.1.1")

    def test_recovery_uses_detector_level_after_sticky_boundary(self) -> None:
        """Covered feature work cannot raise a later uncovered fix recovery."""
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 301; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "feat(runtime): add worker recovery mode",
            "-m",
            "Version-Bump: sdk=minor",
        )
        cmake = (self.tmp / "CMakeLists.txt").read_text()
        (self.tmp / "CMakeLists.txt").write_text(
            cmake.replace("VERSION 0.1.0", "VERSION 0.2.0")
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
        self.f.write(
            "core/runtime/src/foo.cpp",
            "int foo() { return 302; }\n",
        )
        _git(self.tmp, "add", "-A")
        _git(
            self.tmp,
            "commit",
            "-q",
            "-m",
            "fix(runtime): repair later worker race",
        )

        code, out = _run(
            [
                "python3",
                str(VBC),
                "--base",
                "origin/main",
                "--head",
                "HEAD",
                "--apply-version-base",
                "HEAD",
                "--recover-stranded-release",
                "--recover-surfaces",
                "sdk",
                "--recover-levels",
                "sdk=patch",
                "--config",
                str(self.tmp / "tools/scripts/versioning.json"),
                "--mode=apply",
                "--require-bump-for-fix-feat",
                "--pr-title",
                "",
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("VERSION 0.2.1", (self.tmp / "CMakeLists.txt").read_text())
        self.assertNotIn("VERSION 0.3.0", (self.tmp / "CMakeLists.txt").read_text())

    def test_non_fixfeat_title_branch_eq_base_is_unchanged(self) -> None:
        """A non-`fix:`/`feat:` title (e.g. `chore:` / `docs:`) on a
        branch==base diff must keep the pre-#4679 no-op behavior — no
        force-bump, exit 0, no version change."""
        title = "docs(runtime): expand foo() comment"
        self.f.write(
            "core/runtime/src/foo.cpp",
            "// expanded docs comment\nint foo() { return 1; }\n",
        )
        self.f.commit(title)

        code, out = self._run_apply(title)
        self.assertEqual(code, 0, msg=out)
        self.assertIn(
            "VERSION 0.1.0",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )
        self.assertNotIn("Edited files:", out)

    def test_force_helper_min_level_unit_cases(self) -> None:
        """Direct coverage for the title→level helper. Keeps the
        patch-for-fix / minor-for-feat mapping from silently regressing."""
        vbc = self._import_gate_module("version_bump_check")
        self.assertEqual(vbc._fix_feat_min_level("fix: x"), "patch")
        self.assertEqual(vbc._fix_feat_min_level("fix(view): x"), "patch")
        self.assertEqual(vbc._fix_feat_min_level("fix!: x"), "patch")
        self.assertEqual(vbc._fix_feat_min_level("feat: x"), "minor")
        self.assertEqual(vbc._fix_feat_min_level("feat(audio): x"), "minor")
        self.assertEqual(vbc._fix_feat_min_level("feat!: x"), "minor")
        self.assertIsNone(vbc._fix_feat_min_level("chore: bump versions"))
        self.assertIsNone(vbc._fix_feat_min_level("docs: tidy"))
        self.assertIsNone(vbc._fix_feat_min_level(""))

        no_footer = (
            "fix: landed title (#42)\n\n"
            "* feat(plugin): embedded feature\n"
        )
        self.assertEqual(
            vbc._release_subjects("fix: landed title (#42)", no_footer),
            ["fix: landed title (#42)", "feat(plugin): embedded feature"],
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
