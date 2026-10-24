#!/usr/bin/env python3
"""Controls for nm_symbols.py, the one reader of `nm -g -P` listings."""
from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import nm_symbols  # noqa: E402

# One line per type the rules distinguish, in the `nm -A -P` spelling (macOS
# and GNU look alike here), plus a member header, prose and a warning.
LISTING_A = (
    "lib.a[m.o]:\n"
    "lib.a[m.o]: strong_text T 0000000000000040 0000000000000010\n"
    "lib.a[m.o]: strong_data D 10 8\n"
    "lib.a[m.o]: weak_def W 0 0\n"
    "lib.a[m.o]: weak_obj V 0 0\n"
    "lib.a[m.o]: gnu_unique u 0 8\n"
    "lib.a[m.o]: gnu_indirect i 80 4\n"
    "lib.a[m.o]: needed U\n"
    "lib.a[m.o]: weak_ref w\n"
    "lib.a[m.o]: weak_obj_ref v\n"
    "lib.a[m.o]: _objc_msgSend$initWithUTF8String: U 0 0\n"
    "lib.a[m.o]: _objc_sel$stringWithFormat: T 20 0\n"
    "not a symbol line\n"
    "warning: something odd happened\n"
    "lib.a[m.o]: also not a symbol line\n"
)
# The same symbols in the plain `nm -P` spelling.
LISTING_PLAIN = "\n".join(
    line.split(": ", 1)[1] if line.startswith("lib.a[m.o]: ") else line
    for line in LISTING_A.splitlines()) + "\n"

STRONG = {"strong_text", "strong_data", "_objc_sel$stringWithFormat:"}
ANY = STRONG | {"weak_def", "weak_obj", "gnu_unique", "gnu_indirect"}
UNDEFINED = {"needed", "weak_ref", "weak_obj_ref", "_objc_msgSend$initWithUTF8String:"}


class NmSymbolsTest(unittest.TestCase):
    def test_both_spellings_read_the_same_symbols(self):
        for label, listing in (("-A", LISTING_A), ("plain", LISTING_PLAIN)):
            with self.subTest(spelling=label):
                self.assertEqual(nm_symbols.defined_strong(listing), STRONG)
                self.assertEqual(nm_symbols.defined_any(listing), ANY)
                self.assertEqual(nm_symbols.undefined(listing), UNDEFINED)

    def test_members_come_from_the_archive_prefix(self):
        members = {member for member, _, _ in nm_symbols.entries(LISTING_A)}
        self.assertEqual(members, {"m.o"})
        self.assertEqual({member for member, _, _ in nm_symbols.entries(LISTING_PLAIN)}, {None})

    def test_names_ending_in_a_colon_are_symbols(self):
        names = {name for _, name, _ in nm_symbols.entries(LISTING_PLAIN)}
        self.assertIn("_objc_msgSend$initWithUTF8String:", names)
        self.assertIn("_objc_sel$stringWithFormat:", names)

    def test_prose_headers_and_warnings_are_skipped(self):
        names = {name for _, name, _ in nm_symbols.entries(LISTING_A + LISTING_PLAIN)}
        for word in ("not", "also", "warning:", "something", "lib.a[m.o]:"):
            self.assertNotIn(word, names)

    def test_every_symbol_line_is_classified_exactly_once(self):
        # Strong is a subset of any, and nothing is both defined and undefined.
        self.assertLessEqual(STRONG, ANY)
        self.assertFalse(ANY & UNDEFINED)
        self.assertEqual(len(list(nm_symbols.entries(LISTING_A))), len(ANY | UNDEFINED))


if __name__ == "__main__":
    unittest.main()
