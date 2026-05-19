"""Regression test for the HtmlAdapter element-shim union (post-P5-7 split).

Pulp #2337 extracted Element.prototype's Event + Pointer-capture methods
(addEventListener / removeEventListener / dispatchEvent /
setPointerCapture / releasePointerCapture) out of
`web-compat-element.js` into a sibling prelude
`web-compat-element-events.js`. The harness `HtmlAdapter` previously
unioned only the parent file + web-compat-dom-ops.js + the legacy
web-compat.js bundle, so the `html/Element_setPointerCapture` oracle
would false-classify as NOT_IMPL after the split (Codex P2 on #2337).

This test pins the contract that the adapter's element-shim text is
the *union* of the four prelude files — if a future split moves a
prototype method into another file that isn't in the union, this
fails loudly. Mirrors the canvas2d-adapter regression test from #2317.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.html import HtmlAdapter  # noqa: E402


def _prototype_methods(text: str) -> set[str]:
    return set(re.findall(r"Element\.prototype\.([A-Za-z_][A-Za-z0-9_]*)\s*=", text))


class HtmlAdapterElementEventsUnionTests(unittest.TestCase):
    """Pin the methods that moved into the extracted prelude."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.adapter = HtmlAdapter(REPO_ROOT)
        cls.methods = _prototype_methods(cls.adapter._element_js)

    def test_events_prelude_methods_resolve(self) -> None:
        """Methods moved into web-compat-element-events.js must appear in
        the union. Without this, html/Element_setPointerCapture (et al.)
        false-classifies as NOT_IMPL."""
        moved_to_events_prelude = {
            "addEventListener",
            "removeEventListener",
            "dispatchEvent",
            "setPointerCapture",
            "releasePointerCapture",
        }
        missing = moved_to_events_prelude - self.methods
        self.assertFalse(
            missing,
            f"HtmlAdapter not unioning web-compat-element-events.js — missing: {sorted(missing)}",
        )

    def test_parent_element_methods_still_resolve(self) -> None:
        """Methods that stayed in `web-compat-element.js` must still appear
        (catches a future split that drops the parent from the union)."""
        kept_in_parent = {"setAttribute", "removeAttribute", "getBoundingClientRect"}
        missing = kept_in_parent - self.methods
        self.assertFalse(
            missing,
            f"HtmlAdapter dropped the parent web-compat-element.js — missing: {sorted(missing)}",
        )

    def test_dom_ops_methods_still_resolve(self) -> None:
        """Methods from web-compat-dom-ops.js must still appear (catches
        a future refactor that drops it from the union)."""
        dom_ops_methods = {"appendChild", "removeChild", "insertBefore", "replaceChild"}
        missing = dom_ops_methods - self.methods
        self.assertFalse(
            missing,
            f"HtmlAdapter dropped web-compat-dom-ops.js — missing: {sorted(missing)}",
        )


if __name__ == "__main__":
    unittest.main()
