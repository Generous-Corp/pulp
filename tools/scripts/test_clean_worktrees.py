#!/usr/bin/env python3
"""Fixture tests for tools/scripts/clean_worktrees.sh.

The script DELETES worktrees, so its gate is the thing under test. Each case
builds a throwaway git repository containing real worktrees in every state the
gate distinguishes, and asserts what survives as hard as what goes. Nothing
outside the temporary directory is reachable: --repo points at the fixture, so
no real worktree is enumerated.

Covered:
  1. Dry-run classifies and deletes nothing (exit 0).
  2. --yes removes a clean worktree contained in origin/main, and ONLY that —
     dirty, unmerged, lineage-active and busy worktrees all survive.
  3. Containment is exact ancestry: an unmerged branch survives even though its
     worktree is clean, because a clean tree is not merge proof.
  4. Uncommitted content vetoes removal and is reported as at risk.
  5. Liveness gating: a worktree named on a live process's command line is kept.
  6. Lineage `active` vetoes removal (a discovery aid used only as a veto).
  7. The primary worktree is never a candidate.
  8. Preconditions refuse rather than guess: a repo with no origin/main exits 2
     without removing anything.
  9. Unknown argument exits 2; --help exits 0; --repo requires a value.

Run:
    python3 tools/scripts/test_clean_worktrees.py
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import tempfile
import time
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "clean_worktrees.sh"


def git(cwd: pathlib.Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args],
        cwd=str(cwd),
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


class CleanWorktreesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

        # A bare "remote" so origin/main is a real remote-tracking ref rather
        # than a local alias — the script's containment baseline must be the
        # published trunk, not whatever this checkout happens to call main.
        self.origin = self.root / "origin.git"
        subprocess.run(
            ["git", "init", "-q", "--bare", "-b", "main", str(self.origin)],
            check=True, capture_output=True,
        )

        self.primary = self.root / "primary"
        subprocess.run(
            ["git", "clone", "-q", str(self.origin), str(self.primary)],
            check=True, capture_output=True,
        )
        git(self.primary, "config", "user.email", "t@example.com")
        git(self.primary, "config", "user.name", "t")

        (self.primary / "seed.txt").write_text("seed\n")
        git(self.primary, "add", "seed.txt")
        git(self.primary, "commit", "-qm", "seed")
        git(self.primary, "push", "-q", "origin", "main")
        git(self.primary, "fetch", "-q", "origin")
        self.main_sha = git(self.primary, "rev-parse", "origin/main")

        # Contained: branches at the trunk tip. Every commit is already in
        # origin/main, so removing these loses nothing.
        self.merged = self._worktree("wt-merged", self.main_sha)
        self.busy = self._worktree("wt-busy", self.main_sha)
        self.active = self._worktree("wt-active", self.main_sha)
        self.dirty = self._worktree("wt-dirty", self.main_sha)
        (self.dirty / "scratch.txt").write_text("work that is in no ref\n")

        # Not contained: one unique commit that exists here and nowhere else.
        self.unmerged = self._worktree("wt-unmerged", self.main_sha)
        (self.unmerged / "unique.txt").write_text("unique\n")
        git(self.unmerged, "add", "unique.txt")
        git(self.unmerged, "commit", "-qm", "unique work")

        git(self.primary, "config", "branch.wt-active.pulpworktreeStatus", "active")

    def _worktree(self, name: str, base: str) -> pathlib.Path:
        path = self.root / name
        git(self.primary, "worktree", "add", "-q", "-b", name, str(path), base)
        return path

    def stage_busy_process(self) -> subprocess.Popen:
        """Stage a live process that names the busy worktree on its argv.

        A padded-argv Python sleeper, not `sh -c '...' <path>`: a shell given a
        single command may exec-replace itself and drop the argv[0] marker, so
        the fixture would silently stage nothing and the gate would look broken
        when it was the instrument. The settle wait matters for the same reason
        — a just-spawned process is not instantly visible to ps.
        """
        proc = subprocess.Popen(
            ["python3", "-c", "import time; time.sleep(30)", "cmake", "-B", str(self.busy)]
        )
        self.addCleanup(proc.kill)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            listing = subprocess.run(
                ["ps", "-e", "-ww", "-o", "args="], capture_output=True, text=True
            ).stdout
            if any(str(self.busy) in line and "ps -e" not in line for line in listing.splitlines()):
                return proc
            time.sleep(0.1)
        proc.kill()
        self.fail("could not stage a live process naming the busy worktree")

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", str(SCRIPT), "--repo", str(self.primary), *args],
            cwd=str(self.root),
            capture_output=True,
            text=True,
            env=dict(os.environ, GIT_CONFIG_NOSYSTEM="1"),
        )

    def assert_survivors(self) -> None:
        """Everything the gate must protect, in every case."""
        for path in (self.dirty, self.unmerged, self.active, self.primary):
            self.assertTrue(path.is_dir(), f"{path.name} must survive")
        self.assertTrue(
            (self.dirty / "scratch.txt").is_file(),
            "uncommitted content must never be destroyed",
        )
        self.assertTrue((self.unmerged / "unique.txt").is_file())

    def test_dry_run_deletes_nothing(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("dry-run", result.stdout)
        self.assertIn(str(self.merged), result.stdout)
        self.assertTrue(self.merged.is_dir(), "dry-run must not delete")
        self.assert_survivors()

    def test_apply_removes_only_the_contained_clean_idle_worktree(self) -> None:
        result = self.run_script("--yes")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.merged.is_dir(), "a contained clean worktree is removable")
        self.assert_survivors()

        # The registry must agree with the filesystem: a removed worktree is
        # deregistered, and a control that must survive is still listed.
        listing = git(self.primary, "worktree", "list")
        self.assertNotIn(str(self.merged), listing)
        self.assertIn(str(self.dirty), listing)

    def test_removal_records_lineage_before_the_directory_goes(self) -> None:
        self.run_script("--yes")
        # The lineage record is branch-local and outlives the directory, so the
        # disposition must still be readable after removal.
        status = subprocess.run(
            ["git", "config", "--get", "branch.wt-merged.pulpworktreeStatus"],
            cwd=str(self.primary), capture_output=True, text=True,
        ).stdout.strip()
        self.assertEqual(status, "merged")

    def test_uncommitted_content_is_reported_as_at_risk(self) -> None:
        result = self.run_script()
        self.assertIn("AT RISK", result.stdout)
        self.assertIn(str(self.dirty), result.stdout)
        self.assertIn(str(self.unmerged), result.stdout)

    def test_live_process_vetoes_removal(self) -> None:
        proc = self.stage_busy_process()
        try:
            result = self.run_script("--yes")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(self.busy.is_dir(), "a busy worktree must be kept")
            self.assertIn("BUSY", result.stdout)
        finally:
            proc.kill()
            proc.wait()

    def test_lineage_active_vetoes_removal(self) -> None:
        result = self.run_script("--yes")
        self.assertTrue(self.active.is_dir())
        self.assertIn("ACTIVE", result.stdout)

    def test_json_counts_are_consistent_with_the_classification(self) -> None:
        proc = self.stage_busy_process()
        try:
            result = self.run_script("--json")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
        finally:
            proc.kill()
            proc.wait()
        self.assertEqual(report["baseline"], self.main_sha)
        self.assertEqual(report["removable"], 1)
        self.assertEqual(report["dirty"], 1)
        self.assertEqual(report["unmerged"], 1)
        self.assertEqual(report["busy"], 1)
        self.assertEqual(report["active"], 1)
        self.assertFalse(report["applied"])

    def test_missing_baseline_refuses_rather_than_guesses(self) -> None:
        solo = self.root / "solo"
        subprocess.run(
            ["git", "init", "-q", "-b", "main", str(solo)], check=True, capture_output=True
        )
        git(solo, "config", "user.email", "t@example.com")
        git(solo, "config", "user.name", "t")
        (solo / "f.txt").write_text("x\n")
        git(solo, "add", "f.txt")
        git(solo, "commit", "-qm", "only")
        result = subprocess.run(
            ["bash", str(SCRIPT), "--repo", str(solo), "--yes"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("origin/main", result.stderr)
        self.assertIn("nothing removed", result.stderr)

    def test_argument_handling(self) -> None:
        bad = self.run_script("--wat")
        self.assertEqual(bad.returncode, 2)
        self.assertIn("unknown argument", bad.stderr)
        self.assert_survivors()

        helped = subprocess.run(
            ["bash", str(SCRIPT), "--help"], capture_output=True, text=True
        )
        self.assertEqual(helped.returncode, 0)
        self.assertIn("Usage:", helped.stdout)

        missing = subprocess.run(
            ["bash", str(SCRIPT), "--repo"], capture_output=True, text=True
        )
        self.assertEqual(missing.returncode, 2)
        self.assertIn("requires a value", missing.stderr)

        notrepo = subprocess.run(
            ["bash", str(SCRIPT), "--repo", str(self.root / "nope")],
            capture_output=True, text=True,
        )
        self.assertEqual(notrepo.returncode, 2)


if __name__ == "__main__":
    unittest.main()
