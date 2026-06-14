#!/usr/bin/env python3
"""Tests for cloud record facade dependency wiring helpers."""

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_record_facade_helpers.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudRecordFacadeHelpersTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_storage_and_summary_helpers_forward_dependencies(self):
        calls = []

        def save_cloud_record_fn(record, **kwargs):
            calls.append(("save", record, kwargs))
            return Path("stored.json")

        def list_cloud_records_fn(**kwargs):
            calls.append(("list", kwargs))
            return [{"dispatch_id": "abc"}]

        def summary_fn(record, config, **kwargs):
            calls.append(("summary", record, config, kwargs))
            return "summary"

        self.assertEqual(
            self.mod.save_cloud_record_with_deps(
                {"dispatch_id": "abc"},
                save_cloud_record_fn=save_cloud_record_fn,
                ensure_state_dirs_fn=lambda: None,
                cloud_run_path_fn=lambda dispatch_id: Path(f"{dispatch_id}.json"),
                atomic_write_text_fn=lambda path, text: None,
            ),
            Path("stored.json"),
        )
        self.assertEqual(
            self.mod.list_cloud_records_with_deps(
                limit=5,
                list_cloud_records_fn=list_cloud_records_fn,
                ensure_state_dirs_fn=lambda: None,
                cloud_runs_dir_fn=lambda: Path("."),
                load_cloud_record_fn=lambda path: {"path": str(path)},
            ),
            [{"dispatch_id": "abc"}],
        )
        self.assertEqual(
            self.mod.cloud_record_summary_with_deps(
                {"dispatch_id": "abc"},
                {"cfg": True},
                cloud_record_summary_fn=summary_fn,
                estimate_cloud_record_cost_fn=lambda record, config: {},
                format_currency_amount_fn=lambda amount, currency: "",
            ),
            "summary",
        )

        self.assertEqual(calls[0][0], "save")
        self.assertEqual(calls[0][2]["cloud_run_path_fn"]("abc"), Path("abc.json"))
        self.assertEqual(calls[1][1]["limit"], 5)
        self.assertIn("estimate_cloud_record_cost_fn", calls[2][3])


if __name__ == "__main__":
    unittest.main()
