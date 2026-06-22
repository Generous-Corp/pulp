#!/usr/bin/env python3
"""Fixture tests for the version_bump --require-bump-for-fix-feat check.

The PR-title fix/feat-needs-bump guard (issue #1009). These exercise the
helpers (`_is_fix_or_feat_title`, `_range_has_bump_commit`,
`_range_has_version_bump_skip_trailer`, `check_fix_feat_requires_bump`)
that remain in `version_bump_check.py` alongside `main()`.

Runs standalone (`python3 tools/scripts/test_version_bump_fixfeat.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

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
