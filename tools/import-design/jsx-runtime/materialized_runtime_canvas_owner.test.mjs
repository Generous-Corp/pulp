import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';

import { buildMaterializedRuntimeEntry } from './materialized_runtime_entry.mjs';

// A live canvas keeps its authored pointer handlers on the behavior tree while
// the DesignIR sibling owns what is painted, so the runtime has to decide which
// behavior node a visible canvas belongs to. That decision is a cascade —
// ancestor walk, then gesture-channel ownership, then a geometric containment
// tiebreak — and every clause is generated into the entry as a string, so the
// only way to exercise it is to evaluate the emitted realm with a NON-EMPTY
// binding table. A fixture that passes no bindings skips the whole cascade and
// leaves every counter at zero, which reads exactly like a passing run.

const BRIDGE_STUBS = ['createCol', 'setPosition', 'setLeft', 'setTop', 'setFlex',
  'setVisible', 'setOpacity', 'setPointerEvents', 'setZIndex',
  'setTransformOrigin', 'setTransform'];

// The entry is an ES module whose only imports are React and the native
// renderer. Replacing those two lines with stubs lets the rest of the module
// execute verbatim, so these cases exercise the shipped generator output rather
// than a re-implementation of its cascade.
function evaluateEntry({
  canvasBindings = [], behaviorCanvasAnchors = [], registryNodes = [],
  callbackKeys = [], layoutRects = null, bindResult = () => true,
}) {
  const source = buildMaterializedRuntimeEntry({
    capturedCssVariables: {}, presentationTime: 0, requestedState: '',
    textBindings: [], layoutBindings: [], paintBindings: [],
    runtimeDocumentAsset: null, sidecar: null, productPrelude: '',
    surfaceBackground: null, authoredLeft: 0, authoredTop: 0,
    authoredWidth: 100, authoredHeight: 100, authoredTransform: null,
    visualAuthority: null, stateAtlas: [], visualWidth: 100, visualHeight: 100,
    canvasBindings, behaviorCanvasAnchors,
    capturedPaintAuthorityAnchors: [],
  }).replace(/^import .*$/gm, '');

  const bindCalls = [];
  const sandbox = {
    App: () => null,
    __pulpRuntimeImport__: () => {},
    __pulpReactDomRegistry__: { values: () => registryNodes.values() },
    __pulpCallbackKeys__: callbackKeys,
    getLayoutBoxMetrics: () => null,
    bindCanvasBehaviorAt: (anchor, rootId, index, ownerId) => {
      bindCalls.push({ anchor, index, ownerId });
      return bindResult(index);
    },
  };
  if (layoutRects)
    sandbox.getLayoutRect = id => layoutRects[id] || null;
  for (const name of BRIDGE_STUBS) sandbox[name] = () => {};
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(
    // The entry gates its callback maps on `instanceof Map`, which is realm
    // sensitive, so the registry has to be constructed inside the sandbox.
    'globalThis.__pulpReactEventCallbacks__ = ' +
    'new Map(__pulpCallbackKeys__.map(key => [key, () => {}]));' +
    'const React = { createElement: () => null };' +
    'const createPulpRoot = () => ({});' +
    'const renderPulp = () => {}; const unmountPulp = () => {};' + source,
    sandbox, { filename: 'materialized-entry.js' });

  // A module that threw before publishing its entry points would leave every
  // assertion below unreached while still looking like a clean run.
  assert.equal(typeof sandbox.__pulpBindMaterializedCanvases__, 'function',
    'entry did not evaluate far enough to publish the canvas binder');
  return { sandbox, bindCalls };
}

function node(id, tagName, extra = {}) {
  return { tagName, __pulpId: id, id, _children: [], ...extra };
}

function canvasBinding(index, bounds = null) {
  return bounds ? { index, bounds } : { index };
}

// Resolve one canvas's owner through the shipped binder, which is the only
// caller that reaches the cascade.
function resolveOwner(fixture) {
  const { sandbox, bindCalls } = evaluateEntry(fixture);
  const bound = sandbox.__pulpBindMaterializedCanvases__();
  assert.equal(bound, fixture.canvasBindings.length,
    'binder did not walk every binding');
  assert.equal(bindCalls.length, fixture.canvasBindings.length,
    'binder never reached the native bind for every binding');
  return { owners: sandbox.__pulpMaterializedCanvasBehaviorOwners__, bindCalls };
}

test('an ancestor carrying pointer callbacks owns the canvas below it', () => {
  const wrapper = node('wrap0', 'DIV');
  const canvas = node('cv0', 'CANVAS', { _parentElement: wrapper });
  const { owners, bindCalls } = resolveOwner({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [wrapper, canvas],
    // A second unrelated handler owner is what makes this case about ancestry:
    // with only one registered owner the later single-owner clause returns the
    // same answer, so the walk could be broken and this would still pass.
    callbackKeys: ['wrap0:pointerdown', 'chrome:click'],
  });

  assert.equal(owners[0], 'wrap0');
  // The resolved owner must reach the native bind, not just the returned array.
  assert.deepEqual({ ...bindCalls[0] },
    { anchor: 'anchor0', index: 0, ownerId: 'wrap0' });
});

