#!/usr/bin/env python3
"""Fixture tests for tools/scripts/clean_lane_builds.py.

The tool deletes directories, so every test builds a throwaway lane tree of
real git repositories (remote-tracking refs are planted by hand, nothing is
fetched) and drives the real script against it with stubbed `ghread`,
`ghapp` and `lsof`. Nothing outside the temporary directory is touched.

Each guard has a pair: a control where the guard is satisfied and the entry is
reclaimable, and a case where only that guard is violated and the entry is
held. The control matters as much as the case: a held result from a tool that
holds everything proves nothing.

Run:
    python3 tools/scripts/test_clean_lane_builds.py
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "clean_lane_builds.py"
sys.path.insert(0, str(HERE))
import clean_lane_builds as reaper  # noqa: E402

DAY = 86400
GH = "git@github.com:Generous-Corp/fake.git"
GH_PERSONAL = "git@github.com:danielraffel/fake.git"

PR_STUB = """#!/bin/sh
echo "$0 $*" >> "$STUB_LOG"
sha=$(echo "$2" | sed -E 's#.*/commits/([0-9a-f]+)/pulls#\\1#')
if [ -f "$STUB_PR_DIR/$sha.fail" ]; then exit 1; fi
if [ -f "$STUB_PR_DIR/$sha.json" ]; then cat "$STUB_PR_DIR/$sha.json"; else echo '[]'; fi
"""

LSOF_STUB = """#!/bin/sh
if [ -z "$STUB_LSOF_BLIND" ]; then echo "p$PPID"; fi
echo "n/"
if [ -f "$STUB_LSOF_EXTRA" ]; then cat "$STUB_LSOF_EXTRA"; fi
"""


def sh(cwd, *args, env=None):
    e = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@e",
             GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@e")
    e.update(env or {})
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True, env=e).stdout.strip()


def age(path: pathlib.Path, days: float) -> None:
    """Backdate every file and directory under path, children first."""
    when = time.time() - days * DAY
    for dirpath, dirs, files in os.walk(path, topdown=False):
        for n in files + dirs:
            try:
                os.utime(os.path.join(dirpath, n), (when, when), follow_symlinks=False)
            except OSError:
                pass
    os.utime(path, (when, when))


class Base(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._td.name).resolve()
        self.root = self.tmp / "lanes"
        self.root.mkdir()
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.prdir = self.tmp / "prs"
        self.prdir.mkdir()
        self.log = self.tmp / "calls.log"
        self.log.write_text("")
        self.lsof_extra = self.tmp / "lsof-extra"
        for name, body in (("ghread", PR_STUB), ("ghapp", PR_STUB), ("lsof", LSOF_STUB)):
            p = self.bin / name
            p.write_text(body)
            p.chmod(0o755)
        self.env = {
            "PULP_REAPER_GHREAD": str(self.bin / "ghread"),
            "PULP_REAPER_GHAPP": str(self.bin / "ghapp"),
            "PULP_REAPER_LSOF": str(self.bin / "lsof"),
            "STUB_LOG": str(self.log),
            "STUB_PR_DIR": str(self.prdir),
            "STUB_LSOF_EXTRA": str(self.lsof_extra),
        }
        self.cache = self.tmp / "cache.json"

    def tearDown(self):
        self._td.cleanup()

    # -- fixtures ----------------------------------------------------------
    def lane(self, name, *, remote=GH, merged=True, build=True, artifact=False,
             age_days=3.0, tracked_build=False):
        """A lane with one checkout at <lane>/repo, aged `age_days`."""
        lane = self.root / name
        repo = lane / "repo"
        repo.mkdir(parents=True)
        sh(repo, "git", "init", "-q", "-b", "main")
        (repo / "src.txt").write_text("one\n")
        if tracked_build:
            (repo / "build").mkdir()
            (repo / "build" / "kept.txt").write_text("tracked\n")
        sh(repo, "git", "add", "-A")
        sh(repo, "git", "commit", "-q", "-m", f"base {name}")
        sh(repo, "git", "remote", "add", "origin", remote)
        sh(repo, "git", "update-ref", "refs/remotes/origin/main", "HEAD")
        sh(repo, "git", "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main")
        if not merged:
            (repo / "feature.txt").write_text("work\n")
            sh(repo, "git", "add", "feature.txt")
            sh(repo, "git", "commit", "-q", "-m", "unmerged work")
            sh(repo, "git", "update-ref", "refs/remotes/origin/feature", "HEAD")
        if build:
            (repo / "build").mkdir(exist_ok=True)
            (repo / "build" / "obj.o").write_text("x" * 4096)
        if artifact:
            (repo / "Foo.pkg").write_text("p" * 200_000)
        age(lane, age_days)
        return repo

    def head(self, repo):
        return sh(repo, "git", "rev-parse", "HEAD")

    def pr(self, repo, *states):
        prs = []
        for st in states:
            prs.append({"state": "open" if st == "open" else "closed",
                        "merged_at": "2026-11-01T00:00:00Z" if st == "merged" else None})
        (self.prdir / f"{self.head(repo)}.json").write_text(json.dumps(prs))

    def run_reaper(self, *extra, expect_rc=0, env_extra=None):
        env = {**os.environ, **self.env, **(env_extra or {})}
        p = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), "--json",
                            "--cache-file", str(self.cache), "--min-artifact-mb", "0.1", *extra],
                           capture_output=True, text=True, env=env)
        self.assertEqual(p.returncode, expect_rc, p.stdout + p.stderr)
        return json.loads(p.stdout) if expect_rc == 0 else p

    def entry(self, report, lane, kind, name=None):
        rows = report["lanes"].get(lane, {"entries": []})["entries"]
        for e in rows:
            if e["kind"] == kind and (name is None or pathlib.Path(e["path"]).name == name):
                return e
        return None

    def assertReclaim(self, report, lane, kind, name=None):
        e = self.entry(report, lane, kind, name)
        self.assertIsNotNone(e, f"no {kind} entry for {lane}: {report['lanes'].get(lane)}")
        self.assertTrue(e["reclaimable"], e["reasons"])

    def assertHeld(self, report, lane, kind, name=None, why=None):
        e = self.entry(report, lane, kind, name)
        self.assertIsNotNone(e, f"no {kind} entry for {lane}: {report['lanes'].get(lane)}")
        self.assertFalse(e["reclaimable"], "expected held")
        if why:
            self.assertIn(why, " ".join(e["reasons"]))


class BuildGuards(Base):
    def test_control_merged_idle_clean_build_is_reclaimable(self):
        self.lane("a")
        r = self.run_reaper()
        self.assertReclaim(r, "a", "build", "build")
        self.assertGreater(r["reclaimable_bytes"], 0)

    def test_dry_run_deletes_nothing(self):
        repo = self.lane("a")
        self.run_reaper()
        self.assertTrue((repo / "build" / "obj.o").exists())

    def test_unmerged_without_pr_is_held(self):
        self.lane("a", merged=False)
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="no PR")

    def test_open_pr_is_held(self):
        repo = self.lane("a", merged=False)
        self.pr(repo, "open")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="open")

    def test_open_pr_beats_a_merged_one(self):
        repo = self.lane("a", merged=False)
        self.pr(repo, "merged", "open")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="open")

    def test_merged_pr_is_reclaimable(self):
        repo = self.lane("a", merged=False)
        self.pr(repo, "merged")
        self.assertReclaim(self.run_reaper(), "a", "build", "build")

    def test_closed_pr_is_reclaimable(self):
        repo = self.lane("a", merged=False)
        self.pr(repo, "closed")
        self.assertReclaim(self.run_reaper(), "a", "build", "build")

    def test_pr_lookup_failure_is_held(self):
        repo = self.lane("a", merged=False)
        (self.prdir / f"{self.head(repo)}.fail").write_text("")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="error")

    def test_unknown_owner_is_held(self):
        repo = self.lane("a", merged=False, remote="git@github.com:someone-else/x.git")
        self.pr(repo, "merged")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="no PR provider")

    def test_unparseable_remote_is_held(self):
        repo = self.lane("a", merged=False, remote="/some/local/path")
        self.pr(repo, "merged")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="recognised GitHub remote")

    def test_provider_routing_and_cache(self):
        a = self.lane("a", merged=False, remote=GH)
        b = self.lane("b", merged=False, remote=GH_PERSONAL)
        self.pr(a, "merged")
        self.pr(b, "merged")
        r1 = self.run_reaper()
        self.assertEqual(r1["api_calls"], 2)
        calls = self.log.read_text().splitlines()
        self.assertEqual(sum(1 for c in calls if "/ghread " in c), 1)
        self.assertEqual(sum(1 for c in calls if "/ghapp " in c), 1)
        # Terminal states are cached: a second run makes no API call.
        r2 = self.run_reaper()
        self.assertEqual(r2["api_calls"], 0)
        self.assertEqual(len(self.log.read_text().splitlines()), 2)
        self.assertReclaim(r2, "a", "build", "build")

    def test_ancestry_needs_no_api_call(self):
        self.lane("a")
        self.assertEqual(self.run_reaper()["api_calls"], 0)

    def test_recent_source_edit_is_held(self):
        repo = self.lane("a")
        os.utime(repo / "src.txt")  # touched now
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="idle window")

    def test_recent_write_inside_build_is_held(self):
        repo = self.lane("a")
        (repo / "build" / "fresh.o").write_text("y")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="idle window")

    def test_idle_window_is_configurable(self):
        self.lane("a", age_days=1.0)
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="idle window")
        self.assertReclaim(self.run_reaper("--idle-hours", "12"), "a", "build", "build")

    def test_live_process_in_checkout_is_held(self):
        repo = self.lane("a")
        self.lsof_extra.write_text(f"n{repo}/build/obj.o\n")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="live process")

    def test_live_process_cwd_at_checkout_is_held(self):
        repo = self.lane("a")
        self.lsof_extra.write_text(f"n{repo}\n")
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="live process")

    def test_live_process_elsewhere_or_lookalike_does_not_block(self):
        repo = self.lane("a")
        other = self.lane("b")
        self.lsof_extra.write_text(f"n{other}/build/obj.o\nn{repo}-twin/x\n")
        r = self.run_reaper()
        self.assertReclaim(r, "a", "build", "build")
        self.assertHeld(r, "b", "build", "build", why="live process")

    def test_dirty_tracked_tree_is_held(self):
        repo = self.lane("a")
        (repo / "src.txt").write_text("edited\n")
        age(repo, 3)
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="dirty")

    def test_untracked_noise_alone_is_not_dirty(self):
        repo = self.lane("a")
        (repo / "notes.txt").write_text("scratch\n")
        age(repo, 3)
        self.assertReclaim(self.run_reaper(), "a", "build", "build")

    def test_build_dir_with_tracked_files_is_held(self):
        self.lane("a", tracked_build=True)
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="tracked")

    def test_keep_marker_in_checkout_or_entry_is_held(self):
        repo = self.lane("a")
        (repo / ".reaper-keep").write_text("")
        age(repo, 3)
        self.assertHeld(self.run_reaper(), "a", "build", "build", why="reaper-keep")
        repo2 = self.lane("b")
        (repo2 / "build" / ".reaper-keep").write_text("")
        age(repo2, 3)
        self.assertHeld(self.run_reaper(), "b", "build", "build", why="reaper-keep")

    def test_build_name_patterns(self):
        repo = self.lane("a", build=False)
        for n in ("build", "build-host", "build_x", "_build", "builder", "rebuild", "src"):
            (repo / n).mkdir()
            (repo / n / "f").write_text("x")
        age(repo, 3)
        r = self.run_reaper()
        names = {pathlib.Path(e["path"]).name for e in r["lanes"]["a"]["entries"] if e["kind"] == "build"}
        self.assertEqual(names, {"build", "build-host", "build_x", "_build"})

    def test_nested_build_dirs_are_never_touched(self):
        repo = self.lane("a", build=False)
        (repo / "external" / "build").mkdir(parents=True)
        (repo / "external" / "build" / "f").write_text("x")
        age(repo, 3)
        self.assertEqual(self.run_reaper()["lanes"].get("a", {"entries": []})["entries"] and
                         [e for e in self.run_reaper()["lanes"]["a"]["entries"] if e["kind"] == "build"], [])

    def test_symlinked_build_dir_is_ignored(self):
        repo = self.lane("a", build=False)
        outside = self.tmp / "outside"
        outside.mkdir()
        (outside / "precious").write_text("keep")
        os.symlink(outside, repo / "build")
        age(repo, 3)
        r = self.run_reaper("--yes")
        self.assertIsNone(self.entry(r, "a", "build"))
        self.assertTrue((outside / "precious").exists())


class ArtifactGuards(Base):
    def test_large_untracked_pkg_is_reclaimable_small_is_not(self):
        repo = self.lane("a", build=False, artifact=True)
        (repo / "tiny.pkg").write_text("x")
        age(repo, 3)
        r = self.run_reaper()
        self.assertReclaim(r, "a", "artifact", "Foo.pkg")
        self.assertIsNone(self.entry(r, "a", "artifact", "tiny.pkg"))

    def test_extracted_sdk_dir_is_an_artifact(self):
        repo = self.lane("a", build=False)
        (repo / "sdk-copy").mkdir()
        (repo / "sdk-copy" / "lib.a").write_text("z" * 300_000)
        age(repo, 3)
        self.assertReclaim(self.run_reaper(), "a", "artifact", "sdk-copy")

    def test_tracked_artifact_is_held(self):
        repo = self.lane("a", build=False)
        (repo / "Foo.pkg").write_text("p" * 200_000)
        sh(repo, "git", "add", "Foo.pkg")
        sh(repo, "git", "commit", "-q", "-m", "track pkg")
        sh(repo, "git", "update-ref", "refs/remotes/origin/main", "HEAD")
        age(repo, 3)
        self.assertHeld(self.run_reaper(), "a", "artifact", "Foo.pkg", why="tracked")

    def test_artifact_obeys_merged_guard(self):
        self.lane("a", build=False, artifact=True, merged=False)
        self.assertHeld(self.run_reaper(), "a", "artifact", "Foo.pkg", why="no PR")


class LaneGuards(Base):
    def test_control_old_clean_merged_lane_is_reclaimable_whole(self):
        self.lane("a", age_days=9)
        r = self.run_reaper()
        self.assertReclaim(r, "a", "lane")
        self.assertIsNone(self.entry(r, "a", "build"))

    def test_lane_idle_under_seven_days_is_held_but_build_may_go(self):
        self.lane("a", age_days=3)
        r = self.run_reaper()
        self.assertHeld(r, "a", "lane", why="lane idle window")
        self.assertReclaim(r, "a", "build", "build")

    def test_unpushed_commit_on_another_branch_holds_the_lane(self):
        repo = self.lane("a", age_days=9)
        sh(repo, "git", "checkout", "-q", "-b", "wip")
        (repo / "w.txt").write_text("w")
        sh(repo, "git", "add", "w.txt")
        sh(repo, "git", "commit", "-q", "-m", "wip")
        sh(repo, "git", "checkout", "-q", "main")
        age(repo, 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="unpushed")

    def test_stash_holds_the_lane(self):
        repo = self.lane("a", age_days=9)
        (repo / "src.txt").write_text("stashme\n")
        sh(repo, "git", "stash", "-q")
        age(repo, 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="stash")

    def test_dirty_tracked_tree_holds_the_lane(self):
        repo = self.lane("a", age_days=9)
        (repo / "src.txt").write_text("edit\n")
        age(repo, 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="dirty")

    def test_untracked_work_outside_artifact_dirs_holds_the_lane(self):
        repo = self.lane("a", age_days=9)
        (repo / "analysis.md").write_text("notes worth keeping\n")
        age(repo, 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="untracked files")

    def test_unmerged_checkout_holds_the_lane(self):
        self.lane("a", merged=False, age_days=9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="no PR")

    def test_one_unmerged_checkout_of_two_holds_the_lane(self):
        self.lane("a", age_days=9)
        second = self.root / "a" / "second"
        second.mkdir()
        sh(second, "git", "init", "-q", "-b", "main")
        (second / "f").write_text("1")
        sh(second, "git", "add", "f")
        sh(second, "git", "commit", "-q", "-m", "x")
        sh(second, "git", "remote", "add", "origin", GH)
        sh(second, "git", "update-ref", "refs/remotes/origin/main", "HEAD")
        (second / "g").write_text("2")
        sh(second, "git", "add", "g")
        sh(second, "git", "commit", "-q", "-m", "unmerged")
        sh(second, "git", "update-ref", "refs/remotes/origin/wip", "HEAD")
        age(self.root / "a", 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="second")

    def test_live_process_in_lane_holds_it(self):
        self.lane("a", age_days=9)
        self.lsof_extra.write_text(f"n{self.root}/a/log.txt\n")
        self.assertHeld(self.run_reaper(), "a", "lane", why="live process")

    def test_keep_marker_holds_the_lane(self):
        self.lane("a", age_days=9)
        (self.root / "a" / ".reaper-keep").write_text("")
        age(self.root / "a", 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="reaper-keep")

    def test_lane_without_checkout_is_never_reclaimed(self):
        (self.root / "notes").mkdir()
        (self.root / "notes" / "goal.txt").write_text("a goal")
        age(self.root / "notes", 30)
        r = self.run_reaper()
        self.assertHeld(r, "notes", "lane", why="no git checkout")

    def test_hidden_git_repo_deeper_in_the_lane_holds_it(self):
        self.lane("a", age_days=9)
        deep = self.root / "a" / "x" / "y" / "z" / "w" / "repo2"
        deep.mkdir(parents=True)
        sh(deep, "git", "init", "-q", "-b", "main")
        age(self.root / "a", 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="unassessed git repository")

    def test_linked_worktree_checkout_holds_the_lane(self):
        main = self.lane("base", age_days=9)
        wt = self.root / "a" / "wt"
        (self.root / "a").mkdir()
        sh(main, "git", "worktree", "add", "-q", "-b", "wtb", str(wt))
        age(self.root / "a", 9)
        age(self.root / "base", 9)
        self.assertHeld(self.run_reaper(), "a", "lane", why="linked worktree")

    def test_reclaimable_lane_masks_its_inner_entries(self):
        self.lane("a", age_days=9, artifact=True)
        r = self.run_reaper()
        self.assertEqual([e["kind"] for e in r["lanes"]["a"]["entries"]], ["lane"])
        self.assertEqual(len(r["lanes"]), 1)


class Instruments(Base):
    def test_blind_lsof_refuses_to_run_even_with_yes(self):
        repo = self.lane("a", age_days=3)
        p = self.run_reaper("--yes", expect_rc=3, env_extra={"STUB_LSOF_BLIND": "1"})
        self.assertIn("process listing unavailable", p.stderr)
        self.assertTrue((repo / "build" / "obj.o").exists())

    def test_missing_lsof_refuses_to_run(self):
        self.lane("a")
        p = self.run_reaper(expect_rc=3, env_extra={"PULP_REAPER_LSOF": "/nonexistent/lsof"})
        self.assertIn("unavailable", p.stderr)

    def test_reading_state_does_not_make_a_checkout_look_busy(self):
        self.lane("a")
        self.run_reaper()
        self.assertReclaim(self.run_reaper(), "a", "build", "build")


class Removal(Base):
    def test_yes_removes_only_the_build_dir(self):
        repo = self.lane("a", age_days=3)
        sibling = self.lane("keepme", merged=False)
        r = self.run_reaper("--yes")
        self.assertEqual(r["results"][str(repo / "build")], "removed")
        self.assertFalse((repo / "build").exists())
        self.assertTrue((repo / "src.txt").exists())
        self.assertTrue((repo / ".git").exists())
        self.assertTrue((sibling / "build" / "obj.o").exists())
        self.assertEqual([p.name for p in repo.iterdir() if p.name.startswith(".reaping-")], [])

    def test_yes_removes_a_whole_finished_lane(self):
        self.lane("a", age_days=9)
        other = self.lane("b", age_days=9, merged=False)
        r = self.run_reaper("--yes")
        self.assertEqual(r["results"][str(self.root / "a")], "removed")
        self.assertFalse((self.root / "a").exists())
        self.assertTrue(other.exists())
        self.assertEqual([p.name for p in self.root.iterdir() if p.name.startswith(".reaping-")], [])

    def test_yes_does_not_touch_held_entries(self):
        repo = self.lane("a", age_days=3)
        self.lsof_extra.write_text(f"n{repo}\n")
        r = self.run_reaper("--yes")
        self.assertEqual(r["results"], {})
        self.assertTrue((repo / "build" / "obj.o").exists())

    def test_leftover_from_an_interrupted_removal_is_finished(self):
        repo = self.lane("a", build=False)
        left = repo / ".reaping-build-1-2"
        left.mkdir()
        (left / "f").write_text("x" * 10)
        age(repo, 3)
        r = self.run_reaper("--yes")
        self.assertEqual(r["results"][str(left)], "removed")
        self.assertFalse(left.exists())

    def test_remove_refuses_symlink_outside_root_and_git(self):
        outside = self.tmp / "outside"
        outside.mkdir()
        (outside / "f").write_text("x")
        link = self.root / "lnk"
        os.symlink(outside, link)
        it = reaper.Item("lane", link, "lnk")
        self.assertIn("symlink", reaper.remove(it, self.root))
        self.assertTrue((outside / "f").exists())
        it = reaper.Item("lane", outside, "outside")
        self.assertIn("outside the root", reaper.remove(it, self.root))
        self.assertTrue((outside / "f").exists())
        repo = self.lane("a")
        self.assertIn("unsafe name", reaper.remove(reaper.Item("build", repo / ".git", "a"), self.root))
        self.assertTrue((repo / ".git").exists())

    def test_remove_refuses_a_build_dir_that_is_not_inside_a_checkout(self):
        stray = self.root / "a" / "deep" / "er" / "build"
        stray.mkdir(parents=True)
        (stray / "f").write_text("x")
        self.assertIn("not directly inside a checkout",
                      reaper.remove(reaper.Item("build", stray, "a"), self.root))
        self.assertTrue(stray.exists())

    def test_remove_refuses_a_lane_not_directly_under_root(self):
        nested = self.root / "a" / "inner"
        nested.mkdir(parents=True)
        self.assertIn("not directly under the root",
                      reaper.remove(reaper.Item("lane", nested, "a"), self.root))
        self.assertTrue(nested.exists())

    def test_pr_cache_survives_across_runs_but_open_is_not_cached(self):
        repo = self.lane("a", merged=False)
        self.pr(repo, "open")
        self.run_reaper()
        self.pr(repo, "merged")
        self.assertReclaim(self.run_reaper(), "a", "build", "build")


if __name__ == "__main__":
    unittest.main()
