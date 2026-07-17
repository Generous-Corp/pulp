#!/usr/bin/env python3
"""Unit tests for version_at_land.py.

Covers the pure logic (aggregate_intent, plan_assignments) AND — driven
through a real git repo, not injected dicts — the two release-critical
invariants of the single-writer bot:

  * a stray `Version-Bump:` trailer on a re-sync MERGE commit must NOT
    escalate the assigned version (the trailer-scoping fix), and
  * two concurrent post-merge drains must not lose or duplicate a version
    (the `--ff-only` + recompute-per-attempt + `Version-Bump-Applied`
    marker transaction).
"""

from __future__ import annotations

import importlib.util
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


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


val = _load("version_at_land")
surfaces = _load("version_bump_surfaces")

Surface = surfaces.Surface
VersionFile = surfaces.VersionFile


def trailers(*values):
    return {"version-bump": list(values)}


def surface(name):
    return Surface(name=name, label=name,
                   version_files=[VersionFile(path=f"{name}.txt", kind="raw")],
                   trigger_paths=[])


class AggregateIntentTest(unittest.TestCase):
    def test_absent_is_none(self):
        self.assertIsNone(val.aggregate_intent(trailers(), "sdk"))

    def test_single_level(self):
        self.assertEqual(val.aggregate_intent(trailers('sdk=minor reason="x"'), "sdk"), "minor")

    def test_highest_wins_across_range(self):
        t = trailers('sdk=patch reason="a"', 'sdk=major reason="b"', 'sdk=minor reason="c"')
        self.assertEqual(val.aggregate_intent(t, "sdk"), "major")

    def test_skip_is_ignored(self):
        self.assertIsNone(val.aggregate_intent(trailers('sdk=skip reason="x"'), "sdk"))

    def test_skip_does_not_beat_real_level(self):
        t = trailers('sdk=skip reason="x"', 'sdk=patch reason="y"')
        self.assertEqual(val.aggregate_intent(t, "sdk"), "patch")

    def test_other_surface_not_picked_up(self):
        self.assertIsNone(val.aggregate_intent(trailers('plugin=minor reason="x"'), "sdk"))

    def test_unknown_level_ignored(self):
        self.assertIsNone(val.aggregate_intent(trailers('sdk=bogus reason="x"'), "sdk"))


class PlanAssignmentsTest(unittest.TestCase):
    def setUp(self):
        self.config = surfaces.Config(
            surfaces=[surface("sdk"), surface("plugin")],
            generated_globs=[], trailer_version_bump="Version-Bump")
        self.versions = {"sdk": "0.599.0", "plugin": "0.308.1"}

    def plan(self, t):
        return val.plan_assignments(self.config, t, lambda s: self.versions[s.name])

    def test_single_surface_intent(self):
        p = self.plan(trailers('sdk=minor reason="x"'))
        self.assertEqual(len(p), 1)
        self.assertEqual((p[0].surface, p[0].current, p[0].assigned), ("sdk", "0.599.0", "0.600.0"))

    def test_both_surfaces_independent(self):
        p = {a.surface: a for a in self.plan(
            trailers('sdk=patch reason="x"', 'plugin=major reason="y"'))}
        self.assertEqual(p["sdk"].assigned, "0.599.1")
        self.assertEqual(p["plugin"].assigned, "1.0.0")

    def test_no_intent_no_assignment(self):
        self.assertEqual(self.plan(trailers()), [])

    def test_skip_only_surface_gets_no_assignment(self):
        self.assertEqual(self.plan(trailers('sdk=skip reason="x"')), [])

    def test_unreadable_version_is_skipped(self):
        p = val.plan_assignments(self.config, trailers('sdk=minor reason="x"'),
                                 lambda s: None)
        self.assertEqual(p, [])


# ── Git-fixture tests (real range walk + real push transaction) ──────────

from gate_test_support import Fixture, _git  # noqa: E402

SCRIPT = HERE / "version_at_land.py"


