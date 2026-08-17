#!/usr/bin/env python3
"""Negative controls for the release artifact content watchdog."""

from __future__ import annotations

import importlib.util
import hashlib
import io
import json
import os
import re
import tarfile
import tempfile
import unittest
import zipfile
import sys
from dataclasses import replace
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("release_artifact_contents.py")
spec = importlib.util.spec_from_file_location("release_artifact_contents", SCRIPT)
assert spec and spec.loader
rac = importlib.util.module_from_spec(spec)
sys.modules["release_artifact_contents"] = rac
spec.loader.exec_module(rac)

VERSION = "9.8.7"
ROOT = SCRIPT.parents[2]
INSTALL_RULES = SCRIPT.parents[1] / "cmake" / "PulpInstallRules.cmake"
IMPORT_DESIGN_RUNTIME_MANIFEST = (
    SCRIPT.parents[1] / "import-design" / "browser_capture" / "runtime_manifest.txt"
)


def installed_sdk_targets(text: str | None = None) -> set[str]:
    if text is None:
        text = INSTALL_RULES.read_text(encoding="utf-8")
    bracket_comments = re.findall(
        r"#\[(=*)\[(.*?)\]\1\]",
        text,
        flags=re.DOTALL,
    )
    line_comments = re.findall(r"#[^\n]*", text)
    if any(
        re.search(r"\bPULP_SDK_TARGETS\b", comment, flags=re.IGNORECASE)
        for comment in [
            *(body for _, body in bracket_comments),
            *line_comments,
        ]
    ):
        raise AssertionError("PULP_SDK_TARGETS must not appear in comments")
    commands = re.findall(
        r"\b([a-z_]+)\s*\(([^)]*\bPULP_SDK_TARGETS\b[^)]*)\)",
        text,
        flags=re.IGNORECASE | re.DOTALL,
    )
    occurrence_count = len(
        re.findall(r"\bPULP_SDK_TARGETS\b", text, flags=re.IGNORECASE)
    )
    parsed_occurrence_count = sum(
        len(re.findall(r"\bPULP_SDK_TARGETS\b", body, flags=re.IGNORECASE))
        for _, body in commands
    )
    if parsed_occurrence_count != occurrence_count:
        raise AssertionError("a PULP_SDK_TARGETS use is no longer parseable")
    targets: set[str] = set()
    set_count = 0
    install_count = 0
    for command, body in commands:
        tokens = body.split()
        command = command.upper()
        if command == "SET" and tokens[0] == "PULP_SDK_TARGETS":
            set_count += 1
            operands = tokens[1:]
        elif (
            command == "LIST"
            and len(tokens) >= 2
            and tokens[0].upper() == "APPEND"
            and tokens[1] == "PULP_SDK_TARGETS"
        ):
            operands = tokens[2:]
        elif command == "FOREACH" and tokens == [
            "_sdk_target",
            "IN",
            "LISTS",
            "PULP_SDK_TARGETS",
        ]:
            continue
        elif (
            command == "INSTALL"
            and len(tokens) >= 2
            and tokens[0].upper() == "TARGETS"
            and tokens[1] == "${PULP_SDK_TARGETS}"
            and tokens.count("${PULP_SDK_TARGETS}") == 1
        ):
            install_count += 1
            continue
        else:
            raise AssertionError(
                f"unsupported PULP_SDK_TARGETS use in {command.lower()}()"
            )
        if not operands or any(
            re.fullmatch(r"pulp-[a-z0-9-]+", operand) is None
            for operand in operands
        ):
            raise AssertionError(
                "PULP_SDK_TARGETS mutations must contain only literal targets"
            )
        targets.update(operands)
    if set_count != 1:
        raise AssertionError("PULP_SDK_TARGETS must have exactly one canonical set()")
    if install_count != 1:
        raise AssertionError(
            "PULP_SDK_TARGETS must have exactly one canonical install() consumer"
        )
    return targets


def interface_library_targets_from_text(text: str) -> set[str]:
    text = re.sub(
        r"#\[(=*)\[.*?\]\1\]",
        "",
        text,
        flags=re.DOTALL,
    )
    text = re.sub(r"#[^\n]*", "", text)
    return set(
        re.findall(
            r"\badd_library\s*\(\s*(pulp-[a-z0-9-]+)\s+INTERFACE\b",
            text,
            flags=re.IGNORECASE,
        )
    )


def interface_library_targets() -> set[str]:
    targets: set[str] = set()
    definition_files = [
        ROOT / "CMakeLists.txt",
        *ROOT.rglob("CMakeLists.txt"),
        *(ROOT / "tools" / "cmake").rglob("*.cmake"),
    ]
    for path in definition_files:
        text = path.read_text(encoding="utf-8")
        targets.update(interface_library_targets_from_text(text))
    return targets


SOURCE_SHA = "a" * 40
FIXTURE_IMPORTER = b"fixture importer executable"
FIXTURE_CAPABILITIES = b'{"schema":"fixture.agent-capabilities.v1"}\n'
FIXTURE_CAPABILITIES_SCHEMA = json.dumps(
    {
        "type": "object",
        "additionalProperties": False,
        "required": ["schema"],
        "properties": {
            "schema": {"const": "fixture.agent-capabilities.v1"}
        },
    }
).encode()
FIXTURE_HANDOFF_SCHEMA = (
    ROOT / "docs/status/agent-capability-handoff.schema.json"
).read_bytes()


