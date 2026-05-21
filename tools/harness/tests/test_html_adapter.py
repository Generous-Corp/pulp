"""Tests for the HTML harness adapter classifier.

Invocation::

    python3 -m unittest tools.harness.tests.test_html_adapter
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry  # noqa: E402
from tools.harness.adapters.html import HtmlAdapter  # noqa: E402
from tools.harness.status import Status  # noqa: E402


def _entry(
    name: str,
    *,
    status: str = "supported",
    maps_to: str = "WidgetBridge",
    supported_values: list[str] | None = None,
    unsupported_values: list[str] | None = None,
) -> CatalogEntry:
    return CatalogEntry(
        surface="html",
        name=f"html/{name}",
        status=status,
        maps_to=maps_to,
        supported_values=list(supported_values or []),
        unsupported_values=list(unsupported_values or []),
    )


class HtmlAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)

        oracle_dir = self.repo / "tools" / "harness" / "oracles" / "html"
        oracle_dir.mkdir(parents=True)
        oracle = {
            "entries": {
                "html/Element_setAttribute": {
                    "category": "element_method",
                    "js_member": "setAttribute",
                },
                "html/Element_missingMethod": {
                    "category": "element_method",
                    "js_member": "missingMethod",
                },
                "html/Element_badMethod": {"category": "element_method"},
                "html/Element_classList": {
                    "category": "element_property",
                    "js_member": "classList",
                },
                "html/Element_missingProperty": {
                    "category": "element_property",
                    "js_member": "missingProperty",
                },
                "html/Element_badProperty": {"category": "element_property"},
                "html/document_createElement": {
                    "category": "document_member",
                    "js_member": "createElement",
                    "expected_bridge_calls": ["createToggleButton"],
                },
                "html/document_missingBridge": {
                    "category": "document_member",
                    "js_member": "createElement",
                    "expected_bridge_calls": ["missingFactory"],
                },
                "html/document_missingMember": {
                    "category": "document_member",
                    "js_member": "missingMember",
                },
                "html/document_badMember": {"category": "document_member"},
                "html/button": {
                    "category": "html_tag",
                    "tag": "button",
                    "expected_bridge_calls": ["createToggleButton"],
                },
                "html/meter": {
                    "category": "html_tag",
                    "tag": "meter",
                    "expected_bridge_calls": [],
                },
                "html/slider": {
                    "category": "html_tag",
                    "tag": "slider",
                    "expected_bridge_calls": ["missingFactory"],
                },
                "html/badTag": {"category": "html_tag"},
                "html/ARIA": {
                    "category": "feature",
                    "evidence": "ARIA role attributes",
                },
                "html/unknownCategory": {"category": "mystery"},
            }
        }
        (oracle_dir / "html-supported.json").write_text(json.dumps(oracle))

        js_dir = self.repo / "core" / "view" / "js"
        js_dir.mkdir(parents=True)
        (js_dir / "web-compat-element.js").write_text(
            "Element.prototype.setAttribute = function() {};\n"
            "Object.defineProperty(Element.prototype, \"classList\", { get: function() {} });\n"
            "function _ensureNative(tag) {\n"
            "  if (tag === \"button\") return createToggleButton();\n"
            "  if (tag === \"slider\") return createSlider();\n"
            "}\n"
        )
        (js_dir / "web-compat-document.js").write_text(
            "var document = {\n"
            "  createElement: function() {},\n"
            "};\n"
        )
        (js_dir / "web-compat.js").write_text(
            "document.addEventListener = function() {};\n"
        )

        bridge_dir = self.repo / "core" / "view" / "src"
        bridge_dir.mkdir(parents=True)
        (bridge_dir / "widget_bridge.cpp").write_text(
            'engine_.register_function("createToggleButton", []{});\n'
        )

        self.adapter = HtmlAdapter(self.repo)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_evidence_helpers_find_methods_properties_document_and_bridge(self) -> None:
        self.assertTrue(self.adapter._element_has_method("setAttribute"))
        self.assertFalse(self.adapter._element_has_method(""))
        self.assertFalse(self.adapter._element_has_method("missingMethod"))
        self.assertTrue(self.adapter._element_has_property("classList"))
        self.assertFalse(self.adapter._element_has_property(""))
        self.assertFalse(self.adapter._element_has_property("missingProperty"))
        self.assertTrue(self.adapter._document_has_member("createElement"))
        self.assertTrue(self.adapter._document_has_member("addEventListener"))
        self.assertFalse(self.adapter._document_has_member(""))
        self.assertFalse(self.adapter._document_has_member("missingMember"))
        self.assertTrue(self.adapter._bridge_registers("createToggleButton"))
        self.assertFalse(self.adapter._bridge_registers(""))
        self.assertFalse(self.adapter._bridge_registers("missingFactory"))
        self.assertTrue(self.adapter._ensure_native_handles_tag("BUTTON"))
        self.assertFalse(self.adapter._ensure_native_handles_tag(""))
        self.assertFalse(self.adapter._ensure_native_handles_tag("meter"))

    def test_maps_to_helpers_classify_unimplemented_and_noop_markers(self) -> None:
        self.assertTrue(HtmlAdapter._maps_to_marks_unimpl("not wired"))
        self.assertFalse(HtmlAdapter._maps_to_marks_unimpl(""))
        self.assertFalse(HtmlAdapter._maps_to_marks_unimpl("WidgetBridge::create"))
        self.assertTrue(HtmlAdapter._maps_to_marks_noop("stored but not rendered"))
        self.assertFalse(HtmlAdapter._maps_to_marks_noop(""))
        self.assertFalse(HtmlAdapter._maps_to_marks_noop("WidgetBridge::create"))

    def test_wontfix_and_missing_oracle_entries_are_oos(self) -> None:
        wontfix = self.adapter.run(_entry("Element_setAttribute", status="wontfix"))
        self.assertIs(wontfix.status, Status.OOS)
        self.assertIn("wontfix", wontfix.detail)

        missing_oracle = self.adapter.run(_entry("notInOracle", status="missing"))
        self.assertIs(missing_oracle.status, Status.OOS)
        self.assertIn("not present in HTML oracle", missing_oracle.detail)

    def test_catalog_missing_and_noop_markers_short_circuit(self) -> None:
        missing = self.adapter.run(
            _entry("Element_setAttribute", status="missing", maps_to="no bridge")
        )
        self.assertIs(missing.status, Status.NOT_IMPL)
        self.assertIn("catalog says missing", missing.detail)

        noop = self.adapter.run(
            _entry("Element_setAttribute", status="noop", maps_to="accepted but ignored")
        )
        self.assertIs(noop.status, Status.NO_OP)
        self.assertIn("no-op acceptance", noop.detail)

    def test_element_method_and_property_categories(self) -> None:
        method = self.adapter.run(
            _entry("Element_setAttribute", supported_values=["string"])
        )
        self.assertIs(method.status, Status.PASS)
        self.assertEqual(method.matched_supported, ["string"])

        method_missing = self.adapter.run(_entry("Element_missingMethod", status="missing"))
        self.assertIs(method_missing.status, Status.NOT_IMPL)
        self.assertIn("Element.prototype.missingMethod", method_missing.detail)

        method_bad = self.adapter.run(_entry("Element_badMethod", status="missing"))
        self.assertIs(method_bad.status, Status.OOS)
        self.assertIn("missing js_member", method_bad.detail)

        prop = self.adapter.run(_entry("Element_classList"))
        self.assertIs(prop.status, Status.PASS)

        prop_missing = self.adapter.run(_entry("Element_missingProperty", status="missing"))
        self.assertIs(prop_missing.status, Status.NOT_IMPL)
        self.assertIn("missingProperty", prop_missing.detail)

        prop_bad = self.adapter.run(_entry("Element_badProperty", status="missing"))
        self.assertIs(prop_bad.status, Status.OOS)
        self.assertIn("missing js_member", prop_bad.detail)

    def test_document_member_category_checks_bridge_expectations(self) -> None:
        present = self.adapter.run(_entry("document_createElement"))
        self.assertIs(present.status, Status.PASS)
        self.assertIn("document.createElement present", present.detail)

        missing_bridge = self.adapter.run(_entry("document_missingBridge", status="partial"))
        self.assertIs(missing_bridge.status, Status.DIVERGE)
        self.assertEqual(missing_bridge.unmatched_supported, ["missingFactory"])

        missing_member = self.adapter.run(_entry("document_missingMember", status="missing"))
        self.assertIs(missing_member.status, Status.NOT_IMPL)
        self.assertIn("document.missingMember", missing_member.detail)

        bad_member = self.adapter.run(_entry("document_badMember", status="missing"))
        self.assertIs(bad_member.status, Status.OOS)
        self.assertIn("missing js_member", bad_member.detail)

    def test_html_tag_category_checks_dispatch_and_bridge_expectations(self) -> None:
        button = self.adapter.run(_entry("button"))
        self.assertIs(button.status, Status.PASS)
        self.assertIn("<button> wired", button.detail)

        missing_tag = self.adapter.run(_entry("meter", status="missing"))
        self.assertIs(missing_tag.status, Status.NOT_IMPL)
        self.assertIn("_ensureNative does not dispatch", missing_tag.detail)

        missing_bridge = self.adapter.run(_entry("slider", status="partial"))
        self.assertIs(missing_bridge.status, Status.DIVERGE)
        self.assertEqual(missing_bridge.unmatched_supported, ["missingFactory"])

        bad_tag = self.adapter.run(_entry("badTag", status="missing"))
        self.assertIs(bad_tag.status, Status.OOS)
        self.assertIn("missing tag", bad_tag.detail)

    def test_feature_and_unknown_categories(self) -> None:
        supported = self.adapter.run(_entry("ARIA", supported_values=["role"]))
        self.assertIs(supported.status, Status.PASS)
        self.assertEqual(supported.matched_supported, ["role"])

        partial = self.adapter.run(
            _entry("ARIA", status="partial", unsupported_values=["aria-live"])
        )
        self.assertIs(partial.status, Status.DIVERGE)
        self.assertEqual(partial.extra_unsupported, ["aria-live"])

        missing = self.adapter.run(_entry("ARIA", status="missing"))
        self.assertIs(missing.status, Status.NOT_IMPL)
        self.assertIn("feature claimed missing", missing.detail)

        unexpected = self.adapter.run(_entry("ARIA", status="experimental"))
        self.assertIs(unexpected.status, Status.NOT_IMPL)
        self.assertIn("unrecognized catalog status", unexpected.detail)

        unknown = self.adapter.run(_entry("unknownCategory", status="missing"))
        self.assertIs(unknown.status, Status.OOS)
        self.assertIn("unrecognized oracle category", unknown.detail)

    def test_unsupported_values_convert_bound_entries_to_diverge(self) -> None:
        result = self.adapter.run(
            _entry("Element_setAttribute", status="partial", unsupported_values=["none", "dataset"])
        )

        self.assertIs(result.status, Status.DIVERGE)
        self.assertEqual(result.extra_unsupported, ["dataset"])
        self.assertIn("catalog lists 1 unsupported", result.detail)


if __name__ == "__main__":
    unittest.main()
