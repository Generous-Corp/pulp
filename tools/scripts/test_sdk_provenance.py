#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).with_name("sdk_provenance.py")
spec = importlib.util.spec_from_file_location("sdk_provenance", SCRIPT)
assert spec and spec.loader
provenance = importlib.util.module_from_spec(spec)
sys.modules["sdk_provenance"] = provenance
spec.loader.exec_module(provenance)

VERSION = "9.8.7"


class SdkProvenanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "prefix"
        self.build = self.root / "build"
        self.source = self.root / "source"
        self.prefix.mkdir()
        self.build.mkdir()
        self.source.mkdir()
        (self.prefix / "version.txt").write_text(f"{VERSION}\n", encoding="utf-8")
        (self.prefix / "sdk_build_type.txt").write_text("Release\n", encoding="utf-8")
        (self.build / "CMakeCache.txt").write_text(
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=ON\n",
            encoding="utf-8",
        )
        subprocess.run(["git", "init", "-q", self.source], check=True)
        subprocess.run(
            ["git", "-C", self.source, "-c", "user.name=Test", "-c", "user.email=test@example.com",
             "commit", "--allow-empty", "-qm", "fixture"],
            check=True,
        )
        self.sha = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        self.write_build_info()
        (self.prefix / "include/pulp/view").mkdir(parents=True)
        (self.prefix / "include/pulp/view/widget_bridge.hpp").write_bytes(
            b"widget bridge fixture\n"
        )
        (self.prefix / "lib").mkdir()
        (self.prefix / "lib/libpulp-view-script.a").write_bytes(
            b"view script fixture\n"
        )
        subprocess.run(
            ["git", "-C", self.source, "-c", "tag.gpgSign=false", "tag", f"v{VERSION}"],
            check=True,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def marker(self, **overrides: object) -> dict[str, object]:
        arguments = {
            "prefix": self.prefix,
            "build_dir": self.build,
            "source_dir": self.source,
            "release_tag": f"v{VERSION}",
            "source_sha": self.sha,
            "platform": "darwin-arm64",
        }
        arguments.update(overrides)
        return provenance.build_release_marker(**arguments)

    def write_build_info(
        self,
        *,
        build_type: str = "Release",
        dirty: bool = False,
        version: str = VERSION,
        source_sha: str | None = None,
    ) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "#pragma once\n\n"
            "#include <string_view>\n\n"
            "namespace pulp::runtime {\n"
            f'inline constexpr std::string_view kBuildType = "{build_type}";\n'
            'inline constexpr std::string_view kBuildIso8601 = "2026-08-10T00:00:00Z";\n'
            f'inline constexpr std::string_view kGitSha = "{(source_sha or self.sha)[:7]}";\n'
            f"inline constexpr bool kGitDirty = {'true' if dirty else 'false'};\n"
            f'inline constexpr std::string_view kSdkVersion = "{version}";\n'
            f'inline constexpr std::string_view kStampLabel = "{version} fixture";\n'
            "}\n",
            encoding="utf-8",
        )

    def test_stamp_and_verify_positive_release_marker(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            ),
            marker,
        )
        self.assertEqual(marker["features"], {"audio_probes": False, "inspector": True})
        self.assertEqual(marker["integrity"]["schema"], provenance.INTEGRITY_SCHEMA)
        self.assertEqual(
            stat.S_IMODE((self.prefix / "sdk-provenance.json").stat().st_mode),
            0o644,
        )

    def test_stamp_command_emits_capability_handoff(self) -> None:
        shared = self.prefix / "share/pulp"
        binary = self.prefix / "bin"
        shared.mkdir(parents=True)
        binary.mkdir()
        (binary / "pulp-import-design").write_bytes(b"importer fixture")
        for value in provenance._importer_runtime_paths(
            self.prefix, "linux-x64"
        ):
            path = Path(value)
            (self.prefix / path).parent.mkdir(parents=True, exist_ok=True)
            (self.prefix / path).write_bytes(f"fixture {path.name}".encode())
        capabilities = {"schema": "fixture.capabilities.v1"}
        (shared / "agent-capabilities.json").write_text(
            json.dumps(capabilities) + "\n", encoding="utf-8"
        )
        (shared / "agent-capabilities.schema.json").write_text(
            json.dumps(
                {
                    "type": "object",
                    "additionalProperties": False,
                    "required": ["schema"],
                    "properties": {
                        "schema": {"const": "fixture.capabilities.v1"}
                    },
                }
            ),
            encoding="utf-8",
        )
        handoff_schema = (
            SCRIPT.parents[2]
            / "docs/status/agent-capability-handoff.schema.json"
        )
        (shared / handoff_schema.name).write_bytes(handoff_schema.read_bytes())
        self.assertEqual(
            provenance.main(
                [
                    "stamp",
                    "--prefix",
                    str(self.prefix),
                    "--build-dir",
                    str(self.build),
                    "--source-dir",
                    str(self.source),
                    "--release-tag",
                    f"v{VERSION}",
                    "--source-sha",
                    self.sha,
                    "--platform",
                    "linux-x64",
                ]
            ),
            0,
        )
        emitted = json.loads(
            (shared / "agent-capability-handoff.json").read_text(encoding="utf-8")
        )
        self.assertEqual(emitted["sdk_source_sha"], self.sha)
        self.assertEqual(emitted["agent_capabilities"]["content"], capabilities)

    def test_importer_runtime_paths_include_private_node_at_floor(self) -> None:
        (self.prefix / "version.txt").write_text("0.813.0\n", encoding="utf-8")
        before = provenance._importer_runtime_paths(self.prefix, "darwin-arm64")
        self.assertNotIn("bin/browser_capture-v1/node", before)
        (self.prefix / "version.txt").write_text("0.813.1\n", encoding="utf-8")
        at_floor = provenance._importer_runtime_paths(self.prefix, "darwin-arm64")
        self.assertIn("bin/browser_capture-v1/node", at_floor)
        self.assertIn("bin/browser_capture-v1/node.LICENSE", at_floor)
        windows = provenance._importer_runtime_paths(self.prefix, "windows-x64")
        self.assertIn("bin/browser_capture-v1/node.exe", windows)
        self.assertNotIn("bin/browser_capture-v1/node", windows)

    def test_historical_matrix_without_node_floor_remains_supported(self) -> None:
        matrix = self.root / "historical-release-product-matrix.json"
        matrix.write_text(
            json.dumps({"common_cli_members": ["browser_capture/capture.mjs"]}),
            encoding="utf-8",
        )
        with mock.patch.object(provenance, "PRODUCT_MATRIX", matrix):
            paths = provenance._importer_runtime_paths(self.prefix, "darwin-arm64")
        self.assertEqual(paths, {"bin/browser_capture-v1/capture.mjs"})

    def test_stamp_succeeds_without_posix_fchmod(self) -> None:
        marker = self.marker()
        with mock.patch.object(provenance.os, "fchmod", None, create=True):
            provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            json.loads(
                (self.prefix / "sdk-provenance.json").read_text(encoding="utf-8")
            ),
            marker,
        )

    def test_windows_release_binds_the_windows_view_script_archive(self) -> None:
        archive = self.prefix / "lib/pulp-view-script.lib"
        archive.write_bytes(b"windows view script fixture\n")
        marker = self.marker(platform="windows-x64")
        self.assertIn("lib/pulp-view-script.lib", marker["integrity"]["files"])
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="windows-x64",
                expected_source_sha=self.sha,
            ),
            marker,
        )

    def test_rejects_tag_version_mismatch(self) -> None:
        with self.assertRaisesRegex(provenance.ProvenanceError, "does not match"):
            self.marker(release_tag="v1.2.3")

    def test_rejects_oversized_installed_build_info(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        path.write_bytes(b" " * (provenance.BUILD_INFO_MAX_BYTES + 1))
        with self.assertRaisesRegex(provenance.ProvenanceError, "byte limit"):
            self.marker()

    def test_rejects_tag_source_mismatch(self) -> None:
        subprocess.run(
            ["git", "-C", self.source, "-c", "user.name=Test", "-c", "user.email=test@example.com",
             "commit", "--allow-empty", "-qm", "later"],
            check=True,
        )
        later = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        with self.assertRaisesRegex(provenance.ProvenanceError, "identify one commit"):
            self.marker(source_sha=later)

    def test_rejects_missing_release_inspector_component(self) -> None:
        (self.build / "CMakeCache.txt").write_text(
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=OFF\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "inspector=ON"):
            self.marker()

    def test_historical_release_before_inspector_floor_requires_component_off(self) -> None:
        version = "0.771.0"
        (self.prefix / "version.txt").write_text(f"{version}\n", encoding="utf-8")
        self.write_build_info(version=version)
        (self.build / "CMakeCache.txt").write_text(
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=OFF\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["git", "-C", self.source, "-c", "tag.gpgSign=false", "tag", f"v{version}"],
            check=True,
        )
        marker = self.marker(release_tag=f"v{version}")
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(marker["features"], {"audio_probes": False, "inspector": False})
        self.assertNotIn("integrity", marker)
        self.assertEqual(
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            ),
            marker,
        )

    def test_rejects_release_audio_probes(self) -> None:
        (self.build / "CMakeCache.txt").write_text(
            "PULP_ENABLE_AUDIO_PROBES:BOOL=ON\n"
            "PULP_ENABLE_INSPECTOR:BOOL=ON\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "audio_probes=OFF"):
            self.marker()

    def test_rejects_dirty_tracked_source(self) -> None:
        tracked = self.source / "tracked.txt"
        tracked.write_text("clean\n", encoding="utf-8")
        subprocess.run(["git", "-C", self.source, "add", tracked.name], check=True)
        subprocess.run(
            [
                "git",
                "-C",
                self.source,
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.com",
                "commit",
                "-qm",
                "tracked fixture",
            ],
            check=True,
        )
        later = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        subprocess.run(
            [
                "git",
                "-C",
                self.source,
                "-c",
                "tag.gpgSign=false",
                "tag",
                "-f",
                f"v{VERSION}",
                later,
            ],
            check=True,
        )
        tracked.write_text("dirty\n", encoding="utf-8")
        with self.assertRaisesRegex(provenance.ProvenanceError, "clean tracked source"):
            self.marker(source_sha=later)

    def test_untracked_source_input_does_not_block_official_marker(self) -> None:
        (self.source / "configure-input.txt").write_text(
            "untracked\n", encoding="utf-8"
        )
        self.assertEqual(self.marker()["source_git_dirty"], False)

    def test_rejects_unsafe_installed_build_info(self) -> None:
        cases = (
            ({"dirty": True}, "tracked source changes"),
            ({"build_type": "Debug"}, "not a Release build"),
            ({"version": "1.2.3"}, "SDK version does not match"),
            ({"source_sha": "b" * 40}, "source SHA does not match"),
        )
        for values, message in cases:
            with self.subTest(values=values):
                self.write_build_info(**values)
                with self.assertRaisesRegex(provenance.ProvenanceError, message):
                    self.marker()
        self.write_build_info()

    def test_rejects_duplicate_installed_build_info_constant(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        with path.open("a", encoding="utf-8") as handle:
            handle.write(
                'inline constexpr std::string_view kSdkVersion = "9.8.7";\n'
            )
        with self.assertRaisesRegex(
            provenance.ProvenanceError, "canonical generated structure"
        ):
            self.marker()

    def test_rejects_commented_safe_values_with_alternate_active_declaration(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        path.write_text(
            "/*\n"
            'inline constexpr std::string_view kBuildType = "Release";\n'
            f'inline constexpr std::string_view kGitSha = "{self.sha[:7]}";\n'
            "inline constexpr bool kGitDirty = false;\n"
            f'inline constexpr std::string_view kSdkVersion = "{VERSION}";\n'
            "*/\n"
            'constexpr inline std::string_view kBuildType = "Debug";\n'
            f'constexpr inline std::string_view kGitSha = "{self.sha[:7]}";\n'
            "constexpr inline bool kGitDirty = true;\n"
            f'constexpr inline std::string_view kSdkVersion = "{VERSION}";\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            provenance.ProvenanceError, "canonical generated structure"
        ):
            self.marker()

    def test_rejects_preprocessor_disabled_safe_declarations(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        path.write_text("#if 0\n" + path.read_text(encoding="utf-8") + "#endif\n")
        with self.assertRaisesRegex(
            provenance.ProvenanceError, "canonical generated structure"
        ):
            self.marker()

    def test_rejects_preprocessor_directives_joined_on_one_line(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        path.write_text(
            path.read_text(encoding="utf-8").replace(
                "#pragma once\n\n#include <string_view>\n\n",
                "#pragma once #include <string_view>\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            provenance.ProvenanceError, "canonical generated structure"
        ):
            self.marker()

    def test_rejects_preprocessor_directives_split_across_lines(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        original = path.read_text(encoding="utf-8")
        for old, new in (
            ("#pragma once", "#pragma\nonce"),
            ("#include <string_view>", "#include\n<string_view>"),
        ):
            with self.subTest(directive=old):
                path.write_text(original.replace(old, new), encoding="utf-8")
                with self.assertRaisesRegex(
                    provenance.ProvenanceError, "canonical generated structure"
                ):
                    self.marker()
        path.write_text(original, encoding="utf-8")

    def test_rejects_declarations_hidden_by_continued_line_comments(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        hidden = "".join(
            f"// \\\n{line}\n"
            for line in path.read_text(encoding="utf-8").splitlines()
            if "inline constexpr" in line
        )
        path.write_text(hidden, encoding="utf-8")
        with self.assertRaisesRegex(provenance.ProvenanceError, "line splicing"):
            self.marker()

    def test_rejects_preprocessing_digraphs(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        path.write_text(
            "%:if 0\n" + path.read_text(encoding="utf-8") + "%:endif\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "digraphs"):
            self.marker()

    def test_rejects_declarations_embedded_in_raw_string(self) -> None:
        path = self.prefix / provenance.BUILD_INFO_PATH
        declarations = "\n".join(
            line
            for line in path.read_text(encoding="utf-8").splitlines()
            if "inline constexpr" in line
        )
        path.write_text(
            f'constexpr auto payload = R"fixture(\n{declarations}\n)fixture";\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "raw strings"):
            self.marker()

    def test_verify_rejects_marker_for_different_prefix_version(self) -> None:
        marker = self.marker()
        marker["sdk_version"] = "1.2.3"
        marker["source_git_ref"] = "v1.2.3"
        (self.prefix / "sdk-provenance.json").write_text(
            json.dumps(marker), encoding="utf-8"
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "selected SDK prefix"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            )

    def test_verify_rejects_wrong_expected_commit(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaisesRegex(provenance.ProvenanceError, "source_git_sha"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha="b" * 40,
            )

    def test_verify_rejects_wrong_expected_platform(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaisesRegex(provenance.ProvenanceError, "platform"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="linux-x64",
                expected_source_sha=self.sha,
            )

    def test_verify_rejects_mixed_widget_bridge_header_and_archive(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        (self.prefix / "include/pulp/view/widget_bridge.hpp").write_bytes(
            b"stale widget bridge fixture\n"
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "coherence integrity"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            )

    def test_verify_rejects_missing_marker(self) -> None:
        with self.assertRaisesRegex(provenance.ProvenanceError, "cannot read valid"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            )


if __name__ == "__main__":
    unittest.main()
