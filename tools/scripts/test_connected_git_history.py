#!/usr/bin/env python3
"""Prove the shallow-history predicate discriminates, and never skips."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import connected_git_history as connected  # noqa: E402


ROOT = pathlib.Path(__file__).resolve().parents[2]


def git(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    )
    return completed.stdout.strip()


def shallow_clone(destination: pathlib.Path, source: pathlib.Path) -> pathlib.Path:
    """Clone ``source`` with a one-commit history.

    ``--no-local`` is load-bearing. Without it Git hardlinks the source object
    store and silently ignores ``--depth``, producing a complete clone that
    would make every assertion below vacuous.
    """

    git(
        source,
        "clone",
        "--quiet",
        "--depth=1",
        "--no-local",
        f"file://{source}",
        str(destination),
    )
    return destination


class ShallowBoundaryPredicate(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.workspace = pathlib.Path(self.directory.name)

    def build_repository(self, commits: int = 3) -> pathlib.Path:
        root = self.workspace / "origin"
        root.mkdir()
        git(root, "init", "--quiet", "--initial-branch=main")
        git(root, "config", "user.email", "history@example.invalid")
        git(root, "config", "user.name", "History Fixture")
        for index in range(commits):
            (root / "leaf.txt").write_text(f"leaf {index}\n", encoding="utf-8")
            git(root, "add", "leaf.txt")
            git(root, "commit", "--quiet", "-m", f"leaf {index}")
        return root

    def test_complete_checkout_has_no_boundaries(self) -> None:
        root = self.build_repository()
        self.assertEqual(connected.shallow_boundaries(root), set())
        connected.require_connected_history(root, "the fixture")

    def test_depth_one_clone_resolves_every_path_to_its_boundary(self) -> None:
        origin = self.build_repository()
        clone = shallow_clone(self.workspace / "clone", origin)

        self.assertEqual(git(clone, "rev-list", "--count", "HEAD"), "1")
        boundaries = connected.shallow_boundaries(clone)
        self.assertEqual(len(boundaries), 1)
        self.assertTrue(
            connected.resolves_to_boundary(clone, "HEAD", "leaf.txt", boundaries)
        )
        with self.assertRaises(connected.ShallowCheckoutError) as caught:
            connected.require_connected_history(clone, "the fixture")
        self.assertIn("git fetch --unshallow", str(caught.exception))

    def test_an_empty_answer_reads_as_truncated(self) -> None:
        """An empty answer is the quietest form of the bug.

        ``git log`` exits 0 and prints nothing when the requested commit range
        holds no change to the path -- no SHA to inspect, no error to notice,
        just a row that silently loses its owner. A truncated checkout reaches
        that state for any path whose history lies outside the range it kept,
        so the predicate must read an empty answer as "cannot attribute" too.
        Note the graft commit itself reports every file it contains as *added*
        (it has no parents), which is why the depth-1 case above lands on the
        other shape, a boundary SHA.
        """

        origin = self.build_repository()
        clone = shallow_clone(self.workspace / "clone", origin)
        boundaries = connected.shallow_boundaries(clone)
        self.assertEqual(len(boundaries), 1)

        outside = clone / "outside-the-horizon.txt"
        outside.write_text("present on disk, absent from the kept range\n", encoding="utf-8")
        self.assertEqual(
            git(clone, "log", "-1", "--format=%H", "HEAD", "--", outside.name), ""
        )
        self.assertTrue(
            connected.resolves_to_boundary(clone, "HEAD", outside.name, boundaries)
        )

    def test_an_unaskable_query_follows_the_boundary_set(self) -> None:
        """A refused query is truncation only where a graft already exists.

        A pinned revision that is not in the object store -- the ordinary state
        of every row in a ``--depth=1`` clone -- makes ``git log`` exit nonzero
        rather than answer. On a checkout that already carries a graft, that is
        the truncation showing through. On a complete one the same refusal means
        a bad argument, and calling it truncation would misdiagnose a checkout
        with nothing wrong with it.
        """

        origin = self.build_repository()
        clone = shallow_clone(self.workspace / "clone", origin)
        absent = "0" * 40
        self.assertTrue(
            connected.resolves_to_boundary(
                clone, absent, "leaf.txt", connected.shallow_boundaries(clone)
            )
        )
        self.assertEqual(connected.shallow_boundaries(origin), set())
        self.assertFalse(
            connected.resolves_to_boundary(origin, absent, "leaf.txt", set())
        )

    def test_a_real_owning_commit_is_not_a_boundary(self) -> None:
        root = self.build_repository()
        boundaries = {"0" * 40}
        self.assertFalse(
            connected.resolves_to_boundary(root, "HEAD", "leaf.txt", boundaries)
        )

    def test_boundaries_come_from_the_common_git_directory(self) -> None:
        """A linked worktree shares the object store, so it shares the horizon."""

        origin = self.build_repository()
        clone = shallow_clone(self.workspace / "clone", origin)
        linked = self.workspace / "linked"
        git(clone, "worktree", "add", "--quiet", "--detach", str(linked), "HEAD")

        # --git-dir in a linked worktree points at .git/worktrees/<name>, where
        # no shallow file exists; only --git-common-dir finds the real one.
        private = pathlib.Path(git(linked, "rev-parse", "--git-dir"))
        if not private.is_absolute():
            private = linked / private
        self.assertFalse((private / "shallow").exists())
        self.assertEqual(
            connected.shallow_boundaries(linked), connected.shallow_boundaries(clone)
        )

    def test_a_broken_instrument_is_not_a_shallow_finding(self) -> None:
        """A directory that is not a repository must not read as truncated."""

        outside = self.workspace / "not-a-repo"
        outside.mkdir()
        self.assertEqual(connected.shallow_boundaries(outside), set())
        connected.require_connected_history(outside, "the fixture")

    def test_live_worktree_is_shallow_marked_yet_answers_every_question(self) -> None:
        """The falsification that rules out ``--is-shallow-repository``.

        This checkout reports ``true`` for ``--is-shallow-repository`` and still
        resolves every pinned row to a real owning commit. A guard keyed on that
        flag would fail it -- and every CI runner checkout in the same state,
        including the ones producing the required ``macos`` gate.

        The state cannot be synthesised: a repository is shallow-marked only
        because Git wrote a ``shallow`` file during a truncated fetch, and the
        rows must resolve past it, which needs the real deep history. So this
        asserts against the live worktree and states the two facts it depends on
        rather than assuming them. When the checkout is *not* shallow-marked the
        falsification has nothing to say, and the assertion that matters --
        no shallow finding -- still holds and is still made.
        """

        boundaries = connected.shallow_boundaries(ROOT)
        marked = (
            subprocess.run(
                ["git", "rev-parse", "--is-shallow-repository"],
                cwd=ROOT, capture_output=True, text=True, check=False,
            ).stdout.strip()
            == "true"
        )
        self.assertEqual(marked, bool(boundaries))

        probes = connected._ledger_probes(ROOT)
        self.assertGreater(len(probes), 0)
        on_boundary = [
            path
            for commit, path in probes
            if connected.resolves_to_boundary(ROOT, commit, path, boundaries)
        ]
        self.assertEqual(on_boundary, [])
        connected.require_connected_history(ROOT, "the live worktree")


class SetUpModuleRaisesRatherThanSkips(unittest.TestCase):
    """A raise in ``setUpModule`` must surface as an error, never a skip."""

    def test_raising_setupmodule_fails_nonzero_and_reports_no_skip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            suite = pathlib.Path(directory) / "test_raising_setup.py"
            suite.write_text(
                textwrap.dedent(
                    """
                    import unittest


                    def setUpModule():
                        raise RuntimeError("history is truncated")


                    class Probe(unittest.TestCase):
                        def test_never_runs(self):
                            self.fail("setUpModule should have stopped this")


                    if __name__ == "__main__":
                        unittest.main()
                    """
                ),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [sys.executable, "-B", str(suite)],
                capture_output=True,
                text=True,
                check=False,
            )
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0, output)
        self.assertIn("FAILED (errors=", output)
        self.assertNotIn("skipped", output)
        self.assertIn("history is truncated", output)


if __name__ == "__main__":
    unittest.main()
