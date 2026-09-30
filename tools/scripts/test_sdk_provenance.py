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
            "PULP_TRACING:BOOL=OFF\n"
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=ON\n"
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=FALSE\n"
            "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE:INTERNAL=FALSE\n",
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
        self.assertEqual(
            marker["features"],
            {"audio_probes": False, "inspector": True, "tracing": False},
        )
        self.assertEqual(marker["integrity"]["schema"], provenance.INTEGRITY_SCHEMA)
        self.assertEqual(
            stat.S_IMODE((self.prefix / "sdk-provenance.json").stat().st_mode),
            0o644,
        )

    def install_gpu_audio_capabilities(self, *, shared=True, exact=True, convolver=True):
        facts = {"shared_provider": shared, "shared_convolver": convolver,
                 "exact_provider_proof": exact}
        cache = self.build / "CMakeCache.txt"
        with cache.open("a") as stream:
            stream.write("PULP_GPU_AUDIO_CAPABILITY_SCHEMA:INTERNAL=1\n")
            for key, (name, _) in provenance.GPU_AUDIO_CAPABILITIES.items():
                stream.write(f"{name}:BOOL={'ON' if facts[key] else 'OFF'}\n")
        config = self.prefix / "lib/cmake/Pulp/PulpConfig.cmake"
        config.parent.mkdir(parents=True)
        config.write_text('set(PULP_GPU_AUDIO_CAPABILITY_SCHEMA "1")\n' + "\n".join(
            f'set({exported} "{"ON" if facts[key] else "OFF"}")'
            for key, (_, exported) in provenance.GPU_AUDIO_CAPABILITIES.items()
        ))
        (self.prefix / "lib/libpulp-gpu-audio.a").write_bytes(b"gpu audio fixture")
        return facts

    def test_gpu_audio_capabilities_bind_installed_config_and_archive(self):
        facts = self.install_gpu_audio_capabilities()
        marker = self.marker()
        self.assertEqual(marker["gpu_audio"]["capabilities"], facts)
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(provenance.verify_release_marker(
            self.prefix, expected_platform="darwin-arm64", expected_source_sha=self.sha), marker)
        for path in marker["gpu_audio"]["files"]:
            with self.subTest(path=path):
                member = self.prefix / path
                original = member.read_bytes()
                member.write_bytes(original + b"\nchanged")
                with self.assertRaisesRegex(provenance.ProvenanceError, "capability integrity"):
                    provenance.verify_release_marker(
                        self.prefix, expected_platform="darwin-arm64", expected_source_sha=self.sha)
                member.write_bytes(original)

    def test_modern_gpu_audio_disabled_sdk_remains_eligible(self):
        facts = self.install_gpu_audio_capabilities(shared=False, exact=False, convolver=False)
        for platform in ("darwin-arm64", "linux-x64", "windows-x64"):
            with self.subTest(platform=platform):
                if platform.startswith("windows"):
                    for member in ("pulp-view-script", "pulp-gpu-audio"):
                        (self.prefix / f"lib/{member}.lib").write_bytes(b"Windows fixture")
                marker = self.marker(platform=platform)
                self.assertEqual(marker["gpu_audio"]["capabilities"], facts)
                provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
                self.assertTrue(provenance.verify_release_marker(
                    self.prefix, expected_platform=platform, expected_source_sha=self.sha)["distribution_eligible"])

    def test_intel_shared_provider_without_arm_authentication_is_valid(self):
        facts = self.install_gpu_audio_capabilities(exact=False, convolver=False)
        marker = self.marker(platform="darwin-x64")
        self.assertEqual(marker["gpu_audio"]["capabilities"], facts)
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(provenance.verify_release_marker(
            self.prefix, expected_platform="darwin-x64", expected_source_sha=self.sha), marker)

    def test_gpu_audio_modern_receipt_cannot_be_deleted(self):
        self.install_gpu_audio_capabilities()
        marker = self.marker()
        del marker["gpu_audio"]
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaisesRegex(provenance.ProvenanceError, "missing GPU-audio capability receipt"):
            provenance.verify_release_marker(
                self.prefix, expected_platform="darwin-arm64", expected_source_sha=self.sha)

    def test_gpu_audio_boolean_strings_and_partial_tuple_are_rejected(self):
        self.install_gpu_audio_capabilities()
        for value in ("false", 1, None):
            marker = self.marker()
            marker["gpu_audio"]["capabilities"]["shared_provider"] = value
            provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
            with self.subTest(value=value), self.assertRaisesRegex(provenance.ProvenanceError, "capability facts"):
                provenance.verify_release_marker(
                    self.prefix, expected_platform="darwin-arm64", expected_source_sha=self.sha)
        marker = self.marker()
        del marker["gpu_audio"]["capabilities"]["shared_provider"]
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaisesRegex(provenance.ProvenanceError, "capability facts"):
            provenance.verify_release_marker(
                self.prefix, expected_platform="darwin-arm64", expected_source_sha=self.sha)

    def test_gpu_audio_unsupported_or_partial_installed_schema_rejected(self):
        self.install_gpu_audio_capabilities()
        config = self.prefix / "lib/cmake/Pulp/PulpConfig.cmake"
        original = config.read_text()
        for replacement in ('set(PULP_GPU_AUDIO_CAPABILITY_SCHEMA "2")',
                            'set(PULP_GPU_AUDIO_CAPABILITY_SCHEMA)'):
            with self.subTest(replacement=replacement):
                config.write_text(original.replace('set(PULP_GPU_AUDIO_CAPABILITY_SCHEMA "1")', replacement))
                with self.assertRaisesRegex(provenance.ProvenanceError, "unsupported installed"):
                    self.marker()
        config.write_text(original)

    def test_installed_template_exports_producer_facts_over_consumer_cache(self):
        template = (SCRIPT.parents[1] / "cmake/PulpConfig.cmake.in").read_text()
        section = template.split('set(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO', 1)[1].split(
            'if(PULP_GPU_AUDIO_HAS_VELLUM_D15)', 1)[0]
        section = 'set(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO' + section
        for key, (producer, _) in provenance.GPU_AUDIO_CAPABILITIES.items():
            section = section.replace('@' + producer + '@', 'ON')
        section = section.replace('@PULP_GPU_AUDIO_CAPABILITY_SCHEMA@', '1')
        script = self.root / 'exports.cmake'
        exports = [value[1] for value in provenance.GPU_AUDIO_CAPABILITIES.values()]
        script.write_text('\n'.join(f'set({name} OFF CACHE BOOL "consumer spoof")' for name in exports)
                          + '\n' + section + '\n'
                          + '\n'.join(f'if(NOT {name})\nmessage(FATAL_ERROR "wrong producer fact")\nendif()' for name in exports))
        result = subprocess.run(['cmake', '-P', str(script)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_gpu_audio_missing_modern_cache_schema_rejected(self):
        self.install_gpu_audio_capabilities()
        cache = self.build / "CMakeCache.txt"
        cache.write_text(cache.read_text().replace("PULP_GPU_AUDIO_CAPABILITY_SCHEMA:INTERNAL=1\n", ""))
        with self.assertRaisesRegex(provenance.ProvenanceError, "missing GPU-audio producer"):
            self.marker()

    def test_gpu_audio_rejects_unauthenticated_convolver(self):
        self.install_gpu_audio_capabilities(exact=False)
        with self.assertRaisesRegex(provenance.ProvenanceError, "authenticated shared provider"):
            self.marker()

    def test_gpu_audio_rejects_exact_proof_on_intel(self):
        self.install_gpu_audio_capabilities()
        with self.assertRaisesRegex(provenance.ProvenanceError, "darwin-arm64"):
            self.marker(platform="darwin-x64")

    def test_gpu_audio_rejects_installed_config_disagreement(self):
        self.install_gpu_audio_capabilities()
        path = self.prefix / "lib/cmake/Pulp/PulpConfig.cmake"
        path.write_text(path.read_text().replace('ENABLED "ON"', 'ENABLED "OFF"'))
        with self.assertRaisesRegex(provenance.ProvenanceError, "capability mismatch"):
            self.marker()

    def test_gpu_audio_missing_modern_fact_is_not_false(self):
        self.install_gpu_audio_capabilities()
        path = self.build / "CMakeCache.txt"
        path.write_text(path.read_text().replace("PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF:BOOL=ON\n", ""))
        with self.assertRaisesRegex(provenance.ProvenanceError, "missing"):
            self.marker()

    def test_old_gpu_audio_flags_without_schema_remain_unknown(self):
        with (self.build / "CMakeCache.txt").open("a") as stream:
            stream.write("PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF:BOOL=OFF\n")
        self.assertNotIn("gpu_audio", self.marker())

    def test_traced_build_cannot_mint_release_provenance(self) -> None:
        # Control: the fixture mints a marker cleanly with tracing off, so the
        # rejection below is caused by the flag and not by a broken fixture.
        self.assertEqual(self.marker()["features"]["tracing"], False)

        (self.build / "CMakeCache.txt").write_text(
            "PULP_TRACING:BOOL=ON\n"
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=ON\n",
            encoding="utf-8",
        )
        with self.assertRaises(provenance.ProvenanceError) as caught:
            self.marker()
        self.assertIn("tracing=OFF", str(caught.exception))

    def test_development_vellum_d15_cannot_mint_release_provenance(self) -> None:
        # Control: a build without D15 mints the production marker.
        self.assertTrue(self.marker()["distribution_eligible"])

        cache = (self.build / "CMakeCache.txt").read_text(encoding="utf-8")
        cache = cache.replace(
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=FALSE",
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=TRUE",
        )
        (self.build / "CMakeCache.txt").write_text(cache, encoding="utf-8")
        with self.assertRaisesRegex(
            provenance.ProvenanceError, "Vellum D15.*development-only"
        ):
            self.marker()

    def test_release_eligible_flag_alone_cannot_mint_d15_provenance(self) -> None:
        cache = (self.build / "CMakeCache.txt").read_text(encoding="utf-8")
        cache = cache.replace(
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=FALSE",
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=TRUE",
        ).replace(
            "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE:INTERNAL=FALSE",
            "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE:INTERNAL=TRUE",
        )
        (self.build / "CMakeCache.txt").write_text(cache, encoding="utf-8")
        with self.assertRaisesRegex(
            provenance.ProvenanceError, "not yet supported by the production archive"
        ):
            self.marker()

    def test_verify_rejects_a_marker_claiming_a_traced_release(self) -> None:
        marker = self.marker()
        marker["features"]["tracing"] = True
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaises(provenance.ProvenanceError):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            )

    def test_verify_accepts_a_marker_minted_before_the_tracing_key_existed(self) -> None:
        marker = self.marker()
        del marker["features"]["tracing"]
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            ),
            marker,
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
            "PULP_TRACING:BOOL=OFF\n"
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=OFF\n"
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=FALSE\n"
            "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE:INTERNAL=FALSE\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "inspector=ON"):
            self.marker()

    def test_historical_release_before_inspector_floor_requires_component_off(self) -> None:
        version = "0.771.0"
        (self.prefix / "version.txt").write_text(f"{version}\n", encoding="utf-8")
        self.write_build_info(version=version)
        (self.build / "CMakeCache.txt").write_text(
            "PULP_TRACING:BOOL=OFF\n"
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=OFF\n"
            "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=FALSE\n"
            "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE:INTERNAL=FALSE\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["git", "-C", self.source, "-c", "tag.gpgSign=false", "tag", f"v{version}"],
            check=True,
        )
        marker = self.marker(release_tag=f"v{version}")
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            marker["features"],
            {"audio_probes": False, "inspector": False, "tracing": False},
        )
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
