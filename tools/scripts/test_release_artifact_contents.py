#!/usr/bin/env python3
"""Negative controls for the release artifact content watchdog."""

from __future__ import annotations

import importlib.util
import io
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
