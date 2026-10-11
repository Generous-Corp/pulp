#!/usr/bin/env python3
"""Negative controls for design_unit_boundary_check.py.

Each case builds a small synthetic tree plus a manifest shaped like the one
configure writes, starts from a layout that passes, and injects exactly one
boundary violation. A gate that passes the clean layout but also passes any of
the injected ones cannot see that violation.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import design_unit_boundary_check as gate  # noqa: E402


class Tree:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.files = {
            "include/pulp/view/design_ir.hpp": "#pragma once\n#include <string>\n",
            "include/pulp/view/design_source_types.hpp":
                "#pragma once\n#include <pulp/view/design_ir.hpp>\n",
            "include/pulp/view/view.hpp": "#pragma once\n#include <pulp/view/design_ir.hpp>\n",
            "include/pulp/view/design_import.hpp":
                "#pragma once\n#include <pulp/view/design_source_types.hpp>\n#include <pulp/state/store.hpp>\n",
            "include/pulp/view/recognition_audio_pack.hpp":
                "#pragma once\n#include <pulp/view/design_import.hpp>\n",
            "src/view.cpp": '#include <pulp/view/view.hpp>\n#include "view_private.hpp"\n',
            "src/view_private.hpp": "#pragma once\n#include <pulp/view/design_source_types.hpp>\n",
            "src/design_import.cpp":
                '#include <pulp/view/design_import.hpp>\n#include "design_import_internal.hpp"\n'
                'const char* k = "#include <pulp/audio/buffer.hpp>";\n',
            "src/recognition_audio_pack.cpp": "#include <pulp/view/recognition_audio_pack.hpp>\n",
        }
        self.targets = {
            "pulp-design-ir": {
                "type": "INTERFACE_LIBRARY", "sources": "", "links": "", "interface_links": "",
                "headers": "include/pulp/view/design_ir.hpp;include/pulp/view/design_source_types.hpp",
            },
            "pulp-view-core": {
                "type": "STATIC_LIBRARY", "sources": "src/view.cpp",
                "links": "pulp::canvas;pulp::state;pulp::signal;pulp::audio;pulp::design-ir",
                "interface_links": "", "headers": "",
            },
            "pulp-design-import": {
                "type": "STATIC_LIBRARY", "sources": "src/design_import.cpp",
                "links": "pulp::design-ir;pulp::view-core;yaml-cpp::yaml-cpp",
                "interface_links": "", "headers": "include/pulp/view/design_import.hpp",
            },
            "pulp-design-import-audio-pack": {
                "type": "STATIC_LIBRARY", "sources": "src/recognition_audio_pack.cpp",
                "links": "pulp::design-import", "interface_links": "",
                "headers": "include/pulp/view/recognition_audio_pack.hpp",
            },
        }

    def write(self) -> Path:
        for rel, text in self.files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        lines = []
        for name, fields in self.targets.items():
            lines.append(f"{name}\tsource_dir\t{self.root}")
            for key, value in fields.items():
                lines.append(f"{name}\t{key}\t{value}")
        manifest = self.root / "manifest.txt"
        manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return manifest


class DesignUnitBoundaryCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tree = Tree(Path(self._tmp.name))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_gate(self) -> int:
        return gate.main(["--manifest", str(self.tree.write())])

    def assert_fails(self) -> None:
        self.assertEqual(self.run_gate(), 1)

    def test_clean_layout_passes(self) -> None:
        self.assertEqual(self.run_gate(), 0)

    def test_design_import_off_configuration_passes(self) -> None:
        del self.tree.targets["pulp-design-import"]
        del self.tree.targets["pulp-design-import-audio-pack"]
        self.assertEqual(self.run_gate(), 0)

    def test_design_ir_header_reaching_view_fails(self) -> None:
        self.tree.files["include/pulp/view/design_ir.hpp"] += "#include <pulp/view/theme.hpp>\n"
        self.assert_fails()

    def test_design_ir_linking_a_pulp_target_fails(self) -> None:
        self.tree.targets["pulp-design-ir"]["interface_links"] = "pulp::canvas"
        self.assert_fails()

    def test_view_core_source_including_importer_fails(self) -> None:
        self.tree.files["src/view.cpp"] += "#include <pulp/view/design_import.hpp>\n"
        self.assert_fails()

    def test_view_core_private_header_including_importer_fails(self) -> None:
        self.tree.files["src/view_private.hpp"] += "#include <pulp/view/design_import.hpp>\n"
        self.assert_fails()

    def test_generic_public_header_including_importer_fails(self) -> None:
        self.tree.files["include/pulp/view/view.hpp"] += "#include <pulp/view/design_import.hpp>\n"
        self.assert_fails()

    def test_view_core_linking_importer_fails(self) -> None:
        self.tree.targets["pulp-view-core"]["links"] += ";pulp::design-import"
        self.assert_fails()

    def test_view_core_without_design_ir_fails(self) -> None:
        self.tree.targets["pulp-view-core"]["links"] = "pulp::canvas"
        self.assert_fails()

    def test_view_core_compiling_importer_source_fails(self) -> None:
        self.tree.targets["pulp-view-core"]["sources"] += ";src/design_import.cpp"
        self.assert_fails()

    def test_importer_linking_audio_fails(self) -> None:
        self.tree.targets["pulp-design-import"]["links"] += ";pulp-audio"
        self.assert_fails()

    def test_importer_link_wrapped_in_generator_expression_fails(self) -> None:
        self.tree.targets["pulp-design-import"]["links"] += ";$<BUILD_INTERFACE:pulp::signal>"
        self.assert_fails()

    def test_non_pulp_links_are_not_product_edges(self) -> None:
        self.tree.targets["pulp-design-import"]["links"] += (
            ";$<BUILD_INTERFACE:yaml-cpp::yaml-cpp>;$<INSTALL_INTERFACE:Pulp::yaml-cpp>;-framework CoreAudio")
        self.assertEqual(self.run_gate(), 0)

    def test_importer_linking_format_view_fails(self) -> None:
        self.tree.targets["pulp-design-import"]["links"] += ";pulp::format-view"
        self.assert_fails()

    def test_importer_including_signal_fails(self) -> None:
        self.tree.files["src/design_import.cpp"] += "#include <pulp/signal/biquad.hpp>\n"
        self.assert_fails()

    def test_importer_including_audio_pack_fails(self) -> None:
        self.tree.files["src/design_import.cpp"] += "#include <pulp/view/recognition_audio_pack.hpp>\n"
        self.assert_fails()

    def test_importer_linking_audio_pack_fails(self) -> None:
        self.tree.targets["pulp-design-import"]["links"] += ";pulp::design-import-audio-pack"
        self.assert_fails()

    def test_missing_manifest_header_fails(self) -> None:
        self.tree.targets["pulp-design-ir"]["headers"] += ";include/pulp/view/gone.hpp"
        self.assert_fails()

    def test_include_inside_string_literal_is_not_an_include(self) -> None:
        # The clean tree already carries one; the control is that it passes.
        self.assertIn('"#include <pulp/audio/', self.tree.files["src/design_import.cpp"])
        self.assertEqual(self.run_gate(), 0)

    def test_manifest_without_design_ir_is_inconclusive(self) -> None:
        del self.tree.targets["pulp-design-ir"]
        self.assertEqual(self.run_gate(), 2)

    def test_manifest_with_half_the_import_units_is_inconclusive(self) -> None:
        del self.tree.targets["pulp-design-import-audio-pack"]
        self.assertEqual(self.run_gate(), 2)


if __name__ == "__main__":
    unittest.main()
