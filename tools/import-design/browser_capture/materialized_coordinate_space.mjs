// SPDX-License-Identifier: MIT

// Coordinate contract for executable Chromium materializations.
//
// DOMSnapshot.bounds are expressed after ancestor CSS transforms, while font
// sizes, Canvas client dimensions, border widths, and the React tree itself
// remain authored in local CSS pixels.  Pairing those two spaces produces a
// plausible but systematically clipped native result.  Preserve the authored
// frame and the complete affine mapping separately so every downstream datum
// can be normalized deliberately.

const finite = (value) => typeof value === 'number' && Number.isFinite(value);

export function normalizeMaterializedCoordinateSpace(value,
                                                     label = 'coordinate space') {
  if (!value || typeof value !== 'object' ||
      value.schema !== 'pulp-materialized-coordinate-space-v1' ||
      !value.authored_box || !value.captured_transform) {
    throw new Error(`${label} is invalid`);
  }
  const width = Number(value.authored_box.width);
  const height = Number(value.authored_box.height);
  const matrix = ['a', 'b', 'c', 'd', 'e', 'f'].map(
    key => Number(value.captured_transform[key]));
  if (![width, height, ...matrix].every(finite) || width <= 0 || height <= 0 ||
      width > 65536 || height > 65536) {
    throw new Error(`${label} is non-finite or out of range`);
  }
  const [a, b, c, d, e, f] = matrix;
  const determinant = a * d - b * c;
  if (!finite(determinant) || Math.abs(determinant) < 1e-9) {
    throw new Error(`${label} transform is singular`);
  }
  return {
    schema: 'pulp-materialized-coordinate-space-v1',
    authored_box: { width, height },
    captured_transform: { a, b, c, d, e, f },
  };
}

export function materializedCoordinateSpaceFromQuad(value) {
  const width = Number(value?.width);
  const height = Number(value?.height);
  const points = [value?.p1, value?.p2, value?.p4];
  if (![width, height].every(finite) || width <= 0 || height <= 0 ||
      !points.every(point => point && finite(Number(point.x)) &&
        finite(Number(point.y)))) {
    throw new Error('materialized authored quad is invalid');
  }
  const p1 = points[0], p2 = points[1], p4 = points[2];
  return normalizeMaterializedCoordinateSpace({
    schema: 'pulp-materialized-coordinate-space-v1',
    authored_box: { width, height },
    captured_transform: {
      a: (Number(p2.x) - Number(p1.x)) / width,
      b: (Number(p2.y) - Number(p1.y)) / width,
      c: (Number(p4.x) - Number(p1.x)) / height,
      d: (Number(p4.y) - Number(p1.y)) / height,
      e: Number(p1.x),
      f: Number(p1.y),
    },
  });
}

export function materializedRectToAuthored(row, coordinateSpace) {
  if (!coordinateSpace) return Array.isArray(row) ? row.slice(0, 4) : null;
  const space = normalizeMaterializedCoordinateSpace(coordinateSpace);
  if (!Array.isArray(row) || row.length < 4) return null;
  const rect = row.slice(0, 4).map(Number);
  if (!rect.every(finite) || rect[2] < 0 || rect[3] < 0) return null;
  const { a, b, c, d, e, f } = space.captured_transform;
  const determinant = a * d - b * c;
  const inverse = (x, y) => ({
    x: (d * (x - e) - c * (y - f)) / determinant,
    y: (-b * (x - e) + a * (y - f)) / determinant,
  });
  const corners = [
    inverse(rect[0], rect[1]),
    inverse(rect[0] + rect[2], rect[1]),
    inverse(rect[0], rect[1] + rect[3]),
    inverse(rect[0] + rect[2], rect[1] + rect[3]),
  ];
  const xs = corners.map(point => point.x);
  const ys = corners.map(point => point.y);
  const left = Math.min(...xs), top = Math.min(...ys);
  return [left, top, Math.max(...xs) - left, Math.max(...ys) - top];
}
