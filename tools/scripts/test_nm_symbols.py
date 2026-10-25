#!/usr/bin/env python3
"""Controls for nm_symbols.py, the one reader of `nm -g -P` listings."""
from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import nm_symbols  # noqa: E402

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures" / "nm_symbols"


def fixture(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8")

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


class SpellingTest(unittest.TestCase):
    def test_the_spelling_is_decided_once_per_listing(self):
        # In an `-A` listing a name ending in ":" stays a name; in a plain
        # listing the same text is not misread as an archive prefix.
        prefixed = "lib.a[m.o]: _objc$sel: U 0 0\nlib.a[m.o]: _real T 10 0\n"
        plain = "_objc$sel: U 0 0\n_real T 10 0\n"
        for listing in (prefixed, plain):
            self.assertEqual({name for _, name, _ in nm_symbols.entries(listing)}, {"_objc$sel:", "_real"})
        # A listing that mixes in a plain-only line is read plainly throughout,
        # so its prefixed lines are not half-read as names.
        mixed = "lib.a[m.o]: _a T 0 0\n_b T 0 0\n"
        self.assertEqual({name for _, name, _ in nm_symbols.entries(mixed)}, {"_b"})


class MachOWeaknessTest(unittest.TestCase):
    """Real Apple nm output: weakness is only in `nm -m`, never in the -P letter."""

    def test_apple_letters_show_no_weak_definitions(self):
        for stem in ("darwin_compiled_view_core_planted", "darwin_view_core", "darwin_view_audio"):
            with self.subTest(stem=stem):
                self.assertEqual(nm_symbols.weak_defined(fixture(f"{stem}.P.txt")), set())

    def test_darwin_entries_read_weak_strong_and_undefined(self):
        entries = dict(nm_symbols.darwin_entries(fixture("darwin_view_core.m.txt")))
        # weak private external, weak external automatically hidden
        self.assertEqual(entries["__ZTIN4pulp4view16ChannelStripViewE"], "weak")
        self.assertEqual(entries["__ZTVN4pulp4view16ChannelStripViewE"], "weak")
        self.assertEqual(entries["__ZTVNSt3__112format_errorE"], "weak")
        audio = dict(nm_symbols.darwin_entries(fixture("darwin_view_audio.m.txt")))
        self.assertEqual(audio["__ZN4pulp4view5Meter9set_levelEff"], "strong")
        self.assertEqual(audio["__ZTVN4pulp4view5MeterE"], "strong")
        self.assertEqual(audio["__ZN4pulp4view5MeterD1Ev"], "weak")
        self.assertEqual(audio["__Unwind_Resume"], "undefined")

    def test_strength_uses_darwin_output_when_it_is_darwin(self):
        listing = fixture("darwin_compiled_view_core_planted.P.txt")
        strong, weak = nm_symbols.strength(listing, fixture("darwin_compiled_view_core_planted.m.txt"))
        self.assertIn("__ZTVN4pulp4view5MeterE", weak)
        self.assertIn("__ZN4pulp4view10make_meterEv", strong)
        self.assertFalse(strong & weak)
        # BSD-format `nm -m` output (what Apple's nm prints for ELF) is not
        # darwin, so the letters decide.
        bsd = "view_core.o:\n0000000000000000 W _ZN4pulp4view10SliderCore12notify_valueEv\n"
        self.assertFalse(nm_symbols.is_darwin(bsd))
        self.assertEqual(nm_symbols.strength(listing, bsd), (nm_symbols.defined_strong(listing), set()))


class ElfWeaknessTest(unittest.TestCase):
    def test_elf_letters_carry_weakness(self):
        listing = fixture("elf_view_core_planted.P.txt")
        weak = nm_symbols.weak_defined(listing)
        self.assertIn("_ZTVN4pulp4view5MeterE", weak)
        self.assertIn("_ZTISt12format_error", weak)
        self.assertNotIn("_ZN4pulp4view10make_meterEv", weak)
        self.assertEqual(nm_symbols.strength(listing, None), (nm_symbols.defined_strong(listing), weak))


if __name__ == "__main__":
    unittest.main()
