"""Tests for the CSS harness adapter classifier.

Invocation::

    python3 -m unittest tools.harness.tests.test_css_adapter
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
from tools.harness.adapters.css import CssAdapter  # noqa: E402
from tools.harness.status import Status  # noqa: E402


def _entry(
    name: str,
    *,
    status: str = "supported",
    maps_to: str = "View::style",
    supported_values: list[str] | None = None,
    unsupported_values: list[str] | None = None,
) -> CatalogEntry:
    return CatalogEntry(
        surface="css",
        name=f"css/{name}",
        status=status,
        maps_to=maps_to,
        supported_values=list(supported_values or []),
        unsupported_values=list(unsupported_values or []),
    )


class CssAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)

        oracle_dir = self.repo / "tools" / "harness" / "oracles" / "css"
        oracle_dir.mkdir(parents=True)
        oracle = {
            "enums": {
                "display": {"values": ["flex", "none"]},
                "overflow": {"values": ["hidden", "scroll"]},
                "opacity": {"values": ["0", "1"]},
            }
        }
        (oracle_dir / "css-supported.json").write_text(json.dumps(oracle))

        js_dir = self.repo / "core" / "view" / "js"
        js_dir.mkdir(parents=True)
        (js_dir / "web-compat-style-decl.js").write_text(
            'switch (prop) { case "display": break; case "overflow": break; }\n'
        )
        (js_dir / "web-compat-style-decl-layout.js").write_text(
            'switch (prop) { case "opacity": break; }\n'
        )
        (js_dir / "web-compat-style-decl-paint.js").write_text(
            'switch (prop) { case "color": break; }\n'
        )

        mdn_dir = self.repo / "tools" / "import-design" / "catalogs"
        mdn_dir.mkdir(parents=True)
        (mdn_dir / "mdn-css.tsv").write_text(
            "\n"
            "line-height\tcss-property\n"
            "not-enough-columns\n"
            "print-color-adjust\tcss-property\n"
        )

        react_dir = self.repo / "packages" / "pulp-react" / "src"
        react_dir.mkdir(parents=True)
        (react_dir / "prop-applier.ts").write_text("// fixture\n")

        self.adapter = CssAdapter(self.repo)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_extract_wired_cases_handles_empty_and_cases(self) -> None:
        self.assertEqual(CssAdapter._extract_wired_cases(""), set())
        self.assertEqual(
            CssAdapter._extract_wired_cases(
                'case "display":\ncase "fontSize" :\ndefault:'
            ),
            {"display", "fontSize"},
        )

    def test_maps_to_and_name_helpers(self) -> None:
        self.assertTrue(CssAdapter._maps_to_marks_unimpl(""))
        self.assertTrue(CssAdapter._maps_to_marks_unimpl("n/a"))
        self.assertTrue(CssAdapter._maps_to_marks_unimpl("no CSS bridge"))
        self.assertFalse(CssAdapter._maps_to_marks_unimpl("View::setOpacity"))
        self.assertFalse(CssAdapter._maps_to_marks_noop(""))
        self.assertTrue(CssAdapter._maps_to_marks_noop("accepted but ignored"))
        self.assertFalse(CssAdapter._maps_to_marks_noop("View::setOpacity"))
        self.assertEqual(CssAdapter._camel_to_kebab("borderTopLeftRadius"), "border-top-left-radius")
        self.assertTrue(CssAdapter._is_synthetic(_entry("__hover_pseudo")))
        self.assertFalse(CssAdapter._is_synthetic(_entry("display")))

    def test_missing_mdn_catalog_loads_as_empty_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            oracle_dir = repo / "tools" / "harness" / "oracles" / "css"
            oracle_dir.mkdir(parents=True)
            (oracle_dir / "css-supported.json").write_text(json.dumps({"enums": {}}))

            adapter = CssAdapter(repo)

        self.assertEqual(adapter._mdn, set())

    def test_wontfix_and_synthetic_entries_short_circuit(self) -> None:
        wontfix = self.adapter.run(_entry("display", status="wontfix"))
        self.assertIs(wontfix.status, Status.OOS)
        self.assertIn("wontfix", wontfix.detail)

        synthetic = self.adapter.run(
            _entry("__pseudo_classes_note", status="partial", maps_to="catalog note")
        )
        self.assertIs(synthetic.status, Status.DIVERGE)
        self.assertIn("synthetic feature entry", synthetic.detail)

    def test_unwired_entries_distinguish_unimplemented_oos_and_mdn_known(self) -> None:
        unimplemented = self.adapter.run(
            _entry("missingBranch", status="missing", maps_to="no bridge")
        )
        self.assertIs(unimplemented.status, Status.NOT_IMPL)
        self.assertIn("mapsTo signals no implementation", unimplemented.detail)

        out_of_scope = self.adapter.run(
            _entry("madeUpWidgetColor", status="missing", maps_to="design-only token")
        )
        self.assertIs(out_of_scope.status, Status.OOS)
        self.assertIn("not in the MDN catalog", out_of_scope.detail)

        mdn_known = self.adapter.run(
            _entry("lineHeight", status="missing", maps_to="View::lineHeight")
        )
        self.assertIs(mdn_known.status, Status.NOT_IMPL)
        self.assertIn('no `case "lineHeight":`', mdn_known.detail)

    def test_noop_marker_wins_when_catalog_marks_acceptance_stub(self) -> None:
        result = self.adapter.run(
            _entry("color", status="noop", maps_to="accepted but ignored")
        )

        self.assertIs(result.status, Status.NO_OP)
        self.assertIn("no-op acceptance", result.detail)

    def test_wired_enum_entry_passes_when_values_match_oracle(self) -> None:
        result = self.adapter.run(
            _entry("display", supported_values=["flex", "none", "grid"])
        )

        self.assertIs(result.status, Status.PASS)
        self.assertEqual(result.matched_supported, ["flex", "none"])

    def test_wired_enum_entry_diverges_when_values_are_partial(self) -> None:
        result = self.adapter.run(
            _entry("display", status="partial", supported_values=["flex"])
        )

        self.assertIs(result.status, Status.DIVERGE)
        self.assertEqual(result.matched_supported, ["flex"])
        self.assertEqual(result.unmatched_supported, ["none"])

    def test_wired_enum_entry_marks_not_impl_when_no_oracle_values_are_claimed(self) -> None:
        result = self.adapter.run(
            _entry("overflow", status="missing", supported_values=["visible"])
        )

        self.assertIs(result.status, Status.NOT_IMPL)
        self.assertEqual(result.unmatched_supported, ["hidden", "scroll"])

    def test_wired_enum_entry_diverges_when_unsupported_overlaps_oracle(self) -> None:
        result = self.adapter.run(
            _entry(
                "opacity",
                status="partial",
                supported_values=["0", "1"],
                unsupported_values=["1"],
            )
        )

        self.assertIs(result.status, Status.DIVERGE)
        self.assertEqual(result.extra_unsupported, ["1"])

    def test_wired_non_enum_uses_meaningful_unsupported_values(self) -> None:
        diverged = self.adapter.run(
            _entry(
                "color",
                status="partial",
                unsupported_values=["none", "lab()"],
            )
        )
        self.assertIs(diverged.status, Status.DIVERGE)
        self.assertEqual(diverged.extra_unsupported, ["lab()"])

        passed = self.adapter.run(_entry("color", unsupported_values=["none"]))
        self.assertIs(passed.status, Status.PASS)
        self.assertIn("non-enum CSS property", passed.detail)


if __name__ == "__main__":
    unittest.main()
