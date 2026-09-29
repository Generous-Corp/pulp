import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';

import { buildMaterializedRuntimeEntry } from './materialized_runtime_entry.mjs';

// The generated entry reapplies captured metadata from `resetAfterCommit`, so
// every cost inside one application is multiplied by the React commit rate.
// These cases pin the two costs that are shared across an application — the
// registry path index and a selector's parse — to work that scales with the
// registry snapshot rather than with the number of bindings or nodes scanned.
// Counting operations keeps the assertions deterministic; a wall-clock budget
// would flake on a shared runner and could not say which cost regressed.

// Native bridge functions the entry calls while mounting its behavior root.
const BRIDGE_STUBS = ['createCol', 'setPosition', 'setLeft', 'setTop', 'setFlex',
  'setVisible', 'setOpacity', 'setPointerEvents', 'setZIndex',
  'setTransformOrigin', 'setTransform'];

// The entry is an ES module whose only imports are React and the native
// renderer. Replacing those two lines with stubs lets the rest of the module
// execute verbatim in a sandbox, so these cases exercise the shipped source
// rather than a re-implementation of it. Evaluating it (instead of parsing it)
// is deliberate: a declaration placed in the wrong function body still parses.
function evaluateEntry({ layoutBindings = [], registryNodes = [],
                         textBindings = [], paintBindings = [], stateAtlas = [],
                         bridge = {} }) {
  const source = buildMaterializedRuntimeEntry({
    capturedCssVariables: {}, presentationTime: 0, requestedState: '',
    textBindings, layoutBindings, paintBindings,
    runtimeDocumentAsset: null, sidecar: null, productPrelude: '',
    surfaceBackground: null, authoredLeft: 0, authoredTop: 0,
    authoredWidth: 100, authoredHeight: 100, authoredTransform: null,
    visualAuthority: null, stateAtlas, visualWidth: 100, visualHeight: 100,
    canvasBindings: [], behaviorCanvasAnchors: [],
    capturedPaintAuthorityAnchors: [],
  }).replace(/^import .*$/gm, '');

  const sandbox = {
    App: () => null,
    __pulpRuntimeImport__: () => {},
    __pulpReactDomRegistry__: { values: () => registryNodes.values() },
    getLayoutBoxMetrics: () => null,
  };
  for (const name of BRIDGE_STUBS) sandbox[name] = () => {};
  Object.assign(sandbox, bridge);
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(
    'const React = { createElement: () => null };' +
    'const createPulpRoot = () => ({});' +
    'const renderPulp = () => {}; const unmountPulp = () => {};' + source,
    sandbox, { filename: 'materialized-entry.js' });

  // A module that threw before publishing its entry points would leave every
  // counter at zero, which reads exactly like a perfect score.
  assert.equal(typeof sandbox.__pulpApplyMaterializedImportMetadata__,
    'function', 'entry did not evaluate far enough to publish the applier');
  return sandbox;
}

// Roots are found by reading each registry node's parent, so one read per node
// is one index build. Counting through a getter attributes the reads to the
// index rather than to a timer.
function countingRegistry(count, onRead) {
  const nodes = [];
  for (let index = 0; index < count; ++index) {
    const node = {
      tagName: 'DIV', __pulpId: `n${index}`, id: `n${index}`, _children: [],
    };
    Object.defineProperty(node, 'parentElement', {
      get() { onRead(); return null; }, configurable: true,
    });
    nodes.push(node);
  }
  return nodes;
}

function flatLayoutBindings(count, registrySize) {
  const bindings = [];
  for (let index = 0; index < count; ++index) {
    bindings.push({
      index, tag: 'div', path: [{ index: index % registrySize, tag: 'div' }],
      box: { width: 10, height: 10, left: 0, top: 0 },
    });
  }
  return bindings;
}

// One application reads each registry node's parent once for the shared index,
// plus once per resolved binding when translating that node's coordinate space.
function applyAndCountParentReads(registrySize, bindingCount) {
  let reads = 0;
  const registryNodes = countingRegistry(registrySize, () => { ++reads; });
  const sandbox = evaluateEntry({
    registryNodes,
    layoutBindings: flatLayoutBindings(bindingCount, registrySize),
  });
  reads = 0;
  const applied = sandbox.__pulpApplyMaterializedImportMetadata__();
  assert.equal(applied, bindingCount, 'fixture did not resolve every binding');
  return reads;
}

