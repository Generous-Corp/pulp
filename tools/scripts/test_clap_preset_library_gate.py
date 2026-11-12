#!/usr/bin/env python3
"""The CLAP adapter's preset library compiles out on wasi (WebCLAP).

WebCLAP links a PresetManager stub and not the PresetLibrary sources, so any use
of the library in clap_adapter.cpp that is not inside `#if PULP_CLAP_PRESET_LIBRARY`
fails the wasm link. This preprocesses the gate header for a native and a wasi
target and checks every library use in the adapter sits inside such a block.

Run:
    python3 tools/scripts/test_clap_preset_library_gate.py
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "core/format/include/pulp/format/detail/clap_preset_library_gate.hpp"
ADAPTER_CPP = ROOT / "core/format/src/clap_adapter.cpp"
ADAPTER_HPP = ROOT / "core/format/include/pulp/format/clap_adapter.hpp"
USES = re.compile(r"(?<!clap_)preset_library|PresetLibrary")


def gate_value(*defines: str) -> int:
    compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if compiler is None:
        raise unittest.SkipTest("no C++ compiler on PATH")
    source = f'#include "{GATE}"\nVALUE=PULP_CLAP_PRESET_LIBRARY\n'
    out = subprocess.run(
        [compiler, "-E", "-P", "-x", "c++", *defines, "-"],
        input=source, capture_output=True, text=True, encoding="utf-8", check=True).stdout
    match = re.search(r"VALUE=(\d)", out)
    assert match, out
    return int(match.group(1))


def unguarded_uses(path: Path) -> list[str]:
    """Lines naming the preset library outside an `#if PULP_CLAP_PRESET_LIBRARY` block."""
    depth_stack: list[bool] = []  # one entry per open #if: True when it is the gate
    bad: list[str] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if re.match(r"#\s*if(def|ndef)?\b", stripped):
            depth_stack.append(bool(re.match(r"#\s*if\s+PULP_CLAP_PRESET_LIBRARY\b", stripped)))
        elif re.match(r"#\s*endif\b", stripped):
            if depth_stack:
                depth_stack.pop()
        elif re.match(r"#\s*else\b", stripped):
            if depth_stack and depth_stack[-1]:
                depth_stack[-1] = False  # the else arm is the wasi arm
        elif USES.search(stripped) and not stripped.startswith(("//", "///", "*")):
            if not any(depth_stack):
                bad.append(f"{path.name}:{number}: {stripped}")
    return bad


class ClapPresetLibraryGateTests(unittest.TestCase):
    def test_the_library_is_built_natively_and_compiled_out_on_wasi(self) -> None:
        self.assertEqual(gate_value(), 1)
        self.assertEqual(gate_value("-D__wasi__"), 0)

    def test_every_library_use_in_the_adapter_is_inside_the_gate(self) -> None:
        self.assertEqual(unguarded_uses(ADAPTER_CPP), [])
        self.assertEqual(unguarded_uses(ADAPTER_HPP), [])

    def test_the_scan_finds_uses_to_check(self) -> None:
        # Control: a scan that matched no uses would report "none unguarded".
        text = ADAPTER_CPP.read_text(encoding="utf-8")
        self.assertGreaterEqual(len(USES.findall(text)), 6)

    def test_an_unguarded_use_is_reported(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            sample = Path(tmp) / "sample.cpp"
            sample.write_text(
                "#if PULP_CLAP_PRESET_LIBRARY\nauto a = preset_library;\n#endif\n"
                "auto b = preset_library;\n"
                "#if PULP_CLAP_PRESET_LIBRARY\n#else\nauto c = preset_library;\n#endif\n",
                encoding="utf-8")
            bad = unguarded_uses(sample)
            self.assertEqual(len(bad), 2, bad)
            self.assertIn("sample.cpp:4", bad[0])
            self.assertIn("sample.cpp:7", bad[1])


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=2))
