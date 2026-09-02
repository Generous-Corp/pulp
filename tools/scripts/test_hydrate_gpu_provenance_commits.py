#!/usr/bin/env python3
"""Planted controls for bounded GPU provenance ancestry hydration."""

from __future__ import annotations

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
