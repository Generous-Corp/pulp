#!/usr/bin/env python3
"""Tests for direct cloud record-store module-attribute binding installer."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_record_store_attr_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudRecordStoreAttrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_record_store_exports_are_unique(self):
        self.assertIn("load_cloud_record", self.mod.CLOUD_RECORD_STORE_EXPORTS)
        self.assertIn("update_cloud_record_from_run", self.mod.CLOUD_RECORD_STORE_EXPORTS)
        self.assertEqual(len(self.mod.CLOUD_RECORD_STORE_EXPORTS), len(set(self.mod.CLOUD_RECORD_STORE_EXPORTS)))

    def test_install_cloud_record_store_attr_helpers_wires_late_bound_exports(self):
        calls = []

        def normalize_cloud_record(record):
            calls.append(("normalize_cloud_record", record))
            return {"normalized": record}

        bindings = {"_cloud": types.SimpleNamespace(normalize_cloud_record=normalize_cloud_record)}

        self.mod.install_cloud_record_store_attr_helpers(bindings, ("normalize_cloud_record",))

        self.assertEqual(bindings["normalize_cloud_record"]({"id": "run"}), {"normalized": {"id": "run"}})
        self.assertEqual(calls, [("normalize_cloud_record", {"id": "run"})])


if __name__ == "__main__":
    unittest.main()
