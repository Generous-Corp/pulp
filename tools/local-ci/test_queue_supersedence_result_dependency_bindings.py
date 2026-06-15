#!/usr/bin/env python3
"""Tests for queue supersedence result dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("queue_supersedence_result_dependency_bindings.py")


class QueueSupersedenceResultDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_supersedence_result_dependencies_bind_now_iso(self) -> None:
        bindings = {"now_iso": object()}

        deps = self.mod.queue_supersedence_result_dependencies(bindings)

        self.assertIs(deps["now_iso_fn"], bindings["now_iso"])


if __name__ == "__main__":
    unittest.main()