test('builds the registry path index once per application, not per binding',
  () => {
    const registrySize = 40;
    const bindingCount = 25;
    const reads = applyAndCountParentReads(registrySize, bindingCount);

    // Rebuilding the index per binding costs registrySize reads each time.
    const perBindingRebuild = registrySize * bindingCount + bindingCount;
    assert.equal(reads, registrySize + bindingCount);
    assert.ok(reads < perBindingRebuild / 4,
      `expected a shared index, saw ${reads} of ${perBindingRebuild} reads`);
  });

test('index cost stays additive as the binding count grows', () => {
  const registrySize = 40;
  const base = applyAndCountParentReads(registrySize, 25);
  const doubled = applyAndCountParentReads(registrySize, 50);

  // A shared index adds one read per extra binding. Rebuilding per binding
  // would add registrySize reads for each of them.
  assert.equal(doubled - base, 25);
  assert.ok(doubled - base < registrySize,
    'per-binding growth scales with the registry, so the index is rebuilt');
});

test('resolves nested captured paths through the shared index', () => {
  // Perf counters alone would stay green if resolution broke, so pin the
  // traversal the index feeds: roots, then registry-filtered children.
  const parent = { tagName: 'DIV', __pulpId: 'p', id: 'p', parentElement: null };
  const first = { tagName: 'DIV', __pulpId: 'c0', id: 'c0', _children: [] };
  const second = { tagName: 'SPAN', __pulpId: 'c1', id: 'c1', _children: [] };
  first.parentElement = parent;
  second.parentElement = parent;
  parent._children = [first, second];

  const sandbox = evaluateEntry({
    registryNodes: [parent, first, second],
    layoutBindings: [{
      index: 0, tag: 'span',
      path: [{ index: 0, tag: 'div' }, { index: 1, tag: 'span' }],
      box: { width: 10, height: 10, left: 0, top: 0 },
    }],
  });

  const applied = sandbox.__pulpApplyMaterializedImportMetadata__();
  assert.equal(applied, 1, 'nested path did not resolve through the index');
});

test('parses a selector once per scan rather than once per candidate node', () => {
  // Every registry scan tests one selector against every node. Counting the
  // parse's own regex passes shows whether that work follows the selector
  // vocabulary or the registry size.
  const scan = (nodeCount) => {
    const registryNodes = [];
    for (let index = 0; index < nodeCount; ++index) {
      const last = index === nodeCount - 1;
      registryNodes.push({
        tagName: 'DIV', __pulpId: `n${index}`, id: `n${index}`, _children: [],
        className: '', parentElement: null,
        getAttribute: (name) => (name === 'data-x' && last ? '1' : null),
      });
    }
    const sandbox = evaluateEntry({ registryNodes });
    const sandboxString = vm.runInContext('String', sandbox);
    const original = sandboxString.prototype.replace;
    let passes = 0;
    sandboxString.prototype.replace = function (...args) {
      ++passes;
      return original.apply(this, args);
    };
    try {
      const found = sandbox.__pulpFindMaterializedElement__('div[data-x="1"]');
      assert.equal(found && found.__pulpId, `n${nodeCount - 1}`,
        'selector scan did not find the matching node');
    } finally {
      sandboxString.prototype.replace = original;
    }
    return passes;
  };

  const small = scan(20);
  const large = scan(200);
  assert.ok(small > 0, 'no parse observed, so the counter is not wired');
  assert.equal(large, small,
    `selector re-parsed per node: ${small} passes at 20 nodes, ${large} at 200`);
});

// Captured-state resolution runs one selector per state on every React commit
// and takes the first that answers, so every state ahead of the live one costs
// a scan that match-tests the whole registry before returning null. These cases
// pin that a miss is paid once per mutation epoch rather than once per commit,
// and that a runtime with no epoch published declines to cache at all.
function scanCountingRegistry(count, onTest) {
  const nodes = [];
  for (let index = 0; index < count; ++index) {
    const node = {
      __pulpId: `n${index}`, id: `n${index}`, _children: [],
      className: '', parentElement: null,
      getAttribute: () => null,
    };
    Object.defineProperty(node, 'tagName', {
      get() { onTest(); return 'DIV'; }, configurable: true,
    });
    nodes.push(node);
  }
  return nodes;
}

