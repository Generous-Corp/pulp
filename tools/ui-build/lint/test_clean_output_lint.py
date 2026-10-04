#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path

from clean_output_lint import lint_source


class CleanOutputLintTests(unittest.TestCase):
    def _report(self, source: str, name: str = "Panel.tsx"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / name).write_text(source, encoding="utf-8")
            return lint_source(root)

    def test_clean_semantic_source_passes(self):
        report = self._report(
            """export function FilterPanel({value}: {value: number}) {
  return <button data-pulp-action="filter" style={{color: tokens.text}}>{value}</button>;
}
"""
        )
        self.assertTrue(report["ok"], report)
        self.assertEqual(report["findings"], [])

    def test_planted_controls_fail(self):
        report = self._report(
            """export function Div_123() {
  return <div onClick={() => Date.now()} style={{color: '#fff'}}>x</div>;
}
export function Other() {
  return <><div className="duplicate">x</div><div className="duplicate">x</div></>;
}
"""
        )
        codes = {finding["code"] for finding in report["findings"]}
        self.assertFalse(report["ok"])
        self.assertTrue({"generic-name", "inline-static-style", "literal-color",
                         "nonsemantic-click-target", "nondeterministic-expression",
                         "duplicate-markup"} <= codes, report)

    def test_json_report_is_deterministic(self):
        first = self._report("export function Knob() { return <button data-pulp-action=\"x\">x</button>; }\n")
        second = self._report("export function Knob() { return <button data-pulp-action=\"x\">x</button>; }\n")
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True))


if __name__ == "__main__":
    unittest.main()
