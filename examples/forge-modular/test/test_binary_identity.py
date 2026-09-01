#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "binary_identity.py"
SPEC = importlib.util.spec_from_file_location("binary_identity", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def macho(code: bytes, signature: bytes) -> bytes:
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0, 0, 2, 2, 88, 0, 0)
    signature_offset = len(header) + 72 + 16 + len(code)
    segment = struct.pack(
        "<II16sQQQQIIII", 0x19, 72, b"__TEXT", 0, signature_offset,
        0, signature_offset, 7, 5, 0, 0)
    command = struct.pack("<IIII", 0x1D, 16, signature_offset, len(signature))
    return header + segment + command + code + signature


class BinaryIdentityTest(unittest.TestCase):
    def test_signature_replacement_does_not_change_content_identity(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            first = Path(td) / "first"
            second = Path(td) / "second"
            first.write_bytes(macho(b"same code", b"adhoc"))
            second.write_bytes(macho(b"same code", b"developer id signature"))
            self.assertEqual(
                MODULE.content_sha256(str(first)), MODULE.content_sha256(str(second))
            )

    def test_code_change_changes_content_identity(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            first = Path(td) / "first"
            second = Path(td) / "second"
            first.write_bytes(macho(b"first code", b"signature"))
            second.write_bytes(macho(b"other code", b"signature"))
            self.assertNotEqual(
                MODULE.content_sha256(str(first)), MODULE.content_sha256(str(second))
            )

    def test_signature_offset_cannot_hide_executable_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            candidate = Path(td) / "candidate"
            data = bytearray(macho(b"executable code", b"signature"))
            signature_command = 32 + 72
            forged_offset = signature_command + 16
            struct.pack_into("<II", data, signature_command + 8,
                             forged_offset, len(data) - forged_offset)
            candidate.write_bytes(data)
            with self.assertRaisesRegex(ValueError,
                                        "file-backed executable data"):
                MODULE.content_sha256(str(candidate))

    def test_signature_blob_must_be_terminal(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            candidate = Path(td) / "candidate"
            candidate.write_bytes(macho(b"code", b"signature") + b"tail")
            with self.assertRaisesRegex(ValueError, "terminal Mach-O blob"):
                MODULE.content_sha256(str(candidate))

    @unittest.skipUnless(platform.system() == "Darwin", "requires macOS codesign")
    def test_real_codesign_replacement_is_identity_invariant(self) -> None:
        clang = shutil.which("clang")
        codesign = shutil.which("codesign")
        if not clang or not codesign:
            self.skipTest("clang and codesign are required")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "main.c"
            original = root / "original"
            resigned = root / "resigned"
            mutated = root / "mutated"
            source.write_text("int main(void) { return 0; }\n")
            subprocess.run([clang, str(source), "-o", str(original)], check=True)
            shutil.copy2(original, resigned)
            subprocess.run(
                [codesign, "--force", "-s", "-", str(resigned)],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertEqual(
                MODULE.content_sha256(str(original)),
                MODULE.content_sha256(str(resigned)),
            )

            source.write_text("int main(void) { return 7; }\n")
            subprocess.run([clang, str(source), "-o", str(mutated)], check=True)
            subprocess.run(
                [codesign, "--force", "-s", "-", str(mutated)],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertNotEqual(
                MODULE.content_sha256(str(original)),
                MODULE.content_sha256(str(mutated)),
            )


if __name__ == "__main__":
    unittest.main()
