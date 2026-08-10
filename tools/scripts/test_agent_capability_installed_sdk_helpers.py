#!/usr/bin/env python3
"""Focused regression tests for installed-SDK proof helpers."""
from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from test_agent_capability_installed_sdk import (
    combine_flag_values,
    configure_consumer,
    find_build_tree_artifact,
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


if __name__ == "__main__":
    unittest.main()
