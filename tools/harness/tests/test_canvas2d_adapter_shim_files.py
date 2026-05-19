"""Regression test for the Canvas2dAdapter shim-file union (post-P5-6 split).

Pulp #2253 (P5-6 follow-up) extracted `measureText` / `drawImage` /
`setLineDash` / `getLineDash` / `getImageData` / `putImageData` out of
`web-compat-canvas.js` into a sibling prelude `web-compat-canvas-image.js`,
and `_PulpCanvasMatrix` into `web-compat-canvas-matrix.js`. The harness
adapter previously read only `web-compat-canvas.js`, so the canvas2d
compat verifier mis-classified the six extracted APIs as `NOT-IMPL`
(Codex P1 on PR #2253).

This test pins the contract that the adapter's shim text is the *union*
of the parent file + its sibling preludes — if a future split moves a
prototype method out into another prelude file that isn't in the union,
this fails loudly.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.canvas2d import Canvas2dAdapter  # noqa: E402


class Canvas2dShimFileUnionTests(unittest.TestCase):
    """Pin the prototype methods that live in the extracted preludes."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.adapter = Canvas2dAdapter(REPO_ROOT)

    def test_image_prelude_methods_resolve(self) -> None:
        """The six API gap-closure methods from `web-compat-canvas-image.js`
        must appear in the adapter's prototype-method set, otherwise the
        verifier reports `canvas2d/measureText` etc. as NOT-IMPL."""
        moved_to_image_prelude = {
            "measureText",
            "drawImage",
            "setLineDash",
            "getLineDash",
            "getImageData",
            "putImageData",
        }
        missing = moved_to_image_prelude - self.adapter._shim_methods
        self.assertFalse(
            missing,
            f"Adapter is not reading web-compat-canvas-image.js — missing: {sorted(missing)}",
        )

    def test_parent_canvas_methods_still_resolve(self) -> None:
        """Methods that stayed in `web-compat-canvas.js` must still appear
        (i.e. the union didn't accidentally drop the parent file)."""
        kept_in_parent = {"fillRect", "strokeRect", "fill", "stroke", "beginPath"}
        missing = kept_in_parent - self.adapter._shim_methods
        self.assertFalse(
            missing,
            f"Adapter dropped the parent web-compat-canvas.js — missing: {sorted(missing)}",
        )


if __name__ == "__main__":
    unittest.main()
