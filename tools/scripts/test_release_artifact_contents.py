#!/usr/bin/env python3
"""Negative controls for the release artifact content watchdog."""

from __future__ import annotations

import importlib.util
import io
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


def member_payload(name: str) -> bytes:
    if name == "pulp-sdk/version.txt":
        return f"{VERSION}\n".encode()
    if name == "pulp-sdk/sdk_build_type.txt":
        return b"Release\n"
    return b"fixture"


def write_archive(path: Path, members: set[str], *, as_zip: bool) -> None:
    if as_zip:
        with zipfile.ZipFile(path, "w") as archive:
            for name in sorted(members):
                info = zipfile.ZipInfo(name)
                info.external_attr = 0o755 << 16
                archive.writestr(info, member_payload(name))
        return
    with tarfile.open(path, "w:gz") as archive:
        for name in sorted(members):
            data = member_payload(name)
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755 if "/bin/" in name or name in {"pulp", "pulp-cpp", "pulp-mcp"} else 0o644
            archive.addfile(info, io.BytesIO(data))


def make_platform(root: Path, platform: str) -> tuple[set[str], set[str]]:
    cli = set(rac.cli_members(platform))
    sdk = set(rac.required_sdk_members(platform, rac.DEFAULT_MATRIX))
    write_archive(root / rac.cli_asset_name(platform), cli, as_zip=platform.startswith("windows-"))
    # Windows SDKs intentionally have a .tar.gz name but ZIP bytes.
    write_archive(root / rac.sdk_asset_name(platform), sdk, as_zip=platform.startswith("windows-"))
    return cli, sdk


class ReleaseArtifactContentsTests(unittest.TestCase):
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

    def test_complete_windows_matrix_passes_despite_misleading_sdk_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "windows-x64")
            rac.verify_platform(root, "windows-x64", VERSION, native_signatures=False)

    def test_negative_control_missing_format_library_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "linux-x64")
            sdk.remove("pulp-sdk/lib/libpulp-format.a")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("linux-x64"), sdk, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "libpulp-format"):
                rac.verify_platform(root, "linux-x64", VERSION, native_signatures=False)

    def test_negative_control_stale_vanished_target_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, sdk = make_platform(root, "darwin-arm64")
            sdk.add("pulp-sdk/lib/libpulp-retired-target.a")
            write_archive(root / rac.cli_asset_name("darwin-arm64"), cli, as_zip=False)
            write_archive(root / rac.sdk_asset_name("darwin-arm64"), sdk, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "retired-target"):
                rac.verify_platform(root, "darwin-arm64", VERSION, native_signatures=False)

    def test_negative_control_unexpected_cli_payload_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cli, _sdk = make_platform(root, "linux-x64")
            cli.add("stale-plugin.vst3")
            write_archive(root / rac.cli_asset_name("linux-x64"), cli, as_zip=False)
            with self.assertRaisesRegex(rac.ContentError, "stale-plugin"):
                rac.verify_platform(root, "linux-x64", VERSION, native_signatures=False)

    def test_negative_control_invalid_signature_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "darwin-arm64")
            failed = mock.Mock(returncode=1, stdout="", stderr="invalid signature")
            with mock.patch.object(rac.sys, "platform", "darwin"), mock.patch.object(
                rac.subprocess, "run", return_value=failed
            ):
                with self.assertRaisesRegex(rac.ContentError, "invalid signature"):
                    rac.verify_native_macos_signatures(root, "darwin-arm64")

    def test_negative_control_wrong_version_fires(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            make_platform(root, "linux-x64")
            with self.assertRaisesRegex(rac.ContentError, "expected '1.2.3'"):
                rac.verify_platform(root, "linux-x64", "1.2.3", native_signatures=False)

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
                    ]
                ),
                0,
            )


if __name__ == "__main__":
    unittest.main()
