#!/usr/bin/env python3
"""Fixture tests for the version_bump apply + render clusters.

`--mode=apply` version-file rewriting and `render_report` verdict
rendering (including the partial-multi-file-bump regression). Mirrors
`version_bump_apply.py` and `version_bump_render.py`. Split from
`test_gates.py` (P9-NEW refactor, 2026-05); test bodies are
byte-identical to their previous definitions.

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
        """Plugin surface with two version files: bumping only ONE used to
        let the gate pass (Codex P1). Now fails hard."""
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

    def test_apply_bumps_writes_version_files_only_post_c1(self) -> None:
        # Post-C1 (2026-05): apply_bumps writes version files only.
        # CHANGELOG.md is owned by Shipyard post-tag sync via
        # `.github/workflows/post-tag-sync.yml`. Two PRs both proposing
        # `sdk=minor` must produce identical CHANGELOG.md state to avoid
        # the multi-PR-train rebase class.
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
        # Post-C1: CHANGELOG.md is NOT staged — `apply_bumps()` skips it
        # entirely so Shipyard's post-tag regen owns the file without
        # racing PRs over identical stub headers.
        self.assertNotIn("CHANGELOG.md", status)


if __name__ == "__main__":
    unittest.main(verbosity=2)
