from __future__ import annotations

import unittest

import builtins
from module_test_utils import load_module_from_path
import pathlib
import types


MODULE_PATH = pathlib.Path(__file__).with_name("binding_utils.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class BindingUtilsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_binding_returns_named_value(self) -> None:
        value = object()

        self.assertIs(self.mod.binding({"dependency": value}, "dependency"), value)

    def test_binding_preserves_missing_key_errors(self) -> None:
        with self.assertRaises(KeyError):
            self.mod.binding({}, "missing")

    def test_binding_attr_returns_named_attribute(self) -> None:
        class Dependency:
            value = object()

        self.assertIs(self.mod.binding_attr({"dependency": Dependency}, "dependency", "value"), Dependency.value)

    def test_binding_attr_preserves_missing_attribute_errors(self) -> None:
        with self.assertRaises(AttributeError):
            self.mod.binding_attr({"dependency": object()}, "dependency", "missing")

    def test_print_binding_returns_facade_print_when_present(self) -> None:
        print_fn = object()

        self.assertIs(self.mod.print_binding({"print": print_fn}), print_fn)

    def test_print_binding_falls_back_to_builtin_print(self) -> None:
        self.assertIs(self.mod.print_binding({}), builtins.print)

    def test_install_local_helpers_wires_bound_facades(self) -> None:
        captured = {}

        def helper(bindings, value):
            captured["bindings"] = bindings
            captured["value"] = value
            return "ok"

        bindings = {}
        self.mod.install_local_helpers(bindings, {"helper": helper}, ("helper",))

        self.assertEqual(bindings["helper"]("value"), "ok")
        self.assertIs(captured["bindings"], bindings)
        self.assertEqual(captured["value"], "value")
        self.assertEqual(bindings["helper"].__name__, "helper")

    def test_install_module_attrs_late_binds_current_module_attribute(self) -> None:
        first = types.SimpleNamespace(helper=lambda: "first")
        second = types.SimpleNamespace(helper=lambda: "second")
        bindings = {"module": first}

        self.mod.install_module_attrs(bindings, "module", ("helper",))
        bindings["module"] = second

        self.assertEqual(bindings["helper"](), "second")
        self.assertEqual(bindings["helper"].__name__, "<lambda>")


if __name__ == "__main__":
    unittest.main()
