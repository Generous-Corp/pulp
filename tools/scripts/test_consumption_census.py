#!/usr/bin/env python3
"""Fixture tests for the drift description in tools/scripts/consumption_census.py.

The drift gate's value is entirely in what it SAYS. `public_headers.count`
comes from walking each target's exported include roots, so a single new header
under a root a target already exports drifts the census with no target, symbol
or CMake change — and a message that calls that a closure change sends the
reader to the link graph, where there is nothing to find.

These drive `describe_drift` over the committed census, so the fixture is the
real document rather than a shape invented here. Verified:

  1. A header-count drift names the target, both counts, and the exported
     include root the count came from — and never claims a closure changed.
  2. It also states the cause class once, so the reader learns that a header
     add alone is enough.
  3. A genuine closure change is still reported as one, with the member that
     arrived or left named.
  4. An external-dependency change is named as such, duplicates included.
  5. A changed field with no dedicated phrasing names the field rather than
     falling back to a closure claim.

Run:
    python3 tools/scripts/test_consumption_census.py
"""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS.parent.parent
sys.path.insert(0, str(SCRIPTS))

from consumption_census import describe_drift  # noqa: E402  (needs the path insert)

CENSUS = REPO_ROOT / "docs" / "status" / "consumption-profiles.json"


def committed_profile() -> dict:
    document = json.loads(CENSUS.read_text())
    profiles = document["profiles"]
    if not profiles:
        raise AssertionError(f"{CENSUS} records no profile to drift")
    return profiles[sorted(profiles)[0]]


def target_with_one_root(profile: dict) -> str:
    for name in sorted(profile["targets"]):
        if len(profile["targets"][name]["public_headers"]["roots"]) == 1:
            return name
    raise AssertionError("no exported target exports exactly one include root")


def add_one_header(profile: dict, name: str) -> None:
    """Exactly what a new header under an already-exported root does."""
    profile["targets"][name]["public_headers"]["count"] += 1
    for row in profile["summary"]["ranked_by_closure"]:
        if row["exported_as"] == f"Pulp::{name}":
            row["public_header_count"] += 1


class HeaderCountDrift(unittest.TestCase):
    def setUp(self) -> None:
        self.recorded = committed_profile()
        self.name = target_with_one_root(self.recorded)
        self.row = self.recorded["targets"][self.name]
        self.current = copy.deepcopy(self.recorded)
        add_one_header(self.current, self.name)
        self.lines = describe_drift(self.recorded, self.current)

    def test_names_target_both_counts_and_root(self) -> None:
        old = self.row["public_headers"]["count"]
        root = self.row["public_headers"]["roots"][0]
        named = [line for line in self.lines if f"Pulp::{self.name}:" in line]
        self.assertTrue(named, f"no line names Pulp::{self.name}: {self.lines}")
        body = "\n".join(named)
        self.assertIn("public header count", body)
        self.assertIn(f"{old} -> {old + 1}", body)
        self.assertIn(root, body)

    def test_never_reports_a_closure_change(self) -> None:
        for line in self.lines:
            self.assertNotIn("closure", line, f"header drift claimed a closure change: {line}")

    def test_states_the_cause_once(self) -> None:
        cause = [line for line in self.lines if "already-exported include root" in line]
        self.assertEqual(len(cause), 1, f"expected one cause line, got {cause}")


class OtherDriftKindsStayAccurate(unittest.TestCase):
    def setUp(self) -> None:
        self.recorded = committed_profile()
        self.name = sorted(self.recorded["targets"])[0]

    def drift(self, change) -> str:
        current = copy.deepcopy(self.recorded)
        change(current["targets"][self.name])
        return "\n".join(describe_drift(self.recorded, current))

    def test_closure_growth_is_still_a_closure_line(self) -> None:
        def grow(row: dict) -> None:
            row["closure"]["node_count"] += 1
            row["closure"]["pulp_targets"].append("pulp-arrived")

        body = self.drift(grow)
        old = self.recorded["targets"][self.name]["closure"]["node_count"]
        self.assertIn(f"closure node count {old} -> {old + 1}", body)
        self.assertIn("pulp-arrived", body)
        self.assertNotIn("public header count", body)

    def test_closure_member_swap_at_equal_count_is_named(self) -> None:
        def swap(row: dict) -> None:
            row["closure"]["pulp_targets"][0] = "pulp-substituted"

        body = self.drift(swap)
        self.assertIn("pulp-substituted", body)
        self.assertIn("removed", body)

    def test_external_dependency_change_is_named_as_one(self) -> None:
        def add_framework(row: dict) -> None:
            row["external_dependencies"]["frameworks"].append("NotAFramework")
            row["external_dependencies"]["count"] += 1

        body = self.drift(add_framework)
        self.assertIn("framework added: NotAFramework", body)
        self.assertIn("external dependency count", body)

    def test_duplicate_entry_is_not_called_an_ordering_change(self) -> None:
        def duplicate(row: dict) -> None:
            row["direct_dependencies"].append(row["direct_dependencies"][0])

        body = self.drift(duplicate)
        self.assertIn("direct dependency added", body)

    def test_unphrased_field_names_the_field_not_the_closure(self) -> None:
        def stamp(row: dict) -> None:
            row["exported_as"] = "Pulp::renamed"

        body = self.drift(stamp)
        self.assertIn("exported_as", body)
        self.assertNotIn("closure", body)


class AbsentTargets(unittest.TestCase):
    def test_added_and_removed_targets_are_named(self) -> None:
        recorded = committed_profile()
        name = sorted(recorded["targets"])[0]
        current = copy.deepcopy(recorded)
        current["targets"]["invented"] = copy.deepcopy(current["targets"].pop(name))
        body = "\n".join(describe_drift(recorded, current))
        self.assertIn("new exported target: Pulp::invented", body)
        self.assertIn(f"exported target gone: Pulp::{name}", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