test('a registry miss is rescanned once per mutation epoch, not per commit',
  () => {
    let tests = 0;
    const registryNodes = scanCountingRegistry(50, () => { ++tests; });
    const sandbox = evaluateEntry({ registryNodes });
    sandbox.__pulpMaterializedTreeEpoch__ = 1;

    tests = 0;
    assert.equal(sandbox.__pulpFindMaterializedElement__('span.absent'), null);
    const firstScan = tests;
    assert.ok(firstScan > 0, 'no match test observed, so the counter is dead');

    // Ten further commits with no host mutation: the epoch is unchanged, so the
    // answer cannot have changed and nothing should be rescanned.
    for (let commit = 0; commit < 10; ++commit) {
      assert.equal(sandbox.__pulpFindMaterializedElement__('span.absent'), null);
    }
    assert.equal(tests, firstScan,
      `miss rescanned without a mutation: ${tests} tests vs ${firstScan}`);

    // A host mutation bumps the epoch, and the answer must be recomputed --
    // a cache that never invalidates is a stale answer, not a cheap one.
    sandbox.__pulpMaterializedTreeEpoch__ = 2;
    assert.equal(sandbox.__pulpFindMaterializedElement__('span.absent'), null);
    assert.equal(tests, firstScan * 2,
      'a bumped epoch did not invalidate the retained miss');
  });

test('declines to cache a miss when no mutation epoch is published', () => {
  // A plain importer runtime with no @pulp/react host has nothing to invalidate
  // a retained miss, so it must keep scanning rather than answer from a cache
  // it can never clear.
  let tests = 0;
  const registryNodes = scanCountingRegistry(20, () => { ++tests; });
  const sandbox = evaluateEntry({ registryNodes });
  assert.equal(sandbox.__pulpMaterializedTreeEpoch__, undefined,
    'fixture published an epoch, so this case proves nothing');

  tests = 0;
  assert.equal(sandbox.__pulpFindMaterializedElement__('span.absent'), null);
  const firstScan = tests;
  assert.ok(firstScan > 0, 'no match test observed, so the counter is dead');
  assert.equal(sandbox.__pulpFindMaterializedElement__('span.absent'), null);
  assert.equal(tests, firstScan * 2,
    'a miss was cached with no epoch to invalidate it');
});

test('a positive match is not served from the miss cache', () => {
  // Only negative answers are retained. A node that once matched can have its
  // attributes rewritten, so the hit path must stay live.
  const node = {
    tagName: 'SPAN', __pulpId: 'hit', id: 'hit', _children: [],
    className: 'live', parentElement: null, getAttribute: () => null,
  };
  const sandbox = evaluateEntry({ registryNodes: [node] });
  sandbox.__pulpMaterializedTreeEpoch__ = 1;

  assert.equal(sandbox.__pulpFindMaterializedElement__('span.live'), node);
  node.className = 'changed';
  assert.equal(sandbox.__pulpFindMaterializedElement__('span.live'), null,
    'a stale hit was served after the node stopped matching');
});

// ── Scoped re-apply ────────────────────────────────────────────────
//
// The applier is called once per qualifying React commit, so its cost is
// multiplied by the commit rate; a drag emits hundreds. The host config now
// hands it the ids whose subtrees the commit actually touched, and a binding
// outside that scope keeps the geometry it already has.
//
// Two directions have to hold, and only one of them is a perf number. A scope
// that skips too MUCH renders visibly wrong -- captured nodes keep stale
// boxes -- and no operation count would notice, so every case below pairs the
// saving with an explicit claim about what still got applied.

