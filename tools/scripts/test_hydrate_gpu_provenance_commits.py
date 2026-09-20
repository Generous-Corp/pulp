#!/usr/bin/env python3
"""Planted controls for bounded GPU provenance ancestry hydration."""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import subprocess
import unittest
from unittest import mock

import hydrate_gpu_provenance_commits as hydration


class HydrationTests(unittest.TestCase):
    def test_checked_in_provenance_fits_the_bounded_commit_budget(self) -> None:
        revisions = hydration.required_commits(pathlib.Path(__file__).resolve().parents[2])
        self.assertGreater(len(revisions), 0)
        self.assertLessEqual(len(revisions), hydration.MAX_COMMITS)

    def test_provenance_at_the_bounded_commit_budget_is_accepted(self) -> None:
        handoff = {
            "entries": [
                {
                    "pulp_paths": [
                        {
                            "repo": "Generous-Corp/pulp",
                            "revision": f"{index:040x}",
                        }
                        for index in range(hydration.MAX_COMMITS - 1)
                    ]
                }
            ]
        }
        receipt = {"verification_equivalent_head": "f" * 40}
        with mock.patch.object(hydration, "load_object", side_effect=[handoff, receipt]):
            self.assertEqual(
                len(hydration.required_commits(pathlib.Path("/repo"))),
                hydration.MAX_COMMITS,
            )

    def test_provenance_over_the_bounded_commit_budget_fails_closed(self) -> None:
        handoff = {
            "entries": [
                {
                    "pulp_paths": [
                        {
                            "repo": "Generous-Corp/pulp",
                            "revision": f"{index:040x}",
                        }
                        for index in range(hydration.MAX_COMMITS + 1)
                    ]
                }
            ]
        }
        receipt = {"verification_equivalent_head": "0" * 40}
        with mock.patch.object(hydration, "load_object", side_effect=[handoff, receipt]):
            with self.assertRaisesRegex(hydration.HydrationError, "allowed range"):
                hydration.required_commits(pathlib.Path("/repo"))

    def test_shallow_boundary_is_reconnected_even_when_objects_exist(self) -> None:
        revision = "a" * 40
        calls: list[list[str]] = []

        def run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(command)
            if command[:3] == ["git", "rev-parse", "--is-shallow-repository"]:
                return subprocess.CompletedProcess(command, 0, "true\n", "")
            return subprocess.CompletedProcess(command, 0, "", "")

        with (
            mock.patch.object(hydration, "required_commits", return_value=[revision]),
            mock.patch.object(hydration, "is_commit", return_value=True),
            mock.patch.object(hydration.subprocess, "run", side_effect=run),
            mock.patch.dict(os.environ, {"GITHUB_REF": "refs/pull/7882/merge"}),
        ):
            self.assertEqual(hydration.hydrate(pathlib.Path("/repo"), "origin"), (1, 0))

        fetches = [command for command in calls if command[:2] == ["git", "fetch"]]
        self.assertEqual(len(fetches), 1)
        self.assertIn("--unshallow", fetches[0])
        self.assertIn(
            "+refs/pull/7882/merge:refs/pulp-ci/gpu-provenance/event", fetches[0]
        )

    def test_absent_merge_ref_falls_back_to_the_pull_request_head(self) -> None:
        revision = "a" * 40
        calls: list[list[str]] = []

        def run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(command)
            if command[:3] == ["git", "rev-parse", "--is-shallow-repository"]:
                return subprocess.CompletedProcess(command, 0, "true\n", "")
            if command[:2] == ["git", "fetch"] and any("/merge:" in arg for arg in command):
                return subprocess.CompletedProcess(
                    command, 128, "",
                    "fatal: couldn't find remote ref refs/pull/7882/merge\n",
                )
            return subprocess.CompletedProcess(command, 0, "", "")

        with (
            mock.patch.object(hydration, "required_commits", return_value=[revision]),
            mock.patch.object(hydration, "is_commit", return_value=True),
            mock.patch.object(hydration.subprocess, "run", side_effect=run),
            mock.patch.dict(os.environ, {"GITHUB_REF": "refs/pull/7882/merge"}),
        ):
            self.assertEqual(hydration.hydrate(pathlib.Path("/repo"), "origin"), (1, 0))

        fetches = [command for command in calls if command[:2] == ["git", "fetch"]]
        self.assertEqual(len(fetches), 2)
        self.assertIn(
            "+refs/pull/7882/merge:refs/pulp-ci/gpu-provenance/event", fetches[0]
        )
        self.assertIn(
            "+refs/pull/7882/head:refs/pulp-ci/gpu-provenance/event", fetches[1]
        )

    def test_unfetchable_event_refs_defer_to_the_ancestry_invariant(self) -> None:
        """No candidate ref resolves, so the ancestry check is the only verdict.

        Ref availability is not provenance. The run must not go red merely
        because GitHub has no merge ref, and it must still go red when the
        history it was supposed to reconnect really is missing.
        """
        revision = "a" * 40

        def run_with_ancestry(
            ancestor_rc: int,
        ):
            def run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                if command[:3] == ["git", "rev-parse", "--is-shallow-repository"]:
                    return subprocess.CompletedProcess(command, 0, "true\n", "")
                if command[:2] == ["git", "fetch"]:
                    return subprocess.CompletedProcess(
                        command, 128, "", "fatal: couldn't find remote ref\n")
                if command[:2] == ["git", "merge-base"]:
                    return subprocess.CompletedProcess(command, ancestor_rc, "", "")
                return subprocess.CompletedProcess(command, 0, "", "")
            return run

        for ancestor_rc, expect_raise in ((0, False), (1, True)):
            with self.subTest(ancestor_rc=ancestor_rc):
                with (
                    mock.patch.object(hydration, "required_commits", return_value=[revision]),
                    mock.patch.object(hydration, "is_commit", return_value=True),
                    mock.patch.object(
                        hydration.subprocess, "run",
                        side_effect=run_with_ancestry(ancestor_rc),
                    ),
                    mock.patch.dict(os.environ, {"GITHUB_REF": "refs/pull/7882/merge"}),
                ):
                    if expect_raise:
                        with self.assertRaisesRegex(
                            hydration.HydrationError, "not ancestors of HEAD"
                        ):
                            hydration.hydrate(pathlib.Path("/repo"), "origin")
                    else:
                        self.assertEqual(
                            hydration.hydrate(pathlib.Path("/repo"), "origin"), (1, 0)
                        )

    def test_only_a_merge_ref_gains_a_head_fallback(self) -> None:
        self.assertEqual(
            hydration.event_ref_candidates("refs/pull/12/merge"),
            ["refs/pull/12/merge", "refs/pull/12/head"],
        )
        for exact in ("refs/heads/main", "refs/pull/12/head",
                      "refs/heads/gh-readonly-queue/main/pr-12-abc"):
            self.assertEqual(hydration.event_ref_candidates(exact), [exact])

    def test_shallow_checkout_without_exact_event_ref_fails_closed(self) -> None:
        with (
            mock.patch.object(hydration, "required_commits", return_value=["a" * 40]),
            mock.patch.object(hydration, "is_commit", return_value=True),
            mock.patch.object(
                hydration.subprocess, "run",
                return_value=subprocess.CompletedProcess([], 0, "true\n", ""),
            ),
            mock.patch.dict(os.environ, {}, clear=True),
        ):
            with self.assertRaises(hydration.HydrationError):
                hydration.hydrate(pathlib.Path("/repo"), "origin")


