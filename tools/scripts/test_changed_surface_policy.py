#!/usr/bin/env python3
"""Static and mutation contract for Pulp's Shipyard changed-surface policy."""

from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = REPO_ROOT / ".shipyard" / "config.toml"


def load_config() -> dict:
    with CONFIG_PATH.open("rb") as config_file:
        return tomllib.load(config_file)


def load_policy() -> dict:
    return load_config()["targets"]["mac"]["changed_surface_selection"]


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


class ChangedSurfacePolicyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.policy = load_policy()

    def test_schema_v2_is_shadow_safe_and_literal(self) -> None:
        self.assertEqual(self.policy["schema_version"], 2)
        self.assertEqual(self.policy["build_type"], "debug")
        self.assertGreater(self.policy["full_test_count"], 20_000)
        self.assertTrue(self.policy["baseline_tests"])
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

    def test_inventory_cardinality_drift_is_rejected(self) -> None:
        inventory = literal_tests(self.policy)
        inventory.update(
            f"inventory-placeholder-{index}"
            for index in range(self.policy["full_test_count"] - len(inventory))
        )
        validate_inventory(self.policy, inventory)
        inventory.pop()
        with self.assertRaisesRegex(AssertionError, "full_test_count"):
            validate_inventory(self.policy, inventory)

    def test_inventory_scope_requires_the_declared_mac_debug_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "CMAKE_BUILD_TYPE:STRING=Debug\n"
                "PULP_BUILD_TESTS:BOOL=ON\n"
                "PULP_BUILD_EXAMPLES:BOOL=ON\n",
                encoding="utf-8",
            )
            self.assertTrue(inventory_matches_policy_target(build_dir, platform="darwin"))
            self.assertFalse(inventory_matches_policy_target(build_dir, platform="linux"))
            cache.write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "PULP_BUILD_TESTS:BOOL=ON\n"
                "PULP_BUILD_EXAMPLES:BOOL=OFF\n",
                encoding="utf-8",
            )
            self.assertFalse(inventory_matches_policy_target(build_dir, platform="darwin"))


def validate_inventory(policy: dict, inventory: set[str]) -> None:
    expected_count = policy["full_test_count"]
    if len(inventory) != expected_count:
        raise AssertionError(
            f"full_test_count={expected_count} but CTest inventory has {len(inventory)} tests"
        )
    missing = sorted(literal_tests(policy) - inventory)
    if missing:
        raise AssertionError(f"policy names tests absent from CTest inventory: {missing}")


def inventory_matches_policy_target(build_dir: Path, *, platform: str = sys.platform) -> bool:
    if platform != "darwin":
        return False
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return False
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_with_type, value = line.split("=", 1)
        key = key_with_type.split(":", 1)[0]
        values[key] = value
    return (
        values.get("CMAKE_BUILD_TYPE", "").casefold() == "debug"
        and values.get("PULP_BUILD_TESTS") == "ON"
        and values.get("PULP_BUILD_EXAMPLES") == "ON"
    )


def validate_ctest_inventory(build_dir: Path) -> None:
    if not inventory_matches_policy_target(build_dir):
        print(
            "changed-surface-policy: inventory check skipped outside the "
            "declared macOS Debug/examples-on target"
        )
        return
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    inventory = {test["name"] for test in json.loads(result.stdout)["tests"]}
    validate_inventory(load_policy(), inventory)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    args, _ = parser.parse_known_args()
    if args.build_dir is not None:
        validate_ctest_inventory(args.build_dir)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ChangedSurfacePolicyTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
