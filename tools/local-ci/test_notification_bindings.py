#!/usr/bin/env python3
"""Tests for local_ci facade notification binding wiring."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("notification_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_install_notification_helpers_wires_named_exports(self) -> None:
        notifications = mock.Mock()
        subprocess = mock.Mock()
        bindings = {
            "_notifications": notifications,
            "print": mock.Mock(name="print"),
            "subprocess": subprocess,
        }

        self.mod.install_notification_helpers(bindings, ("notify",))
        bindings["notify"]("done")

        notifications.notify.assert_called_once_with(
            "done",
            print_fn=bindings["print"],
            run_fn=subprocess.run,
        )


if __name__ == "__main__":
    unittest.main()
