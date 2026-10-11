// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import { buildMaterializedPseudoBindings } from './materialized_pseudo_bindings.mjs';

const styleNames = ['opacity', 'background-image', 'background-color', 'content',
  'z-index', 'pointer-events'];

test('captures a body before gradient as an absolute pseudo plane', () => {
  const strings = ['#document', 'HTML', 'BODY', '::before', 'radial-gradient(circle, rgba(0, 255, 255, 0.2), transparent)', 'transparent', 'none', 'auto', '1'];
  const style = (...values) => values.map(value => strings.indexOf(value));
  const snapshot = { strings, computedStyleNames: styleNames, documents: [{
    nodes: { parentIndex: [-1, 0, 1, 2], nodeType: [9, 1, 1, 1],
      nodeName: [0, 1, 2, 3], attributes: [[], [], [], []] },
    layout: { nodeIndex: [3], bounds: [[0, 0, 320, 240]],
      styles: [style('1', 'radial-gradient(circle, rgba(0, 255, 255, 0.2), transparent)', 'transparent', 'none', 'auto', 'none')] },
  }] };
  assert.deepEqual(buildMaterializedPseudoBindings(snapshot), {
    bindings: [{ index: 0, anchor: 'body', path: [], pseudo: 'before',
      box: { left: 0, top: 0, width: 320, height: 240 },
      background_gradient: 'radial-gradient(circle, rgba(0, 255, 255, 0.2), transparent)',
      opacity: 1, z_index: 0, pointer_events: 'none' }], diagnostics: [],
  });
});

test('fails closed for generated text and url image pseudo paint', () => {
  const strings = ['#document', 'HTML', 'BODY', 'DIV', '::after', '"x"', 'url(foo.png)', 'transparent', '1', 'auto', 'none'];
  const style = (...values) => values.map(value => strings.indexOf(value));
  const snapshot = { strings, computedStyleNames: styleNames, documents: [{
    nodes: { parentIndex: [-1, 0, 1, 2, 2], nodeType: [9, 1, 1, 1, 1],
      nodeName: [0, 1, 2, 3, 4], attributes: [[], [], [], [], []] },
    layout: { nodeIndex: [4], bounds: [[0, 0, 10, 10]],
      styles: [style('1', 'url(foo.png)', 'transparent', '"x"', 'auto', 'none')] },
  }] };
  const result = buildMaterializedPseudoBindings(snapshot);
  assert.deepEqual(result.bindings, []);
  assert.deepEqual(result.diagnostics.map(item => item.reason), [
    'generated-content-unsupported', 'background-image-unsupported', 'no-supported-paint',
  ]);
});

test('maps a pseudo hosted directly by the authored #root to an empty root path', () => {
  const strings = ['#document', 'HTML', 'BODY', 'DIV', '::before', 'root',
    'id', 'linear-gradient(#fff, transparent)', 'transparent', 'none', 'auto', '1'];
  const style = (...values) => values.map(value => strings.indexOf(value));
  const snapshot = { strings, computedStyleNames: styleNames, documents: [{
    nodes: { parentIndex: [-1, 0, 1, 2, 3], nodeType: [9, 1, 1, 1, 1],
      nodeName: [0, 1, 2, 3, 4], attributes: [[], [], [], [6, 5], []] },
    layout: { nodeIndex: [4], bounds: [[1, 2, 40, 20]],
      styles: [style('1', 'linear-gradient(#fff, transparent)', 'transparent', 'none', 'auto', 'none')] },
  }] };
  const result = buildMaterializedPseudoBindings(snapshot);
  assert.equal(result.diagnostics.length, 0);
  assert.equal(result.bindings[0].anchor, '#root');
  assert.deepEqual(result.bindings[0].path, []);
});
