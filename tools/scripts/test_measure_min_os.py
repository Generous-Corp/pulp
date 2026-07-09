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


if __name__ == "__main__":
    unittest.main()
