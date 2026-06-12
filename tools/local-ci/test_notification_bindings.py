#!/usr/bin/env python3
"""Tests for local_ci facade notification binding wiring."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("notification_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("notification_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class NotificationBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_notify_wires_facade_print_and_subprocess_run(self) -> None:
        notifications = mock.Mock()
        subprocess = mock.Mock()
        bindings = {
            "_notifications": notifications,
            "print": mock.Mock(name="print"),
            "subprocess": subprocess,
        }

        self.mod.notify(bindings, "done")

        notifications.notify.assert_called_once_with(
            "done",
            print_fn=bindings["print"],
            run_fn=subprocess.run,
        )


if __name__ == "__main__":
    unittest.main()
