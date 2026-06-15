#!/usr/bin/env python3
"""Contract tests for focused local-ci binding installer fallbacks."""

from __future__ import annotations

import ast
from module_test_utils import load_module_from_path
import inspect
from pathlib import Path
import re
import types
import unittest


MODULE_DIR = Path(__file__).parent
INSTALLER_RE = re.compile(r"^def (install_[A-Za-z0-9_]+_helpers)\(", re.MULTILINE)


def local_fallback_installers() -> list[tuple[Path, str]]:
    installers: list[tuple[Path, str]] = []
    for module_path in sorted(MODULE_DIR.glob("*_bindings.py")):
        if module_path.name.startswith("test_"):
            continue
        text = module_path.read_text()
        for match in INSTALLER_RE.finditer(text):
            start = match.start()
            end = text.find("\ndef ", start + 1)
            block = text[start : end if end != -1 else len(text)]
            if "unknown_names" in block and "install_local_helpers" in block:
                installers.append((module_path, match.group(1)))
    return installers


def direct_local_known_installers() -> list[tuple[Path, str]]:
    installers: list[tuple[Path, str]] = []
    for module_path in sorted(MODULE_DIR.glob("*_bindings.py")):
        if module_path.name.startswith("test_"):
            continue
        tree = ast.parse(module_path.read_text())
        for node in tree.body:
            if not (
                isinstance(node, ast.FunctionDef)
                and node.name.startswith("install_")
                and node.name.endswith("_helpers")
            ):
                continue
            first_installer_call = None
            for child in ast.walk(node):
                if not isinstance(child, ast.Call):
                    continue
                func = child.func
                if not isinstance(func, ast.Name) or not func.id.startswith("install_"):
                    continue
                first_installer_call = child
                break
            if first_installer_call is None or not isinstance(first_installer_call.func, ast.Name):
                continue
            if first_installer_call.func.id != "install_local_helpers":
                continue
            if len(first_installer_call.args) < 2 or not isinstance(first_installer_call.args[1], ast.Call):
                continue
            namespace_func = first_installer_call.args[1].func
            if isinstance(namespace_func, ast.Name) and namespace_func.id == "globals":
                installers.append((module_path, node.name))
    return installers


def cloud_module_attr_installers() -> list[tuple[Path, str]]:
    installers: list[tuple[Path, str]] = []
    for module_path in sorted(MODULE_DIR.glob("cloud*_bindings.py")):
        if module_path.name.startswith("test_"):
            continue
        text = module_path.read_text()
        for match in INSTALLER_RE.finditer(text):
            start = match.start()
            end = text.find("\ndef ", start + 1)
            block = text[start : end if end != -1 else len(text)]
            if 'install_module_attrs(bindings, "_cloud"' in block or (
                "unknown_names" in block and "install_cloud_module_attr_helpers" in block
            ):
                installers.append((module_path, match.group(1)))
    return installers


class BindingInstallerFallbackContractTests(unittest.TestCase):
    def test_direct_local_installers_wire_known_exports_through_bindings(self) -> None:
        installers = direct_local_known_installers()
        self.assertGreaterEqual(len(installers), 150)

        for module_path, installer_name in installers:
            with self.subTest(module=module_path.name, installer=installer_name):
                module = load_module_from_path(module_path, add_module_dir=True)
                installer = getattr(module, installer_name)
                default_names = inspect.signature(installer).parameters["names"].default
                helper_name = default_names[0]
                calls = []

                def replacement(bindings, *args, **kwargs):
                    calls.append((bindings, args, kwargs))
                    return "replacement-result"

                replacement.__name__ = helper_name
                original = getattr(module, helper_name)
                try:
                    setattr(module, helper_name, replacement)
                    bindings = {"sentinel": f"{module_path.name}:{installer_name}"}

                    installer(bindings, (helper_name,))

                    self.assertEqual(bindings[helper_name]("arg", key="value"), "replacement-result")
                    self.assertEqual(bindings[helper_name].__name__, helper_name)
                    self.assertEqual(calls, [(bindings, ("arg",), {"key": "value"})])
                finally:
                    setattr(module, helper_name, original)

    def test_local_fallback_installers_keep_unknown_helpers_bound_to_bindings(self) -> None:
        installers = local_fallback_installers()
        self.assertGreaterEqual(len(installers), 200)

        for module_path, installer_name in installers:
            with self.subTest(module=module_path.name, installer=installer_name):
                module = load_module_from_path(module_path, add_module_dir=True)
                helper_name = "future_contract_helper"

                def future_contract_helper(bindings):
                    return bindings["sentinel"]

                setattr(module, helper_name, future_contract_helper)
                bindings = {"sentinel": f"{module_path.name}:{installer_name}"}

                getattr(module, installer_name)(bindings, (helper_name,))

                self.assertEqual(bindings[helper_name](), bindings["sentinel"])
                self.assertEqual(bindings[helper_name].__name__, helper_name)

    def test_cloud_module_attr_installers_keep_unknown_helpers_late_bound_to_cloud_module(self) -> None:
        installers = cloud_module_attr_installers()
        self.assertEqual(len(installers), 7)

        for module_path, installer_name in installers:
            with self.subTest(module=module_path.name, installer=installer_name):
                module = load_module_from_path(module_path, add_module_dir=True)
                helper_name = "future_contract_helper"

                def first(value):
                    return f"first:{value}"

                def second(value):
                    return f"second:{value}"

                bindings = {"_cloud": types.SimpleNamespace(**{helper_name: first})}

                getattr(module, installer_name)(bindings, (helper_name,))
                helper = bindings[helper_name]

                self.assertEqual(helper("value"), "first:value")
                bindings["_cloud"] = types.SimpleNamespace(**{helper_name: second})
                self.assertEqual(helper("value"), "second:value")


if __name__ == "__main__":
    unittest.main()
