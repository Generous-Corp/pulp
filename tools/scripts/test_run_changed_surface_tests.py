#!/usr/bin/env python3
"""Hostile unit tests for the authoritative changed-surface CTest adapter."""

from __future__ import annotations

import base64
import copy
import hashlib
import json
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import changed_surface_inventory as inventory
import run_changed_surface_tests as runner


def fixture(name: str, executable: str = "/repo/build/bin/tests") -> dict:
    return {
        "name": name,
        "command": [executable, name],
        "properties": [{"name": "WORKING_DIRECTORY", "value": "/repo/build"}],
    }


def policy() -> dict:
    return {
        "schema_version": 2,
        "full_test_count": 3,
        "build_type": "debug",
        "build_flags": ["-DCMAKE_BUILD_TYPE=Debug"],
        "baseline_tests": ["smoke"],
        "families": [
            {
                "name": "core",
                "tests": ["core"],
                "extended_tests": ["neighbor"],
            }
        ],
    }


def manifest_contract(tests: list[dict], source_root: Path, build_dir: Path) -> dict:
    manifest = inventory.build_manifest(tests, source_root, build_dir, policy())
    return {
        key: manifest[key]
        for key in (
            "registration_count",
            "unique_name_count",
            "unique_composite_count",
            "duplicate_name_group_count",
            "duplicate_name_excess_count",
            "duplicate_composite_group_count",
            "target_contract_digest",
            "authoritative_filter_digest",
            "inventory_digest",
        )
    }


def selection_receipt(
    selected_tests: list[str] | None = None,
    selected_build_targets: list[str] | None = None,
) -> dict:
    names = selected_tests or ["smoke", "core"]
    literal = "".join(f"{name}\n" for name in names).encode("utf-8")
    receipt = {
        "schema_version": 1 if selected_build_targets is None else 2,
        "repository": "Generous-Corp/pulp",
        "pull_request": 42,
        "target": "mac",
        "base_sha": "a" * 40,
        "head_sha": "b" * 40,
        "tree_sha": "c" * 40,
        "policy_digest": "d" * 64,
        "selection_receipt_digest": "e" * 64,
        "validation_contract_digest": "f" * 64,
        "workflow_digest": "0" * 64,
        "selected_tests_digest": hashlib.sha256(literal).hexdigest(),
        "selected_tests": names,
    }
    if selected_build_targets is not None:
        target_payload = "".join(
            f"{target}\n" for target in selected_build_targets
        ).encode("utf-8")
        receipt.update(
            {
                "selected_build_targets_digest": hashlib.sha256(
                    target_payload
                ).hexdigest(),
                "selected_build_targets": selected_build_targets,
            }
        )
    return receipt


def encode_receipt(receipt: dict) -> tuple[str, str]:
    payload = json.dumps(receipt, separators=(",", ":")).encode("utf-8")
    return (
        base64.urlsafe_b64encode(payload).decode("ascii").rstrip("="),
        hashlib.sha256(payload).hexdigest(),
    )


