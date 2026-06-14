#!/usr/bin/env python3
"""Binding tests for local-ci I/O helper facades."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("io_utils_bindings.py")
io_utils_bindings = load_module_from_path(MODULE_PATH, module_name="io_utils_bindings", add_module_dir=True)


class FakeIoUtils:
    def __init__(self) -> None:
        self.calls: list[tuple] = []
        self.lock_token = object()

    def tail_lines(self, path, limit=80):
        self.calls.append(("tail_lines", path, limit))
        return ["tail"]

    def trim_line(self, value, max_len=160):
        self.calls.append(("trim_line", value, max_len))
        return "trimmed"

    def atomic_write_text(self, path, text):
        self.calls.append(("atomic_write_text", path, text))

    def image_change_summary(self, before_path, after_path, *, diff_output_path=None):
        self.calls.append(("image_change_summary", before_path, after_path, diff_output_path))
        return {"changed": True}

    def file_lock(self, path, *, blocking):
        self.calls.append(("file_lock", path, blocking))
        return self.lock_token


class IoUtilsBindingTests(unittest.TestCase):
    def test_io_utils_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeIoUtils()
        bindings = {"_io_utils": fake}
        before = Path("before.png")
        after = Path("after.png")
        diff = Path("diff.png")

        self.assertEqual(io_utils_bindings.tail_lines(bindings, before, 12), ["tail"])
        self.assertEqual(io_utils_bindings.trim_line(bindings, " value ", 10), "trimmed")
        self.assertIsNone(io_utils_bindings.atomic_write_text(bindings, before, "text"))
        self.assertEqual(
            io_utils_bindings.image_change_summary(bindings, before, after, diff_output_path=diff),
            {"changed": True},
        )
        self.assertIs(io_utils_bindings.file_lock(bindings, before, blocking=False), fake.lock_token)
        self.assertEqual(
            fake.calls,
            [
                ("tail_lines", before, 12),
                ("trim_line", " value ", 10),
                ("atomic_write_text", before, "text"),
                ("image_change_summary", before, after, diff),
                ("file_lock", before, False),
            ],
        )

    def test_install_io_utils_helpers_wires_named_exports(self) -> None:
        fake = FakeIoUtils()
        bindings = {"_io_utils": fake}
        before = Path("before.png")
        after = Path("after.png")

        io_utils_bindings.install_io_utils_helpers(bindings, ("tail_lines", "image_change_summary"))

        self.assertEqual(bindings["tail_lines"](before, 12), ["tail"])
        self.assertEqual(bindings["image_change_summary"](before, after), {"changed": True})
        self.assertEqual(bindings["tail_lines"].__name__, "tail_lines")
        self.assertEqual([call[0] for call in fake.calls], ["tail_lines", "image_change_summary"])


if __name__ == "__main__":
    unittest.main()
