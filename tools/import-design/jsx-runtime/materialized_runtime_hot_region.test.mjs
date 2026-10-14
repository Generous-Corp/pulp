import assert from 'node:assert/strict';
import test from 'node:test';

import { evaluateEntry } from './materialized_runtime_test_helpers.mjs';

// ── Static/dynamic hot-region setter accounting ─────────────────────────
//
// A scoped commit is useful only when it reduces typed bridge traffic while
// preserving every binding in the changed subtree. This fixture keeps a cold
// (static) region beside a dynamic hot region and counts the public typed
// setters by channel. The counts are deterministic operation evidence rather
// than a wall-clock claim, and each count is paired with the ids that were
// actually written so a skip cannot masquerade as an optimization. The full
// fixture emits 57 typed setter calls; the hot scope emits 26 (26/57).
const HOT_REGION_SETTER_GROUPS = Object.freeze({
  layout: ['setPosition', 'setLeft', 'setTop', 'setFlex'],
  text: ['setFontFamily', 'setFontSize', 'setFontWeight', 'setFontStyle',
    'setLetterSpacing', 'setCapturedLineBoxes'],
  paint: ['setOpacity', 'setTextColor', 'setSvgFill', 'setSvgStroke',
    'setSvgStrokeWidth'],
});

function typedSetterCounter() {
  const groupBySetter = new Map(Object.entries(HOT_REGION_SETTER_GROUPS)
    .flatMap(([group, setters]) => setters.map(setter => [setter, group])));
  const calls = [];
  const bridge = {};
  for (const [setter, group] of groupBySetter) {
    bridge[setter] = (id, ...args) => calls.push({
      group, setter, id: String(id), args,
    });
  }
  return {
    bridge,
    calls,
    counts() {
      return Object.fromEntries(Object.keys(HOT_REGION_SETTER_GROUPS).map(
        group => [group, calls.filter(call => call.group === group).length]));
    },
    ids(group) {
      return [...new Set(calls.filter(call => call.group === group)
        .map(call => call.id))].sort();
    },
  };
}

function hotRegionDocument() {
  const make = (tag, id, parent, textContent = '') => {
    const node = { tagName: tag.toUpperCase(), __pulpId: id, id,
      parentElement: parent, _children: [], textContent,
      className: '', getAttribute: () => null };
    if (parent) parent._children.push(node);
    return node;
  };
  const root = make('div', 'root', null);
  const staticRegion = make('div', 'static-region', root);
  const hotRegion = make('div', 'hot-region', root);
  const staticLabel = make('span', 'static-label', staticRegion, 'STATIC');
  const staticPaint = make('path', 'static-paint', staticRegion);
  const hotLabel = make('span', 'hot-label', hotRegion, 'HOT');
  const hotPaint = make('path', 'hot-paint', hotRegion);
  const nodes = [root, staticRegion, hotRegion, staticLabel, staticPaint,
    hotLabel, hotPaint];
  const path = (...steps) => [{ index: 0, tag: 'div' }, ...steps];
  const box = seed => ({ left: seed, top: seed, width: 10, height: 10 });
  const layoutBindings = [
    { index: 0, tag: 'div', path: path(), box: box(0) },
    { index: 1, tag: 'div', path: path({ index: 0, tag: 'div' }), box: box(1) },
    { index: 2, tag: 'span', path: path(
      { index: 0, tag: 'div' }, { index: 0, tag: 'span' }), box: box(2) },
    { index: 3, tag: 'path', path: path(
      { index: 0, tag: 'div' }, { index: 1, tag: 'path' }), box: box(3) },
    { index: 4, tag: 'div', path: path({ index: 1, tag: 'div' }), box: box(4) },
    { index: 5, tag: 'span', path: path(
      { index: 1, tag: 'div' }, { index: 0, tag: 'span' }), box: box(5) },
    { index: 6, tag: 'path', path: path(
      { index: 1, tag: 'div' }, { index: 1, tag: 'path' }), box: box(6) },
  ];
  const basis = width => ({ width, resolved_face: 'Inter', requested: {
    font_family: 'Inter', font_size: 12, font_weight: 400,
    font_slant: 0, letter_spacing: 0,
  } });
  const textBindings = [
    { text: 'STATIC', path: path(
      { index: 0, tag: 'div' }, { index: 0, tag: 'span' }),
      boxes: [{ left: 0, top: 0, width: 10, height: 10 }], basis: basis(10) },
    { text: 'HOT', path: path(
      { index: 1, tag: 'div' }, { index: 0, tag: 'span' }),
      boxes: [{ left: 0, top: 0, width: 10, height: 10 }], basis: basis(10) },
  ];
  const paint = (index, pathSteps) => ({ index, tag: 'path', path: path(...pathSteps),
    paint: { opacity: 1, color: `rgb(${index}, ${index}, ${index})`,
      fill: `rgb(${index + 1}, ${index + 1}, ${index + 1})`, stroke: 'none',
      stroke_width: 0, stroke_dasharray: 'none' },
  });
  const paintBindings = [
    paint(0, [{ index: 0, tag: 'div' }, { index: 1, tag: 'path' }]),
    paint(1, [{ index: 1, tag: 'div' }, { index: 1, tag: 'path' }]),
  ];
  return {
    nodes, layoutBindings, textBindings, paintBindings,
    staticIds: ['root', 'static-region', 'static-label', 'static-paint'],
    hotIds: ['hot-region', 'hot-label', 'hot-paint'],
  };
}

