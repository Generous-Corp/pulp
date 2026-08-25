#!/usr/bin/env python3
"""Fixture tests for tools/scripts/clean_worktree_builds.sh.

The script deletes directories, so the tests build a real throwaway repository
— a bare origin, a clone that plays the main checkout, and real
`git worktree add` worktrees in each state the gate distinguishes — and drive
the script against that. Nothing outside the temporary directory is touched.

What is proved here, in rough order of how much it matters:

  1. The safety invariant refuses every shape of wrong path, including the one
     from the near-miss this script encodes: a path handed in from a list some
     other process wrote.
  2. `--yes` removes `build/` and nothing else: source, uncommitted work, the
     worktree, `.git`, and lookalike directories all survive.
  3. Each gate independently keeps a directory, including unique history,
     missing/stale lineage, active use, and recent writes.
  4. The main checkout is never reaped, even when its branch is gone from
     origin — the case a live host cannot demonstrate on demand.

Run:
    python3 tools/scripts/test_clean_worktree_builds.py
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "clean_worktree_builds.sh"


def git(cwd: pathlib.Path, *args: str) -> str:
    env = dict(
        os.environ,
        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@e",
        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@e",
    )
    # A parent git hook environment would redirect these at the real repo.
    for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"):
        env.pop(var, None)
    res = subprocess.run(
        ["git", *args], cwd=str(cwd), env=env,
        capture_output=True, text=True, check=True,
    )
    return res.stdout


class Fixture:
    """A bare origin, a main checkout holding the script, and worktrees."""

    def __init__(self, tmp: pathlib.Path) -> None:
        # Match Git's physical spelling on macOS (/private/var, not /var).
        self.tmp = tmp.resolve()
        self.origin = self.tmp / "origin.git"
        self.main = self.tmp / "main-checkout"
        self.wts = self.tmp / "wts"
        self.wts.mkdir()

        subprocess.run(
            ["git", "init", "--bare", "-b", "main", str(self.origin)],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["git", "clone", str(self.origin), str(self.main)],
            check=True, capture_output=True,
        )
        (self.main / "README").write_text("seed\n")
        git(self.main, "add", "README")
        git(self.main, "commit", "-m", "seed")
        git(self.main, "push", "-u", "origin", "main")

        scripts = self.main / "tools" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "clean_worktree_builds.sh").write_text(SCRIPT.read_text())
        ci = self.main / "tools" / "ci"
        ci.mkdir(parents=True)
        (ci / "build_dir_lock.py").write_text(
            (REPO_ROOT / "tools" / "ci" / "build_dir_lock.py").read_text())
        git(self.main, "add", "tools")
        git(self.main, "commit", "-m", "script")
        git(self.main, "push")

    @property
    def script(self) -> pathlib.Path:
        return self.main / "tools" / "scripts" / "clean_worktree_builds.sh"

    def advance_main(self, marker: str) -> str:
        (self.main / marker).write_text(marker)
        git(self.main, "add", marker)
        git(self.main, "commit", "-m", marker)
        git(self.main, "push")
        return git(self.main, "rev-parse", "HEAD").strip()

    def mark_lineage(self, worktree: pathlib.Path, *, status: str = "merged",
                     durable_sha: str | None = None,
                     pr: str = "https://github.com/example/pulp/pull/1") -> None:
        branch = git(worktree, "branch", "--show-current").strip()
        head = durable_sha or git(worktree, "rev-parse", "HEAD").strip()
        git(self.main, "config", "--local",
            f"branch.{branch}.pulpWorktreeStatus", status)
        git(self.main, "config", "--local",
            f"branch.{branch}.pulpWorktreeDurableSha", head)
        if pr:
            git(self.main, "config", "--local",
                f"branch.{branch}.pulpWorktreePr", pr)

    def merge_and_mark(self, worktree: pathlib.Path) -> None:
        branch = git(worktree, "branch", "--show-current").strip()
        git(self.main, "merge", "--ff-only", branch)
        git(self.main, "push")
        self.mark_lineage(worktree)
        self.advance_main(f"after-{branch}.txt")

    def add_worktree(self, name: str, *, branch: str | None = None,
                     commit: bool = True, push: bool = False,
                     detach: bool = False, build: bool = True) -> pathlib.Path:
        path = self.wts / name
        if detach:
            git(self.main, "worktree", "add", "--detach", str(path), "HEAD")
        else:
            git(self.main, "worktree", "add", "-b", branch or name, str(path))
        if commit and not detach:
            (path / f"{name}.txt").write_text(name)
            git(path, "add", f"{name}.txt")
            git(path, "commit", "-m", name)
        if push and not detach:
            git(path, "push", "-u", "origin", branch or name)
        if build:
            self.make_build(path)
        # Source and uncommitted work that must survive every run.
        (path / "src").mkdir(exist_ok=True)
        (path / "src" / "keep.txt").write_text("source")
        (path / "uncommitted.txt").write_text("never committed")
        return path

    @staticmethod
    def make_build(worktree: pathlib.Path, *, stale: bool = True) -> pathlib.Path:
        build = worktree / "build"
        (build / "obj").mkdir(parents=True, exist_ok=True)
        (build / "obj" / "a.o").write_text("x" * 64)
        (build / "CMakeCache.txt").write_text("cache")
        if stale:
            # Older than any idle window the tests use.
            old = time.time() - 86_400
            for p in (build / "obj" / "a.o", build / "CMakeCache.txt",
                      build / "obj", build):
                os.utime(p, (old, old))
        return build


def run_script(fx: Fixture, *args: str, env_extra: dict | None = None,
               timeout: float = 120) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                "PULP_WORKTREES_ROOT", "PULP_WORKTREE_BUILD_IDLE_HOURS",
                "PULP_REAP_LIB_ONLY"):
        env.pop(var, None)
    env["PULP_BUILD_DIR_LOCK_ROOT"] = str(fx.tmp / "build-dir-locks")
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        ["bash", str(fx.script), *args], cwd=str(fx.main), env=env,
        capture_output=True, text=True, timeout=timeout,
    )


def lib_eval(fx: Fixture, snippet: str) -> subprocess.CompletedProcess:
    """Source the script in lib-only mode and exercise its helpers."""
    env = dict(os.environ, PULP_REAP_LIB_ONLY="1")
    for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"):
        env.pop(var, None)
    return subprocess.run(
        ["bash", "-c", f'source "{fx.script}"\n{snippet}'],
        cwd=str(fx.main), env=env, capture_output=True, text=True, timeout=60,
    )


class FixtureTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._td.name).resolve()
        self.fx = Fixture(self.root)

    def tearDown(self) -> None:
        self._td.cleanup()


class SafetyInvariantTests(FixtureTestCase):
    """`assert_reapable_path` refuses everything that is not a worktree build."""

    def _roots_file(self, *roots: pathlib.Path) -> pathlib.Path:
        f = self.root / "roots"
        f.write_text("".join(f"{r}\n" for r in roots))
        return f

    def _check(self, path, roots) -> subprocess.CompletedProcess:
        return lib_eval(
            self.fx,
            f'rc=0; assert_reapable_path "{path}" "{roots}" || rc=$?; echo "rc=$rc"',
        )

    def test_accepts_a_real_worktree_build(self) -> None:
        wt = self.fx.add_worktree("wt-ok")
        r = self._check(wt / "build", self._roots_file(wt))
        self.assertIn("rc=0", r.stdout, r.stderr)

    def test_refuses_a_path_that_is_not_a_build_dir(self) -> None:
        # The near-miss shape: a destructive loop handed a path from a list
        # another process wrote. Nothing about it is a build directory.
        wt = self.fx.add_worktree("wt-x")
        foreign = self.root / "GB|LIVE|AGE_D|BRANCH_STATE"
        foreign.mkdir()
        r = self._check(foreign, self._roots_file(wt))
        self.assertIn("rc=1", r.stdout, r.stderr)
        self.assertIn("not an absolute path ending in /build", r.stderr)
        self.assertTrue(foreign.is_dir(), "the refused path must be untouched")

    def test_refuses_a_relative_path(self) -> None:
        wt = self.fx.add_worktree("wt-rel")
        r = self._check("build", self._roots_file(wt))
        self.assertIn("rc=1", r.stdout, r.stderr)

    def test_refuses_a_symlink_named_build(self) -> None:
        wt = self.fx.add_worktree("wt-link", build=False)
        victim = self.root / "victim"
        victim.mkdir()
        (victim / "precious.txt").write_text("do not delete")
        (wt / "build").symlink_to(victim)
        r = self._check(wt / "build", self._roots_file(wt))
        self.assertIn("rc=1", r.stdout, r.stderr)
        self.assertIn("symlink", r.stderr)

    def test_refuses_when_the_parent_is_not_a_git_worktree(self) -> None:
        plain = self.root / "plain"
        (plain / "build").mkdir(parents=True)
        r = self._check(plain / "build", self._roots_file(plain))
        self.assertIn("rc=1", r.stdout, r.stderr)
        self.assertIn("not a git worktree", r.stderr)

    def test_refuses_a_worktree_this_run_did_not_enumerate(self) -> None:
        # Even a genuine worktree build is refused if it is not in the list this
        # run produced — the defence against acting on someone else's data.
        wt = self.fx.add_worktree("wt-unlisted")
        other = self.fx.add_worktree("wt-listed")
        r = self._check(wt / "build", self._roots_file(other))
        self.assertIn("rc=1", r.stdout, r.stderr)
        self.assertIn("not an enumerated worktree", r.stderr)

    def test_refuses_when_the_roots_file_is_missing(self) -> None:
        wt = self.fx.add_worktree("wt-noroots")
        r = self._check(wt / "build", self.root / "nope")
        self.assertIn("rc=1", r.stdout, r.stderr)

    def test_refuses_a_registered_root_replaced_by_a_symlink(self) -> None:
        wt = self.fx.add_worktree("wt-root-link")
        real = wt.with_name("wt-root-link-real")
        roots = self._roots_file(wt)
        wt.rename(real)
        wt.symlink_to(real, target_is_directory=True)
        try:
            r = self._check(wt / "build", roots)
            self.assertIn("rc=1", r.stdout, r.stderr)
            self.assertIn("symlink/replacement component", r.stderr)
            self.assertTrue((real / "build").is_dir())
        finally:
            wt.unlink()
            real.rename(wt)


class PathAliasTests(FixtureTestCase):
    """The /private spelling problem, tested where it is deterministic.

    macOS resolves /tmp, /var and /etc through /private. `git worktree list`
    reports the physical path; a build command line usually carries whatever the
    caller typed. Matching only one spelling made the process gate miss a live
    build in a /tmp worktree — and this repo really does keep worktrees there.
    On Linux the two spellings coincide, so the end-to-end process test cannot
    exercise this; these cases can, on any platform.
    """

    def _aliases(self, path: str) -> list[str]:
        r = lib_eval(self.fx, f'path_aliases "{path}"')
        self.assertEqual(r.returncode, 0, r.stderr)
        return [l for l in r.stdout.splitlines() if l]

    def test_private_spelling_yields_the_bare_one(self) -> None:
        self.assertEqual(
            self._aliases("/private/tmp/wt-x"), ["/private/tmp/wt-x", "/tmp/wt-x"])

    def test_bare_spelling_yields_the_private_one(self) -> None:
        self.assertEqual(
            self._aliases("/var/folders/x/wt"), ["/var/folders/x/wt", "/private/var/folders/x/wt"])

    def test_an_ordinary_path_has_exactly_one_spelling(self) -> None:
        self.assertEqual(
            self._aliases("/Volumes/Workshop/Code/wt"), ["/Volumes/Workshop/Code/wt"])

    def test_process_match_crosses_the_two_spellings(self) -> None:
        snap = self.root / "ps"
        snap.write_text("cmake --build /tmp/wt-busy/build -j8\n/sbin/launchd\n")
        hit = lib_eval(
            self.fx,
            f'PS_SNAPSHOT="{snap}"; LSOF_SNAPSHOT="{snap}.lsof"; : > "$LSOF_SNAPSHOT"; rc=0; '
            f'a_live_process_is_using "/private/tmp/wt-busy" || rc=$?; echo "rc=$rc"',
        )
        self.assertIn("rc=0", hit.stdout, hit.stderr)

    def test_process_match_does_not_fire_on_an_unrelated_path(self) -> None:
        # The other direction: without this the matcher could return 0 always
        # and the test above would prove nothing.
        snap = self.root / "ps"
        snap.write_text("cmake --build /tmp/wt-busy/build -j8\n")
        miss = lib_eval(
            self.fx,
            f'PS_SNAPSHOT="{snap}"; LSOF_SNAPSHOT="{snap}.lsof"; : > "$LSOF_SNAPSHOT"; rc=0; '
            f'a_live_process_is_using "/private/tmp/wt-idle" || rc=$?; echo "rc=$rc"',
        )
        self.assertIn("rc=1", miss.stdout, miss.stderr)


class GateTests(FixtureTestCase):
    """Each condition of the five-part gate keeps a directory on its own."""

    def test_branch_gone_with_unique_history_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-gone")
        # Even a forged/stale terminal record cannot override unique history.
        self.fx.mark_lineage(wt)
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("unique or unproven history", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_branch_live_on_origin_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-live", push=True)
        r = run_script(self.fx, "--verbose")
        self.assertNotIn(f"would remove {wt}", r.stdout)
        self.assertIn("unique or unproven history", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_merged_branch_is_reapable_even_while_it_still_exists(self) -> None:
        wt = self.fx.add_worktree("wt-merged", push=True)
        merged_tip = git(wt, "rev-parse", "HEAD").strip()
        # Land it, then move main past it so the worktree is a strict ancestor.
        git(self.fx.main, "merge", "--ff-only", "wt-merged")
        git(self.fx.main, "push")
        self.fx.mark_lineage(wt)
        self.fx.advance_main("later.txt")
        r = run_script(self.fx)
        self.assertIn(str(wt / "build"), r.stdout)
        self.assertIn("(merged, lineage-proven)", r.stdout)
        self.assertTrue(merged_tip)

    def test_squash_merged_head_is_reapable_with_live_pr_proof(self) -> None:
        wt = self.fx.add_worktree("wt-squash")
        source_head = git(wt, "rev-parse", "HEAD").strip()
        self.fx.advance_main("concurrent-main.txt")
        git(self.fx.main, "cherry-pick", source_head)
        merge_sha = git(self.fx.main, "rev-parse", "HEAD").strip()
        self.assertNotEqual(source_head, merge_sha)
        git(self.fx.main, "push")
        self.fx.mark_lineage(wt, pr="https://github.com/example/pulp/pull/77")
        self.fx.advance_main("after-squash.txt")
        shim = self.root / "bin"
        shim.mkdir()
        gh = shim / "gh"
        gh.write_text(
            "#!/bin/sh\n"
            "printf 'closed\\t2026-08-25T00:00:00Z\\t%s\\tmain\\t%s\\n' "
            '"$PR_HEAD" "$MERGE_SHA"\n'
        )
        gh.chmod(0o755)
        r = run_script(
            self.fx,
            env_extra={
                "PATH": f"{shim}:{os.environ['PATH']}",
                "PR_HEAD": source_head,
                "MERGE_SHA": merge_sha,
            },
        )
        self.assertIn(str(wt / "build"), r.stdout)
        self.assertIn("(merged, lineage-proven)", r.stdout)

    def test_merged_history_without_lineage_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-unclassified")
        branch = git(wt, "branch", "--show-current").strip()
        git(self.fx.main, "merge", "--ff-only", branch)
        git(self.fx.main, "push")
        self.fx.advance_main("later-unclassified.txt")
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertIn("lineage does not prove this exact head merged", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_active_lineage_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-active")
        self.fx.merge_and_mark(wt)
        self.fx.mark_lineage(wt, status="active")
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertIn("lineage does not prove this exact head merged", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_stale_lineage_head_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-stale-lineage")
        self.fx.merge_and_mark(wt)
        self.fx.mark_lineage(wt, durable_sha="0" * 40)
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertIn("lineage does not prove this exact head merged", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_same_head_branch_switch_invalidates_final_identity(self) -> None:
        wt = self.fx.add_worktree("wt-original")
        head = git(wt, "rev-parse", "HEAD").strip()
        self.fx.merge_and_mark(wt)
        git(wt, "switch", "-c", "wt-new-active")
        self.fx.mark_lineage(wt)
        r = lib_eval(
            self.fx,
            f'DEFAULT_BRANCH=main; rc=0; '
            f'current_worktree_identity_proves_merge "{wt}" "{head}" "wt-original" '
            f'"$(git rev-parse refs/remotes/origin/main)" || rc=$?; '
            f'echo "rc=$rc"',
        )
        self.assertIn("rc=1", r.stdout, r.stderr)
        self.assertTrue((wt / "build").is_dir())

    def test_changed_origin_tip_invalidates_final_identity(self) -> None:
        wt = self.fx.add_worktree("wt-tip-moved")
        head = git(wt, "rev-parse", "HEAD").strip()
        self.fx.merge_and_mark(wt)
        expected_tip = git(self.fx.main, "rev-parse", "refs/remotes/origin/main").strip()
        git(self.fx.main, "update-ref", "refs/remotes/origin/main", head)
        r = lib_eval(
            self.fx,
            f'DEFAULT_BRANCH=main; rc=0; '
            f'current_worktree_identity_proves_merge "{wt}" "{head}" "wt-tip-moved" '
            f'"{expected_tip}" || rc=$?; echo "rc=$rc"',
        )
        self.assertIn("rc=1", r.stdout, r.stderr)
        self.assertTrue((wt / "build").is_dir())

    def test_merged_lineage_without_pr_provenance_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-no-pr")
        self.fx.merge_and_mark(wt)
        branch = git(wt, "branch", "--show-current").strip()
        git(self.fx.main, "config", "--local", "--unset-all",
            f"branch.{branch}.pulpWorktreePr")
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertIn("lineage does not prove this exact head merged", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_worktree_sitting_at_the_main_tip_is_kept(self) -> None:
        # No commits of its own: indistinguishable from one created minutes ago,
        # and ancestry alone would call it merged.
        wt = self.fx.add_worktree("wt-at-tip", commit=False)
        r = run_script(self.fx, "--verbose")
        self.assertIn("no commits of its own", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_recently_modified_build_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-fresh")
        self.fx.merge_and_mark(wt)
        (wt / "build" / "CMakeCache.txt").write_text("touched now")
        r = run_script(self.fx, "--verbose")
        self.assertIn("modified within 2h", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_deep_recent_output_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-deep-fresh")
        self.fx.merge_and_mark(wt)
        deep = wt / "build" / "CMakeFiles" / "target.dir" / "src"
        deep.mkdir(parents=True)
        (deep / "fresh.o").write_text("fresh")
        old = time.time() - 86_400
        for directory in (deep, deep.parent, deep.parent.parent, wt / "build"):
            os.utime(directory, (old, old))
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertIn("modified within 2h", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_idle_window_is_configurable(self) -> None:
        wt = self.fx.add_worktree("wt-window")
        self.fx.merge_and_mark(wt)
        # The build is a day old, so a 48h window must keep it and the default
        # 2h window must not.
        wide = run_script(
            self.fx, "--verbose",
            env_extra={"PULP_WORKTREE_BUILD_IDLE_HOURS": "48"})
        self.assertIn("modified within 48h", wide.stdout)
        narrow = run_script(self.fx)
        self.assertIn(str(wt / "build"), narrow.stdout)

    def test_a_live_process_in_the_worktree_keeps_it(self) -> None:
        wt = self.fx.add_worktree("wt-busy")
        self.fx.merge_and_mark(wt)
        proc = subprocess.Popen(
            ["python3", "-c", "import time; time.sleep(30)"], cwd=str(wt))
        try:
            time.sleep(0.3)
            r = run_script(self.fx, "--verbose")
            self.assertIn("a live process is working in it", r.stdout)
            self.assertTrue((wt / "build").is_dir())
        finally:
            proc.terminate()
            proc.wait()

    def test_detached_and_unmerged_is_kept(self) -> None:
        wt = self.fx.add_worktree("wt-detached", detach=True, build=False)
        (wt / "extra.txt").write_text("x")
        git(wt, "add", "extra.txt")
        git(wt, "commit", "-m", "detached work")
        self.fx.make_build(wt)
        r = run_script(self.fx, "--verbose")
        self.assertIn("unique or unproven history", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_the_main_checkout_is_never_reaped(self) -> None:
        # The branch is gone from origin, the build is stale, nothing is running
        # in it — every other condition says reap. It must still be kept: that
        # build is a human's interactive rebuild cost.
        git(self.fx.main, "checkout", "-b", "local-only-branch")
        self.fx.make_build(self.fx.main)
        r = run_script(self.fx, "--verbose", "--yes")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("primary checkout", r.stdout)
        self.assertTrue(
            (self.fx.main / "build").is_dir(),
            "the main checkout's build directory was deleted",
        )


class DeletionTests(FixtureTestCase):
    """`--yes` removes build directories and nothing else."""

    def test_dry_run_deletes_nothing(self) -> None:
        wt = self.fx.add_worktree("wt-dry")
        self.fx.merge_and_mark(wt)
        r = run_script(self.fx)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("would remove", r.stdout)
        self.assertIn("re-run with --yes", r.stdout)
        self.assertTrue((wt / "build").is_dir(), "dry run deleted a build dir")

    def test_apply_removes_the_build_and_preserves_everything_else(self) -> None:
        wt = self.fx.add_worktree("wt-apply")
        self.fx.merge_and_mark(wt)
        r = run_script(self.fx, "--yes")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("removed", r.stdout)
        self.assertFalse((wt / "build").exists(), "build/ survived --yes")
        # The worktree, its source, its uncommitted work and its git link stay.
        self.assertTrue(wt.is_dir())
        self.assertTrue((wt / "src" / "keep.txt").exists())
        self.assertEqual((wt / "uncommitted.txt").read_text(), "never committed")
        self.assertTrue((wt / ".git").exists())
        self.assertTrue((wt / "wt-apply.txt").exists())

    def test_shared_build_lock_prevents_removal(self) -> None:
        wt = self.fx.add_worktree("wt-build-locked")
        self.fx.merge_and_mark(wt)
        ready = self.root / "build-lock-held"
        build_alias = self.root / "locked-build"
        build_alias.symlink_to(wt / "build", target_is_directory=True)
        lock_root = self.fx.tmp / "build-dir-locks"
        holder = subprocess.Popen(
            ["python3", str(self.fx.main / "tools" / "ci" / "build_dir_lock.py"),
             "--build-dir", str(build_alias), "--", "python3", "-c",
             "import pathlib,time,sys; pathlib.Path(sys.argv[1]).write_text('held'); time.sleep(30)",
             str(ready)],
            env={**os.environ, "PULP_BUILD_DIR_LOCK_ROOT": str(lock_root)},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            deadline = time.time() + 10
            while time.time() < deadline and not ready.exists():
                time.sleep(0.01)
            self.assertTrue(ready.exists(), "fixture never acquired the build lock")
            r = run_script(self.fx, "--yes")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("FAILED to remove", r.stdout)
            self.assertIn("kept 1 of 1", r.stdout)
            self.assertTrue((wt / "build").is_dir())
        finally:
            holder.terminate()
            holder.wait()

    def test_process_entering_after_quarantine_forces_restore(self) -> None:
        wt = self.fx.add_worktree("wt-post-rename-busy")
        self.fx.merge_and_mark(wt)
        watcher = subprocess.Popen(
            ["python3", "-c", (
                "import glob, os, time\n"
                "root=os.environ['WATCH_ROOT']\n"
                "deadline=time.time()+20\n"
                "while time.time()<deadline:\n"
                " p=glob.glob(root+'/.pulp-reap-build-*')\n"
                " if p:\n"
                "  f=open(p[0]+'/CMakeCache.txt','rb')\n"
                "  time.sleep(10)\n"
                "  break\n"
                " time.sleep(0.001)\n"
            )],
            env={**os.environ, "WATCH_ROOT": str(wt)},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            r = run_script(self.fx, "--yes")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("FAILED to remove", r.stdout)
            self.assertTrue((wt / "build" / "CMakeCache.txt").exists())
        finally:
            watcher.terminate()
            watcher.wait()

    def test_short_lived_writer_after_quarantine_forces_restore(self) -> None:
        wt = self.fx.add_worktree("wt-post-rename-write")
        self.fx.merge_and_mark(wt)
        watcher = subprocess.Popen(
            ["python3", "-c", (
                "import glob, os, pathlib, time\n"
                "root=os.environ['WATCH_ROOT']\n"
                "deadline=time.time()+20\n"
                "while time.time()<deadline:\n"
                " p=glob.glob(root+'/.pulp-reap-build-*')\n"
                " if p:\n"
                "  pathlib.Path(p[0], 'late-object.o').write_text('new')\n"
                "  break\n"
                " time.sleep(0.001)\n"
            )],
            env={**os.environ, "WATCH_ROOT": str(wt)},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            r = run_script(self.fx, "--yes")
            watcher.wait(timeout=20)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("FAILED to remove", r.stdout)
            self.assertEqual((wt / "build" / "late-object.o").read_text(), "new")
        finally:
            if watcher.poll() is None:
                watcher.terminate()
                watcher.wait()

    def test_quarantine_entry_replacement_is_never_deleted(self) -> None:
        wt = self.fx.add_worktree("wt-quarantine-replaced")
        self.fx.merge_and_mark(wt)
        precious = wt / "precious-source"
        precious.mkdir()
        (precious / "unique.txt").write_text("keep")
        old = time.time() - 86_400
        os.utime(precious / "unique.txt", (old, old))
        os.utime(precious, (old, old))
        watcher = subprocess.Popen(
            ["python3", "-c", (
                "import glob, os, pathlib, time\n"
                "root=pathlib.Path(os.environ['WATCH_ROOT'])\n"
                "deadline=time.time()+20\n"
                "while time.time()<deadline:\n"
                " p=glob.glob(str(root/'.pulp-reap-build-*'))\n"
                " if p:\n"
                "  q=pathlib.Path(p[0])\n"
                "  q.rename(root/'saved-original-build')\n"
                "  (root/'precious-source').rename(q)\n"
                "  break\n"
                " time.sleep(0.001)\n"
            )],
            env={**os.environ, "WATCH_ROOT": str(wt)},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            r = run_script(self.fx, "--yes")
            watcher.wait(timeout=20)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("FAILED to remove", r.stdout)
            quarantines = list(wt.glob(".pulp-reap-build-*"))
            self.assertEqual(len(quarantines), 1)
            self.assertEqual((quarantines[0] / "unique.txt").read_text(), "keep")
            self.assertTrue((wt / "saved-original-build" / "CMakeCache.txt").exists())
        finally:
            if watcher.poll() is None:
                watcher.terminate()
                watcher.wait()

    def test_lineage_change_after_quarantine_forces_restore(self) -> None:
        wt = self.fx.add_worktree("wt-lineage-changed-late")
        self.fx.merge_and_mark(wt)
        branch = git(wt, "branch", "--show-current").strip()
        watcher = subprocess.Popen(
            ["python3", "-c", (
                "import glob, os, subprocess, time\n"
                "root=os.environ['WATCH_ROOT']\n"
                "deadline=time.time()+20\n"
                "while time.time()<deadline:\n"
                " if glob.glob(root+'/.pulp-reap-build-*'):\n"
                "  subprocess.run(['git','-C',os.environ['MAIN_ROOT'],'config','--local',"
                "os.environ['KEY'],'active'],check=True)\n"
                "  break\n"
                " time.sleep(0.001)\n"
            )],
            env={**os.environ, "WATCH_ROOT": str(wt),
                 "MAIN_ROOT": str(self.fx.main),
                 "KEY": f"branch.{branch}.pulpWorktreeStatus"},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            r = run_script(self.fx, "--yes")
            watcher.wait(timeout=20)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("FAILED to remove", r.stdout)
            self.assertTrue((wt / "build" / "CMakeCache.txt").exists())
        finally:
            if watcher.poll() is None:
                watcher.terminate()
                watcher.wait()

    def test_lookalike_directories_are_never_touched(self) -> None:
        wt = self.fx.add_worktree("wt-lookalike")
        self.fx.merge_and_mark(wt)
        for name in ("build2", "buildx", "build-cov", "prebuild"):
            d = wt / name
            d.mkdir()
            (d / "f").write_text(name)
        # A `build/` that belongs to no registered worktree.
        stray = self.root / "not-a-worktree"
        (stray / "build").mkdir(parents=True)
        (stray / "build" / "f").write_text("stray")

        r = run_script(self.fx, "--yes")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((wt / "build").exists())
        for name in ("build2", "buildx", "build-cov", "prebuild"):
            self.assertTrue((wt / name / "f").exists(), f"{name} was removed")
        self.assertTrue((stray / "build" / "f").exists(),
                        "a build dir outside every worktree was removed")

    def test_a_worktree_with_no_build_is_left_alone(self) -> None:
        wt = self.fx.add_worktree("wt-nobuild", build=False)
        r = run_script(self.fx, "--yes")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(wt.is_dir())


class ScopeAndReportingTests(FixtureTestCase):
    """The run states its own scope; a filtered total is not a host total."""

    def test_root_filter_limits_the_sweep_and_says_so(self) -> None:
        inside = self.fx.add_worktree("wt-inside")
        self.fx.merge_and_mark(inside)
        outside_root = self.root / "elsewhere"
        outside_root.mkdir()
        outside = outside_root / "wt-outside"
        git(self.fx.main, "worktree", "add", "-b", "wt-outside", str(outside))
        (outside / "f.txt").write_text("f")
        git(outside, "add", "f.txt")
        git(outside, "commit", "-m", "f")
        self.fx.make_build(outside)
        self.fx.merge_and_mark(outside)

        r = run_script(self.fx, env_extra={"PULP_WORKTREES_ROOT": str(self.fx.wts)})
        self.assertIn("LIMITED to worktrees under", r.stdout)
        self.assertIn(str(self.fx.wts), r.stdout)
        self.assertIn(str(inside / "build"), r.stdout)
        self.assertNotIn(str(outside / "build"), r.stdout)

    def test_unfiltered_run_does_not_claim_to_be_limited(self) -> None:
        self.fx.add_worktree("wt-any")
        r = run_script(self.fx)
        self.assertNotIn("LIMITED", r.stdout)
        self.assertIn("registered worktree(s)", r.stdout)

    def test_header_states_the_idle_window_and_default_branch(self) -> None:
        wt = self.fx.add_worktree("wt-hdr")
        self.fx.merge_and_mark(wt)
        r = run_script(self.fx, env_extra={"PULP_WORKTREE_BUILD_IDLE_HOURS": "7"})
        self.assertIn("idle window 7h", r.stdout)
        self.assertIn("default branch main", r.stdout)


class FailureModeTests(FixtureTestCase):
    """Unknown state fails closed; bad input fails loudly."""

    def test_unreachable_origin_removes_nothing(self) -> None:
        # Current merged ancestry is unknowable. It must refuse instead.
        wt = self.fx.add_worktree("wt-offline")
        git(self.fx.main, "remote", "set-url", "origin",
            str(self.root / "no-such-repo.git"))
        r = run_script(self.fx, "--yes")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("could not refresh origin/main", r.stderr)
        self.assertIn("Nothing removed", r.stderr)
        self.assertTrue((wt / "build").is_dir())

    def test_origin_with_no_branches_removes_nothing(self) -> None:
        # An origin without main cannot supply current ancestry authority.
        wt = self.fx.add_worktree("wt-emptyremote")
        empty = self.root / "empty-origin.git"
        subprocess.run(["git", "init", "--bare", "-b", "main", str(empty)],
                       check=True, capture_output=True)
        git(self.fx.main, "remote", "set-url", "origin", str(empty))
        r = run_script(self.fx, "--yes")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("could not refresh origin/main", r.stderr)
        self.assertTrue((wt / "build").is_dir())

    def test_unreadable_process_list_removes_nothing(self) -> None:
        # Without a process list the "nothing is running in it" condition
        # cannot be evaluated, and an unreadable gate passes everything —
        # exactly when a build might be in flight. It must refuse instead.
        wt = self.fx.add_worktree("wt-nops")
        shim = self.root / "shim"
        shim.mkdir()
        # Partial stdout from a failed ps must not count as a full snapshot.
        (shim / "ps").write_text("#!/bin/sh\necho '999 partial-process'\nexit 1\n")
        (shim / "ps").chmod(0o755)
        r = run_script(
            self.fx, "--yes",
            env_extra={"PATH": f"{shim}:{os.environ['PATH']}"})
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("could not read process argv/cwd/open-file state", r.stderr)
        self.assertTrue((wt / "build").is_dir())

    def test_failed_find_without_stderr_is_not_treated_as_idle(self) -> None:
        wt = self.fx.add_worktree("wt-find-fails")
        self.fx.merge_and_mark(wt)
        shim = self.root / "find-shim"
        shim.mkdir()
        (shim / "find").write_text("#!/bin/sh\nexit 42\n")
        (shim / "find").chmod(0o755)
        r = run_script(
            self.fx, "--verbose", "--yes",
            env_extra={"PATH": f"{shim}:{os.environ['PATH']}"})
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("build freshness is unreadable", r.stdout)
        self.assertTrue((wt / "build").is_dir())

    def test_unknown_argument_exits_2(self) -> None:
        r = run_script(self.fx, "--delete-everything")
        self.assertEqual(r.returncode, 2)
        self.assertIn("unknown argument", r.stderr)

    def test_invalid_idle_window_exits_2(self) -> None:
        r = run_script(self.fx, env_extra={"PULP_WORKTREE_BUILD_IDLE_HOURS": "two"})
        self.assertEqual(r.returncode, 2)
        self.assertIn("invalid PULP_WORKTREE_BUILD_IDLE_HOURS", r.stderr)

    def test_help_exits_0(self) -> None:
        r = run_script(self.fx, "--help")
        self.assertEqual(r.returncode, 0)
        self.assertIn("clean_worktree_builds", r.stdout)

    def test_script_is_executable(self) -> None:
        self.assertTrue(SCRIPT.exists(), f"missing {SCRIPT}")
        self.assertTrue(os.access(SCRIPT, os.X_OK), "script is not executable")


if __name__ == "__main__":
    unittest.main(verbosity=2)
