#!/usr/bin/env python3
"""Focused regression tests for installed-SDK proof helpers."""
from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from test_agent_capability_installed_sdk import (
    ConsumerProof,
    combine_flag_values,
    configure_build_run_suite,
    configure_consumer,
    configure_consumer_suite,
    find_build_tree_artifact,
    positive_consumer_proofs,
    validate_positive_proof_coverage,
)


class InstrumentationFlagTests(unittest.TestCase):
    def test_instrumentation_is_appended_to_cached_flags(self) -> None:
        self.assertEqual(
            combine_flag_values("-Wall", "-fprofile-instr-generate"),
            "-Wall -fprofile-instr-generate",
        )

    def test_instrumentation_survives_empty_cached_flags(self) -> None:
        self.assertEqual(
            combine_flag_values("", "-fprofile-instr-generate"),
            "-fprofile-instr-generate",
        )


class BuildTreeArtifactTests(unittest.TestCase):
    def test_nested_installed_fixture_is_not_a_build_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            build_dir = pathlib.Path(temp)
            canonical = build_dir / "core/audio/libpulp-audio.a"
            nested_install = (
                build_dir / "control-sdk-consumer-smoke/prefix/lib/libpulp-audio.a"
            )
            canonical.parent.mkdir(parents=True)
            nested_install.parent.mkdir(parents=True)
            canonical.touch()
            nested_install.touch()

            self.assertEqual(
                find_build_tree_artifact(build_dir, canonical.name, "Release"),
                canonical.resolve(),
            )

    def test_duplicate_canonical_artifacts_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            build_dir = pathlib.Path(temp)
            for configuration in ("Debug", "Release"):
                archive = build_dir / f"core/audio/{configuration}/libpulp-audio.a"
                archive.parent.mkdir(parents=True)
                archive.touch()

            with self.assertRaisesRegex(RuntimeError, "expected one canonical"):
                find_build_tree_artifact(build_dir, "libpulp-audio.a", None)

            self.assertEqual(
                find_build_tree_artifact(build_dir, "libpulp-audio.a", "Release"),
                (build_dir / "core/audio/Release/libpulp-audio.a").resolve(),
            )


class ConfigureConsumerTests(unittest.TestCase):
    def command_for_configuration(self, configuration: str) -> list[str]:
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            prefix = root / "prefix"
            config = prefix / "lib/cmake/Pulp/PulpConfig.cmake"
            config.parent.mkdir(parents=True)
            config.touch()

            with mock.patch(
                "test_agent_capability_installed_sdk.run",
                return_value=subprocess.CompletedProcess([], 0, "", ""),
            ) as run_mock:
                configure_consumer(
                    "cmake",
                    prefix,
                    root / "consumer",
                    source="int main() { return 0; }\n",
                    target="Pulp::audio",
                    generator=None,
                    generator_options=[],
                    configuration=configuration,
                )
            return run_mock.call_args.args[0]

    def test_debug_sdk_is_explicitly_acknowledged(self) -> None:
        command = self.command_for_configuration("Debug")
        self.assertIn("-DPULP_ALLOW_DEBUG_SDK=ON", command)
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", command)

    def test_release_sdk_does_not_need_debug_acknowledgement(self) -> None:
        command = self.command_for_configuration("Release")
        self.assertNotIn("-DPULP_ALLOW_DEBUG_SDK=ON", command)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", command)


