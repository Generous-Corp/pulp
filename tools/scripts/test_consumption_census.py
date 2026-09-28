#!/usr/bin/env python3
"""Fixture tests for the drift description in tools/scripts/consumption_census.py.

The drift gate's value is entirely in what it SAYS. The census lists public
headers by name under each exported include root (`public_headers_by_root`), so
a single new header under a root a target already exports drifts the census
with no target, symbol or CMake change — and a message that calls that a
closure change sends the reader to the link graph, where there is nothing to
find.

These drive `describe_drift` / `describe_header_drift` over the committed
census, so the fixture is the real document rather than a shape invented here.
Verified:

  1. A header drift names the file, the root and the targets exporting it —
     and never claims a closure changed. An unrecorded header fails; a
     recorded one that is gone fails; a root with no list fails.
  2. It also states the cause class once, so the reader learns that a header
     add alone is enough.
  2a. Two branches that each add a different header merge (a real git merge
     of the rendered census) into a list that matches the merged tree. The
     count the census used to record merged cleanly into a wrong number; that
     control is kept so the reason for names stays demonstrated.
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
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS.parent.parent
sys.path.insert(0, str(SCRIPTS))

import consumption_census as cc  # noqa: E402  (needs the path insert)
from consumption_census import describe_drift, describe_header_drift  # noqa: E402

CENSUS = REPO_ROOT / "docs" / "status" / "consumption-profiles.json"


def committed_document() -> dict:
    return json.loads(CENSUS.read_text())


def committed_profile() -> dict:
    document = committed_document()
    profiles = document["profiles"]
    if not profiles:
        raise AssertionError(f"{CENSUS} records no profile to drift")
    return profiles[sorted(profiles)[0]]


def target_with_one_root(profile: dict) -> str:
    for name in sorted(profile["targets"]):
        if len(profile["targets"][name]["public_headers"]["roots"]) == 1:
            return name
    raise AssertionError("no exported target exports exactly one include root")


def live_headers(document: dict, profile: dict) -> dict[str, list[str]]:
    """The committed lists for this profile's roots: what an unchanged tree reads."""
    return {r: list(document["public_headers_by_root"][r]) for r in cc.profile_roots(profile)}


class HeaderNameDrift(unittest.TestCase):
    def setUp(self) -> None:
        self.document = committed_document()
        self.profile = committed_profile()
        self.name = target_with_one_root(self.profile)
        self.root = self.profile["targets"][self.name]["public_headers"]["roots"][0]
        self.recorded = self.document["public_headers_by_root"]

    def test_unchanged_tree_reports_nothing(self) -> None:
        # Control: the committed lists against themselves.
        self.assertEqual(
            describe_header_drift(self.recorded, live_headers(self.document, self.profile),
                                  self.profile), [])

    def test_unrecorded_header_is_named_with_root_and_target(self) -> None:
        live = live_headers(self.document, self.profile)
        live[self.root] = sorted(live[self.root] + ["pulp/zz_unrecorded.hpp"])
        lines = describe_header_drift(self.recorded, live, self.profile)
        body = "\n".join(lines)
        self.assertIn(f"public header added under {self.root}", body)
        self.assertIn("pulp/zz_unrecorded.hpp", body)
        self.assertIn(f"Pulp::{self.name}", body)
        for line in lines:
            self.assertNotIn("closure", line, f"header drift claimed a closure change: {line}")
        cause = [line for line in lines if "already-exported include root" in line]
        self.assertEqual(len(cause), 1, f"expected one cause line, got {cause}")

    def test_recorded_header_that_is_gone_is_named(self) -> None:
        live = live_headers(self.document, self.profile)
        gone = live[self.root].pop(0)
        body = "\n".join(describe_header_drift(self.recorded, live, self.profile))
        self.assertIn(f"public header removed under {self.root}", body)
        self.assertIn(gone, body)

    def test_root_without_a_list_is_drift(self) -> None:
        recorded = dict(self.recorded)
        del recorded[self.root]
        body = "\n".join(describe_header_drift(
            recorded, live_headers(self.document, self.profile), self.profile))
        self.assertIn(f"exported include root {self.root}", body)
        self.assertIn("has no header list", body)

    def test_committed_lists_match_the_checkout(self) -> None:
        # The names are a fact of the source tree; the committed document
        # must list exactly what the checkout holds under every root.
        for root, names in self.recorded.items():
            self.assertEqual(names, cc.header_names(REPO_ROOT, root), root)


