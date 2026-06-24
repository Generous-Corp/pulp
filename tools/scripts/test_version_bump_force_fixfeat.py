#!/usr/bin/env python3
"""Fixture tests for the `--mode=apply` force-bump reconciliation.

Issue #4679: `--mode=apply` and the CI gate
`--require-bump-for-fix-feat` disagreed when a branch's version equalled
its merge-base. The per-surface heuristic found no delta → apply wrote
nothing → CI hard-failed the `fix:` / `feat:` PR for lacking a
`chore: bump versions` commit, forcing a manual hand-bump.

`force_fix_feat_bump` (wired into `main()`'s apply branch) closes the
gap: when the title is `fix:` / `feat:`, nothing else bumped, and the
diff touched a versioned surface's `trigger_paths`, apply now FORCES the
minimum bump (patch for `fix:`, minor for `feat:`). When NO versioned
surface is touched it emits an actionable reclassify/skip message and
exits non-zero rather than silently no-opping.

The branch==base trap is reproduced with a comment-only edit to a
trigger path: `git_diff_ignore_whitespace_nonempty` filters it out, so
the per-surface verdict is "none" even though the path was touched —
exactly the shape a same-version-base branch presents to the gate.

Runs standalone (`python3 tools/scripts/test_version_bump_force_fixfeat.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