def standalone_manifest_payload() -> bytes:
    document = {
        "schema": "dev.pulp.control/artifact-manifest@1",
        "schema_version": 1,
        "profile": "developer-local",
        "target": "pulp-control-standalone-host",
        "product_name": "Pulp Control Standalone Host",
        "bundle_id": "dev.pulp.control-standalone-host",
        "build_id": "build:0123456789abcdef0123456789abcdef",
        "registry_digest": rac._control_registry_digest(),
        "endpoint_included": True,
        "unsafe_runtime_eval_acknowledged": False,
        "permission_terms": list(rac.CONTROL_MANIFEST_PERMISSION_TERMS),
        "capabilities": list(rac.CONTROL_STANDALONE_HOST_CAPABILITIES),
    }
    return rac._canonical_standalone_manifest(document)


def standalone_host_payload() -> bytes:
    digest = hashlib.sha256(standalone_manifest_payload()).hexdigest()
    return (
        "PULP_STANDALONE_COMPONENT_V1\0"
        "PULP_INSPECT_SHIPPING_MANIFEST_V1\0"
        "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1\0"
        f"PULP_CONTROL_MANIFEST_SHA256_{digest}_V1\0"
        "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1\0"
        "PULP_INSPECT_CAPABILITY_STATE_READ_V1"
    ).encode()


def member_payload(name: str, platform: str = "linux-x64") -> bytes:
    if name in {
        rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST,
        rac.CONTROL_STANDALONE_HOST_SDK_MANIFEST,
    }:
        return standalone_manifest_payload()
    if name in {
        rac.CONTROL_STANDALONE_HOST_CLI_MEMBER,
        rac.CONTROL_STANDALONE_HOST_SDK_MEMBER,
    }:
        return standalone_host_payload()
    if name == "pulp-sdk/version.txt":
        return f"{VERSION}\n".encode()
    if name == "pulp-sdk/sdk_build_type.txt":
        return b"Release\n"
    if name == "pulp-sdk/sdk-provenance.json":
        return (
            json.dumps(
                {
                    "schema": "pulp.sdk-provenance.v1",
                    "kind": "release",
                    "profile": "official-release",
                    "distribution_eligible": True,
                    "sdk_version": VERSION,
                    "source_git_ref": f"v{VERSION}",
                    "source_git_sha": SOURCE_SHA,
                    "source_git_dirty": False,
                    "platform": platform,
                    "build_type": "Release",
                    "features": {"audio_probes": False, "inspector": True},
                }
            )
            + "\n"
        ).encode()
    if name == "pulp-sdk/include/pulp/runtime/build_info.hpp":
        return (
            "#pragma once\n"
            "#include <string_view>\n"
            "namespace pulp::runtime {\n"
            'inline constexpr std::string_view kBuildType = "Release";\n'
            'inline constexpr std::string_view kBuildIso8601 = "2026-08-10T00:00:00Z";\n'
            f'inline constexpr std::string_view kGitSha = "{SOURCE_SHA[:7]}";\n'
            "inline constexpr bool kGitDirty = false;\n"
            f'inline constexpr std::string_view kSdkVersion = "{VERSION}";\n'
            f'inline constexpr std::string_view kStampLabel = "{VERSION} fixture";\n'
            "}\n"
        ).encode()
    importer = (
        "pulp-sdk/bin/pulp-import-design.exe"
        if platform.startswith("windows-")
        else "pulp-sdk/bin/pulp-import-design"
    )
    if name == importer:
        return FIXTURE_IMPORTER
    if name == "pulp-sdk/share/pulp/agent-capabilities.json":
        return FIXTURE_CAPABILITIES
    if name == "pulp-sdk/share/pulp/agent-capabilities.schema.json":
        return FIXTURE_CAPABILITIES_SCHEMA
    if name == "pulp-sdk/share/pulp/agent-capability-handoff.schema.json":
        return FIXTURE_HANDOFF_SCHEMA
    if name == "pulp-sdk/share/pulp/agent-capability-handoff.json":
        document = {
            "$schema": "agent-capability-handoff.schema.json",
            "schema": "pulp.agent-capability-handoff.v1",
            "sdk_source_sha": SOURCE_SHA,
            "platform": platform,
            "schemas": {
                "handoff": {
                    "path": "share/pulp/agent-capability-handoff.schema.json",
                    "sha256": hashlib.sha256(FIXTURE_HANDOFF_SCHEMA).hexdigest(),
                },
                "agent_capabilities": {
                    "path": "share/pulp/agent-capabilities.schema.json",
                    "sha256": hashlib.sha256(FIXTURE_CAPABILITIES_SCHEMA).hexdigest(),
                },
            },
            "importer": {
                "path": importer.removeprefix("pulp-sdk/"),
                "sha256": hashlib.sha256(FIXTURE_IMPORTER).hexdigest(),
                "runtime": [
                    {
                        "path": member.removeprefix("pulp-sdk/"),
                        "sha256": hashlib.sha256(
                            member_payload(member, platform)
                        ).hexdigest(),
                    }
                    for member in sorted(
                        rac.sdk_import_design_runtime_members(
                            rac.DEFAULT_MATRIX, VERSION
                        )
                    )
                ],
            },
            "agent_capabilities": {
                "path": "share/pulp/agent-capabilities.json",
                "sha256": hashlib.sha256(FIXTURE_CAPABILITIES).hexdigest(),
                "content": json.loads(FIXTURE_CAPABILITIES),
            },
        }
        return (json.dumps(document) + "\n").encode()
    return b"fixture"


