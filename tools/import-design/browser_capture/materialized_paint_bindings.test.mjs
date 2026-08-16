// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import { buildMaterializedPaintBindings } from './materialized_paint_bindings.mjs';

const styleNames = ['color', 'fill', 'stroke', 'opacity', 'fill-opacity',
  'stroke-opacity', 'stroke-width', 'stroke-dasharray'];

test('captures resolved currentColor and multicolor SVG paint by structural path', () => {
  const strings = ['#document', 'HTML', 'BODY', 'DIV', 'SVG', 'PATH', 'RECT',
    'id', 'root', 'rgba(255, 255, 255, 0.85)', 'none', '1', '1.4px',
    'rgb(94, 94, 237)'];
  const style = (...values) => values.map(value => strings.indexOf(value));
  const snapshot = {
    strings,
    computedStyleNames: styleNames,
    documents: [{
      nodes: {
        parentIndex: [-1, 0, 1, 2, 3, 4, 4],
        nodeType: [9, 1, 1, 1, 1, 1, 1],
        nodeName: [0, 1, 2, 3, 4, 5, 6],
        attributes: [[], [], [], [7, 8], [], [], []],
      },
      layout: {
        nodeIndex: [3, 4, 5, 6],
        bounds: [[0, 0, 320, 240], [10, 20, 16, 16],
          [11, 21, 14, 14], [12, 26, 2, 4]],
        styles: [
          style('rgba(255, 255, 255, 0.85)', 'none', 'none', '1', '1', '1', '1.4px', 'none'),
          style('rgba(255, 255, 255, 0.85)', 'none', 'none', '1', '1', '1', '1.4px', 'none'),
          style('rgba(255, 255, 255, 0.85)', 'none', 'rgba(255, 255, 255, 0.85)', '1', '1', '1', '1.4px', 'none'),
          style('rgba(255, 255, 255, 0.85)', 'rgb(94, 94, 237)', 'none', '1', '1', '1', '1.4px', 'none'),
        ],
      },
    }],
  };
  assert.deepEqual(buildMaterializedPaintBindings(snapshot), [{
    index: 0, anchor: '#root',
    path: [{ tag: 'svg', index: 0 }], tag: 'svg',
    paint: { color: 'rgba(255, 255, 255, 0.85)', fill: 'none', stroke: 'none',
      opacity: 1, fill_opacity: 1, stroke_opacity: 1,
      stroke_width: 1.4, stroke_dasharray: 'none' },
  }, {
    index: 1, anchor: '#root',
    path: [{ tag: 'svg', index: 0 }, { tag: 'path', index: 0 }], tag: 'path',
    paint: { color: 'rgba(255, 255, 255, 0.85)', fill: 'none',
      stroke: 'rgba(255, 255, 255, 0.85)', opacity: 1,
      fill_opacity: 1, stroke_opacity: 1, stroke_width: 1.4,
      stroke_dasharray: 'none' },
  }, {
    index: 2, anchor: '#root',
    path: [{ tag: 'svg', index: 0 }, { tag: 'rect', index: 1 }], tag: 'rect',
    paint: { color: 'rgba(255, 255, 255, 0.85)', fill: 'rgb(94, 94, 237)',
      stroke: 'none', opacity: 1, fill_opacity: 1, stroke_opacity: 1,
      stroke_width: 1.4, stroke_dasharray: 'none' },
    box: { left: 2, top: 6, width: 2, height: 4 },
  }]);
});

test('fails closed without the complete computed-style contract', () => {
  assert.deepEqual(buildMaterializedPaintBindings({}), []);
  assert.deepEqual(buildMaterializedPaintBindings({
    strings: [], computedStyleNames: ['fill'], documents: [{
      nodes: {}, layout: { nodeIndex: [], styles: [] },
    }],
  }), []);
});
