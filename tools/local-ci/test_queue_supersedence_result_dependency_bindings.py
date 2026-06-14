#!/usr/bin/env python3
"""Tests for queue supersedence result dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("queue_supersedence_result_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueSupersedenceResultDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_supersedence_result_dependencies_bind_now_iso(self) -> None:
        bindings = {"now_iso": object()}

        deps = self.mod.queue_supersedence_result_dependencies(bindings)

        self.assertIs(deps["now_iso_fn"], bindings["now_iso"])


if __name__ == "__main__":
    unittest.main()
