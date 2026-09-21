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
function evaluateEntry({ layoutBindings = [], registryNodes = [] }) {
  const source = buildMaterializedRuntimeEntry({
    capturedCssVariables: {}, presentationTime: 0, requestedState: '',
    textBindings: [], layoutBindings, paintBindings: [],
    runtimeDocumentAsset: null, sidecar: null, productPrelude: '',
    surfaceBackground: null, authoredLeft: 0, authoredTop: 0,
    authoredWidth: 100, authoredHeight: 100, authoredTransform: null,
    visualAuthority: null, stateAtlas: [], visualWidth: 100, visualHeight: 100,
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