def _cmake_version(repo: Path) -> str:
    import re
    return re.search(r"VERSION (\d+\.\d+\.\d+)",
                     (repo / "CMakeLists.txt").read_text()).group(1)


def _run_script(repo: Path, *args: str) -> tuple[int, str, str]:
    p = subprocess.run(
        ["python3", str(SCRIPT), "--config",
         str(repo / "tools/scripts/versioning.json"), *args],
        cwd=repo, capture_output=True, text=True,
    )
    return p.returncode, p.stdout, p.stderr


class MergeCommitTrailerScopingTest(unittest.TestCase):
    """A stray `Version-Bump:` trailer on a re-sync merge commit must not
    escalate — the ce17af6ad shape."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-val-scope-"))
        self.f = Fixture(self.tmp)
        self.f.init()
        self.base = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _sha(self, ref: str = "HEAD") -> str:
        return subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", ref],
            capture_output=True, text=True, check=True).stdout.strip()

    def _make_resync_merge_with_stray_intent(self) -> None:
        # main: a normal PR commit with NO intent.
        (self.tmp / "core/runtime/src/foo.cpp").write_text("int foo(){return 2;}\n")
        _git(self.tmp, "add", "--", "core/runtime/src/foo.cpp")
        _git(self.tmp, "commit", "-q", "-m", "fix(core): tweak foo")
        main_after = self._sha()

        # side branch off the ORIGINAL base, diverging so the merge is real.
        _git(self.tmp, "checkout", "-q", "-b", "side", self.base)
        (self.tmp / "core/runtime/src/bar.cpp").write_text("int bar(){return 1;}\n")
        _git(self.tmp, "add", "--", "core/runtime/src/bar.cpp")
        _git(self.tmp, "commit", "-q", "-m", "chore(core): add bar")

        # Back on main, merge side with a stray intent trailer on the MERGE
        # commit — exactly what a "Merge origin/main into <branch>" re-sync
        # commit carries when a rebase/union-resolve declares intent.
        _git(self.tmp, "checkout", "-q", "main")
        _git(self.tmp, "merge", "--no-ff", "-q", "side", "-m",
             "Merge side into main\n\nVersion-Bump: sdk=minor\n")

    def test_stray_merge_trailer_does_not_escalate(self) -> None:
        self._make_resync_merge_with_stray_intent()
        # Sanity: with merges INCLUDED the stray trailer IS visible — so the
        # ONLY thing preventing escalation is the no-merges scoping. (Revert
        # that scoping and the script test below flips to a non-empty plan.)
        with_merges = val.git_range_trailers(
            self.base, "HEAD", no_merges=False, cwd=self.tmp)
        self.assertIn("sdk=minor",
                      " ".join(with_merges.get("version-bump", [])))

        rc, out, err = _run_script(
            self.tmp, "--base", self.base, "--head", "HEAD",
            "--mode", "dry-run", "--json")
        self.assertEqual(rc, 0, err)
        self.assertEqual(json.loads(out), [],
                         "stray intent on a merge commit escalated the release")

    def test_intent_on_a_normal_commit_is_still_honored(self) -> None:
        # Guard against over-filtering: a genuine intent on a non-merge commit
        # must still drive an assignment.
        (self.tmp / "core/runtime/src/foo.cpp").write_text("int foo(){return 3;}\n")
        _git(self.tmp, "add", "--", "core/runtime/src/foo.cpp")
        _git(self.tmp, "commit", "-q", "-m",
             "feat(core): real change\n\nVersion-Bump: sdk=minor\n")
        rc, out, err = _run_script(
            self.tmp, "--base", self.base, "--head", "HEAD",
            "--mode", "dry-run", "--json")
        self.assertEqual(rc, 0, err)
        plan = json.loads(out)
        self.assertEqual(len(plan), 1)
        self.assertEqual((plan[0]["surface"], plan[0]["assigned"]),
                         ("sdk", "0.2.0"))


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
        cfg = surfaces.load_config(repo / "tools/scripts/versioning.json")
        return cfg

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
