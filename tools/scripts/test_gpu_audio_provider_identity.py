#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

import fetch_skia_for_release as skia_fetch


SCRIPT = Path(__file__).with_name("gpu_audio_provider_identity.py")
DAWN_SHA = "0123456789abcdef0123456789abcdef01234567"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ProviderFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.skia = root / "skia"
        self.header = self.skia / "build/include/dawn/dawn_version.h"
        self.library = self.skia / "build/mac-gpu/lib/Release/libdawn_combined.a"
        self.skia_library = self.skia / "build/mac-gpu/lib/Release/libskia.a"
        self.manifest = root / "manifest.json"
        self.result = root / "result.json"
        self.cmake = root / "identity.cmake"
        self.executable = root / "probe"
        self.prelink = root / "pre-link.json"
        self.bound = root / "bound.json"
        self.header.parent.mkdir(parents=True)
        self.library.parent.mkdir(parents=True)
        bytes_text = ", ".join(f"0x{DAWN_SHA[i:i + 2]}" for i in range(0, 40, 2))
        self.header.write_text(
            "namespace dawn { static constexpr std::array<unsigned char, 20> kDawnVersion = {"
            + bytes_text
            + "}; }\n",
            encoding="utf-8",
        )
        self.library.write_bytes(b"bounded fake Dawn archive\n")
        self.skia_library.write_bytes(b"bounded fake Skia archive\n")
        self.executable.write_bytes(b"bounded fake probe\n")
        archive = self.skia / skia_fetch.SOURCE_ARCHIVE
        with zipfile.ZipFile(archive, "w") as output:
            for path in (self.header, self.library, self.skia_library):
                output.write(path, path.relative_to(self.skia).as_posix())
        self.asset_sha = sha256(archive)
        (self.skia / ".skia-asset-sha256").write_text(
            self.asset_sha + "\n", encoding="ascii"
        )
        self.manifest.write_text(
            json.dumps(
                {
                    "dependencies": [
                        {
                            "name": "Skia",
                            "determinism": {
                                "built_dawn": DAWN_SHA,
                                "release_assets": {
                                    "mac-arm64": {"sha256": self.asset_sha}
                                },
                            },
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        self.write_generation()

    def write_generation(self) -> None:
        skia_fetch.write_generation_receipt(self.skia, "darwin-arm64", self.asset_sha)

    def replace_with_authenticated_provider_at_same_dawn_revision(self) -> None:
        self.library.write_bytes(b"replacement fake Dawn archive\n")
        self.skia_library.write_bytes(b"replacement fake Skia archive\n")
        archive = self.skia / skia_fetch.SOURCE_ARCHIVE
        archive.unlink()
        with zipfile.ZipFile(archive, "w") as output:
            for path in (self.header, self.library, self.skia_library):
                output.write(path, path.relative_to(self.skia).as_posix())
        self.asset_sha = sha256(archive)
        (self.skia / ".skia-asset-sha256").write_text(
            self.asset_sha + "\n", encoding="ascii"
        )
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["dependencies"][0]["determinism"]["release_assets"]["mac-arm64"] = {
            "sha256": self.asset_sha
        }
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        self.write_generation()

    def command(self, action: str) -> list[str]:
        command = [
            sys.executable,
            str(SCRIPT),
            action,
            "--manifest",
            str(self.manifest),
            "--platform",
            "darwin-arm64",
            "--skia-dir",
            str(self.skia),
            "--dawn-header",
            str(self.header),
            "--dawn-library",
            str(self.library),
        ]
        if action == "validate":
            command.extend(["--result", str(self.result), "--cmake-output", str(self.cmake)])
        elif action == "prelink":
            command.extend(
                ["--configured-receipt", str(self.result), "--result", str(self.prelink)]
            )
        elif action == "bind":
            command.extend(
                [
                    "--configured-receipt",
                    str(self.result),
                    "--prelink-receipt",
                    str(self.prelink),
                    "--executable",
                    str(self.executable),
                    "--result",
                    str(self.bound),
                ]
            )
        elif action == "verify":
            command.extend(
                [
                    "--configured-receipt",
                    str(self.result),
                    "--prelink-receipt",
                    str(self.prelink),
                    "--executable",
                    str(self.executable),
                    "--receipt",
                    str(self.bound),
                ]
            )
        return command

    def run(self, action: str = "validate") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            self.command(action),
            text=True,
            capture_output=True,
            check=False,
        )

    def establish_binding(self) -> subprocess.CompletedProcess[str]:
        configured = self.run("validate")
        if configured.returncode != 0:
            return configured
        linked = self.run("prelink")
        if linked.returncode != 0:
            return linked
        return self.run("bind")


class GpuAudioProviderIdentityTest(unittest.TestCase):
    def fixture(self) -> tuple[tempfile.TemporaryDirectory[str], ProviderFixture]:
        temporary = tempfile.TemporaryDirectory()
        return temporary, ProviderFixture(Path(temporary.name))

    def test_matching_pinned_generation_passes_and_emits_cmake_identity(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            result = fixture.run()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            receipt = json.loads(result.stdout)
            self.assertEqual(receipt["status"], "passed")
            self.assertEqual(receipt["expected_dawn_revision"], DAWN_SHA)
            self.assertIn(DAWN_SHA, fixture.cmake.read_text(encoding="utf-8"))
            self.assertIn(fixture.asset_sha, fixture.cmake.read_text(encoding="utf-8"))

    def test_unpinned_asset_fails_closed(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            (fixture.skia / ".skia-asset-sha256").write_text("b" * 64 + "\n", encoding="ascii")
            result = fixture.run()
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "provider_asset_not_pinned")
            self.assertFalse(fixture.cmake.exists())

    def test_header_manifest_version_mismatch_fails_closed(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            manifest = json.loads(fixture.manifest.read_text(encoding="utf-8"))
            manifest["dependencies"][0]["determinism"]["built_dawn"] = "f" * 40
            fixture.manifest.write_text(json.dumps(manifest), encoding="utf-8")
            result = fixture.run()
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "dawn_header_manifest_mismatch")

    def test_archive_drift_fails_generation_digest(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            fixture.library.write_bytes(b"mutated archive\n")
            result = fixture.run()
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "provider_generation_invalid")

    def test_correct_stamp_with_wrong_retained_archive_fails_closed(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            archive = fixture.skia / skia_fetch.SOURCE_ARCHIVE
            archive.write_bytes(b"forged archive with an authentic-looking stamp\n")
            result = fixture.run()
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "provider_generation_invalid")

    def test_missing_retained_archive_fails_closed(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            (fixture.skia / skia_fetch.SOURCE_ARCHIVE).unlink()
            result = fixture.run()
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "provider_generation_invalid")

    def test_shadowed_header_outside_provider_root_fails_closed(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            shadowed = fixture.root / "shadowed-dawn-version.h"
            shadowed.write_bytes(fixture.header.read_bytes())
            command = fixture.command("validate")
            command[command.index(str(fixture.header))] = str(shadowed)
            result = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "provider_file_outside_root")

    def test_missing_generation_manifest_fails_closed(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            (fixture.skia / ".skia-generation-manifest.json").unlink()
            result = fixture.run()
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["reason"], "provider_generation_invalid")

    def test_bound_identity_passes_before_mutation(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            bound = fixture.establish_binding()
            self.assertEqual(bound.returncode, 0, bound.stdout + bound.stderr)
            receipt = json.loads(bound.stdout)
            self.assertEqual(receipt["executable_sha256"], sha256(fixture.executable))
            verified = fixture.run("verify")
            self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)

    def test_executable_mutation_after_binding_is_rejected(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            self.assertEqual(fixture.establish_binding().returncode, 0)
            fixture.executable.write_bytes(b"mutated probe\n")
            verified = fixture.run("verify")
            self.assertEqual(verified.returncode, 1)
            self.assertEqual(
                json.loads(verified.stdout)["reason"],
                "bound_provider_identity_stale",
            )

    def test_manifest_mutation_after_binding_is_rejected(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            self.assertEqual(fixture.establish_binding().returncode, 0)
            manifest = json.loads(fixture.manifest.read_text(encoding="utf-8"))
            manifest["identity_control"] = "post-bind mutation"
            fixture.manifest.write_text(json.dumps(manifest), encoding="utf-8")
            verified = fixture.run("verify")
            self.assertEqual(verified.returncode, 1)
            self.assertEqual(
                json.loads(verified.stdout)["reason"],
                "configured_provider_identity_stale",
            )

    def test_provider_artifact_mutation_after_binding_is_rejected(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            self.assertEqual(fixture.establish_binding().returncode, 0)
            fixture.library.write_bytes(b"post-bind archive mutation\n")
            verified = fixture.run("verify")
            self.assertEqual(verified.returncode, 1)
            self.assertEqual(json.loads(verified.stdout)["reason"], "provider_generation_invalid")

    def test_header_artifact_mutation_after_binding_is_rejected(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            self.assertEqual(fixture.establish_binding().returncode, 0)
            fixture.header.write_text("forged Dawn version header\n", encoding="utf-8")
            verified = fixture.run("verify")
            self.assertEqual(verified.returncode, 1)
            self.assertEqual(json.loads(verified.stdout)["reason"], "provider_generation_invalid")

    def test_authenticated_same_revision_provider_change_before_link_is_rejected(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            self.assertEqual(fixture.run("validate").returncode, 0)
            fixture.replace_with_authenticated_provider_at_same_dawn_revision()
            linked = fixture.run("prelink")
            self.assertEqual(linked.returncode, 1)
            self.assertEqual(
                json.loads(linked.stdout)["reason"],
                "configured_provider_identity_stale",
            )

    def test_provider_drift_after_prelink_is_rejected_before_binding(self) -> None:
        temporary, fixture = self.fixture()
        with temporary:
            self.assertEqual(fixture.run("validate").returncode, 0)
            self.assertEqual(fixture.run("prelink").returncode, 0)
            fixture.library.write_bytes(b"post-prelink archive mutation\n")
            bound = fixture.run("bind")
            self.assertEqual(bound.returncode, 1)
            self.assertEqual(json.loads(bound.stdout)["reason"], "provider_generation_invalid")


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class ProductionProviderCmakeTests(unittest.TestCase):
    """Exercise production initialization without rendering tests or a GPU."""

    def configure(self, root: Path, *, tests: bool, arch: str = "arm64",
                  mutate: str = "", exact: bool = True) -> tuple[subprocess.CompletedProcess, ProviderFixture, Path]:
        fixture = ProviderFixture(root / "provider")
        source = root / "source"
        scripts = source / "tools/scripts"
        scripts.mkdir(parents=True)
        for filename in ("gpu_audio_provider_identity.py", "fetch_skia_for_release.py"):
            shutil.copyfile(SCRIPT.with_name(filename), scripts / filename)
        deps = source / "tools/deps"
        deps.mkdir()
        shutil.copyfile(fixture.manifest, deps / "manifest.json")
        fixture.manifest = deps / "manifest.json"
        if mutate == "header":
            fixture.header.write_text("untrusted header")
        elif mutate == "library":
            fixture.library.write_bytes(b"untrusted archive")
        elif mutate == "manifest":
            manifest = json.loads(fixture.manifest.read_text())
            manifest["dependencies"][0]["determinism"]["built_dawn"] = "f" * 40
            fixture.manifest.write_text(json.dumps(manifest))
        helper = SCRIPT.parents[1] / "cmake/PulpGpuAudioProviderIdentity.cmake"
        (source / "dummy.c").write_text("int gpu_audio_fixture;\n")
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(ProviderIdentityFixture LANGUAGES C)
set(APPLE TRUE)
set(CMAKE_OSX_ARCHITECTURES "{arch}")
set(PULP_HAS_SKIA TRUE)
set(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF {"TRUE" if exact else "FALSE"})
set(PULP_BUILD_TESTS {"ON" if tests else "OFF"})
set(SKIA_DIR "{fixture.skia}")
set(SKIA_INCLUDE_DIRS "{fixture.header.parent.parent}")
set(DAWN_LIBRARY "{fixture.library}")
add_library(pulp-gpu-audio STATIC dummy.c)
include("{helper}")
pulp_gpu_audio_configure_provider_identity(pulp-gpu-audio)
get_target_property(identity pulp-gpu-audio PULP_PROVIDER_expected_dawn_sha)
get_target_property(definitions pulp-gpu-audio COMPILE_DEFINITIONS)
file(WRITE "${{CMAKE_BINARY_DIR}}/selected.txt" "${{identity}}\\n${{definitions}}")
''')
        build = root / "build"
        result = subprocess.run(["cmake", "-S", str(source), "-B", str(build)],
                                text=True, capture_output=True)
        return result, fixture, build

    def test_tests_disabled_and_enabled_have_identical_production_identity(self):
        selected = []
        for tests in (False, True):
            with tempfile.TemporaryDirectory() as temporary:
                result, _, build = self.configure(Path(temporary), tests=tests)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                selected.append((build / "selected.txt").read_text())
                self.assertIn(DAWN_SHA, selected[-1])
                self.assertIn("PULP_GPU_AUDIO_EXPECTED_DAWN_SHA=", selected[-1])
                self.assertTrue((build / "gpu-audio-provider-identity/configure.json").exists())
        self.assertEqual(selected[0], selected[1])

    def test_nonexact_identity_is_empty_independently_of_test_registration(self):
        selected = []
        for tests in (False, True):
            with tempfile.TemporaryDirectory() as temporary:
                result, _, build = self.configure(Path(temporary), tests=tests, exact=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                selected.append((build / "selected.txt").read_text())
                self.assertNotIn(DAWN_SHA, selected[-1])
                self.assertNotIn("unknown", selected[-1])
                self.assertIn('PULP_GPU_AUDIO_EXPECTED_DAWN_SHA=""', selected[-1])
                self.assertFalse((build / "gpu-audio-provider-identity/configure.json").exists())
        self.assertEqual(selected[0], selected[1])

    def test_nonexact_intel_and_universal_do_not_request_arm_provider_proof(self):
        for arch in ("x86_64", "arm64;x86_64"):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as temporary:
                result, _, build = self.configure(
                    Path(temporary), tests=False, arch=arch, exact=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                selected = (build / "selected.txt").read_text()
                self.assertIn('PULP_GPU_AUDIO_EXPECTED_DAWN_SHA=""', selected)
                self.assertNotIn(DAWN_SHA, selected)
                self.assertFalse((build / "gpu-audio-provider-identity").exists())

    def test_tests_disabled_rejects_provider_mismatch(self):
        for mutation in ("header", "library", "manifest"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                result, _, _ = self.configure(Path(temporary), tests=False, mutate=mutation)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("exact-provider validation failed", result.stderr)

    def test_tests_disabled_rejects_non_thin_arm64(self):
        for arch in ("x86_64", "arm64;x86_64"):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as temporary:
                result, _, _ = self.configure(Path(temporary), tests=False, arch=arch)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("thin Apple-Silicon macOS target", " ".join(result.stderr.split()))

    def test_prelink_validates_without_tests_and_refuses_later_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, fixture, build = self.configure(Path(temporary), tests=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            command = ["cmake", "--build", str(build), "--target", "pulp-gpu-audio-provider-prelink"]
            valid = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
            fixture.replace_with_authenticated_provider_at_same_dawn_revision()
            stale = subprocess.run(command, text=True, capture_output=True)
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn("configured_provider_identity_stale", stale.stdout + stale.stderr)


if __name__ == "__main__":
    unittest.main()
