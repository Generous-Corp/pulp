#!/usr/bin/env python3
"""Static contract tests for pulp_add_plugin identity propagation."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
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


def _cmake_function(path, name):
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(f"function({name}")
    return text[start:text.index("endfunction()", start) + len("endfunction()")]


ENTRY = """#include <pulp/format/vst3_entry.hpp>
{prefix}
PULP_VST3_PLUGIN(
    INLINE_UID(0x1, 0x2, 0x3, 0x4),
    "Name", Steinberg::Vst::PlugType::kFx, "Vendor",
    {version},
    "https://example.test/x", ns::create_plugin)
{suffix}
"""


@unittest.skipUnless(shutil.which("cmake"), "cmake not available")
class FactoryVersionCheckBehaviourTests(unittest.TestCase):
    """Runs the real checker (and entry resolution) in cmake script mode."""

    def run_check(self, version, prefix="", suffix="", strict=False, configured="1.2.3"):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "vst3_entry.cpp").write_text(
                ENTRY.format(prefix=prefix, version=version, suffix=suffix), encoding="utf-8")
            harness = tmp / "harness.cmake"
            harness.write_text(
                _cmake_function("tools/cmake/PulpPluginFormats.cmake", "_pulp_plugin_entry_path")
                + "\n"
                + _cmake_function("tools/cmake/PulpPlugin.cmake", "_pulp_check_vst3_factory_version")
                + f'\nset(CMAKE_CURRENT_SOURCE_DIR "{tmp}")\n'
                + (f"set(PULP_STRICT_PLUGIN_IDENTITY ON)\n" if strict else "")
                + f'_pulp_check_vst3_factory_version(demo "{configured}")\n',
                encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(harness)], capture_output=True,
                                  text=True, timeout=20)

    def assertMismatch(self, result, strict):
        if strict:
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertIn("does not match VERSION", result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("does not match VERSION", result.stderr)
            self.assertIn("Warning", result.stderr)

    def assertClean(self, result, strict=False):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("does not match", result.stderr)

    def test_mismatched_literal_warns_then_fails_when_strict(self):
        self.assertMismatch(self.run_check('"9.9.9"'), strict=False)
        self.assertMismatch(self.run_check('"9.9.9"', strict=True), strict=True)

    def test_matching_literal_and_version_macro_pass_in_strict_mode(self):
        self.assertClean(self.run_check('"1.2.3"', strict=True))
        self.assertClean(self.run_check("PULP_PLUGIN_VERSION", strict=True))

    def test_comment_mentioning_version_macro_does_not_disable_validation(self):
        for prefix, suffix in (("// use PULP_PLUGIN_VERSION here", ""),
                               ("/* PULP_PLUGIN_VERSION */", ""),
                               ("", "// PULP_PLUGIN_VERSION")):
            self.assertMismatch(
                self.run_check('"9.9.9"', prefix=prefix, suffix=suffix, strict=True), strict=True)

    def test_commented_out_call_is_ignored(self):
        old = 'PULP_VST3_PLUGIN(u, n, c, v, "1.2.3", "", f)'
        for prefix in (f"// {old}", f"/* {old} */"):
            self.assertMismatch(
                self.run_check('"9.9.9"', prefix=prefix, strict=True), strict=True)

    def test_macro_mentioned_outside_the_version_argument_does_not_disable_validation(self):
        self.assertMismatch(
            self.run_check('"9.9.9"', prefix="#define UNRELATED PULP_PLUGIN_VERSION", strict=True),
            strict=True)

    def test_unrelated_quoted_versions_are_not_picked(self):
        # A later unrelated literal must not replace the factory's own version,
        # in either direction.
        self.assertClean(self.run_check('"1.2.3"', suffix='static const char* k = "7.7.7";',
                                        strict=True))
        self.assertMismatch(
            self.run_check('"9.9.9"', suffix='static const char* k = "1.2.3";', strict=True),
            strict=True)
        self.assertClean(self.run_check("PULP_PLUGIN_VERSION",
                                        suffix='static const char* k = "7.7.7";', strict=True))

    def test_pulp_add_plugin_invokes_the_checker_with_its_version(self):
        text = (ROOT / "tools/cmake/PulpPlugin.cmake").read_text(encoding="utf-8")
        self.assertRegex(
            text, re.compile(r'_pulp_check_vst3_factory_version\(\$\{target\} "\$\{PLUGIN_VERSION\}"\)'))


if __name__ == "__main__":
    unittest.main()
