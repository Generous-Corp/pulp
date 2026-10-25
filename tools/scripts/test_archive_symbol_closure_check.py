#!/usr/bin/env python3
"""Negative controls for archive_symbol_closure_check.py.

A stub nm prints canned `nm -g -P` output per archive path, so the gate's
verdict logic is exercised without a toolchain: a clean subject passes, a
subject that references a forbidden archive's symbol fails, and a subject the
instrument cannot see into is inconclusive rather than passing.
"""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import archive_symbol_closure_check as gate  # noqa: E402
import nm_symbols  # noqa: E402
import rtti_ownership  # noqa: E402


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
        # ELF letters: content owns Registry (a strong member); core carries
        # a weak copy of Registry's typeinfo and references nothing.
        content = self.nm.archive("libcontent.a", CONTENT
                                  + "    _ZN7content8Registry4openEv T 0 0\n"
                                  + "    _ZTIN7content8RegistryE V 0 0\n")
        core = self.nm.archive("libcore.a", "libcore.a[x.o]:\n_ZTIN7content8RegistryE V 0 0\n")
        nm = self.nm.write()
        argv = ["--nm", nm, "--forbidden", str(content), "--forbidden-defines", "any",
                "--no-control", "--check", f"core={core}"]
        self.assertEqual(gate.main(argv), 0)
        self.assertEqual(gate.main([*argv, "--weak-rtti-contamination"]), 1)

    def test_a_missing_demangler_is_inconclusive(self) -> None:
        content = self.nm.archive("libcontent.a", CONTENT + "    _ZN7content8Registry4openEv T 0 0\n")
        core = self.nm.archive("libcore.a", "libcore.a[x.o]:\n_ZTIN7content8RegistryE V 0 0\n")
        nm = self.nm.write()
        self.assertEqual(gate.main(["--nm", nm, "--forbidden", str(content), "--no-control",
                                    "--check", f"core={core}", "--weak-rtti-contamination",
                                    "--cxxfilt", str(Path(self._tmp.name) / "no-such-cxxfilt")]), 2)


    def test_no_demangler_on_path_is_inconclusive(self) -> None:
        # Without --cxxfilt the demangler comes from PATH; with neither
        # c++filt nor llvm-cxxfilt there, the RTTI check cannot read
        # ownership and must say so rather than pass.
        content = self.nm.archive("libcontent.a", CONTENT + "    _ZN7content8Registry4openEv T 0 0\n")
        core = self.nm.archive("libcore.a", "libcore.a[x.o]:\n_ZTIN7content8RegistryE V 0 0\n")
        nm = self.nm.write()
        empty = Path(self._tmp.name) / "empty-path"
        empty.mkdir()
        with mock.patch.dict(os.environ, {"PATH": str(empty)}):
            with self.assertRaises(rtti_ownership.DemangleError):
                rtti_ownership.demangle({"_ZTIN7content8RegistryE"})
            self.assertEqual(gate.main(["--nm", nm, "--forbidden", str(content), "--no-control",
                                        "--check", f"core={core}", "--weak-rtti-contamination"]), 2)
        # Control: the same call with the demangler on PATH runs and finds the copy.
        self.assertEqual(gate.main(["--nm", nm, "--forbidden", str(content), "--no-control",
                                    "--check", f"core={core}", "--weak-rtti-contamination"]), 1)

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


