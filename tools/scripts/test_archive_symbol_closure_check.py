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

    def run_gate(self, state: str) -> int:
        subject = self.nm.archive("libstate.a", state)
        runtime = self.nm.archive("libruntime.a", RUNTIME)
        content = self.nm.archive("libcontent.a", CONTENT)
        nm = self.nm.write()
        return gate.main(["--nm", nm, "--subject", str(subject),
                          "--forbidden", str(content), "--control", str(runtime)])

    def test_clean_subject_passes(self) -> None:
        self.assertEqual(self.run_gate(STATE), 0)

    def test_reference_to_forbidden_definition_fails(self) -> None:
        self.assertEqual(self.run_gate(STATE + "    _content_registry_ctor U\n"), 1)

    def test_internal_reference_is_not_a_dependency(self) -> None:
        # _state_helper is undefined in one member and defined in another.
        self.assertEqual(self.run_gate(STATE), 0)

    def test_weak_definition_is_not_a_dependency(self) -> None:
        self.assertEqual(self.run_gate(STATE + "    _inline_shared U\n"), 0)

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
