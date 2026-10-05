#!/usr/bin/env python3
"""Static, mutation, and live-inventory contract for changed-surface policy."""

from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import os
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

import changed_surface_inventory as inventory
import run_changed_surface_tests as runner
import changed_surface_script_families as script_families


REPO_ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = REPO_ROOT / ".shipyard" / "config.toml"
INVENTORY_CONTRACT_PATH = REPO_ROOT / ".shipyard" / "changed-surface-inventory.json"

CHILD_PROCESS_TESTS = [
    "exec captures stdout",
    "exec captures non-zero exit code",
    "exec respects timeout",
    "line callback fires per line",
    "line callback exceptions cancel the child during wait",
    "line callback failure preserves the other output stream during cancellation",
    "line callback exceptions cancel a child during input-channel setup",
    "cancel terminates process",
    "process_id is available and stable after wait",
    "find_on_path finds known binary",
    "find_on_path returns nullopt for missing binary",
    "exec with a binary that does not exist reports failure",
    "exec captures empty-stdout cleanly",
    "find_on_path returns nullopt for nonexistent",
    "run with empty command fails gracefully",
    "stderr is captured separately",
    "wait and read before start return default results",
    "disabled stream capture discards stdout stderr and callbacks",
    "read_available_output drains stdout while process is running",
    "read_available_output is empty when stdout capture is disabled",
    "run honors working directory",
    "output capture respects max byte limits",
    "zero max output bytes drains without capturing lines",
    "stderr line callback fires independently",
    "line callback buffers partial stdout without trailing newline",
    "line callback buffers partial stderr without trailing newline",
    "move assignment transfers a running child process",
    "move constructor transfers a running child process",
    "wait is idempotent after process completion",
    "start after observed completion waits previous child without cancellation state",
    "cancel after natural exit reports completed process result",
    "process reuse clears timeout state from prior run",
    "process reuse clears cancellation state from prior run",
    "start cancels a running child before launching replacement",
    "failed restart after completion does not retain stale child state",
    "argv arguments preserve empty strings spaces and punctuation",
    "line callback emits empty lines and preserves order",
    "line callback joins a line split across drain passes",
    "line callback honors output cap before later complete lines",
    "read_available_output drains stdout without losing stderr result",
    "timeout preserves output emitted before termination",
    "wait preserves output after is_running observes fast exit",
    "ChildProcess destructor cancels a still-running POSIX child",
    "ChildProcess move construction transfers a running POSIX child",
    "ChildProcess move assignment transfers a running POSIX child",
    "cancel escalates when a POSIX child ignores SIGTERM",
    "timeout escalates when a POSIX child ignores SIGTERM",
    "starting a POSIX replacement cancels a still-running child",
    "starting a POSIX replacement drains a previously observed exit",
    "POSIX wait consumes cached fast-exit status after polling",
]

FORGE_RACK_GENERATOR_TESTS = [
    "rack-generator-safety",
    "rack-generator-endings",
]

FORGE_RACK_GENERATOR_EXTENDED_TESTS = [
    "rack-generation-eligibility",
    "rack-inert-input-cause",
]


def load_config() -> dict:
    with CONFIG_PATH.open("rb") as config_file:
        return tomllib.load(config_file)


def load_policy() -> dict:
    # The same merge Shipyard applies to the selection's families_file.
    return runner.merge_families_file(
        load_config()["targets"]["mac"]["changed_surface_selection"], REPO_ROOT
    )


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


def selected_build_targets(policy: dict, family_name: str) -> set[str]:
    targets = set(policy["baseline_build_targets"])
    family = next(f for f in policy["families"] if f["name"] == family_name)
    targets.update(family["build_targets"])
    return targets


