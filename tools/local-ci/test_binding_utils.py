from __future__ import annotations

import unittest

import importlib.util
import pathlib


MODULE_PATH = pathlib.Path(__file__).with_name("binding_utils.py")


def load_module():
    spec = importlib.util.spec_from_file_location("binding_utils_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class BindingUtilsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_binding_returns_named_value(self) -> None:
        value = object()

        self.assertIs(self.mod.binding({"dependency": value}, "dependency"), value)

    def test_binding_preserves_missing_key_errors(self) -> None:
        with self.assertRaises(KeyError):
            self.mod.binding({}, "missing")


if __name__ == "__main__":
    unittest.main()
