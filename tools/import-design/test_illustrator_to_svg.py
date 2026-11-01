#!/usr/bin/env python3
"""Focused tests for the PDF-compatible Illustrator to SVG boundary."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest

from illustrator_to_svg import ConversionError, convert, main


FAKE_CONVERTER = """#!/usr/bin/env python3
import pathlib
import sys

if len(sys.argv) == 2 and sys.argv[1] == '-v':
    print('pdftocairo version 24.08.0')
    raise SystemExit(0)
if '-svg' not in sys.argv:
    raise SystemExit(9)
out = pathlib.Path(sys.argv[-1])
out.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="10" height="8"><path d="M0 0"/></svg>\\n', encoding='utf-8')
"""


class IllustratorToSvgTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "panel.ai"
        self.source.write_bytes(b"%PDF-1.7\\nillustrator fixture\\n")
        self.tool = self.root / "pdftocairo-fake"
        self.tool.write_text(FAKE_CONVERTER, encoding="utf-8")
        self.tool.chmod(self.tool.stat().st_mode | stat.S_IXUSR)
        self.old_override = os.environ.get("PULP_PDF_TO_SVG")
        os.environ["PULP_PDF_TO_SVG"] = str(self.tool)

    def tearDown(self) -> None:
        if self.old_override is None:
            os.environ.pop("PULP_PDF_TO_SVG", None)
        else:
            os.environ["PULP_PDF_TO_SVG"] = self.old_override
        self.temp.cleanup()

    def test_conversion_writes_svg_and_hash_bound_provenance(self) -> None:
        output = self.root / "out" / "panel.svg"
        receipt_path = self.root / "out" / "panel.receipt.json"
        receipt = convert(self.source, output, receipt_path, page=2)

        self.assertTrue(output.is_file())
        self.assertEqual(receipt["schema"], "pulp.illustrator-svg-provenance.v1")
        self.assertEqual(receipt["source"]["format"], "ai")
        self.assertEqual(receipt["converter"]["page"], 2)
        self.assertEqual(receipt["converter"]["version"], "pdftocairo version 24.08.0")
        self.assertEqual(receipt["source"]["sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertEqual(receipt["output"]["sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
        on_disk = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(on_disk, receipt)

    def test_default_paths_are_next_to_source(self) -> None:
        receipt = convert(self.source)
        output = self.root / "panel.svg"
        self.assertTrue(output.is_file())
        self.assertTrue((self.root / "panel.svg.provenance.json").is_file())
        self.assertEqual(receipt["output"]["path"], str(output.resolve()))

    def test_unsupported_source_fails_closed(self) -> None:
        source = self.root / "panel.svg"
        source.write_text("<svg></svg>", encoding="utf-8")
        with self.assertRaisesRegex(ConversionError, "unsupported source extension"):
            convert(source)

    def test_missing_converter_fails_closed(self) -> None:
        os.environ["PULP_PDF_TO_SVG"] = str(self.root / "missing")
        with self.assertRaisesRegex(ConversionError, "pdftocairo is not an executable file"):
            convert(self.source)

    def test_symlink_input_is_rejected(self) -> None:
        link = self.root / "alias.ai"
        link.symlink_to(self.source)
        with self.assertRaisesRegex(ConversionError, "must not be a symlink"):
            convert(link)

    def test_cli_returns_nonzero_for_invalid_page(self) -> None:
        self.assertEqual(main([str(self.source), "--page", "0"]), 2)


if __name__ == "__main__":
    unittest.main()
