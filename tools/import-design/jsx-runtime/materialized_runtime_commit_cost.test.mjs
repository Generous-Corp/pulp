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