class ChangedSurfaceExecutionTest(unittest.TestCase):
    def setUp(self) -> None:
        # Inventory provenance is exercised by changed_surface_inventory's own
        # tests. These fixtures deliberately use synthetic absolute paths, so
        # pin the two Git identity queries while testing selection expansion.
        git_value = mock.patch.object(inventory, "_git_value", return_value="a" * 40)
        git_value.start()
        self.addCleanup(git_value.stop)

    def test_literal_payload_requires_authenticated_identity_and_unique_lines(self) -> None:
        receipt = selection_receipt()
        encoded, digest = encode_receipt(receipt)
        names, literal, targets, target_literal, decoded = runner.decode_selection_receipt(
            encoded, digest
        )
        self.assertEqual(names, ["smoke", "core"])
        self.assertEqual(literal, b"smoke\ncore\n")
        self.assertEqual(targets, [])
        self.assertEqual(target_literal, b"")
        self.assertEqual(decoded, receipt)
        malformed = (["smoke", ""], ["smoke", "smoke"], ["bad\rname"])
        for candidate in malformed:
            with self.subTest(selected_tests=candidate):
                candidate_receipt = selection_receipt(candidate)
                candidate_encoded, candidate_digest = encode_receipt(candidate_receipt)
                with self.assertRaises(runner.SelectionExecutionError):
                    runner.decode_selection_receipt(candidate_encoded, candidate_digest)
        with self.assertRaisesRegex(runner.SelectionExecutionError, "digest mismatch"):
            runner.decode_selection_receipt(encoded, "0" * 64)
        with self.assertRaisesRegex(runner.SelectionExecutionError, "URL-safe"):
            runner.decode_selection_receipt("bad+payload", digest)
        oversized_receipt = selection_receipt(["x" * runner.MAX_SELECTED_TEST_BYTES])
        oversized_encoded, oversized_digest = encode_receipt(oversized_receipt)
        with self.assertRaisesRegex(runner.SelectionExecutionError, "safe execution limit"):
            runner.decode_selection_receipt(oversized_encoded, oversized_digest)

    def test_schema_v2_receipt_binds_canonical_build_targets(self) -> None:
        receipt = selection_receipt(selected_build_targets=["pulp-cli", "pulp_tests"])
        encoded, digest = encode_receipt(receipt)
        names, literal, targets, target_literal, decoded = runner.decode_selection_receipt(
            encoded, digest
        )
        self.assertEqual(names, ["smoke", "core"])
        self.assertEqual(literal, b"smoke\ncore\n")
        self.assertEqual(targets, ["pulp-cli", "pulp_tests"])
        self.assertEqual(target_literal, b"pulp-cli\npulp_tests\n")
        self.assertEqual(decoded, receipt)

        for invalid_targets in ([], ["--clean-first"], ["bad target"], ["pulp-cli", "pulp-cli"]):
            with self.subTest(targets=invalid_targets):
                candidate = selection_receipt(selected_build_targets=invalid_targets)
                candidate_encoded, candidate_digest = encode_receipt(candidate)
                with self.assertRaises(runner.SelectionExecutionError):
                    runner.decode_selection_receipt(candidate_encoded, candidate_digest)

        tampered = selection_receipt(selected_build_targets=["pulp-cli"])
        tampered["selected_build_targets_digest"] = "0" * 64
        tampered_encoded, tampered_digest = encode_receipt(tampered)
        with self.assertRaisesRegex(runner.SelectionExecutionError, "digest mismatch"):
            runner.decode_selection_receipt(tampered_encoded, tampered_digest)

    def test_receipt_identity_matches_clean_checkout(self) -> None:
        receipt = selection_receipt()
        values = iter(["b" * 40, "c" * 40, ""])
        with mock.patch.object(runner, "git_value", side_effect=lambda *_: next(values)):
            runner.validate_receipt_identity(receipt, "mac")
        stale = dict(receipt)
        stale["head_sha"] = "e" * 40
        values = iter(["b" * 40, "c" * 40])
        with mock.patch.object(runner, "git_value", side_effect=lambda *_: next(values)):
            with self.assertRaisesRegex(runner.SelectionExecutionError, "HEAD and tree"):
                runner.validate_receipt_identity(stale, "mac")

    def test_private_snapshot_is_owner_read_only_and_exact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            payload = b"smoke\ncore\n"
            snapshot = runner.write_private_selection(Path(directory), payload)
            self.assertEqual(snapshot.read_bytes(), payload)
            self.assertEqual(stat.S_IMODE(snapshot.stat().st_mode), 0o400)
            with self.assertRaises(FileExistsError):
                runner.write_private_selection(Path(directory), payload)

    def test_ctest_329_is_required_for_literal_file_selection(self) -> None:
        with mock.patch.object(
            runner.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                ["ctest", "--version"], 0, stdout="ctest version 3.28.6\n"
            ),
        ):
            with self.assertRaisesRegex(runner.SelectionExecutionError, "3.29"):
                runner.require_ctest_version()
        with mock.patch.object(
            runner.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                ["ctest", "--version"], 0, stdout="ctest version 3.29.0\n"
            ),
        ):
            self.assertEqual(runner.require_ctest_version(), (3, 29))

    def test_shadow_comparison_keeps_full_authoritative_and_classifies_coverage(self) -> None:
        self.assertEqual(runner.failure_coverage(0, 0), "no_failure_observed")
        self.assertEqual(runner.failure_coverage(1, 1), "failure_observed_by_selected")
        self.assertEqual(runner.failure_coverage(0, 1), "missed_full_failure")
        self.assertEqual(runner.failure_coverage(1, 0), "selected_only_failure")
        self.assertEqual(runner.failure_coverage(1, None), "not_compared")
        self.assertEqual(runner.comparison_verdict(0, 0), "matched_pass")
        self.assertEqual(
            runner.comparison_verdict(1, 1), "failure_overlap_unproven"
        )
        self.assertEqual(
            runner.comparison_verdict(0, 1), "mismatched_non_graduation"
        )
        self.assertEqual(
            runner.comparison_verdict(1, 0), "mismatched_non_graduation"
        )
        selected = runner.execution_argv(Path("/repo/build"), Path("/tmp/selected"))
        full = runner.execution_argv(Path("/repo/build"))
        self.assertIn("--tests-from-file", selected)
        self.assertNotIn("--tests-from-file", full)

    def test_shadow_build_timing_names_remainder_and_total_estimate(self) -> None:
        self.assertEqual(
            runner.full_build_timing_fields(1.25, 2.75),
            {
                "full_build_is_incremental_after_selected": True,
                "full_build_incremental_duration_seconds": 2.75,
                "full_build_estimated_total_duration_seconds": 4.0,
            },
        )
        self.assertEqual(
            runner.full_build_timing_fields(1.25, None),
            {
                "full_build_is_incremental_after_selected": None,
                "full_build_incremental_duration_seconds": None,
                "full_build_estimated_total_duration_seconds": None,
            },
        )
        self.assertEqual(
            runner.full_build_timing_fields(None, None),
            {
                "full_build_is_incremental_after_selected": None,
                "full_build_incremental_duration_seconds": None,
                "full_build_estimated_total_duration_seconds": None,
            },
        )
        with self.assertRaisesRegex(
            runner.SelectionExecutionError, "no preceding selected-build timing"
        ):
            runner.full_build_timing_fields(None, 2.75)

    def test_result_receipts_are_append_only_and_require_absolute_directory(self) -> None:
        with self.assertRaisesRegex(runner.SelectionExecutionError, "must be absolute"):
            runner.write_result_receipt(Path("relative"), {"schema_version": 1})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(runner.os, "fsync", wraps=runner.os.fsync) as fsync:
                first = runner.write_result_receipt(root, {"schema_version": 1})
                self.assertGreaterEqual(fsync.call_count, 2)
            second = runner.write_result_receipt(root, {"schema_version": 1})
            self.assertNotEqual(first, second)
            self.assertEqual(len(list(root.glob("result-*.json"))), 2)

    def test_live_cmake_configuration_must_match_every_policy_flag(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            build_policy = policy()
            build_policy["build_flags"] = [
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DPULP_BUILD_TESTS=ON",
            ]
            cache = build / "CMakeCache.txt"
            cache.write_text(
                "CMAKE_BUILD_TYPE:STRING=Debug\nPULP_BUILD_TESTS:BOOL=ON\n",
                encoding="utf-8",
            )
            runner.validate_build_configuration(build, build_policy)
            cache.write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\nPULP_BUILD_TESTS:BOOL=ON\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                runner.SelectionExecutionError, "CMAKE_BUILD_TYPE"
            ):
                runner.validate_build_configuration(build, build_policy)
            cache.write_text("CMAKE_BUILD_TYPE:STRING=Debug\n", encoding="utf-8")
            with self.assertRaisesRegex(
                runner.SelectionExecutionError, "PULP_BUILD_TESTS"
            ):
                runner.validate_build_configuration(build, build_policy)

    def test_exact_inventory_and_selection_expansion_pass(self) -> None:
        source = Path("/repo")
        build = Path("/repo/build")
        tests = [fixture("smoke"), fixture("core"), fixture("neighbor")]
        contract = manifest_contract(tests, source, build)
        runner.validate_selection(
            selected_names=["smoke", "core"],
            full_tests=tests,
            selected_tests=[tests[0], tests[1]],
            source_root=source,
            build_dir=build,
            policy=policy(),
            contract=contract,
            target="mac",
        )

    def test_shadow_defers_only_proven_unbuilt_inventory_until_full_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / "source"
            build = root / "build"
            source.mkdir()
            build.mkdir()
            name = "pulp-test-later_NOT_BUILT-b12d07c"
            tests_path = build / "pulp-test-later-b12d07c_tests.cmake"
            (build / "pulp-test-later-b12d07c_include.cmake").write_text(
                f'if(EXISTS "{tests_path}")\n'
                f'  include("{tests_path}")\n'
                "else()\n"
                f"  add_test({name} {name})\n"
                "endif()\n",
                encoding="utf-8",
            )
            smoke = fixture("smoke", str(build / "smoke"))
            core = fixture("core", str(build / "core"))
            neighbor = fixture("neighbor", str(build / "neighbor"))
            placeholder = {
                "name": name,
                "properties": [
                    {"name": "WORKING_DIRECTORY", "value": str(build)}
                ],
            }
            self.assertEqual(
                runner.validate_deferred_shadow_selection(
                    selected_names=["smoke", "core"],
                    full_tests=[smoke, core, neighbor, placeholder],
                    selected_tests=[smoke, core],
                    source_root=source,
                    build_dir=build,
                    policy=policy(),
                ),
                1,
            )
            with self.assertRaisesRegex(
                runner.SelectionExecutionError, "differs from the reviewed"
            ):
                runner.validate_deferred_shadow_selection(
                    selected_names=["smoke", "core"],
                    full_tests=[smoke, core, neighbor, placeholder],
                    selected_tests=[smoke],
                    source_root=source,
                    build_dir=build,
                    policy=policy(),
                )

    def test_fully_hydrated_selected_build_uses_exact_inventory_validation(self) -> None:
        selected = [fixture("smoke"), fixture("core")]
        with (
            mock.patch.object(
                runner.inventory,
                "split_proven_unbuilt_placeholders",
                return_value=(selected, []),
            ),
            mock.patch.object(runner, "validate_selection") as validate_exact,
            mock.patch.object(
                runner, "validate_deferred_shadow_selection"
            ) as validate_deferred,
            mock.patch.object(
                runner, "validate_build_target_projection"
            ) as validate_projection,
        ):
            runner.validate_after_selected_build(
                selected_names=["smoke", "core"],
                full_tests=selected,
                selected_tests=selected,
                source_root=Path("/repo"),
                build_dir=Path("/repo/build"),
                policy=policy(),
                contract={"inventory_digest": "exact"},
                target="mac",
                selected_build_targets=["pulp-test-build-check"],
            )
        validate_exact.assert_called_once()
        validate_deferred.assert_not_called()
        validate_projection.assert_called_once()

    def test_schema_v1_cold_inventory_remains_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            build.mkdir()
            config = root / "config.toml"
            contract = root / "contract.json"
            config.write_text("# mocked\n", encoding="utf-8")
            contract.write_text("{}\n", encoding="utf-8")
            args = mock.Mock(
                config=config,
                inventory_contract=contract,
                selection_receipt_b64="encoded",
                selection_receipt_sha256="0" * 64,
                target="mac",
            )
            receipt = selection_receipt()
            selected = [fixture("smoke"), fixture("core")]
            with (
                mock.patch.dict(
                    runner.os.environ,
                    {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1"},
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "decode_selection_receipt",
                    return_value=(
                        ["smoke", "core"],
                        b"smoke\ncore\n",
                        [],
                        b"",
                        receipt,
                    ),
                ),
                mock.patch.object(runner, "validate_receipt_identity"),
                mock.patch.object(runner, "require_ctest_version"),
                mock.patch.object(runner, "load_policy", return_value=policy()),
                mock.patch.object(
                    runner.inventory,
                    "source_root_for_build",
                    return_value=runner.REPO_ROOT,
                ),
                mock.patch.object(runner, "validate_build_configuration"),
                mock.patch.object(runner, "ctest_json", return_value=selected),
                mock.patch.object(
                    runner,
                    "validate_selection",
                    side_effect=inventory.InventoryError(
                        "has no unambiguous command"
                    ),
                ),
                mock.patch.object(
                    runner.inventory, "split_proven_unbuilt_placeholders"
                ) as split_placeholders,
                mock.patch.object(runner.subprocess, "run") as execute,
            ):
                with self.assertRaisesRegex(
                    inventory.InventoryError, "unambiguous command"
                ):
                    runner.run_locked(args, build)
            split_placeholders.assert_not_called()
            execute.assert_not_called()

    def test_cold_shadow_runs_selected_leg_before_authoritative_full_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            build.mkdir()
            config = root / "config.toml"
            contract = root / "contract.json"
            config.write_text("# mocked\n", encoding="utf-8")
            contract.write_text("{}\n", encoding="utf-8")
            args = mock.Mock(
                config=config,
                inventory_contract=contract,
                selection_receipt_b64="encoded",
                selection_receipt_sha256="0" * 64,
                target="mac",
            )
            selected_names = ["smoke", "core"]
            selected_payload = b"smoke\ncore\n"
            receipt = selection_receipt(
                selected_build_targets=["pulp-test-build-check"]
            )
            placeholder = {"name": "later"}
            selected_tests = [fixture("smoke"), fixture("core")]
            hydrated_tests = [*selected_tests, fixture("neighbor")]
            ctest_results = iter(
                [
                    [*selected_tests, placeholder],
                    selected_tests,
                    [*selected_tests, placeholder],
                    selected_tests,
                    hydrated_tests,
                    selected_tests,
                ]
            )
            inventory_splits = iter(
                [
                    (selected_tests, [placeholder]),
                    (selected_tests, [placeholder]),
                    (hydrated_tests, []),
                ]
            )
            commands: list[list[str]] = []

            def run_command(argv: list[str], **_: object) -> subprocess.CompletedProcess:
                commands.append(argv)
                return subprocess.CompletedProcess(argv, 0)

            with (
                mock.patch.dict(
                    runner.os.environ,
                    {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1"},
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "decode_selection_receipt",
                    return_value=(
                        selected_names,
                        selected_payload,
                        ["pulp-test-build-check"],
                        b"pulp-test-build-check\n",
                        receipt,
                    ),
                ),
                mock.patch.object(runner, "validate_receipt_identity"),
                mock.patch.object(runner, "require_ctest_version"),
                mock.patch.object(runner, "load_policy", return_value=policy()),
                mock.patch.object(
                    runner.inventory,
                    "source_root_for_build",
                    return_value=runner.REPO_ROOT,
                ),
                mock.patch.object(runner, "validate_build_configuration"),
                mock.patch.object(
                    runner,
                    "ctest_json",
                    side_effect=lambda *_: next(ctest_results),
                ),
                mock.patch.object(
                    runner,
                    "validate_selection",
                    side_effect=[inventory.InventoryError("has no unambiguous command"), None],
                ),
                mock.patch.object(
                    runner.inventory,
                    "split_proven_unbuilt_placeholders",
                    side_effect=lambda *_: next(inventory_splits),
                ),
                mock.patch.object(runner, "validate_build_target_projection"),
                mock.patch.object(
                    runner,
                    "validate_deferred_shadow_selection",
                    return_value=1,
                ),
                mock.patch.object(runner.subprocess, "run", side_effect=run_command),
                mock.patch.object(runner, "clear_build_sentinel", return_value=0),
            ):
                self.assertEqual(runner.run_locked(args, build), 0)

            self.assertEqual(
                [
                    "selected-build",
                    "selected-test",
                    "full-build",
                    "full-test",
                ],
                [
                    (
                        "selected-build"
                        if "cmake" in command and "--target" in command
                        else "full-build"
                        if "cmake" in command
                        else "selected-test"
                        if "--tests-from-file" in command
                        else "full-test"
                    )
                    for command in commands
                ],
            )

    def test_missing_baseline_undeclared_name_and_expansion_mismatch_refuse(self) -> None:
        source = Path("/repo")
        build = Path("/repo/build")
        tests = [fixture("smoke"), fixture("core"), fixture("neighbor")]
        contract = manifest_contract(tests, source, build)
        cases = [
            (["core"], [tests[1]], "baseline"),
            (["smoke", "attacker .*"], [tests[0]], "undeclared"),
            (["smoke", "core"], [tests[0]], "differs"),
        ]
        for selected, observed, message in cases:
            with self.subTest(selected=selected):
                with self.assertRaisesRegex(runner.SelectionExecutionError, message):
                    runner.validate_selection(
                        selected_names=selected,
                        full_tests=tests,
                        selected_tests=observed,
                        source_root=source,
                        build_dir=build,
                        policy=policy(),
                        contract=contract,
                        target="mac",
                    )

    def test_inventory_drift_refuses_before_execution(self) -> None:
        source = Path("/repo")
        build = Path("/repo/build")
        tests = [fixture("smoke"), fixture("core"), fixture("neighbor")]
        contract = manifest_contract(tests, source, build)
        drifted = copy.deepcopy(tests)
        drifted[1]["command"][0] = "/repo/build/bin/changed"
        with self.assertRaisesRegex(inventory.InventoryError, "inventory contract drift"):
            runner.validate_selection(
                selected_names=["smoke", "core"],
                full_tests=drifted,
                selected_tests=[drifted[0], drifted[1]],
                source_root=source,
                build_dir=build,
                policy=policy(),
                contract=contract,
                target="mac",
            )

    def test_duplicate_display_name_expands_all_registrations(self) -> None:
        source = Path("/repo")
        build = Path("/repo/build")
        tests = [
            fixture("smoke"),
            fixture("core", "/repo/build/bin/one"),
            fixture("core", "/repo/build/bin/two"),
        ]
        duplicate_policy = policy()
        duplicate_policy["full_test_count"] = 3
        contract_manifest = inventory.build_manifest(
            tests, source, build, duplicate_policy
        )
        contract = {
            key: contract_manifest[key]
            for key in (
                "registration_count",
                "unique_name_count",
                "unique_composite_count",
                "duplicate_name_group_count",
                "duplicate_name_excess_count",
                "duplicate_composite_group_count",
                "target_contract_digest",
                "authoritative_filter_digest",
                "inventory_digest",
            )
        }
        runner.validate_selection(
            selected_names=["smoke", "core"],
            full_tests=tests,
            selected_tests=tests,
            source_root=source,
            build_dir=build,
            policy=duplicate_policy,
            contract=contract,
            target="mac",
        )
        with self.assertRaisesRegex(runner.SelectionExecutionError, "differs"):
            runner.validate_selection(
                selected_names=["smoke", "core"],
                full_tests=tests,
                selected_tests=tests[:2],
                source_root=source,
                build_dir=build,
                policy=duplicate_policy,
                contract=contract,
                target="mac",
            )

    def test_execution_argv_keeps_the_file_as_one_argument(self) -> None:
        selected = Path("/tmp/a path/$(touch nope);.*.txt")
        argv = runner.execution_argv(Path("/repo/build"), selected)
        file_option = argv.index("--tests-from-file")
        self.assertEqual(argv[file_option + 1], str(selected))
        self.assertIn("--no-tests=error", argv)
        self.assertNotIn("-R", argv)
        self.assertNotIn("--tests-regex", argv)

    def test_cmake_codemodel_proves_each_selected_test_producer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
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
                            {
                                "targets": [
                                    {"jsonFile": "target-cli.json"},
                                    {"jsonFile": "target-tests.json"},
                                ]
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (reply / "target-cli.json").write_text(
                json.dumps(
                    {"name": "pulp-cli", "artifacts": [{"path": "bin/pulp"}]}
                ),
                encoding="utf-8",
            )
            (reply / "target-tests.json").write_text(
                json.dumps(
                    {
                        "name": "pulp-test-build-check",
                        "artifacts": [{"path": "bin/pulp-tests"}],
                    }
                ),
                encoding="utf-8",
            )
            selected_tests = [fixture("smoke", str(build / "bin" / "pulp-tests"))]
            runner.validate_build_target_projection(
                build_dir=build,
                selected_tests=selected_tests,
                selected_build_targets=["pulp-test-build-check"],
            )
            with self.assertRaisesRegex(
                runner.SelectionExecutionError, "undeclared target"
            ):
                runner.validate_build_target_projection(
                    build_dir=build,
                    selected_tests=selected_tests,
                    selected_build_targets=["pulp-cli"],
                )
            with self.assertRaisesRegex(
                runner.SelectionExecutionError, "absent from the codemodel"
            ):
                runner.validate_build_target_projection(
                    build_dir=build,
                    selected_tests=selected_tests,
                    selected_build_targets=["does-not-exist"],
                )

    def test_build_directory_lock_covers_verification_and_execution(self) -> None:
        args = mock.Mock()
        args.build_dir = Path("/repo/build")
        events: list[str] = []

        class Lock:
            def __enter__(self) -> None:
                events.append("lock-enter")

            def __exit__(self, *_: object) -> None:
                events.append("lock-exit")

        with (
            mock.patch.object(Path, "resolve", return_value=Path("/repo/build")),
            mock.patch.object(
                runner.build_dir_lock,
                "exclusive_build_dir",
                side_effect=lambda _build_dir: Lock(),
            ),
            mock.patch.object(
                runner,
                "run_locked",
                side_effect=lambda _args, _build_dir: events.append("run") or 0,
            ),
        ):
            self.assertEqual(runner.run(args), 0)
        self.assertEqual(events, ["lock-enter", "run", "lock-exit"])


if __name__ == "__main__":
    unittest.main()