test('a click-only handler claims ownership when no ancestor has pointers', () => {
  const canvas = node('cv0', 'CANVAS');
  const { owners } = resolveOwner({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [canvas],
    // No key starts with an id + ':pointer', so the ancestor walk finds
    // nothing and the gesture-channel clause is what must answer.
    callbackKeys: ['btn:click'],
  });

  assert.equal(owners[0], 'btn');
});

test('keyboard and focus channels do not claim a canvas', () => {
  const canvas = node('cv0', 'CANVAS');
  const { owners } = resolveOwner({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [canvas],
    callbackKeys: ['fld:keydown', 'fld:focus'],
  });

  // A lone registered handler is not enough — it has to be on a channel a
  // canvas gesture actually arrives on, or the canvas has no behavior owner.
  assert.equal(owners[0], '');
});

test('a full gesture set breaks a tie against a bare click handler', () => {
  const canvas = node('cv0', 'CANVAS');
  const { owners } = resolveOwner({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [canvas],
    callbackKeys: ['menu:click',
      'pad:pointerdown', 'pad:pointermove', 'pad:pointerup',
      'pad:pointerleave', 'pad:wheel'],
  });

  assert.equal(owners[0], 'pad');
});

test('geometry picks the tightest handler box containing the canvas centre',
  () => {
    const canvas = node('cv0', 'CANVAS');
    const { owners } = resolveOwner({
      canvasBindings: [canvasBinding(0, { left: 10, top: 10, width: 20, height: 20 })],
      behaviorCanvasAnchors: ['anchor0'],
      registryNodes: [canvas],
      callbackKeys: ['panel:click', 'knob:click'],
      layoutRects: {
        panel: { left: 0, top: 0, width: 200, height: 200 },
        knob: { left: 5, top: 5, width: 40, height: 40 },
      },
    });

    assert.equal(owners[0], 'knob');
  });

test('two equally sized handler boxes are ambiguous, not a coin flip', () => {
  const canvas = node('cv0', 'CANVAS');
  const { owners } = resolveOwner({
    canvasBindings: [canvasBinding(0, { left: 10, top: 10, width: 20, height: 20 })],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [canvas],
    callbackKeys: ['a:click', 'b:click'],
    layoutRects: {
      a: { left: 0, top: 0, width: 40, height: 40 },
      b: { left: 1, top: 1, width: 40, height: 40 },
    },
  });

  // Guessing between two equally plausible owners would wire the canvas to the
  // wrong handler silently, which is worse than leaving it unowned.
  assert.equal(owners[0], '');
});

test('the strict binder refuses a canvas with no visual anchor', () => {
  const { sandbox } = evaluateEntry({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: [],
    registryNodes: [node('cv0', 'CANVAS')],
  });

  assert.throws(() => sandbox.__pulpBindMaterializedCanvases__(),
    /missing visual anchor for live canvas 0/);
});

test('the strict binder refuses a canvas the native side would not bind', () => {
  const { sandbox } = evaluateEntry({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [node('cv0', 'CANVAS')],
    bindResult: () => false,
  });

  assert.throws(() => sandbox.__pulpBindMaterializedCanvases__(),
    /could not bind live canvas 0/);
});

test('the post-commit refresh reports an unbound canvas instead of aborting',
  () => {
    // React commits the behavior tree before the product attaches the DesignIR
    // sibling, so a refresh that threw on a missing anchor would abort a realm
    // that is merely early. It has to stay silent and leave evidence instead.
    const { sandbox } = evaluateEntry({
      canvasBindings: [canvasBinding(0), canvasBinding(1)],
      behaviorCanvasAnchors: [undefined, 'anchor1'],
      registryNodes: [node('cv0', 'CANVAS'), node('cv1', 'CANVAS')],
      callbackKeys: ['btn:click'],
    });

    assert.doesNotThrow(() => sandbox.__pulpApplyMaterializedImportMetadata__());
    assert.deepEqual([...sandbox.__pulpMaterializedCanvasBindingsReady__],
      [false, true]);
    // The refresh still resolves owners for every binding, including the one it
    // could not bind, so a later strict pass has the same answer available.
    assert.deepEqual([...sandbox.__pulpMaterializedCanvasBehaviorOwners__],
      ['btn', 'btn']);
  });

test('the post-commit refresh reports a refused bind instead of aborting', () => {
  const { sandbox } = evaluateEntry({
    canvasBindings: [canvasBinding(0)],
    behaviorCanvasAnchors: ['anchor0'],
    registryNodes: [node('cv0', 'CANVAS')],
    bindResult: () => false,
  });

  assert.doesNotThrow(() => sandbox.__pulpApplyMaterializedImportMetadata__());
  assert.deepEqual([...sandbox.__pulpMaterializedCanvasBindingsReady__], [false]);
});