// A small captured document with two sibling subtrees. Bindings cover every
// node, so "the scope worked" and "the scope dropped work it owed" are both
// observable from which ids received bridge calls.
function twoSubtreeDocument() {
  const make = (id, tag = 'div') => ({
    tagName: tag.toUpperCase(), __pulpId: id, id, _children: [],
    parentElement: null, className: '', getAttribute: () => null,
  });
  const root = make('root');
  const a = make('a');
  const b = make('b');
  const a0 = make('a0');
  const a1 = make('a1');
  const b0 = make('b0');
  const b1 = make('b1');
  const link = (parent, children) => {
    parent._children = children;
    for (const child of children) child.parentElement = parent;
  };
  link(root, [a, b]);
  link(a, [a0, a1]);
  link(b, [b0, b1]);
  const nodes = [root, a, b, a0, a1, b0, b1];
  const step = { index: 0, tag: 'div' };
  const path = (...indices) =>
    [step, ...indices.map((index) => ({ index, tag: 'div' }))];
  const layoutBindings = [
    { index: 0, tag: 'div', path: path(), box: box(0) },
    { index: 1, tag: 'div', path: path(0), box: box(1) },
    { index: 2, tag: 'div', path: path(1), box: box(2) },
    { index: 3, tag: 'div', path: path(0, 0), box: box(3) },
    { index: 4, tag: 'div', path: path(0, 1), box: box(4) },
    { index: 5, tag: 'div', path: path(1, 0), box: box(5) },
    { index: 6, tag: 'div', path: path(1, 1), box: box(6) },
  ];
  // Binding order matches `nodes` order, which is what the id assertions read.
  const expectedIds = ['root', 'a', 'b', 'a0', 'a1', 'b0', 'b1'];
  return { nodes, layoutBindings, expectedIds };
}

function box(seed) {
  return { width: 10 + seed, height: 10 + seed, left: seed, top: seed };
}

// Records which ids the applier actually wrote geometry to. An operation count
// alone cannot distinguish "skipped the right nodes" from "skipped the wrong
// ones", and the wrong one is invisible in every green perf assertion.
function applyWithRecorder(sandbox, scope) {
  const writes = [];
  let metricReads = 0;
  for (const name of ['setPosition', 'setLeft', 'setTop']) {
    sandbox[name] = (id) => { writes.push(`${name}:${id}`); };
  }
  sandbox.setFlex = (id, axis) => { writes.push(`setFlex.${axis}:${id}`); };
  sandbox.getLayoutBoxMetrics = () => { ++metricReads; return null; };
  // The applier returns before publishing diagnostics when the text bridge is
  // absent, so a document with no text bindings still needs the stub present.
  sandbox.setCapturedLineBoxes = () => {};
  const applied = scope === undefined
    ? sandbox.__pulpApplyMaterializedImportMetadata__()
    : sandbox.__pulpApplyMaterializedImportMetadata__(scope);
  const ids = new Set(writes.map((write) => write.split(':')[1]));
  return { applied, writes, ids, metricReads };
}

test('an unscoped application still writes every captured binding', () => {
  // The control for every scoped case below. If this ever reads short, the
  // fixture is broken and the savings measured against it mean nothing.
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });
  const full = applyWithRecorder(sandbox, undefined);

  assert.equal(full.applied, doc.layoutBindings.length);
  assert.deepEqual([...full.ids].sort(), [...doc.expectedIds].sort());
});

test('a scoped application skips bindings outside the changed subtree', () => {
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });
  const full = applyWithRecorder(sandbox, undefined);
  const scoped = applyWithRecorder(sandbox, ['a']);

  // The saving. Every write and every forced layout read the applier avoids is
  // paid once per qualifying commit in production.
  assert.ok(scoped.writes.length < full.writes.length,
    `scope saved nothing: ${scoped.writes.length} of ${full.writes.length}`);
  assert.ok(scoped.metricReads < full.metricReads,
    'scope did not reduce forced layout metric reads');

  // The correctness half: a scope naming `a` owes `a` AND its descendants, and
  // owes nothing for `b`'s subtree or the untouched root.
  assert.deepEqual([...scoped.ids].sort(), ['a', 'a0', 'a1']);
  assert.equal(scoped.applied, 3);
});

