#!/usr/bin/env python3
"""Host-side fixtures for the ARM64EC provider archive guard."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import struct
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("validate_arm64ec_provider.py")
_spec = importlib.util.spec_from_file_location("validate_arm64ec_provider", SCRIPT)
assert _spec and _spec.loader
_mod = importlib.util.module_from_spec(_spec)
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
            json.dumps({"target": {"architecture": "arm64ec"}}), encoding="utf-8"
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
        members = _mod.validate_provider([archive], "arm64ec", self.metadata)
        self.assertEqual({member.machine for member in members}, {0xA641})

    def test_arm64x_may_contain_native_and_ec_members(self) -> None:
        self.metadata.write_text(
            json.dumps({"target": {"architecture": "arm64x"}}), encoding="utf-8"
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
