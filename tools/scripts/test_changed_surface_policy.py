#!/usr/bin/env python3
"""Static, mutation, and exact-inventory contract for changed-surface policy."""

from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import os
import sys
import tomllib
import unittest
from pathlib import Path

import changed_surface_inventory as inventory


REPO_ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = REPO_ROOT / ".shipyard" / "config.toml"
INVENTORY_CONTRACT_PATH = REPO_ROOT / ".shipyard" / "changed-surface-inventory.json"


def load_config() -> dict:
    with CONFIG_PATH.open("rb") as config_file:
        return tomllib.load(config_file)


def load_policy() -> dict:
    return load_config()["targets"]["mac"]["changed_surface_selection"]


def load_inventory_contract() -> dict:
    return json.loads(INVENTORY_CONTRACT_PATH.read_text(encoding="utf-8"))


def matches(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def disposition(policy: dict, path: str) -> str:
    if path == ".shipyard/config.toml" or matches(path, policy["policy_paths"]):
        return "selector_policy"
    if matches(path, policy["test_topology_paths"]):
        return "test_topology"
    if matches(path, policy["full_required_paths"]):
        return "full_required"
    if matches(path, policy.get("baseline_only_paths", [])):
        return "mandatory"
    if any(matches(path, family["paths"]) for family in policy["families"]):
        return "bounded"
    return "unknown_full"


def literal_tests(policy: dict) -> set[str]:
    tests = set(policy["baseline_tests"])
    for family in policy["families"]:
        tests.update(family["tests"])
        tests.update(family.get("extended_tests", []))
    return tests


def fixture(
    name: str,
    executable: str = "/repo/build/bin/tests",
    *argv: str,
    properties: list[dict] | None = None,
) -> dict:
    return {
        "name": name,
        "command": [executable, *argv],
        "properties": properties
        if properties is not None
        else [{"name": "WORKING_DIRECTORY", "value": "/repo/build"}],
    }


class ChangedSurfacePolicyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.policy = load_policy()
        self.contract = load_inventory_contract()
        self.source_root = Path("/repo")
        self.build_dir = Path("/repo/build")

    def test_schema_v2_is_shadow_safe_and_literal(self) -> None:
        self.assertEqual(self.policy["schema_version"], 2)
        self.assertEqual(self.policy["build_type"], "debug")
        self.assertGreater(self.policy["full_test_count"], 20_000)
        self.assertEqual(
            self.policy["full_test_count"], self.contract["registration_count"]
        )
        self.assertTrue(self.policy["baseline_tests"])
        self.assertIn("changed-surface-policy-selftest", self.policy["baseline_tests"])
        self.assertTrue(self.policy["families"])
        self.assertNotIn("**", self.policy.get("baseline_only_paths", []))
        for family in self.policy["families"]:
            self.assertIn(family["risk_class"], {"low", "medium", "high"})
            self.assertTrue(family["tests"])
            self.assertTrue(family["paths"])
            if family["risk_class"] == "medium":
                self.assertTrue(family.get("extended_tests"))

    def test_shadow_policy_cannot_replace_full_execution_or_authorize_merge(self) -> None:
        config = load_config()
        full_test = config["validation"]["default"]["test"]
        self.assertIn("ctest --test-dir build", full_test)
        self.assertNotIn("changed-surface", full_test)
        self.assertNotIn("selected", full_test)
        self.assertFalse(
            {"authoritative", "execute", "merge_gate", "shadow_only"}
            & self.policy.keys()
        )
        self.assertEqual(config["merge"]["require_platforms"], ["macos", "linux", "windows"])

    def test_sensitive_and_unknown_surfaces_fail_closed(self) -> None:
        expected = {
            "tools/cmake/PulpDependencies.cmake": "full_required",
            "tools/cmake/toolchains/macos-arm64-osxcross.cmake": "full_required",
            "core/signal/include/pulp/signal/signal.hpp": "full_required",
            "tools/import-design/browser_capture/security.mjs": "full_required",
            "tools/rack/provenance_check.py": "full_required",
            "test/cmake/quality_tests.cmake": "test_topology",
            ".shipyard/config.toml": "selector_policy",
            ".shipyard/changed-surface-inventory.json": "selector_policy",
            "tools/scripts/changed_surface_inventory.py": "selector_policy",
            "core/future_subsystem/new_runtime.cpp": "unknown_full",
        }
        for path, reason in expected.items():
            with self.subTest(path=path):
                self.assertEqual(disposition(self.policy, path), reason)

    def test_full_required_mutations_remain_fail_closed_as_unknown(self) -> None:
        sentinels = [
            "tools/cmake/PulpDependencies.cmake",
            "core/signal/include/pulp/signal/signal.hpp",
            "tools/import-design/browser_capture/security.mjs",
            "tools/rack/provenance_check.py",
        ]
        for path in sentinels:
            with self.subTest(path=path):
                mutated = copy.deepcopy(self.policy)
                mutated["full_required_paths"] = [
                    pattern
                    for pattern in mutated["full_required_paths"]
                    if not fnmatch.fnmatchcase(path, pattern)
                ]
                self.assertEqual(disposition(mutated, path), "unknown_full")

    def test_only_reviewed_narrow_surfaces_are_bounded(self) -> None:
        self.assertEqual(disposition(self.policy, "docs/guides/local-ci.md"), "mandatory")
        self.assertEqual(
            disposition(self.policy, "docs/status/forge-catalog.json"), "unknown_full"
        )
        self.assertEqual(
            disposition(self.policy, "docs/status/dsp-capabilities.json"), "unknown_full"
        )
        self.assertEqual(disposition(self.policy, "tools/cli/cmd_forge.cpp"), "bounded")
        self.assertEqual(disposition(self.policy, "tools/cli/cmd_misc.cpp"), "unknown_full")
        self.assertEqual(disposition(self.policy, "tools/cli/new_command.cpp"), "unknown_full")

    def test_contract_pins_registration_multiset_not_unique_names(self) -> None:
        self.assertEqual(self.contract["schema_version"], 1)
        self.assertEqual(self.contract["registration_count"], 20_727)
        self.assertEqual(self.contract["unique_name_count"], 20_668)
        self.assertEqual(self.contract["unique_composite_count"], 20_727)
        self.assertEqual(self.contract["duplicate_name_group_count"], 55)
        self.assertEqual(self.contract["duplicate_name_excess_count"], 59)
        self.assertEqual(self.contract["duplicate_composite_group_count"], 0)
        self.assertEqual(
            self.contract["authoritative_filter_digest"],
            inventory.authoritative_filter_digest(),
        )
        self.assertRegex(self.contract["target_contract_digest"], r"^[0-9a-f]{64}$")
        self.assertRegex(self.contract["toolchain_digest"], r"^[0-9a-f]{64}$")
        self.assertRegex(self.contract["inventory_digest"], r"^[0-9a-f]{64}$")

    def test_authoritative_filter_matches_validation_command(self) -> None:
        tests = [
            fixture("keep"),
            fixture("AudioWorkgroup fixture"),
            fixture(
                "slow fixture",
                properties=[
                    {"name": "LABELS", "value": ["slow"]},
                    {"name": "WORKING_DIRECTORY", "value": "/repo/build"},
                ],
            ),
            fixture(
                "quality fixture",
                properties=[
                    {"name": "LABELS", "value": ["quality-lab"]},
                    {"name": "WORKING_DIRECTORY", "value": "/repo/build"},
                ],
            ),
        ]
        self.assertEqual(
            [test["name"] for test in inventory.authoritative_tests(tests)], ["keep"]
        )

    def test_duplicate_names_keep_distinct_commands_and_expand_all_instances(self) -> None:
        tests = [
            fixture("same title", "/repo/build/bin/one", "case"),
            fixture("same title", "/repo/build/bin/two", "case"),
        ]
        groups = inventory.inventory_groups(tests, self.source_root, self.build_dir)
        manifest = {"groups": groups, "duplicate_composite_group_count": 0}
        expanded = inventory.expand_literal_selection(manifest, ["same title"])
        self.assertEqual(len(expanded), 2)
        self.assertEqual(
            {group["composite"]["executable"] for group in expanded},
            {"build:bin/one", "build:bin/two"},
        )

    def test_registration_order_does_not_change_composite_inventory(self) -> None:
        tests = [fixture("a", "/repo/build/a"), fixture("b", "/repo/build/b")]
        forward = inventory.inventory_groups(tests, self.source_root, self.build_dir)
        reverse = inventory.inventory_groups(
            list(reversed(tests)), self.source_root, self.build_dir
        )
        self.assertEqual(forward, reverse)

    def test_command_change_changes_registration_fingerprint(self) -> None:
        first = inventory.inventory_groups(
            [fixture("same", "/repo/build/a")], self.source_root, self.build_dir
        )
        second = inventory.inventory_groups(
            [fixture("same", "/repo/build/b")], self.source_root, self.build_dir
        )
        self.assertNotEqual(first[0]["fingerprint"], second[0]["fingerprint"])

    def test_missing_or_relative_path_command_is_ambiguous(self) -> None:
        missing = fixture("missing")
        missing["command"] = []
        with self.assertRaisesRegex(inventory.InventoryError, "command"):
            inventory.inventory_groups([missing], self.source_root, self.build_dir)
        with self.assertRaisesRegex(inventory.InventoryError, "relative executable"):
            inventory.inventory_groups(
                [fixture("relative", "bin/test")], self.source_root, self.build_dir
            )

    def test_property_order_is_not_registration_identity(self) -> None:
        properties = [
            {"name": "LABELS", "value": ["one", "two"]},
            {"name": "WORKING_DIRECTORY", "value": "/repo/build"},
        ]
        forward = inventory.inventory_groups(
            [fixture("same", properties=properties)], self.source_root, self.build_dir
        )
        reverse = inventory.inventory_groups(
            [fixture("same", properties=list(reversed(properties)))],
            self.source_root,
            self.build_dir,
        )
        self.assertEqual(forward, reverse)

    def test_canonical_json_uses_jcs_number_boundaries(self) -> None:
        self.assertEqual(inventory.canonical_json({"n": 900.0}), b'{"n":900}')
        self.assertEqual(inventory.canonical_json({"n": 0.000001}), b'{"n":0.000001}')
        self.assertEqual(inventory.canonical_json({"n": 1e-7}), b'{"n":1e-7}')
        self.assertEqual(inventory.canonical_json({"n": 1e21}), b'{"n":1e+21}')

    def test_exact_duplicate_composite_is_ambiguous_and_fails_full(self) -> None:
        repeated = fixture("same", "/repo/build/a")
        groups = inventory.inventory_groups(
            [repeated, copy.deepcopy(repeated)], self.source_root, self.build_dir
        )
        self.assertEqual(groups[0]["multiplicity"], 2)
        with self.assertRaisesRegex(inventory.InventoryError, "ambiguous"):
            inventory.expand_literal_selection(
                {"groups": groups, "duplicate_composite_group_count": 1}, ["same"]
            )

    def test_duplicate_property_names_and_newline_names_fail_full(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "duplicate CTest property"):
            inventory.inventory_groups(
                [
                    fixture(
                        "same",
                        properties=[
                            {"name": "WORKING_DIRECTORY", "value": "/repo/build"},
                            {"name": "WORKING_DIRECTORY", "value": "/repo/build"},
                        ],
                    )
                ],
                self.source_root,
                self.build_dir,
            )
        with self.assertRaisesRegex(inventory.InventoryError, "newlines"):
            inventory.inventory_groups(
                [fixture("bad\nname")], self.source_root, self.build_dir
            )

    def test_path_anchors_are_boundary_safe_and_environment_is_structural(self) -> None:
        anchored = fixture(
            "paths",
            "/repo/build/bin/test",
            "/repo/source.cpp",
            properties=[
                {"name": "ENVIRONMENT", "value": ["ROOT=/repo", "OTHER=/repo-other"]},
                {"name": "WORKING_DIRECTORY", "value": "/repo/build"},
            ],
        )
        composite = inventory.registration_composite(
            anchored, self.source_root, self.build_dir
        )
        self.assertEqual(composite["executable"], "build:bin/test")
        self.assertEqual(composite["argv"], ["source:source.cpp"])
        environment = next(
            prop["value"]
            for prop in composite["properties"]
            if prop["name"] == "ENVIRONMENT"
        )
        self.assertEqual(environment, ["ROOT=source:.", "OTHER=external:repo-other"])

    def test_embedded_command_paths_are_word_boundary_anchored(self) -> None:
        first = fixture(
            "emit",
            "/repo/build/python",
            "--emit-cmd",
            "/usr/bin/python3 /repo/tools/emit.py --input /repo/data.json",
        )
        second = copy.deepcopy(first)
        second["command"] = [
            "/other/build/python",
            "--emit-cmd",
            "/opt/bin/python3 /other/tools/emit.py --input /other/data.json",
        ]
        second["properties"] = [
            {"name": "WORKING_DIRECTORY", "value": "/other/build"}
        ]
        first_group = inventory.inventory_groups(
            [first], Path("/repo"), Path("/repo/build")
        )
        second_group = inventory.inventory_groups(
            [second], Path("/other"), Path("/other/build")
        )
        self.assertEqual(first_group, second_group)

    def test_contract_digest_drift_forces_full(self) -> None:
        observed = dict(self.contract)
        observed["inventory_digest"] = "0" * 64
        with self.assertRaisesRegex(inventory.InventoryError, "require full suite"):
            inventory.validate_manifest(observed, self.contract)

    def test_literal_selection_rejects_missing_or_repeated_requests(self) -> None:
        groups = inventory.inventory_groups(
            [fixture("one")], self.source_root, self.build_dir
        )
        manifest = {"groups": groups, "duplicate_composite_group_count": 0}
        with self.assertRaisesRegex(inventory.InventoryError, "absent"):
            inventory.expand_literal_selection(manifest, ["missing"])
        with self.assertRaisesRegex(inventory.InventoryError, "duplicate requested"):
            inventory.expand_literal_selection(manifest, ["one", "one"])


def validate_ctest_inventory(build_dir: Path, manifest_output: Path | None = None) -> None:
    policy = load_policy()
    source_root = inventory.source_root_for_build(build_dir)
    manifest = inventory.build_manifest(
        inventory.load_ctest_json(build_dir),
        source_root,
        Path(os.path.abspath(build_dir)),
        policy,
    )
    inventory.validate_manifest(manifest, load_inventory_contract())
    missing = sorted(
        literal_tests(policy)
        - {group["composite"]["name"] for group in manifest["groups"]}
    )
    if missing:
        raise inventory.InventoryError(
            f"policy names tests absent from CTest inventory: {missing}"
        )
    inventory.expand_literal_selection(manifest, literal_tests(policy))
    if manifest_output is not None:
        manifest_output.write_bytes(inventory.canonical_json(manifest) + b"\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--write-manifest", type=Path)
    args, _ = parser.parse_known_args()
    if args.write_manifest is not None and args.build_dir is None:
        parser.error("--write-manifest requires --build-dir")
    if args.build_dir is not None:
        validate_ctest_inventory(args.build_dir, args.write_manifest)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ChangedSurfacePolicyTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
