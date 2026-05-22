"""Tests for the React Native ViewStyle harness adapter classifier."""

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
from tools.harness.adapters.rn import RnAdapter  # noqa: E402
from tools.harness.status import Status  # noqa: E402


def _entry(
    name: str,
    *,
    status: str = "supported",
    maps_to: str = "prop-applier case -> setWidth",
    supported_values: list[str] | None = None,
    unsupported_values: list[str] | None = None,
) -> CatalogEntry:
    return CatalogEntry(
        surface="rn",
        name=f"rn/{name}",
        status=status,
        maps_to=maps_to,
        supported_values=list(supported_values or []),
        unsupported_values=list(unsupported_values or []),
    )


class RnAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)

        oracle_dir = self.repo / "tools" / "harness" / "oracles" / "rn"
        oracle_dir.mkdir(parents=True)
        oracle = {
            "properties": {
                "alignItems": {"kind": "enum", "values": ["center", "stretch"]},
                "display": {"kind": "enum-or-number", "values": ["flex", "none"]},
                "width": {"kind": "length"},
                "opacity": {"kind": "number"},
                "shadowColor": {"kind": "color", "platformOnly": "ios"},
                "backgroundColor": {"kind": "color"},
                "flexDirection": {"kind": "enum", "values": ["column", "row"]},
                "d": {"kind": "string"},
            },
            "bridgeFunctions": {
                "registered": [
                    "setAlignItems",
                    "setBackground",
                    "setFlex",
                    "setOpacity",
                    "setSvgPath",
                    "setWidth",
                ]
            },
        }
        (oracle_dir / "rn-viewstyle.json").write_text(json.dumps(oracle), encoding="utf-8")

        react_dir = self.repo / "packages" / "pulp-react" / "src"
        react_dir.mkdir(parents=True)
        files = {
            "prop-applier.ts": "switch (prop) { case 'width': break; }\n",
            "prop-applier-layout.ts": "switch (prop) { case 'alignItems': break; case 'display': break; }\n",
            "prop-applier-paint.ts": "switch (prop) { case 'background': break; }\n",
            "prop-applier-typography.ts": "// no typography fixture\n",
            "prop-applier-transform.ts": "switch (prop) { case 'direction': break; }\n",
            "prop-applier-events.ts": "switch (prop) { case 'd': break; }\n",
        }
        for name, text in files.items():
            (react_dir / name).write_text(text, encoding="utf-8")

        self.adapter = RnAdapter(self.repo)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_source_loading_and_helper_extractors(self) -> None:
        self.assertEqual(RnAdapter._extract_prop_applier_cases(""), set())
        self.assertEqual(
            RnAdapter._extract_prop_applier_cases("case 'width':\ncase 'alignItems' :"),
            {"width", "alignItems"},
        )
        self.assertEqual(self.adapter._read("missing.ts"), "")
        self.assertIn("width", self.adapter._prop_applier_cases)
        self.assertIn("background", self.adapter._prop_applier_cases)

    def test_maps_to_marker_helpers(self) -> None:
        self.assertTrue(RnAdapter._maps_to_marks_unimpl(""))
        self.assertTrue(RnAdapter._maps_to_marks_unimpl("no prop-applier case"))
        self.assertTrue(RnAdapter._maps_to_marks_unimpl("silently dropped by router"))
        self.assertFalse(RnAdapter._maps_to_marks_unimpl("prop-applier case -> setWidth"))
        self.assertFalse(RnAdapter._maps_to_marks_noop(""))
        self.assertTrue(RnAdapter._maps_to_marks_noop("accepted but ignored"))
        self.assertEqual(RnAdapter._bridge_fns_in_maps_to("setWidth then setOpacity"), ["setWidth", "setOpacity"])
        self.assertEqual(RnAdapter._bridge_fns_in_maps_to("no bridge"), [])

    def test_wontfix_unknown_and_platform_only_entries_are_oos(self) -> None:
        wontfix = self.adapter.run(_entry("width", status="wontfix"))
        self.assertIs(wontfix.status, Status.OOS)
        self.assertIn("wontfix", wontfix.detail)

        unknown = self.adapter.run(_entry("notAViewStyle", maps_to="prop-applier case -> setWidth"))
        self.assertIs(unknown.status, Status.OOS)
        self.assertIn("not present", unknown.detail)

        platform = self.adapter.run(_entry("shadowColor", maps_to="prop-applier case -> setShadowColor"))
        self.assertIs(platform.status, Status.OOS)
        self.assertIn("ios-only", platform.detail)

    def test_noop_and_unimplemented_markers_short_circuit(self) -> None:
        noop = self.adapter.run(_entry("width", status="noop", maps_to="accepted but ignored"))
        self.assertIs(noop.status, Status.NO_OP)
        self.assertIn("no-op acceptance", noop.detail)

        missing = self.adapter.run(_entry("width", status="missing", maps_to="no bridge support"))
        self.assertIs(missing.status, Status.NOT_IMPL)
        self.assertIn("no implementation", missing.detail)

    def test_unregistered_bridge_function_reports_diverge(self) -> None:
        result = self.adapter.run(_entry("width", status="partial", maps_to="prop-applier case -> setMissingBridge"))

        self.assertIs(result.status, Status.DIVERGE)
        self.assertIn("not in widget_bridge.cpp", result.detail)

    def test_missing_prop_applier_case_reports_not_impl_without_marker(self) -> None:
        result = self.adapter.run(_entry("opacity", status="missing", maps_to="View maps setOpacity"))

        self.assertIs(result.status, Status.NOT_IMPL)
        self.assertIn("no prop-applier case", result.detail)

    def test_alternate_prop_names_count_as_bindings(self) -> None:
        background = self.adapter.run(_entry("backgroundColor", maps_to="background prop -> setBackground"))
        self.assertIs(background.status, Status.PASS)
        self.assertIn("color", background.detail)

        direction = self.adapter.run(
            _entry("flexDirection", maps_to="prop-applier 'direction' -> setFlex", supported_values=["column", "row"])
        )
        self.assertIs(direction.status, Status.PASS)
        self.assertEqual(direction.matched_supported, ["column", "row"])

    def test_enum_entries_pass_diverge_or_report_missing_values(self) -> None:
        passed = self.adapter.run(_entry("alignItems", supported_values=["center", "stretch", "baseline"]))
        self.assertIs(passed.status, Status.PASS)
        self.assertEqual(passed.matched_supported, ["center", "stretch"])

        partial = self.adapter.run(_entry("alignItems", status="partial", supported_values=["center"]))
        self.assertIs(partial.status, Status.DIVERGE)
        self.assertEqual(partial.matched_supported, ["center"])
        self.assertEqual(partial.unmatched_supported, ["stretch"])

        none_claimed = self.adapter.run(_entry("display", status="partial", supported_values=["grid"]))
        self.assertIs(none_claimed.status, Status.DIVERGE)
        self.assertEqual(none_claimed.unmatched_supported, ["flex", "none"])

    def test_enum_unsupported_overlap_blocks_pass(self) -> None:
        result = self.adapter.run(
            _entry(
                "display",
                status="partial",
                supported_values=["flex", "none"],
                unsupported_values=["none"],
            )
        )

        self.assertIs(result.status, Status.DIVERGE)
        self.assertEqual(result.extra_unsupported, ["none"])

    def test_non_enum_entries_handle_meaningful_unsupported_and_all_marker(self) -> None:
        diverged = self.adapter.run(_entry("width", status="partial", unsupported_values=["none", "auto"]))
        self.assertIs(diverged.status, Status.DIVERGE)
        self.assertEqual(diverged.extra_unsupported, ["auto"])

        all_missing = self.adapter.run(_entry("width", status="missing", unsupported_values=["all"]))
        self.assertIs(all_missing.status, Status.NOT_IMPL)
        self.assertIn("rejects every value", all_missing.detail)

        passed = self.adapter.run(_entry("width", unsupported_values=["none"]))
        self.assertIs(passed.status, Status.PASS)
        self.assertIn("length", passed.detail)


if __name__ == "__main__":
    unittest.main()
