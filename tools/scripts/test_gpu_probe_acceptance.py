#!/usr/bin/env python3
"""Portable mutation tests for A2 structural proof and fresh-run certification."""

from __future__ import annotations

import binascii
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys
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
darwin_mutation_proof = unittest.skipUnless(
    sys.platform == "darwin", "requires macOS kqueue/renameatx_np mutation proof"
)


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
    def publish(self, staging: Path, output: Path) -> None:
        claim = RECORDER.retain_staged_evidence(staging)
        try:
            RECORDER.publish_receipt_directory_no_replace(staging, output, claim)
        finally:
            claim.close()

    def test_mcp_partial_line_cannot_escape_the_response_deadline(self):
        class Claim:
            @staticmethod
            def assert_current():
                return None

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "partial-mcp"
            executable.write_text("#!/bin/sh\nprintf '{'\nsleep 10\n")
            executable.chmod(0o755)
            session = RECORDER.McpSession(executable, root, {}, Claim())
            with mock.patch.object(RECORDER, "MCP_RESPONSE_TIMEOUT_SECONDS", 0.05):
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "response exceeded"
                ):
                    session.request("initialize")
            session.close()
            self.assertIsNotNone(session.process.returncode)

    def test_mcp_rejects_non_object_and_boolean_id_responses(self):
        class Claim:
            @staticmethod
            def assert_current():
                return None

        for response in ([], {"jsonrpc": "2.0", "id": True, "result": {}}):
            with self.subTest(response=response), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                session = RECORDER.McpSession(Path("/bin/cat"), root, {}, Claim())
                try:
                    with mock.patch.object(
                        RECORDER.provenance,
                        "read_bounded_process_line",
                        return_value=json.dumps(response) + "\n",
                    ):
                        with self.assertRaisesRegex(
                            RECORDER.AcceptanceError, "incoherent response"
                        ):
                            session.request("initialize")
                finally:
                    session.close()

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
        tree_claim = {
            "method": "retained-complete-tree-vnode-and-descriptor-v1",
            "file_count": 3,
            "bytes": 3,
            "manifest_sha256": "a" * 64,
        }
        manifest_sha256, skia_assets = VERIFIER._pinned_provider_asset_digests(
            "Skia"
        )
        _manifest_sha256, v8_assets = VERIFIER._pinned_provider_asset_digests("V8")
        skia_asset = sorted(skia_assets)[0]
        v8_asset = sorted(v8_assets)[0]
        skia_stamp_sha256 = hashlib.sha256(
            (skia_asset + "\n").encode()
        ).hexdigest()
        v8_stamp_sha256 = hashlib.sha256((v8_asset + "\n").encode()).hexdigest()
        providers = {
            "skia_dawn": {
                "root_role": "resolved-skia-dawn-provider-root",
                "resolution": "exact CMake/Ninja Skia layout plus pinned asset generation",
                "root_authority": {
                    "method": "pinned-release-asset-stamp-and-exact-layout-v1",
                    "dependency": "Skia",
                    "dependency_manifest_path": VERIFIER.PROVIDER_MANIFEST_PATH,
                    "dependency_manifest_sha256": manifest_sha256,
                    "generation_stamp_path": ".skia-asset-sha256",
                    "generation_asset_sha256": skia_asset,
                    "generation_stamp_sha256": skia_stamp_sha256,
                    "top_level_entries": [
                        ".skia-asset-sha256", "build", "include"
                    ],
                    "cache_authority": "SKIA_DIR",
                    "provider_layout": "build/<platform>-gpu/lib/Release",
                    "consumed_skia_archives": [
                        "build/mac-gpu/lib/Release/libskia.a"
                    ],
                    "consumed_top_level_entries": ["build"],
                },
                "tree_claim": tree_claim,
                "required_members": [
                    {"path": ".skia-asset-sha256", "git_blob_sha1": "0" * 40,
                     "sha256": skia_stamp_sha256, "bytes": 65},
                    {"path": "lib/libskia.a", "git_blob_sha1": "1" * 40,
                     "sha256": "1" * 64, "bytes": 1},
                    {"path": "lib/libdawn_combined.a", "git_blob_sha1": "2" * 40,
                     "sha256": "2" * 64, "bytes": 1},
                ],
            },
            "v8": {
                "root_role": "resolved-v8-provider-root",
                "resolution": "exact CMake V8 runtime layout plus pinned asset generation",
                "root_authority": {
                    "method": "pinned-release-asset-stamp-and-exact-layout-v1",
                    "dependency": "V8",
                    "dependency_manifest_path": VERIFIER.PROVIDER_MANIFEST_PATH,
                    "dependency_manifest_sha256": manifest_sha256,
                    "generation_stamp_path": ".v8-asset-sha256",
                    "generation_asset_sha256": v8_asset,
                    "generation_stamp_sha256": v8_stamp_sha256,
                    "top_level_entries": [".v8-asset-sha256", "include", "lib"],
                    "cache_authority": "V8_RUNTIME_LIBRARY",
                    "provider_layout": "lib/<runtime>",
                    "consumed_runtime": "lib/libv8.dylib",
                },
                "tree_claim": tree_claim,
                "required_members": [
                    {"path": ".v8-asset-sha256", "git_blob_sha1": "5" * 40,
                     "sha256": v8_stamp_sha256, "bytes": 65},
                    {"path": "include/v8.h", "git_blob_sha1": "3" * 40,
                     "sha256": "3" * 64, "bytes": 1},
                    {"path": "lib/libv8.dylib", "git_blob_sha1": "4" * 40,
                     "sha256": "4" * 64, "bytes": 1},
                ],
            },
        }
        source_disposition = "not-present-at-configured-or-resolved-sibling"
        render_provider_claim = {
            "method": "retained-resolved-render-provider-trees-v3",
            "skia_source_disposition": source_disposition,
            "providers": providers,
            "manifest_sha256": hashlib.sha256(
                json.dumps(
                    {
                        "providers": providers,
                        "skia_source_disposition": source_disposition,
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode()
            ).hexdigest(),
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
                "install_prefix_initial_state": "absent-and-atomically-claimed",
                "install_prefix_claim_method": (
                    "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
                ),
                "install_prefix_claim": {"device": 1, "inode": 2},
                "cmake_cache_sha256": "7" * 64, "build_info_sha256": "8" * 64,
                "source_tree_claim": {
                    "method": "retained-git-tree-vnode-and-descriptor-v1",
                    "revision": head,
                    "root_tree": RECORDER.provenance._git_text(
                        ROOT, "rev-parse", f"{head}^{{tree}}"
                    ),
                    "overlay_paths": [],
                    "excluded_gitlinks": VERIFIER._expected_gitlinks(ROOT, head),
                    "regular_or_symlink_files": 1,
                    "retained_directories": 1,
                },
                "build_input_claim": {
                    "method": "regenerated-cmake-ninja-input-descriptor-v1",
                    "file_count": 3,
                    "manifest_sha256": "9" * 64,
                    "build_targets": [
                        "pulp-rust-cli", "pulp-cli", "pulp-mcp"
                    ],
                    "forced_clean_before_build": True,
                },
                "render_provider_input_claim": render_provider_claim,
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
                       "build_output_sha256": str(index) * 64,
                       "build_output_bytes": 1}
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
                "source_tree_claim": {
                    "method": "retained-git-tree-vnode-and-descriptor-v1",
                    "revision": VERIFIER.EXPECTED_FORGE_REVISION,
                    "root_tree": VERIFIER.EXPECTED_FORGE_ROOT_TREE,
                    "overlay_paths": ["PULP_SDK_REF"],
                    "excluded_gitlinks": {},
                    "regular_or_symlink_files": 1,
                    "retained_directories": 1,
                },
                "pulp_sdk_tree_claim": tree_claim,
                "build_input_claim": {
                    "method": "regenerated-cmake-ninja-input-descriptor-v1",
                    "file_count": 3,
                    "manifest_sha256": "b" * 64,
                    "build_targets": ["ForgeModular_Standalone"],
                    "forced_clean_before_build": False,
                },
                "build_directory_claim_method": (
                    "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
                ),
                "build_directory_claim": {"device": 1, "inode": 3},
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
                "bundle_tree_claim": tree_claim,
                "screenshot_metrics": VERIFIER._png_metrics(screenshot),
            },
            "additional_pulp_path_canaries": {
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
            "acceptance": {
                "terminal_status": VERIFIER.OFFLINE_STRUCTURAL_STATUS,
                "all_four_installed_cli": "pass",
                "all_four_installed_mcp": "pass", "seeded_negative_controls": "pass",
                "forge_modular_and_additional_pulp_path_canaries": "pass",
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

    def test_synthetic_v2_fixture_passes_structural_validation_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            self.assertEqual(VERIFIER.verify(root), [])

    def test_synthetic_v2_fixture_cannot_request_terminal_certification(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            errors = VERIFIER.verify(root, require_terminal=True)
            self.assertIn(VERIFIER.OFFLINE_TERMINAL_ERROR, errors)
            self.assertIsNone(getattr(RECORDER, "certify_fresh_recording", None))
            directory_shell = object.__new__(RECORDER.RetainedDirectoryClaim)
            directory_shell.closed = True
            tree_shell = object.__new__(RECORDER.provenance.RetainedTreeClaim)
            tree_shell.closed = True
            shells = (
                directory_shell,
                tree_shell,
                object.__new__(RECORDER.provenance.RetainedClaimSet),
            )
            self.assertEqual(len(shells), 3)
            self.assertNotIn(
                "fresh_recorder_certification", RECORDER.SCRIPT_DIR.joinpath(
                    "gpu_probe_acceptance.py"
                ).read_text()
            )
            source = RECORDER.SCRIPT_DIR.joinpath(
                "gpu_probe_acceptance.py"
            ).read_text()
            after_claim_closure = source.rsplit(
                "output_parent_claim.close()", 1
            )[1].split("return 0", 1)[0]
            self.assertIn("structural-evidence-written", after_claim_closure)
            self.assertNotRegex(after_claim_closure, r"(?i)terminal|certif")

    def test_stale_parent_cannot_request_terminal_certification(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            parent = subprocess_text(["git", "rev-parse", "HEAD^"], ROOT)
            receipt["integration_head"] = parent
            receipt["source_identity"]["revision"] = parent
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root, require_terminal=True)
            self.assertTrue(any(
                "integration_head to equal live checkout HEAD" in error
                for error in errors
            ))

    def test_v2_source_binding_includes_retained_tree_helper(self):
        helper = "tools/scripts/gpu_trace_overhead_acceptance.py"
        self.assertIn(helper, VERIFIER.EXPECTED_SOURCE_BLOBS_V2)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.v2_fixture(root)
            original = VERIFIER._checkout_blobs

            def drifted_checkout(paths):
                blobs = original(paths)
                blobs[helper] = "f" * 40
                return blobs

            with mock.patch.object(
                VERIFIER, "_checkout_blobs", side_effect=drifted_checkout
            ):
                errors = VERIFIER.verify(root)
            self.assertIn(
                f"current checkout source blob drift for {helper}", errors
            )

    def test_v2_source_binding_includes_artifact_publication_boundary(self):
        boundary = {
            "tools/cli/gpu_artifact_publication.cpp",
            "tools/cli/gpu_artifact_publication.hpp",
        }
        self.assertTrue(boundary.issubset(VERIFIER.EXPECTED_SOURCE_BLOBS_V2))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = VERIFIER._checkout_blobs

            def checkout_as_revision(_revision, paths):
                return original(paths)

            # The boundary may still be uncommitted when this focused mutation
            # test runs locally. Model the pending integration revision from the
            # live checkout; the verifier's checkout-drift assertion remains the
            # behavior under test below.
            with mock.patch.object(
                VERIFIER, "_git_blobs", side_effect=checkout_as_revision
            ):
                self.v2_fixture(root)

            def drifted_checkout(paths):
                blobs = original(paths)
                for path in boundary:
                    blobs[path] = "f" * 40
                return blobs

            with (
                mock.patch.object(
                    VERIFIER, "_git_blobs", side_effect=checkout_as_revision
                ),
                mock.patch.object(
                    VERIFIER, "_checkout_blobs", side_effect=drifted_checkout
                ),
            ):
                errors = VERIFIER.verify(root)
            for path in boundary:
                self.assertIn(
                    f"current checkout source blob drift for {path}", errors
                )

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
            receipt["install_provenance"]["install_prefix_initial_state"] = "preexisting"
            receipt["forge_downstream"]["bundle_build_info"]["pulp_sdk"] = "wrong"
            receipt["acceptance"]["terminal_status"] = "draft"
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("accepted plan revision" in error for error in errors))
            self.assertTrue(any("installed CLI provenance" in error for error in errors))
            self.assertTrue(any("install prefix was not fresh" in error for error in errors))
            self.assertTrue(any("Forge bundle stamp" in error for error in errors))

    def test_v2_rejects_empty_installed_source_stamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            receipt["install_provenance"]["build_info"]["kGitSha"] = ""
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("installed CLI provenance" in error for error in errors))

    def test_v2_rejects_missing_or_mismatched_build_output_bytes(self):
        for value in (None, False, 2):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                receipt = self.v2_fixture(root)
                binary = receipt["binaries"]["installed_mcp"]
                if value is None:
                    del binary["build_output_bytes"]
                else:
                    binary["build_output_bytes"] = value
                (root / "receipt.json").write_text(json.dumps(receipt))
                errors = VERIFIER.verify(root)
                self.assertTrue(
                    any("refreshed build output" in error for error in errors)
                )

    def test_v2_rejects_unretained_source_build_and_forge_bundle_claims(self):
        mutations = (
            (
                lambda receipt: receipt["install_provenance"].pop(
                    "source_tree_claim"
                ),
                "Pulp source was not retained",
            ),
            (
                lambda receipt: receipt["install_provenance"][
                    "build_input_claim"
                ].update({"build_targets": []}),
                "forced clean",
            ),
            (
                lambda receipt: receipt["forge_downstream"].pop(
                    "source_tree_claim"
                ),
                "Forge source was not retained",
            ),
            (
                lambda receipt: receipt["install_provenance"].pop(
                    "render_provider_input_claim"
                ),
                "render providers",
            ),
            (
                lambda receipt: receipt["forge_downstream"].pop(
                    "pulp_sdk_tree_claim"
                ),
                "consumed Pulp SDK",
            ),
            (
                lambda receipt: receipt["forge_downstream"].pop(
                    "build_input_claim"
                ),
                "Forge build inputs",
            ),
            (
                lambda receipt: receipt["forge_downstream"].pop(
                    "bundle_tree_claim"
                ),
                "Forge bundle was not retained",
            ),
        )
        for mutate, expected in mutations:
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                receipt = self.v2_fixture(root)
                mutate(receipt)
                (root / "receipt.json").write_text(json.dumps(receipt))
                errors = VERIFIER.verify(root)
                self.assertTrue(any(expected in error for error in errors), errors)

    def test_v2_rejects_omitted_consumed_adjacent_skia_source_provider(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            claim = receipt["install_provenance"]["render_provider_input_claim"]
            claim["skia_source_disposition"] = (
                "retained-complete-adjacent-source-tree"
            )
            claim["manifest_sha256"] = hashlib.sha256(
                json.dumps(
                    {
                        "providers": claim["providers"],
                        "skia_source_disposition": claim[
                            "skia_source_disposition"
                        ],
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode()
            ).hexdigest()
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("sealed provider" in error for error in errors), errors)

    def test_v2_rejects_forged_render_provider_root_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            claim = receipt["install_provenance"]["render_provider_input_claim"]
            claim["providers"]["skia_dawn"]["root_role"] = "caller-selected-root"
            claim["manifest_sha256"] = hashlib.sha256(
                json.dumps(
                    {
                        "providers": claim["providers"],
                        "skia_source_disposition": claim[
                            "skia_source_disposition"
                        ],
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode()
            ).hexdigest()
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("exact root provenance" in error for error in errors))

    def test_v2_rejects_render_provider_with_unrelated_monorepo_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            claim = receipt["install_provenance"]["render_provider_input_claim"]
            claim["providers"]["skia_dawn"]["root_authority"][
                "top_level_entries"
            ].append("unrelated-product-gpu")
            claim["manifest_sha256"] = hashlib.sha256(
                json.dumps(
                    {
                        "providers": claim["providers"],
                        "skia_source_disposition": claim[
                            "skia_source_disposition"
                        ],
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode()
            ).hexdigest()
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("pinned generation authority" in error for error in errors))

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

    def test_generated_paths_require_fresh_distinct_install_and_forge_trees(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "pulp-build"
            build.mkdir()
            prefix = root / "pulp-prefix"
            forge = root / "forge-build"
            output = root / "receipt"
            RECORDER.validate_generated_paths(build, prefix, forge, output)

            prefix.mkdir()
            with self.assertRaisesRegex(RECORDER.AcceptanceError, "install-prefix"):
                RECORDER.validate_generated_paths(build, prefix, forge, output)
            prefix.rmdir()

            with self.assertRaisesRegex(RECORDER.AcceptanceError, "must not overlap"):
                RECORDER.validate_generated_paths(
                    build, build / "prefix", forge, output
                )

    @darwin_mutation_proof
    def test_receipt_publication_claims_directory_without_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            (staging / "evidence.json").write_text("{}\n", encoding="utf-8")
            (staging / "receipt.json").write_text("{}\n", encoding="utf-8")

            raced_output = root / "raced-output"
            raced_output.mkdir()
            inode = raced_output.stat().st_ino
            with self.assertRaisesRegex(RECORDER.AcceptanceError, "appeared"):
                self.publish(staging, raced_output)
            self.assertEqual(raced_output.stat().st_ino, inode)
            self.assertEqual(list(raced_output.iterdir()), [])

            output = root / "published"
            self.publish(staging, output)
            self.assertEqual(
                sorted(path.name for path in output.iterdir()),
                ["evidence.json", "receipt.json"],
            )

    @darwin_mutation_proof
    def test_receipt_publication_rejects_named_directory_swap_before_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            (staging / "evidence.json").write_text("{}\n", encoding="utf-8")
            (staging / "receipt.json").write_text("{}\n", encoding="utf-8")
            output = root / "published"
            moved = root / "moved-published"
            real_link = RECORDER.os.link
            swapped = False

            def swap_after_first_link(*args, **kwargs):
                nonlocal swapped
                result = real_link(*args, **kwargs)
                if not swapped:
                    output.rename(moved)
                    output.mkdir()
                    swapped = True
                return result

            with mock.patch.object(
                RECORDER.os, "link", side_effect=swap_after_first_link
            ):
                with self.assertRaisesRegex(RECORDER.AcceptanceError, "no longer names"):
                    self.publish(staging, output)
            self.assertFalse((moved / "receipt.json").exists())
            self.assertEqual(list(output.iterdir()), [])

    @darwin_mutation_proof
    def test_receipt_publication_rejects_parent_directory_swap_before_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            (staging / "evidence.json").write_text("{}\n", encoding="utf-8")
            (staging / "receipt.json").write_text("{}\n", encoding="utf-8")
            parent = root / "publication-parent"
            parent.mkdir()
            output = parent / "published"
            moved_parent = root / "moved-publication-parent"
            real_link = RECORDER.os.link
            swapped = False

            def swap_parent_after_first_link(*args, **kwargs):
                nonlocal swapped
                result = real_link(*args, **kwargs)
                if not swapped:
                    parent.rename(moved_parent)
                    parent.mkdir()
                    swapped = True
                return result

            with mock.patch.object(
                RECORDER.os, "link", side_effect=swap_parent_after_first_link
            ):
                with self.assertRaisesRegex(RECORDER.AcceptanceError, "no longer names"):
                    self.publish(staging, output)
            self.assertFalse((moved_parent / "published" / "receipt.json").exists())
            self.assertEqual(list(parent.iterdir()), [])

    @darwin_mutation_proof
    def test_retained_output_parent_rejects_swap_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "publication-parent"
            parent.mkdir()
            staging = root / "staging"
            staging.mkdir()
            (staging / "receipt.json").write_text("{}\n", encoding="utf-8")
            staging_claim = RECORDER.retain_staged_evidence(staging)
            parent_claim = RECORDER.retain_existing_directory(
                parent, "output-parent"
            )
            moved = root / "moved-parent"
            try:
                parent.rename(moved)
                moved.rename(parent)
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "mutation event"
                ):
                    RECORDER.publish_receipt_directory_no_replace(
                        staging, parent / "published", staging_claim, parent_claim
                    )
            finally:
                parent_claim.close()
                staging_claim.close()

    @darwin_mutation_proof
    def test_fresh_directory_claim_rejects_staging_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "prefix"
            real_rename = RECORDER.renameat_no_replace

            def swap_then_publish(source_parent, source_name, destination_parent,
                                  destination_name):
                os.rename(
                    source_name, source_name + ".original",
                    src_dir_fd=source_parent, dst_dir_fd=source_parent,
                )
                os.mkdir(source_name, dir_fd=source_parent)
                return real_rename(
                    source_parent, source_name,
                    destination_parent, destination_name,
                )

            with mock.patch.object(
                RECORDER, "renameat_no_replace", side_effect=swap_then_publish
            ):
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "created inode"
                ):
                    RECORDER.claim_fresh_directory(path, "install-prefix")
            self.assertTrue(path.is_dir())

    @darwin_mutation_proof
    def test_receipt_publication_rejects_staged_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            evidence = staging / "evidence.json"
            evidence.write_text('{"verified":true}\n', encoding="utf-8")
            (staging / "receipt.json").write_text("{}\n", encoding="utf-8")
            output = root / "published"
            real_link = RECORDER.os.link
            swapped = False

            def swap_source_before_first_link(*args, **kwargs):
                nonlocal swapped
                if not swapped:
                    evidence.rename(staging / "original-evidence.json")
                    evidence.write_text('{"verified":false}\n', encoding="utf-8")
                    swapped = True
                return real_link(*args, **kwargs)

            with mock.patch.object(
                RECORDER.os, "link", side_effect=swap_source_before_first_link
            ):
                with self.assertRaisesRegex(RECORDER.AcceptanceError, "identity changed"):
                    self.publish(staging, output)
            self.assertFalse((output / "receipt.json").exists())
            self.assertFalse((output / "evidence.json").exists())

    @darwin_mutation_proof
    def test_receipt_publication_rejects_in_place_receipt_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            (staging / "evidence.json").write_text("{}\n", encoding="utf-8")
            (staging / "receipt.json").write_text(
                '{"verified":true}\n', encoding="utf-8"
            )
            output = root / "published"
            real_link = RECORDER.os.link

            def mutate_linked_receipt(source, destination, **kwargs):
                result = real_link(source, destination, **kwargs)
                if source == "receipt.json":
                    (output / "receipt.json").write_text(
                        '{"verified":false}\n', encoding="utf-8"
                    )
                return result

            with mock.patch.object(
                RECORDER.os, "link", side_effect=mutate_linked_receipt
            ):
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "mutation event|bytes differ"
                ):
                    self.publish(staging, output)
            self.assertFalse((output / "receipt.json").exists())

    @darwin_mutation_proof
    def test_receipt_publication_rejects_post_verification_staging_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            evidence = staging / "evidence.json"
            evidence.write_text('{"verified":true}\n', encoding="utf-8")
            receipt = staging / "receipt.json"
            receipt.write_text(
                '{"raw_sha256":{"evidence.json":"verified"}}\n',
                encoding="utf-8",
            )
            claim = RECORDER.retain_staged_evidence(staging)
            try:
                evidence.write_text('{"verified":false}\n', encoding="utf-8")
                receipt.write_text(
                    '{"raw_sha256":{"evidence.json":"forged"}}\n',
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "mutation event|changed"
                ):
                    RECORDER.publish_receipt_directory_no_replace(
                        staging, root / "published", claim
                    )
            finally:
                claim.close()
            self.assertFalse((root / "published" / "receipt.json").exists())

    def test_claimed_install_prefix_rejects_path_substitution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            prefix.mkdir()
            descriptor = RECORDER.os.open(prefix, RECORDER.directory_open_flags())
            claim = RECORDER.directory_identity(
                RECORDER.os.fstat(descriptor), "install-prefix"
            )
            moved = root / "moved-prefix"
            prefix.rename(moved)
            prefix.mkdir()
            try:
                with self.assertRaisesRegex(RECORDER.AcceptanceError, "no longer names"):
                    RECORDER.assert_directory_claim(
                        prefix, claim, "install-prefix", descriptor
                    )
            finally:
                RECORDER.os.close(descriptor)

    def test_retained_executable_claim_rejects_child_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            binary = prefix / "bin/pulp"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"claimed executable")
            descriptor = RECORDER.os.open(prefix, RECORDER.directory_open_flags())
            claim = RECORDER.RetainedDirectoryClaim(
                prefix,
                descriptor,
                RECORDER.directory_identity(
                    RECORDER.os.fstat(descriptor), "install-prefix"
                ),
                "install-prefix",
            )
            claim.bind_file(binary, "installed Rust CLI", RECORDER.sha256(binary))
            moved = binary.with_name("pulp-original")
            binary.rename(moved)
            binary.write_bytes(b"substituted executable")
            try:
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "retained file claim"
                ):
                    claim.assert_current()
            finally:
                claim.close()

    @darwin_mutation_proof
    def test_retained_build_outputs_reject_replace_and_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            prefix.mkdir()
            build = root / "build"
            for index, (relative, _installed) in enumerate(
                RECORDER.BINARY_PATHS.values()
            ):
                path = build / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(f"build-output-{index}".encode())
                os.chmod(path, 0o755)
            descriptor = os.open(prefix, RECORDER.directory_open_flags())
            claim = RECORDER.RetainedDirectoryClaim(
                prefix,
                descriptor,
                RECORDER.directory_identity(os.fstat(descriptor), "install-prefix"),
                "install-prefix",
            )
            try:
                claim.seal()
                RECORDER.bind_build_outputs(build, claim)
                target = build / RECORDER.BINARY_PATHS["installed_rust_cli"][0]
                moved = root / "moved-pulp"
                payload = target.read_bytes()
                target.rename(moved)
                target.write_bytes(payload)
                os.chmod(target, 0o755)
                target.unlink()
                moved.rename(target)
                with self.assertRaisesRegex(
                    RECORDER.AcceptanceError, "mutation event"
                ):
                    claim.assert_current()
            finally:
                claim.close()

    @darwin_mutation_proof
    def test_bounded_launch_rechecks_child_inode_after_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            binary = prefix / "bin/pulp"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"claimed executable")
            descriptor = RECORDER.os.open(prefix, RECORDER.directory_open_flags())
            claim = RECORDER.RetainedDirectoryClaim(
                prefix,
                descriptor,
                RECORDER.directory_identity(
                    RECORDER.os.fstat(descriptor), "install-prefix"
                ),
                "install-prefix",
            )
            claim.bind_file(binary, "installed Rust CLI", RECORDER.sha256(binary))
            claim.seal()

            def swap_during_launch(*_args, **_kwargs):
                original = binary.with_name("pulp-original")
                binary.rename(original)
                binary.write_bytes(b"substituted executable")
                binary.unlink()
                original.rename(binary)
                return RECORDER.subprocess.CompletedProcess([str(binary)], 0, "", "")

            try:
                with mock.patch.object(
                    RECORDER.subprocess, "run", side_effect=swap_during_launch
                ):
                    with self.assertRaisesRegex(
                        RECORDER.AcceptanceError, "mutation event"
                    ):
                        RECORDER.run_bounded(
                            [str(binary)], cwd=root, environment={}, timeout=1,
                            directory_claim=claim,
                        )
            finally:
                claim.close()

    @darwin_mutation_proof
    def test_bounded_launch_detects_restored_ancestor_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            container = root / "container"
            prefix = container / "prefix"
            binary = prefix / "bin/pulp"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"claimed executable")
            descriptor = RECORDER.os.open(prefix, RECORDER.directory_open_flags())
            claim = RECORDER.RetainedDirectoryClaim(
                prefix,
                descriptor,
                RECORDER.directory_identity(
                    RECORDER.os.fstat(descriptor), "install-prefix"
                ),
                "install-prefix",
            )
            claim.bind_file(binary, "installed Rust CLI", RECORDER.sha256(binary))
            claim.seal()

            def swap_ancestor_during_launch(*_args, **_kwargs):
                moved = root / "moved-container"
                container.rename(moved)
                substitute = container / "prefix/bin/pulp"
                substitute.parent.mkdir(parents=True)
                substitute.write_bytes(b"substituted executable")
                shutil.rmtree(container)
                moved.rename(container)
                return RECORDER.subprocess.CompletedProcess([str(binary)], 0, "", "")

            try:
                with mock.patch.object(
                    RECORDER.subprocess,
                    "run",
                    side_effect=swap_ancestor_during_launch,
                ):
                    with self.assertRaisesRegex(
                        RECORDER.AcceptanceError, "mutation event"
                    ):
                        RECORDER.run_bounded(
                            [str(binary)], cwd=root, environment={}, timeout=1,
                            directory_claim=claim,
                        )
            finally:
                claim.close()

    def test_additional_pulp_path_claim_must_bind_executed_canary_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            receipt["additional_pulp_path_canaries"]["threejs"][
                "mcp_positive_response_id"
            ] = 2
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("additional Pulp" in error for error in errors))

    def test_pulp_path_canaries_cannot_masquerade_as_forge_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self.v2_fixture(root)
            receipt["forge_downstream"]["missing_path_canaries"] = (
                receipt["additional_pulp_path_canaries"]
            )
            (root / "receipt.json").write_text(json.dumps(receipt))
            errors = VERIFIER.verify(root)
            self.assertTrue(any("must not be claimed as Forge" in error for error in errors))

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
