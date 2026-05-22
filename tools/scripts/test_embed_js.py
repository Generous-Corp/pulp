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
        self.assertEqual(embed_js.var_name_for(Path("/tmp/nested/web-compat.js")), "web_compat")
        self.assertEqual(embed_js.var_name_for(Path("already_clean")), "already_clean")

    def test_chunk_text_splits_and_keeps_empty_input_represented(self) -> None:
        self.assertEqual(embed_js.chunk_text("abcdef", 2), ["ab", "cd", "ef"])
        self.assertEqual(embed_js.chunk_text("abcde", 2), ["ab", "cd", "e"])
        self.assertEqual(embed_js.chunk_text("emoji-π", 6), ["emoji-", "π"])
        self.assertEqual(embed_js.chunk_text("same", 99), ["same"])
        self.assertEqual(embed_js.chunk_text("", 4000), [""])

    def test_chunk_text_respects_default_msvc_boundary(self) -> None:
        self.assertEqual(len(embed_js.chunk_text("a" * (embed_js.CHUNK_SIZE - 1), embed_js.CHUNK_SIZE)), 1)
        self.assertEqual(len(embed_js.chunk_text("a" * embed_js.CHUNK_SIZE, embed_js.CHUNK_SIZE)), 1)
        self.assertEqual(len(embed_js.chunk_text("a" * (embed_js.CHUNK_SIZE + 1), embed_js.CHUNK_SIZE)), 2)
        self.assertEqual(embed_js.chunk_text("a" * (embed_js.CHUNK_SIZE + 1), embed_js.CHUNK_SIZE)[1], "a")
        self.assertEqual(embed_js.chunk_text("π" * (embed_js.CHUNK_SIZE + 1), embed_js.CHUNK_SIZE)[1], "π")

    def test_main_writes_cpp_translation_unit_with_chunking_and_empty_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.cpp"
            first = root / "alpha-beta.js"
            second = root / "empty.js"
            first.write_text("a" * 4001, encoding="utf-8")
            second.write_text("", encoding="utf-8")

            result = embed_js.main(["embed_js.py", str(output), str(first), str(second)])

            self.assertEqual(result, 0)
            text = output.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("// Auto-generated from JS prelude files"))
            self.assertIn('#include "web_compat_preludes_gen.hpp"\n\n', text)
            self.assertIn("namespace pulp::view::preludes {\n\n", text)
            self.assertIn("const char* const alpha_beta =\n", text)
            self.assertIn('R"__JS__(' + ("a" * 4000) + ')__JS__"\n', text)
            self.assertIn('R"__JS__(a)__JS__"\n', text)
            self.assertIn("const char* const empty =\n", text)
            self.assertIn('R"__JS__()__JS__"\n;\n\n', text)
            self.assertNotIn("static const char*", text)
            self.assertTrue(text.endswith("}  // namespace pulp::view::preludes\n"))

    def test_main_preserves_input_order_and_utf8_text(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.cpp"
            first = root / "first-file.js"
            second = root / "second-file.js"
            first.write_text("globalThis.label = 'π';\n", encoding="utf-8")
            second.write_text("globalThis.answer = 42;\n", encoding="utf-8")

            result = embed_js.main(["embed_js.py", str(output), str(first), str(second)])

            text = output.read_text(encoding="utf-8")
            self.assertEqual(result, 0)
            self.assertLess(text.index("const char* const first_file"), text.index("const char* const second_file"))
            self.assertIn("globalThis.label = 'π';\n", text)
            self.assertIn("globalThis.answer = 42;\n", text)
            self.assertEqual(text.count("R\"__JS__("), 2)

    def test_main_emits_one_definition_per_input(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.cpp"
            alpha = root / "alpha-one.js"
            beta = root / "beta-two.js"
            alpha.write_text("alpha();", encoding="utf-8")
            beta.write_text("beta();", encoding="utf-8")

            result = embed_js.main(["embed_js.py", str(output), str(alpha), str(beta)])

            text = output.read_text(encoding="utf-8")
            self.assertEqual(result, 0)
            self.assertEqual(text.count("const char* const "), 2)
            self.assertIn("const char* const alpha_one =\n", text)
            self.assertIn("const char* const beta_two =\n", text)
            self.assertEqual(text.count(";\n\n"), 2)
            self.assertIn('R"__JS__(alpha();)__JS__"\n', text)
            self.assertIn('R"__JS__(beta();)__JS__"\n', text)

    def test_main_does_not_create_output_for_usage_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "out.cpp"
            stderr = io.StringIO()

            with contextlib.redirect_stderr(stderr):
                result = embed_js.main(["embed_js.py", str(output)])

            self.assertEqual(result, 1)
            self.assertFalse(output.exists())
            self.assertIn("<output.cpp>", stderr.getvalue())

    def test_main_prints_usage_when_arguments_are_missing(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = embed_js.main(["embed_js.py", "out.cpp"])

        self.assertEqual(result, 1)
        self.assertEqual(
            stderr.getvalue(),
            "usage: embed_js.py <output.cpp> <input1.js> [input2.js ...]\n",
        )

    def test_main_surfaces_missing_input_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = Path(td) / "missing.js"
            output = Path(td) / "out.cpp"

            with self.assertRaises(FileNotFoundError):
                embed_js.main(["embed_js.py", str(output), str(missing)])

            self.assertFalse(output.exists())

    def test_main_leaves_existing_output_untouched_when_read_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "out.cpp"
            output.write_text("previous contents\n", encoding="utf-8")
            missing = root / "missing.js"

            with self.assertRaises(FileNotFoundError):
                embed_js.main(["embed_js.py", str(output), str(missing)])

            self.assertEqual(output.read_text(encoding="utf-8"), "previous contents\n")

    def test_script_entry_point_writes_cpp_translation_unit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.cpp"
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
            text = output.read_text(encoding="utf-8")
            self.assertIn("const char* const entry =\n", text)
            self.assertIn('#include "web_compat_preludes_gen.hpp"', text)
            self.assertIn("namespace pulp::view::preludes", text)

    def test_script_entry_point_reports_usage_error(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "out.cpp"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertEqual(
            result.stderr,
            "usage: embed_js.py <output.cpp> <input1.js> [input2.js ...]\n",
        )


if __name__ == "__main__":
    unittest.main()