function assertHotRegionEvidence(result, document) {
  assert.deepEqual(result.counter.ids('layout'), [...document.hotIds].sort(),
    'scoped layout writes escaped the hot region');
  assert.deepEqual(result.counter.ids('text'), ['hot-label'],
    'scoped text writes escaped the hot region');
  assert.deepEqual(result.counter.ids('paint'), ['hot-paint'],
    'scoped paint writes escaped the hot region');
  assert.deepEqual(result.counter.counts(), { layout: 15, text: 6, paint: 5 });
  assert.equal(result.total, 26);
  assert.equal(result.total / result.fullTotal, 26 / 57,
    'hot-region setter ratio changed; review the operation contract');
}

function plainRuntimeValue(value) {
  return JSON.parse(JSON.stringify(value));
}

function applyHotRegionPass(document, scope) {
  const counter = typedSetterCounter();
  const sandbox = evaluateEntry({ registryNodes: document.nodes,
    layoutBindings: document.layoutBindings, textBindings: document.textBindings,
    paintBindings: document.paintBindings, bridge: counter.bridge });
  const fullCounter = typedSetterCounter();
  Object.assign(sandbox, fullCounter.bridge);
  const fullApplied = sandbox.__pulpApplyMaterializedImportMetadata__();
  const fullTotal = fullCounter.calls.length;
  const fullDiagnostics = sandbox.__pulpMaterializedMetadataDiagnostics__;
  Object.assign(sandbox, counter.bridge);
  counter.calls.length = 0;
  const applied = sandbox.__pulpApplyMaterializedImportMetadata__(scope);
  return {
    counter, fullCounter, applied, total: counter.calls.length, fullTotal,
    fullApplied, fullDiagnostics,
    scopedDiagnostics: sandbox.__pulpMaterializedScopedApplyDiagnostics__,
    sandbox,
  };
}

