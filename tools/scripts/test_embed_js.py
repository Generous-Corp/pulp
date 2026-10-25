"""Focused unit coverage for core/view/js/embed_js.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
import hashlib
import re
import zlib
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "core" / "view" / "js" / "embed_js.py"

spec = importlib.util.spec_from_file_location("embed_js", SCRIPT)
assert spec is not None
embed_js = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(embed_js)


RECORD = re.compile(
    r"static const unsigned char (?P<var>\w+)_deflated\[\] = \{\n(?P<body>.*?)\n\};\n"
    r"const EmbeddedPrelude (?P=var)\{\"(?P<file>[^\"]+)\", (?P=var)_deflated, "
    r"(?P<dsize>\d+), (?P<ssize>\d+), \"(?P<sha>[0-9a-f]{64})\"\};\n",
    re.S,
)


def parse_records(text: str) -> list[dict]:
    records = []
    for m in RECORD.finditer(text):
        values = [int(v) for v in m.group("body").replace("\n", "").split(",") if v.strip()]
        records.append({
            "var": m.group("var"),
            "file_name": m.group("file"),
            "deflated_size": int(m.group("dsize")),
            "source_size": int(m.group("ssize")),
            "sha256": m.group("sha"),
            "bytes": bytes(values),
        })
    return records


class EmbedJsTests(unittest.TestCase):
    def test_var_name_replaces_hyphens_only_in_stem(self) -> None:
        self.assertEqual(embed_js.var_name_for(Path("alpha-beta.js")), "alpha_beta")
        self.assertEqual(embed_js.var_name_for(Path("alpha.beta.js")), "alpha.beta")
        self.assertEqual(embed_js.var_name_for(Path("/tmp/nested/web-compat.js")), "web_compat")
        self.assertEqual(embed_js.var_name_for(Path("already_clean")), "already_clean")

    def test_compress_round_trips_through_zlib(self) -> None:
        text = ("globalThis.label = 'π';\n" * 200).encode("utf-8")
        deflated = embed_js.compress(text)
        self.assertLess(len(deflated), len(text) // 4)
        self.assertEqual(zlib.decompress(deflated), text)
        self.assertEqual(deflated[:1], b"\x78")  # RFC 1950 header, not raw deflate or gzip

    def test_byte_lines_cover_every_byte_and_empty_input(self) -> None:
        data = bytes(range(45))
        lines = embed_js.byte_lines(data)
        self.assertEqual(len(lines), 3)
        values = [int(v) for line in lines for v in line.strip().rstrip(",").split(",")]
        self.assertEqual(values, list(data))
        self.assertEqual(embed_js.byte_lines(b""), ["    0"])

    def test_main_emits_compressed_prelude_records_that_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.cpp"
            alpha = root / "alpha-one.js"
            empty = root / "empty.js"
            alpha_text = "alpha('π');\n" * 50
            alpha.write_text(alpha_text, encoding="utf-8")
            empty.write_text("", encoding="utf-8")

            result = embed_js.main(["embed_js.py", str(output), str(alpha), str(empty)])

            self.assertEqual(result, 0)
            text = output.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("// Auto-generated from JS prelude files"))
            self.assertIn('#include "web_compat_preludes_gen.hpp"\n\n', text)
            self.assertIn("namespace pulp::view::preludes {\n\n", text)
            self.assertNotIn("R\"__JS__(", text)
            self.assertNotIn("alpha('", text)  # the source text itself is not embedded

            records = parse_records(text)
            self.assertEqual([r["var"] for r in records], ["alpha_one", "empty"])
            raw = alpha_text.encode("utf-8")
            alpha_rec = records[0]
            self.assertEqual(alpha_rec["file_name"], "alpha-one.js")
            self.assertEqual(alpha_rec["source_size"], len(raw))
            self.assertEqual(alpha_rec["deflated_size"], len(alpha_rec["bytes"]))
            self.assertEqual(zlib.decompress(alpha_rec["bytes"]), raw)
            self.assertEqual(alpha_rec["sha256"], hashlib.sha256(raw).hexdigest())
            self.assertEqual(zlib.decompress(records[1]["bytes"]), b"")
            self.assertEqual(records[1]["source_size"], 0)

            self.assertIn("const EmbeddedPrelude* const kAll[] = {\n    &alpha_one,\n    &empty,\n};\n", text)
            self.assertIn("const std::size_t kAllCount = 2;\n", text)
            self.assertTrue(text.endswith("}  // namespace pulp::view::preludes\n"))

    def test_main_preserves_input_order(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "preludes.cpp"
            first = root / "first-file.js"
            second = root / "second-file.js"
            first.write_text("globalThis.a = 1;\n", encoding="utf-8")
            second.write_text("globalThis.b = 2;\n", encoding="utf-8")

            self.assertEqual(embed_js.main(["embed_js.py", str(output), str(first), str(second)]), 0)

            records = parse_records(output.read_text(encoding="utf-8"))
            self.assertEqual([r["var"] for r in records], ["first_file", "second_file"])

    def test_main_rejects_non_utf8_prelude(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "out.cpp"
            bad = root / "bad.js"
            bad.write_bytes(b"\xff\xfe not utf-8")

            with self.assertRaises(UnicodeDecodeError):
                embed_js.main(["embed_js.py", str(output), str(bad)])

            self.assertFalse(output.exists())

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
            self.assertIn("const EmbeddedPrelude entry{\"entry.js\", entry_deflated, ", text)
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
