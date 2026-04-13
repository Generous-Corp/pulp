#!/usr/bin/env python3
"""Fixture tests for version_bump_check.py and skill_sync_check.py.

Uses Python's unittest so it runs with no extra deps on PEP-668 systems.
Each test spins up a throwaway git repo with a minimal versioning.json,
stages a scenario via real `git commit`s, and asserts the script's exit
code and stdout against the expected verdict.

Run:
    python3 tools/scripts/test_gates.py
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
VBC = REPO_ROOT / "tools" / "scripts" / "version_bump_check.py"
SSC = REPO_ROOT / "tools" / "scripts" / "skill_sync_check.py"


def _run(cmd: list[str], cwd: Path) -> tuple[int, str]:
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def _git(cwd: Path, *args: str, env: dict[str, str] | None = None) -> None:
    full_env = os.environ.copy()
    full_env.update(env or {})
    subprocess.run(["git", "-C", str(cwd), *args], check=True, env=full_env,
                   capture_output=True)


class Fixture:
    """Throwaway repo with the gate scripts accessible.

    The scripts are called via absolute path (they look up the repo root
    via --repo-root for skill-sync / via `git rev-parse` for
    version-bump). Layout per fixture:

        <tmp>/
            .git/
            CMakeLists.txt
            .claude-plugin/plugin.json
            .agents/skills/
                ci/SKILL.md
                cli-maintenance/SKILL.md
            tools/scripts/versioning.json
            tools/scripts/skill_path_map.json
            core/runtime/include/pulp/runtime/foo.hpp
            core/runtime/src/foo.cpp
            ...
    """

    def __init__(self, root: Path) -> None:
        self.root = root

    def init(self) -> None:
        r = self.root
        _git(r, "init", "-q", "-b", "main")
        _git(r, "config", "user.email", "test@example.com")
        _git(r, "config", "user.name",  "Test")
        _git(r, "config", "commit.gpgsign", "false")

        # Minimal CMakeLists with a project(VERSION ...).
        (r / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(Test VERSION 0.1.0 LANGUAGES CXX)\n"
        )

        # Minimal plugin manifest.
        (r / ".claude-plugin").mkdir(parents=True)
        (r / ".claude-plugin" / "plugin.json").write_text(
            json.dumps({"name": "test", "version": "0.1.0"}, indent=2) + "\n"
        )

        # Two skills so path-map self-check has something to cover.
        skills = r / ".agents" / "skills"
        (skills / "ci").mkdir(parents=True)
        (skills / "cli-maintenance").mkdir(parents=True)
        (skills / "ci" / "SKILL.md").write_text("# ci skill\n")
        (skills / "cli-maintenance" / "SKILL.md").write_text("# cli-maintenance skill\n")

        # Some starter code to diff against.
        (r / "core" / "runtime" / "include" / "pulp" / "runtime").mkdir(parents=True)
        (r / "core" / "runtime" / "src").mkdir(parents=True)
        (r / "core" / "runtime" / "include" / "pulp" / "runtime" / "foo.hpp").write_text(
            "#pragma once\nint foo();\n"
        )
        (r / "core" / "runtime" / "src" / "foo.cpp").write_text(
            "int foo() { return 1; }\n"
        )

        # Test file path so test-only changes can be exercised.
        (r / "test").mkdir()
        (r / "test" / "test_foo.cpp").write_text("int main() { return 0; }\n")

        # tools/cli so cli-maintenance paths resolve.
        (r / "tools" / "cli").mkdir(parents=True)
        (r / "tools" / "cli" / "cmd_foo.cpp").write_text("// noop\n")

        (r / "tools" / "scripts").mkdir(parents=True)

        # versioning.json — minimal two-surface config.
        (r / "tools" / "scripts" / "versioning.json").write_text(json.dumps({
            "schema_version": 1,
            "surfaces": {
                "sdk": {
                    "label": "SDK",
                    "version_files": [
                        {"path": "CMakeLists.txt", "kind": "cmake_project_version"}
                    ],
                    "trigger_paths": [
                        "core/**/include/**/*.hpp",
                        "core/**/src/**/*.cpp",
                    ],
                    "public_api_paths": ["core/**/include/**/*.hpp"],
                    "internal_only_paths": ["core/**/src/**"],
                },
                "plugin": {
                    "label": "Plugin",
                    "version_files": [
                        {"path": ".claude-plugin/plugin.json",
                         "kind": "json_field", "field": "version"}
                    ],
                    "trigger_paths": [".claude-plugin/**"],
                    "public_api_paths": [".claude-plugin/plugin.json"],
                    "internal_only_paths": [],
                }
            },
            "skills": {
                "skills_dir": ".agents/skills",
                "path_map": "tools/scripts/skill_path_map.json",
            },
            "generated_globs": [
                "build/**",
                "**/*.generated.*",
            ],
            "trailers": {
                "version_bump": "Version-Bump",
                "skill_update": "Skill-Update",
                "release": "Release",
            },
        }, indent=2) + "\n")

        # skill_path_map.json covering both skills.
        (r / "tools" / "scripts" / "skill_path_map.json").write_text(json.dumps({
            "schema_version": 1,
            "skills": {
                "ci": {"paths": [".github/workflows/**", "ci/**"]},
                "cli-maintenance": {"paths": ["tools/cli/**"]},
            },
        }, indent=2) + "\n")

        _git(r, "add", ".")
        _git(r, "commit", "-q", "-m", "initial")
        # Create a parallel "origin/main" ref at this point so the scripts
        # can diff against it when subsequent commits land.
        _git(r, "update-ref", "refs/remotes/origin/main", "HEAD")

    def commit(self, message: str) -> None:
        _git(self.root, "add", "-A")
        _git(self.root, "commit", "-q", "-m", message)

    def write(self, rel: str, content: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)

    def run_ssc(self, extra: list[str] | None = None) -> tuple[int, str]:
        return _run(
            ["python3", str(SSC), "--base", "origin/main",
             "--config", str(self.root / "tools/scripts/versioning.json"),
             "--mode=report",
             *(extra or [])],
            cwd=self.root,
        )

    def run_vbc(self, extra: list[str] | None = None) -> tuple[int, str]:
        return _run(
            ["python3", str(VBC), "--base", "origin/main",
             "--config", str(self.root / "tools/scripts/versioning.json"),
             "--mode=report",
             *(extra or [])],
            cwd=self.root,
        )


# ── Tests ──────────────────────────────────────────────────────────────


class GateFixtureTests(unittest.TestCase):
    """Shared setup — one temp dir per test; rm -rf at teardown."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-gate-"))
        self.f = Fixture(self.tmp)
        self.f.init()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ── version_bump_check fixtures ────────────────────────────────────

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

    # ── skill_sync_check fixtures ──────────────────────────────────────

    def test_skill_path_touched_without_md_update_fails(self) -> None:
        self.f.write("tools/cli/cmd_foo.cpp", "// added content\nint x();\n")
        self.f.commit("cli: tweak cmd_foo")
        code, out = self.f.run_ssc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("cli-maintenance", out)
        self.assertIn("SKILL.md NOT updated", out)

    def test_skill_path_touched_with_md_update_passes(self) -> None:
        self.f.write("tools/cli/cmd_foo.cpp", "// added content\nint x();\n")
        self.f.write(".agents/skills/cli-maintenance/SKILL.md",
                     "# cli-maintenance skill\n\nNew gotcha: ...\n")
        self.f.commit("cli: tweak cmd_foo + record gotcha")
        code, out = self.f.run_ssc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("SKILL.md updated", out)

    def test_skill_update_bypass_trailer_passes(self) -> None:
        self.f.write("tools/cli/cmd_foo.cpp", "// added content\nint x();\n")
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'cli: mechanical rename\n\n'
             'Skill-Update: skip skill=cli-maintenance reason="mechanical rename"')
        code, out = self.f.run_ssc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("bypassed", out)

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


# ── Entry ──────────────────────────────────────────────────────────────


if __name__ == "__main__":
    unittest.main(verbosity=2)
