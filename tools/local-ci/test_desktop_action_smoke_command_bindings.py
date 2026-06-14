#!/usr/bin/env python3
"""Tests for desktop smoke command facade bindings."""

from __future__ import annotations

import types
import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_smoke_command_bindings.py")


class DesktopActionSmokeCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_match_wrapper(self) -> None:
        self.assertEqual(self.mod.DESKTOP_ACTION_SMOKE_COMMAND_EXPORTS, ("cmd_desktop_smoke",))

    def test_cmd_desktop_smoke_delegates_with_shared_kwargs(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        args_obj = object()
        kwargs = {"load_config_fn": object()}
        bindings = {"_desktop_action_commands_cli": types.SimpleNamespace(cmd_desktop_smoke=runner)}

        with mock.patch.object(self.mod, "desktop_action_command_kwargs", return_value=kwargs):
            self.assertEqual(self.mod.cmd_desktop_smoke(bindings, args_obj), 5)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], kwargs)


if __name__ == "__main__":
    unittest.main()