class PositiveConsumerSuiteTests(unittest.TestCase):
    document = {
        "capabilities": [
            {
                "key": "alpha",
                "bindings": [
                    {
                        "role": "request",
                        "qualified_name": "pulp::Alpha",
                        "kind": "cpp_type",
                        "include": "pulp/alpha.hpp",
                        "target": "Pulp::alpha",
                    },
                    {
                        "role": "result",
                        "qualified_name": "pulp::AlphaResult",
                        "kind": "cpp_type",
                        "include": "pulp/alpha.hpp",
                        "target": "Pulp::alpha",
                    },
                ],
            }
        ]
    }

    def proofs(self) -> list[ConsumerProof]:
        return positive_consumer_proofs(
            self.document,
            {"alpha": "auto alpha = pulp::Alpha{}; (void)alpha;"},
            {
                ("alpha", "request", "pulp::Alpha"): "auto request = pulp::Alpha{}; (void)request;",
                ("alpha", "result", "pulp::AlphaResult"): "auto result = pulp::AlphaResult{}; (void)result;",
            },
            {},
        )

    def test_suite_configures_once_with_independent_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            prefix = root / "prefix"
            config = prefix / "lib/cmake/Pulp/PulpConfig.cmake"
            config.parent.mkdir(parents=True)
            config.touch()
            project = root / "consumer"

            with mock.patch(
                "test_agent_capability_installed_sdk.run",
                return_value=subprocess.CompletedProcess([], 0, "", ""),
            ) as run_mock:
                configure_consumer_suite(
                    "cmake",
                    prefix,
                    project,
                    proofs=self.proofs(),
                    generator=None,
                    generator_options=[],
                    configuration="Release",
                )

            self.assertEqual(run_mock.call_count, 1)
            cmake = (project / "CMakeLists.txt").read_text()
            self.assertIn("add_executable(capability_0 capability_0.cpp)", cmake)
            self.assertIn("add_executable(binding_0_0 binding_0_0.cpp)", cmake)
            self.assertIn("add_executable(binding_0_1 binding_0_1.cpp)", cmake)
            self.assertEqual(cmake.count("target_link_libraries("), 3)
            self.assertEqual(cmake.count(" PRIVATE Pulp::alpha)"), 3)

    def test_missing_proof_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "coverage mismatch"):
            validate_positive_proof_coverage(self.document, self.proofs()[:-1])

    def test_duplicate_proof_is_rejected(self) -> None:
        proofs = self.proofs()
        with self.assertRaisesRegex(RuntimeError, "identities are not unique"):
            validate_positive_proof_coverage(self.document, [*proofs, proofs[0]])

    def test_wrong_minimal_target_is_rejected(self) -> None:
        proofs = self.proofs()
        proofs[0] = ConsumerProof(
            identity=proofs[0].identity,
            name=proofs[0].name,
            source=proofs[0].source,
            target="Pulp::wrong",
        )
        with self.assertRaisesRegex(RuntimeError, "target mismatch"):
            validate_positive_proof_coverage(self.document, proofs)

    def test_suite_configures_and_builds_once_but_runs_every_proof(self) -> None:
        proofs = self.proofs()
        success = subprocess.CompletedProcess([], 0, "", "")
        with (
            mock.patch(
                "test_agent_capability_installed_sdk.configure_consumer_suite",
                return_value=success,
            ) as configure_mock,
            mock.patch(
                "test_agent_capability_installed_sdk.inspect_isolation"
            ) as inspect_mock,
            mock.patch(
                "test_agent_capability_installed_sdk.build_consumer",
                return_value=success,
            ) as build_mock,
            mock.patch(
                "test_agent_capability_installed_sdk.executable_path",
                return_value=pathlib.Path("/fixture/consumer"),
            ),
            mock.patch(
                "test_agent_capability_installed_sdk.run", return_value=success
            ) as run_mock,
        ):
            configure_build_run_suite(
                "cmake",
                pathlib.Path("/prefix"),
                pathlib.Path("/project"),
                proofs,
                None,
                [],
                "Release",
                [pathlib.Path("/checkout")],
            )

        self.assertEqual(configure_mock.call_count, 1)
        self.assertEqual(build_mock.call_count, 1)
        self.assertEqual(inspect_mock.call_count, len(proofs))
        self.assertEqual(run_mock.call_count, len(proofs))


if __name__ == "__main__":
    unittest.main()
