from __future__ import annotations

import unittest

import builtins
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

    def test_print_binding_returns_facade_print_when_present(self) -> None:
        print_fn = object()

        self.assertIs(self.mod.print_binding({"print": print_fn}), print_fn)

    def test_print_binding_falls_back_to_builtin_print(self) -> None:
        self.assertIs(self.mod.print_binding({}), builtins.print)


if __name__ == "__main__":
    unittest.main()