test('a scope naming a parent still re-applies its whole subtree', () => {
  // The failure this guards is silent and visual: a scope that resolved only
  // the named node would leave freshly reordered children with stale captured
  // boxes, and every operation count would look better for it.
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });
  const scoped = applyWithRecorder(sandbox, ['root']);

  assert.deepEqual([...scoped.ids].sort(), [...doc.expectedIds].sort());
  assert.equal(scoped.applied, doc.layoutBindings.length);
});

test('several scoped ids from one commit are applied together', () => {
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });
  const scoped = applyWithRecorder(sandbox, ['a1', 'b0']);

  assert.deepEqual([...scoped.ids].sort(), ['a1', 'b0']);
});

test('an absent or empty scope falls back to applying everything', () => {
  // A runtime that predates the scope argument, and a commit whose blast
  // radius the host config could not name, both arrive here as nothing. The
  // fallback has to be the slow-but-correct direction.
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });

  for (const scope of [null, [], undefined]) {
    const result = applyWithRecorder(sandbox, scope);
    assert.equal(result.applied, doc.layoutBindings.length,
      `scope ${JSON.stringify(scope)} did not fall back to a full pass`);
  }
});

test('a scope naming an id that is not in the registry applies nothing', () => {
  // Deliberately pinned rather than left undefined. An unknown id must not
  // degrade to "apply everything" -- that would hide a host-config regression
  // that stopped publishing real ids behind a permanently full re-apply.
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });
  const scoped = applyWithRecorder(sandbox, ['not-a-node']);

  assert.equal(scoped.applied, 0);
  assert.equal(scoped.writes.length, 0);
});

test('a scoped pass does not overwrite the full-application diagnostics', () => {
  // Import validators read `__pulpMaterializedMetadataDiagnostics__` and
  // compare applied against expected. A scoped pass legitimately applies a
  // fraction of the document, so publishing its counts under that key would
  // report a mass miss on a pass that was correct by construction.
  const doc = twoSubtreeDocument();
  const sandbox = evaluateEntry({
    registryNodes: doc.nodes, layoutBindings: doc.layoutBindings,
  });
  applyWithRecorder(sandbox, undefined);
  const full = sandbox.__pulpMaterializedMetadataDiagnostics__;
  assert.equal(full.layout_applied, doc.layoutBindings.length,
    'full diagnostics were not published, so this case proves nothing');

  applyWithRecorder(sandbox, ['a']);
  assert.equal(sandbox.__pulpMaterializedMetadataDiagnostics__.layout_applied,
    doc.layoutBindings.length, 'a scoped pass clobbered the full diagnostics');
  const scoped = sandbox.__pulpMaterializedScopedApplyDiagnostics__;
  assert.equal(scoped.scoped, true);
  assert.equal(scoped.layout_applied, 3);
  assert.equal(scoped.layout_out_of_scope, 4);
});

// ── Per-pass path memo ──────────────────────────────────────────────────
// Captured paths share prefixes: every control in a row walks through the
// row. Counting reads of `_children` measures how often the pass filters a
// node's children against the registry.
function rowDocument(rows, onChildrenRead) {
  const make = (tag, id, parent) => {
    const kids = [];
    const node = { tagName: tag.toUpperCase(), __pulpId: id, id,
      parentElement: parent, textContent: '' };
    Object.defineProperty(node, '_children', {
      get() { onChildrenRead(); return kids; },
    });
    if (parent) parent.__kids.push(node);
    Object.defineProperty(node, '__kids', { value: kids });
    return node;
  };
  const root = make('div', 'root', null);
  const nodes = [root];
  const layout = [{ index: 0, tag: 'div', path: [{ index: 0, tag: 'div' }],
    box: { left: 0, top: 0, width: 10, height: 10 } }];
  for (let r = 0; r < rows; ++r) {
    const row = make('div', `row${r}`, root);
    nodes.push(row);
    for (let c = 0; c < 4; ++c) {
      nodes.push(make('span', `cell${r}-${c}`, row));
      layout.push({ index: layout.length, tag: 'span',
        path: [{ index: 0, tag: 'div' }, { index: r, tag: 'div' },
          { index: c, tag: 'span' }],
        box: { left: 0, top: 0, width: 10, height: 10 } });
    }
  }
  return { nodes, layout };
}

