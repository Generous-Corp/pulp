#!/usr/bin/env python3
"""Source-only tests for plugin_footprint_check.py (synthetic Mach-O images)."""

from __future__ import annotations

import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True

import plugin_footprint_check as check  # noqa: E402


def segment(name: str, filesize: int) -> bytes:
    return struct.pack("<II16sQQQQiiII", 0x19, 72, name.encode(), 0, filesize, 0, filesize, 7, 7, 0, 0)


def image(cpu: int, segments: dict[str, int]) -> bytes:
    cmds = b"".join(segment(n, s) for n, s in segments.items())
    return struct.pack("<IiiIIIII", 0xFEEDFACF, cpu, 0, 8, len(segments), len(cmds), 0, 0) + cmds


def fat(slices: list[tuple[int, bytes]]) -> bytes:
    header = struct.pack(">II", 0xCAFEBABE, len(slices))
    offset = 4096
    table, body = b"", b""
    for cpu, data in slices:
        table += struct.pack(">iiIII", cpu, 0, offset + len(body), len(data), 12)
        body += data + b"\0" * ((-len(data)) % 4096)
    return header + table + b"\0" * (offset - len(header) - len(table)) + body


SEGMENTS = {"__PAGEZERO": 0, "__TEXT": 9000, "__DATA_CONST": 500, "__DATA": 300, "__LINKEDIT": 7000}


class MachOTest(unittest.TestCase):
    def test_thin_image_mapped_bytes_exclude_linkedit(self):
        data = image(check._CPU_ARM64, SEGMENTS)
        self.assertEqual(check.macho_slices(data), {"arm64": (0, len(data))})
        segs = check.segment_sizes(data)
        self.assertEqual(sum(v for k, v in segs.items() if k != "__LINKEDIT"), 9800)
        self.assertEqual(segs["__LINKEDIT"], 7000)

    def test_universal_image_reports_every_slice(self):
        arm = image(check._CPU_ARM64, SEGMENTS)
        x86 = image(check._CPU_X86_64, {"__TEXT": 12000, "__LINKEDIT": 10})
        data = fat([(check._CPU_X86_64, x86), (check._CPU_ARM64, arm)])
        slices = check.macho_slices(data)
        self.assertEqual(set(slices), {"arm64", "x86_64"})
        self.assertEqual(check.segment_sizes(data, slices["x86_64"][0])["__TEXT"], 12000)
        self.assertEqual(check.segment_sizes(data, slices["arm64"][0])["__TEXT"], 9000)

    def test_rejects_non_macho(self):
        with self.assertRaises(check.FootprintError):
            check.macho_slices(b"\x7fELF" + b"\0" * 60)


class IcuDataTest(unittest.TestCase):
    def test_size_is_distance_to_next_symbol(self):
        symbols = [(0x1000, "_a"), (0x2000, "_icudt78_dat"), (0x2000, "_alias"), (0x5000, "_b")]
        self.assertEqual(check.icu_data_bytes(symbols), 0x3000)

    def test_missing_symbol_reads_zero(self):
        self.assertEqual(check.icu_data_bytes([(0x1000, "_a")]), 0)


def report(mapped: int, icu: int, names: list[str], arch: str = "arm64") -> dict:
    return {"binary": "Plug", "slices": {arch: {
        "mapped_bytes": mapped, "linkedit_bytes": 0, "icu_data_bytes": icu, "symbol_names": names}}}


class EvaluateTest(unittest.TestCase):
    def test_within_budget_passes(self):
        budget = {"max_mapped_bytes": 100, "max_icu_data_bytes": 50, "require_icu_data": True}
        self.assertEqual(check.evaluate(report(100, 50, []), budget), [])

    def test_each_limit_fails_on_its_own(self):
        self.assertEqual(len(check.evaluate(report(101, 0, []), {"max_mapped_bytes": 100})), 1)
        self.assertEqual(len(check.evaluate(report(1, 51, []), {"max_icu_data_bytes": 50})), 1)
        self.assertEqual(len(check.evaluate(report(1, 0, []), {"require_icu_data": True})), 1)

    def test_forbidden_symbols_fail(self):
        budget = {"forbidden_symbol_patterns": ["4pulp4view", "quickjs"]}
        failures = check.evaluate(report(1, 0, ["__ZN4pulp6format1xE", "__ZN4pulp4view4ViewC2Ev"]), budget)
        self.assertEqual(len(failures), 1)
        self.assertIn("__ZN4pulp4view4ViewC2Ev", failures[0])
        self.assertEqual(check.evaluate(report(1, 0, ["__ZN4pulp6format1xE"]), budget), [])

    def test_arches_scope_the_budget(self):
        budget = {"arches": ["arm64"], "max_mapped_bytes": 10}
        self.assertEqual(check.evaluate(report(99, 0, [], arch="x86_64"), budget), [])
        self.assertEqual(len(check.evaluate(report(99, 0, [], arch="arm64"), budget)), 1)


class BudgetFileTest(unittest.TestCase):
    def test_shipped_budget_profiles_are_well_formed(self):
        profiles = json.loads(check.BUDGET_FILE.read_text(encoding="utf-8"))["profiles"]
        for name in ("editor", "lite-editor", "no-editor", "icu-data"):
            self.assertIn(name, profiles)
            self.assertEqual(profiles[name].get("arches"), ["arm64"])
        self.assertTrue(profiles["editor"]["require_icu_data"])
        self.assertTrue(profiles["no-editor"]["forbidden_symbol_patterns"])
        lite = profiles["lite-editor"]
        self.assertFalse(lite.get("require_icu_data", False))
        self.assertLess(lite["max_mapped_bytes"], profiles["editor"]["max_mapped_bytes"] // 4)
        for family in ("icudt", "SkCanvas", "dawn", "tint", "MacGpuPluginViewHost"):
            self.assertTrue(any(family in p for p in lite["forbidden_symbol_patterns"]), family)
        self.assertLess(profiles["editor"]["max_icu_data_bytes"], 10_000_000)


if __name__ == "__main__":
    unittest.main()
