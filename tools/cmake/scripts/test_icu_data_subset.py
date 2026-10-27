#!/usr/bin/env python3
"""Source-only tests for icu_data_subset.py (synthetic data, no toolchain)."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True

import icu_data_subset as subset  # noqa: E402


def make_header(copyright_text: bytes = b"test") -> bytes:
    """A little-endian CmnD header padded to 16 bytes, like ICU's."""
    info = struct.pack("<HHBBBB4s4s4s", 20, 0, 0, 0, 2, 0, b"CmnD", b"\x01\0\0\0", b"\0\0\0\0")
    body = info + copyright_text + b"\0"
    size = 4 + len(body)
    size += (-size) % 16
    return struct.pack("<HBB", size, 0xDA, 0x27) + body + b"\0" * (size - 4 - len(body))


ITEMS = [
    ("icudt78l/brkitr/line.brk", b"L" * 40),
    ("icudt78l/brkitr/thaidict.dict", b"T" * 33),
    ("icudt78l/cnvalias.icu", b"A" * 16),
    ("icudt78l/coll/root.res", b"C" * 50),
    ("icudt78l/gb18030.cnv", b"G" * 70),
    ("icudt78l/metaZones.res", b"M" * 9),
    ("icudt78l/nfc.nrm", b"N" * 24),
    ("icudt78l/root.res", b"R" * 17),
    ("icudt78l/zone/en.res", b"Z" * 12),
]


def make_package(items=ITEMS) -> bytes:
    return subset.write_common_data(make_header(), sorted(items))


def make_macho_object(payload: bytes) -> bytes:
    """A minimal MH_OBJECT: one segment with one __TEXT,__const section holding
    ``payload``, LC_BUILD_VERSION, LC_SYMTAB with one external symbol and an
    LC_DYSYMTAB, laid out the way clang emits the ICU data object."""
    ncmds = 4
    seg_size = 72 + 80
    sizeofcmds = seg_size + 24 + 24 + 80
    data_off = 32 + sizeofcmds
    symoff = data_off + len(payload)
    strtab = b"\0_icudt78_dat\0\0\0"
    stroff = symoff + 16
    hdr = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 1, ncmds, sizeofcmds, 0, 0)
    seg = struct.pack("<II16sQQQQiiII", 0x19, seg_size, b"", 0, len(payload),
                      data_off, len(payload), 7, 7, 1, 0)
    sect = struct.pack("<16s16sQQIIIIIIII", b"__const", b"__TEXT", 0, len(payload),
                       data_off, 4, 0, 0, 0, 0, 0, 0)
    build = struct.pack("<IIIIII", 0x32, 24, 1, 0x000D0000, 0, 0)
    symtab = struct.pack("<IIIIII", 0x2, 24, symoff, 1, stroff, len(strtab))
    dysym = struct.pack("<II18I", 0xB, 80, 0, 0, 0, 1, 1, 0, *([0] * 12))
    nlist = struct.pack("<IBBHQ", 1, 0x0F, 1, 0, 0)
    return hdr + seg + sect + build + symtab + dysym + payload + nlist + strtab


class PolicyTest(unittest.TestCase):
    def test_thin_and_universal_lipo_reports_are_told_apart(self):
        self.assertTrue(subset.is_thin(
            "Non-fat file: /sdk/libskunicode_icu.a is architecture: arm64\n"))
        self.assertFalse(subset.is_thin(
            "Architectures in the fat file: /x/libskunicode_icu.a are: x86_64 arm64\n"))

    def test_keeps_break_rules_dictionaries_and_alias_table(self):
        for name in ("icudt78l/brkitr/line.brk", "icudt78l/brkitr/cjdict.dict",
                     "icudt78l/cnvalias.icu", "icudt78l/root.res", "icudt78l/nfc.nrm",
                     "icudt78l/uemoji.icu", "icudt78l/likelySubtags.res"):
            self.assertFalse(subset.is_dropped(name), name)

    def test_drops_services_pulp_does_not_link(self):
        for name in ("icudt78l/coll/root.res", "icudt78l/zone/en.res", "icudt78l/curr/en.res",
                     "icudt78l/lang/en.res", "icudt78l/region/en.res", "icudt78l/unit/en.res",
                     "icudt78l/translit/root.res", "icudt78l/rbnf/en.res",
                     "icudt78l/gb18030.cnv", "icudt78l/confusables.cfu",
                     "icudt78l/zoneinfo64.res", "icudt78l/metaZones.res",
                     "icudt78l/uts46.nrm"):
            self.assertTrue(subset.is_dropped(name), name)

    def test_unknown_items_are_kept(self):
        self.assertFalse(subset.is_dropped("icudt79l/newthing/data.res"))
        self.assertFalse(subset.is_dropped("icudt79l/brand-new.icu"))