class RttiOwnershipTest(unittest.TestCase):
    def test_rtti_family_includes_vtt_and_construction_vtables(self) -> None:
        for name in ("__ZTIN3foo3BarE", "_ZTVN3foo3BarE", "_ZTSN3foo3BarE", "_ZTTN3foo3BarE",
                     "__ZTCN3foo3BarE0_NS_4BaseE"):
            self.assertTrue(rtti_ownership.RTTI_MANGLED.match(name), name)
        self.assertFalse(rtti_ownership.RTTI_MANGLED.match("_ZN3foo3Bar3bazEv"))
        self.assertEqual(rtti_ownership.rtti_class("VTT for foo::Bar"), "foo::Bar")
        self.assertEqual(rtti_ownership.rtti_class("construction vtable for foo::Base-in-foo::Bar"), "foo::Bar")
        self.assertEqual(rtti_ownership.rtti_class("typeinfo name for foo::Bar<int>"), "foo::Bar<int>")

    def test_demangle_never_lets_the_tool_strip_an_underscore(self) -> None:
        # A stand-in for the c++filt on GitHub-hosted macOS: it strips one
        # leading underscore unless told -n, then demangles only "_Z" names.
        with tempfile.TemporaryDirectory() as tmp:
            tool = Path(tmp) / "c++filt"
            tool.write_text(
                f"#!{sys.executable}\n"
                "import sys\n"
                "strip = '-n' not in sys.argv[1:]\n"
                "known = {'_ZTIN7content8RegistryE': 'typeinfo for content::Registry'}\n"
                "for line in sys.stdin.read().splitlines():\n"
                "    name = line[1:] if strip and line.startswith('_') else line\n"
                "    print(known.get(name, line))\n",
                encoding="utf-8")
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
            names = rtti_ownership.demangle(
                {"_ZTIN7content8RegistryE", "__ZTIN7content8RegistryE"}, str(tool))
        # Both the ELF spelling and the Mach-O one (extra underscore) demangle.
        self.assertEqual(set(names.values()), {"typeinfo for content::Registry"})

    def test_member_owner_reads_the_enclosing_class(self) -> None:
        cases = {
            "pulp::view::Meter::set_level(float, float)": "pulp::view::Meter",
            "non-virtual thunk to pulp::view::Meter::~Meter()": "pulp::view::Meter",
            "guard variable for pulp::view::Meter::instance()::s": "pulp::view::Meter",
            "virtual thunk to pulp::view::Meter::~Meter()": "pulp::view::Meter",
            "covariant return thunk to pulp::view::Meter::clone() const": "pulp::view::Meter",
            "reference temporary #0 for pulp::view::Meter::kDefault": "pulp::view::Meter",
            "TLS init function for pulp::view::Meter::tls_state": "pulp::view::Meter",
            "thread-local wrapper routine for pulp::view::Meter::tls_state": "pulp::view::Meter",
            "void pulp::view::Grid<int>::fill<float>(float)": "pulp::view::Grid<int>",
            "pulp::view::Meter::operator()(int) const": "pulp::view::Meter",
            "pulp::view::Meter::kMaxLevel": "pulp::view::Meter",
            "pulp::view::register_audio_widgets()": "pulp::view",
            "main": None,
        }
        for demangled, owner in cases.items():
            self.assertEqual(rtti_ownership.member_owner(demangled), owner, demangled)
        self.assertIsNone(rtti_ownership.member_owner("vtable for pulp::view::Meter"))

    def test_std_and_closures_are_never_flagged(self) -> None:
        for cls in ("std::__1::__function::__func<pulp::view::Meter::bind()::'lambda'(), void ()>",
                    "std::format_error", "pulp::view::Meter::bind()::'lambda'()",
                    "pulp::view::Meter::bind()::{lambda()#1}", "pulp::view::$_0",
                    "(anonymous namespace)::Helper"):
            self.assertTrue(rtti_ownership.never_flagged(cls), cls)
        self.assertFalse(rtti_ownership.never_flagged("pulp::view::Meter"))

    def test_only_weak_only_rtti_of_owned_classes_is_copied(self) -> None:
        names = {"S1": "pulp::view::Meter::set_level(float, float)",
                 "_ZTVN4pulp4view5MeterE": "vtable for pulp::view::Meter",
                 "_ZTVN4pulp4view4ViewE": "vtable for pulp::view::View",
                 "_ZTVSt12format_error": "vtable for std::format_error"}
        owned = rtti_ownership.owned_classes({"S1", "_ZTVN4pulp4view5MeterE"}, names)
        self.assertEqual(owned, {"pulp::view::Meter"})
        weak = {"_ZTVN4pulp4view5MeterE", "_ZTVN4pulp4view4ViewE", "_ZTVSt12format_error"}
        self.assertEqual(rtti_ownership.copied_rtti(weak, owned, names),
                         {"_ZTVN4pulp4view5MeterE": "vtable for pulp::view::Meter"})

    def test_owned_std_or_closure_rtti_is_still_never_copied(self) -> None:
        # Even where the forbidden archive strongly defines a member of a std
        # class or a closure, a weak copy of that RTTI is shared, not copied.
        names = {"S1": "std::format_error::what() const",
                 "S2": "pulp::view::Meter::bind()::'lambda'()::operator()() const",
                 "_ZTVSt12format_error": "vtable for std::format_error",
                 "W2": "typeinfo for pulp::view::Meter::bind()::'lambda'()"}
        owned = {"std::format_error", "pulp::view::Meter::bind()::'lambda'()"}
        self.assertEqual(rtti_ownership.copied_rtti({"_ZTVSt12format_error", "W2"}, owned, names), {})

    def test_a_symbol_also_defined_strongly_is_not_weak_only(self) -> None:
        # The "- strong" qualifier: one member's strong definition wins over
        # another member's weak copy, in both readings.
        letters = "a.a[x.o]: _ZTVN3foo3BarE V 0 0\na.a[y.o]: _ZTVN3foo3BarE D 0 0\n"
        self.assertEqual(nm_symbols.weak_defined(letters), set())
        darwin = ("x.o:\n0000000000000010 (__DATA,__const) weak external automatically hidden __ZTVN3foo3BarE\n"
                  "y.o:\n0000000000000020 (__DATA,__const) external __ZTVN3foo3BarE\n")
        strong, weak = nm_symbols.strength("", darwin)
        self.assertEqual((strong, weak), ({"__ZTVN3foo3BarE"}, set()))


if __name__ == "__main__":
    unittest.main()