test('a pass filters each node\'s children once, however many paths cross it',
  () => {
    let reads = 0;
    const rows = 10;
    const { nodes, layout } = rowDocument(rows, () => { ++reads; });
    const sandbox = evaluateEntry({ registryNodes: nodes, layoutBindings: layout });
    reads = 0;
    const applied = sandbox.__pulpApplyMaterializedImportMetadata__();
    // CONTROL: resolution still works, or zero reads would mean nothing ran.
    assert.equal(applied, layout.length);
    // root + 10 rows + 40 cells: each read once. Walking every path from the
    // root re-filters the root and a row for each of the 41 bindings (~123).
    assert.equal(reads, 1 + rows + rows * 4);
  });

test('the path memo lives for one pass, so a reparent resolves freshly', () => {
  const { nodes, layout } = rowDocument(2, () => {});
  const sandbox = evaluateEntry({ registryNodes: nodes, layoutBindings: layout });
  const positions = [];
  sandbox.setLeft = (id) => positions.push(id);
  sandbox.__pulpApplyMaterializedImportMetadata__();
  // Swap the two rows: paths now resolve to the other row's cells.
  const root = nodes[0];
  root.__kids.reverse();
  positions.length = 0;
  sandbox.__pulpApplyMaterializedImportMetadata__();
  assert.equal(positions[1], 'cell1-0');
  assert.equal(positions[5], 'cell0-0');
});

// ── Typography once per Label per pass ─────────────────────────────────
function textDocument(duplicates) {
  const root = { tagName: 'DIV', __pulpId: 'root', id: 'root', parentElement: null,
    textContent: '' };
  const label = { tagName: 'SPAN', __pulpId: 'l', id: 'l', parentElement: root,
    textContent: 'Theme', _children: [] };
  root._children = [label];
  const binding = { text: 'Theme', path: [{ index: 0, tag: 'div' },
    { index: 0, tag: 'span' }], boxes: [{ left: 0, top: 0, width: 5, height: 5 }],
    basis: { width: 5, resolved_face: 'Mono', requested: { font_family: 'Mono',
      font_size: 11, font_weight: 400, font_slant: 0, letter_spacing: 0 } } };
  return { nodes: [root, label],
    text: Array.from({ length: 1 + duplicates }, () => ({ ...binding })) };
}

test('applies a Label\'s face once per pass however many bindings reach it', () => {
  const { nodes, text } = textDocument(56);
  const fonts = [];
  const lineBoxes = [];
  const sandbox = evaluateEntry({ registryNodes: nodes, textBindings: text,
    bridge: {
      setCapturedLineBoxes: (id) => lineBoxes.push(id),
      setFontFamily: (id, face) => fonts.push([id, face]),
      setFontSize: () => {}, setFontWeight: () => {}, setFontStyle: () => {},
      setLetterSpacing: () => {},
    } });
  fonts.length = 0;
  lineBoxes.length = 0;
  sandbox.__pulpApplyMaterializedImportMetadata__();
  // CONTROL: every binding was still applied (57 line-box writes).
  assert.equal(lineBoxes.length, 57);
  assert.deepEqual(fonts, [['l', 'Mono']]);
  // A second pass applies it again: the dedup is per pass, not forever.
  sandbox.__pulpApplyMaterializedImportMetadata__();
  assert.equal(fonts.length, 2);
});

test('a different face for the same Label in one pass is still applied', () => {
  const { nodes, text } = textDocument(1);
  text[1] = { ...text[1], basis: { ...text[1].basis,
    requested: { ...text[1].basis.requested, font_size: 14 } } };
  const sizes = [];
  const sandbox = evaluateEntry({ registryNodes: nodes, textBindings: text,
    bridge: { setCapturedLineBoxes: () => {}, setFontFamily: () => {},
      setFontSize: (id, size) => sizes.push(size) } });
  sizes.length = 0;
  sandbox.__pulpApplyMaterializedImportMetadata__();
  assert.deepEqual(sizes, [11, 14]);
});

