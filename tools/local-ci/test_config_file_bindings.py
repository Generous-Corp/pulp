#!/usr/bin/env python3
"""Tests for config file facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("config_file_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ConfigFileBindingsTests(unittest.TestCase):
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

    def test_install_config_file_helpers_wires_named_exports(self) -> None:
        self.config_path.write_text('{"desktop_automation": {"targets": {}}}')
        bindings = self._bindings()

        self.mod.install_config_file_helpers(bindings, ("load_config", "save_config"))

        self.assertTrue(bindings["load_config"]()["normalized"])
        bindings["save_config"]({"ok": True})
        self.assertEqual(self.config_path.read_text(), '{\n  "ok": true\n}\n')
        self.assertEqual(bindings["load_config"].__name__, "load_config")

    def test_install_config_file_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_config_file_helper = lambda _bindings: "future"

        self.mod.install_config_file_helpers(bindings, ("future_config_file_helper",))

        self.assertEqual(bindings["future_config_file_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
