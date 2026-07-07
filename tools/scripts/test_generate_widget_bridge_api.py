import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("generate_widget_bridge_api.py")
spec = importlib.util.spec_from_file_location("generate_widget_bridge_api", SCRIPT)
generator = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = generator
spec.loader.exec_module(generator)


class WidgetBridgeApiGeneratorTest(unittest.TestCase):
    def test_generated_types_model_capability_gated_globals(self):
        root = generator.repo_root()
        rows = generator.api_rows(root)
        autocaps = generator.read_autocaps(root)
        fingerprint = generator.widget_bridge_input_fingerprint(root)
        dts = generator.emit_types(rows, autocaps, fingerprint)

        self.assertIn("var createKnob: PulpBridgeAlwaysGlobals['createKnob'];", dts)
        self.assertIn("var on: PulpBridgeAlwaysGlobals['on'];", dts)
        self.assertIn("var navigatorGPU: PulpBridgeAlwaysGlobals['navigatorGPU'];", dts)
        self.assertIn("var setImageSource: PulpBridgeFilesystemGlobals['setImageSource'] | undefined;", dts)
        self.assertIn("var exec: PulpBridgeExecGlobals['exec'] | undefined;", dts)
        self.assertIn("export type PulpBridgeGlobals<C extends PulpBridgeCapability = never>", dts)
        self.assertRegex(dts, r"loadFont:\s+\(path: string\) => boolean;")
        self.assertRegex(dts, r"registerFont:\s+\(family: string, path: string\) => boolean;")
        self.assertRegex(dts, r"setSvgViewBox:\s+\(id: string, width: number, height: number\) => void;")
        self.assertRegex(dts, r"setCornerRadius:\s+\(id: string, corner: 'All' \| 'TopLeft' \| 'TopRight' \| 'BottomLeft' \| 'BottomRight' \| string, radius: number\) => void;")
        self.assertRegex(dts, r"setKnobSpriteCore:\s+\(id: string, coreX: number, coreY: number, coreWidth: number, coreHeight: number\) => void;")
        self.assertRegex(dts, r"setFaderSkin:\s+\(id: string, trackColor\?: string, fillColor\?: string, thumbColor\?: string, thumbBorderColor\?: string, thumbWidth\?: number, thumbHeight\?: number, cornerRadius\?: number\) => void;")
        self.assertRegex(dts, r"setFaderTrackBorder:\s+\(id: string, color: string\) => void;")
        self.assertRegex(dts, r"setMeterColors:\s+\(id: string, backgroundColor: string, stopsCsv: string\) => void;")
        self.assertRegex(dts, r"setListSelected:\s+\(id: string, value: number\) => void;")
        self.assertRegex(dts, r"setListRowHeight:\s+\(id: string, value: number\) => void;")
        self.assertRegex(dts, r"bindWidgetToParam:\s+\(widgetId: string, paramName: string, transform\?: PulpBridgeBindingTransform\) => boolean;")
        self.assertRegex(dts, r"bindMeter:\s+\(widgetId: string, source: string, transform\?: PulpBridgeBindingTransform\) => boolean;")
        self.assertRegex(dts, r"unbindWidget:\s+\(widgetId: string\) => number;")
        self.assertRegex(dts, r"createMeter:\s+\(id: string, orientation: 'vertical' \| 'horizontal', parentId: string\) => string;")

    def test_fingerprint_normalizes_text_line_endings(self):
        self.assertEqual(generator.normalized_text_bytes(b"a\r\nb\rc\n"), b"a\nb\nc\n")

    def test_mock_registry_excludes_host_objects(self):
        root = generator.repo_root()
        rows = generator.api_rows(root)
        fingerprint = generator.widget_bridge_input_fingerprint(root)
        mock = generator.emit_mock(
            generator.full_mock_names(rows),
            fingerprint,
            "bridgeMockFunctionNames",
            "BridgeMockFunctionName",
            "test",
        )

        self.assertIn("'createKnob'", mock)
        self.assertIn("'on'", mock)
        self.assertNotIn("'navigatorGPU'", mock)

    def test_safe_mock_registry_excludes_capability_gated_functions(self):
        root = generator.repo_root()
        rows = generator.api_rows(root)
        autocaps = generator.read_autocaps(root)
        names = generator.safe_mock_names(rows, autocaps)

        self.assertIn("createKnob", names)
        self.assertIn("on", names)
        self.assertNotIn("exec", names)
        self.assertNotIn("setImageSource", names)
        self.assertNotIn("navigatorGPU", names)

    def test_autocaps_are_manifest_backed(self):
        root = generator.repo_root()
        names = {row.name for row in generator.read_manifest(root)}
        autocaps = generator.read_autocaps(root)

        self.assertTrue(autocaps)
        self.assertLessEqual(set(autocaps), names)
        self.assertEqual(autocaps["setImageSource"], "filesystem")
        self.assertEqual(autocaps["storageGetItem"], "storage")
        self.assertEqual(autocaps["getAICli"], "ai")

    def test_generated_outputs_include_input_fingerprint(self):
        root = generator.repo_root()
        outputs = generator.build_outputs(root)
        fingerprint = generator.widget_bridge_input_fingerprint(root)

        self.assertRegex(fingerprint, r"^fnv1a64:[0-9a-f]{16}$")
        for rel, content in outputs.items():
            with self.subTest(path=str(rel)):
                self.assertIn(f"{generator.FINGERPRINT_PREFIX}{fingerprint}", content)

    def test_check_outputs_matches_generated_files_after_write(self):
        root = generator.repo_root()
        outputs = generator.build_outputs(root)

        for rel, expected in outputs.items():
            with self.subTest(path=str(rel)):
                self.assertEqual((root / rel).read_text(), expected)


if __name__ == "__main__":
    unittest.main()