class VerifyOnlyTests(unittest.TestCase):
    """The push-time arm's plumbing, mocked where mocking is honest.

    What the checkout holds is a question only real git can answer, so the
    adjudication itself is covered against real repositories next door. What
    belongs here is the contract the arm owes its callers: that it dispatches
    away from hydration, and that it never reaches the network.
    """

    def run_main(self, argv: list[str]) -> tuple[int, str]:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            code = hydration.main(argv)
        return code, stderr.getvalue()

    def test_verify_only_dispatches_away_from_hydration(self) -> None:
        with (
            mock.patch.object(hydration, "verify", return_value=([], [], None, None)),
            mock.patch.object(hydration, "hydrate") as hydrate,
        ):
            code, _ = self.run_main(["--root", "/repo", "--verify-only"])
        self.assertEqual(code, 0)
        hydrate.assert_not_called()

    def test_verify_only_reports_a_broken_pin_as_failure(self) -> None:
        with mock.patch.object(
            hydration, "verify", return_value=(["a" * 40], [], None, None)
        ):
            code, output = self.run_main(["--root", "/repo", "--verify-only"])
        self.assertEqual(code, 1)
        self.assertIn("a" * 40, output)

    def test_verify_only_never_fetches(self) -> None:
        """The one assertion a mock can make better than real git can.

        A verification that silently repaired what it was asked to judge would
        pass every adjudication test next door while defeating the gate, and on
        a partial clone the repair is what an ordinary presence probe does by
        default. Watching every argv is how that stays impossible.
        """
        handoff = {
            "entries": [
                {"pulp_paths": [{"repo": "Generous-Corp/pulp", "revision": "a" * 40}]}
            ]
        }
        receipt = {"verification_equivalent_head": "b" * 40}
        calls = []

        def record(argv, **kwargs):
            calls.append((list(argv), kwargs.get("env") or {}))
            return subprocess.CompletedProcess(argv, 0, "", "")

        with (
            mock.patch.object(hydration, "load_object", side_effect=[handoff, receipt]),
            mock.patch.object(hydration.subprocess, "run", side_effect=record),
        ):
            code, _ = self.run_main(["--root", "/repo", "--verify-only"])

        self.assertEqual(code, 1)  # proves nothing, so it fails closed
        self.assertTrue(calls, "no git was run at all; the assertion below is vacuous")
        for argv, _env in calls:
            self.assertNotIn("fetch", argv)
        object_reads = [
            env for argv, env in calls
            if "cat-file" in argv or "rev-list" in argv or "log" in argv
        ]
        self.assertTrue(object_reads, "no object read was attempted")
        for env in object_reads:
            self.assertEqual(env.get("GIT_NO_LAZY_FETCH"), "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
