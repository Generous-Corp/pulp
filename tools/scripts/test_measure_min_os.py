#!/usr/bin/env python3
"""Tests for tools/scripts/measure_min_os.py --elf/--max floor verification.

The --elf mode checks one freshly-linked binary's glibc floor against a max
(default: min_os.json linux-x64 floor), so CI can catch a build host that
re-leaks a higher glibc than the shipped prebuilts declare. We exercise the
four outcomes by stubbing the objdump/readelf reader (_glibc_floor) so the test
needs no real versioned ELF and runs on any host:

  * measured <= max          -> exit 0
  * measured >  max          -> exit 1
  * no GLIBC_ symbols found  -> exit 0 (vacuously OK: static archive / static bin)
  * --elf path missing       -> exit 2

Run:
    python3 tools/scripts/test_measure_min_os.py
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/scripts/measure_min_os.py"


def _load():
    spec = importlib.util.spec_from_file_location("measure_min_os", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ElfFloorCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = _load()
        # A real, existing file so the path check passes; its actual bytes are
        # irrelevant because _glibc_floor is stubbed in each case.
        cls._tmp = tempfile.NamedTemporaryFile(suffix=".so", delete=False)
        cls._tmp.write(b"\x7fELF not-really")
        cls._tmp.close()
        cls.elf = cls._tmp.name

    @classmethod
    def tearDownClass(cls):
        pathlib.Path(cls.elf).unlink(missing_ok=True)

    def _run(self, argv, floor):
        with mock.patch.object(self.mod, "_glibc_floor", return_value=floor):
            old = sys.argv
            sys.argv = ["measure_min_os.py", *argv]
            try:
                return self.mod.main()
            finally:
                sys.argv = old

    def test_below_floor_passes(self):
        self.assertEqual(self._run(["--elf", self.elf, "--max", "2.34"], "2.34"), 0)
        # Older glibc than the ceiling is fine (2.17 < 2.34).
        self.assertEqual(self._run(["--elf", self.elf, "--max", "2.34"], "2.17"), 0)

    def test_above_floor_fails(self):
        self.assertEqual(self._run(["--elf", self.elf, "--max", "2.34"], "2.39"), 1)
        # Minor-number compare is numeric, not lexical: 2.9 < 2.34 must pass,
        # 2.35 > 2.34 must fail.
        self.assertEqual(self._run(["--elf", self.elf, "--max", "2.34"], "2.9"), 0)
        self.assertEqual(self._run(["--elf", self.elf, "--max", "2.34"], "2.35"), 1)

    def test_no_glibc_symbols_is_vacuously_ok(self):
        self.assertEqual(self._run(["--elf", self.elf, "--max", "2.34"], None), 0)

    def test_missing_path_errors(self):
        self.assertEqual(
            self._run(["--elf", "/no/such/binary.so", "--max", "2.34"], "2.34"), 2
        )

    def test_default_max_reads_min_os_json_floor(self):
        # With no --max, the ceiling comes from min_os.json platforms.linux-x64.floor.
        # That floor is 2.34 in this repo, so a 2.34 measurement must pass and a
        # 2.99 measurement must fail — proving the default is wired, not hardcoded.
        self.assertEqual(self._run(["--elf", self.elf], "2.34"), 0)
        self.assertEqual(self._run(["--elf", self.elf], "2.99"), 1)


class ArtifactKind(unittest.TestCase):
    """_artifact_kind() classifies by magic bytes; measure_artifact() dispatches
    to the right reader (and tries both readers on an ar archive)."""

    @classmethod
    def setUpClass(cls):
        cls.mod = _load()

    def _write(self, magic: bytes) -> str:
        tmp = tempfile.NamedTemporaryFile(delete=False)
        tmp.write(magic + b"\x00" * 32)
        tmp.close()
        self.addCleanup(pathlib.Path(tmp.name).unlink, missing_ok=True)
        return tmp.name

    def test_kind_detection(self):
        cases = {
            b"!<arch>\n": "ar",
            b"MZ\x90\x00": "pe",
            b"\x7fELF\x02\x01\x01": "elf",
            b"\xcf\xfa\xed\xfe": "macho",   # thin 64-bit little-endian
            # fat: magic + nfat_arch=1 + first arch cputype=x86_64 (0x01000007)
            b"\xca\xfe\xba\xbe\x00\x00\x00\x01\x01\x00\x00\x07": "macho",
            b"not a binary!!!": None,
        }
        for magic, expected in cases.items():
            self.assertEqual(self.mod._artifact_kind(pathlib.Path(self._write(magic))),
                             expected, msg=f"magic={magic!r}")

    def test_fat_macho_magic_is_disambiguated_from_java_class(self):
        # A Java `.class` shares the 0xCAFEBABE magic. It must NOT be classified
        # as a Mach-O: bytes 4..8 are minor+major version (major 52 = Java 8) and
        # bytes 8..12 are the constant-pool count — never a Mach-O cputype.
        java_class = b"\xca\xfe\xba\xbe\x00\x00\x00\x34\x00\x1b\x07\x00"
        self.assertIsNone(
            self.mod._artifact_kind(pathlib.Path(self._write(java_class))))
        # A real fat Mach-O (nfat=2, first arch arm64 = 0x0100000C) IS a macho.
        fat = b"\xca\xfe\xba\xbe\x00\x00\x00\x02\x01\x00\x00\x0c"
        self.assertEqual(
            self.mod._artifact_kind(pathlib.Path(self._write(fat))), "macho")
        # A bare magic with no valid fat header (all-zero fields) is not a macho.
        self.assertIsNone(
            self.mod._artifact_kind(pathlib.Path(self._write(b"\xca\xfe\xba\xbe"))))

    def test_kind_missing_file(self):
        self.assertIsNone(self.mod._artifact_kind(pathlib.Path("/no/such/file")))

    def test_measure_dispatches_by_kind(self):
        macho = pathlib.Path(self._write(b"\xcf\xfa\xed\xfe"))
        elf = pathlib.Path(self._write(b"\x7fELF"))
        pe = pathlib.Path(self._write(b"MZ\x90\x00"))
        with mock.patch.object(self.mod, "_otool_minos", return_value="13.3"):
            self.assertEqual(self.mod.measure_artifact(macho), ("macho", "13.3"))
        with mock.patch.object(self.mod, "_glibc_floor", return_value="2.34"):
            self.assertEqual(self.mod.measure_artifact(elf), ("elf", "2.34"))
        with mock.patch.object(self.mod, "_win_os_version", return_value="10.0"):
            self.assertEqual(self.mod.measure_artifact(pe), ("pe", "10.0"))

    def test_measure_ar_tries_both_readers(self):
        ar = pathlib.Path(self._write(b"!<arch>\n"))
        # Mach-O member: otool returns a value.
        with mock.patch.object(self.mod, "_otool_minos", return_value="13.3"), \
             mock.patch.object(self.mod, "_glibc_floor", return_value=None):
            self.assertEqual(self.mod.measure_artifact(ar), ("ar", "13.3"))
        # ELF member: otool yields nothing, glibc reader wins.
        with mock.patch.object(self.mod, "_otool_minos", return_value=None), \
             mock.patch.object(self.mod, "_glibc_floor", return_value="2.34"):
            self.assertEqual(self.mod.measure_artifact(ar), ("ar", "2.34"))

    def test_measure_unrecognized(self):
        junk = pathlib.Path(self._write(b"not a binary!!!"))
        self.assertEqual(self.mod.measure_artifact(junk), (None, None))


class OtoolMinosParse(unittest.TestCase):
    """_parse_otool_minos reads both the modern LC_BUILD_VERSION and the legacy
    LC_VERSION_MIN_MACOSX load commands."""

    @classmethod
    def setUpClass(cls):
        cls.mod = _load()

    def test_lc_build_version(self):
        text = ("Load command 9\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n"
                " platform 1\n    minos 13.3\n      sdk 14.0\n")
        self.assertEqual(self.mod._parse_otool_minos(text), "13.3")

    def test_legacy_lc_version_min_macosx(self):
        # An older Mach-O with ONLY the legacy command must still yield a floor.
        text = ("Load command 10\n      cmd LC_VERSION_MIN_MACOSX\n  cmdsize 16\n"
                "  version 10.13\n      sdk 10.14\n")
        self.assertEqual(self.mod._parse_otool_minos(text), "10.13")

    def test_max_across_commands(self):
        text = ("      cmd LC_VERSION_MIN_MACOSX\n  version 10.13\n"
                "      cmd LC_BUILD_VERSION\n    minos 13.3\n")
        self.assertEqual(self.mod._parse_otool_minos(text), "13.3")

    def test_none_when_absent(self):
        self.assertIsNone(self.mod._parse_otool_minos("no load commands here\n"))


class MeasureCli(unittest.TestCase):
    """--measure <path> prints '<kind> <floor>' and exits 0/2."""

    @classmethod
    def setUpClass(cls):
        cls.mod = _load()

    def _run(self, argv):
        old = sys.argv
        sys.argv = ["measure_min_os.py", *argv]
        try:
            return self.mod.main()
        finally:
            sys.argv = old

    def test_missing_path(self):
        self.assertEqual(self._run(["--measure", "/no/such/bin"]), 2)

    def test_unrecognized_format(self):
        tmp = tempfile.NamedTemporaryFile(delete=False)
        tmp.write(b"plain text, not a binary")
        tmp.close()
        self.addCleanup(pathlib.Path(tmp.name).unlink, missing_ok=True)
        self.assertEqual(self._run(["--measure", tmp.name]), 2)

    def test_recognized_returns_zero(self):
        tmp = tempfile.NamedTemporaryFile(delete=False)
        tmp.write(b"\x7fELF" + b"\x00" * 32)
        tmp.close()
        self.addCleanup(pathlib.Path(tmp.name).unlink, missing_ok=True)
        with mock.patch.object(self.mod, "_glibc_floor", return_value="2.34"):
            self.assertEqual(self._run(["--measure", tmp.name]), 0)


if __name__ == "__main__":
    unittest.main()
