#!/usr/bin/env python3
"""Unit tests for version_at_land.py.

The bot must reproduce what a hand-bump wrote, so it derives the level from the
SAME path + conventional-commit heuristic (`assess_surfaces`) the version-bump
gate and the `--mode=apply` writer use — NOT from positive `Version-Bump:`
intent trailers, which the hand-bump model uses only as `skip`/override escapes.

The source-driven cases build a real one-PR git range against the production
`versioning.json` surfaces and assert the assignment for each signal class:
feat -> minor, fix -> patch, public-API path -> minor, `skip` -> no bump,
positive override trailer -> that exact level. Against the previous
positive-trailer-only implementation every source-driven case here yields NO
assignment (the release-stranding defect), so they are RED before the fix.

Driven through a real git repo (not injected dicts), the fixture tests also
cover the single-writer transaction's release-critical invariant: two
concurrent post-merge drains must not lose or duplicate a version (the
`--ff-only` + recompute-per-attempt + `Version-Bump-Applied` marker path).
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
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


# ── Git-fixture tests (real range walk + real push transaction) ──────────

# NOTE: only `Fixture` is imported here — the module-local `_git` above returns
# stdout (needed by `RangeRepo` to read SHAs) and is a superset of
# gate_test_support's `_git`, so importing the latter would shadow it.
from gate_test_support import Fixture  # noqa: E402

SCRIPT = HERE / "version_at_land.py"


def _run_script(repo: Path, *args: str) -> tuple[int, str, str]:
    p = subprocess.run(
        ["python3", str(SCRIPT), "--config",
         str(repo / "tools/scripts/versioning.json"), *args],
        cwd=repo, capture_output=True, text=True,
    )
    return p.returncode, p.stdout, p.stderr


class ScriptDryRunTest(unittest.TestCase):
    """End-to-end through the CLI (`main`/argparse), not just in-process
    `plan_assignments`: a real one-commit range drives an assignment via the
    live heuristic."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-val-cli-"))
        self.f = Fixture(self.tmp)
        self.f.init()
        self.base = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_source_change_drives_an_assignment(self) -> None:
        # A genuine user-facing change over the range must drive an assignment
        # through the full CLI path (feat on a public source path -> minor).
        (self.tmp / "core/runtime/src/foo.cpp").write_text("int foo(){return 3;}\n")
        _git(self.tmp, "add", "--", "core/runtime/src/foo.cpp")
        _git(self.tmp, "commit", "-q", "-m", "feat(core): real change")
        rc, out, err = _run_script(
            self.tmp, "--base", self.base, "--head", "HEAD",
            "--mode", "dry-run", "--json")
        self.assertEqual(rc, 0, err)
        plan = json.loads(out)
        self.assertEqual(len(plan), 1)
        self.assertEqual((plan[0]["surface"], plan[0]["assigned"]),
                         ("sdk", "0.2.0"))

    def test_non_source_change_is_noop(self) -> None:
        (self.tmp / "docs" / "guide.md").parent.mkdir(parents=True, exist_ok=True)
        (self.tmp / "docs" / "guide.md").write_text("just docs\n")
        _git(self.tmp, "add", "--", "docs/guide.md")
        _git(self.tmp, "commit", "-q", "-m", "docs: unrelated")
        rc, out, err = _run_script(
            self.tmp, "--base", self.base, "--head", "HEAD",
            "--mode", "dry-run", "--json")
        self.assertEqual(rc, 0, err)
        self.assertEqual(json.loads(out), [])


