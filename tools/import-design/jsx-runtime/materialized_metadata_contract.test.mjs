// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  materializedAbsoluteInsets,
  materializedMergedTextBindings,
  materializedRuntimeFontStack,
  materializedSvgRectGeometry,
  materializedTextTargetGeometry,
  normalizeMaterializedMetadata,
} from './materialized_metadata_contract.mjs';

const rectPaint = {
  anchor: '#root',
  path: [{ tag: 'svg', index: 0 }, { tag: 'rect', index: 0 }],
  tag: 'rect',
  paint: {
    color: 'rgb(255, 255, 255)', fill: 'rgb(94, 94, 237)',
    stroke: 'none', opacity: 1, fill_opacity: 1, stroke_opacity: 1,
    stroke_width: 0, stroke_dasharray: 'none',
  },
  box: { left: 0.9375, top: 5.578125, width: 1.875, height: 3.75 },
};

test('preserves captured rect geometry through shared metadata normalization', () => {
  const normalized = normalizeMaterializedMetadata({
    paint_bindings: [rectPaint],
  });
  assert.deepEqual(normalized.paint_bindings[0].box, rectPaint.box);
});

test('replays captured SVG rect boxes in viewport-local coordinates', () => {
  assert.deepEqual(materializedSvgRectGeometry(rectPaint.box), {
    x: 0.9375, y: 5.578125, width: 1.875, height: 3.75,
  });
  assert.equal(materializedSvgRectGeometry({
    left: 0, top: 0, width: Number.NaN, height: 1,
  }), null);
});

test('rejects invalid or non-rect paint geometry', () => {
  assert.throws(() => normalizeMaterializedMetadata({
    paint_bindings: [{ ...rectPaint, box: { ...rectPaint.box, width: NaN } }],
  }), /paint binding 0 box is invalid/);
  assert.throws(() => normalizeMaterializedMetadata({
    paint_bindings: [{ ...rectPaint, tag: 'path' }],
  }), /paint binding 0 box is invalid/);
});

test('preserves a finite non-singular authored coordinate contract', () => {
  const coordinate = {
    schema: 'pulp-materialized-coordinate-space-v1',
    authored_box: { width: 1320, height: 860 },
    captured_transform: {
      a: 800 / 860, b: 0, c: 0, d: 800 / 860,
      e: 26.046511627906966, f: 0,
    },
  };
  assert.deepEqual(normalizeMaterializedMetadata({
    coordinate_space: coordinate,
  }).coordinate_space, coordinate);
  assert.throws(() => normalizeMaterializedMetadata({
    coordinate_space: { ...coordinate,
      captured_transform: { a: 1, b: 0, c: 1, d: 0, e: 0, f: 0 } },
  }), /singular/);
});

test('passes an already quoted materialized font alias to the renderer verbatim', () => {
  const runtime = '"JetBrains Mono [pulp-materialized-asset-deadbeef]"';
  const requested = '"JetBrains Mono", ui-monospace, monospace';
  const stack = materializedRuntimeFontStack({
    runtime_font_family: runtime,
    basis: { requested: { font_family: requested } },
  });
  assert.equal(stack, `${runtime}, ${requested}`);
  assert.doesNotMatch(stack, /\\"JetBrains/);
});

test('drops legacy SVG primitive ink bounds from normalized layout metadata', () => {
  const box = { left: 3.859375, top: 1.8125, width: 14.28125, height: 8 };
  const normalized = normalizeMaterializedMetadata({
    layout_bindings: [
      { anchor: '#root', path: [{ tag: 'svg', index: 0 }],
        box: { left: 11, top: 6, width: 22, height: 16 } },
      { anchor: '#root', path: [
        { tag: 'svg', index: 0 }, { tag: 'path', index: 0 },
      ], box },
    ],
  });

  assert.equal(normalized.layout_bindings.length, 1);
  assert.equal(normalized.layout_bindings[0].path.at(-1).tag, 'svg');
});

test('keeps fractional captured text width across first-commit Yoga rounding', () => {
  assert.deepEqual(materializedTextTargetGeometry(
    29.03125,
    { localX: 1, localY: 1, offsetWidth: 28 },
    { offsetWidth: 30, borderLeftWidth: 1, borderRightWidth: 1 },
  ), { localX: 1, localY: 1, basisWidth: 27.03125 });
});

test('translates captured border-box coordinates into Yoga absolute insets', () => {
  assert.deepEqual(materializedAbsoluteInsets(
    { left: 39, top: 6.75 },
    { borderLeftWidth: 1, borderTopWidth: 1 },
    { marginLeft: 6, marginTop: 0 },
  ), { left: 32, top: 5.75 });
});

test('state typography inherits new base targets and overrides matching paths', () => {
  const sculpt = { anchor: '#root', path: [{ tag: 'span', index: 0 }],
    text: 'SCULPT ▾' };
  const basePeak = { anchor: '#root', path: [{ tag: 'span', index: 1 }],
    text: 'PEAK ▾', boxes: [{ top: 1 }] };
  const statePeak = { ...basePeak, boxes: [{ top: 2 }] };
  const menu = { anchor: '#root', path: [{ tag: 'div', index: 9 }],
    text: 'MENU' };
  assert.deepEqual(materializedMergedTextBindings(
    [sculpt, basePeak], [statePeak, menu]), [
      { ...sculpt, runtime_optional: true }, statePeak, menu,
    ]);
});
