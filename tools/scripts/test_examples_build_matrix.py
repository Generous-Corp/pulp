#!/usr/bin/env python3
"""Tests for tools/scripts/examples_build_matrix.py."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import examples_build_matrix as matrix  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]


def t(name: str, kind: str = "EXECUTABLE") -> dict:
    return {"name": name, "type": kind}


class PrimaryTargetsTest(unittest.TestCase):
    def test_plugin_is_stood_for_by_its_clap_target(self):
        targets = [t("Foo_AU", "MODULE_LIBRARY"), t("Foo_CLAP", "MODULE_LIBRARY"),
                   t("Foo_Standalone"), t("Foo_CLAP_BuildInfo", "UTILITY"),
                   t("Foo_Core", "STATIC_LIBRARY")]
        self.assertEqual(matrix.primary_targets(targets), ["Foo_CLAP"])

    def test_standalone_is_used_when_there_is_no_clap(self):
        targets = [t("Foo_Standalone"), t("Foo_Core", "STATIC_LIBRARY")]
        self.assertEqual(matrix.primary_targets(targets), ["Foo_Standalone"])

    def test_plain_executables_and_tests_are_kept(self):
        targets = [t("pulp-foo"), t("pulp-foo-test")]
        self.assertEqual(matrix.primary_targets(targets), ["pulp-foo", "pulp-foo-test"])

    def test_bookkeeping_targets_are_left_out(self):
        targets = [t("Foo_CLAP", "MODULE_LIBRARY"),
                   t("Foo_CLAP_ControlShippingScan_CLAP", "UTILITY"),
                   t("Foo_AUv3Framework", "SHARED_LIBRARY")]
        self.assertEqual(matrix.primary_targets(targets), ["Foo_CLAP"])

    def test_no_targets_means_nothing_to_build(self):
        self.assertEqual(matrix.primary_targets([]), [])


class SelectedExamplesTest(unittest.TestCase):
    MANIFEST = {"examples": {
        "keep": {"class": "audio-only", "platform": "cross"},
        "ui": {"class": "moves-to-vellum", "platform": "cross"},
        "phone": {"class": "audio-only", "platform": "ios"},
        "old": {"class": "quarantined", "platform": "android"},
        "gone": {"class": "deleted", "platform": "macos"},
    }}

    def test_only_retained_macos_examples_are_selected(self):
        names = [n for n, _ in matrix.selected_examples(self.MANIFEST, [])]
        self.assertEqual(names, ["keep", "ui"])

    def test_only_filter_narrows_the_set(self):
        names = [n for n, _ in matrix.selected_examples(self.MANIFEST, ["ui"])]
        self.assertEqual(names, ["ui"])


class ReceiptTest(unittest.TestCase):
    def test_example_without_targets_is_not_configured_never_built(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs/status").mkdir(parents=True)
            (root / "docs/status/examples-manifest.json").write_text(json.dumps({"examples": {
                "gated": {"class": "moves-to-vellum", "platform": "cross", "gate": "PULP_HAS_RACK"},
            }}), encoding="utf-8")
            out = root / "receipts.json"
            original = matrix.example_targets
            matrix.example_targets = lambda _build: {}
            try:
                rc = matrix.main(["--repo-root", str(root), "--build-dir", str(root / "b"),
                                  "--out", str(out)])
            finally:
                matrix.example_targets = original
            self.assertEqual(rc, 0)
            receipt = json.loads(out.read_text(encoding="utf-8"))["examples"]["gated"]
            self.assertEqual(receipt["status"], "not-configured")
            self.assertEqual(receipt["reason"], "PULP_HAS_RACK")


class CheckRecordTest(unittest.TestCase):
    MANIFEST = SelectedExamplesTest.MANIFEST
    GOOD = {"examples": {
        "keep": {"status": "built", "targets": ["k"], "exit_code": 0},
        "ui": {"status": "not-configured", "reason": "PULP_HAS_RACK"},
    }}

    def test_complete_record_passes(self):
        self.assertEqual(matrix.check_record(self.MANIFEST, self.GOOD), [])

    def test_missing_receipt_fails(self):
        record = {"examples": {"keep": self.GOOD["examples"]["keep"]}}
        problems = matrix.check_record(self.MANIFEST, record)
        self.assertEqual(problems, ["ui: retained example has no build receipt"])

    def test_receipt_for_unknown_example_fails(self):
        record = {"examples": {**self.GOOD["examples"], "ghost": {"status": "built"}}}
        self.assertTrue(any(p.startswith("ghost:") for p in matrix.check_record(self.MANIFEST, record)))

    def test_failed_build_fails(self):
        record = {"examples": {**self.GOOD["examples"],
                               "keep": {"status": "failed", "targets": ["k"], "exit_code": 1}}}
        self.assertTrue(any("status must be" in p for p in matrix.check_record(self.MANIFEST, record)))

    def test_built_without_targets_fails(self):
        record = {"examples": {**self.GOOD["examples"], "keep": {"status": "built", "exit_code": 0}}}
        self.assertTrue(any("needs targets" in p for p in matrix.check_record(self.MANIFEST, record)))

    def test_missing_record_file_is_an_error_not_a_crash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs/status").mkdir(parents=True)
            (root / "docs/status/examples-manifest.json").write_text(
                json.dumps(self.MANIFEST), encoding="utf-8")
            self.assertEqual(matrix.main(["--repo-root", str(root), "--check-record"]), 1)


def write_reply(build: Path, source_root: str, target_sources: dict[str, str]) -> None:
    """A minimal file-API reply: one target per (name -> paths.source)."""
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    refs = []
    for name, source in target_sources.items():
        (reply / f"target-{name}.json").write_text(
            json.dumps({"name": name, "type": "EXECUTABLE", "paths": {"source": source}}),
            encoding="utf-8")
        refs.append({"jsonFile": f"target-{name}.json"})
    (reply / "codemodel.json").write_text(json.dumps({
        "paths": {"source": source_root, "build": str(build)},
        "configurations": [{"targets": refs}]}), encoding="utf-8")
    (reply / "index-1.json").write_text(json.dumps({
        "objects": [{"kind": "codemodel", "jsonFile": "codemodel.json"}]}), encoding="utf-8")


class ExampleTargetsTest(unittest.TestCase):
    def _targets(self, source_root: str, sources: dict[str, str]) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            write_reply(build, source_root, sources)
            return matrix.example_targets(build)

    def test_absolute_source_paths_are_made_relative_to_the_source_root(self):
        found = self._targets("/work/pulp", {
            "Foo_CLAP": "/work/pulp/examples/foo",
            "bar": "/work/pulp/examples/bar/sub",
            "core": "/work/pulp/core/audio",
        })
        self.assertEqual(sorted(found), ["bar", "foo"])
        self.assertEqual(found["foo"], [{"name": "Foo_CLAP", "type": "EXECUTABLE"}])

    def test_relative_source_paths_still_work(self):
        found = self._targets("/work/pulp", {"Foo": "examples/foo", "core": "core/audio"})
        self.assertEqual(sorted(found), ["foo"])

    def test_absolute_path_outside_the_source_root_is_ignored(self):
        found = self._targets("/work/pulp", {"Ext": "/elsewhere/examples/foo"})
        self.assertEqual(found, {})

    def test_the_examples_directory_itself_names_no_example(self):
        self.assertIsNone(matrix.example_dir_of("/work/pulp/examples", "/work/pulp"))
        self.assertIsNone(matrix.example_dir_of("examples", "/work/pulp"))


if __name__ == "__main__":
    unittest.main()
