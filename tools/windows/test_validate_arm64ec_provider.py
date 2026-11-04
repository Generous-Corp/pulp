#!/usr/bin/env python3
"""Host-side fixtures for the ARM64EC provider archive guard."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import struct
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("validate_arm64ec_provider.py")
_spec = importlib.util.spec_from_file_location("validate_arm64ec_provider", SCRIPT)
assert _spec and _spec.loader
_mod = importlib.util.module_from_spec(_spec)
# ``dataclasses`` resolves postponed annotations through ``sys.modules`` while
# importing the validator.  Register the dynamically loaded module first; this
# is required by Python 3.14 and is also the correct importlib contract.
sys.modules[_spec.name] = _mod
_spec.loader.exec_module(_mod)


def _member(name: str, payload: bytes) -> bytes:
    header = (
        f"{name:<16}{0:>12}{0:>6}{0:>6}{0:>8}{len(payload):>10}`\n"
    ).encode("ascii")
    assert len(header) == 60
    return header + payload + (b"\n" if len(payload) & 1 else b"")


def _archive(path: pathlib.Path, machines: list[int]) -> None:
    longnames = b"very-long-provider-object.obj/\n"
    data = bytearray(b"!<arch>\n")
    data.extend(_member("/", b"linker index"))
    data.extend(_member("//", longnames))
    for index, machine in enumerate(machines):
        payload = bytearray(20)
        struct.pack_into("<H", payload, 0, machine)
        data.extend(_member("/0" if index == 0 else f"member{index}.obj/", bytes(payload)))
    path.write_bytes(data)


class Arm64EcProviderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.metadata = self.root / "msvc-toolchain.json"
        self.metadata.write_text(
            json.dumps(
                {
                    "target": {"architecture": "arm64ec"},
                    "producer": {
                        "skia_gn_compiler": {"version": "19.51.36260"},
                        "dawn_cmake_compiler": {"version": "19.51.36260"},
                    },
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_pure_arm64_archive_is_rejected_even_with_ec_metadata(self) -> None:
        archive = self.root / "skia.lib"
        _archive(archive, [0xAA64, 0xAA64])
        with self.assertRaisesRegex(ValueError, "incompatible arm64ec"):
            _mod.validate_provider([archive], "arm64ec", self.metadata)

    def test_ec_archive_is_accepted(self) -> None:
        archive = self.root / "dawn_combined.lib"
        _archive(archive, [0xA641, 0xA641])
        members = _mod.validate_provider(
            [archive], "arm64ec", self.metadata, "19.51.36260"
        )
        self.assertEqual({member.machine for member in members}, {0xA641})

    def test_older_consumer_toolset_is_rejected_before_link(self) -> None:
        archive = self.root / "skia.lib"
        _archive(archive, [0xA641])
        with self.assertRaisesRegex(ValueError, "older than the provider compiler"):
            _mod.validate_provider([archive], "arm64ec", self.metadata, "19.44.35207")

    def test_newer_consumer_toolset_is_accepted(self) -> None:
        archive = self.root / "skia.lib"
        _archive(archive, [0xA641])
        members = _mod.validate_provider(
            [archive], "arm64ec", self.metadata, "19.52.10000"
        )
        self.assertEqual(len(members), 1)

    def test_consumer_check_rejects_provider_without_compiler_record(self) -> None:
        archive = self.root / "skia.lib"
        _archive(archive, [0xA641])
        self.metadata.write_text(
            json.dumps({"target": {"architecture": "arm64ec"}}), encoding="utf-8"
        )
        with self.assertRaisesRegex(ValueError, "producer compiler records"):
            _mod.validate_provider([archive], "arm64ec", self.metadata, "19.51.36260")

    def test_arm64x_may_contain_native_and_ec_members(self) -> None:
        self.metadata.write_text(
            json.dumps(
                {
                    "target": {"architecture": "arm64x"},
                    "producer": {
                        "skia_gn_compiler": {"version": "19.51.36260"},
                        "dawn_cmake_compiler": {"version": "19.51.36260"},
                    },
                }
            ),
            encoding="utf-8",
        )
        archive = self.root / "skia.lib"
        _archive(archive, [0xAA64, 0xA64E])
        members = _mod.validate_provider([archive], "arm64x", self.metadata)
        self.assertEqual({member.machine for member in members}, {0xAA64, 0xA64E})

    def test_native_metadata_is_rejected_before_link(self) -> None:
        self.metadata.write_text(
            json.dumps({"target": {"architecture": "arm64"}}), encoding="utf-8"
        )
        archive = self.root / "skia.lib"
        _archive(archive, [0xAA64])
        with self.assertRaisesRegex(ValueError, "declares 'arm64'"):
            _mod.validate_provider([archive], "arm64ec", self.metadata)


if __name__ == "__main__":
    unittest.main()
