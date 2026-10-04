#!/usr/bin/env python3
"""Hostile unit tests for the authoritative changed-surface CTest adapter."""

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
import tempfile
import tomllib
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
        # Every add_test registration carries a backtrace; discovered cases do not.
        "backtrace": 0,
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


def base_of(tests: list[dict], source_root: Path, build_dir: Path) -> dict:
    """The base projection a tree with exactly these registrations would record."""
    return inventory.project_registrations({"tests": tests}, source_root, build_dir,
                                           shape="configure")


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
        base = base_of(tests, source, build)
        runner.validate_selection(
            selected_names=["smoke", "core"],
            full_payload={"tests": tests},
            selected_tests=[tests[0], tests[1]],
            source_root=source,
            build_dir=build,
            policy=policy(),
            base=base,
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
                full_payload={"tests": selected},
                selected_tests=selected,
                source_root=Path("/repo"),
                build_dir=Path("/repo/build"),
                policy=policy(),
                base={},
                target="mac",
                selected_build_targets=["pulp-test-build-check"],
            )
        validate_exact.assert_called_once()
        # Only the selected targets are built here, so unbuilt programs are fine.
        self.assertIs(validate_exact.call_args.kwargs["require_built"], False)
        validate_deferred.assert_not_called()
        validate_projection.assert_called_once()

    def test_schema_v1_cold_inventory_remains_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            build.mkdir()
            config = root / "config.toml"
            config.write_text("# mocked\n", encoding="utf-8")
            args = mock.Mock(
                config=config,
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
                mock.patch.object(runner, "ctest_payload", return_value={"tests": selected}),
                mock.patch.object(runner, "base_projection", return_value={}),
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
            config.write_text("# mocked\n", encoding="utf-8")
            args = mock.Mock(
                config=config,
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
                    "ctest_payload",
                    side_effect=lambda *_: {"tests": next(ctest_results)},
                ),
                mock.patch.object(runner, "base_projection", return_value={}),
                mock.patch.object(
                    runner,
                    "validate_selection",
                    side_effect=[inventory.InventoryError("has no unambiguous command"), None],
                ) as validate_calls,
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
            # Before any build the comparison tolerates unbuilt programs; after
            # the full build it requires every registration to have one.
            prebuild, post_full = validate_calls.call_args_list
            self.assertIs(prebuild.kwargs["require_built"], False)
            self.assertNotIn("require_built", post_full.kwargs)

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
        base = base_of(tests, source, build)
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
                        full_payload={"tests": tests},
                        selected_tests=observed,
                        source_root=source,
                        build_dir=build,
                        policy=policy(),
                        base=base,
                        target="mac",
                    )

    def test_inventory_drift_refuses_before_execution(self) -> None:
        source = Path("/repo")
        build = Path("/repo/build")
        tests = [fixture("smoke"), fixture("core"), fixture("neighbor")]
        base = base_of(tests, source, build)
        drifted = copy.deepcopy(tests)
        # A build-tree program and its arguments are not compared: ctest lists
        # an unbuilt one as a bare empty command, so a configure-only base
        # carries neither. Its name and properties are compared.
        drifted[1]["properties"] = [*drifted[1]["properties"], {"name": "TIMEOUT", "value": 9}]
        with self.assertRaisesRegex(runner.SelectionExecutionError, "differ from the protected base"):
            runner.validate_selection(
                selected_names=["smoke", "core"],
                full_payload={"tests": drifted},
                selected_tests=[drifted[0], drifted[1]],
                source_root=source,
                build_dir=build,
                policy=policy(),
                base=base,
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
        base = base_of(tests, source, build)
        runner.validate_selection(
            selected_names=["smoke", "core"],
            full_payload={"tests": tests},
            selected_tests=tests,
            source_root=source,
            build_dir=build,
            policy=duplicate_policy,
            base=base,
            target="mac",
        )
        with self.assertRaisesRegex(runner.SelectionExecutionError, "differs"):
            runner.validate_selection(
                selected_names=["smoke", "core"],
                full_payload={"tests": tests},
                selected_tests=tests[:2],
                source_root=source,
                build_dir=build,
                policy=duplicate_policy,
                base=base,
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

    def test_shadow_refusal_runs_bare_full_authority_and_records_non_graduation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            results = root / "results"
            build.mkdir()
            receipt = selection_receipt(
                selected_build_targets=["pulp-test-build-check"]
            )
            encoded, digest = encode_receipt(receipt)
            args = mock.Mock(
                build_dir=build,
                selection_receipt_b64=encoded,
                selection_receipt_sha256=digest,
                _changed_surface_full_authority_started=False,
                _changed_surface_fallback_safe=True,
                _changed_surface_receipt_identity_verified=True,
            )
            commands: list[list[str]] = []

            def execute(argv: list[str], **_: object) -> subprocess.CompletedProcess:
                commands.append(argv)
                return subprocess.CompletedProcess(argv, 0)

            with (
                mock.patch.dict(
                    runner.os.environ,
                    {
                        "SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1",
                        "SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(results),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError(
                        "selected native CTest executable has no CMake producer"
                    ),
                ),
                mock.patch.object(runner.subprocess, "run", side_effect=execute),
                mock.patch.object(runner, "clear_build_sentinel", return_value=0),
            ):
                self.assertEqual(runner.run(args), 0)

            self.assertEqual(len(commands), 2)
            self.assertIn("cmake", commands[0])
            self.assertNotIn("--target", commands[0])
            self.assertNotIn("--tests-from-file", commands[1])
            result_files = list(results.glob("result-*.json"))
            self.assertEqual(len(result_files), 1)
            fallback = json.loads(result_files[0].read_text(encoding="utf-8"))
            self.assertEqual(fallback["schema_version"], 2)
            self.assertEqual(fallback["head_sha"], receipt["head_sha"])
            self.assertEqual(
                fallback["selected_execution_disposition"],
                "refused_fallback_full",
            )
            self.assertTrue(fallback["full_authoritative"])
            self.assertFalse(fallback["graduation_eligible"])
            self.assertEqual(fallback["full_returncode"], 0)
            self.assertIsNone(fallback["selected_registration_count"])
            self.assertIsNone(fallback["full_registration_count"])
            self.assertIsNone(fallback["selected_returncode"])

    def test_authoritative_mode_refusal_never_uses_shadow_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            args = mock.Mock(
                build_dir=build,
                _changed_surface_full_authority_started=False,
            )
            with (
                mock.patch.dict(
                    runner.os.environ,
                    {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "0"},
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError("ambiguous producer"),
                ),
                mock.patch.object(runner, "run_full_fallback") as fallback,
            ):
                with self.assertRaisesRegex(
                    runner.SelectionExecutionError, "ambiguous producer"
                ):
                    runner.run(args)
            fallback.assert_not_called()

    def test_shadow_fallback_requires_immutable_receipt_destination(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            receipt = selection_receipt(
                selected_build_targets=["pulp-test-build-check"]
            )
            encoded, digest = encode_receipt(receipt)
            args = mock.Mock(
                build_dir=build,
                selection_receipt_b64=encoded,
                selection_receipt_sha256=digest,
                _changed_surface_full_authority_started=False,
                _changed_surface_fallback_safe=True,
                _changed_surface_receipt_identity_verified=True,
            )
            with (
                mock.patch.dict(
                    runner.os.environ,
                    {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1"},
                    clear=True,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError(
                        "selected native CTest executable has no CMake producer"
                    ),
                ),
                mock.patch.object(runner.subprocess, "run") as execute,
            ):
                with self.assertRaisesRegex(
                    runner.FullAuthorityExecutionError,
                    "requires an immutable result receipt directory",
                ):
                    runner.run(args)
            execute.assert_not_called()

    def test_successful_fallback_fails_closed_when_receipt_write_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            result_file = root / "not-a-directory"
            build.mkdir()
            result_file.write_text("occupied\n", encoding="utf-8")
            receipt = selection_receipt()
            encoded, digest = encode_receipt(receipt)
            args = mock.Mock(
                build_dir=build,
                selection_receipt_b64=encoded,
                selection_receipt_sha256=digest,
                _changed_surface_full_authority_started=False,
                _changed_surface_fallback_safe=True,
                _changed_surface_receipt_identity_verified=True,
            )
            with (
                mock.patch.dict(
                    runner.os.environ,
                    {
                        "SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1",
                        "SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(result_file),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError(
                        "selected CTest registration differs from request"
                    ),
                ),
                mock.patch.object(
                    runner.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ),
            ):
                with self.assertRaises(runner.FullAuthorityExecutionError):
                    runner.run(args)

    def test_unverified_provenance_cannot_enter_full_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            build.mkdir()
            receipt = selection_receipt(
                selected_build_targets=["pulp-test-build-check"]
            )
            encoded, digest = encode_receipt(receipt)
            args = mock.Mock(
                build_dir=build,
                selection_receipt_b64=encoded,
                selection_receipt_sha256=digest,
                _changed_surface_full_authority_started=False,
                _changed_surface_fallback_safe=False,
                _changed_surface_receipt_identity_verified=False,
            )
            with (
                mock.patch.dict(
                    runner.os.environ,
                    {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1"},
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError(
                        "selection receipt does not match checkout HEAD and tree"
                    ),
                ),
                mock.patch.object(runner, "run_full_fallback") as fallback,
            ):
                with self.assertRaisesRegex(
                    runner.SelectionExecutionError, "HEAD and tree"
                ):
                    runner.run(args)
            fallback.assert_not_called()

    def test_fallback_full_build_failure_is_authoritative_and_skips_ctest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            results = root / "results"
            build.mkdir()
            receipt = selection_receipt(
                selected_build_targets=["pulp-test-build-check"]
            )
            encoded, digest = encode_receipt(receipt)
            args = mock.Mock(
                build_dir=build,
                selection_receipt_b64=encoded,
                selection_receipt_sha256=digest,
                _changed_surface_full_authority_started=False,
                _changed_surface_fallback_safe=True,
                _changed_surface_receipt_identity_verified=True,
            )
            commands: list[list[str]] = []

            def fail_build(argv: list[str], **_: object) -> subprocess.CompletedProcess:
                commands.append(argv)
                return subprocess.CompletedProcess(argv, 17)

            with (
                mock.patch.dict(
                    runner.os.environ,
                    {
                        "SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1",
                        "SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(results),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError(
                        "selected native CTest executable has no CMake producer"
                    ),
                ),
                mock.patch.object(runner.subprocess, "run", side_effect=fail_build),
                mock.patch.object(runner, "clear_build_sentinel") as clear_sentinel,
            ):
                self.assertEqual(runner.run(args), 17)

            self.assertEqual(len(commands), 1)
            self.assertIn("cmake", commands[0])
            clear_sentinel.assert_not_called()
            fallback = json.loads(
                next(results.glob("result-*.json")).read_text(encoding="utf-8")
            )
            self.assertEqual(fallback["full_build_returncode"], 17)
            self.assertEqual(fallback["full_returncode"], 17)
            self.assertTrue(fallback["full_authoritative"])

    def test_schema_v1_fallback_preserves_full_ctest_without_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            results = root / "results"
            build.mkdir()
            receipt = selection_receipt()
            encoded, digest = encode_receipt(receipt)
            args = mock.Mock(
                build_dir=build,
                selection_receipt_b64=encoded,
                selection_receipt_sha256=digest,
                _changed_surface_full_authority_started=False,
                _changed_surface_fallback_safe=True,
                _changed_surface_receipt_identity_verified=True,
            )
            commands: list[list[str]] = []

            def execute(argv: list[str], **_: object) -> subprocess.CompletedProcess:
                commands.append(argv)
                return subprocess.CompletedProcess(argv, 0)

            with (
                mock.patch.dict(
                    runner.os.environ,
                    {
                        "SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1",
                        "SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(results),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    runner,
                    "run_locked",
                    side_effect=runner.SelectionExecutionError(
                        "selected CTest registration differs from request"
                    ),
                ),
                mock.patch.object(runner.subprocess, "run", side_effect=execute),
                mock.patch.object(runner, "clear_build_sentinel") as clear_sentinel,
            ):
                self.assertEqual(runner.run(args), 0)

            self.assertEqual(len(commands), 1)
            self.assertNotIn("cmake", commands[0])
            self.assertNotIn("--tests-from-file", commands[0])
            clear_sentinel.assert_not_called()
            fallback = json.loads(
                next(results.glob("result-*.json")).read_text(encoding="utf-8")
            )
            self.assertIsNone(fallback["full_build_returncode"])
            self.assertIsNone(fallback["full_build_is_incremental_after_selected"])
            self.assertEqual(fallback["full_returncode"], 0)

    def test_failure_after_full_authority_started_never_reenters_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            args = mock.Mock(
                build_dir=build,
                _changed_surface_full_authority_started=False,
            )

            def fail_after_full_started(
                namespace: argparse.Namespace, _build_dir: Path
            ) -> int:
                namespace._changed_surface_full_authority_started = True
                raise runner.SelectionExecutionError(
                    "full build left provenance-backed CTest placeholders unresolved"
                )

            with (
                mock.patch.dict(
                    runner.os.environ,
                    {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1"},
                    clear=False,
                ),
                mock.patch.object(
                    runner, "run_locked", side_effect=fail_after_full_started
                ),
                mock.patch.object(runner, "run_full_fallback") as fallback,
            ):
                with self.assertRaisesRegex(
                    runner.FullAuthorityExecutionError, "placeholders unresolved"
                ):
                    runner.run(args)
            fallback.assert_not_called()

    def test_real_subprocess_target_closure_fallback_canary(self) -> None:
        fixture = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "changed_surface_fallback_canary.py"
        )
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(fixture), directory],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
                shell=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads(result.stdout)
        self.assertEqual(summary["commands"], ["cmake", "sentinel", "ctest"])
        self.assertEqual(summary["disposition"], "refused_fallback_full")
        self.assertFalse(summary["graduation_eligible"])


class BaseInventoryTest(unittest.TestCase):
    """A bounded run compares this tree's registrations with the base's own."""

    BASE = "a" * 40

    def test_unbuilt_base_matches_built_tree_and_any_untouched_difference_refuses(self) -> None:
        source, build = Path("/repo"), Path("/repo/build")
        built = [fixture("smoke"), fixture("core"), fixture("neighbor")]
        unbuilt = copy.deepcopy(built)
        for test in unbuilt:
            test["command"] = [""]  # a configure-only tree lists no program yet
        base = base_of(unbuilt, source, build)
        self.assertFalse(base["recordable"])
        runner.validate_registrations_match_base({"tests": built}, source, build, base)
        for label, tree in [
            ("deleted", built[:2]),
            ("added", [*built, fixture("extra")]),
            ("renamed", [built[0], built[1], fixture("neighbour")]),
            ("relabelled", [*built[:2], {**built[2], "properties": [
                *built[2]["properties"], {"name": "LABELS", "value": ["slow"]}]}]),
        ]:
            with self.subTest(label=label):
                with self.assertRaisesRegex(runner.SelectionExecutionError, "protected base"):
                    runner.validate_registrations_match_base({"tests": tree}, source, build, base)

    def run_base(self, build: Path, results: list[int], payload: dict | None = None,
                 merge_base: str = BASE, base_cache: str | None = None,
                 envs: list | None = None, base_links: dict | None = None,
                 base_policy: dict | None = None):
        """Run base_projection with a fake runner. The fake base configure
        writes `base_cache` as the scratch tree's CMakeCache.txt; by default it
        copies the head's provisioning switches, as a correctly provisioned
        base would."""
        calls: list[list[str]] = []
        if base_cache is None:
            head = runner._cache_entries(build)
            base_cache = "".join(f"{k}:INTERNAL={v}\n" for k, v in runner.provisioning(head).items())

        def fake(argv, **kwargs):
            if "merge-base" in argv:
                return subprocess.CompletedProcess(argv, 0, merge_base + "\n", "")
            calls.append(argv)
            if envs is not None:
                envs.append(kwargs.get("env"))
            code = results.pop(0) if results else 0
            if code == 0 and "cmake" in argv and "-B" in argv:
                tree_build = Path(argv[argv.index("-B") + 1])
                tree_build.mkdir(parents=True, exist_ok=True)
                (tree_build / "CMakeCache.txt").write_text(base_cache)
                for name, real in (base_links or {}).items():
                    entry = tree_build.parent / "external" / name
                    entry.parent.mkdir(parents=True, exist_ok=True)
                    entry.mkdir() if real else entry.symlink_to(tree_build)
            return subprocess.CompletedProcess(argv, code, "", "boom")

        with mock.patch.object(runner, "ctest_payload",
                               return_value=payload or {"tests": [fixture("smoke")]}):
            return calls, runner.base_projection(self.BASE, base_policy or policy(), build,
                                                 Path("/repo"), fake)

    def test_base_is_configured_from_the_exact_commit_and_cached(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_GENERATOR:INTERNAL=Ninja\nPython3_EXECUTABLE:FILEPATH=/usr/bin/python3\n")
            envs: list = []
            calls, first = self.run_base(build, [0, 0, 0, 0], envs=envs)
            self.assertEqual(calls[0][:6], ["git", "-C", "/repo", "worktree", "add", "--detach"])
            self.assertEqual(calls[0][-1], self.BASE)
            # The base is provisioned like the head, offline, before configuring.
            self.assertTrue(calls[1][1].endswith("/setup.sh"), calls[1])
            self.assertEqual(calls[1][2:], ["--deps-only", "--non-interactive"])
            self.assertTrue(calls[1][1].startswith(calls[0][-2]), calls[1])
            self.assertEqual(envs[1]["GIT_ALLOW_PROTOCOL"], "file")
            self.assertIn("-DCMAKE_BUILD_TYPE=Debug", calls[2])
            self.assertIn("Ninja", calls[2])
            self.assertTrue(calls[2][1].endswith("tools/ci/governed-build.sh"), calls[2])
            self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", calls[2])
            self.assertEqual(first["configure"]["flags"], ["-DCMAKE_BUILD_TYPE=Debug"])
            self.assertEqual(calls[-1][3:5], ["worktree", "remove"])
            self.assertEqual(first["base_sha"], self.BASE)
            again, second = self.run_base(build, [])
            self.assertEqual(again, [])
            self.assertEqual(second["configure_seconds"], 0.0)
            self.assertEqual(second["digest"], first["digest"])

    HEAD_WITH_SDKS = ("CMAKE_GENERATOR:INTERNAL=Ninja\n"
                      "PULP_HAS_AUSDK:INTERNAL=TRUE\nPULP_HAS_VST3:INTERNAL=TRUE\n"
                      "PULP_HAS_SKIA:INTERNAL=TRUE\n")

    def test_a_base_provisioned_without_the_heads_sdks_refuses_by_name(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text(self.HEAD_WITH_SDKS)
            without_ausdk = ("CMAKE_GENERATOR:INTERNAL=Ninja\nPULP_HAS_AUSDK:INTERNAL=FALSE\n"
                             "PULP_HAS_VST3:INTERNAL=TRUE\nPULP_HAS_SKIA:INTERNAL=TRUE\n")
            with self.assertRaisesRegex(
                    runner.SelectionExecutionError,
                    r"base_provisioning_mismatch: PULP_HAS_AUSDK base=FALSE head=TRUE$"):
                self.run_base(build, [0, 0, 0, 0], base_cache=without_ausdk)
            # The cached base is refused again, not served as if it matched.
            with self.assertRaisesRegex(runner.SelectionExecutionError, "base_provisioning_mismatch"):
                self.run_base(build, [])
            # A switch the base never set is a mismatch too.
            (build / "CMakeCache.txt").write_text(self.HEAD_WITH_SDKS + "PULP_HAS_LV2:INTERNAL=TRUE\n")
            with self.assertRaisesRegex(runner.SelectionExecutionError,
                                        r"PULP_HAS_LV2 base=<unset> head=TRUE"):
                self.run_base(build, [0, 0, 0, 0], base_cache=self.HEAD_WITH_SDKS)

    def test_a_tree_records_only_the_links_setup_made(self) -> None:
        # A scratch base whose setup.sh linked nothing (an offline cache miss
        # fails earlier) has no external/ yet; that is no links, not an error.
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            self.assertEqual(runner.linked_externals(tree), [])
            (tree / "external" / "tracked").mkdir(parents=True)
            self.assertEqual(runner.linked_externals(tree), [])
            (tree / "external" / "vst3sdk").symlink_to(tree / "external" / "tracked")
            self.assertEqual(runner.linked_externals(tree), ["vst3sdk"])

    def test_a_base_provisioned_like_the_head_is_projected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text(self.HEAD_WITH_SDKS)
            _, base = self.run_base(build, [0, 0, 0, 0],
                                    base_links={"AudioUnitSDK": False, "vst3sdk": False, "miniz": True})
            self.assertEqual(base["provisioning"], {"CMAKE_GENERATOR": "Ninja", "PULP_HAS_AUSDK": "TRUE",
                                                    "PULP_HAS_SKIA": "TRUE", "PULP_HAS_VST3": "TRUE"})
            # Only the links setup.sh made are recorded, never a tracked directory.
            self.assertEqual(base["linked_externals"], ["AudioUnitSDK", "vst3sdk"])

    def test_a_dependency_pin_difference_refuses_even_with_the_sdks_linked(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            contract = "PULP_CHECKOUT_DEPENDENCY_CONTRACT:INTERNAL=pulp-shared-source-v1;ausdk={}\n"
            (build / "CMakeCache.txt").write_text(self.HEAD_WITH_SDKS + contract.format("AudioUnitSDK-1.4.0"))
            with self.assertRaisesRegex(
                    runner.SelectionExecutionError,
                    r"base_provisioning_mismatch: PULP_CHECKOUT_DEPENDENCY_CONTRACT "
                    r"base=pulp-shared-source-v1;ausdk=AudioUnitSDK-1.3.0 "
                    r"head=pulp-shared-source-v1;ausdk=AudioUnitSDK-1.4.0$"):
                self.run_base(build, [0, 0, 0, 0],
                              base_cache=self.HEAD_WITH_SDKS + contract.format("AudioUnitSDK-1.3.0"))

    def test_a_cached_base_records_no_provisioning_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(runner.SelectionExecutionError,
                                        "base provisioning not recorded"):
                runner.validate_provisioning({"rows": []}, Path(directory))

    def test_a_changed_head_provisioning_is_a_different_base_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            cache = build / "CMakeCache.txt"
            cache.write_text(self.HEAD_WITH_SDKS)
            self.run_base(build, [0, 0, 0, 0])
            cache.write_text(self.HEAD_WITH_SDKS.replace("AUSDK:INTERNAL=TRUE", "AUSDK:INTERNAL=FALSE"))
            calls, _ = self.run_base(build, [0, 0, 0, 0])
            self.assertTrue(calls, "a head with different SDKs reused the cached base inventory")

    def test_a_cached_base_inventory_is_reused_only_for_the_same_configure(self) -> None:
        # The cache key must cover every input of the base configure: a
        # different flag set, generator or Python is a different inventory.
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            cache = build / "CMakeCache.txt"
            cache.write_text("CMAKE_GENERATOR:INTERNAL=Ninja\nPython3_EXECUTABLE:FILEPATH=/usr/bin/python3\n")
            first_calls, _ = self.run_base(build, [0, 0, 0])
            self.assertTrue(first_calls)
            reused, _ = self.run_base(build, [])
            self.assertEqual(reused, [])
            for label, change in (
                ("generator", lambda: cache.write_text(
                    "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\nPython3_EXECUTABLE:FILEPATH=/usr/bin/python3\n")),
                ("python", lambda: cache.write_text(
                    "CMAKE_GENERATOR:INTERNAL=Ninja\nPython3_EXECUTABLE:FILEPATH=/opt/python3\n")),
            ):
                with self.subTest(changed=label):
                    change()
                    calls, _ = self.run_base(build, [0, 0, 0])
                    self.assertTrue(calls, f"a changed {label} reused the cached base inventory")
            cache.write_text("CMAKE_GENERATOR:INTERNAL=Ninja\nPython3_EXECUTABLE:FILEPATH=/usr/bin/python3\n")
            flagged = {**policy(), "build_flags": ["-DCMAKE_BUILD_TYPE=Release"]}
            calls, _ = self.run_base(build, [0, 0, 0, 0], base_policy=flagged)
            self.assertTrue(calls, "a changed flag set reused the cached base inventory")

    def test_unavailable_base_says_so_and_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            for results in ([1], [0, 1]):
                with self.subTest(results=results):
                    calls = []
                    with self.assertRaisesRegex(runner.SelectionExecutionError,
                                                "inventory: base not recorded"):
                        calls, _ = self.run_base(build, list(results))
            leftovers = [p for p in (build / runner.BASE_INVENTORY_CACHE).iterdir()]
            self.assertEqual(leftovers, [])
            with self.assertRaisesRegex(runner.SelectionExecutionError, "base not recorded"):
                runner.base_projection("not-a-sha", policy(), build)

    def test_the_base_comparison_runs_before_any_other_inventory_check(self) -> None:
        # The placeholder-tolerant path is reachable only after the base
        # comparison passed; a drifted tree must fail on the base, first.
        source, build = Path("/repo"), Path("/repo/build")
        tests = [fixture("smoke"), fixture("core")]
        base = base_of(tests, source, build)
        drifted = [*tests, fixture("extra")]
        with mock.patch.object(runner.inventory, "build_manifest",
                               side_effect=AssertionError("manifest built before the base check")):
            with self.assertRaisesRegex(runner.SelectionExecutionError, "protected base"):
                runner.validate_selection(
                    selected_names=["smoke", "core"], full_payload={"tests": drifted},
                    selected_tests=tests, source_root=source, build_dir=build,
                    policy=policy(), base=base, target="mac")

    def test_a_universal_build_refuses_before_any_configure(self) -> None:
        for cache, flags in [
            ("CMAKE_OSX_ARCHITECTURES:STRING=arm64;x86_64\n", ["-DCMAKE_BUILD_TYPE=Debug"]),
            ("", ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"]),
        ]:
            with self.subTest(cache=cache, flags=flags), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                (build / "CMakeCache.txt").write_text(cache)
                universal = {**policy(), "build_flags": flags}
                calls: list = []
                with self.assertRaisesRegex(runner.SelectionExecutionError, "universal build"):
                    runner.base_projection(self.BASE, universal, build, Path("/repo"),
                                           lambda argv, **_: calls.append(argv))
                self.assertEqual(calls, [])
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text("CMAKE_OSX_ARCHITECTURES:STRING=arm64\n")
            calls, _ = self.run_base(build, [0, 0, 0])
            self.assertTrue(calls)

    def test_the_only_raw_download_in_cmake_is_the_universal_webgpu_slice(self) -> None:
        # Every other dependency fetch goes through FetchContent, which the
        # base configure disconnects. A new file(DOWNLOAD) needs its own guard.
        listed = subprocess.run(["git", "-C", str(runner.REPO_ROOT), "grep", "-l", "file(DOWNLOAD",
                                 "--", "*.cmake", "*CMakeLists.txt"],
                                capture_output=True, text=True)
        self.assertEqual(
            listed.stdout.split(), ["tools/cmake/PulpWgpuUniversal.cmake"],
            "a new raw file(DOWNLOAD) can reach the network during a bounded run's "
            "disconnected base configure: fetch it through FetchContent instead, or gate "
            "it like PulpWgpuUniversal.cmake and make base_projection refuse that "
            "configuration, then list it here",
        )
        guard = (runner.REPO_ROOT / "tools/cmake/PulpWgpuUniversal.cmake").read_text()
        self.assertIn('if(NOT (_want_arm64 AND _want_x86_64))', guard)

    def test_a_base_that_is_not_the_checkouts_merge_base_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(runner.SelectionExecutionError, "inventory: base mismatch"):
                self.run_base(Path(directory), [], merge_base="b" * 40)

    def test_a_cached_base_inventory_never_answers_for_another_merge_base(self) -> None:
        # The cache is keyed by base SHA, not by the checkout. A checkout whose
        # merge base moved must refuse before the cache can serve the old base.
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            self.run_base(build, [0, 0, 0])
            self.assertTrue(list((build / runner.BASE_INVENTORY_CACHE).glob(f"{self.BASE}-*.json")))
            with self.assertRaisesRegex(runner.SelectionExecutionError, "inventory: base mismatch"):
                self.run_base(build, [], merge_base="b" * 40)

    def test_a_cold_tree_matches_its_base_until_the_full_build(self) -> None:
        # A lane checkout with nothing built yet lists unbuilt targets without a
        # program, exactly as the configure-only base does. That must compare
        # equal before the build, or every cold run falls back to the full suite.
        source, build = Path("/repo"), Path("/repo/build")
        cold = [fixture("smoke"), fixture("core"), fixture("neighbor")]
        for test in cold:
            test["command"][0] = ""
        base = base_of(copy.deepcopy(cold), source, build)
        runner.validate_registrations_match_base({"tests": cold}, source, build, base,
                                                 require_built=False)
        with self.assertRaisesRegex(runner.SelectionExecutionError, "no command after the build"):
            runner.validate_registrations_match_base({"tests": cold}, source, build, base)

    @staticmethod
    def cold_and_built_trees() -> tuple[list[dict], list[dict]]:
        """One add_test registration and one Catch2 executable, before and after
        the build: unbuilt, ctest lists a bare empty command and the discovery
        placeholder; built, the program's path and each discovered case, which
        carries no backtrace."""
        cold = [fixture("smoke"), {**fixture("unit_tests_NOT_BUILT-abc1234"), "command": []}]
        cold[0]["command"] = [""]
        cold[1].pop("backtrace")
        built = [fixture("smoke")]
        for case in ("adds", "clamps", "rounds"):
            discovered = fixture(case, "/repo/build/bin/unit_tests")
            discovered.pop("backtrace")
            built.append(discovered)
        return cold, built

    def test_an_unknown_projection_shape_refuses_rather_than_listing(self) -> None:
        # A misspelt shape must not fall back to the listing shape: a built
        # tree compared in listing shape refuses every bounded run after its
        # full build, which is the failure the configure shape exists to end.
        cold, _ = self.cold_and_built_trees()
        with self.assertRaisesRegex(ValueError, "unknown projection shape 'confgure'"):
            inventory.project_registrations({"tests": cold}, Path("/repo"), Path("/repo/build"),
                                            shape="confgure")

    def test_a_placeholder_and_its_discovered_cases_fold_to_one_row(self) -> None:
        source, build = Path("/repo"), Path("/repo/build")
        cold, built = self.cold_and_built_trees()
        shaped = [inventory.project_registrations({"tests": tree}, source, build, shape="configure")
                  for tree in (cold, built)]
        self.assertEqual(inventory.projection_differences(*shaped), [])
        self.assertIn({"row": {"executable": "unit_tests", "kind": "discovered"}, "count": 1},
                      shaped[1]["rows"])
        # The listing shape keeps every case, which is why it cannot compare them.
        listed = inventory.project_registrations({"tests": built}, source, build)
        self.assertEqual(listed["row_count"], 4)

    def test_a_built_tree_matches_its_own_prebuild_snapshot(self) -> None:
        source, build = Path("/repo"), Path("/repo/build")
        cold, built = self.cold_and_built_trees()
        snapshot = inventory.project_registrations({"tests": cold}, source, build, shape="configure")
        runner.validate_registrations_match_base(
            {"tests": built}, source, build, snapshot, reference_label=runner.PREBUILD_SNAPSHOT)

    def test_a_registration_added_during_the_build_refuses_by_name(self) -> None:
        # A CMake re-run mid-build (a glob or generated input changed) can
        # register a test the pre-build check never saw.
        source, build = Path("/repo"), Path("/repo/build")
        cold, built = self.cold_and_built_trees()
        snapshot = inventory.project_registrations({"tests": cold}, source, build, shape="configure")
        with self.assertRaisesRegex(
                runner.SelectionExecutionError,
                r"differ from this tree before the build; .*only in this tree: .*\"name\":\"late\""):
            runner.validate_registrations_match_base(
                {"tests": [*built, fixture("late")]}, source, build, snapshot,
                reference_label=runner.PREBUILD_SNAPSHOT)

    def test_a_built_tree_still_missing_a_command_refuses_against_its_snapshot(self) -> None:
        source, build = Path("/repo"), Path("/repo/build")
        cold, built = self.cold_and_built_trees()
        snapshot = inventory.project_registrations({"tests": cold}, source, build, shape="configure")
        built[0]["command"] = [""]
        with self.assertRaisesRegex(runner.SelectionExecutionError, "no command after the build; require full suite: smoke$"):
            runner.validate_registrations_match_base(
                {"tests": built}, source, build, snapshot, reference_label=runner.PREBUILD_SNAPSHOT)

    def test_a_registration_left_without_a_command_after_the_build_refuses(self) -> None:
        source, build = Path("/repo"), Path("/repo/build")
        tests = [fixture("smoke"), fixture("core")]
        base = base_of(tests, source, build)
        hollow = copy.deepcopy(tests)
        hollow[1]["command"] = [""]
        with self.assertRaisesRegex(runner.SelectionExecutionError, "no command after the build"):
            runner.validate_registrations_match_base({"tests": hollow}, source, build, base)
        listed = inventory.project_registrations({"tests": hollow}, source, build)
        self.assertEqual(inventory.name_only_rows(listed), 1)

    def test_unavailable_base_falls_back_to_the_full_suite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            build.mkdir()
            (root / "config.toml").write_text("# mocked\n", encoding="utf-8")
            args = mock.Mock(config=root / "config.toml", selection_receipt_b64="e",
                             selection_receipt_sha256="0" * 64, target="mac")
            with (
                mock.patch.object(runner, "decode_selection_receipt", return_value=(
                    ["smoke"], b"smoke\n", [], b"", selection_receipt())),
                mock.patch.object(runner, "validate_receipt_identity"),
                mock.patch.object(runner, "require_ctest_version"),
                mock.patch.object(runner, "load_policy", return_value=policy()),
                mock.patch.object(runner.inventory, "source_root_for_build",
                                  return_value=runner.REPO_ROOT),
                mock.patch.object(runner, "validate_build_configuration"),
                mock.patch.object(runner, "base_projection", side_effect=runner.SelectionExecutionError(
                    "inventory: base not recorded: cmake failed")),
                mock.patch.object(runner.subprocess, "run") as execute,
            ):
                with self.assertRaisesRegex(runner.SelectionExecutionError, "base not recorded"):
                    runner.run_locked(args, build)
            # Raised after the fallback boundary: the caller runs the full suite.
            self.assertTrue(args._changed_surface_fallback_safe)
            execute.assert_not_called()



class FailureSetTest(unittest.TestCase):
    FIXTURE = Path(__file__).resolve().parent / "fixtures/changed_surface/ctest-junit.xml"

    def test_ctest_junit_failures_exclude_passes_and_skips(self) -> None:
        # A report ctest --output-junit wrote for one pass, two fails (one
        # named with spaces) and one SKIP_RETURN_CODE skip.
        self.assertEqual(runner.junit_failures(self.FIXTURE), {"fails", "has spaces fails"})
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(runner.junit_failures(Path(directory) / "absent.xml"))
            broken = Path(directory) / "broken.xml"
            broken.write_text("<testsuite>", encoding="utf-8")
            self.assertIsNone(runner.junit_failures(broken))

    def test_two_failing_legs_without_sets_stay_unproven(self) -> None:
        self.assertEqual(runner.comparison_verdict(8, 8), "failure_overlap_unproven")
        self.assertEqual(runner.comparison_verdict(8, 8, ({"a"}, set(), {"a"})),
                         "failure_overlap_unproven")

    def test_the_allowlist_is_read_from_the_base_commit_not_the_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            git = lambda *a: subprocess.run(["git", "-C", str(repo), *a], check=True,
                                            capture_output=True, text=True).stdout.strip()
            git("init", "-q")
            git("config", "user.email", "t@example.invalid")
            git("config", "user.name", "t")
            target = repo / runner.LANE_RED_ALLOWLIST
            target.parent.mkdir(parents=True)
            target.write_text(json.dumps({"tests": [
                {"name": "b", "expires": "2026-10-17"}, {"name": "a", "expires": "2026-10-17"},
                {"name": "fixed", "expires": "2026-10-01"}]}), encoding="utf-8")
            git("add", "-A")
            git("commit", "-q", "-m", "base")
            base = git("rev-parse", "HEAD")
            # The working tree, which a PR controls, says something else.
            target.write_text(json.dumps({"tests": [{"name": "new-regression", "expires": "2099-01-01"}]}),
                              encoding="utf-8")
            names, digest = runner.lane_red_allowlist(base, repo, today="2026-10-03")
            # An expired entry is dropped: a fixed red cannot hide a new failure.
            self.assertEqual(names, {"a": "2026-10-17", "b": "2026-10-17"})
            self.assertEqual(digest, hashlib.sha256(
                subprocess.run(["git", "-C", str(repo), "show", f"{base}:{runner.LANE_RED_ALLOWLIST}"],
                               capture_output=True, check=True).stdout).hexdigest())
            self.assertIsNone(runner.lane_red_allowlist("0" * 40, repo))

    def test_the_checked_in_allowlist_parses_and_is_a_policy_path(self) -> None:
        doc = json.loads((runner.REPO_ROOT / runner.LANE_RED_ALLOWLIST).read_text(encoding="utf-8"))
        names = [entry["name"] for entry in doc["tests"]]
        self.assertTrue(names and all(entry.get("reason") and entry.get("owner_issue")
                                      and re.fullmatch(r"\d{4}-\d{2}-\d{2}", entry.get("expires", ""))
                                      for entry in doc["tests"]))
        self.assertEqual(len(names), len(set(names)))
        config = (runner.REPO_ROOT / ".shipyard/config.toml").read_text(encoding="utf-8")
        policy_paths = tomllib.loads(config)["targets"]["mac"]["changed_surface_selection"]["policy_paths"]
        self.assertIn(runner.LANE_RED_ALLOWLIST, policy_paths)


class ExecutableReuseTest(unittest.TestCase):
    """The shadow derivation of the executable-keyed selection: the bound
    inputs, the stale-reply reconfigure, base-sourced code under -I, and the
    copied inputs and digests beside the result receipt."""

    RECORD_SHA = "9" * 64

    def binding(self, build: Path, code: Path) -> dict:
        return {"base_record_run_id": "123", "base_record_sha256": self.RECORD_SHA,
                "base_record_path": "/host/store/record", "base_sha": "a" * 40,
                "derivation_code_dir": str(code), "derivation_code_sha256": "8" * 64,
                "sample_seed": "seed", "sample_percent": 5, "build_dir": str(build)}

    def tree(self, root: Path) -> tuple[Path, Path]:
        build, code = root / "build", root / "code"
        (build / ".cmake/api/v1/reply").mkdir(parents=True)
        (build / ".cmake/api/v1/reply/stale.json").write_text("{}", encoding="utf-8")
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
        for rel in runner.DERIVATION_SCRIPTS.values():
            (code / rel).parent.mkdir(parents=True, exist_ok=True)
            (code / rel).write_text("", encoding="utf-8")
        return build, code

    def fake(self, build: Path, record_sha: str | None = None):
        calls: list[tuple[list[str], dict]] = []
        record_sha = record_sha or self.RECORD_SHA

        def run(argv, **kwargs):
            calls.append((list(argv), kwargs))
            out = ""
            if argv[1:3] == ["cmake", str(build)]:
                # The stale reply must already be gone when CMake runs.
                self.assertFalse((build / ".cmake/api/v1/reply/stale.json").exists())
                (build / ".cmake/api/v1/reply").mkdir(parents=True)
            elif argv[0] == "ctest":
                out = json.dumps({"tests": [{"name": "t"}]})
            elif argv[2] == "-c":
                out = json.dumps({"arch": "arm64"})
            else:
                target = Path(argv[argv.index("--out") + 1])
                if target.name == "executable-keys.json":
                    target.write_text(json.dumps({"producer": {"base_record_sha256": record_sha},
                                                  "reasons": {"keyed": 1}}), encoding="utf-8")
                elif target.name == "selection.json":
                    target.write_text(json.dumps({"would_skip": ["a", "b"], "sampled_executables": ["a"]}),
                                      encoding="utf-8")
                else:
                    target.write_text("{}", encoding="utf-8")
            return subprocess.CompletedProcess(argv, 0, stdout=out, stderr="")
        return run, calls

    def test_binding_shape_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            good = self.binding(Path(directory), Path(directory))
            runner.validate_executable_reuse_binding(good)
            receipt = {**selection_receipt(), "executable_reuse": good}
            self.assertEqual(runner.decode_selection_receipt(*encode_receipt(receipt))[4], receipt)
            for bad in ({**good, "extra": "x"}, {**good, "sample_percent": 0}, {**good, "sample_percent": 101},
                        {**good, "sample_percent": 5.0}, {**good, "sample_percent": True},
                        {**good, "base_record_sha256": "X" * 64}, {**good, "base_sha": "a"},
                        {**good, "sample_seed": ""}, {k: v for k, v in good.items() if k != "build_dir"}):
                with self.subTest(bad=bad), self.assertRaises(runner.SelectionExecutionError):
                    runner.validate_executable_reuse_binding(bad)
            with self.assertRaisesRegex(runner.SelectionExecutionError, "unexpected schema"):
                runner.decode_selection_receipt(*encode_receipt({**selection_receipt(), "other": 1}))

    def test_derives_from_base_code_after_a_fresh_reconfigure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build, code = self.tree(Path(directory))
            result = Path(directory) / "result"
            run, calls = self.fake(build)
            derived = runner.derive_executable_reuse(self.binding(build, code), "b" * 40, build, result, run)
            self.assertEqual(derived["status"], "derived")
            reconfigure = calls[0][0]
            self.assertEqual(reconfigure[1:], ["cmake", str(build)])  # no arguments to change the cache
            self.assertTrue(reconfigure[0].endswith("tools/ci/governed-build.sh"))
            self.assertTrue((build / ".cmake/api/v1/query/codemodel-v2").is_file())
            python = [c for c in calls if c[0][0] == runner.sys.executable]
            self.assertEqual(len(python), 4)  # toolchain, codemodel, keys, selection
            for argv, kwargs in python:
                self.assertEqual(argv[1], "-I")
                self.assertEqual(kwargs["cwd"], str(code))
            keys = next(a for a, _ in python if a[2] == str(code / "tools/ci/executable_keys.py"))
            self.assertEqual(keys[keys.index("--build-dir") + 1], str(build))
            self.assertEqual(keys[keys.index("--base-record") + 1], "/host/store/record")
            self.assertEqual(keys[keys.index("--toolchain-json") + 1], str(result / "toolchain.json"))
            selection = next(a for a, _ in python if a[2] == str(code / "tools/ci/executable_selection.py"))
            self.assertEqual(selection[selection.index("--seed") + 1], "seed")
            self.assertEqual(selection[selection.index("--percent") + 1], "5")
            for name, field in (("ctest-listing.json", "ctest_listing_sha256"),
                                ("toolchain.json", "toolchain_sha256"),
                                ("codemodel-digest.json", "codemodel_digest_sha256"),
                                ("executable-keys.json", "key_manifest_sha256"),
                                ("selection.json", "selection_sha256")):
                self.assertEqual(derived[field], hashlib.sha256((result / name).read_bytes()).hexdigest())
            self.assertEqual(derived["cmake_cache_sha256"],
                             hashlib.sha256((build / "CMakeCache.txt").read_bytes()).hexdigest())
            self.assertEqual((derived["would_skip_count"], derived["sampled_count"]), (2, 1))

    def test_the_toolchain_probe_runs_the_base_copy_for_the_bound_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build, code = self.tree(Path(directory))
            result = Path(directory) / "result"
            probes = []
            for body in ("def probe_toolchain(build_dir):\n    return {'build': str(build_dir)}\n",
                         "def probe_toolchain():\n    return {'build': None}\n"):
                (code / "tools/ci/executable_keys.py").write_text(body, encoding="utf-8")
                run, calls = self.fake(build)

                def real_probe(argv, **kwargs):
                    if argv[2:3] == ["-c"]:
                        return subprocess.run(argv, **kwargs)
                    return run(argv, **kwargs)
                derived = runner.derive_executable_reuse(
                    self.binding(build, code), "b" * 40, build, result, real_probe)
                self.assertEqual(derived["status"], "derived", derived)
                probes.append(json.loads((result / "toolchain.json").read_text(encoding="utf-8")))
            self.assertEqual(probes, [{"build": str(build)}, {"build": None}])

    def test_a_failed_derivation_is_recorded_never_raised(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build, code = self.tree(Path(directory))
            result = Path(directory) / "result"
            run, _ = self.fake(build, record_sha="7" * 64)
            derived = runner.derive_executable_reuse(self.binding(build, code), "b" * 40, build, result, run)
            self.assertEqual(derived, {"status": "error: the base record is not the one Shipyard bound"})
            run, calls = self.fake(build)
            other = {**self.binding(build, code), "build_dir": str(Path(directory) / "elsewhere")}
            self.assertIn("build directory", runner.derive_executable_reuse(
                other, "b" * 40, build, result, run)["status"])
            self.assertEqual(calls, [])
            (code / "tools/ci/executable_selection.py").unlink()
            self.assertIn("executable_selection.py", runner.derive_executable_reuse(
                self.binding(build, code), "b" * 40, build, result, run)["status"])

            def failing(argv, **kwargs):
                return subprocess.CompletedProcess(argv, 1, stdout="", stderr="")
            (code / "tools/ci/executable_selection.py").write_text("", encoding="utf-8")
            self.assertEqual(runner.derive_executable_reuse(
                self.binding(build, code), "b" * 40, build, result, failing)["status"],
                "error: reconfigure exited 1")


class KeyedFullTest(unittest.TestCase):
    """A full plan with a bound executable reuse: the configured stages run
    unchanged and the derived selection is measured against their result."""

    BINDING = {"base_record_run_id": "1", "base_record_sha256": "9" * 64, "base_record_path": "/r",
               "base_sha": "a" * 40, "derivation_code_dir": "/code", "derivation_code_sha256": "8" * 64,
               "sample_seed": "s", "sample_percent": 5, "build_dir": "/b"}

    def receipt(self) -> dict:
        receipt = {k: v for k, v in selection_receipt(["x"], ["t"]).items() if not k.startswith("selected_")}
        return {**receipt, "disposition": "full", "executable_reuse": dict(self.BINDING)}

    def test_a_full_receipt_carries_no_selection_and_requires_the_binding(self) -> None:
        receipt = self.receipt()
        names, literal, targets, _, decoded = runner.decode_selection_receipt(*encode_receipt(receipt))
        self.assertEqual((names, literal, targets, decoded), ([], b"", [], receipt))
        bounded = selection_receipt(["x"], ["t"])
        for bad in ({**receipt, "selected_tests": ["x"]}, {**receipt, "disposition": "bounded"},
                    {k: v for k, v in receipt.items() if k != "executable_reuse"},
                    {**receipt, "schema_version": 1}, {**bounded, "disposition": "full"},
                    {**receipt, "executable_reuse": {**self.BINDING, "sample_percent": 0}}):
            with self.subTest(bad=bad), self.assertRaises(runner.SelectionExecutionError):
                runner.decode_selection_receipt(*encode_receipt(bad))

    def test_keyed_full_execs_the_configured_stages(self) -> None:
        config = tomllib.loads((runner.REPO_ROOT / ".shipyard/config.toml").read_text(encoding="utf-8"))
        stages = config["validation"]["default"]
        build, clear = (shlex.split(part) for part in stages["build"].split("&&"))
        env = {k: v for k, v in runner.STAGE_ENV.items()}
        self.assertEqual(build[0], "PULP_BUILD_CLASS=background")
        self.assertEqual(env, {"PULP_BUILD_CLASS": "background"})
        relative = lambda argv: [os.path.relpath(a, runner.REPO_ROOT) if a.startswith(  # noqa: E731
            str(runner.REPO_ROOT)) else a for a in argv]
        self.assertEqual(build[1:], relative(runner.build_argv(Path("build"))))
        self.assertEqual(clear, ["tools/ci/build-dir-sentinel.sh", "clear", "build"])
        test = shlex.split(stages["test"])
        lock = ["PULP_BUILD_CLASS=background", "python3", "tools/ci/build_dir_lock.py", "--build-dir", "build", "--"]
        self.assertEqual(test[:len(lock)], lock)  # the runner already holds this lock
        self.assertEqual(test[len(lock):], relative(runner.stage_test_argv(Path("build"))))
        self.assertEqual(runner.stage_test_argv(Path("build"), Path("/j"))[-2:], ["--output-junit", "/j"])

    def run_full(self, derived_ok: bool = True, build_rc: int = 0, failing: tuple = ()) -> tuple:
        calls: list = []
        with tempfile.TemporaryDirectory() as directory:
            build, results = Path(directory) / "build", Path(directory) / "results"
            build.mkdir()

            def derive(binding, head, build_dir, result_dir):
                calls.append("derive")
                if not derived_ok:
                    return {"status": "error: base refused"}
                result_dir.mkdir(parents=True, exist_ok=True)
                (result_dir / "selection.json").write_text(json.dumps(
                    {"would_skip": ["test/a", "test/b"], "sampled_executables": ["test/b"]}), encoding="utf-8")
                (result_dir / "executable-keys.json").write_text(json.dumps({"executables": {
                    "test/a": {"registrations": ["a1", "a2"]}, "test/b": {"registrations": ["b1"]}}}),
                    encoding="utf-8")
                return {"status": "derived"}

            def execute(argv, **kwargs):
                calls.append((list(argv), kwargs.get("env", {}).get("PULP_BUILD_CLASS")))
                if "--build" in argv:
                    return subprocess.CompletedProcess(argv, build_rc)
                if "--output-junit" in argv:
                    cases = "".join(f'<testcase name="{n}" status="fail"><failure/></testcase>' for n in failing)
                    Path(argv[argv.index("--output-junit") + 1]).write_text(
                        f"<testsuite>{cases}<testcase name=\"a1\" status=\"run\"/></testsuite>", encoding="utf-8")
                    return subprocess.CompletedProcess(argv, 8 if failing else 0)
                return subprocess.CompletedProcess(argv, 0)

            args = mock.Mock(selection_receipt_sha256="0" * 64)
            with (mock.patch.dict(os.environ, {"SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(results)}),
                  mock.patch.object(runner, "derive_executable_reuse", side_effect=derive),
                  mock.patch.object(runner.subprocess, "run", side_effect=execute),
                  mock.patch.object(runner, "ctest_payload", return_value={"tests": [{"name": "a1"}, {"name": "b1"}]})):
                code = runner.run_keyed_full(args, build, self.receipt())
            receipt = json.loads(sorted(results.glob("result-*.json"))[-1].read_text())
            return code, calls, receipt

    def test_derivation_precedes_the_configured_build_and_full_test(self) -> None:
        code, calls, receipt = self.run_full(failing=("a2", "b1"))
        self.assertEqual(code, 8)  # ctest's verdict
        self.assertEqual(calls[0], "derive")
        argvs = [c[0] for c in calls[1:]]
        self.assertIn("--build", argvs[0])
        self.assertTrue(argvs[1][0].endswith("build-dir-sentinel.sh"))
        build_dir = Path(argvs[2][argvs[2].index("--test-dir") + 1])
        self.assertEqual(argvs[2][:-2], runner.stage_test_argv(build_dir))
        # As in the stage strings, the build class is set for the build and
        # the tests, not for the sentinel clear.
        self.assertEqual([c[1] for c in calls[1:]], ["background", None, "background"])
        self.assertEqual(receipt["selected_execution_disposition"], "keyed_full_shadow")
        self.assertEqual(receipt["comparison_verdict"], "keyed_full_shadow")
        self.assertIs(receipt["full_authoritative"], True)
        self.assertIs(receipt["graduation_eligible"], False)
        self.assertEqual(receipt["selected_tests"], ["a1", "b1"])
        reuse = receipt["executable_reuse"]
        self.assertEqual(reuse["mode"], "keyed_full_shadow")
        self.assertEqual(reuse["bound"], self.BINDING)
        # test/b was sampled, so only test/a's two tests would have skipped,
        # and a2 failing in the full run is a false skip; b1 ran in the sample.
        self.assertEqual((reuse["would_skip_tests"], reuse["false_skip_count"], reuse["false_skips"]),
                         (["a1", "a2"], 1, ["a2"]))
        self.assertEqual((receipt["selected_tests_digest"], receipt["selected_logical_count"],
                          receipt["selected_build_targets_digest"], receipt["selected_build_target_count"]),
                         ("", 0, None, 0))

    def test_a_failed_derivation_still_runs_the_full_suite(self) -> None:
        code, calls, receipt = self.run_full(derived_ok=False)
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 4)  # derive, build, sentinel, test
        self.assertEqual(receipt["executable_reuse"]["derived"], {"status": "error: base refused"})
        self.assertIsNone(receipt["executable_reuse"]["false_skip_count"])
        self.assertEqual(receipt["full_returncode"], 0)

    def test_a_failed_build_is_the_verdict_and_skips_the_tests(self) -> None:
        code, calls, receipt = self.run_full(build_rc=2)
        self.assertEqual(code, 2)
        self.assertEqual(len(calls), 2)  # derive, build
        self.assertIsNone(receipt["full_returncode"])
        self.assertIsNone(receipt["executable_reuse"]["false_skip_count"])


class SelectedLegPipelineTest(unittest.TestCase):
    """Every gate of the selected leg, in the lane's order, across the three
    states a cold lane checkout passes through: configured only, selected
    targets built, everything built. Only process and ctest I/O is faked; every
    check runs as the lane runs it, so a gap between states fails here rather
    than in a proof run."""

    PLACEHOLDER = "unit_tests_NOT_BUILT-abc1234"

    def tree(self, build: Path) -> dict[str, list[dict]]:
        tool = str(build / "bin" / "tool")
        direct = [fixture("smoke", tool), fixture("core", tool), fixture("neighbor", tool)]
        for test in direct:
            test["properties"] = [{"name": "WORKING_DIRECTORY", "value": str(build)}]
        # Catch2's discovery placeholder, with the generated include file that
        # proves it, until the full build writes the real case list.
        cases = build / "unit_tests-abc1234_tests.cmake"
        (build / "unit_tests-abc1234_include.cmake").write_text(
            f'if(EXISTS "{cases}")\n  include("{cases}")\nelse()\n'
            f"  add_test({self.PLACEHOLDER} {self.PLACEHOLDER})\nendif()\n", encoding="utf-8")
        placeholder = {"name": self.PLACEHOLDER,
                       "properties": [{"name": "WORKING_DIRECTORY", "value": str(build)}]}
        # A validator this host resolved to a path that does not exist.
        validator = {"name": "pluginval-Example-VST3", "properties": [
            {"name": "LABELS", "value": ["validation", "vst3"]},
            {"name": "WORKING_DIRECTORY", "value": str(build)}]}
        discovered = []
        for case in ("adds", "clamps"):
            test = {"name": case, "command": [str(build / "bin" / "unit_tests"), case],
                    "properties": [{"name": "WORKING_DIRECTORY", "value": str(build)}]}
            discovered.append(test)
        return {"cold": [*direct, placeholder, validator],
                "built": [*direct, *discovered, validator]}

    def run_pipeline(self, late_registration: bool = False, failures: dict | None = None,
                     allowlist: tuple[list[str], str] | None = None,
                     reuse: dict | None = None,
                     derived_files: dict | None = None) -> tuple[int, list[str], dict]:
        """`failures` maps "selected tests" / "full tests" to the names that leg
        fails; each leg writes them as ctest's JUnit report and exits 8."""
        failures = failures or {}
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            states = self.tree(build)
            if late_registration:
                states["built"].append(fixture("late", str(build / "bin" / "tool")))
                states["built"][-1]["properties"] = [
                    {"name": "WORKING_DIRECTORY", "value": str(build)}]
            source = runner.REPO_ROOT
            base = inventory.project_registrations(
                {"tests": states["cold"]}, source, build, shape="configure")
            current = {"state": "cold"}
            steps: list[str] = []

            def listing(_build_dir, selected_file=None):
                tests = states[current["state"]]
                if selected_file is None:
                    return {"tests": tests}
                names = set(Path(selected_file).read_text().split())
                return [test for test in tests if test["name"] in names]

            real_run = subprocess.run

            def execute(argv, **kwargs):
                text = " ".join(map(str, argv))
                if argv[0] in ("git", "ctest") and "--test-dir" not in argv:
                    return real_run(argv, **kwargs)  # read-only tool queries
                if "build-dir-sentinel" in text:
                    return subprocess.CompletedProcess(argv, 0)
                if "--build" in argv:
                    step = "selected build" if "--target" in argv else "full build"
                    if step == "full build":
                        current["state"] = "built"
                else:
                    step = "selected tests" if "--tests-from-file" in text else "full tests"
                    failed = failures.get(step, [])
                    if "--output-junit" in argv:
                        cases = "".join(
                            f'<testcase name="{name}" status="fail"><failure message="Failed"/></testcase>'
                            for name in failed)
                        Path(argv[argv.index("--output-junit") + 1]).write_text(
                            f"<testsuite>{cases}</testsuite>", encoding="utf-8")
                    steps.append(step)
                    return subprocess.CompletedProcess(argv, 8 if failed else 0)
                steps.append(step)
                return subprocess.CompletedProcess(argv, 0)

            receipt = selection_receipt(["smoke", "core"], ["tool"])
            if reuse is not None:
                receipt["executable_reuse"] = reuse
            def derived(binding, head, build_dir, result_dir):
                result_dir.mkdir(parents=True, exist_ok=True)
                for name, doc in (derived_files or {}).items():
                    (result_dir / name).write_text(json.dumps(doc), encoding="utf-8")
                return {"status": "derived", "would_skip_count": 3}
            derive = mock.Mock(side_effect=derived)
            (build / "config.toml").write_text("# mocked\n", encoding="utf-8")
            args = mock.Mock(config=build / "config.toml", selection_receipt_b64="e",
                             selection_receipt_sha256="0" * 64, target="mac")
            results = build / "results"
            with (
                mock.patch.dict(os.environ, {"SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1",
                                             "SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(results)}),
                mock.patch.object(runner, "decode_selection_receipt", return_value=(
                    ["smoke", "core"], b"smoke\ncore\n", ["tool"], b"tool\n", receipt)),
                mock.patch.object(runner, "validate_receipt_identity"),
                mock.patch.object(runner, "require_ctest_version"),
                mock.patch.object(runner, "load_policy", return_value=policy()),
                mock.patch.object(runner.inventory, "source_root_for_build", return_value=source),
                mock.patch.object(runner, "validate_build_configuration"),
                mock.patch.object(runner, "base_projection", return_value=base),
                mock.patch.object(runner, "validate_build_target_projection"),
                mock.patch.object(runner, "ctest_payload", side_effect=lambda b: listing(b)),
                mock.patch.object(runner, "ctest_json", side_effect=listing),
                mock.patch.object(runner.subprocess, "run", side_effect=execute),
                mock.patch.object(runner, "lane_red_allowlist", return_value=allowlist),
                mock.patch.object(runner, "derive_executable_reuse", derive),
            ):
                code = runner.run_locked(args, build)
            self.derive_calls = derive.call_args_list
            receipts = sorted(results.glob("result-*.json")) if results.is_dir() else []
            receipt_doc = json.loads(receipts[-1].read_text()) if receipts else {}
            return code, steps, receipt_doc

    def test_a_cold_tree_runs_every_gate_and_executes_the_bounded_plan(self) -> None:
        code, steps, result = self.run_pipeline()
        self.assertEqual(code, 0)
        self.assertEqual(steps, ["selected build", "selected tests", "full build", "full tests"])
        self.assertEqual(result.get("comparison_verdict"), "matched_pass", result)
        self.assertIs(result.get("graduation_eligible"), True)
        self.assertEqual(result.get("selected_returncode"), 0)
        self.assertEqual(result.get("prebuild_unbuilt_placeholder_count"), 1)

    def test_a_bound_executable_reuse_is_derived_and_echoed_without_changing_the_run(self) -> None:
        bound = {"sample_seed": "s"}
        code, steps, result = self.run_pipeline(reuse=bound)
        self.assertEqual(code, 0)
        self.assertEqual(steps, ["selected build", "selected tests", "full build", "full tests"])
        self.assertEqual(result["comparison_verdict"], "matched_pass")
        self.assertEqual(result["executable_reuse"],
                         {"mode": "keyed_bounded_shadow", "bound": bound,
                          "derived": {"status": "derived", "would_skip_count": 3},
                          # No selection was written beside the receipt to measure against.
                          "would_skip_tests": None, "false_skip_count": None, "false_skips": None})
        self.assertEqual(len(self.derive_calls), 1)
        self.assertEqual(self.derive_calls[0].args[1], "b" * 40)  # the receipt's head
        _, _, plain = self.run_pipeline()
        self.assertNotIn("executable_reuse", plain)
        self.assertEqual(self.derive_calls, [])
        files = {"selection.json": {"would_skip": ["test/n"], "sampled_executables": []},
                 "executable-keys.json": {"executables": {"test/n": {"registrations": ["neighbor"]}}}}
        _, _, measured = self.run_pipeline(reuse=bound, derived_files=files,
                                           failures={"full tests": ["neighbor"]})
        self.assertEqual(measured["executable_reuse"]["false_skips"], ["neighbor"])

    LANE_REDS = ({"lane-red": "2026-10-17"}, "f" * 64)

    def test_matching_failure_sets_are_a_matched_fail_that_graduates_on_the_base_allowlist(self) -> None:
        code, _, result = self.run_pipeline(
            failures={"selected tests": ["core"], "full tests": ["core", "lane-red"]},
            allowlist=self.LANE_REDS)
        self.assertEqual(result["comparison_verdict"], "matched_fail", result)
        self.assertIs(result["graduation_eligible"], True)
        self.assertEqual(result["selected_tests"], ["smoke", "core"])
        self.assertEqual(result["selected_failures"], ["core"])
        self.assertEqual(result["full_failures"], ["core", "lane-red"])
        self.assertEqual(result["full_failures_outside_selection"], ["lane-red"])
        self.assertEqual(result["lane_red_allowlist"], ["lane-red"])
        self.assertEqual(result["lane_red_allowlist_expires"], {"lane-red": "2026-10-17"})
        self.assertEqual(result["allowlisted_failure_count"], 1)
        self.assertEqual(code, 8)

    def test_a_full_failure_outside_the_allowlist_never_graduates(self) -> None:
        _, _, result = self.run_pipeline(
            failures={"selected tests": ["core"], "full tests": ["core", "new-regression"]},
            allowlist=self.LANE_REDS)
        self.assertEqual(result["comparison_verdict"], "matched_fail")
        self.assertIs(result["graduation_eligible"], False)
        _, _, unread = self.run_pipeline(
            failures={"selected tests": ["core"], "full tests": ["core", "lane-red"]}, allowlist=None)
        self.assertIs(unread["graduation_eligible"], False)

    def test_mismatched_failure_sets_are_named(self) -> None:
        for failures, verdict in (
            ({"selected tests": ["core", "smoke"], "full tests": ["core"]}, "selected_only_failure"),
            ({"selected tests": ["core"], "full tests": ["core", "smoke"]}, "missed_full_failure"),
        ):
            with self.subTest(verdict=verdict):
                _, _, result = self.run_pipeline(failures=failures, allowlist=self.LANE_REDS)
                self.assertEqual(result["comparison_verdict"], verdict)
                self.assertIs(result["graduation_eligible"], False)

    def test_a_registration_added_by_the_build_refuses_against_the_prebuild_snapshot(self) -> None:
        with self.assertRaisesRegex(runner.SelectionExecutionError,
                                    "differ from this tree before the build; .*late"):
            self.run_pipeline(late_registration=True)

if __name__ == "__main__":
    unittest.main()
