"""Map a native layout box through the rotations the native tree paints.

The native layout snapshot reports each View's untransformed box, while a
Chromium DOMSnapshot reports the bounding box of the element after its CSS
transform (and every ancestor's). Native lowering applies a lone
`rotate(<n>deg)` as `View::set_rotation`, which turns the View and its subtree
about the View's centre (the importer folds the authored transform-origin into
the box position), so the comparable native box is the axis-aligned bounds of
the layout box under the composed rotations of the node and its ancestors.
"""

from __future__ import annotations

import math
import re

# (a, b, c, d, e, f): x' = a*x + c*y + e, y' = b*x + d*y + f.
Affine = tuple[float, float, float, float, float, float]
IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)

# The same contract as css_transform_rotation(): one rotate() in degrees and
# nothing else. A transform list or another unit is not rotated natively, so it
# must not be rotated here either.
_ROTATE = re.compile(r"^\s*rotate\(\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*(deg)?\s*\)\s*$")


def css_rotation_degrees(transform: object) -> float | None:
    if not isinstance(transform, str):
        return None
    match = _ROTATE.match(transform)
    return float(match.group(1)) if match else None


def compose(outer: Affine, inner: Affine) -> Affine:
    a1, b1, c1, d1, e1, f1 = outer
    a2, b2, c2, d2, e2, f2 = inner
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2,
            a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


def rotation_about(degrees: float, cx: float, cy: float) -> Affine:
    radians = math.radians(degrees)
    cos, sin = math.cos(radians), math.sin(radians)
    return (cos, sin, -sin, cos, cx - cos * cx + sin * cy, cy - sin * cx - cos * cy)


def node_transform(parent: Affine, transform: object,
                   box: tuple[float, float, float, float] | None) -> Affine:
    """The page transform for a node: its parent's, then its own rotation."""
    degrees = css_rotation_degrees(transform)
    if degrees is None or box is None:
        return parent
    x, y, width, height = box
    return compose(parent, rotation_about(degrees, x + width / 2.0, y + height / 2.0))


def transformed_bounds(matrix: Affine, box: tuple[float, float, float, float] | None
                       ) -> tuple[float, float, float, float] | None:
    if box is None or matrix == IDENTITY:
        return box
    a, b, c, d, e, f = matrix
    x, y, width, height = box
    corners = [(x, y), (x + width, y), (x, y + height), (x + width, y + height)]
    xs = [a * px + c * py + e for px, py in corners]
    ys = [b * px + d * py + f for px, py in corners]
    return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))
