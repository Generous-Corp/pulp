#!/usr/bin/env python3
"""Tests for the targets resolution helpers."""

import unittest

import targets



class TargetsTests(unittest.TestCase):
    def setUp(self):
        self.mod = targets

    def test_extracted_target_helpers_cover_empty_defaults_unknown_and_disabled(self):
        config = {
            "targets": {
                "mac": {"enabled": True},
                "linux": {"enabled": False},
                "windows": {},
            },
            "defaults": {"targets": []},
        }

        self.assertEqual(self.mod.enabled_targets(config), ["mac", "windows"])
        self.assertEqual(self.mod.resolve_targets(config, None), [])

        with self.assertRaisesRegex(ValueError, "Unknown target\\(s\\): ios"):
            self.mod.resolve_targets(config, ["ios"])

        with self.assertRaisesRegex(ValueError, "Requested target\\(s\\) disabled"):
            self.mod.resolve_targets(config, ["linux"])

    def test_parse_targets_arg_trims_sorts_deduplicates_and_handles_empty_values(self):
        self.assertIsNone(self.mod.parse_targets_arg(None))
        self.assertIsNone(self.mod.parse_targets_arg(""))
        self.assertIsNone(self.mod.parse_targets_arg("   "))
        self.assertEqual(
            self.mod.parse_targets_arg(" ubuntu,mac,ubuntu,, windows "),
            ["mac", "ubuntu", "windows"],
        )

    def test_resolve_targets_uses_string_defaults_and_enabled_fallback(self):
        config = {
            "targets": {
                "mac": {"enabled": True},
                "ubuntu": {"enabled": True},
                "windows": {"enabled": True},
            },
            "defaults": {"targets": "ubuntu, mac,ubuntu"},
        }

        self.assertEqual(self.mod.resolve_targets(config, None), ["mac", "ubuntu"])
        self.assertEqual(self.mod.resolve_targets(config, []), [])

        config_without_defaults = {
            "targets": {
                "mac": {"enabled": True},
                "ubuntu": {"enabled": False},
                "windows": {},
            },
            "defaults": {},
        }
        self.assertEqual(
            self.mod.resolve_targets(config_without_defaults, None),
            ["mac", "windows"],
        )



if __name__ == "__main__":
    unittest.main()
