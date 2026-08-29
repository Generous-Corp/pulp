#!/usr/bin/env python3
"""Portable mutation tests for the terminal A2 acceptance recorder contract."""

from __future__ import annotations

import binascii
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parents[2]
RECORDER_PATH = Path(__file__).with_name("gpu_probe_acceptance.py")
RECORDER_SPEC = importlib.util.spec_from_file_location("gpu_probe_acceptance", RECORDER_PATH)
assert RECORDER_SPEC and RECORDER_SPEC.loader
RECORDER = importlib.util.module_from_spec(RECORDER_SPEC)
RECORDER_SPEC.loader.exec_module(RECORDER)
VERIFIER = RECORDER.verifier
OLD = ROOT / "docs/validation/gpu-probes/m3-a2-real-probes-20260828"


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", binascii.crc32(kind + payload) & 0xffffffff))


def write_png(path: Path, *, patterned: bool = True) -> None:
    width, height = 320, 240
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            if patterned:
                rows.extend(((x // 8) % 256, (y // 4) % 256, (x + y) % 256))
            else:
                rows.extend((10, 10, 10))
    payload = b"\x89PNG\r\n\x1a\n"
    payload += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    payload += png_chunk(b"IDAT", zlib.compress(bytes(rows)))
    payload += png_chunk(b"IEND", b"")
    path.write_bytes(payload)


class GpuProbeAcceptanceTests(unittest.TestCase):
    def v2_fixture(self, directory: Path) -> dict:
        for group in RECORDER.RECIPES:
            for suffix in ("run1", "run2", "negative"):
                shutil.copy2(OLD / f"{group}-{suffix}.json", directory)
        transcript = [{
            "jsonrpc": "2.0", "id": 1,
            "result": {"protocolVersion": "2024-11-05"},
        }]
        next_id = 2
        for group in RECORDER.RECIPES:
            for suffix, exit_code in (("run1", 0), ("negative", 1)):
                evidence = json.loads((directory / f"{group}-{suffix}.json").read_text())
                transcript.append({
                    "jsonrpc": "2.0", "id": next_id,
                    "result": {
                        "isError": exit_code != 0,
                        "structuredContent": {"exit_code": exit_code, "evidence": evidence},
                        "content": [{"type": "text", "text": json.dumps(evidence)}],
                    },
                })
                next_id += 1
        (directory / "mcp-transcript.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in transcript)
        )
        screenshot = directory / "forge-modular-screenshot.png"
        write_png(screenshot)
        doctor = json.loads((ROOT / "test/fixtures/gpu-ux/pass-hardware.json").read_text())
        (directory / "forge-gpu-doctor.json").write_text(json.dumps(doctor))
        head = subprocess_text(["git", "rev-parse", "HEAD"], ROOT)
        source_blobs = VERIFIER._git_blobs(head, VERIFIER.EXPECTED_SOURCE_BLOBS_V2)
        raw_names = {
            *(f"{group}-{suffix}.json" for group in RECORDER.RECIPES
              for suffix in ("run1", "run2", "negative")),
            "mcp-transcript.jsonl", "forge-modular-screenshot.png", "forge-gpu-doctor.json",
        }
        receipt = {
            "schema": "pulp.gpu-probe-acceptance-receipt.v2",
            "integration_head": head,
            "source_identity": {
                "repository": "Generous-Corp/pulp", "revision": head, "clean": True,
                "status_sha256": hashlib.sha256(b"").hexdigest(),
            },
            "source_blobs": source_blobs,
            "accepted_plan": {
                "repository": "danielraffel/pulp-planning",
                "revision": VERIFIER.EXPECTED_PLAN_REVISION,
                "path": "research/2026-08-27-vgpu-gpu-ux-inspiration-audit-and-plan.md",
                "blob": VERIFIER.EXPECTED_PLAN_BLOB,
                "sha256": VERIFIER.EXPECTED_PLAN_SHA256,
            },
            "execution_context": {
                "cwd_role": "fresh-temporary-directory-outside-any-checkout",
                "path": RECORDER.SYSTEM_PATH,
            },
            "install_provenance": {
                "cmake_home_revision": head, "cmake_build_type": "Release",
                "rust_profile": "release", "build_install_binary_identity": "pass",
                "cmake_cache_sha256": "7" * 64, "build_info_sha256": "8" * 64,
                "feature_contract": {
                    "PULP_ENABLE_GPU": "ON", "PULP_ENABLE_SCENE3D": "ON",
                    "PULP_ENABLE_THREEJS_RUNTIME": "ON", "PULP_ENABLE_JS": "ON",
                    "PULP_JS_ENGINE": "v8", "PULP_BUILD_RUST_CLI": "ON",
                    "PULP_HAS_THREEJS": "TRUE",
                },
                "build_info": {"kBuildType": "Release", "kGitDirty": False,
                               "kGitSha": head[:12]},
            },
            "binaries": {
                role: {"sha256": str(index) * 64, "bytes": 1,
                       "build_output_sha256": str(index) * 64}
                for index, role in enumerate(
                    ("installed_rust_cli", "installed_cpp_delegate", "installed_mcp"), 1
                )
            },
            "run_groups": {
                group: {"recipe": recipe, "binary_role": "installed_rust_cli"}
                for group, recipe in RECORDER.RECIPES.items()
            },
            "raw_sha256": {
                name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                for name in raw_names
            },
            "forge_downstream": {
                "repository": "Generous-Corp/forge",
                "revision": VERIFIER.EXPECTED_FORGE_REVISION,
                "pulp_sdk_ref_overlay": {
                    "path": "PULP_SDK_REF", "content": head,
                    "original_blob": VERIFIER.EXPECTED_FORGE_PULP_REF_BLOB,
                },
                "all_other_tracked_files_clean": True,
                "bundle_build_info": {
                    "schema": "1", "version": "1.0.0",
                    "packaged": "2026-08-29T00:00:00Z",
                    "product": "Forge Modular", "role": "Rack module and patch generator",
                    "product_id": "com.generous.forge.modular",
                    "format": "Standalone application", "build": "Release · macOS",
                    "pulp_sdk": f"0.0.0 · {head[:12]}",
                },
                "build_target": "ForgeModular_Standalone", "codesign_verify": "pass",
                "cmake_cache_sha256": "4" * 64,
                "bundle_build_info_sha256": "5" * 64,
                "bundle_binary_sha256": "6" * 64,
                "screenshot_metrics": VERIFIER._png_metrics(screenshot),
                "missing_path_canaries": {
                    "gpu_audio": {
                        "status": "pass", "recipe": RECORDER.RECIPES["stft"],
                        "cli_positive_files": ["stft-run1.json", "stft-run2.json"],
                        "cli_negative_file": "stft-negative.json",
                        "mcp_positive_response_id": 4, "mcp_negative_response_id": 5,
                    },
                    "threejs": {
                        "status": "pass", "recipe": RECORDER.RECIPES["threejs"],
                        "cli_positive_files": ["threejs-run1.json", "threejs-run2.json"],
                        "cli_negative_file": "threejs-negative.json",
                        "mcp_positive_response_id": 8, "mcp_negative_response_id": 9,
                    },
                },
            },
            "acceptance": {
                "terminal_status": "pass", "all_four_installed_cli": "pass",
                "all_four_installed_mcp": "pass", "seeded_negative_controls": "pass",
                "forge_modular_and_missing_path_canaries": "pass",
            },
        }
        (directory / "receipt.json").write_text(json.dumps(receipt))
        return receipt

    @staticmethod
    def rebind(directory: Path, name: str) -> None:
        receipt_path = directory / "receipt.json"
        receipt = json.loads(receipt_path.read_text())
        receipt["raw_sha256"][name] = hashlib.sha256((directory / name).read_bytes()).hexdigest()
        receipt_path.write_text(json.dumps(receipt))

    def test_v2_all_four_exact_head_fixture_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            self.assertEqual(VERIFIER.verify(root), [])

    def test_v2_rejects_missing_mcp_recipe_and_direct_binary_role(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            rows = (root / "mcp-transcript.jsonl").read_text().splitlines()
            (root / "mcp-transcript.jsonl").write_text("\n".join(rows[:-2]) + "\n")
            self.rebind(root, "mcp-transcript.jsonl")
            receipt = json.loads((root / "receipt.json").read_text())
            receipt["run_groups"]["renderer"]["binary_role"] = "scene3d_cpp_cli"
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("all-four" in error for error in errors))
            self.assertTrue(any("wrong executable role" in error for error in errors))

    def test_v2_rejects_forged_build_plan_and_forge_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            receipt["accepted_plan"]["revision"] = "0" * 40
            receipt["install_provenance"]["build_info"]["kGitSha"] = "0" * 12
            receipt["forge_downstream"]["bundle_build_info"]["pulp_sdk"] = "wrong"
            receipt["acceptance"]["terminal_status"] = "draft"
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("accepted plan revision" in error for error in errors))
            self.assertTrue(any("installed CLI provenance" in error for error in errors))
            self.assertTrue(any("Forge bundle stamp" in error for error in errors))

    def test_v2_rejects_empty_installed_source_stamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            receipt["install_provenance"]["build_info"]["kGitSha"] = ""
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("installed CLI provenance" in error for error in errors))

    def test_png_content_cap_rejects_blank_even_when_digest_rebound(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            write_png(root / "forge-modular-screenshot.png", patterned=False)
            self.rebind(root, "forge-modular-screenshot.png")
            errors = VERIFIER.verify(root)
            self.assertTrue(any("blank" in error for error in errors))

    def test_malformed_png_ihdr_returns_a_verifier_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            malformed = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", b"\0" * 12)
            malformed += png_chunk(b"IEND", b"")
            (root / "forge-modular-screenshot.png").write_bytes(malformed)
            self.rebind(root, "forge-modular-screenshot.png")
            errors = VERIFIER.verify(root)
            self.assertTrue(any("malformed IHDR" in error for error in errors))

    def test_non_object_gpu_doctor_returns_a_verifier_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            (root / "forge-gpu-doctor.json").write_text("[]\n")
            self.rebind(root, "forge-gpu-doctor.json")
            errors = VERIFIER.verify(root)
            self.assertTrue(any("GPU doctor evidence must be an object" in error for error in errors))

    def test_non_object_receipt_result_and_transcript_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt_root = Path(temporary) / "receipt"
            receipt_root.mkdir()
            (receipt_root / "receipt.json").write_text("[]\n")
            self.assertEqual(
                VERIFIER.verify(receipt_root), ["receipt.json must contain an object"]
            )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            (root / "compute-run1.json").write_text("[]\n")
            self.rebind(root, "compute-run1.json")
            errors = VERIFIER.verify(root)
            self.assertTrue(any("probe result must be an object" in error for error in errors))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            rows = (root / "mcp-transcript.jsonl").read_text().splitlines()
            rows[1] = "[]"
            (root / "mcp-transcript.jsonl").write_text("\n".join(rows) + "\n")
            self.rebind(root, "mcp-transcript.jsonl")
            errors = VERIFIER.verify(root)
            self.assertTrue(any("all-four" in error for error in errors))

    def test_release_build_contract_rejects_missing_threejs(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            values = {
                "CMAKE_HOME_DIRECTORY": str(ROOT), "CMAKE_BUILD_TYPE": "Release",
                "PULP_ENABLE_GPU": "ON", "PULP_ENABLE_SCENE3D": "ON",
                "PULP_ENABLE_THREEJS_RUNTIME": "ON", "PULP_ENABLE_JS": "ON",
                "PULP_JS_ENGINE": "v8", "PULP_BUILD_RUST_CLI": "ON",
                "PULP_RUST_CLI_PROFILE": "release", "PULP_HAS_THREEJS": "FALSE",
            }
            (build / "CMakeCache.txt").write_text(
                "\n".join(f"{key}:STRING={value}" for key, value in values.items())
            )
            with self.assertRaisesRegex(RECORDER.AcceptanceError, "PULP_HAS_THREEJS=TRUE"):
                RECORDER.require_release_pulp_build(build, ROOT)

    def test_missing_path_claim_must_bind_executed_canary_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            receipt["forge_downstream"]["missing_path_canaries"]["threejs"][
                "mcp_positive_response_id"
            ] = 2
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("missing-path" in error for error in errors))

    def test_forge_source_allows_only_exact_sdk_ref_overlay(self):
        responses = {
            ("rev-parse", "HEAD"): VERIFIER.EXPECTED_FORGE_REVISION,
            ("config", "--get", "remote.origin.url"): "git@github.com:Generous-Corp/forge.git",
            ("diff", "--cached", "--name-only"): "",
            ("diff", "--name-only"): "PULP_SDK_REF",
            ("ls-files", "--others", "--exclude-standard"): "",
            ("rev-parse", f"{VERIFIER.EXPECTED_FORGE_REVISION}:PULP_SDK_REF"): "a" * 40,
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "PULP_SDK_REF").write_text("b" * 40 + "\n")
            with mock.patch.object(
                RECORDER.provenance, "_git_text",
                side_effect=lambda _root, *args: responses[args],
            ):
                identity = RECORDER.forge_source_identity(root, "b" * 40)
            self.assertTrue(identity["all_other_tracked_files_clean"])


def subprocess_text(command: list[str], cwd: Path) -> str:
    import subprocess
    return subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


if __name__ == "__main__":
    unittest.main()
