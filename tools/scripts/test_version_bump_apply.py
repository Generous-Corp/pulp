#!/usr/bin/env python3
"""Fixture tests for the version_bump apply + render clusters.

`--mode=apply` version-file rewriting and `render_report` verdict
rendering (including the partial-multi-file-bump regression). Mirrors
`version_bump_apply.py` and `version_bump_render.py`.

Runs standalone (`python3 tools/scripts/test_version_bump_apply.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

import json
import os
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from gate_test_support import GateFixtureTestCase, _git


class VersionBumpApplyTests(GateFixtureTestCase):
    """version_bump_check apply + render cluster fixtures."""

    def test_cluster_helper_fallbacks_cover_isolated_imports(self) -> None:
        vba = self._import_gate_module("version_bump_apply")
        vbr = self._import_gate_module("version_bump_render")
        vbh = self._import_gate_module("version_bump_heuristics")

        with mock.patch.object(vba, "_vbc", return_value=object()):
            self.assertIs(vba._h("bump_version"), vba.bump_version)
        with mock.patch.object(vbr, "_vbc", return_value=object()):
            self.assertIs(vbr._h("already_bumped"), vbr.already_bumped)
        with mock.patch.object(vbh, "_vbc", return_value=object()):
            self.assertIs(vbh._h("git_range_trailers"), vbh.git_range_trailers)

    def test_partial_multi_file_bump_fails(self) -> None:
        """Plugin surface with two version files: bumping only ONE must
        fail hard."""
        r = self.tmp
        (r / ".claude-plugin" / "marketplace.json").write_text(
            json.dumps({"name": "test", "version": "0.1.0"}, indent=2) + "\n"
        )
        cfg_path = r / "tools/scripts/versioning.json"
        cfg = json.loads(cfg_path.read_text())
        cfg["surfaces"]["plugin"]["version_files"].append({
            "path": ".claude-plugin/marketplace.json",
            "kind": "json_field", "field": "version",
        })
        cfg_path.write_text(json.dumps(cfg, indent=2) + "\n")
        self.f.commit("chore: add marketplace manifest")

        # Trigger the plugin surface AND bump plugin.json only.
        (r / ".claude-plugin" / "plugin.json").write_text(
            json.dumps({"name": "test", "version": "0.2.0"}, indent=2) + "\n"
        )
        self.f.commit("feat: bump plugin.json only")
        code, out = self.f.run_vbc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("partial bump", out)
        self.assertIn("marketplace.json", out)

    def test_apply_writes_version_for_tools_cli_top_level_file(self) -> None:
        """`--mode=apply` must actually rewrite CMakeLists.txt for a
        top-level tools/cli/*.cpp edit. The silent-skip was the symptom
        reported by four agents today."""
        self.f.write(
            "tools/cli/cmd_foo.cpp",
            "int cmd_foo_run() { return 0; }\n",
        )
        self.f.commit("feat: cli add cmd_foo")
        code, out = self.f.run_vbc(["--mode=apply"])
        self.assertEqual(code, 0, msg=out)
        self.assertIn("bumped", out)
        new_cmake = (self.tmp / "CMakeLists.txt").read_text()
        # 0.1.0 → minor bump → 0.2.0.
        self.assertIn("VERSION 0.2.0", new_cmake, msg=new_cmake)

    def test_apply_writes_patch_bump_for_internal_fix(self) -> None:
        """A `fix:` change to an internal-only SDK path classifies as
        `patch`; `--mode=apply` must WRITE the version file (0.1.0 → 0.1.1).

        Regression for danielraffel/Shipyard#358: `apply_bumps` skipped
        `final_level == "patch"` alongside `"none"`, so apply mode only
        "suggested" the bump and never wrote it — stranding every `fix:`
        / `feat:`-patch PR at the `--require-bump-for-fix-feat` gate with
        no `chore: bump versions` marker. Only `feat:` (minor) was covered
        before, which is why the patch path silently regressed."""
        self.f.write("core/runtime/src/foo.cpp", "void foo() {}\n")
        self.f.commit("fix: foo internal tweak")
        code, out = self.f.run_vbc(["--mode=apply"])
        self.assertEqual(code, 0, msg=out)
        self.assertIn("bumped", out)
        self.assertIn(
            "VERSION 0.1.1",
            (self.tmp / "CMakeLists.txt").read_text(),
            msg=out,
        )

    def test_apply_bumps_writes_version_files_only_post_c1(self) -> None:
        # apply_bumps writes version files only. CHANGELOG.md is owned by
        # Shipyard post-tag sync via `.github/workflows/post-tag-sync.yml`.
        # Two PRs both proposing `sdk=minor` must produce identical
        # CHANGELOG.md state to avoid the multi-PR-train rebase class.
        vbc = self._import_gate_module("version_bump_check")

        cfg_path = self.tmp / "tools/scripts/versioning.json"
        cfg = json.loads(cfg_path.read_text())
        cfg["surfaces"]["sdk"]["changelog"] = "CHANGELOG.md"
        cfg_path.write_text(json.dumps(cfg, indent=2) + "\n")
        cl_before = "# Changelog\n\n## [0.1.0]\n\n- Initial.\n"
        self.f.write("CHANGELOG.md", cl_before)
        self.f.commit("chore: add changelog config")
        _git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

        path = self.tmp / "core/runtime/include/pulp/runtime/foo.hpp"
        path.write_text("#pragma once\nvoid foo();\n")
        self.f.commit("feat: change public foo signature")

        previous_cwd = Path.cwd()
        os.chdir(self.tmp)
        try:
            loaded = vbc.load_config(cfg_path)
            changed = vbc.filter_generated(
                vbc.git_diff_names("origin/main", "HEAD"),
                loaded.generated_globs,
            )
            verdicts = vbc.assess_surfaces(
                loaded,
                changed,
                "origin/main",
                "HEAD",
                self.tmp,
            )
            edited = vbc.apply_bumps(verdicts, "origin/main", self.tmp)
            # Only the version file is edited; CHANGELOG.md untouched.
            self.assertEqual(set(edited), {"CMakeLists.txt"})

            report, code = vbc.render_report(
                vbc.assess_surfaces(loaded, changed, "origin/main", "HEAD", self.tmp),
                "report",
                "origin/main",
                self.tmp,
            )
            self.assertEqual(code, 0, msg=report)
            self.assertIn("bumped", report)

            edited_again = vbc.apply_bumps(
                vbc.assess_surfaces(loaded, changed, "origin/main", "HEAD", self.tmp),
                "origin/main",
                self.tmp,
            )
            self.assertEqual(edited_again, [])
        finally:
            os.chdir(previous_cwd)

        self.assertIn("VERSION 0.2.0", (self.tmp / "CMakeLists.txt").read_text())
        # CHANGELOG.md is unchanged — Shipyard regenerates it post-tag.
        self.assertEqual((self.tmp / "CHANGELOG.md").read_text(), cl_before)
        status = subprocess.run(
            ["git", "-C", str(self.tmp), "status", "--short"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertIn("M  CMakeLists.txt", status)
        # CHANGELOG.md is NOT staged; `apply_bumps()` skips it entirely so
        # Shipyard's post-tag regen owns the file without racing PRs over
        # identical stub headers.
        self.assertNotIn("CHANGELOG.md", status)

    def _fork_behind_a_bumped_main(self) -> None:
        """origin/main gains a version bump this branch forked before.

        The everyday state on a busy main, not an exotic one: any PR that
        merges a `chore: bump versions` while yours is open puts your branch
        behind on the version line.
        """
        _git(self.tmp, "checkout", "-q", "-b", "mainline")
        (self.tmp / ".claude-plugin" / "plugin.json").write_text(
            json.dumps({"name": "test", "version": "0.3.0"}, indent=2) + "\n"
        )
        self.f.commit("chore: bump versions")
        _git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")
        _git(self.tmp, "checkout", "-q", "-b", "feature", "mainline~1")

    def test_apply_on_a_stale_base_writes_instead_of_claiming_a_phantom_bump(
        self,
    ) -> None:
        """A branch BEHIND base must still get a real write.

        `already_bumped` compared base != head, so a branch behind on the
        version line (head 0.1.0, base 0.3.0) read as "already bumped":
        `apply_bumps` short-circuited, wrote nothing, and the report still
        printed `✓ bumped` at exit 0. The `chore: bump versions` marker never
        appeared and CI's fix/feat gate failed downstream with no hint why.
        Ordering the comparison makes the skip conditional on being AHEAD.
        """
        self._fork_behind_a_bumped_main()
        self.f.write(".claude-plugin/cmd.md", "do a thing\n")
        self.f.commit("feat: add a plugin command")

        code, out = self.f.run_vbc(["--mode=apply"])
        self.assertEqual(code, 0, msg=out)
        # Bumped from the version at base (0.3.0), not from the stale 0.1.0
        # on disk — so the result is correct against the main we'll merge to.
        self.assertEqual(
            json.loads(
                (self.tmp / ".claude-plugin" / "plugin.json").read_text()
            )["version"],
            "0.4.0",
            msg=out,
        )

    def test_stale_base_bump_is_not_a_version_regression(self) -> None:
        """Behind base but self-bumped is still behind.

        head 0.2.0 under base 0.3.0 differs from base, so the inequality
        check called it bumped and let the PR merge a version REGRESSION
        onto main.
        """
        self._fork_behind_a_bumped_main()
        (self.tmp / ".claude-plugin" / "plugin.json").write_text(
            json.dumps({"name": "test", "version": "0.2.0"}, indent=2) + "\n"
        )
        self.f.write(".claude-plugin/cmd.md", "do a thing\n")
        self.f.commit("feat: add a plugin command")

        code, out = self.f.run_vbc(["--mode=apply"])
        self.assertEqual(code, 0, msg=out)
        self.assertEqual(
            json.loads(
                (self.tmp / ".claude-plugin" / "plugin.json").read_text()
            )["version"],
            "0.4.0",
            msg=out,
        )

    def test_apply_refuses_success_when_the_write_silently_no_ops(self) -> None:
        """The guard fires when apply requests a bump that never lands.

        An internal-only `fix:` classifies as `patch`, which `render_report`
        scores as an advisory `?` and exits 0 — so a dropped patch write is
        invisible to the report alone. Here the project() line has no VERSION
        field (a reformat, or a drifted versioning.json pattern), so
        `apply_bumps` finds no `current_version`, declines to write, and
        returns normally. Nothing raises; only re-reading the file catches it.
        """
        self.f.write("core/runtime/src/foo.cpp", "int foo() { return 2; }\n")
        self.f.write(
            "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(Test LANGUAGES CXX)\n",
        )
        self.f.commit("fix: tighten foo")

        code, out = self.f.run_vbc(["--mode=apply"])
        self.assertEqual(code, 1, msg=out)
        self.assertIn("refusing to report success", out)
        self.assertIn("CMakeLists.txt", out)

    def test_apply_verification_stays_quiet_on_a_healthy_apply(self) -> None:
        """PASS control for the guard above.

        A checker that only ever reports failure is indistinguishable from a
        broken one, so pin the negative: a normal apply whose write lands must
        exit 0 and say nothing about refusing.
        """
        self.f.write("core/runtime/src/foo.cpp", "int foo() { return 2; }\n")
        self.f.commit("fix: tighten foo")

        code, out = self.f.run_vbc(["--mode=apply"])
        self.assertEqual(code, 0, msg=out)
        self.assertNotIn("refusing to report success", out)
        self.assertIn("VERSION 0.1.1", (self.tmp / "CMakeLists.txt").read_text())

    def test_apply_verification_allows_a_version_file_new_in_this_branch(
        self,
    ) -> None:
        """A surface whose version file does not exist at base is not stranded.

        `version_at_base` returns None for a file this branch adds, so there
        is no ordering to compare and "not ahead of base" proves nothing about
        a dropped write. The guard must not fail an added surface.
        """
        vbc = self._import_gate_module("version_bump_check")
        surface = vbc.Surface(
            "new", "New",
            [vbc.VersionFile("brand-new.json", "json_field", "version")],
            ["src/**"],
        )
        self.f.write("brand-new.json", '{"version": "0.1.0"}\n')
        verdict = vbc.Verdict(surface, "minor", None, "0.1.0", "minor")

        self.assertIsNone(
            vbc.verify_applied_bumps([verdict], "origin/main", self.tmp)
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
