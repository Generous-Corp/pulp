#!/usr/bin/env python3
"""Tests for config/evidence facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import json
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("config_evidence_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ConfigEvidenceBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, **overrides):
        bindings = {
            "json": json,
            "config_path": lambda: Path("/tmp/config.json"),
            "normalize_desktop_config": lambda cfg: cfg,
            "atomic_write_text": lambda _path, _text: None,
            "evidence_index_module": types.SimpleNamespace(),
        }
        bindings.update(overrides)
        return bindings

    def test_exports_compose_focused_groups(self) -> None:
        expected = (
            *self.mod.CONFIG_FILE_EXPORTS,
            *self.mod.CONFIG_EVIDENCE_SUMMARY_EXPORTS,
        )

        self.assertEqual(self.mod.CONFIG_EVIDENCE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_config_evidence_helpers_wires_named_exports(self) -> None:
        calls = {}

        def evidence_empty_line(*, has_header):
            calls["has_header"] = has_header
            return "empty-with-header" if has_header else "empty"

        def atomic_write_text(path, text):
            calls["write"] = (path, text)

        evidence_module = types.SimpleNamespace(
            evidence_empty_line=evidence_empty_line,
        )
        config_path = Path("/tmp/local-ci-config.json")
        bindings = self._bindings(
            atomic_write_text=atomic_write_text,
            config_path=lambda: config_path,
            evidence_index_module=evidence_module,
        )

        self.mod.install_config_evidence_helpers(bindings, ("save_config", "evidence_empty_line"))

        bindings["save_config"]({"ok": True})
        self.assertEqual(bindings["evidence_empty_line"](has_header=True), "empty-with-header")
        self.assertEqual(calls["write"], (config_path, '{\n  "ok": true\n}\n'))
        self.assertEqual(calls["has_header"], True)
        self.assertEqual(bindings["save_config"].__name__, "save_config")


if __name__ == "__main__":
    unittest.main()
