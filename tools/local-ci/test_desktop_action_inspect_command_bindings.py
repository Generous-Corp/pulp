#!/usr/bin/env python3
"""Tests for desktop inspect command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_action_inspect_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionInspectCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_match_wrapper(self) -> None:
        self.assertEqual(self.mod.DESKTOP_ACTION_INSPECT_COMMAND_EXPORTS, ("cmd_desktop_inspect",))

    def test_cmd_desktop_inspect_delegates_with_shared_kwargs(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        args_obj = object()
        kwargs = {"load_config_fn": object()}
        bindings = {"_desktop_action_commands_cli": types.SimpleNamespace(cmd_desktop_inspect=runner)}

        with mock.patch.object(self.mod, "desktop_action_command_kwargs", return_value=kwargs):
            self.assertEqual(self.mod.cmd_desktop_inspect(bindings, args_obj), 5)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], kwargs)


if __name__ == "__main__":
    unittest.main()
