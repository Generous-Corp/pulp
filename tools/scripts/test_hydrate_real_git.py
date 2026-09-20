"""Real-git coverage for the provenance hydrator's ref-candidate fallback.

The sibling unit tests mock `subprocess` entirely, so they prove the candidate
ORDER and the fall-through, never that git agrees. These drive real git against
a genuinely shallow clone whose `refs/pull/<n>/merge` is absent -- the
production condition -- and assert both that the head fallback reconnects the
history and that an unreachable provenance commit still fails closed.

The verification arm is driven against real git for the same reason, and for
one more: its whole job is to tell a pin that was rewritten away apart from
history the clone simply never fetched. Those two look identical to any mock.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).parent))

import hydrate_gpu_provenance_commits as hydration


def git(cwd: pathlib.Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=cwd, text=True, capture_output=True, check=True,
    ).stdout.strip()


def git_at(cwd: pathlib.Path, when: str, *args: str) -> str:
    """Run git with both commit timestamps pinned.

    The shallow adjudication compares a pin's committer date against the graft
    horizon, so a fixture that wants to land on one side of that comparison has
    to say which side rather than race the clock.
    """
    env = dict(os.environ)
    env["GIT_AUTHOR_DATE"] = when
    env["GIT_COMMITTER_DATE"] = when
    return subprocess.run(
        ["git", *args], cwd=cwd, text=True, capture_output=True, check=True, env=env,
    ).stdout.strip()


def write_provenance(
    checkout: pathlib.Path, ledger_revisions: list[str], receipt_revision: str,
) -> None:
    """Plant both checked-in provenance surfaces the hydrator reads."""
    handoff = checkout / "docs/status"
    handoff.mkdir(parents=True, exist_ok=True)
    (handoff / "gpu-vellum-handoff.yaml").write_text(
        json.dumps(
            {
                "entries": [
                    {
                        "pulp_paths": [
                            {"repo": "Generous-Corp/pulp", "revision": revision}
                            for revision in ledger_revisions
                        ]
                    }
                ]
            }
        ),
        encoding="utf-8",
    )
    receipt = checkout / "docs/validation/gpu-probes/m3-a2-real-probes-20260828"
    receipt.mkdir(parents=True, exist_ok=True)
    (receipt / "receipt.json").write_text(
        json.dumps({"verification_equivalent_head": receipt_revision}),
        encoding="utf-8",
    )


def run_verify_only(checkout: pathlib.Path) -> tuple[int, str]:
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
        code = hydration.main(["--root", str(checkout), "--verify-only"])
    return code, stderr.getvalue()


class RealGitHydrationTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)

        source = self.root / "source"
        source.mkdir()
        git(source, "init", "-q", "-b", "main", ".")
        git(source, "config", "user.email", "hydration@pulp.test")
        git(source, "config", "user.name", "hydration test")
        for index in range(4):
            (source / f"f{index}.txt").write_text(f"{index}\n", encoding="utf-8")
            git(source, "add", f"f{index}.txt")
            git(source, "commit", "-qm", f"c{index}")
        self.provenance = git(source, "rev-parse", "HEAD~3")
        tip = git(source, "rev-parse", "HEAD")

        self.remote = self.root / "remote.git"
        git(self.root, "clone", "-q", "--bare", str(source), str(self.remote))
        # A pull request's `head` ref survives; its `merge` ref does not, which
        # is exactly the state GitHub leaves behind on close and during a
        # mergeability recompute.
        git(self.remote, "update-ref", "refs/pull/999/head", tip)

    def shallow_checkout(self, name: str) -> pathlib.Path:
        checkout = self.root / name
        git(self.root, "clone", "-q", "--depth", "1", self.remote.as_uri(), str(checkout))
        git(checkout, "config", "user.email", "hydration@pulp.test")
        git(checkout, "config", "user.name", "hydration test")
        write_provenance(checkout, [self.provenance], self.provenance)
        self.assertEqual(git(checkout, "rev-parse", "--is-shallow-repository"), "true")
        self.assertFalse(hydration.is_commit(checkout, self.provenance))
        return checkout

    def test_absent_merge_ref_reconnects_through_the_pull_request_head(self) -> None:
        checkout = self.shallow_checkout("with-head")
        with mock.patch.dict("os.environ", {"GITHUB_REF": "refs/pull/999/merge"}):
            total, missing = hydration.hydrate(checkout, "origin")
        self.assertEqual((total, missing), (1, 1))
        self.assertTrue(hydration.is_commit(checkout, self.provenance))

    def test_no_resolvable_candidate_still_fails_closed(self) -> None:
        checkout = self.shallow_checkout("no-candidate")
        with mock.patch.dict("os.environ", {"GITHUB_REF": "refs/pull/12345/merge"}):
            with self.assertRaises(hydration.HydrationError) as raised:
                hydration.hydrate(checkout, "origin")
        self.assertIn("remain unresolved", str(raised.exception))

    def test_a_pin_older_than_the_graft_horizon_is_skipped_not_failed(self) -> None:
        checkout = self.shallow_checkout("beyond-horizon")
        broken, undecidable, horizon, _oldest = hydration.verify(checkout)
        self.assertEqual(broken, [])
        self.assertEqual(undecidable, [self.provenance])
        self.assertIsNotNone(horizon)
        code, output = run_verify_only(checkout)
        self.assertEqual(code, 0)
        self.assertIn("SKIP", output)
        self.assertIn(self.provenance, output)

    def test_an_orphan_newer_than_the_graft_horizon_is_still_reported(self) -> None:
        checkout = self.shallow_checkout("orphan-past-horizon")
        # Dated past anything this clone could hold, so truncation is not an
        # available explanation for its absence the way it is for the pin the
        # sibling test covers. Both pins are unreachable; only one is a defect.
        far_future = "2099-01-01T00:00:00 +0000"
        (checkout / "orphan.txt").write_text("orphan\n", encoding="utf-8")
        git(checkout, "add", "orphan.txt")
        git_at(checkout, far_future, "commit", "-qm", "to be rewritten")
        orphan = git(checkout, "rev-parse", "HEAD")
        git_at(checkout, far_future, "commit", "-q", "--amend", "-m", "rewritten")
        self.assertNotEqual(git(checkout, "rev-parse", "HEAD"), orphan)

        write_provenance(checkout, [self.provenance, orphan], self.provenance)
        broken, undecidable, _horizon, _oldest = hydration.verify(checkout)
        self.assertEqual(broken, [orphan])
        self.assertEqual(undecidable, [self.provenance])
        code, output = run_verify_only(checkout)
        self.assertEqual(code, 1)
        self.assertIn(orphan, output)


class OrphanedPinTest(unittest.TestCase):
    """A pin the ledger named before an amend rewrote that commit away.

    No truncation here, so the checkout holds every fact the question needs and
    an unreachable pin has exactly one explanation. This is the shape the gate
    exists for: the rewrite happens AFTER the ledger is written, by the amend
    that rewrites the commit carrying it, so no authoring-time check can see it.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.checkout = pathlib.Path(self._tmp.name) / "checkout"
        self.checkout.mkdir(parents=True)
        git(self.checkout, "init", "-q", "-b", "main", ".")
        git(self.checkout, "config", "user.email", "hydration@pulp.test")
        git(self.checkout, "config", "user.name", "hydration test")
        (self.checkout / "f.txt").write_text("0\n", encoding="utf-8")
        git(self.checkout, "add", "f.txt")
        git(self.checkout, "commit", "-qm", "c0")
        self.pinned = git(self.checkout, "rev-parse", "HEAD")
        write_provenance(self.checkout, [self.pinned], self.pinned)

    def test_a_reachable_pin_is_accepted(self) -> None:
        broken, undecidable, _horizon, _oldest = hydration.verify(self.checkout)
        self.assertEqual((broken, undecidable), ([], []))
        code, output = run_verify_only(self.checkout)
        self.assertEqual(code, 0)
        self.assertNotIn("FAIL", output)

    def test_an_amended_pin_is_reported_broken(self) -> None:
        git(self.checkout, "commit", "-q", "--amend", "-m", "c0 reworded")
        self.assertNotEqual(git(self.checkout, "rev-parse", "HEAD"), self.pinned)
        # The rewritten commit is still in the object store, which is precisely
        # why asking whether the object exists cannot answer this question.
        self.assertTrue(hydration.is_commit(self.checkout, self.pinned))

        broken, undecidable, _horizon, _oldest = hydration.verify(self.checkout)
        self.assertEqual((broken, undecidable), ([self.pinned], []))
        code, output = run_verify_only(self.checkout)
        self.assertEqual(code, 1)
        self.assertIn(self.pinned, output)
        self.assertIn("will not exist on the remote", output)


if __name__ == "__main__":
    unittest.main()
