#!/usr/bin/env python3
"""Additional edge coverage for jsfx_subset.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import textwrap
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "jsfx_subset.py"

spec = importlib.util.spec_from_file_location("jsfx_subset", SCRIPT)
jsfx = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = jsfx
spec.loader.exec_module(jsfx)


class ParseJsfxEdgeTests(unittest.TestCase):
    def write_jsfx(self, content: str) -> pathlib.Path:
        handle = tempfile.NamedTemporaryFile("w", suffix=".jsfx", delete=False)
        handle.write(textwrap.dedent(content))
        handle.flush()
        handle.close()
        path = pathlib.Path(handle.name)
        self.addCleanup(lambda: path.unlink(missing_ok=True))
        return path

    def test_parse_rejects_missing_file(self) -> None:
        missing = pathlib.Path(tempfile.gettempdir()) / "pulp-missing-jsfx-fixture.jsfx"
        missing.unlink(missing_ok=True)
        with self.assertRaisesRegex(jsfx.JsfxSubsetError, "jsfx file not found"):
            jsfx.parse_jsfx(missing)

    def test_parse_rejects_missing_desc(self) -> None:
        path = self.write_jsfx(
            """
            @sample
              spl0 *= 0.5;
            """
        )
        with self.assertRaisesRegex(jsfx.JsfxSubsetError, "missing desc"):
            jsfx.parse_jsfx(path)

    def test_parse_rejects_missing_sections(self) -> None:
        path = self.write_jsfx(
            """
            desc:No Sections
            slider1:1<0,2,0.01>Gain
            """
        )
        with self.assertRaisesRegex(jsfx.JsfxSubsetError, "no JSFX sections"):
            jsfx.parse_jsfx(path)

    def test_print_human_handles_no_sliders(self) -> None:
        path = self.write_jsfx(
            """
            desc:No Sliders
            @sample
              spl0 = -spl0;
            """
        )
        summary = jsfx.parse_jsfx(path)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            jsfx.print_human(summary)

        text = output.getvalue()
        self.assertIn("slider_count: 0", text)
        self.assertIn("unsupported_sections: none", text)
        self.assertNotIn("sliders:", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
