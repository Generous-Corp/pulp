#!/usr/bin/env python3
"""The capability-contract gate is wired into both push surfaces, with a base.

`agent_capability_manifest.py --check` compares this tree's public surface and
its two reserved counters against a *base*, and which base it resolves changes
the answer: left to itself it takes the merge base, while CI forces the base
tip. Measured on one tree in one second, those two resolutions produced 80 and
82 -- and 80 was the number a pull request had already carried into the merge
queue. So a gate that runs the check without naming a base is not a weaker
gate, it is a gate that answers a different question from the one that decides
whether the branch lands.

That matters more here than for the other gates because this failure cannot be
repaired afterwards. It is visible only once the merge group builds, and a pull
request that has entered the merge queue refuses a push (`GH006`), so the fix
is locked out by the same queue that is about to reject the branch.

These assertions are structural on purpose: the defect is a shell surface
losing its base ref or its predicate, and no synthetic repository reproduces
that. The predicate itself is exercised against real paths, so a narrowing that
silently stops covering installed headers fails here rather than in a merge
group.
"""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GATES = ROOT / "tools" / "scripts" / "gates.sh"
HOOK = ROOT / ".githooks" / "pre-push"

# A push surface must set this from a resolved commit, never from the raw base
# name, so the check answers against the tip CI will validate against.
BASE_ENV = "PULP_AGENT_CAPABILITY_BASE_REF"
RESOLVED = 'capability_base="$(git rev-parse --verify --quiet "$BASE^{commit}" || true)"'

PREDICATE = re.compile(r"\^\((?:[^)]*\|)*[^)]*\)")


def _predicate(text: str) -> str:
    """The extended-regex alternation each surface uses to decide relevance."""
    match = re.search(r"'(\^\([^']+\))'", text)
    if match is None:
        raise AssertionError("no capability path predicate found")
    return match.group(1)


class CapabilityContractGateWiring(unittest.TestCase):
    def setUp(self) -> None:
        self.gates = GATES.read_text()
        self.hook = HOOK.read_text()

    def test_both_push_surfaces_run_the_check(self) -> None:
        for name, text in (("gates.sh", self.gates), ("pre-push", self.hook)):
            with self.subTest(surface=name):
                self.assertIn("agent_capability_manifest.py", text)
                self.assertIn("--check", text)

    def test_both_surfaces_resolve_the_base_to_a_commit(self) -> None:
        for name, text in (("gates.sh", self.gates), ("pre-push", self.hook)):
            with self.subTest(surface=name):
                self.assertIn(RESOLVED, text)
                self.assertIn(f'{BASE_ENV}="$capability_base"', text)
                # The raw base name reaches the check only through the
                # resolution above; passing it directly re-opens the merge-base
                # answer that does not match CI.
                self.assertNotIn(f'{BASE_ENV}="$BASE"', text)

    def test_an_unresolvable_base_says_so_rather_than_passing(self) -> None:
        # A base that does not resolve means the gate measured nothing. Silence
        # there reads exactly like a clean run.
        self.assertIn("A skip is not a pass", self.gates)
        self.assertIn("SKIPPED", self.hook)

    def test_the_two_surfaces_share_one_predicate(self) -> None:
        self.assertEqual(_predicate(self.gates), _predicate(self.hook))

    def test_the_predicate_covers_what_can_take_a_counter(self) -> None:
        pattern = re.compile(_predicate(self.gates))
        covered = [
            "core/midi/include/pulp/midi/message.hpp",
            "core/format/include/pulp/format/processor.hpp",
            "tools/scripts/agent_capability_registry.py",
            "tools/scripts/agent_capability_manifest.py",
            "tools/agent-capabilities/legacy-unreviewed-baseline.json",
            "docs/status/agent-capability-surface.json",
            "docs/status/agent-capabilities.json",
        ]
        for path in covered:
            with self.subTest(path=path):
                self.assertTrue(pattern.search(path), f"{path} must be covered")

    def test_the_predicate_leaves_alone_what_cannot(self) -> None:
        # A branch touching none of the surface pays no part of the ~15s cost.
        uncovered = [
            "docs/guides/versioning.md",
            "core/midi/src/message.cpp",
            ".github/workflows/build.yml",
            "test/test_au_plugin_state.mm",
            "tools/scripts/gates.sh",
        ]
        pattern = re.compile(_predicate(self.gates))
        for path in uncovered:
            with self.subTest(path=path):
                self.assertFalse(pattern.search(path), f"{path} must not trigger")


if __name__ == "__main__":
    unittest.main(verbosity=2, argv=[sys.argv[0]])
