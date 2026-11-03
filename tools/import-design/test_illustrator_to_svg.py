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
from unittest import mock

from illustrator_to_svg import ConversionError, convert, main


FAKE_CONVERTER = """#!/usr/bin/env python3
import json
import os
import pathlib
import sys

if len(sys.argv) == 2 and sys.argv[1] == '-v':
    print('pdftocairo version 24.08.0')
    raise SystemExit(0)
if '-svg' not in sys.argv:
    raise SystemExit(9)
out = pathlib.Path(sys.argv[-1])
probe = os.environ.get("FAKE_PROBE_LOG")
if probe:
    pathlib.Path(probe).write_text(json.dumps({
        "existed": out.exists(),
        "parent": str(out.parent),
        "parent_mode": out.parent.stat().st_mode & 0o777,
    }), encoding="utf-8")
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

    def test_symlinked_provenance_is_rejected_and_target_untouched(self) -> None:
        victim = self.root / "victim.json"
        victim.write_text("keep", encoding="utf-8")
        link = self.root / "panel.receipt.json"
        link.symlink_to(victim)
        with self.assertRaisesRegex(ConversionError, "provenance must not be a symlink"):
            convert(self.source, self.root / "panel.svg", link)
        self.assertEqual(victim.read_text(encoding="utf-8"), "keep")
        self.assertFalse((self.root / "panel.svg").exists())

    def test_dangling_symlinked_provenance_is_rejected(self) -> None:
        link = self.root / "panel.receipt.json"
        link.symlink_to(self.root / "missing-dir" / "victim.json")
        with self.assertRaisesRegex(ConversionError, "provenance must not be a symlink"):
            convert(self.source, self.root / "panel.svg", link)
        self.assertFalse((self.root / "missing-dir").exists())

    def test_symlinked_default_provenance_is_rejected(self) -> None:
        victim = self.root / "victim.json"
        victim.write_text("keep", encoding="utf-8")
        (self.root / "panel.svg.provenance.json").symlink_to(victim)
        with self.assertRaisesRegex(ConversionError, "provenance must not be a symlink"):
            convert(self.source)
        self.assertEqual(victim.read_text(encoding="utf-8"), "keep")

    def test_preplanted_receipt_temporary_is_not_followed(self) -> None:
        victim = self.root / "victim.json"
        victim.write_text("keep", encoding="utf-8")
        provenance = self.root / "panel.receipt.json"
        # The old fixed temporary name for this receipt.
        (self.root / ".panel.receipt.json.tmp").symlink_to(victim)
        convert(self.source, self.root / "panel.svg", provenance)
        self.assertEqual(victim.read_text(encoding="utf-8"), "keep")
        self.assertEqual(json.loads(provenance.read_text(encoding="utf-8"))["schema"],
                         "pulp.illustrator-svg-provenance.v1")

    def test_symlinked_output_is_rejected_and_target_untouched(self) -> None:
        victim = self.root / "victim.svg"
        victim.write_text("keep", encoding="utf-8")
        link = self.root / "panel.svg"
        link.symlink_to(victim)
        with self.assertRaisesRegex(ConversionError, "output must not be a symlink"):
            convert(self.source, link)
        self.assertEqual(victim.read_text(encoding="utf-8"), "keep")

    def test_output_and_provenance_may_not_overwrite_inputs(self) -> None:
        source = self.root / "art.pdf"
        source.write_bytes(b"%PDF-1.4\n")
        with self.assertRaisesRegex(ConversionError, "output must not overwrite the source"):
            convert(source, source)
        with self.assertRaisesRegex(ConversionError, "provenance must not overwrite"):
            convert(self.source, self.root / "panel.svg", self.source)
        with self.assertRaisesRegex(ConversionError, "provenance must not overwrite"):
            convert(self.source, self.root / "panel.svg", self.root / "panel.svg")
        self.assertEqual(source.read_bytes(), b"%PDF-1.4\n")

    def test_no_temporaries_left_after_success(self) -> None:
        convert(self.source)
        self.assertEqual(sorted(p.name for p in self.root.iterdir() if p.name.startswith(".")), [])

    def test_case_variant_output_does_not_overwrite_the_source(self) -> None:
        source = self.root / "art.pdf"
        source.write_bytes(b"%PDF-1.4\n")
        with self.assertRaisesRegex(ConversionError, "output must not overwrite the source"):
            convert(source, self.root / "ART.PDF")
        self.assertEqual(source.read_bytes(), b"%PDF-1.4\n")

    def test_case_variant_provenance_does_not_overwrite_the_source(self) -> None:
        before = self.source.read_bytes()
        with self.assertRaisesRegex(ConversionError, "provenance must not overwrite"):
            convert(self.source, self.root / "p.svg", self.root / "Panel.Ai")
        self.assertEqual(self.source.read_bytes(), before)
        self.assertFalse((self.root / "p.svg").exists())

    def test_case_variant_provenance_does_not_overwrite_the_output(self) -> None:
        with self.assertRaisesRegex(ConversionError, "provenance must not overwrite"):
            convert(self.source, self.root / "panel.svg", self.root / "PANEL.svg")
        self.assertFalse((self.root / "panel.svg").exists())

    def test_case_variant_directory_is_compared_by_identity(self) -> None:
        (self.root / "out").mkdir()
        if not (self.root / "OUT").exists():
            self.skipTest("this volume is case-sensitive, so OUT is a different directory")
        with self.assertRaisesRegex(ConversionError, "provenance must not overwrite"):
            convert(self.source, self.root / "out" / "panel.svg", self.root / "OUT" / "Panel.SVG")

    def test_converter_writes_into_a_fresh_private_directory(self) -> None:
        probe = self.root / "probe.json"
        os.environ["FAKE_PROBE_LOG"] = str(probe)
        self.addCleanup(os.environ.pop, "FAKE_PROBE_LOG", None)
        output = self.root / "out" / "panel.svg"
        convert(self.source, output)
        seen = json.loads(probe.read_text(encoding="utf-8"))
        self.assertFalse(seen["existed"])
        self.assertEqual(seen["parent_mode"], 0o700)
        workdir = Path(seen["parent"])
        self.assertTrue(os.path.samefile(workdir.parent, output.parent))
        self.assertFalse(workdir.exists())

    def test_receipt_is_written_through_the_mkstemp_descriptor(self) -> None:
        with mock.patch.object(Path, "write_text", side_effect=AssertionError("path write")):
            receipt = convert(self.source)
        self.assertEqual(receipt["schema"], "pulp.illustrator-svg-provenance.v1")

    def test_cli_returns_nonzero_for_invalid_page(self) -> None:
        self.assertEqual(main([str(self.source), "--page", "0"]), 2)


if __name__ == "__main__":
    unittest.main()
