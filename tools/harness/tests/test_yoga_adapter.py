"""Tests for the Yoga harness adapter classifier."""

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
from tools.harness.adapters.yoga import YogaAdapter  # noqa: E402
from tools.harness.status import Status  # noqa: E402


def _entry(
    name: str,
    *,
    status: str = "supported",
    maps_to: str = "FlexStyle.width -> YGNodeStyleSetWidth",
    supported_values: list[str] | None = None,
    unsupported_values: list[str] | None = None,
) -> CatalogEntry:
    return CatalogEntry(
        surface="yoga",
        name=f"yoga/{name}",
        status=status,
        maps_to=maps_to,
        supported_values=list(supported_values or []),
        unsupported_values=list(unsupported_values or []),
    )


class YogaAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)

        oracle_dir = self.repo / "tools" / "harness" / "oracles" / "yoga"
        oracle_dir.mkdir(parents=True)
        oracle = {
            "properties": {
                "alignItems": {"kind": "enum", "values": ["center", "stretch"]},
                "display": {"kind": "enum", "values": ["flex", "none"]},
                "height": {"kind": "length"},
                "width": {"kind": "length"},
                "gap": {"kind": "length-or-percentage"},
                "position": {"kind": "enum", "values": ["static", "relative", "absolute"]},
            }
        }
        (oracle_dir / "yoga-supported.json").write_text(json.dumps(oracle), encoding="utf-8")

        include_dir = self.repo / "core" / "view" / "include" / "pulp" / "view"
        include_dir.mkdir(parents=True)
        (include_dir / "geometry.hpp").write_text(
            "struct FlexStyle { int align_items; int display; float width; float gap; };\n",
            encoding="utf-8",
        )
        (include_dir / "view.hpp").write_text(
            "class View { void set_position(int); int position() const; };\n",
            encoding="utf-8",
        )
        src_dir = self.repo / "core" / "view" / "src"
        src_dir.mkdir(parents=True)
        (src_dir / "yoga_layout.cpp").write_text(
            "YGNodeStyleSetWidth(node, style.width);\nYGNodeStyleSetGap(node, YGGutterAll, style.gap);\n",
            encoding="utf-8",
        )

        self.adapter = YogaAdapter(self.repo)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_source_loading_and_helper_extractors(self) -> None:
        self.assertEqual(self.adapter._read("missing.hpp"), "")
        self.assertTrue(self.adapter._flex_style_has_field(["width"]))
        self.assertTrue(self.adapter._flex_style_has_field(["missing", "align_items"]))
        self.assertFalse(self.adapter._flex_style_has_field(["height"]))
        self.assertTrue(self.adapter._view_has_accessor(["position"]))
        self.assertFalse(self.adapter._view_has_accessor(["left"]))
        self.assertTrue(self.adapter._yoga_layout_has_call("YGNodeStyleSetWidth"))
        self.assertEqual(YogaAdapter._camel_to_snake("alignItems"), "align_items")

    def test_enum_value_normalization(self) -> None:
        self.assertEqual(YogaAdapter._normalize_enum_value("flex (implicit)"), "flex")
        self.assertEqual(YogaAdapter._normalize_enum_value("none (via setVisible(false))"), "none")
        self.assertEqual(YogaAdapter._normalize_enum_value("mid (paren) inside"), "mid (paren) inside")
        self.assertEqual(YogaAdapter._normalize_enum_value("unbalanced ("), "unbalanced (")
        self.assertEqual(YogaAdapter._normalize_enum_value(""), "")
        self.assertEqual(YogaAdapter._normalize_enum_values(["flex (implicit)", "", "none"]), {"flex", "none"})

    def test_maps_to_marker_helpers(self) -> None:
        self.assertTrue(YogaAdapter._maps_to_marks_unimpl(""))
        self.assertTrue(YogaAdapter._maps_to_marks_unimpl("no `align_content` branch"))
        self.assertTrue(YogaAdapter._maps_to_marks_unimpl("RTL unsupported"))
        self.assertFalse(YogaAdapter._maps_to_marks_unimpl("FlexStyle.width"))
        self.assertFalse(YogaAdapter._maps_to_marks_noop(""))
        self.assertTrue(YogaAdapter._maps_to_marks_noop("accepted but ignored"))

    def test_wontfix_unknown_noop_and_unimplemented_markers(self) -> None:
        wontfix = self.adapter.run(_entry("width", status="wontfix"))
        self.assertIs(wontfix.status, Status.OOS)
        self.assertIn("wontfix", wontfix.detail)

        unknown = self.adapter.run(_entry("float", maps_to="FlexStyle.float"))
        self.assertIs(unknown.status, Status.OOS)
        self.assertIn("not present", unknown.detail)

        missing = self.adapter.run(_entry("width", maps_to="no implementation today"))
        self.assertIs(missing.status, Status.NOT_IMPL)
        self.assertIn("no implementation", missing.detail)

        noop = self.adapter.run(_entry("width", status="noop", maps_to="accepted but ignored"))
        self.assertIs(noop.status, Status.NO_OP)
        self.assertIn("no-op acceptance", noop.detail)

    def test_missing_binding_reports_not_impl(self) -> None:
        result = self.adapter.run(_entry("height", maps_to="FlexStyle.height"))

        self.assertIs(result.status, Status.NOT_IMPL)
        self.assertIn("no FlexStyle field or View accessor", result.detail)

    def test_flexstyle_and_view_accessor_bindings_pass(self) -> None:
        width = self.adapter.run(_entry("width", maps_to="FlexStyle.width -> YGNodeStyleSetWidth"))
        self.assertIs(width.status, Status.PASS)
        self.assertIn("length", width.detail)

        position = self.adapter.run(
            _entry(
                "position",
                maps_to="View::set_position -> YGNodeStyleSetPositionType",
                supported_values=["static", "relative", "absolute"],
            )
        )
        self.assertIs(position.status, Status.PASS)
        self.assertEqual(position.matched_supported, ["absolute", "relative", "static"])

    def test_enum_entries_pass_diverge_or_report_missing_values(self) -> None:
        passed = self.adapter.run(_entry("display", supported_values=["flex (implicit)", "none"]))
        self.assertIs(passed.status, Status.PASS)
        self.assertEqual(passed.matched_supported, ["flex", "none"])

        partial = self.adapter.run(_entry("alignItems", status="partial", supported_values=["center"]))
        self.assertIs(partial.status, Status.DIVERGE)
        self.assertEqual(partial.matched_supported, ["center"])
        self.assertEqual(partial.unmatched_supported, ["stretch"])

        blocked = self.adapter.run(_entry("display", supported_values=["flex", "none"], unsupported_values=["none"]))
        self.assertIs(blocked.status, Status.DIVERGE)
        self.assertEqual(blocked.extra_unsupported, ["none"])

        none_claimed = self.adapter.run(_entry("alignItems", status="partial", supported_values=[]))
        self.assertIs(none_claimed.status, Status.NOT_IMPL)
        self.assertEqual(none_claimed.unmatched_supported, ["center", "stretch"])

    def test_non_enum_entries_handle_unsupported_values(self) -> None:
        diverged = self.adapter.run(_entry("gap", status="partial", unsupported_values=["none", "auto"]))
        self.assertIs(diverged.status, Status.DIVERGE)
        self.assertEqual(diverged.extra_unsupported, ["none", "auto"])

        passed = self.adapter.run(_entry("gap", unsupported_values=["none"]))
        self.assertIs(passed.status, Status.PASS)
        self.assertIn("length-or-percentage", passed.detail)


if __name__ == "__main__":
    unittest.main()