def write_archive(
    path: Path,
    members: set[str],
    *,
    as_zip: bool,
    platform: str = "linux-x64",
    mode_overrides: dict[str, int] | None = None,
    payload_overrides: dict[str, bytes] | None = None,
) -> None:
    mode_overrides = mode_overrides or {}
    payload_overrides = payload_overrides or {}
    if as_zip:
        with zipfile.ZipFile(path, "w") as archive:
            for name in sorted(members):
                info = zipfile.ZipInfo(name)
                info.external_attr = mode_overrides.get(name, 0o755) << 16
                archive.writestr(
                    info, payload_overrides.get(name, member_payload(name, platform))
                )
        return
    with tarfile.open(path, "w:gz") as archive:
        for name in sorted(members):
            data = payload_overrides.get(name, member_payload(name, platform))
            info = tarfile.TarInfo(name)
            info.size = len(data)
            if name in {
                rac.CONTROL_STANDALONE_HOST_CLI_MEMBER,
                rac.CONTROL_STANDALONE_HOST_SDK_MEMBER,
            }:
                default_mode = 0o700
            elif name in {
                rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST,
                rac.CONTROL_STANDALONE_HOST_SDK_MANIFEST,
            }:
                default_mode = 0o600
            elif (
                "/bin/" in name
                or name
                in rac.cli_binary_members(platform, rac.DEFAULT_MATRIX, VERSION)
                or name
                in rac.sdk_binary_members(platform, rac.DEFAULT_MATRIX, VERSION)
            ):
                default_mode = 0o755
            else:
                default_mode = 0o644
            info.mode = mode_overrides.get(name, default_mode)
            archive.addfile(info, io.BytesIO(data))


def make_platform(root: Path, platform: str) -> tuple[set[str], set[str]]:
    cli = set(rac.cli_members(platform, rac.DEFAULT_MATRIX, VERSION))
    sdk = set(rac.required_sdk_members(platform, rac.DEFAULT_MATRIX, VERSION))
    write_archive(
        root / rac.cli_asset_name(platform),
        cli,
        as_zip=platform.startswith("windows-"),
        platform=platform,
    )
    # Windows SDKs intentionally have a .tar.gz name but ZIP bytes.
    write_archive(
        root / rac.sdk_asset_name(platform),
        sdk,
        as_zip=platform.startswith("windows-"),
        platform=platform,
    )
    return cli, sdk