test('typed setter counters measure full versus dynamic hot-region work', () => {
  const document = hotRegionDocument();
  const result = applyHotRegionPass(document, ['hot-region']);

  // Full pass control: every binding is still applied before the scoped pass.
  assert.equal(result.fullApplied,
    document.layoutBindings.length + document.textBindings.length +
      document.paintBindings.length);
  assert.deepEqual(result.fullCounter.counts(), {
    layout: 35, text: 12, paint: 10,
  });
  assert.deepEqual(result.fullCounter.ids('layout'), [
    ...document.staticIds, ...document.hotIds,
  ].sort());
  assert.deepEqual(result.fullCounter.ids('text'),
    ['hot-label', 'static-label']);
  assert.deepEqual(result.fullCounter.ids('paint'),
    ['hot-paint', 'static-paint']);
  assert.deepEqual(plainRuntimeValue(result.fullDiagnostics), {
    state_id: '', layout_expected: 7, layout_applied: 7,
    layout_node_miss: 0, layout_out_of_scope: 0, layout_dynamic_nodes: 0,
    text_expected: 2, text_applied: 2, text_node_miss: 0,
    text_content_mismatch: 0, text_target_miss: 0, text_out_of_scope: 0,
    text_optional_expected: 0, text_optional_applied: 0,
    text_optional_miss: 0, paint_expected: 2, paint_applied: 2,
    paint_node_miss: 0, paint_out_of_scope: 0, paint_unsupported: 0,
    paint_nodes: [{ index: 0, tag: 'path', id: 'static-paint' },
      { index: 1, tag: 'path', id: 'hot-paint' }],
  });
  assert.equal(result.fullTotal, 57);
  assertHotRegionEvidence(result, document);
  assert.equal(result.applied, 5,
    'scoped pass must apply hot layout, text, and paint bindings');
  assert.deepEqual(plainRuntimeValue(result.scopedDiagnostics), {
    state_id: '', layout_expected: 7, layout_applied: 3,
    layout_node_miss: 0, layout_out_of_scope: 4, layout_dynamic_nodes: 0,
    text_expected: 2, text_applied: 1, text_node_miss: 0,
    text_content_mismatch: 0, text_target_miss: 0, text_out_of_scope: 1,
    text_optional_expected: 0, text_optional_applied: 0,
    text_optional_miss: 0, paint_expected: 2, paint_applied: 1,
    paint_node_miss: 0, paint_out_of_scope: 1, paint_unsupported: 0,
    paint_nodes: [{ index: 1, tag: 'path', id: 'hot-paint' }], scoped: true,
    scope_size: 1,
  });
});

test('paint-only reconciliation writes only changed owned channels', () => {
  const document = hotRegionDocument();
  const result = applyHotRegionPass(document, ['hot-region']);
  result.counter.calls.length = 0;
  const reconciled = result.sandbox.__pulpReconcileMaterializedPaint__(
    'hot-paint', {
      fill: '#fff',
      color: 'rgb(1, 1, 1)',
      textColor: 'rgb(1, 1, 1)',
      opacity: 1,
    });
  assert.equal(reconciled, 1);
  assert.deepEqual(result.counter.calls, [{ group: 'paint', setter: 'setSvgFill',
    id: 'hot-paint', args: ['rgb(2, 2, 2)'] }]);
  assert.equal(result.sandbox.__pulpReconcileMaterializedPaint__(
    'not-captured', { fill: '#fff' }), 0,
  'unowned nodes must stay on authored React channels');
});

test('counter assertions reject planted hot-region leakage and dropped writes', () => {
  const document = hotRegionDocument();
  const result = applyHotRegionPass(document, ['hot-region']);
  const leaked = {
    ...result,
    counter: { ...result.counter,
      ids: group => group === 'layout'
        ? [...result.counter.ids(group), 'static-region'].sort()
        : result.counter.ids(group),
    },
  };
  assert.throws(() => assertHotRegionEvidence(leaked, document),
    /scoped layout writes escaped/,
  'negative control did not detect static-region leakage');

  const dropped = {
    ...result,
    counter: { ...result.counter,
      counts: () => ({ layout: 11, text: 6, paint: 5 }),
    },
  };
  assert.throws(() => assertHotRegionEvidence(dropped, document),
    /23|layout/,
  'negative control did not detect a dropped typed setter');
});