// ── Captured paint ownership and per-node reconcile ─────────────────────
function paintDocument() {
  const root = { tagName: 'DIV', __pulpId: 'root', id: 'root', parentElement: null };
  const icon = { tagName: 'PATH', __pulpId: 'icon', id: 'icon', parentElement: root,
    _children: [] };
  const plain = { tagName: 'PATH', __pulpId: 'plain', id: 'plain',
    parentElement: root, _children: [] };
  root._children = [icon, plain];
  const paint = [{ index: 0, tag: 'path', path: [{ index: 0, tag: 'div' },
    { index: 0, tag: 'path' }], paint: { opacity: 0.5, color: 'rgb(1, 1, 1)',
    fill: 'rgb(2, 2, 2)', stroke: 'none', stroke_width: 1,
    stroke_dasharray: 'none' } }];
  return { nodes: [root, icon, plain], paint };
}

function paintRecorder() {
  const writes = [];
  const bridge = {};
  for (const verb of ['setOpacity', 'setTextColor', 'setSvgFill', 'setSvgStroke',
    'setSvgStrokeWidth']) bridge[verb] = (id, value) => writes.push([verb, id, value]);
  return { writes, bridge };
}

test('reconciles only a captured channel whose React value differs', () => {
  const { nodes, paint } = paintDocument();
  const { writes, bridge } = paintRecorder();
  const sandbox = evaluateEntry({ registryNodes: nodes, paintBindings: paint, bridge });
  sandbox.__pulpApplyMaterializedImportMetadata__();
  writes.length = 0;
  const reconcile = sandbox.__pulpReconcileMaterializedPaint__;
  assert.equal(reconcile('icon', { fill: '#fff', color: 'rgb(1, 1, 1)' }), 1);
  assert.deepEqual(writes, [['setSvgFill', 'icon', 'rgb(2, 2, 2)']]);
  // textColor and color land on one channel: written once.
  writes.length = 0;
  assert.equal(reconcile('icon', { color: '#f00', textColor: '#f00' }), 1);
  // A node the capture does not own is left to React.
  assert.equal(reconcile('plain', { fill: '#fff', opacity: 0 }), 0);
  assert.equal(writes.length, 1);
});

test('a full pass forgets ownership the new metadata no longer carries', () => {
  const { nodes, paint } = paintDocument();
  const { writes, bridge } = paintRecorder();
  const state = { id: 'bare', match: { selector: '[data-bare]' },
    metadata: { layout_bindings: [], text_bindings: [], paint_bindings: [] } };
  const sandbox = evaluateEntry({ registryNodes: nodes, paintBindings: paint,
    stateAtlas: [state], bridge });
  sandbox.__pulpApplyMaterializedImportMetadata__();
  const reconcile = sandbox.__pulpReconcileMaterializedPaint__;
  assert.equal(reconcile('icon', { fill: '#fff' }), 1);
  nodes[1].getAttribute = (name) => name === 'data-bare' ? '' : null;
  assert.equal(sandbox.__pulpRefreshMaterializedState__(), 'bare');
  writes.length = 0;
  assert.equal(reconcile('icon', { fill: '#fff' }), 0);
  assert.deepEqual(writes, []);
});

test('publishes every attribute a captured-state selector names up front', () => {
  const states = ['[data-a="1"]', 'div[ data-b ] span', '#x.y'].map((selector, i) =>
    ({ id: `s${i}`, match: { selector, ancestor: i === 0 ? '[aria-expanded]' : '' },
      metadata: { layout_bindings: [], text_bindings: [], paint_bindings: [] } }));
  // The last state matches, so resolution never asks about the others: their
  // attributes are published only because they are recorded up front.
  const matched = { tagName: 'DIV', __pulpId: 'x', id: 'x', className: 'y',
    parentElement: null, _children: [], getAttribute: () => null };
  const sandbox = evaluateEntry({ stateAtlas: states, registryNodes: [matched] });
  assert.equal(sandbox.__pulpRefreshMaterializedState__(), 's2');
  assert.deepEqual([...sandbox.__pulpMaterializedSelectorAttributes__].sort(),
    ['aria-expanded', 'data-a', 'data-b']);
  sandbox.__pulpFindMaterializedElement__('[data-late]');
  assert.ok(sandbox.__pulpMaterializedSelectorAttributes__.has('data-late'));
});