class ReleaseArtifactContentsTests(unittest.TestCase):
    def test_relocated_backfill_verifier_resolves_registry_from_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as td, mock.patch.dict(
            os.environ, {"GITHUB_WORKSPACE": str(ROOT)}
        ), mock.patch.object(rac, "__file__", str(Path(td) / "release_artifact_contents.py")):
            self.assertEqual(
                rac._control_registry_digest(),
                "b3bfbc17c377a58531c0689ce961d33d43d7504c61f8db979cd1a0df678409bc",
            )

    def test_cli_contract_tracks_import_design_runtime_manifest(self) -> None:
        runtime_manifest = (
            ROOT
            / "tools"
            / "import-design"
            / "browser_capture"
            / "runtime_manifest.txt"
        )
        runtime_members = {
            f"browser_capture/{line.strip()}"
            for line in runtime_manifest.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }

        unix_members = rac.cli_members(
            "darwin-arm64", rac.DEFAULT_MATRIX, VERSION
        )
        windows_members = rac.cli_members(
            "windows-x64", rac.DEFAULT_MATRIX, VERSION
        )
        self.assertTrue(runtime_members)
        self.assertLessEqual(runtime_members, unix_members)
        self.assertLessEqual(runtime_members, windows_members)
        self.assertIn("pulp-import-design", unix_members)
        self.assertIn("pulp-import-design.exe", windows_members)
        self.assertIn("pulp-control-broker", unix_members)
        self.assertNotIn("pulp-control-broker", windows_members)

    def test_control_broker_contract_is_darwin_only_and_version_floored(self) -> None:
        self.assertNotIn(
            "pulp-control-broker",
            rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, "0.794.0"),
        )
        self.assertIn(
            "pulp-control-broker",
            rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, "0.795.0"),
        )
        self.assertIn(
            "pulp-sdk/libexec/pulp/pulp-control-broker",
            rac.required_sdk_members(
                "darwin-arm64", rac.DEFAULT_MATRIX, "0.795.0"
            ),
        )
        self.assertNotIn(
            "pulp-sdk/libexec/pulp/pulp-control-broker",
            rac.required_sdk_members("linux-x64", rac.DEFAULT_MATRIX, "9.8.7"),
        )

    def test_control_standalone_host_contract_is_darwin_only_and_version_floored(self) -> None:
        for platform in ("darwin-arm64", "darwin-x64"):
            with self.subTest(platform=platform):
                cli_before = rac.cli_members(platform, rac.DEFAULT_MATRIX, "0.803.0")
                cli_at_floor = rac.cli_members(
                    platform, rac.DEFAULT_MATRIX, "0.803.1"
                )
                sdk_at_floor = rac.required_sdk_members(
                    platform, rac.DEFAULT_MATRIX, "0.803.1"
                )
                self.assertNotIn(
                    rac.CONTROL_STANDALONE_HOST_CLI_MEMBER, cli_before
                )
                self.assertNotIn(
                    rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST, cli_before
                )
                self.assertIn(
                    rac.CONTROL_STANDALONE_HOST_CLI_MEMBER, cli_at_floor
                )
                self.assertIn(
                    rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST, cli_at_floor
                )
                self.assertIn(
                    rac.CONTROL_STANDALONE_HOST_SDK_MEMBER, sdk_at_floor
                )
                self.assertIn(
                    rac.CONTROL_STANDALONE_HOST_SDK_MANIFEST, sdk_at_floor
                )

        for platform in (
            "linux-arm64",
            "linux-x64",
            "windows-arm64",
            "windows-x64",
        ):
            with self.subTest(platform=platform):
                self.assertNotIn(
                    rac.CONTROL_STANDALONE_HOST_CLI_MEMBER,
                    rac.cli_members(platform, rac.DEFAULT_MATRIX, "9.8.7"),
                )
                self.assertNotIn(
                    rac.CONTROL_STANDALONE_HOST_SDK_MEMBER,
                    rac.required_sdk_members(platform, rac.DEFAULT_MATRIX, "9.8.7"),
                )

    def test_cli_contract_preserves_pre_import_design_releases(self) -> None:
        members = rac.cli_members(
            "linux-x64", rac.DEFAULT_MATRIX, "0.763.0"
        )
        self.assertEqual(
            members,
            frozenset({"pulp", "pulp-cpp", "pulp-mcp", "libwgpu_native.so"}),
        )

    def test_interface_target_parser_accepts_cmake_case_and_whitespace(self) -> None:
        self.assertEqual(
            interface_library_targets_from_text(
                "ADD_LIBRARY (pulp-example INTERFACE)"
            ),
            {"pulp-example"},
        )

    def test_interface_target_parser_ignores_cmake_comments(self) -> None:
        self.assertEqual(
            interface_library_targets_from_text(
                """
                # add_library(pulp-line-comment INTERFACE)
                #[[
                add_library(pulp-bracket-comment INTERFACE)
                ]]
                add_library(pulp-live INTERFACE)
                """
            ),
            {"pulp-live"},
        )

    def test_sdk_target_parser_accepts_whitespace_and_multiline_appends(self) -> None:
        self.assertEqual(
            installed_sdk_targets(
                """
                set ( PULP_SDK_TARGETS pulp-one )
                list (
                    APPEND
                    PULP_SDK_TARGETS
                    pulp-two
                )
                install(TARGETS ${PULP_SDK_TARGETS})
                """
            ),
            {"pulp-one", "pulp-two"},
        )

    def test_sdk_target_parser_rejects_noncanonical_mutations(self) -> None:
        for mutation in (
            "list(PREPEND PULP_SDK_TARGETS pulp-two)",
            "set(pulp_sdk_targets pulp-two)",
            "list(APPEND pulp_sdk_targets pulp-two)",
            "unset(PULP_SDK_TARGETS)",
            "set(PULP_SDK_TARGETS ${PULP_SDK_TARGETS} pulp-two)",
            'list(APPEND PULP_SDK_TARGETS "${_new_target}")',
            "string(APPEND PULP_SDK_TARGETS pulp-two)",
            "if((TRUE) AND PULP_SDK_TARGETS)",
            "install(FILES ${PULP_SDK_TARGETS} DESTINATION lib)",
            "install(TARGETS ${PULP_SDK_TARGETS} ${PULP_SDK_TARGETS})",
            "# set(PULP_SDK_TARGETS pulp-two)",
            "#[[ set(PULP_SDK_TARGETS pulp-two) ]]",
        ):
            with self.subTest(mutation=mutation):
                with self.assertRaises(AssertionError):
                    installed_sdk_targets(
                        "set(PULP_SDK_TARGETS pulp-one)\n"
                        f"{mutation}\n"
                        "install(TARGETS ${PULP_SDK_TARGETS})\n"
                    )

    def test_sdk_target_parser_requires_one_install_consumer(self) -> None:
        for consumers in (
            "",
            """
            install(TARGETS ${PULP_SDK_TARGETS})
            install(TARGETS ${PULP_SDK_TARGETS})
            """,
        ):
            with self.subTest(consumers=consumers):
                with self.assertRaises(AssertionError):
                    installed_sdk_targets(
                        f"set(PULP_SDK_TARGETS pulp-one)\n{consumers}\n"
                    )

    def test_installed_sdk_archives_match_release_matrix(self) -> None:
        archive_targets = installed_sdk_targets() - interface_library_targets()
        matrix_targets = set(rac.DEFAULT_MATRIX.pulp_library_stems)
        for platform_targets in rac.DEFAULT_MATRIX.platform_library_stems.values():
            matrix_targets.update(platform_targets)
        self.assertEqual(
            archive_targets,
            matrix_targets,
            "PULP_SDK_TARGETS archives and release_product_matrix.json drifted",
        )

    def test_packaged_cli_products_match_release_matrix(self) -> None:
        runtime_members = {
            f"browser_capture/{line.strip()}"
            for line in IMPORT_DESIGN_RUNTIME_MANIFEST.read_text(
                encoding="utf-8"
            ).splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        self.assertEqual(
            rac.DEFAULT_MATRIX.cli_binary_stems,
            {"pulp", "pulp-cpp", "pulp-import-design", "pulp-mcp"},
        )
        self.assertEqual(
            rac.DEFAULT_MATRIX.common_cli_members,
            runtime_members,
            "browser-capture runtime manifest and release product matrix drifted",
        )
        self.assertEqual(
            rac.sdk_import_design_runtime_members(rac.DEFAULT_MATRIX, VERSION),
            {
                "pulp-sdk/bin/browser_capture-v1/"
                + member.removeprefix("browser_capture/")
                for member in runtime_members
            },
            "SDK importer runtime and release product matrix drifted",
        )

    def test_declared_matrix_selects_historical_cli_contracts(self) -> None:
        legacy = {"pulp", "pulp-cpp", "pulp-mcp", "libwgpu_native.dylib"}
        self.assertEqual(
            rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, "0.763.0"),
            legacy,
        )
        self.assertEqual(
            rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, "0.764.0"),
            {
                "pulp",
                "pulp-cpp",
                "pulp-import-design",
                "pulp-mcp",
                "libwgpu_native.dylib",
                *rac.PRE_DECLARATIVE_IMPORT_DESIGN_COMMON_CLI_MEMBERS,
            },
        )
        self.assertEqual(
            rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, "0.765.0"),
            {
                "pulp",
                "pulp-cpp",
                "pulp-import-design",
                "pulp-mcp",
                "libwgpu_native.dylib",
                *rac.DEFAULT_MATRIX.common_cli_members,
            },
        )

    def test_legacy_product_matrix_selects_versioned_cli_contract(self) -> None:
        legacy_doc = json.loads(
            rac.DEFAULT_MATRIX_PATH.read_text(encoding="utf-8")
        )
        legacy_doc.pop("cli_binary_stems")
        legacy_doc.pop("common_cli_members")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            path = root / "release_product_matrix.json"
            path.write_text(json.dumps(legacy_doc), encoding="utf-8")
            matrix = rac.ProductMatrix.load(path)
            cli_path = root / rac.cli_asset_name("darwin-arm64")
            write_archive(
                cli_path,
                set(rac.cli_members("darwin-arm64", matrix, "0.764.0")),
                as_zip=False,
            )
            rac.verify_cli_archive(
                cli_path, "darwin-arm64", "0.764.0", matrix
            )

        self.assertEqual(matrix.cli_binary_stems, {"pulp", "pulp-cpp", "pulp-mcp"})
        self.assertEqual(matrix.common_cli_members, set())
        self.assertEqual(
            rac.cli_members("darwin-arm64", matrix, "0.763.0"),
            {"pulp", "pulp-cpp", "pulp-mcp", "libwgpu_native.dylib"},
        )
        self.assertEqual(
            rac.cli_members("darwin-arm64", matrix, "0.764.0"),
            {
                "pulp",
                "pulp-cpp",
                "pulp-import-design",
                "pulp-mcp",
                "libwgpu_native.dylib",
                *rac.PRE_DECLARATIVE_IMPORT_DESIGN_COMMON_CLI_MEMBERS,
            },
        )

    def test_windows_library_matrix_tracks_x64_skia_availability(self) -> None:
        self.assertIn(
            "pulp-bundled-fonts",
            rac.expected_pulp_libraries("windows-x64", rac.DEFAULT_MATRIX),
        )
        self.assertNotIn(
            "pulp-bundled-fonts",
            rac.expected_pulp_libraries("windows-arm64", rac.DEFAULT_MATRIX),
        )

    def test_complete_windows_x64_matrix_passes_with_bundled_fonts(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _cli, sdk = make_platform(root, "windows-x64")
            self.assertIn("pulp-sdk/lib/pulp-bundled-fonts.lib", sdk)
            rac.verify_platform(
                root, "windows-x64", VERSION, SOURCE_SHA, native_signatures=False
            )

    def test_complete_windows_arm64_matrix_passes_without_bundled_fonts(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _cli, sdk = make_platform(root, "windows-arm64")
            self.assertNotIn("pulp-sdk/lib/pulp-bundled-fonts.lib", sdk)
            rac.verify_platform(
                root, "windows-arm64", VERSION, SOURCE_SHA,
                native_signatures=False,
            )

    def test_negative_control_windows_x64_requires_bundled_fonts(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _cli, sdk = make_platform(root, "windows-x64")
            sdk.remove("pulp-sdk/lib/pulp-bundled-fonts.lib")
            write_archive(
                root / rac.sdk_asset_name("windows-x64"),
                sdk,
                as_zip=True,
                platform="windows-x64",
            )
            with self.assertRaisesRegex(rac.ContentError, "pulp-bundled-fonts"):
                rac.verify_platform(
                    root, "windows-x64", VERSION, SOURCE_SHA,
                    native_signatures=False,
                )

    def test_negative_control_windows_arm64_rejects_bundled_fonts(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _cli, sdk = make_platform(root, "windows-arm64")
            sdk.add("pulp-sdk/lib/pulp-bundled-fonts.lib")
            write_archive(
                root / rac.sdk_asset_name("windows-arm64"),
                sdk,
                as_zip=True,
                platform="windows-arm64",
            )
            with self.assertRaisesRegex(
                rac.ContentError, "stale_or_unexpected=.*pulp-bundled-fonts"
            ):
                rac.verify_platform(
                    root, "windows-arm64", VERSION, SOURCE_SHA,
                    native_signatures=False,
                )

    def test_negative_control_missing_format_library_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "linux-x64")
            sdk.remove("pulp-sdk/lib/libpulp-format.a")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "libpulp-format"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_stale_vanished_target_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "darwin-arm64")
            sdk.add("pulp-sdk/lib/libpulp-retired-target.a")
            write_archive(
                root / rac.cli_asset_name("darwin-arm64"),
                cli,
                as_zip=False,
                platform="darwin-arm64",
            )
            write_archive(
                root / rac.sdk_asset_name("darwin-arm64"),
                sdk,
                as_zip=False,
                platform="darwin-arm64",
            )
            with self.assertRaisesRegex(rac.ContentError, "retired-target"):
                rac.verify_platform(
                    root, "darwin-arm64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_unexpected_cli_payload_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, _sdk = make_platform(root, "linux-x64")
            cli.add("stale-plugin.vst3")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "stale-plugin"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_rejects_pre_floor_sdk_broker(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            matrix = replace(rac.DEFAULT_MATRIX, control_broker_floor="10.0.0")
            sdk = set(
                rac.required_sdk_members("darwin-arm64", matrix, VERSION)
            )
            sdk.add(rac.CONTROL_BROKER_SDK_MEMBER)
            path = root / rac.sdk_asset_name("darwin-arm64")
            write_archive(
                path, sdk, as_zip=False, platform="darwin-arm64"
            )

            with self.assertRaisesRegex(rac.ContentError, "stale pre-floor"):
                rac.verify_sdk_archive(
                    path, "darwin-arm64", VERSION, SOURCE_SHA, matrix
                )

    def test_negative_control_rejects_pre_floor_sdk_standalone_host(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            matrix = replace(
                rac.DEFAULT_MATRIX, control_standalone_host_floor="10.0.0"
            )
            sdk = set(rac.required_sdk_members("darwin-arm64", matrix, VERSION))
            sdk.update(
                {
                    rac.CONTROL_STANDALONE_HOST_SDK_MEMBER,
                    rac.CONTROL_STANDALONE_HOST_SDK_MANIFEST,
                }
            )
            path = root / rac.sdk_asset_name("darwin-arm64")
            write_archive(path, sdk, as_zip=False, platform="darwin-arm64")

            with self.assertRaisesRegex(rac.ContentError, "stale pre-floor"):
                rac.verify_sdk_archive(
                    path, "darwin-arm64", VERSION, SOURCE_SHA, matrix
                )

    def test_negative_control_rejects_executable_standalone_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli = set(rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, VERSION))
            path = root / rac.cli_asset_name("darwin-arm64")
            write_archive(
                path,
                cli,
                as_zip=False,
                platform="darwin-arm64",
                mode_overrides={rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST: 0o700},
            )

            with self.assertRaisesRegex(rac.ContentError, "mode is 0o700"):
                rac.verify_cli_archive(path, "darwin-arm64", VERSION)

    def test_negative_control_rejects_malformed_standalone_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli = set(rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, VERSION))
            path = root / rac.cli_asset_name("darwin-arm64")
            original_member_payload = member_payload
            with mock.patch(
                __name__ + ".member_payload",
                side_effect=lambda name, platform="linux-x64": (
                    b"{not-json"
                    if name == rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST
                    else original_member_payload(name, platform)
                ),
            ):
                write_archive(path, cli, as_zip=False, platform="darwin-arm64")

            with self.assertRaisesRegex(rac.ContentError, "invalid Standalone host manifest"):
                rac.verify_cli_archive(path, "darwin-arm64", VERSION)

    def test_negative_control_rejects_stale_standalone_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli = set(rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, VERSION))
            path = root / rac.cli_asset_name("darwin-arm64")
            original_member_payload = member_payload

            def stale_payload(name: str, platform: str = "linux-x64") -> bytes:
                if name != rac.CONTROL_STANDALONE_HOST_CLI_MANIFEST:
                    return original_member_payload(name, platform)
                document = json.loads(standalone_manifest_payload())
                document["registry_digest"] = "0" * 64
                return rac._canonical_standalone_manifest(document)

            with mock.patch(__name__ + ".member_payload", side_effect=stale_payload):
                write_archive(path, cli, as_zip=False, platform="darwin-arm64")

            with self.assertRaisesRegex(rac.ContentError, "manifest contract"):
                rac.verify_cli_archive(path, "darwin-arm64", VERSION)

    def test_selected_source_registry_digest_overrides_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli = set(rac.cli_members("darwin-arm64", rac.DEFAULT_MATRIX, VERSION))
            path = root / rac.cli_asset_name("darwin-arm64")
            write_archive(path, cli, as_zip=False, platform="darwin-arm64")

            with self.assertRaisesRegex(rac.ContentError, "manifest contract"):
                rac.verify_cli_archive(
                    path,
                    "darwin-arm64",
                    VERSION,
                    rac.DEFAULT_MATRIX,
                    "0" * 64,
                )

    def test_negative_control_rejects_tampered_standalone_host_binding(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sdk = set(rac.required_sdk_members("darwin-arm64", rac.DEFAULT_MATRIX, VERSION))
            path = root / rac.sdk_asset_name("darwin-arm64")
            original_member_payload = member_payload

            def tampered_payload(name: str, platform: str = "linux-x64") -> bytes:
                if name == rac.CONTROL_STANDALONE_HOST_SDK_MEMBER:
                    return standalone_host_payload().replace(
                        b"PULP_CONTROL_MANIFEST_SHA256_", b"TAMPERED_MANIFEST_SHA256_"
                    )
                return original_member_payload(name, platform)

            with mock.patch(__name__ + ".member_payload", side_effect=tampered_payload):
                write_archive(path, sdk, as_zip=False, platform="darwin-arm64")

            with self.assertRaisesRegex(rac.ContentError, "binding mismatch"):
                rac.verify_sdk_archive(
                    path, "darwin-arm64", VERSION, SOURCE_SHA, rac.DEFAULT_MATRIX
                )

    def test_negative_control_invalid_signature_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "darwin-arm64")
            failed = mock.Mock(returncode=1, stdout="", stderr="invalid signature")
            with mock.patch.object(rac.sys, "platform", "darwin"), mock.patch.object(
                rac.subprocess, "run", return_value=failed
            ):
                with self.assertRaisesRegex(rac.ContentError, "invalid signature"):
                    rac.verify_native_macos_signatures(
                        root, "darwin-arm64", VERSION
                    )

    def test_negative_control_wrong_version_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            with self.assertRaisesRegex(rac.ContentError, "expected '1.2.3'"):
                rac.verify_platform(
                    root, "linux-x64", "1.2.3", SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_missing_provenance_fires_at_new_floor(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "linux-x64")
            sdk.remove("pulp-sdk/sdk-provenance.json")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "sdk-provenance.json"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_missing_installed_build_info_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _cli, sdk = make_platform(root, "linux-x64")
            sdk.remove("pulp-sdk/include/pulp/runtime/build_info.hpp")
            write_archive(
                root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False
            )
            with self.assertRaisesRegex(rac.ContentError, "build_info.hpp"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_tampered_installed_build_info_fires(self) -> None:
        member = "pulp-sdk/include/pulp/runtime/build_info.hpp"
        mutations = {
            "dirty": member_payload(member).replace(
                b"kGitDirty = false", b"kGitDirty = true"
            ),
            "build type": member_payload(member).replace(
                b'kBuildType = "Release"', b'kBuildType = "Debug"'
            ),
            "version": member_payload(member).replace(
                b'kSdkVersion = "9.8.7"', b'kSdkVersion = "1.2.3"'
            ),
            "source SHA": member_payload(member).replace(
                b'kGitSha = "aaaaaaa"', b'kGitSha = "bbbbbbb"'
            ),
        }
        for name, payload in mutations.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                _cli, sdk = make_platform(root, "linux-x64")
                write_archive(
                    root / rac.sdk_asset_name("linux-x64"),
                    sdk,
                    as_zip=False,
                    platform="linux-x64",
                    payload_overrides={member: payload},
                )
                with self.assertRaisesRegex(
                    rac.ContentError, "unsafe installed build_info.hpp"
                ):
                    rac.verify_platform(
                        root,
                        "linux-x64",
                        VERSION,
                        SOURCE_SHA,
                        native_signatures=False,
                    )

    def test_negative_control_missing_capability_handoff_fires_at_new_floor(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "linux-x64")
            sdk.remove("pulp-sdk/share/pulp/agent-capability-handoff.json")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False)
            with self.assertRaisesRegex(
                rac.ContentError, "agent-capability-handoff.json"
            ):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_missing_sdk_importer_runtime_fires_at_new_floor(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "linux-x64")
            sdk.remove("pulp-sdk/bin/browser_capture-v1/capture.mjs")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "browser_capture-v1/capture.mjs"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_stale_sdk_importer_runtime_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            sdk_path = root / rac.sdk_asset_name("linux-x64")
            with rac.Archive(sdk_path) as archive:
                members = set(archive.members)
            runtime_member = "pulp-sdk/bin/browser_capture-v1/capture.mjs"
            handoff_member = "pulp-sdk/share/pulp/agent-capability-handoff.json"
            original_payload = member_payload
            stamped_handoff = original_payload(handoff_member, "linux-x64")

            def stale_runtime_payload(
                name: str, platform: str = "linux-x64"
            ) -> bytes:
                if name == handoff_member:
                    return stamped_handoff
                if name == runtime_member:
                    return b"stale browser runtime"
                return original_payload(name, platform)

            with mock.patch(
                __name__ + ".member_payload", side_effect=stale_runtime_payload
            ):
                write_archive(sdk_path, members, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "runtime sha256"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_unexpected_sdk_importer_runtime_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "linux-x64")
            sdk.add("pulp-sdk/bin/browser_capture-v1/stale.mjs")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "stale_or_unexpected"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_unsafe_provenance_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            sdk_path = root / rac.sdk_asset_name("linux-x64")
            with rac.Archive(sdk_path) as archive:
                members = set(archive.members)
            original_payload = member_payload

            def unsafe_payload(name: str, platform: str = "linux-x64") -> bytes:
                if name == "pulp-sdk/sdk-provenance.json":
                    marker = json.loads(original_payload(name, platform))
                    marker["features"]["inspector"] = False
                    return json.dumps(marker).encode()
                return original_payload(name, platform)

            with mock.patch(__name__ + ".member_payload", side_effect=unsafe_payload):
                write_archive(sdk_path, members, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "unsafe SDK provenance"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_wrong_provenance_commit_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            with self.assertRaisesRegex(rac.ContentError, "source_git_sha"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, "b" * 40, native_signatures=False
                )

    def test_negative_control_wrong_handoff_sdk_sha_fires(self) -> None:
        self._assert_mutated_handoff_rejected(
            lambda document: document.__setitem__("sdk_source_sha", "b" * 40),
            "sdk_source_sha",
        )

    def test_negative_control_wrong_handoff_importer_hash_fires(self) -> None:
        self._assert_mutated_handoff_rejected(
            lambda document: document["importer"].__setitem__("sha256", "b" * 64),
            "importer sha256",
        )

    def test_negative_control_wrong_handoff_capability_hash_fires(self) -> None:
        self._assert_mutated_handoff_rejected(
            lambda document: document["agent_capabilities"].__setitem__(
                "sha256", "b" * 64
            ),
            "capability sha256",
        )

    def test_negative_control_substituted_capability_schema_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            sdk_path = root / rac.sdk_asset_name("linux-x64")
            with rac.Archive(sdk_path) as archive:
                members = set(archive.members)
            original_payload = member_payload
            changed_capabilities = b'{"unexpected":true}'

            def substituted_payload(
                name: str, platform: str = "linux-x64"
            ) -> bytes:
                if name == "pulp-sdk/share/pulp/agent-capabilities.json":
                    return changed_capabilities
                if name == "pulp-sdk/share/pulp/agent-capabilities.schema.json":
                    return b'{"type":"object"}'
                payload = original_payload(name, platform)
                if name == "pulp-sdk/share/pulp/agent-capability-handoff.json":
                    document = json.loads(payload)
                    document["agent_capabilities"]["sha256"] = hashlib.sha256(
                        changed_capabilities
                    ).hexdigest()
                    document["agent_capabilities"]["content"] = json.loads(
                        changed_capabilities
                    )
                    return json.dumps(document).encode()
                return payload

            with mock.patch(
                __name__ + ".member_payload", side_effect=substituted_payload
            ):
                write_archive(sdk_path, members, as_zip=False)
            with self.assertRaisesRegex(
                rac.ContentError, "agent_capabilities schema sha256"
            ):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA,
                    native_signatures=False,
                )

    def _assert_mutated_handoff_rejected(self, mutate, message: str) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            sdk_path = root / rac.sdk_asset_name("linux-x64")
            with rac.Archive(sdk_path) as archive:
                members = set(archive.members)
            original_payload = member_payload

            def mutated_payload(name: str, platform: str = "linux-x64") -> bytes:
                payload = original_payload(name, platform)
                if name == "pulp-sdk/share/pulp/agent-capability-handoff.json":
                    document = json.loads(payload)
                    mutate(document)
                    return json.dumps(document).encode()
                return payload

            with mock.patch(
                __name__ + ".member_payload", side_effect=mutated_payload
            ):
                write_archive(sdk_path, members, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, message):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_wrong_provenance_platform_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            sdk_path = root / rac.sdk_asset_name("linux-x64")
            with rac.Archive(sdk_path) as archive:
                members = set(archive.members)
            original_payload = member_payload

            def wrong_platform_payload(
                name: str, platform: str = "linux-x64"
            ) -> bytes:
                if name == "pulp-sdk/sdk-provenance.json":
                    marker = json.loads(original_payload(name, platform))
                    marker["platform"] = "darwin-arm64"
                    return json.dumps(marker).encode()
                return original_payload(name, platform)

            with mock.patch(
                __name__ + ".member_payload", side_effect=wrong_platform_payload
            ):
                write_archive(sdk_path, members, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "platform"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_negative_control_unreadable_provenance_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _cli, sdk = make_platform(root, "linux-x64")
            write_archive(
                root / rac.sdk_asset_name("linux-x64"),
                sdk,
                as_zip=False,
                mode_overrides={"pulp-sdk/sdk-provenance.json": 0o600},
            )
            with self.assertRaisesRegex(rac.ContentError, "not world-readable"):
                rac.verify_platform(
                    root, "linux-x64", VERSION, SOURCE_SHA, native_signatures=False
                )

    def test_pre_contract_release_is_explicitly_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(
                rac.main(
                    [
                        td,
                        "--platform",
                        "linux-x64",
                        "--version",
                        "0.759.0",
                        "--source-sha",
                        SOURCE_SHA,
                    ]
                ),
                0,
            )

    def test_historical_matrix_without_provenance_floor_remains_loadable(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "historical-matrix.json"
            document = json.loads(rac.DEFAULT_MATRIX_PATH.read_text(encoding="utf-8"))
            del document["sdk_provenance_floor"]
            del document["capability_handoff_floor"]
            del document["inspector_sdk_floor"]
            del document["control_broker_floor"]
            del document["control_standalone_host_floor"]
            path.write_text(json.dumps(document), encoding="utf-8")
            historical = rac.ProductMatrix.load(path)
            self.assertEqual(historical.sdk_provenance_floor, "999999.0.0")
            self.assertEqual(historical.capability_handoff_floor, "999999.0.0")
            self.assertEqual(historical.inspector_sdk_floor, "999999.0.0")
            self.assertEqual(historical.control_broker_floor, "999999.0.0")
            self.assertEqual(
                historical.control_standalone_host_floor, "999999.0.0"
            )
            self.assertNotIn(
                "pulp-control-broker",
                rac.cli_members("darwin-arm64", historical, "0.795.0"),
            )
            self.assertNotIn(
                "pulp-sdk/sdk-provenance.json",
                rac.required_sdk_members("linux-x64", historical, "0.763.0"),
            )


class ActivePlatformsContract(unittest.TestCase):
    """The active_platforms shipping knob and its asset-contract derivation."""

    @staticmethod
    def _load(mutate) -> "rac.ProductMatrix":
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "matrix.json"
            document = json.loads(
                rac.DEFAULT_MATRIX_PATH.read_text(encoding="utf-8")
            )
            mutate(document)
            path.write_text(json.dumps(document), encoding="utf-8")
            return rac.ProductMatrix.load(path)

    def test_absent_field_means_every_platform(self) -> None:
        matrix = self._load(lambda d: d.pop("active_platforms", None))
        self.assertEqual(matrix.active_platforms, matrix.platforms)

    def test_subset_round_trips(self) -> None:
        matrix = self._load(
            lambda d: d.update(active_platforms=["darwin-arm64"])
        )
        self.assertEqual(matrix.active_platforms, ("darwin-arm64",))

    def test_flip_back_to_full_restores_the_full_contract(self) -> None:
        subset = self._load(
            lambda d: d.update(active_platforms=["darwin-arm64"])
        )
        restored = self._load(lambda d: d.pop("active_platforms", None))
        self.assertEqual(
            rac.release_asset_names(subset),
            ("pulp-darwin-arm64.tar.gz", "pulp-sdk-darwin-arm64.tar.gz"),
        )
        self.assertEqual(len(rac.release_asset_names(restored)), 12)
        self.assertIn("pulp-windows-x64.zip", rac.release_asset_names(restored))
        self.assertIn(
            "pulp-sdk-linux-arm64.tar.gz", rac.release_asset_names(restored)
        )

    def test_empty_subset_is_rejected(self) -> None:
        with self.assertRaisesRegex(rac.ContentError, "active_platforms"):
            self._load(lambda d: d.update(active_platforms=[]))

    def test_unknown_platform_is_rejected(self) -> None:
        with self.assertRaisesRegex(rac.ContentError, "not in the platform"):
            self._load(lambda d: d.update(active_platforms=["freebsd-x64"]))

    def test_darwinless_subset_is_rejected(self) -> None:
        with self.assertRaisesRegex(rac.ContentError, "darwin"):
            self._load(lambda d: d.update(active_platforms=["linux-x64"]))

    def test_all_platforms_verification_walks_only_the_active_subset(
        self,
    ) -> None:
        # --all-platforms against an empty directory fails on the FIRST
        # missing archive; with an arm-only subset that first (and only)
        # platform demanded must be darwin-arm64, and no linux/windows
        # archive may be requested.
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "matrix.json"
            document = json.loads(
                rac.DEFAULT_MATRIX_PATH.read_text(encoding="utf-8")
            )
            document["active_platforms"] = ["darwin-arm64"]
            path.write_text(json.dumps(document), encoding="utf-8")
            empty = Path(td) / "assets"
            empty.mkdir()
            import contextlib
            import io

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = rac.main(
                    [
                        str(empty),
                        "--all-platforms",
                        "--version",
                        VERSION,
                        "--source-sha",
                        SOURCE_SHA,
                        "--matrix",
                        str(path),
                    ]
                )
            self.assertEqual(rc, 1)
            self.assertIn("pulp-darwin-arm64", stderr.getvalue())
            self.assertNotIn("linux", stderr.getvalue())
            self.assertNotIn("windows", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
