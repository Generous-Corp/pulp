#!/usr/bin/env python3
"""Unit tests for version_at_land.py.

The bot must reproduce what a hand-bump wrote, so it derives the level from the
SAME path + conventional-commit heuristic (`assess_surfaces`) the version-bump
gate and the `--mode=apply` writer use — NOT from positive `Version-Bump:`
intent trailers, which the hand-bump model uses only as `skip`/override escapes.

These tests build a real one-PR git range against the production
`versioning.json` surfaces and assert the assignment for each signal class:
feat -> minor, fix -> patch, public-API path -> minor, `skip` -> no bump,
positive override trailer -> that exact level. Against the previous
positive-trailer-only implementation every source-driven case here yields NO
assignment (the release-stranding defect), so they are RED before the fix.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import version_at_land as val  # noqa: E402
from version_bump_surfaces import load_config  # noqa: E402
from gate_common import git_diff_names  # noqa: E402
from version_bump_heuristics import filter_generated  # noqa: E402

CONFIG = load_config(HERE / "versioning.json")


def _git(repo: Path, *args: str, env=None) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True, env=env).stdout.strip()


class RangeRepo:
    """A throwaway git repo seeded with the two versioned surfaces, plus helpers
    to add commits and produce a (base, head) range like a single rebased PR."""

    def __init__(self, tmp: Path):
        self.repo = tmp
        env = os.environ.copy()
        env.update(GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@e",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@e")
        self.env = env
        _git(tmp, "init", "-q", "-b", "main", env=env)
        self.write("CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.20)\n"
                   "project(Pulp VERSION 1.2.3 LANGUAGES CXX)\n")
        self.write(".claude-plugin/plugin.json", '{\n  "version": "0.5.0"\n}\n')
        self.write(".claude-plugin/marketplace.json",
                   '{\n  "version": "0.5.0",\n'
                   '  "plugins": [\n    { "version": "0.5.0" }\n  ]\n}\n')
        self.commit("seed base state")
        self.base = self.head()

    def write(self, rel: str, content: str) -> None:
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)

    def commit(self, subject: str, body: str = "") -> str:
        _git(self.repo, "add", "-A", env=self.env)
        msg = subject if not body else f"{subject}\n\n{body}"
        _git(self.repo, "commit", "-q", "-m", msg, env=self.env)
        return self.head()

    def head(self) -> str:
        return _git(self.repo, "rev-parse", "HEAD", env=self.env)

    def plan(self):
        head = self.head()
        changed = filter_generated(git_diff_names(self.base, head),
                                   CONFIG.generated_globs)
        return val.plan_assignments(CONFIG, changed, self.base, head, self.repo)

    def assigned(self):
        return {a.surface: a.assigned for a in self.plan()}


class VersionAtLandTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        # Resolve symlinks (macOS /var -> /private/var) so git range refs and
        # `git -C` agree with cwd-relative helpers.
        self.repo = Path(self._tmp.name).resolve()
        self._cwd = os.getcwd()
        self.r = RangeRepo(self.repo)
        os.chdir(self.repo)  # assess_surfaces' git helpers run in cwd

    def tearDown(self):
        os.chdir(self._cwd)
        self._tmp.cleanup()

    # ── source-driven signal classes (RED before the fix) ─────────────────
    def test_feat_touching_src_is_minor(self):
        self.r.write("core/audio/src/mixer.cpp", "int mix() { return 1; }\n")
        self.r.commit("feat(audio): add a mixer")
        self.assertEqual(self.r.assigned(), {"sdk": "1.3.0"})

    def test_fix_touching_src_is_patch(self):
        self.r.write("core/audio/src/mixer.cpp", "int mix() { return 2; }\n")
        self.r.commit("fix(audio): correct the mixer")
        self.assertEqual(self.r.assigned(), {"sdk": "1.2.4"})

    def test_public_api_header_is_minor(self):
        self.r.write("core/audio/include/pulp/audio/mixer.hpp",
                     "#pragma once\nint mix();\n")
        self.r.commit("chore(audio): expose mixer header")  # no feat/fix subject
        self.assertEqual(self.r.assigned(), {"sdk": "1.3.0"})

    def test_plugin_command_public_api_is_minor(self):
        self.r.write(".claude/commands/newcmd.md",
                     "Run the new command and report status.\n")
        self.r.commit("docs(plugin): add a command")
        self.assertEqual(self.r.assigned(), {"plugin": "0.6.0"})

    def test_plugin_skill_internal_is_patch(self):
        self.r.write(".agents/skills/demo/SKILL.md", "# demo skill\nbody\n")
        self.r.commit("docs(skill): tweak demo skill")
        self.assertEqual(self.r.assigned(), {"plugin": "0.5.1"})

    # ── trailer overrides (skip suppresses, explicit level is authoritative) ─
    def test_skip_trailer_suppresses_bump(self):
        self.r.write("core/audio/src/mixer.cpp", "int mix() { return 3; }\n")
        self.r.commit("feat(audio): mixer with skip",
                      'Version-Bump: sdk=skip reason="generated artifact identical"')
        self.assertEqual(self.r.assigned(), {})

    def test_positive_override_wins(self):
        # Heuristic would be patch (internal src) but the author declared major.
        self.r.write("core/audio/src/mixer.cpp", "int mix() { return 4; }\n")
        self.r.commit("fix(audio): mixer",
                      'Version-Bump: sdk=major reason="breaks ABI"')
        self.assertEqual(self.r.assigned(), {"sdk": "2.0.0"})

    def test_override_can_lower_below_heuristic(self):
        # Public-API header heuristic is minor; author judges it patch.
        self.r.write("core/audio/include/pulp/audio/mixer.hpp",
                     "#pragma once\nint mix(int);\n")
        self.r.commit("feat(audio): mixer header",
                      'Version-Bump: sdk=patch reason="additive, still patch"')
        self.assertEqual(self.r.assigned(), {"sdk": "1.2.4"})

    # ── no-op / independence ──────────────────────────────────────────────
    def test_non_trigger_change_is_noop(self):
        self.r.write("docs/guide.md", "just docs\n")
        self.r.commit("docs: unrelated")
        self.assertEqual(self.r.assigned(), {})

    def test_both_surfaces_independent(self):
        self.r.write("core/audio/src/mixer.cpp", "int mix() { return 5; }\n")
        self.r.write(".claude/commands/newcmd.md", "Run the command.\n")
        self.r.commit("feat: sdk + plugin",
                      body="feat(audio): mixer\n\nfeat(plugin): command")
        self.assertEqual(self.r.assigned(), {"sdk": "1.3.0", "plugin": "0.6.0"})

    def test_bumps_from_base_not_head(self):
        # A hand-bump already sitting in the range must NOT be re-bumped off the
        # HEAD value: the floor is read at base (1.2.3), never the mid-range 9.9.9.
        self.r.write("core/audio/src/mixer.cpp", "int mix() { return 6; }\n")
        self.r.commit("feat(audio): mixer")
        self.r.write("CMakeLists.txt",
                     "cmake_minimum_required(VERSION 3.20)\n"
                     "project(Pulp VERSION 9.9.9 LANGUAGES CXX)\n")
        self.r.commit("chore: bump versions")
        self.assertEqual(self.r.assigned()["sdk"], "1.3.0")


if __name__ == "__main__":
    unittest.main()
