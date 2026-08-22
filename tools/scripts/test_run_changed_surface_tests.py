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


def selection_receipt(selected_tests: list[str] | None = None) -> dict:
    names = selected_tests or ["smoke", "core"]
    literal = "".join(f"{name}\n" for name in names).encode("utf-8")
    return {
        "schema_version": 1,
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
        names, literal, decoded = runner.decode_selection_receipt(encoded, digest)
        self.assertEqual(names, ["smoke", "core"])
        self.assertEqual(literal, b"smoke\ncore\n")
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
        selected = runner.execution_argv(Path("/repo/build"), Path("/tmp/selected"))
        full = runner.execution_argv(Path("/repo/build"))
        self.assertIn("--tests-from-file", selected)
        self.assertNotIn("--tests-from-file", full)

    def test_result_receipts_are_append_only_and_require_absolute_directory(self) -> None:
        with self.assertRaisesRegex(runner.SelectionExecutionError, "must be absolute"):
            runner.write_result_receipt(Path("relative"), {"schema_version": 1})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = runner.write_result_receipt(root, {"schema_version": 1})
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
