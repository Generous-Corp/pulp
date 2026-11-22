#!/usr/bin/env python3
"""Static contract tests for pulp_add_plugin identity propagation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class PluginIdentityContractTests(unittest.TestCase):
    def test_vst3_templates_use_configured_identity_macros(self):
        for template in sorted((ROOT / "tools/templates").glob("*/vst3_entry.cpp.template")):
            text = template.read_text(encoding="utf-8")
            self.assertIn("PULP_PLUGIN_NAME", text, template)
            self.assertIn("PULP_PLUGIN_VENDOR", text, template)
            self.assertIn("PULP_PLUGIN_VERSION", text, template)
            self.assertNotIn('{{VERSION}}', text, template)

    def test_vst3_entry_does_not_ship_example_email(self):
        text = (ROOT / "core/format/include/pulp/format/vst3_entry.hpp").read_text(encoding="utf-8")
        self.assertNotIn("info@example.com", text)

    def test_plugin_api_propagates_identity_and_display_name(self):
        text = (ROOT / "tools/cmake/PulpPlugin.cmake").read_text(encoding="utf-8")
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

    def test_factory_version_check_uses_the_builders_entry_resolution(self):
        text = (ROOT / "tools/cmake/PulpPlugin.cmake").read_text(encoding="utf-8")
        start = text.index("function(_pulp_check_vst3_factory_version")
        body = text[start:text.index("endfunction()", start)]
        # PULP_PLUGIN_ENTRY_DIR support, bundle entry before single-plugin entry.
        self.assertIn("_pulp_plugin_entry_path(_entry vst3_bundle_entry.cpp)", body)
        self.assertLess(body.index("vst3_bundle_entry.cpp"), body.index("vst3_entry.cpp)"))
        self.assertNotIn("CMAKE_CURRENT_SOURCE_DIR}/vst3", body)

    def test_au_display_name_is_xml_escaped_before_plist_substitution(self):
        text = (ROOT / "tools/cmake/PulpPluginFormats.cmake").read_text(encoding="utf-8")
        self.assertNotIn('set(PULP_AU_DISPLAY_NAME "${display_name}")', text)
        amp = text.index('string(REPLACE "&" "&amp;" _pulp_au_display_xml')
        self.assertLess(amp, text.index('string(REPLACE "<" "&lt;" _pulp_au_display_xml'))
        self.assertLess(amp, text.index('set(PULP_AU_DISPLAY_NAME "${_pulp_au_display_xml}")'))

    def test_macos_plists_declare_minimum_system_version(self):
        for name in ("au", "vst3", "clap", "standalone"):
            text = (ROOT / f"tools/cmake/PulpInfoPlist.{name}.in").read_text(encoding="utf-8")
            self.assertIn("LSMinimumSystemVersion", text, name)


if __name__ == "__main__":
    unittest.main()
