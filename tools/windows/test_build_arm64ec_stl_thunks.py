#!/usr/bin/env python3
"""Host-only tests for the ARM64EC STL thunk archive generator."""

from __future__ import annotations

import importlib.util
import pathlib
import struct
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("build_arm64ec_stl_thunks.py")
_spec = importlib.util.spec_from_file_location("build_arm64ec_stl_thunks", SCRIPT)
assert _spec and _spec.loader
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)


class Arm64EcThunkToolTests(unittest.TestCase):
    def test_member_selection_is_filtered_and_deterministic(self) -> None:
        listing = "\n".join(
            [
                "  C:\\VC\\lib\\arm64ec\\z.obj",
                "  C:\\VC\\lib\\arm64\\native.obj",
                "  C:\\VC\\lib\\ARM64EC\\a.obj",
                "  C:\\VC\\lib\\arm64ec\\locale0_implib.obj",
                "  C:\\VC\\lib\\arm64ec\\z.obj",
            ]
        )
        self.assertEqual(
            _mod.select_arm64ec_members(listing),
            ["C:\\VC\\lib\\ARM64EC\\a.obj", "C:\\VC\\lib\\arm64ec\\z.obj"],
        )

    def test_strip_directives_preserves_object_and_zeros_only_directive_bytes(self) -> None:
        data = bytearray(20 + 40 + 5 + 3)
        struct.pack_into("<H", data, 2, 1)  # one section
        struct.pack_into("<H", data, 16, 0)  # no optional header
        data[20:28] = b".drectve"
        struct.pack_into("<I", data, 20 + 16, 5)
        struct.pack_into("<I", data, 20 + 20, 60)
        data[60:65] = b"/MD /X"
        data[65:68] = b"xyz"
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "member.obj"
            path.write_bytes(data)
            _mod.strip_directives(path)
            result = path.read_bytes()
        self.assertEqual(result[60:65], b"\0" * 5)
        self.assertEqual(result[65:68], b"xyz")

    def test_short_object_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "short.obj"
            path.write_bytes(b"short")
            with self.assertRaises(RuntimeError):
                _mod.strip_directives(path)


if __name__ == "__main__":
    unittest.main()