def git(cwd: pathlib.Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-c", "user.email=t@t", "-c", "user.name=t", *args],
        cwd=cwd, capture_output=True, text=True,
    )


class ParallelAdditionsMerge(unittest.TestCase):
    """Two branches each add a different header under the same root; the merge
    of their census edits must describe the merged tree."""

    def merge(self, base_text: str, edit_a, edit_b) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp)
            census = repo / "census.json"
            git(repo, "init", "-q", "-b", "main")
            census.write_text(base_text)
            git(repo, "add", "census.json")
            git(repo, "commit", "-qm", "base")
            for branch, edit in (("a", edit_a), ("b", edit_b)):
                git(repo, "checkout", "-q", "-b", branch, "main")
                census.write_text(edit(base_text))
                git(repo, "commit", "-qam", branch)
            git(repo, "checkout", "-q", "a")
            merged = git(repo, "merge", "-q", "--no-edit", "b")
            return merged.returncode, census.read_text()

    def setUp(self) -> None:
        self.document = committed_document()
        self.profile = committed_profile()
        self.root = self.profile["targets"][target_with_one_root(self.profile)][
            "public_headers"]["roots"][0]
        self.base_names = self.document["public_headers_by_root"][self.root]
        # Two names that sort far apart, as unrelated headers usually do, so
        # neither insertion sits in the other's diff context.
        self.new_a, self.new_b = "aaa_first_branch.hpp", "zzz_second_branch.hpp"

    def with_names(self, text: str, *added: str) -> str:
        document = json.loads(text)
        names = document["public_headers_by_root"][self.root]
        document["public_headers_by_root"][self.root] = sorted(names + list(added))
        return cc.render(document)

    def test_both_additions_survive_the_merge(self) -> None:
        base = cc.render(self.document)
        rc, text = self.merge(base, lambda t: self.with_names(t, self.new_a),
                              lambda t: self.with_names(t, self.new_b))
        self.assertEqual(rc, 0, "the two census edits conflicted")
        merged = json.loads(text)["public_headers_by_root"][self.root]
        tree = sorted(self.base_names + [self.new_a, self.new_b])
        self.assertEqual(merged, tree)
        self.assertEqual(
            describe_header_drift(json.loads(text)["public_headers_by_root"],
                                  {self.root: tree}, self.profile), [])

    def test_a_count_merges_cleanly_into_the_wrong_number(self) -> None:
        # Control: the shape the census used to record. Both branches write
        # the same N+1, git sees identical edits, and the merge is clean and
        # wrong for a tree that now holds N+2.
        n = len(self.base_names)
        base = json.dumps({"count": n}, indent=2) + "\n"
        bump = lambda t: json.dumps({"count": json.loads(t)["count"] + 1}, indent=2) + "\n"
        rc, text = self.merge(base, bump, bump)
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(text)["count"], n + 1)
        self.assertNotEqual(json.loads(text)["count"], n + 2)


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
        self.assertNotIn("public header", body)

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


class SummaryConsistency(unittest.TestCase):
    """The derived ranked summary must agree with every recorded target row."""

    def test_every_profile_summary_matches_target_rows(self) -> None:
        document = json.loads(CENSUS.read_text())
        for key, profile in document["profiles"].items():
            expected = [
                {
                    "exported_as": row["exported_as"],
                    "closure_node_count": row["closure"]["node_count"],
                }
                for _, row in sorted(
                    profile["targets"].items(),
                    key=lambda item: (-item[1]["closure"]["node_count"], item[0]),
                )
            ]
            self.assertEqual(
                profile["summary"]["ranked_by_closure"],
                expected,
                f"{key}: ranked summary is stale relative to its target rows",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
