"""Deterministic raster rendering for visual-harness `kind: render` fixtures.

Fixtures declare their drawing as data (an `ops` list) rather than as code, so a
golden's identity is reviewable in the fixture diff and a renderer change cannot
quietly redefine what the golden depicts.

Renderer choice: this module rasterizes through the pinned `skia-python` wheel
(`pins.SKIA_PYTHON_SMOKE_VERSION`) rather than `pulp::view::render_to_png`.
That is deliberate and load-bearing for a cross-host byte comparison:

  * `render_to_png` resolves to CoreGraphics on Apple and to Skia elsewhere, so
    its bytes are per-platform by construction and can never be compared across
    hosts.
  * The Linux lane of this harness runs a Python container with no Pulp C++
    build, so a C++ raster is not reachable on both hosts at all.

The pinned wheel is the one rasterizer that executes on every host this harness
runs on, which is what makes a single shared golden meaningful.

Fixture content deliberately contains NO TEXT. Font rasterization is the largest
source of cross-platform divergence (CoreText vs fontconfig/FreeType, hinting,
subpixel positioning); a text-free fixture isolates geometry and blending so a
byte mismatch points at the raster stack instead of at font plumbing.
"""

from __future__ import annotations

import json
import os
import platform
from pathlib import Path
from typing import Any


# Fixture driver that routes a capture through this module instead of through
# the native visual binary.
DECLARATIVE_RASTER_DRIVER = "declarative_raster"


class LockedSkiaUnavailable(RuntimeError):
    """Raised when the pinned skia-python dependency cannot be used."""


def platform_key() -> str:
    """Stable host key, e.g. ``darwin-arm64`` / ``linux-x86_64``.

    Keys the per-host expectations in ``pins.RASTER_GOLDEN_SHA256``. Uses the
    ISA reported by the running interpreter, so an x86_64 interpreter under
    Rosetta keys as ``darwin-x86_64`` rather than as the host's native arm64.
    """
    system = platform.system().lower()
    machine = platform.machine().lower()
    if machine in {"amd64", "x64"}:
        machine = "x86_64"
    if machine == "aarch64":
        machine = "arm64"
    return f"{system}-{machine}"


def import_locked_skia():
    """Import skia-python, enforcing the pin.

    Raises LockedSkiaUnavailable when the module is missing. Callers decide
    whether that is a skip or a hard failure; CI sets PULP_VISUAL_REQUIRE_SKIA=1
    so a missing raster dependency fails loudly instead of passing as a skip.

    A wrong version is ALWAYS an AssertionError, never a skip: rendering with an
    unpinned Skia would compare bytes the golden never claimed to describe.
    """
    from tools.harness.visual import pins

    try:
        import skia  # type: ignore[import-not-found]
    except ImportError as exc:
        raise LockedSkiaUnavailable(
            "locked raster dependency not installed: skia-python=="
            f"{pins.SKIA_PYTHON_SMOKE_VERSION} could not be imported ({exc}). "
            "Install it with its platform runtime libraries, or run "
            "ci/visual-harness.Dockerfile."
        ) from exc

    actual = getattr(skia, "__version__", None)
    if actual != pins.SKIA_PYTHON_SMOKE_VERSION:
        raise AssertionError(
            "wrong skia-python version for a deterministic raster golden: "
            f"expected {pins.SKIA_PYTHON_SMOKE_VERSION}, got {actual!r}"
        )
    return skia


def require_locked_skia_env() -> bool:
    """True when the environment demands a real raster instead of a skip."""
    return os.environ.get("PULP_VISUAL_REQUIRE_SKIA") == "1"


def _argb(skia, value: str):
    text = value.strip().lstrip("#")
    if len(text) != 8:
        raise ValueError(f"colour must be #AARRGGBB, got {value!r}")
    a, r, g, b = (int(text[i : i + 2], 16) for i in (0, 2, 4, 6))
    return skia.ColorSetARGB(a, r, g, b)


