"""Focused unit coverage for core/view/js/embed_js.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "core" / "view" / "js" / "embed_js.py"

spec = importlib.util.spec_from_file_location("embed_js", SCRIPT)
assert spec is not None
embed_js = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(embed_js)


class EmbedJsTests(unittest.TestCase):
    def test_var_name_replaces_hyphens_only_in_stem(self) -> None:
        self.assertEqual(embed_js.var_name_for(Path("alpha-beta.js")), "alpha_beta")
        self.assertEqual(embed_js.var_name_for(Path("alpha.beta.js")), "alpha.beta")

    def test_chunk_text_splits_and_keeps_empty_input_represented(self) -> None:
        self.assertEqual(embed_js.chunk_text("abcdef", 2), ["ab", "cd", "ef"])
        self.assertEqual(embed_js.chunk_text("", 4000), [""])

    def test_main_embeds_inputs_with_chunking_and_empty_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.hpp"
            first = root / "alpha-beta.js"
            second = root / "empty.js"
            first.write_text("a" * 4001, encoding="utf-8")
            second.write_text("", encoding="utf-8")

            result = embed_js.main(["embed_js.py", str(output), str(first), str(second)])

            self.assertEqual(result, 0)
            text = output.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("#pragma once\n"))
            self.assertIn("namespace pulp::view::preludes {\n\n", text)
            self.assertIn("static const char* alpha_beta =\n", text)
            self.assertIn('R"__JS__(' + ("a" * 4000) + ')__JS__"\n', text)
            self.assertIn('R"__JS__(a)__JS__"\n', text)
            self.assertIn("static const char* empty =\n", text)
            self.assertIn('R"__JS__()__JS__"\n;\n\n', text)
            self.assertTrue(text.endswith("} // namespace pulp::view::preludes\n"))

    def test_main_prints_usage_when_arguments_are_missing(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = embed_js.main(["embed_js.py", "out.hpp"])

        self.assertEqual(result, 1)
        self.assertEqual(
            stderr.getvalue(),
            "usage: embed_js.py <output.hpp> <input1.js> [input2.js ...]\n",
        )

    def test_main_surfaces_missing_input_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = Path(td) / "missing.js"
            output = Path(td) / "out.hpp"

            with self.assertRaises(FileNotFoundError):
                embed_js.main(["embed_js.py", str(output), str(missing)])

            self.assertFalse(output.exists())

    def test_script_entry_point_writes_header(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.hpp"
            source = root / "entry.js"
            source.write_text("globalThis.answer = 42;\n", encoding="utf-8")

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(output), str(source)],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertIn("static const char* entry =\n", output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