class CommonDataTest(unittest.TestCase):
    def test_round_trip_preserves_every_item(self):
        data = make_package()
        header_size, items = subset.read_common_data(data)
        self.assertEqual(header_size % 16, 0)
        self.assertEqual([n for n, _ in items], sorted(n for n, _ in ITEMS))
        for (name, payload), (_, original) in zip(items, sorted(ITEMS)):
            self.assertEqual(payload.rstrip(b"\0"), original, name)

    def test_subset_keeps_only_policy_items_aligned_and_sorted(self):
        data = make_package()
        out, stats = subset.subset_common_data(data)
        _, items = subset.read_common_data(out)
        kept = [n for n, _ in items]
        self.assertEqual(kept, ["icudt78l/brkitr/line.brk", "icudt78l/brkitr/thaidict.dict",
                                "icudt78l/cnvalias.icu", "icudt78l/nfc.nrm", "icudt78l/root.res"])
        header_size = struct.unpack_from("<H", out, 0)[0]
        count = struct.unpack_from("<I", out, header_size)[0]
        for i in range(count):
            _, data_off = struct.unpack_from("<II", out, header_size + 4 + 8 * i)
            self.assertEqual((header_size + data_off) % 16, 0)
        self.assertEqual(out[:header_size], data[:header_size])
        self.assertEqual(stats["items_in"], len(ITEMS))
        self.assertEqual(stats["items_kept"], 5)
        self.assertLess(stats["bytes_out"], stats["bytes_in"])
        self.assertIn("coll/", stats["dropped_bytes_by_group"])
        self.assertIn("cnv", stats["dropped_bytes_by_group"])

    def test_rejects_non_common_data(self):
        with self.assertRaises(subset.SubsetError):
            subset.read_common_data(b"\0" * 64)
        bad = bytearray(make_package())
        bad[12:16] = b"ResB"
        with self.assertRaises(subset.SubsetError):
            subset.read_common_data(bytes(bad))
        big_endian = bytearray(make_package())
        big_endian[8] = 1
        with self.assertRaises(subset.SubsetError):
            subset.read_common_data(bytes(big_endian))

    def test_rejects_unsorted_toc(self):
        data = subset.write_common_data(make_header(), [("b/x", b"1" * 16), ("a/y", b"2" * 16)])
        with self.assertRaises(subset.SubsetError):
            subset.read_common_data(data)


class MachOTest(unittest.TestCase):
    def test_replace_payload_moves_symbol_table(self):
        payload = make_package()
        obj = make_macho_object(payload)
        self.assertEqual(subset.extract_macho_payload(obj), payload)
        smaller, _ = subset.subset_common_data(payload)
        new = subset.replace_macho_payload(obj, smaller)
        self.assertEqual(subset.extract_macho_payload(new), smaller)
        delta = len(smaller) - len(payload)
        self.assertEqual(len(new), len(obj) + delta)
        # The symbol table and string table are carried over unchanged.
        self.assertEqual(new[-32:], obj[-32:])
        symoff, _, stroff, _ = struct.unpack_from("<IIII", new, 32 + 152 + 24 + 8)
        self.assertEqual(new[symoff:symoff + 16], obj[-32:-16])
        self.assertEqual(new[stroff:stroff + 16], obj[-16:])
        # Segment file size and section size follow the payload.
        _, _, filesize = struct.unpack_from("<QQQ", new, 32 + 32)
        self.assertEqual(filesize, len(smaller))
        self.assertEqual(struct.unpack_from("<Q", new, 32 + 72 + 40)[0], len(smaller))

    def test_rejects_objects_with_relocations(self):
        obj = bytearray(make_macho_object(make_package()))
        struct.pack_into("<I", obj, 32 + 72 + 60, 1)  # section nreloc
        with self.assertRaises(subset.SubsetError):
            subset.extract_macho_payload(bytes(obj))

    def test_rejects_non_object_files(self):
        obj = bytearray(make_macho_object(make_package()))
        struct.pack_into("<I", obj, 12, 6)  # MH_DYLIB
        with self.assertRaises(subset.SubsetError):
            subset.extract_macho_payload(bytes(obj))


if __name__ == "__main__":
    unittest.main()