def _paint(skia, op: dict[str, Any]):
    paint = skia.Paint(AntiAlias=bool(op.get("antialias", False)))
    paint.setColor(_argb(skia, op["color"]))
    width = op.get("stroke_width")
    if width is not None:
        paint.setStyle(skia.Paint.kStroke_Style)
        paint.setStrokeWidth(float(width))
    return paint


def render_ops_png(skia, viewport: dict[str, Any], ops: list[dict[str, Any]]) -> bytes:
    """Rasterize a fixture's declared ops to PNG bytes.

    Renders through an SkPicture before drawing to the surface so the recorded
    op stream, not just the final surface, is exercised.
    """
    width = int(viewport["w"])
    height = int(viewport["h"])
    if width <= 0 or height <= 0:
        raise ValueError(f"viewport must be positive, got {viewport!r}")

    recorder = skia.PictureRecorder()
    canvas = recorder.beginRecording(skia.Rect.MakeWH(width, height))

    for op in ops:
        kind = op.get("op")
        if kind == "clear":
            canvas.clear(_argb(skia, op["color"]))
        elif kind == "rect":
            canvas.drawRect(
                skia.Rect.MakeXYWH(
                    float(op["x"]), float(op["y"]), float(op["w"]), float(op["h"])
                ),
                _paint(skia, op),
            )
        elif kind == "circle":
            canvas.drawCircle(
                float(op["cx"]), float(op["cy"]), float(op["r"]), _paint(skia, op)
            )
        elif kind == "line":
            path = skia.Path()
            path.moveTo(float(op["x0"]), float(op["y0"]))
            path.lineTo(float(op["x1"]), float(op["y1"]))
            canvas.drawPath(path, _paint(skia, op))
        elif kind == "linear_gradient_rect":
            paint = skia.Paint(AntiAlias=bool(op.get("antialias", False)))
            paint.setShader(
                skia.GradientShader.MakeLinear(
                    points=[
                        (float(op["x0"]), float(op["y0"])),
                        (float(op["x1"]), float(op["y1"])),
                    ],
                    colors=[_argb(skia, c) for c in op["colors"]],
                )
            )
            canvas.drawRect(
                skia.Rect.MakeXYWH(
                    float(op["x"]), float(op["y"]), float(op["w"]), float(op["h"])
                ),
                paint,
            )
        else:
            raise ValueError(f"unsupported raster fixture op {kind!r}")

    picture = recorder.finishRecordingAsPicture()

    surface = skia.Surface(width, height)
    surface_canvas = surface.getCanvas()
    surface_canvas.clear(skia.ColorTRANSPARENT)
    surface_canvas.drawPicture(picture)
    image = surface.makeImageSnapshot()
    return bytes(image.encodeToData(skia.kPNG, 100))


def render_fixture_png(skia, fixture: dict[str, Any]) -> bytes:
    ops = fixture.get("ops")
    if not isinstance(ops, list) or not ops:
        raise ValueError("render fixture must declare a non-empty 'ops' list")
    viewport = fixture.get("viewport")
    if not isinstance(viewport, dict):
        raise ValueError("render fixture must declare a 'viewport' object")
    return render_ops_png(skia, viewport, ops)


def load_fixture(path: Path) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _main(argv: list[str] | None = None) -> int:
    """Report this host's raster digest for a fixture.

    Exists so a CI lane can publish the digest it actually computed, pass or
    fail. A mismatch is then attributable to a specific host and a specific
    value rather than to "the comparison failed somewhere".
    """
    import argparse
    import hashlib
    import sys

    parser = argparse.ArgumentParser(description=_main.__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    args = parser.parse_args(argv)

    try:
        skia = import_locked_skia()
    except LockedSkiaUnavailable as exc:
        print(f"{platform_key()} unavailable: {exc}", file=sys.stderr)
        return 1

    png = render_fixture_png(skia, load_fixture(args.fixture))
    print(
        f"platform={platform_key()} fixture={args.fixture} "
        f"bytes={len(png)} sha256={hashlib.sha256(png).hexdigest()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
