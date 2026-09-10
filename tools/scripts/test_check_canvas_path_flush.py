#!/usr/bin/env python3
"""Self-test for check_canvas_path_flush.py.

The lint's whole value is that it fails on a missed flush, so this proves it
does -- and that it refuses to report success when its own parser stops
matching, which is the way a lint like this normally dies unnoticed.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

LINT = pathlib.Path(__file__).with_name("check_canvas_path_flush.py")

CLEAN = """
CanvasRenderingContext2D.prototype._fp = function() {
    if (typeof canvasPathPolyline === "function") canvasPathPolyline(this._id, []);
};
CanvasRenderingContext2D.prototype.moveTo = function(x, y) {
    this._pendPts = [x, y];
};
CanvasRenderingContext2D.prototype.lineTo = function(x, y) {
    if (typeof canvasLineTo === "function") canvasLineTo(this._id, x, y);
};
CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
    this._fp();
    if (typeof canvasFillRect === "function") canvasFillRect(this._id, x, y, w, h);
};
"""

DIRTY = CLEAN.replace("    this._fp();\n", "", 1)
# A guard is not an emission: this must stay clean even without a flush.
GUARD_ONLY = CLEAN + """
CanvasRenderingContext2D.prototype.isSupported = function() {
    return typeof canvasFillRect === "function";
};
"""


def run(source: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d) / "web-compat-canvas.js"
        f.write_text(source)
        return subprocess.run(
            [sys.executable, str(LINT), "--file", str(f)],
            capture_output=True,
            text=True,
        )


def main() -> int:
    failures = []

    r = run(CLEAN)
    if r.returncode != 0:
        failures.append(f"clean source rejected: {r.stderr.strip()}")

    r = run(DIRTY)
    if r.returncode == 0:
        failures.append("a fillRect() missing its flush was accepted")
    elif "fillRect" not in r.stderr:
        failures.append(f"violation did not name the method: {r.stderr.strip()}")

    r = run(GUARD_ONLY)
    if r.returncode != 0:
        failures.append(
            "a `typeof canvasX === \"function\"` guard was treated as an "
            f"emission: {r.stderr.strip()}"
        )

    # Parser drift must fail, not pass vacuously.
    r = run(CLEAN.replace("CanvasRenderingContext2D.prototype.", "Other.prototype."))
    if r.returncode == 0:
        failures.append("a file the parser no longer matches was reported clean")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("check_canvas_path_flush selftest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
