"""Real-git coverage for the provenance hydrator's ref-candidate fallback.

The sibling unit tests mock `subprocess` entirely, so they prove the candidate
ORDER and the fall-through, never that git agrees. These drive real git against
a genuinely shallow clone whose `refs/pull/<n>/merge` is absent -- the
production condition -- and assert both that the head fallback reconnects the
history and that an unreachable provenance commit still fails closed.
"""

from __future__ import annotations

import json
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
        handoff = checkout / "docs/status"
        handoff.mkdir(parents=True)
        (handoff / "gpu-vellum-handoff.yaml").write_text(
            json.dumps(
                {
                    "entries": [
                        {
                            "pulp_paths": [
                                {
                                    "repo": "Generous-Corp/pulp",
                                    "revision": self.provenance,
                                }
                            ]
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        receipt = checkout / "docs/validation/gpu-probes/m3-a2-real-probes-20260828"
        receipt.mkdir(parents=True)
        (receipt / "receipt.json").write_text(
            json.dumps({"verification_equivalent_head": self.provenance}),
            encoding="utf-8",
        )
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


if __name__ == "__main__":
    unittest.main()
