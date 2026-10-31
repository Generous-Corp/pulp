#!/usr/bin/env python3
"""Shared fixture support for the gate test modules.

Extracted from `test_gates.py` (P9-NEW refactor, 2026-05) so the
per-cluster `test_version_bump_*.py` modules and `test_skill_sync.py`
can share one throwaway-repo `Fixture` without duplicating it. The
helper bodies are byte-identical to their previous in-file definitions.

`test_gates.py` remains the aggregate entrypoint
(`python3 tools/scripts/test_gates.py`) and imports every `TestCase`
subclass from the cluster modules, so the full suite still runs as
one command with no extra deps on PEP-668 systems.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys  # noqa: F401  (re-exported; cluster tests reference it)
import tempfile
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
                        "tools/cli/**/*.cpp",
                        "tools/cli/**/*.hpp",
                    ],
                    "public_api_paths": ["core/**/include/**/*.hpp"],
                    "internal_only_paths": [
                        "core/**/src/**",
                        "tools/cli/**",
                    ],
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
            "writer_allowed_globs": [
                "CMakeLists.txt",
                ".claude-plugin/plugin.json",
                ".claude-plugin/marketplace.json",
                "CHANGELOG.md",
                "docs/migrations/v*.md",
                "docs/migrations/unreleased/*.md",
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


class GateFixtureTestCase(unittest.TestCase):
    """Shared setup — one temp dir per test; rm -rf at teardown.

    Cluster test modules subclass this so each gets an isolated
    throwaway repo via `self.f`.
    """

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-gate-"))
        self.f = Fixture(self.tmp)
        self.f.init()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _import_gate_module(self, name: str):
        scripts = str(REPO_ROOT / "tools" / "scripts")
        if scripts not in sys.path:
            sys.path.insert(0, scripts)
        return __import__(name)
