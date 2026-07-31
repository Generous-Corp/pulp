#!/usr/bin/env python3
"""Negative controls for the release artifact content watchdog."""

from __future__ import annotations

import importlib.util
import io
import json
import re
import tarfile
import tempfile
import unittest
import zipfile
import sys
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


def member_payload(name: str, platform: str = "linux-x64") -> bytes:
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
                    "features": {"audio_probes": False, "inspector": False},
                }
            )
            + "\n"
        ).encode()
    return b"fixture"


def write_archive(
    path: Path,
    members: set[str],
    *,
    as_zip: bool,
    platform: str = "linux-x64",
    mode_overrides: dict[str, int] | None = None,
) -> None:
    mode_overrides = mode_overrides or {}
    if as_zip:
        with zipfile.ZipFile(path, "w") as archive:
            for name in sorted(members):
                info = zipfile.ZipInfo(name)
                info.external_attr = mode_overrides.get(name, 0o755) << 16
                archive.writestr(info, member_payload(name, platform))
        return
    with tarfile.open(path, "w:gz") as archive:
        for name in sorted(members):
            data = member_payload(name, platform)
            info = tarfile.TarInfo(name)
            info.size = len(data)
            default_mode = (
                0o755
                if "/bin/" in name
                or name in rac.cli_binary_members(
                    platform, rac.DEFAULT_MATRIX, VERSION
                )
                else 0o644
            )
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

    def test_complete_windows_matrix_passes_despite_misleading_sdk_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "windows-x64")
            rac.verify_platform(
                root, "windows-x64", VERSION, SOURCE_SHA, native_signatures=False
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
                    marker["features"]["inspector"] = True
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
            path.write_text(json.dumps(document), encoding="utf-8")
            historical = rac.ProductMatrix.load(path)
            self.assertEqual(historical.sdk_provenance_floor, "999999.0.0")
            self.assertNotIn(
                "pulp-sdk/sdk-provenance.json",
                rac.required_sdk_members("linux-x64", historical, "0.763.0"),
            )


if __name__ == "__main__":
    unittest.main()
