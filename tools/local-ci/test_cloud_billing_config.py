#!/usr/bin/env python3
"""Tests for cloud billing configuration normalization helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_billing_config.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudBillingConfigTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_parse_rate_value_accepts_non_negative_numbers(self) -> None:
        self.assertEqual(self.mod.parse_rate_value("1.25"), 1.25)
        self.assertEqual(self.mod.parse_rate_value(0), 0.0)
        self.assertIsNone(self.mod.parse_rate_value(""))
        self.assertIsNone(self.mod.parse_rate_value(None))
        self.assertIsNone(self.mod.parse_rate_value("-1"))
        self.assertIsNone(self.mod.parse_rate_value("bad"))

    def test_parse_optional_bool_accepts_only_bools_and_empty(self) -> None:
        self.assertTrue(self.mod.parse_optional_bool(True, "flag"))
        self.assertFalse(self.mod.parse_optional_bool(False, "flag"))
        self.assertIsNone(self.mod.parse_optional_bool("", "flag"))
        self.assertIsNone(self.mod.parse_optional_bool(None, "flag"))
        with self.assertRaisesRegex(ValueError, "flag must be true or false"):
            self.mod.parse_optional_bool("true", "flag")

    def test_resolve_billing_settings_normalizes_config(self) -> None:
        settings = self.mod.resolve_billing_settings(
            {
                "telemetry": {
                    "billing": {
                        "currency": "eur",
                        "billing_period_start_day": "12",
                        "enable_provider_reported_totals": True,
                        "github_hosted_job_os_rates_per_minute": {
                            " macOS ": "0.16",
                            "linux": -1,
                            "": 1,
                        },
                        "namespace_profile_tag_rates_per_hour": {
                            "large": "1.5",
                            "bad": "nope",
                        },
                        "namespace_machine_shape_rates_per_hour": [
                            {"os": "Linux", "arch": "ARM64", "virtual_cpu": "4", "memory_megabytes": "8192", "rate": "2.25"},
                            {"os": "windows", "rate": ""},
                        ],
                    }
                }
            }
        )

        self.assertEqual(settings["currency"], "EUR")
        self.assertEqual(settings["billing_period_start_day"], 12)
        self.assertTrue(settings["enable_provider_reported_totals"])
        self.assertEqual(settings["github_hosted_job_os_rates_per_minute"], {"macos": 0.16})
        self.assertEqual(settings["namespace_profile_tag_rates_per_hour"], {"large": 1.5})
        self.assertEqual(
            settings["namespace_machine_shape_rates_per_hour"],
            [{"os": "linux", "arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192, "rate": 2.25}],
        )


if __name__ == "__main__":
    unittest.main()
