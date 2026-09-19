#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
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


if __name__ == "__main__":
    unittest.main()