class ConcurrentDrainRaceTest(unittest.TestCase):
    """Two post-merge drains racing on the same origin must apply the bump
    exactly once — no lost update, no double bump."""

    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="pulp-val-race-"))
        # 1. Seed repo (Fixture) → 2. bare origin → 3. two clones.
        self.seed = self.root / "seed"
        self.seed.mkdir()
        Fixture(self.seed).init()

        self.origin = self.root / "origin.git"
        subprocess.run(["git", "clone", "--bare", "-q",
                        str(self.seed), str(self.origin)], check=True)

        # A fix PR lands on origin: an intent trailer, NO version-file edit.
        land = self.root / "land"
        subprocess.run(["git", "clone", "-q", str(self.origin), str(land)],
                       check=True)
        self._config(land)
        (land / "core/runtime/src/foo.cpp").write_text("int foo(){return 9;}\n")
        _git(land, "add", "--", "core/runtime/src/foo.cpp")
        _git(land, "commit", "-q", "-m",
             "feat(core): user-facing change\n\nVersion-Bump: sdk=minor\n")
        _git(land, "push", "-q", "origin", "HEAD:main")

        self.cloneA = self.root / "cloneA"
        self.cloneB = self.root / "cloneB"
        # Distinct identities so the two bots' bump commits get DIFFERENT SHAs
        # even when produced in the same second — otherwise identical
        # tree+parent+message+time collapse to one SHA and B's push degenerates
        # into a fast-forward no-op, masking the non-ff rejection we test.
        subprocess.run(["git", "clone", "-q", str(self.origin), str(self.cloneA)],
                       check=True)
        self._config(self.cloneA, "bot-a")
        subprocess.run(["git", "clone", "-q", str(self.origin), str(self.cloneB)],
                       check=True)
        self._config(self.cloneB, "bot-b")

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    @staticmethod
    def _config(repo: Path, who: str = "bot") -> None:
        _git(repo, "config", "user.email", f"{who}@example.com")
        _git(repo, "config", "user.name", who)
        _git(repo, "config", "commit.gpgsign", "false")

    def _load_for(self, repo: Path):
        return load_config(repo / "tools/scripts/versioning.json")

    def _origin_cmake_version(self) -> str:
        import re
        show = subprocess.run(
            ["git", "-C", str(self.origin), "show", "main:CMakeLists.txt"],
            capture_output=True, text=True, check=True).stdout
        return re.search(r"VERSION (\d+\.\d+\.\d+)", show).group(1)

    def _origin_bump_commit_count(self) -> int:
        out = subprocess.run(
            ["git", "-C", str(self.origin), "log", "main",
             "--grep", "chore: bump versions", "--format=%H"],
            capture_output=True, text=True, check=True).stdout
        return len([x for x in out.splitlines() if x.strip()])

    def test_concurrent_runs_apply_once(self) -> None:
        # Run B, but on B's first push attempt, land A's whole transaction
        # first so B's push loses the fast-forward race.
        a_status = {}

        def land_A_first(attempt: int) -> None:
            if attempt != 0 or a_status:
                return
            cfgA = self._load_for(self.cloneA)
            a_status["status"], a_status["plan"] = val.apply_and_push(
                self.cloneA, cfgA, remote="origin", branch="main",
                max_retries=3)

        cfgB = self._load_for(self.cloneB)
        b_status, b_plan = val.apply_and_push(
            self.cloneB, cfgB, remote="origin", branch="main",
            max_retries=3, on_before_push=land_A_first)

        # A won the race and applied; B recomputed and no-oped.
        self.assertEqual(a_status.get("status"), "applied", a_status)
        self.assertEqual(b_status, "noop",
                         "B double-bumped or clobbered A's push")
        # Applied EXACTLY once: 0.1.0 -> 0.2.0 (never 0.3.0), one bump commit.
        self.assertEqual(self._origin_cmake_version(), "0.2.0")
        self.assertEqual(self._origin_bump_commit_count(), 1)

    def test_push_rejection_detail_is_surfaced(self) -> None:
        # A rejected push must print git's real reason, not just the caller's
        # generic "branch kept moving" summary — otherwise a NON-race rejection
        # (branch ruleset / merge-queue / auth) is invisible in the run log.
        a_status = {}

        def land_A_first(attempt: int) -> None:
            if attempt != 0 or a_status:
                return
            cfgA = self._load_for(self.cloneA)
            a_status["status"], a_status["plan"] = val.apply_and_push(
                self.cloneA, cfgA, remote="origin", branch="main",
                max_retries=3)

        cfgB = self._load_for(self.cloneB)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            b_status, _ = val.apply_and_push(
                self.cloneB, cfgB, remote="origin", branch="main",
                max_retries=3, on_before_push=land_A_first)
        self.assertEqual(b_status, "noop")

        captured = err.getvalue()
        # The attempt-level line names the losing attempt and its exit code.
        self.assertIn("push attempt 0 rejected", captured)
        # And git's own rejection text is echoed (a non-ff push says "rejected"
        # and hints "fetch first" / "[rejected]").
        self.assertRegex(captured.lower(), r"reject|fetch first|non-fast")

    def test_single_run_applies_and_is_idempotent_on_rerun(self) -> None:
        cfgA = self._load_for(self.cloneA)
        status, plan = val.apply_and_push(self.cloneA, cfgA)
        self.assertEqual(status, "applied")
        self.assertEqual(self._origin_cmake_version(), "0.2.0")
        # Re-run against an origin that already carries the marker → no-op.
        status2, _ = val.apply_and_push(self.cloneB, self._load_for(self.cloneB))
        self.assertEqual(status2, "noop")
        self.assertEqual(self._origin_cmake_version(), "0.2.0")
        self.assertEqual(self._origin_bump_commit_count(), 1)


if __name__ == "__main__":
    unittest.main()
