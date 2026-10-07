#!/usr/bin/env python3
import json
import hashlib
import subprocess
import sys
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

    def test_missing_or_empty_fixture_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = lint_source(root / "missing")
            self.assertFalse(missing["ok"])
            self.assertEqual(missing["findings"][0]["code"], "missing-source-root")

            empty = root / "empty"
            empty.mkdir()
            report = lint_source(empty)
            self.assertFalse(report["ok"])
            self.assertEqual(report["findings"][0]["code"], "empty-source-root")

    def test_cli_negative_control_rejects_missing_and_empty_fixture(self):
        lint = Path(__file__).with_name("clean_output_lint.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = subprocess.run(
                [sys.executable, str(lint), str(root / "missing")],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("missing-source-root", missing.stdout)

            empty = root / "empty"
            empty.mkdir()
            result = subprocess.run(
                [sys.executable, str(lint), str(empty), "--json"],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stdout)["findings"][0]["code"],
                             "empty-source-root")

    def test_cli_manifest_binds_captured_output_bytes(self):
        lint = Path(__file__).with_name("clean_output_lint.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Captured.tsx"
            source.write_text(
                'export function Captured() { return <button data-pulp-action="x">x</button>; }\n',
                encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "schema": "pulp.clean-output-corpus.v1",
                "producer": "pulp import-design --emit source",
                "files": [{"path": source.name,
                           "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}],
            }), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            source.write_text(source.read_text(encoding="utf-8") + "// drift\n", encoding="utf-8")
            stale = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn("invalid-corpus-manifest", stale.stdout)

    def test_gates_runs_lint_when_fixture_directory_is_missing(self):
        gates = Path(__file__).parents[3] / "tools/scripts/gates.sh"
        text = gates.read_text(encoding="utf-8")
        start = text.index("# ── 0e. clean-output source fixture")
        end = text.index("# ── 1. skill-sync", start)
        block = text[start:end]
        self.assertIn('if [ ! -f "$CLEAN_OUTPUT_LINT" ]; then', block)
        self.assertIn('"$ROOT/tools/ui-build/lint/fixtures/clean"', block)
        self.assertIn('CLEAN_OUTPUT_CORPUS', block)
        self.assertNotIn('&& [ -d "$ROOT/tools/ui-build/lint/fixtures/clean" ]', block)
        self.assertIn("fail=1", block)


if __name__ == "__main__":
    unittest.main()
