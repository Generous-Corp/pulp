#!/usr/bin/env python3
"""Static contract tests for pulp_add_plugin identity propagation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class PluginIdentityContractTests(unittest.TestCase):
    def test_vst3_templates_use_configured_identity_macros(self):
        for template in sorted((ROOT / "tools/templates").glob("*/vst3_entry.cpp.template")):
            text = template.read_text()
            self.assertIn("PULP_PLUGIN_NAME", text, template)
            self.assertIn("PULP_PLUGIN_VENDOR", text, template)
            self.assertIn("PULP_PLUGIN_VERSION", text, template)
            self.assertNotIn('{{VERSION}}', text, template)

    def test_vst3_entry_does_not_ship_example_email(self):
        text = (ROOT / "core/format/include/pulp/format/vst3_entry.hpp").read_text()
        self.assertNotIn("info@example.com", text)

    def test_plugin_api_propagates_identity_and_display_name(self):
        text = (ROOT / "tools/cmake/PulpPlugin.cmake").read_text()
        for macro in (
            "PULP_PLUGIN_NAME",
            "PULP_PLUGIN_VENDOR",
            "PULP_PLUGIN_VERSION",
            "PULP_BUNDLE_ID",
            "PULP_PLUGIN_EMAIL",
        ):
            self.assertIn(macro, text)
        self.assertIn("AU_DISPLAY_NAME", text)
        self.assertIn("_pulp_check_vst3_factory_version", text)
        self.assertIn("PULP_STRICT_PLUGIN_IDENTITY", text)
        for format_name in ("VST3", "CLAP", "AU", "LV2", "AAX", "AUv3", "Standalone"):
            self.assertIn(f"TARGET ${{target}}_${{_pulp_identity_format}}", text)
            self.assertIn(format_name, text)

    def test_macos_plists_declare_minimum_system_version(self):
        for name in ("au", "vst3", "clap", "standalone"):
            text = (ROOT / f"tools/cmake/PulpInfoPlist.{name}.in").read_text()
            self.assertIn("LSMinimumSystemVersion", text, name)


if __name__ == "__main__":
    unittest.main()