def selection_mode(policy: dict, paths: list[str]) -> str:
    dispositions = {disposition(policy, path) for path in paths}
    if dispositions <= {"mandatory"}:
        return "mandatory"
    if dispositions <= {"mandatory", "bounded"} and "bounded" in dispositions:
        return "bounded"
    return "full"


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
        self.source_root = Path("/repo")
        self.build_dir = Path("/repo/build")

    def test_schema_v3_is_shadow_safe_and_binds_literal_build_targets(self) -> None:
        self.assertEqual(self.policy["schema_version"], 3)
        self.assertEqual(self.policy["build_type"], "debug")
        self.assertGreater(self.policy["full_test_count"], 20_000)
        self.assertFalse(INVENTORY_CONTRACT_PATH.exists(),
                         "the committed inventory pin is retired; bounded runs compare with the base")
        self.assertTrue(self.policy["baseline_tests"])
        self.assertTrue(self.policy["baseline_build_targets"])
        self.assertIn("changed-surface-policy-selftest", self.policy["baseline_tests"])
        # The live-tree inventory check, registered only under Shipyard's
        # PULP_CHANGED_SURFACE_INVENTORY_TARGET, is what a bounded leg relies on.
        self.assertIn("changed-surface-policy-inventory", self.policy["baseline_tests"])
        self.assertTrue(self.policy["families"])
        self.assertNotIn("**", self.policy.get("baseline_only_paths", []))
        for family in self.policy["families"]:
            self.assertIn(family["risk_class"], {"low", "medium", "high"})
            self.assertTrue(family["tests"])
            self.assertTrue(family["build_targets"])
            self.assertTrue(family["paths"])
            if family["risk_class"] == "medium":
                self.assertTrue(family.get("extended_tests"))

    def test_the_lane_build_is_not_universal(self) -> None:
        # A universal build downloads a WebGPU slice outside FetchContent, so
        # a bounded run could not derive its base inventory offline.
        architectures = [flag for flag in self.policy["build_flags"]
                         if flag.startswith("-DCMAKE_OSX_ARCHITECTURES=")]
        self.assertFalse(any(";" in flag for flag in architectures), architectures)

    def test_shadow_policy_cannot_replace_full_execution_or_authorize_merge(self) -> None:
        config = load_config()
        full_test = config["validation"]["default"]["test"]
        self.assertIn("ctest --test-dir build", full_test)
        self.assertNotIn("changed-surface", full_test)
        self.assertNotIn("selected", full_test)
        self.assertEqual(self.policy["execution"]["stage"], "build_and_test")
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
            "tools/scripts/run_changed_surface_tests.py": "selector_policy",
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
            disposition(self.policy, "docs/validation/fast-trigonometry.md"),
            "mandatory",
        )
        self.assertEqual(
            disposition(self.policy, "docs/status/forge-catalog.json"), "unknown_full"
        )
        self.assertEqual(
            disposition(self.policy, "docs/status/dsp-capabilities.json"), "unknown_full"
        )
        self.assertEqual(disposition(self.policy, "tools/cli/cmd_forge.cpp"), "bounded")
        self.assertEqual(disposition(self.policy, "tools/cli/cmd_misc.cpp"), "unknown_full")
        self.assertEqual(disposition(self.policy, "tools/cli/new_command.cpp"), "unknown_full")

    def test_validation_docs_are_mobile_skip_safe_without_widening_status_data(self) -> None:
        self.assertIn("docs/validation/**", self.policy["baseline_only_paths"])
        self.assertTrue(
            any(
                fnmatch.fnmatchcase("docs/validation/fast-trigonometry.md", pattern)
                for pattern in self.policy["ios_compile_skip_safe_paths"]
            )
        )
        self.assertEqual(
            disposition(self.policy, "docs/status/sequencer-exposure.json"),
            "unknown_full",
        )

    def test_child_process_family_is_exact_and_deduplicates_mandatory_work(self) -> None:
        family = next(
            family
            for family in self.policy["families"]
            if family["name"] == "child-process-test-source-only"
        )
        self.assertEqual(family["paths"], ["test/test_child_process.cpp"])
        self.assertEqual(family["tests"], CHILD_PROCESS_TESTS)
        self.assertEqual(len(family["tests"]), 50)
        self.assertEqual(family["build_targets"], ["pulp-test-child-process"])
        self.assertEqual(family["supported_build_types"], ["debug", "release"])
        self.assertEqual(family["risk_class"], "low")

        selected_tests = set(self.policy["baseline_tests"]) | set(family["tests"])
        self.assertEqual(len(selected_tests), 56)
        self.assertEqual(
            selected_build_targets(self.policy, family["name"]),
            {"pulp-test-build-check", "pulp-cli", "pulp-test-child-process"},
        )

    def test_child_process_family_keeps_neighboring_surfaces_full(self) -> None:
        expected = {
            "test/test_child_process.cpp": "bounded",
            "test/test_child_process_standard_input.cpp": "unknown_full",
            "test/fixtures/child_process_input_fixture.cpp": "unknown_full",
            "test/test_runtime_utils.cpp": "unknown_full",
            "core/platform/src/child_process.cpp": "unknown_full",
            "core/platform/include/pulp/platform/child_process.hpp": "full_required",
        }
        for path, reason in expected.items():
            with self.subTest(path=path):
                self.assertEqual(disposition(self.policy, path), reason)

        self.assertEqual(
            selection_mode(
                self.policy,
                ["test/test_child_process.cpp", "test/test_runtime_utils.cpp"],
            ),
            "full",
        )

    def test_forge_rack_generator_family_is_exact_and_keeps_neighbors_full(self) -> None:
        family = next(
            family
            for family in self.policy["families"]
            if family["name"] == "forge-rack-generator"
        )
        self.assertEqual(
            family["paths"],
            [
                "tools/rack/generate.py",
                "tools/rack/test_generate_safety.py",
                ".agents/skills/forge-modular/SKILL.md",
            ],
        )
        self.assertEqual(family["tests"], FORGE_RACK_GENERATOR_TESTS)
        self.assertEqual(
            family["extended_tests"], FORGE_RACK_GENERATOR_EXTENDED_TESTS
        )
        self.assertEqual(family["build_targets"], ["pulp-cli"])
        self.assertEqual(family["supported_build_types"], ["debug", "release"])
        self.assertEqual(family["risk_class"], "medium")

        for path in family["paths"]:
            with self.subTest(path=path):
                self.assertEqual(disposition(self.policy, path), "bounded")

        neighboring_paths = [
            "tools/rack/patch.py",
            "tools/rack/provenance_check.py",
            "tools/rack/test_acid_preflight.py",
        ]
        for path in neighboring_paths:
            with self.subTest(path=path):
                self.assertNotEqual(disposition(self.policy, path), "bounded")
        # A neighboring skill doc selects through the skill-doc readers, never
        # through the Rack generator's tests.
        self.assertFalse(
            matches(".agents/skills/forge-app-delivery/SKILL.md", family["paths"])
        )

        self.assertEqual(
            selection_mode(self.policy, family["paths"]),
            "bounded",
        )
        self.assertEqual(
            selection_mode(
                self.policy,
                ["tools/rack/generate.py", "tools/rack/patch.py"],
            ),
            "full",
        )

    def test_test_topology_is_narrow_and_precedes_family_matching(self) -> None:
        topology_paths = {
            "CMakeLists.txt",
            "test/CMakeLists.txt",
            "test/cmake/native_component_tests.cmake",
        }
        for path in topology_paths:
            with self.subTest(path=path):
                self.assertEqual(disposition(self.policy, path), "test_topology")

        self.assertNotIn("test/**", self.policy["test_topology_paths"])
        # A Python test script's body is not registration; its edits select
        # the ctests that run it through the generated families.
        self.assertFalse(
            matches("tools/scripts/test_runner_topology_check.py",
                    self.policy["test_topology_paths"])
        )
        self.assertIn("test/cmake/**", self.policy["test_topology_paths"])
        self.assertEqual(
            disposition(self.policy, "tools/scripts/test_changed_surface_policy.py"),
            "selector_policy",
        )

        mutated = copy.deepcopy(self.policy)
        child_family = next(
            family
            for family in mutated["families"]
            if family["name"] == "child-process-test-source-only"
        )
        child_family["paths"].append("test/cmake/native_component_tests.cmake")
        self.assertEqual(
            disposition(mutated, "test/cmake/native_component_tests.cmake"),
            "test_topology",
        )

    def generated_families(self) -> list[dict]:
        return tomllib.loads((REPO_ROOT / script_families.FAMILIES_FILE).read_text(
            encoding="utf-8"))["families"]

    def test_generated_families_live_in_the_families_file(self) -> None:
        selection = load_config()["targets"]["mac"]["changed_surface_selection"]
        self.assertEqual(selection["families_file"], str(script_families.FAMILIES_FILE))
        inline = {f["name"] for f in selection.get("families", [])}
        self.assertFalse(inline & {f["name"] for f in self.generated_families()})
        self.assertIn(str(script_families.FAMILIES_FILE), self.policy["policy_paths"])
        self.assertEqual(
            disposition(self.policy, str(script_families.FAMILIES_FILE)), "selector_policy"
        )

    def test_families_file_merge_fails_closed(self) -> None:
        base = {"families": [{"name": "inline"}], "policy_paths": []}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            (root / ".shipyard").mkdir(parents=True)
            (root / "tools").mkdir()
            valid = "[[families]]\nname = \"file\"\n"
            # Every path below exists with valid content, so only the path rule refuses it.
            for rel in ["../x.toml", "tools/x.toml", ".shipyard/x.json", ".shipyard/../x.toml"]:
                (root / rel).write_text(valid)
            for path in ["../x.toml", "tools/x.toml", ".shipyard/x.json", ".shipyard/../x.toml"]:
                with self.subTest(path=path):
                    with self.assertRaisesRegex(runner.SelectionExecutionError, "under .shipyard"):
                        runner.merge_families_file({**base, "families_file": path}, root)
            target = root / ".shipyard" / "f.toml"
            for body in ["", "families = []\n", "x = 1\n[[families]]\nname = \"a\"\n"]:
                target.write_text(body)
                with self.subTest(body=body), self.assertRaises(runner.SelectionExecutionError):
                    runner.merge_families_file({**base, "families_file": ".shipyard/f.toml"}, root)
            target.write_text("[[families]]\nname = \"file\"\n")
            merged = runner.merge_families_file({**base, "families_file": ".shipyard/f.toml"}, root)
        self.assertEqual([f["name"] for f in merged["families"]], ["inline", "file"])
        self.assertEqual(merged["policy_paths"], [".shipyard/f.toml"])
        self.assertNotIn("families_file", merged)

    def test_policy_prose_does_not_force_full_validation(self) -> None:
        self.assertNotIn(".agents/skills/ci/SKILL.md", self.policy["policy_paths"])
        self.assertEqual(disposition(self.policy, ".agents/skills/ci/SKILL.md"), "bounded")
        for path in [
            "tools/scripts/changed_surface_script_families.py",
            "test/ctest_script_inputs.json",
        ]:
            with self.subTest(path=path):
                self.assertEqual(disposition(self.policy, path), "selector_policy")

    def test_generated_script_families_are_exact_and_carry_whole_tree_tests(self) -> None:
        generated = self.generated_families()
        names = {family["name"] for family in generated}
        self.assertIn("script-surface-whole-tree", names)
        self.assertIn("agent-skill-docs", names)
        whole_tree = next(f for f in generated if f["name"] == "script-surface-whole-tree")
        self.assertTrue(whole_tree["tests"])
        self.assertTrue(all(
            script_families.WHOLE_TREE_NAME_RE.search(test) for test in whole_tree["tests"]))
        covered = set(whole_tree["paths"])
        for family in generated:
            with self.subTest(family=family["name"]):
                self.assertEqual(family["risk_class"], "low")
                self.assertIn("pulp-cli", family["build_targets"])
                self.assertTrue(set(family["paths"]) <= covered)
                for path in family["paths"]:
                    # Literal paths only: a pattern could map a script the
                    # generator excluded.
                    self.assertTrue(path == script_families.SKILL_DOC_PATTERN
                                    or not any(c in path for c in "*?["), path)

    def test_scripts_native_code_runs_stay_full(self) -> None:
        # The CLI runs these scripts, so no ctest input list can bound them.
        for path in [
            "tools/scripts/dsp_capability_registry.py",
            "tools/scripts/version_bump_check.py",
            "tools/scripts/generate_widget_bridge_api.py",
        ]:
            with self.subTest(path=path):
                self.assertEqual(disposition(self.policy, path), "unknown_full")
        self.assertEqual(
            selection_mode(self.policy, [".agents/skills/ci/SKILL.md",
                                         "tools/scripts/version_bump_check.py"]),
            "full",
        )

    def test_authoritative_filter_digest_is_stable(self) -> None:
        self.assertEqual(
            inventory.authoritative_filter_digest(), inventory.authoritative_filter_digest()
        )
        self.assertRegex(inventory.authoritative_filter_digest(), r"^[0-9a-f]{64}$")

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

    def test_external_basename_portability_retains_resolution_in_toolchain_digest(self) -> None:
        first_tests = [fixture("same", "/opt/tool-a/bin/runner")]
        second_tests = [fixture("same", "/opt/tool-b/bin/runner")]
        first_groups = inventory.inventory_groups(
            first_tests, self.source_root, self.build_dir
        )
        second_groups = inventory.inventory_groups(
            second_tests, self.source_root, self.build_dir
        )
        self.assertEqual(first_groups, second_groups)
        first_toolchain = inventory._toolchain_contract(
            self.build_dir, first_groups, first_tests, self.source_root
        )
        second_toolchain = inventory._toolchain_contract(
            self.build_dir, second_groups, second_tests, self.source_root
        )
        self.assertNotEqual(
            inventory.contract_digest(first_toolchain),
            inventory.contract_digest(second_toolchain),
        )

    def test_missing_or_relative_path_command_is_ambiguous(self) -> None:
        missing = fixture("missing")
        missing["command"] = []
        with self.assertRaisesRegex(inventory.InventoryError, "command"):
            inventory.inventory_groups([missing], self.source_root, self.build_dir)
        with self.assertRaisesRegex(inventory.InventoryError, "relative executable"):
            inventory.inventory_groups(
                [fixture("relative", "bin/test")], self.source_root, self.build_dir
            )

    def test_only_exact_generated_not_built_placeholders_are_separable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            name = "pulp-test-example_NOT_BUILT-b12d07c"
            tests_path = build / "pulp-test-example-b12d07c_tests.cmake"
            include_path = build / "pulp-test-example-b12d07c_include.cmake"
            include_path.write_text(
                f'if(EXISTS "{tests_path}")\n'
                f'  include("{tests_path}")\n'
                "else()\n"
                f"  add_test({name} {name})\n"
                "endif()\n",
                encoding="utf-8",
            )
            placeholder = {
                "name": name,
                "properties": [
                    {"name": "WORKING_DIRECTORY", "value": str(build)}
                ],
            }
            ready, placeholders = inventory.split_proven_unbuilt_placeholders(
                [fixture("ready", str(build / "ready")), placeholder], build
            )
            self.assertEqual([test["name"] for test in ready], ["ready"])
            self.assertEqual(placeholders, [placeholder])

            include_path.write_text("# attacker-controlled lookalike\n", encoding="utf-8")
            with self.assertRaisesRegex(inventory.InventoryError, "unambiguous command"):
                inventory.split_proven_unbuilt_placeholders([placeholder], build)
            with self.assertRaisesRegex(inventory.InventoryError, "unambiguous command"):
                inventory.split_proven_unbuilt_placeholders(
                    [{"name": name, "properties": []}], build
                )

    def test_a_commandless_excluded_registration_is_not_the_inventorys_to_prove(self) -> None:
        # A validator the host lacks (or that find_program resolved to a path
        # that does not exist) lists no command even after the build. A
        # registration the authoritative filter drops is never selected or
        # compared, so it is skipped; the same registration unexcluded refuses.
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            excluded = {
                "name": "pluginval-PulpGain-VST3",
                "properties": [
                    {"name": "LABELS", "value": ["validation", "vst3"]},
                    {"name": "WORKING_DIRECTORY", "value": str(build)},
                ],
            }
            ready, placeholders = inventory.split_proven_unbuilt_placeholders(
                [fixture("ready", str(build / "ready")), excluded], build
            )
            self.assertEqual([test["name"] for test in ready], ["ready"])
            self.assertEqual(placeholders, [])
            unexcluded = {**excluded, "properties": excluded["properties"][1:]}
            with self.assertRaisesRegex(inventory.InventoryError, "unambiguous command"):
                inventory.split_proven_unbuilt_placeholders([unexcluded], build)

    def test_commandless_direct_test_requires_ctestfile_and_codemodel_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            reply = build / ".cmake" / "api" / "v1" / "reply"
            reply.mkdir(parents=True)
            (reply / "index-exact.json").write_text(
                json.dumps(
                    {"reply": {"codemodel-v2": {"jsonFile": "codemodel.json"}}}
                ),
                encoding="utf-8",
            )
            (reply / "codemodel.json").write_text(
                json.dumps(
                    {
                        "configurations": [
                            {"targets": [{"jsonFile": "target.json"}]}
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (reply / "target.json").write_text(
                json.dumps(
                    {"name": "pulp-test-direct", "artifacts": [{"path": "direct"}]}
                ),
                encoding="utf-8",
            )
            (build / "CTestTestfile.cmake").write_text(
                f'add_test([=[direct-test]=] "{build / "direct"}")\n',
                encoding="utf-8",
            )
            placeholder = {
                "backtrace": 1,
                "name": "direct-test",
                "properties": [
                    {"name": "WORKING_DIRECTORY", "value": str(build)}
                ],
            }
            ready, placeholders = inventory.split_proven_unbuilt_placeholders(
                [placeholder], build
            )
            self.assertEqual(ready, [])
            self.assertEqual(placeholders, [placeholder])

            forged = copy.deepcopy(placeholder)
            forged["name"] = "other-test"
            with self.assertRaisesRegex(inventory.InventoryError, "unambiguous command"):
                inventory.split_proven_unbuilt_placeholders([forged], build)
            del placeholder["backtrace"]
            with self.assertRaisesRegex(inventory.InventoryError, "unambiguous command"):
                inventory.split_proven_unbuilt_placeholders([placeholder], build)

    @staticmethod
    def partly_built_tree(build: Path) -> None:
        """A build tree whose `direct` program is a CMake artifact not built yet."""
        reply = build / ".cmake" / "api" / "v1" / "reply"
        reply.mkdir(parents=True)
        (reply / "index-exact.json").write_text(
            json.dumps({"reply": {"codemodel-v2": {"jsonFile": "codemodel.json"}}}),
            encoding="utf-8")
        (reply / "codemodel.json").write_text(
            json.dumps({"configurations": [{"targets": [{"jsonFile": "target.json"}]}]}),
            encoding="utf-8")
        (reply / "target.json").write_text(
            json.dumps({"name": "pulp-test-direct", "artifacts": [{"path": "direct"}]}),
            encoding="utf-8")
        (build / "CTestTestfile.cmake").write_text(
            f'add_test([=[direct-test]=] "{build / "direct"}")\n', encoding="utf-8")

    def test_a_partly_built_tree_passes_only_with_proof_each_gap_is_unbuilt(self) -> None:
        # A bounded leg builds only its selected targets; the other
        # registrations have no command yet, and the selftest ran in that leg.
        policy = {"build_flags": [], "build_type": "debug", "baseline_tests": ["smoke"],
                  "families": [{"name": "f", "tests": ["direct-test"]}]}
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            self.partly_built_tree(build)
            ready = fixture("smoke", str(build / "bin" / "tests"),
                            properties=[{"name": "WORKING_DIRECTORY", "value": str(build)}])
            unbuilt = {"backtrace": 1, "name": "direct-test",
                       "properties": [{"name": "WORKING_DIRECTORY", "value": str(build)}]}
            self.assertEqual(
                check_ctest_inventory({"tests": [ready, unbuilt]}, REPO_ROOT, build, policy), 1)
            # Built but listed without a command: no longer merely unbuilt.
            (build / "direct").write_text("", encoding="utf-8")
            with self.assertRaisesRegex(inventory.InventoryError, "direct-test"):
                check_ctest_inventory({"tests": [ready, unbuilt]}, REPO_ROOT, build, policy)
            (build / "direct").unlink()
            # Commandless with no CMake provenance at all.
            orphan = {**unbuilt, "name": "orphan-test"}
            with self.assertRaisesRegex(inventory.InventoryError, "orphan-test"):
                check_ctest_inventory({"tests": [ready, unbuilt, orphan]}, REPO_ROOT, build, policy)
            # A literal name nothing registers still refuses when no Catch2
            # discovery is pending.
            with self.assertRaisesRegex(inventory.InventoryError, "absent from CTest inventory"):
                check_ctest_inventory({"tests": [ready]}, REPO_ROOT, build, policy)

    def test_a_pending_catch2_discovery_defers_only_the_literal_name_check(self) -> None:
        # While a Catch2 executable is unbuilt its cases are one placeholder, so a
        # literal name that may be one of them cannot be checked yet; the same
        # selftest checks it after the full build. Proof is still required.
        policy = {"build_flags": [], "build_type": "debug", "baseline_tests": ["smoke"],
                  "families": [{"name": "f", "tests": ["a-discovered-case"]}]}
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            name = "unit_tests_NOT_BUILT-abc1234"
            cases = build / "unit_tests-abc1234_tests.cmake"
            (build / "unit_tests-abc1234_include.cmake").write_text(
                f'if(EXISTS "{cases}")\n  include("{cases}")\nelse()\n'
                f"  add_test({name} {name})\nendif()\n", encoding="utf-8")
            ready = fixture("smoke", str(build / "bin" / "tests"),
                            properties=[{"name": "WORKING_DIRECTORY", "value": str(build)}])
            placeholder = {"name": name,
                           "properties": [{"name": "WORKING_DIRECTORY", "value": str(build)}]}
            self.assertEqual(
                check_ctest_inventory({"tests": [ready, placeholder]}, REPO_ROOT, build, policy), 1)
            # Once discovery has run, the name must be a real registration.
            with self.assertRaisesRegex(inventory.InventoryError, "a-discovered-case"):
                check_ctest_inventory({"tests": [ready]}, REPO_ROOT, build, policy)
            # A placeholder without its generated include proves nothing.
            (build / "unit_tests-abc1234_include.cmake").unlink()
            with self.assertRaisesRegex(inventory.InventoryError, "unambiguous command"):
                check_ctest_inventory({"tests": [ready, placeholder]}, REPO_ROOT, build, policy)

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

    def test_duplicate_composite_identity_is_refused(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "ambiguous"):
            inventory.require_unambiguous({"duplicate_composite_group_count": 1})
        inventory.require_unambiguous({"duplicate_composite_group_count": 0})

    def test_literal_selection_rejects_missing_or_repeated_requests(self) -> None:
        groups = inventory.inventory_groups(
            [fixture("one")], self.source_root, self.build_dir
        )
        manifest = {"groups": groups, "duplicate_composite_group_count": 0}
        with self.assertRaisesRegex(inventory.InventoryError, "absent"):
            inventory.expand_literal_selection(manifest, ["missing"])
        with self.assertRaisesRegex(inventory.InventoryError, "duplicate requested"):
            inventory.expand_literal_selection(manifest, ["one", "one"])


def check_ctest_inventory(
    payload: dict, source_root: Path, build_dir: Path, policy: dict
) -> int:
    """The tree's registrations must be complete and name every literal test
    the policy declares; returns how many are proven unbuilt.

    A bounded leg builds only its selected targets, so other registrations
    have no command yet. One passes only with the runner's own proof that it
    is merely unbuilt (a Catch2 NOT_BUILT placeholder with its generated
    include, or a direct test whose program is a CMake artifact not built
    yet); a built registration without a command, or one with no proof,
    refuses. Literal names that may be undiscovered Catch2 cases are checked
    once no placeholder remains, which the same test does after the full
    build."""
    ready, unbuilt = inventory.split_proven_unbuilt_placeholders(payload["tests"], build_dir)
    manifest = inventory.build_manifest(
        ready,
        source_root,
        Path(os.path.abspath(build_dir)),
        policy,
    )
    inventory.require_unambiguous(manifest)
    present = {group["composite"]["name"] for group in manifest["groups"]}
    present |= {test["name"] for test in unbuilt
                if not inventory.NOT_BUILT_PLACEHOLDER.match(test["name"])}
    undiscovered = any(inventory.NOT_BUILT_PLACEHOLDER.match(test["name"]) for test in unbuilt)
    missing = sorted(literal_tests(policy) - present)
    if missing and not undiscovered:
        raise inventory.InventoryError(
            f"policy names tests absent from CTest inventory: {missing}"
        )
    if not unbuilt:
        inventory.expand_literal_selection(manifest, literal_tests(policy))
    return len(unbuilt)


def validate_ctest_inventory(build_dir: Path, manifest_output: Path | None = None) -> None:
    """Check a configured tree's registrations (see check_ctest_inventory).
    Whether they match the protected base is decided per plan, against that
    base (run_changed_surface_tests.base_projection)."""
    policy = load_policy()
    source_root = inventory.source_root_for_build(build_dir)
    payload = inventory.load_ctest_payload(build_dir)
    unbuilt = check_ctest_inventory(payload, source_root, build_dir, policy)
    if unbuilt:
        print(f"changed-surface-policy: {unbuilt} registration(s) proven unbuilt in a "
              "partly built tree; literal-name checks complete after the full build")
    if manifest_output is not None:
        projection = inventory.project_registrations(payload, source_root, build_dir)
        manifest_output.write_bytes(inventory.canonical_json(projection) + b"\n")


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
