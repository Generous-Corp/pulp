// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  materializedCoordinateSpaceFromQuad,
  materializedRectToAuthored,
  normalizeMaterializedCoordinateSpace,
} from './materialized_coordinate_space.mjs';

const spectr = materializedCoordinateSpaceFromQuad({
  width: 1320, height: 860,
  p1: { x: 26.046511627906966, y: 0 },
  p2: { x: 1253.953488372093, y: 0 },
  p4: { x: 26.046511627906966, y: 800 },
});

test('preserves authored space separately from the captured affine', () => {
  assert.equal(spectr.authored_box.width, 1320);
  assert.equal(spectr.authored_box.height, 860);
  assert.ok(Math.abs(spectr.captured_transform.a - 800 / 860) < 1e-12);
  assert.equal(spectr.captured_transform.b, 0);
  assert.equal(spectr.captured_transform.c, 0);
  assert.ok(Math.abs(spectr.captured_transform.d - 800 / 860) < 1e-12);
});

test('maps captured boxes back to authored CSS pixels exactly once', () => {
  const authored = materializedRectToAuthored(
    [666.046511627907, 10.232558139534884, 44.65116279069767,
      20.46511627906977], spectr);
  assert.ok(Math.abs(authored[0] - 688) < 1e-9);
  assert.ok(Math.abs(authored[1] - 11) < 1e-9);
  assert.ok(Math.abs(authored[2] - 48) < 1e-9);
  assert.ok(Math.abs(authored[3] - 22) < 1e-9);
});

test('retains a full affine and rejects a scalar-only or singular contract', () => {
  const rotated = materializedCoordinateSpaceFromQuad({
    width: 100, height: 50,
    p1: { x: 8, y: 12 }, p2: { x: 8, y: 112 }, p4: { x: -42, y: 12 },
  });
  assert.deepEqual(rotated.captured_transform,
    { a: 0, b: 1, c: -1, d: 0, e: 8, f: 12 });
  assert.throws(() => normalizeMaterializedCoordinateSpace({
    schema: 'pulp-materialized-coordinate-space-v1',
    authored_box: { width: 100, height: 50 },
    captured_transform: { a: 1, b: 0, c: 2, d: 0, e: 0, f: 0 },
  }), /singular/);
});
