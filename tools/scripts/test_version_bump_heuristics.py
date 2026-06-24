#!/usr/bin/env python3
"""Fixture tests for the version_bump heuristics cluster.

Path/content heuristic, conventional-commit classification, trailer
overrides, glob matching, revert suppression. Mirrors
`version_bump_heuristics.py`.

Runs standalone (`python3 tools/scripts/test_version_bump_heuristics.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

import json
import sys
import unittest

from gate_test_support import GateFixtureTestCase, REPO_ROOT, _git


class VersionBumpHeuristicsTests(GateFixtureTestCase):
    """version_bump_check heuristics-cluster fixtures."""

    def test_new_public_header_flags_minor(self) -> None:
        self.f.write(
            "core/runtime/include/pulp/runtime/bar.hpp",
            "#pragma once\nint bar();\n",
        )
        self.f.commit("feat: add bar() public API")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("minor", out)
        self.assertIn("bump required", out)

    def test_comments_only_edit_does_not_trigger(self) -> None:
        path = self.tmp / "core/runtime/include/pulp/runtime/foo.hpp"
        path.write_text(
            "#pragma once\n"
            "// A brand new explanatory comment.\n"
            "// Another line of comment.\n"
            "int foo();\n"
        )
        self.f.commit("docs: add header comments")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 0, msg=out)
        # The heuristic should fall through to none (no real diff) or at
        # most patch-suggested — never require a minor or major bump.
        self.assertNotIn("bump required", out)

    def test_whitespace_only_edit_does_not_trigger(self) -> None:
        path = self.tmp / "core/runtime/include/pulp/runtime/foo.hpp"
        path.write_text("#pragma once\n\n\nint foo();\n\n")
        self.f.commit("chore: reflow whitespace")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 0, msg=out)
        self.assertNotIn("bump required", out)

    def test_breaking_trailer_triggers_major(self) -> None:
        path = self.tmp / "core/runtime/include/pulp/runtime/foo.hpp"
        path.write_text("#pragma once\nvoid foo();\n")  # return type change
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             "feat!: foo() is now void\n\nBREAKING: return type changed.")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("major", out)

    def test_test_only_change_reports_none(self) -> None:
        self.f.write("test/test_foo.cpp", "int main() { return 42; }\n")
        self.f.commit("test: tighten foo assertions")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump needed", out)

    def test_generated_file_is_skipped(self) -> None:
        self.f.write("build/generated.cpp", "// generated\n")
        self.f.commit("build: regenerate artifacts")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump needed", out)

    def test_version_bump_trailer_skip_suppresses(self) -> None:
        self.f.write(
            "core/runtime/include/pulp/runtime/bar.hpp",
            "#pragma once\nint bar();\n",
        )
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'feat: add bar()\n\nVersion-Bump: sdk=skip reason="release cadence defer"')
        code, out = self.f.run_vbc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump needed", out)

    def test_revert_does_not_trigger_version_bump(self) -> None:
        # Forward commit that would normally require a minor bump.
        self.f.write(
            "core/runtime/include/pulp/runtime/bar.hpp",
            "#pragma once\nint bar();\n",
        )
        self.f.commit("feat: add bar()")
        # Then revert it by removing the file in a commit whose subject
        # starts with `Revert`.
        (self.tmp / "core/runtime/include/pulp/runtime/bar.hpp").unlink()
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'Revert "feat: add bar()"\n\nRevert-Of: dummy')
        code, out = self.f.run_vbc()
        # Net diff from origin/main to HEAD is empty on this surface; the
        # gate should not demand a bump.
        self.assertEqual(code, 0, msg=out)
        self.assertIn("no bump needed", out)

    # ── Regression: 2026-04-20 fnmatch `**` glob bug (#538/#540/#541/#546) ─

    def test_tools_cli_top_level_file_is_detected(self) -> None:
        """Editing tools/cli/cmd_foo.cpp must match the sdk trigger pattern
        tools/cli/**/*.cpp. The stdlib fnmatch-based matcher previously
        failed here because it didn't treat `**` as zero-or-more segments,
        so a `feat:` on a top-level CLI file silently produced "no bump
        needed". See today's incidents on PRs #538/#540/#541/#546."""
        self.f.write(
            "tools/cli/cmd_foo.cpp",
            "int cmd_foo_run() { return 0; }\n",
        )
        self.f.commit("feat: cli add cmd_foo")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("[sdk]", out)
        self.assertIn("bump required", out)
        self.assertIn("minor", out)

    def test_glob_matching_unit_cases(self) -> None:
        """Direct unit coverage for the glob-to-regex helper. These are the
        cases the incident playbook explicitly calls out — keep them
        exhaustive so a future refactor can't silently regress."""
        # Import here so the rest of the file keeps working even without
        # the fix rolled out.
        sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
        from version_bump_check import _glob_match

        positives = [
            ("tools/cli/cmd_doctor.cpp", "tools/cli/**/*.cpp"),
            ("tools/cli/cmd_doctor.cpp", "tools/cli/*.cpp"),
            ("tools/cli/sub/cmd_x.cpp",  "tools/cli/**/*.cpp"),
            ("core/view/src/widget_bridge.cpp", "core/**"),
            ("core/view/src/widget.cpp", "core/**/src/**/*.cpp"),
            ("core/view/include/pulp/view.hpp", "core/**/include/**/*.hpp"),
            ("CMakeLists.txt", "CMakeLists.txt"),
            ("docs/reference/cli.md", "docs/**/*.md"),
            ("docs/cli.md", "docs/**/*.md"),
            (".claude-plugin/plugin.json", ".claude-plugin/**"),
            (".claude-plugin/sub/foo.json", ".claude-plugin/**"),
            ("build/foo/bar.cpp", "build/**"),
            ("build/bar.cpp", "build/**"),
            ("foo.generated.cpp", "**/*.generated.*"),
            ("sub/foo.generated.cpp", "**/*.generated.*"),
            ("pulp.lock", "*.lock"),
            ("foo/bar/baz.hpp", "**/bar/*.hpp"),
            ("bar/baz.hpp", "**/bar/*.hpp"),
        ]
        negatives = [
            # Non-sibling directory — core/** must not match external/**.
            ("external/choc/json.hpp", "core/**"),
            # Wrong extension.
            ("tools/cli/foo.js", "tools/cli/**/*.cpp"),
            # Pattern has no leading `**/` so root-level file must not match.
            ("sub/pulp.lock", "*.lock"),
            # `**/bar/*.hpp` requires a `bar` segment.
            ("baz.hpp", "**/bar/*.hpp"),
            # Completely unrelated top-level file.
            ("CMakeLists.txt", "core/**"),
            # Prefix that looks like a subdir but isn't.
            ("coretools/foo.cpp", "core/**"),
            # Regression #554: zero-segment '**'
            # collapse must preserve the surrounding '/' boundaries.
            # `tools/cli/**/*.cpp` must NOT match `tools/clicmd.cpp`
            # (the '/' after `cli` is a required anchor).
            ("tools/clicmd.cpp", "tools/cli/**/*.cpp"),
            # Same check with two middle '**': `core/**/src/**/*.cpp`
            # must NOT match `coresrc/foo.cpp` (both '/' anchors are
            # required).
            ("coresrc/foo.cpp", "core/**/src/**/*.cpp"),
            # Middle '**' zero-segment between concrete directories:
            # `a/**/b/*.txt` must NOT match `ab/foo.txt`.
            ("ab/foo.txt", "a/**/b/*.txt"),
        ]
        for path, pattern in positives:
            self.assertTrue(
                _glob_match(path, pattern),
                msg=f"expected {path!r} to match {pattern!r}",
            )
        for path, pattern in negatives:
            self.assertFalse(
                _glob_match(path, pattern),
                msg=f"expected {path!r} to NOT match {pattern!r}",
            )

    def test_override_applies_when_heuristic_is_comment_only(self) -> None:
        """An explicit Version-Bump trailer should apply when the surface's
        trigger paths were touched, even if the diff is entirely comments
        and the heuristic would otherwise fall through to "none"."""
        # Comment-only edit to a touched trigger path → heuristic none.
        path = self.tmp / "core/runtime/include/pulp/runtime/foo.hpp"
        path.write_text(
            "#pragma once\n"
            "// Explanatory header comment added by a docs-only PR.\n"
            "int foo();\n"
        )
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'docs: flesh out foo() header comment\n\n'
             'Version-Bump: sdk=patch reason="doc-only header tweak, explicit patch"')
        code, out = self.f.run_vbc()
        # Patch is advisory in report mode; what we verify here is that
        # the trailer was honored, not ignored — the verdict line should
        # show `override=patch` alongside `final=patch`.
        self.assertIn("override=patch", out, msg=out)
        self.assertIn("final=patch", out, msg=out)

    def test_override_for_untouched_surface_is_ignored(self) -> None:
        """Anti-rubberstamp: a Version-Bump trailer referencing a surface
        whose trigger paths weren't touched must NOT force a bump."""
        # Touch only the plugin manifest; SDK trigger paths are untouched.
        (self.tmp / ".claude-plugin" / "plugin.json").write_text(
            json.dumps({"name": "test", "version": "0.2.0"}, indent=2) + "\n"
        )
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'chore(plugin): bump\n\n'
             'Version-Bump: sdk=major reason="should be ignored, sdk untouched"')
        code, out = self.f.run_vbc()
        # Plugin surface is properly bumped, SDK is untouched and the stray
        # override must not conjure a bump for it.
        self.assertIn("[sdk]", out)
        self.assertIn("no bump needed", out)

    def test_version_arithmetic_conventional_and_trailer_helpers(self) -> None:
        vbc = self._import_gate_module("version_bump_check")

        self.assertEqual(vbc.bump_version("1.2.3", "major"), "2.0.0")
        self.assertEqual(vbc.bump_version("1.2.3", "minor"), "1.3.0")
        self.assertEqual(vbc.bump_version("1.2.3", "patch"), "1.2.4")
        self.assertEqual(vbc.bump_version("1.2.3", "none"), "1.2.3")
        self.assertEqual(vbc.bump_version("not-semver", "major"), "not-semver")

        subjects = {
            "feat(cli): add command": "minor",
            "fix(runtime): repair edge case": "patch",
            "perf(audio): avoid copy": "patch",
            "feat!: remove old API": "major",
            "BREAKING: change API": "major",
            "docs: refresh guide": "none",
            "release train": "none",
        }
        for subject, level in subjects.items():
            self.assertEqual(vbc.classify_conventional(subject), level)

        trailers = {"version-bump": [
            'sdk=none reason="invalid and ignored"',
            'plugin=skip reason="not shipping plugin"',
            'sdk=minor reason="public API"',
        ]}
        self.assertEqual(
            vbc.surface_trailer_override(trailers, "Version-Bump", "sdk"),
            "minor",
        )
        self.assertEqual(
            vbc.surface_trailer_override(trailers, "Version-Bump", "plugin"),
            "skip",
        )
        self.assertIsNone(
            vbc.surface_trailer_override(
                {"version-bump": ['sdk=none reason="invalid"']},
                "Version-Bump",
                "sdk",
            )
        )

    def test_version_helper_edges(self) -> None:
        vbc = self._import_gate_module("version_bump_check")

        self.assertEqual(
            vbc.filter_generated(
                ["build/out.cpp", "src/generated/foo.cpp", "src/live.cpp"],
                ["build/**", "**/generated/**"],
            ),
            ["src/live.cpp"],
        )
        self.assertTrue(vbc.is_revert_commit("chore: undo", {"revert-of": ["abc"]}))
        self.assertFalse(vbc.is_revert_commit("fix: ordinary change", {}))
        self.assertEqual(vbc.max_level("patch", "minor", "major"), "major")
        self.assertEqual(vbc.max_level("none", "patch"), "patch")


if __name__ == "__main__":
    unittest.main(verbosity=2)
