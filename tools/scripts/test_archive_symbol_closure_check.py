#!/usr/bin/env python3
"""Negative controls for archive_symbol_closure_check.py.

A stub nm prints canned `nm -g -P` output per archive path, so the gate's
verdict logic is exercised without a toolchain: a clean subject passes, a
subject that references a forbidden archive's symbol fails, and a subject the
instrument cannot see into is inconclusive rather than passing.
"""

from __future__ import annotations

import json
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import archive_symbol_closure_check as gate  # noqa: E402


class StubNm:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.listings: dict[str, str] = {}
        self.path = root / "nm-stub"

    def archive(self, name: str, listing: str) -> Path:
        path = self.root / name
        path.write_text("", encoding="utf-8")
        self.listings[str(path)] = textwrap.dedent(listing).lstrip("\n")
        return path

    def write(self) -> str:
        table = self.root / "listings.json"
        table.write_text(json.dumps(self.listings), encoding="utf-8")
        self.path.write_text(
            f"#!{sys.executable}\n"
            "import json, sys\n"
            f"table = json.load(open({str(table)!r}, encoding='utf-8'))\n"
            "path = sys.argv[-1]\n"
            "if path not in table:\n"
            "    sys.stderr.write('no such archive\\n'); sys.exit(1)\n"
            "sys.stdout.write(table[path])\n",
            encoding="utf-8")
        self.path.chmod(self.path.stat().st_mode | stat.S_IXUSR)
        return str(self.path)


STATE = """
    libstate.a[preset.o]:
    _state_preset T 0 0
    _runtime_log U
    _state_helper U
    libstate.a[helper.o]:
    _state_helper T 0 0
"""
RUNTIME = """
    libruntime.a[log.o]:
    _runtime_log T 0 0
"""
CONTENT = """
    libcontent.a[registry.o]:
    _content_registry_ctor T 0 0
    _inline_shared W 0 0
"""


class ArchiveSymbolClosureCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.nm = StubNm(Path(self._tmp.name))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_gate(self, state: str, *extra: str) -> int:
        subject = self.nm.archive("libstate.a", state)
        runtime = self.nm.archive("libruntime.a", RUNTIME)
        content = self.nm.archive("libcontent.a", CONTENT)
        nm = self.nm.write()
        return gate.main(["--nm", nm, "--subject", str(subject),
                          "--forbidden", str(content), "--control", str(runtime), *extra])

    def test_clean_subject_passes(self) -> None:
        self.assertEqual(self.run_gate(STATE), 0)

    def test_reference_to_forbidden_definition_fails(self) -> None:
        self.assertEqual(self.run_gate(STATE + "    _content_registry_ctor U\n"), 1)

    def test_internal_reference_is_not_a_dependency(self) -> None:
        # _state_helper is undefined in one member and defined in another.
        self.assertEqual(self.run_gate(STATE), 0)

    def test_weak_definition_is_not_a_dependency(self) -> None:
        self.assertEqual(self.run_gate(STATE + "    _inline_shared U\n"), 0)

    def test_a_contamination_floor_counts_weak_definitions(self) -> None:
        # --forbidden-defines any: a weak definition (typeinfo, vtable) is
        # still the forbidden archive's, so referencing it is contamination.
        self.assertEqual(self.run_gate(STATE + "    _inline_shared U\n", "--forbidden-defines", "any"), 1)
        self.assertEqual(self.run_gate(STATE, "--forbidden-defines", "any"), 0)

    def test_a_subject_weak_definition_resolves_its_own_reference_under_any(self) -> None:
        # The subject defines _inline_shared weakly itself, so its own
        # reference is internal even when the forbidden archive defines it too.
        state = STATE + "    _inline_shared U\n    libstate.a[inline.o]:\n    _inline_shared W 0 0\n"
        self.assertEqual(self.run_gate(state, "--forbidden-defines", "any"), 0)

    def test_weak_rtti_contamination_is_opt_in(self) -> None:
        content = self.nm.archive("libcontent.a", CONTENT + "    __ZTIN7content8RegistryE S 0 0\n")
        core = self.nm.archive("libcore.a", "libcore.a[x.o]:\n__ZTIN7content8RegistryE W 0 0\n")
        nm = self.nm.write()
        argv = ["--nm", nm, "--forbidden", str(content), "--forbidden-defines", "any",
                "--no-control", "--check", f"core={core}"]
        self.assertEqual(gate.main(argv), 0)
        self.assertEqual(gate.main([*argv, "--weak-rtti-contamination"]), 1)

    def test_rtti_names_in_both_manglings(self) -> None:
        listing = ("a.a[x.o]: __ZTIN3foo3BarE W 0 0\n"
                   "a.a[x.o]: _ZTVN3foo3BarE V 0 0\n"
                   "a.a[x.o]: _ZTSN3foo3BarE W 0 0\n"
                   "a.a[x.o]: _ZN3foo3Bar3bazEv W 0 0\n"
                   "a.a[x.o]: __ZTIN3foo3QuxE T 0 0\n")
        self.assertEqual(gate.weak_rtti(listing),
                         {"__ZTIN3foo3BarE", "_ZTVN3foo3BarE", "_ZTSN3foo3BarE"})

    def test_check_mode_uses_the_chosen_rule(self) -> None:
        content = self.nm.archive("libcontent.a", CONTENT)
        core = self.nm.archive("libcore.a", "libcore.a[x.o]:\n_inline_shared U\n")
        nm = self.nm.write()
        argv = ["--nm", nm, "--forbidden", str(content), "--no-control", "--check", f"core={core}"]
        self.assertEqual(gate.main(argv), 0)
        self.assertEqual(gate.main([*argv, "--forbidden-defines", "any"]), 1)

    def test_weak_undefined_reference_to_forbidden_definition_fails(self) -> None:
        # A weak call can still resolve from the forbidden archive. Lowercase
        # nm types must not disappear from the closure instrument.
        self.assertEqual(self.run_gate(STATE + "    _content_registry_ctor w\n"), 1)

    def test_blind_instrument_is_inconclusive(self) -> None:
        blind = """
            libstate.a[preset.o]:
            _state_preset T 0 0
        """
        self.assertEqual(self.run_gate(blind), 2)

    def test_failing_nm_is_inconclusive(self) -> None:
        subject = self.nm.archive("libstate.a", STATE)
        runtime = self.nm.archive("libruntime.a", RUNTIME)
        nm = self.nm.write()
        missing = Path(self._tmp.name) / "libmissing.a"
        self.assertEqual(gate.main(["--nm", nm, "--subject", str(subject),
                                    "--forbidden", str(missing), "--control", str(runtime)]), 2)

    def test_missing_forbidden_list_is_inconclusive(self) -> None:
        subject = self.nm.archive("libstate.a", STATE)
        runtime = self.nm.archive("libruntime.a", RUNTIME)
        nm = self.nm.write()
        self.assertEqual(gate.main(["--nm", nm, "--subject", str(subject),
                                    "--control", str(runtime)]), 2)


if __name__ == "__main__":
    unittest.main()
