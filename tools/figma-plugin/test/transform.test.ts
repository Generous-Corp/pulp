// Rotation / transform decoding for the Figma plugin lane (audit "Rotation /
// transform" row). decodeRelativeTransform lowers a node's 2x3
// relativeTransform to the `rotate(<deg>deg)` contract the shared codegen
// maps to setRotation, with the .fig lane's guards (fig/scene.mjs::styleFor)
// mirrored field-for-field: non-orthogonal angles only (0.5deg tolerance
// either side of each 90deg multiple — the #6277 slider-fill regression
// guard) and an unrepresentable-matrix path (skew / non-unit scale /
// mirror-plus-rotation) that diagnoses instead of faking an angle. Kept in
// lockstep with the REST mirror (figma_rest_export.py::_decode_rotation +
// test_figma_rest_export.py).

import { test } from "node:test";
import assert from "node:assert/strict";

import { decodeRelativeTransform, transformMatrixAttr } from "../src/extract-pure";

/// Figma-style rotation matrix for `deg` (clockwise, screen y down) with a
/// translation column — the exact shape relativeTransform carries.
const rot = (deg: number, tx = 0, ty = 0): number[][] => {
  const r = (deg * Math.PI) / 180;
  return [
    [Math.cos(r), -Math.sin(r), tx],
    [Math.sin(r), Math.cos(r), ty],
  ];
};

test("a 45deg rotation decodes to the rotate lowering with the matrix attr", () => {
  const d = decodeRelativeTransform(rot(45, 10, 20), "Needle");
  assert.equal(d.kind, "rotate");
  if (d.kind !== "rotate") return;
  assert.ok(Math.abs(d.deg - 45) < 1e-9);
  assert.equal(d.matrixAttr, transformMatrixAttr(rot(45, 10, 20)));
  // Stable trimmed formatting, row-major wire order.
  assert.equal(d.matrixAttr, "0.7071,-0.7071,10,0.7071,0.7071,20");
});

test("negative angles keep their sign (a needle sweeps both ways)", () => {
  const d = decodeRelativeTransform(rot(-43.4), "Needle");
  assert.equal(d.kind, "rotate");
  if (d.kind !== "rotate") return;
  assert.ok(Math.abs(d.deg - -43.4) < 1e-6);
});

test("orthogonal rotations fall through to identity — the #6277 guard", () => {
  // A 90deg multiple keeps the box axis-aligned; the AABB placement the walk
  // already emitted is exact, and re-rotating shifted a slider's
  // 180deg-rotated fill off its track.
  for (const deg of [0, 90, 180, -90, -180, 270]) {
    assert.equal(decodeRelativeTransform(rot(deg), "Fill").kind, "identity",
      `expected identity at ${deg}deg`);
  }
  // Just inside the 0.5deg tolerance: still identity (float noise, not design).
  assert.equal(decodeRelativeTransform(rot(90.4), "Fill").kind, "identity");
  // Just outside: a real rotation.
  assert.equal(decodeRelativeTransform(rot(91), "Fill").kind, "rotate");
});

test("translation-only and missing matrices are identity", () => {
  assert.equal(decodeRelativeTransform([[1, 0, 33], [0, 1, 44]], "Box").kind, "identity");
  assert.equal(decodeRelativeTransform(undefined, "Box").kind, "identity");
  assert.equal(decodeRelativeTransform([[1, 0], [0, 1]], "Box").kind, "identity");
});

test("a sheared matrix diagnoses transform-skew-approximated, never a fake angle", () => {
  const d = decodeRelativeTransform([[1, 0.5, 20], [0, 1, 30]], "Sheared");
  assert.equal(d.kind, "unrepresentable");
  if (d.kind !== "unrepresentable") return;
  assert.equal(d.diagnostic.code, "transform-skew-approximated");
  assert.equal(d.diagnostic.kind, "unsupported_property");
  assert.equal(d.diagnostic.severity, "warning");
  assert.match(d.diagnostic.message, /skew/);
  assert.equal(d.matrixAttr, "1,0.5,20,0,1,30");
});

test("non-unit scale diagnoses — rotate() carries no scale", () => {
  const d = decodeRelativeTransform([[2, 0, 0], [0, 2, 0]], "Scaled");
  assert.equal(d.kind, "unrepresentable");
  if (d.kind !== "unrepresentable") return;
  assert.match(d.diagnostic.message, /non-unit scale/);
  // Rotation + scale together is still unrepresentable, not "rotate".
  const rs = rot(30).map((row) => row.map((v) => v * 1.5));
  assert.equal(decodeRelativeTransform(rs, "Scaled").kind, "unrepresentable");
});

test("mirror-plus-rotation diagnoses; a pure orthogonal flip stays identity", () => {
  // Flip-H composed with a 30deg rotation: a center rotate() of atan2's angle
  // would render the un-mirrored art at a wrong-looking angle.
  const flipRot = [
    [-Math.cos(Math.PI / 6), -Math.sin(Math.PI / 6), 0],
    [-Math.sin(Math.PI / 6), Math.cos(Math.PI / 6), 0],
  ];
  const d = decodeRelativeTransform(flipRot, "Mirrored");
  assert.equal(d.kind, "unrepresentable");
  if (d.kind !== "unrepresentable") return;
  assert.match(d.diagnostic.message, /mirroring/);
  // A pure flip keeps the box axis-aligned — every lane's shipped behavior,
  // and diagnosing each flipped icon would bury real findings.
  assert.equal(decodeRelativeTransform([[-1, 0, 10], [0, 1, 0]], "FlipH").kind, "identity");
  assert.equal(decodeRelativeTransform([[1, 0, 0], [0, -1, 10]], "FlipV").kind, "identity");
});

test("degenerate (zero-scale) matrices are identity — nothing renders to rotate", () => {
  assert.equal(decodeRelativeTransform([[0, 0, 5], [0, 0, 5]], "Collapsed").kind, "identity");
});
