#!/usr/bin/env python3
"""Self-test for embed_wgsl.py, the generator behind pulp_add_wgsl_shaders."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("embed_wgsl.py")


def run(src: pathlib.Path, out: pathlib.Path, var: str = "glow_wgsl") -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--src", str(src), "--out", str(out),
         "--namespace", "my::shaders", "--var", var],
        capture_output=True, text=True, encoding="utf-8", check=False)


class EmbedWgslTest(unittest.TestCase):
    def test_round_trips_bytes_and_length(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "glow.wgsl"
            source = "@fragment\nfn fs_main() -> @location(0) vec4f { return vec4f(1.0); }\n// é\n"
            src.write_bytes(source.encode("utf-8"))
            out = pathlib.Path(tmp) / "gen" / "glow_wgsl.hpp"
            self.assertEqual(run(src, out).returncode, 0)
            text = out.read_text(encoding="utf-8")
            self.assertIn("namespace my::shaders {", text)
            raw = source.encode("utf-8")
            self.assertIn(f"std::string_view glow_wgsl{{glow_wgsl_data, {len(raw)}}}", text)
            body = text[text.index("glow_wgsl_data[] = {"):text.index("};")]
            values = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body)]
            self.assertEqual(bytes(values[:-1]), raw)
            self.assertEqual(values[-1], 0, "array must be NUL-terminated")

    def test_unchanged_output_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "a.wgsl"
            src.write_text("fn f() {}\n", encoding="utf-8")
            out = pathlib.Path(tmp) / "a_wgsl.hpp"
            self.assertEqual(run(src, out).returncode, 0)
            first = out.stat().st_mtime_ns
            self.assertEqual(run(src, out).returncode, 0)
            self.assertEqual(out.stat().st_mtime_ns, first)

    def test_rejects_nul_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "bad.wgsl"
            src.write_bytes(b"fn f() {}\0\n")
            out = pathlib.Path(tmp) / "bad_wgsl.hpp"
            result = run(src, out)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
