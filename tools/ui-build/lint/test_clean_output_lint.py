#!/usr/bin/env python3
import json
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from clean_output_lint import lint_source

# CTest input tracking: these are consumed through subprocesses, dynamic
# imports, and the checked-in provenance fixture rather than Python imports.
# "tools/ui-build/ui_build.py"
# "tools/ui-build/lint/clean_output_lint.py"
# "tools/ui-build/lint/fixtures/generated"
# "test/fixtures/imports/claude/2024.10"


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

    def test_multiline_duplicate_subtree_fails(self):
        report = self._report(
            """export function Panel() {
  return <>
    <div className="duplicate">
      <span>same</span>
    </div>
    <div className="duplicate">
      <span>same</span>
    </div>
  </>;
}
"""
        )
        duplicates = [f for f in report["findings"] if f["code"] == "duplicate-markup"]
        self.assertFalse(report["ok"], report)
        self.assertTrue(duplicates, report)
        self.assertTrue(any("across lines" in f["message"] for f in duplicates), report)

    def test_distinct_multiline_subtrees_pass(self):
        report = self._report(
            """export function Panel() {
  return <>
    <div className="first">one</div>
    <div className="second">two</div>
  </>;
}
"""
        )
        self.assertTrue(report["ok"], report)

    def test_multiline_markup_in_strings_and_comments_is_ignored(self):
        report = self._report(
            """const documentation = `
  <div className="duplicate">
    <span>same</span>
  </div>
  <div className="duplicate">
    <span>same</span>
  </div>
`;
/*
  <aside>
    <span>comment</span>
  </aside>
  <aside>
    <span>comment</span>
  </aside>
*/
"""
        )
        self.assertTrue(report["ok"], report)

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

    def test_checked_in_corpus_binds_source_fixture_provenance(self):
        lint = Path(__file__).with_name("clean_output_lint.py")
        repo = Path(__file__).parents[3]
        source = repo / "tools/ui-build/lint/fixtures/generated"
        manifest = source / "manifest.json"
        result = subprocess.run(
            [sys.executable, str(lint), str(source), "--manifest", str(manifest), "--json"],
            text=True, capture_output=True, check=False, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["files"], 2)

        with tempfile.TemporaryDirectory(dir=repo) as directory:
            stale = Path(directory) / "manifest.json"
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["source_fixture_sha256"] = "0" * 64
            stale.write_text(json.dumps(document), encoding="utf-8")
            rejected = subprocess.run(
                [sys.executable, str(lint), str(source), "--manifest", str(stale), "--json"],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(rejected.returncode, 0)
            report = json.loads(rejected.stdout)
            self.assertEqual(report["findings"][0]["code"], "invalid-corpus-manifest")
            self.assertIn("source fixture hash mismatch", report["findings"][0]["message"])

        with tempfile.TemporaryDirectory() as outside, tempfile.TemporaryDirectory(dir=repo) as directory:
            outside_fixture = Path(outside) / "example.html"
            outside_fixture.write_bytes(
                (repo / "test/fixtures/imports/claude/2024.10/example.html").read_bytes())
            symlink = Path(directory) / "source-link"
            symlink.symlink_to(Path(outside))
            escaped = Path(directory) / "manifest.json"
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["source_fixture"] = f"{Path(directory).name}/source-link/example.html"
            escaped.write_text(json.dumps(document), encoding="utf-8")
            rejected = subprocess.run(
                [sys.executable, str(lint), str(source), "--manifest", str(escaped), "--json"],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(rejected.returncode, 0)
            report = json.loads(rejected.stdout)
            self.assertEqual(report["findings"][0]["code"], "invalid-corpus-manifest")
            self.assertIn("symlink", report["findings"][0]["message"])

    def test_manifest_binds_the_complete_sorted_source_set(self):
        lint = Path(__file__).with_name("clean_output_lint.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = {
                "Alpha.tsx": 'export function Alpha() { return <button data-pulp-action="a">a</button>; }\n',
                "Beta.tsx": 'export function Beta() { return <button data-pulp-action="b">b</button>; }\n',
            }
            for name, contents in files.items():
                (root / name).write_text(contents, encoding="utf-8")
            manifest = root / "manifest.json"

            def write_manifest(paths):
                manifest.write_text(json.dumps({
                    "schema": "pulp.clean-output-corpus.v1",
                    "producer": "pulp import-design --emit source",
                    "files": [{
                        "path": name,
                        "sha256": hashlib.sha256((root / name).read_bytes()).hexdigest(),
                    } for name in paths],
                }), encoding="utf-8")

            write_manifest(["Alpha.tsx"])
            omitted = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(omitted.returncode, 0)
            self.assertIn("omits source file(s): Beta.tsx", omitted.stdout)

            write_manifest(["Beta.tsx", "Alpha.tsx"])
            unsorted = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(unsorted.returncode, 0)
            self.assertIn("file paths are not sorted", unsorted.stdout)

            write_manifest(["Alpha.tsx", "Alpha.tsx"])
            duplicate = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(duplicate.returncode, 0)
            self.assertIn("duplicate file path", duplicate.stdout)

    def test_manifest_rejects_noncanonical_and_symlink_entries(self):
        lint = Path(__file__).with_name("clean_output_lint.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Captured.tsx"
            source.write_text(
                'export function Captured() { return <button data-pulp-action="x">x</button>; }\n',
                encoding="utf-8")
            manifest = root / "manifest.json"

            def run_with_path(path):
                manifest.write_text(json.dumps({
                    "schema": "pulp.clean-output-corpus.v1",
                    "producer": "pulp import-design --emit source",
                    "files": [{"path": path,
                               "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}],
                }), encoding="utf-8")
                return subprocess.run(
                    [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                    text=True, capture_output=True, check=False, encoding="utf-8")

            noncanonical = run_with_path("./Captured.tsx")
            self.assertNotEqual(noncanonical.returncode, 0)
            self.assertIn("path is not canonical", noncanonical.stdout)

            manifest.write_text("[]", encoding="utf-8")
            non_object = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest)],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(non_object.returncode, 0)
            self.assertIn("manifest must be a JSON object", non_object.stdout)

            nul = run_with_path("Captured\x00.tsx")
            self.assertNotEqual(nul.returncode, 0)
            self.assertIn("path is not canonical", nul.stdout)

            link = root / "Linked.tsx"
            link.symlink_to(source)
            linked = run_with_path("Linked.tsx")
            self.assertNotEqual(linked.returncode, 0)
            self.assertIn("must not be a symlink", linked.stdout)

    def test_manifest_requires_known_roles(self):
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
                "files": [{"path": source.name, "role": "mystery",
                           "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}],
            }), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(lint), str(root), "--manifest", str(manifest), "--json"],
                text=True, capture_output=True, check=False, encoding="utf-8")
            self.assertNotEqual(result.returncode, 0)
            report = json.loads(result.stdout)
            self.assertEqual(report["findings"][0]["code"], "invalid-corpus-manifest")
            self.assertIn("invalid file role", report["findings"][0]["message"])

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
