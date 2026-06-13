#!/usr/bin/env python3
"""Tests for config/evidence facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import json
from pathlib import Path
import tempfile
import types
import unittest


MODULE_PATH = Path(__file__).with_name("config_evidence_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ConfigEvidenceBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.config_path = self.root / "config.json"

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def _bindings(self, **overrides):
        bindings = {
            "json": json,
            "config_path": lambda: self.config_path,
            "normalize_desktop_config": lambda cfg: {**cfg, "normalized": True},
            "atomic_write_text": lambda path, text: Path(path).write_text(text),
            "evidence_index_module": types.SimpleNamespace(),
            "load_evidence_index": lambda: {"version": 3, "entries": {}},
            "collect_evidence_groups": lambda **_kwargs: {"full": []},
            "collect_evidence_groups_from_index": lambda index, **kwargs: {
                "index": index,
                "filters": kwargs,
            },
        }
        bindings.update(overrides)
        return bindings

    def test_load_config_normalizes_required_config(self) -> None:
        self.config_path.write_text('{"desktop_automation": {"targets": {}}}')

        config = self.mod.load_config(self._bindings())

        self.assertTrue(config["normalized"])
        self.assertEqual(config["desktop_automation"], {"targets": {}})

    def test_load_config_file_reports_missing_config_path(self) -> None:
        missing = self.root / "missing.json"

        with self.assertRaisesRegex(FileNotFoundError, str(missing)):
            self.mod.load_config_file(self._bindings(), missing)

    def test_optional_config_is_raw_and_missing_safe(self) -> None:
        self.assertIsNone(self.mod.load_optional_config(self._bindings()))

        self.config_path.write_text('{"desktop_automation": {"targets": {"mac": {}}}}')

        self.assertEqual(
            self.mod.load_optional_config(self._bindings()),
            {"desktop_automation": {"targets": {"mac": {}}}},
        )

    def test_save_config_writes_pretty_json_with_trailing_newline(self) -> None:
        self.mod.save_config(self._bindings(), {"b": 2, "a": 1})

        self.assertEqual(self.config_path.read_text(), '{\n  "b": 2,\n  "a": 1\n}\n')

    def test_evidence_wrappers_preserve_facade_monkeypatch_seams(self) -> None:
        calls = {}

        evidence_module = types.SimpleNamespace(
            load_evidence_index=lambda: {"loaded": True},
            update_evidence_index=lambda result, path: calls.setdefault("update", (result, path)),
            print_evidence_summary_from_groups=lambda groups, **kwargs: calls.setdefault(
                "summary",
                (groups, kwargs),
            )
            or True,
            evidence_scope_header_line=lambda branch, sha: f"{branch}:{sha}",
            evidence_empty_line=lambda *, has_header: "empty-with-header" if has_header else "empty",
        )
        bindings = self._bindings(
            evidence_index_module=evidence_module,
            load_evidence_index=lambda: {"facade": True},
        )

        self.assertEqual(self.mod.load_evidence_index(bindings), {"loaded": True})
        self.mod.update_evidence_index(bindings, {"job": "1"}, Path("/tmp/result.json"))
        self.assertEqual(calls["update"], ({"job": "1"}, Path("/tmp/result.json")))

        groups = self.mod.collect_evidence_groups(bindings, branch="feature/a", sha="abc")
        self.assertEqual(groups, {"index": {"facade": True}, "filters": {"branch": "feature/a", "sha": "abc"}})

        self.assertTrue(self.mod.print_evidence_summary(bindings, branch="feature/a", limit=2, indent="  "))
        self.assertEqual(
            calls["summary"],
            ({"full": []}, {"limit": 2, "indent": "  "}),
        )
        self.assertEqual(self.mod.evidence_scope_header_line(bindings, "feature/a", None), "feature/a:None")
        self.assertEqual(self.mod.evidence_empty_line(bindings, has_header=True), "empty-with-header")

    def test_install_config_evidence_helpers_wires_named_exports(self) -> None:
        self.config_path.write_text('{"desktop_automation": {"targets": {}}}')
        calls = {}

        def evidence_empty_line(*, has_header):
            calls["has_header"] = has_header
            return "empty-with-header" if has_header else "empty"

        evidence_module = types.SimpleNamespace(
            evidence_empty_line=evidence_empty_line,
        )
        bindings = self._bindings(evidence_index_module=evidence_module)

        self.mod.install_config_evidence_helpers(bindings, ("load_config", "evidence_empty_line"))

        self.assertTrue(bindings["load_config"]()["normalized"])
        self.assertEqual(bindings["evidence_empty_line"](has_header=True), "empty-with-header")
        self.assertEqual(calls["has_header"], True)
        self.assertEqual(bindings["load_config"].__name__, "load_config")

    def test_install_config_evidence_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_config_evidence_helper = lambda _bindings: "future"

        self.mod.install_config_evidence_helpers(bindings, ("future_config_evidence_helper",))

        self.assertEqual(bindings["future_config_evidence_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
