#!/usr/bin/env python3
"""Tests for tools/scripts/resolve_release_runners.py.

The invariant that matters most is FLUIDITY: with every variable unset, routing must
be byte-identical to the old hard-coded behavior. Opting a leg onto the local pool
(or pinning it to one machine) is a variable change; reverting is unsetting it.
Neither may need a code change — if the local pool is down, getting back to GitHub's
runners has to be one command.

The second invariant is FAIL LOUD. A leg routed to a runner that does not exist sits
queued forever, and "queued forever" is the failure mode this pipeline is worst at
noticing (see release_reconcile.py's STUCK_QUEUE). A typo'd variable must abort the
resolve, not silently produce a labelset nothing matches.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from resolve_release_runners import HOSTED, SOURCES, resolve  # noqa: E402

LEGS = (
    "darwin-arm64", "darwin-x64",
    "linux-x64", "linux-arm64",
    "windows-x64", "windows-arm64",
)

LOCAL_LINUX = '["self-hosted","Linux","ARM64","pulp-build-linux"]'
LOCAL_M5 = '["self-hosted","Linux","ARM64","pulp-build-linux","pulp-host-m5"]'


class FluidityInvariant(unittest.TestCase):
    """Unset everything -> exactly the previous hard-coded routing."""

    def test_no_variables_means_github_hosted_everywhere(self) -> None:
        self.assertEqual(resolve({}), HOSTED)

    def test_every_leg_has_a_hosted_default(self) -> None:
        """A leg with no default could resolve to nothing and queue forever."""
        for leg in LEGS:
            self.assertIn(leg, HOSTED)
            self.assertTrue(HOSTED[leg])

    def test_every_leg_is_individually_routable(self) -> None:
        for leg in LEGS:
            self.assertIn(leg, SOURCES)
            self.assertTrue(SOURCES[leg])

    def test_unsetting_one_leg_reverts_only_that_leg(self) -> None:
        both = resolve({"LINUX_ARM64": LOCAL_LINUX, "WINDOWS_ARM64": '["self-hosted","Windows"]'})
        self.assertNotEqual(both["linux-arm64"], HOSTED["linux-arm64"])

        reverted = resolve({"WINDOWS_ARM64": '["self-hosted","Windows"]'})
        self.assertEqual(reverted["linux-arm64"], HOSTED["linux-arm64"])   # reverted
        self.assertNotEqual(reverted["windows-arm64"], HOSTED["windows-arm64"])  # untouched


class PerLegRouting(unittest.TestCase):
    def test_a_leg_can_be_moved_to_the_local_pool(self) -> None:
        out = resolve({"LINUX_ARM64": LOCAL_LINUX})
        self.assertEqual(
            out["linux-arm64"],
            ["self-hosted", "Linux", "ARM64", "pulp-build-linux"],
        )
        # …and nothing else moves.
        self.assertEqual(out["linux-x64"], HOSTED["linux-x64"])
        self.assertEqual(out["windows-x64"], HOSTED["windows-x64"])

    def test_a_leg_can_be_PINNED_TO_ONE_MACHINE(self) -> None:
        """Host identity lives in the labels, so pinning needs no new machinery.

        Adding `pulp-host-m5` narrows the leg from "anywhere in the pool" to "that
        box". This is how you move a slow SDK build off a machine that is busy
        serving something else.
        """
        out = resolve({"LINUX_ARM64": LOCAL_M5})
        self.assertIn("pulp-host-m5", out["linux-arm64"])

    def test_darwin_x64_can_go_local(self) -> None:
        """Proven capable: the release golden has Rosetta and cross-compiles x86_64."""
        out = resolve({"DARWIN_X64": '["self-hosted","macOS","ARM64","pulp-build-vm-release"]'})
        self.assertIn("pulp-build-vm-release", out["darwin-x64"])
        self.assertEqual(out["darwin-arm64"], HOSTED["darwin-arm64"])  # independent


class LegacyVariablesStillWork(unittest.TestCase):
    """An existing config must not break when this lands."""

    def test_legacy_macos_release_var_is_honoured(self) -> None:
        out = resolve({"LOCAL_MACOS": '["self-hosted","macOS","ARM64","pulp-build"]'})
        self.assertIn("pulp-build", out["darwin-arm64"])

    def test_the_new_per_leg_var_wins_over_the_legacy_one(self) -> None:
        out = resolve({
            "DARWIN_ARM64": '["self-hosted","new"]',
            "LOCAL_MACOS": '["self-hosted","legacy"]',
        })
        self.assertEqual(out["darwin-arm64"], ["self-hosted", "new"])


class FailLoud(unittest.TestCase):
    """A typo'd variable must abort, not route a release into the void."""

    def test_malformed_json_aborts(self) -> None:
        with self.assertRaises(SystemExit):
            resolve({"LINUX_ARM64": "self-hosted,Linux"})   # forgot the JSON

    def test_empty_labelset_aborts(self) -> None:
        with self.assertRaises(SystemExit):
            resolve({"LINUX_ARM64": "[]"})


if __name__ == "__main__":
    unittest.main()
