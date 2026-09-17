import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import { buildMaterializedRuntimeEntry } from './materialized_runtime_entry.mjs';

const BRIDGE_STUBS = ['createCol', 'setPosition', 'setLeft', 'setTop', 'setFlex',
  'setVisible', 'setOpacity', 'setPointerEvents', 'setZIndex',
  'setTransformOrigin', 'setTransform'];

// The entry is an ES module whose only imports are React and the native
// renderer. Replacing those two lines with stubs lets the rest of the module
// execute verbatim in a sandbox, so these cases exercise the shipped source
// rather than a re-implementation of it. Evaluating it (instead of parsing it)
// is deliberate: a declaration placed in the wrong function body still parses.
function evaluateEntry({ layoutBindings = [], registryNodes = [], stateAtlas = [] }) {
  const source = buildMaterializedRuntimeEntry({
    capturedCssVariables: {}, presentationTime: 0, requestedState: '',
    textBindings: [], layoutBindings, paintBindings: [],
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
    setCapturedLineBoxes: () => {},
  };
  sandbox.writes = new Map();
  for (const name of BRIDGE_STUBS) sandbox[name] = (id, ...args) => {
    const key = name === 'setFlex' ? name + ':' + args.shift() : name;
    sandbox.writes.set(id + ':' + key, args[0]);
  };
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


function fixture() {
  const node = (id, tag = 'div') => ({ tagName: tag.toUpperCase(), __pulpId: id,
    id, _children: [], parentElement: null,
    getAttribute: name => name === 'data-menu' && id === 'menu' ? 'open' : null,
    __pulpAuthoredLayout__: {} });
  const menu = node('menu');
  menu.__pulpAuthoredLayout__ = { position: 'fixed', left: 8, top: 12, width: 230 };
  const first = node('first', 'button');
  const last = node('last', 'button');
  const header = node('header');
  first.parentElement = last.parentElement = menu;
  menu._children = [first, last];
  const registry = [menu, first, last, header];
  const path = [{ tag: 'div', index: 0 }];
  const binding = (path, height, top = 0) => ({ path,
    box: { left: 0, top, width: 230, height } });
  const bindings = [binding(path, 60),
    binding([...path, { tag: 'button', index: 0 }], 30),
    binding([...path, { tag: 'button', index: 1 }], 30, 30),
    binding([{ tag: 'div', index: 1 }], 40)];
  const sandbox = evaluateEntry({ registryNodes: registry,
    stateAtlas: [{ id: 'menu', match: { selector: '[data-menu="open"]' },
      activate: [], metadata: { layout_bindings: bindings } }] });
  const extra = node('extra', 'button');
  extra.parentElement = menu;
  return { sandbox, menu, first, last, registry, extra };
}

test('conditional state children release captured positions and intrinsic height', () => {
  const { sandbox: s, menu, registry, extra } = fixture();
  assert.equal(s.writes.get('menu:setFlex:height'), 60, 'control: captured state applied');
  assert.equal(s.writes.get('last:setTop'), 30);
  menu._children.splice(1, 0, extra);
  registry.push(extra);
  s.__pulpApplyMaterializedImportMetadata__();
  assert.equal(s.writes.get('menu:setFlex:height'), 'auto');
  assert.equal(s.writes.get('menu:setPosition'), 'fixed');
  assert.equal(s.writes.get('menu:setLeft'), 8);
  assert.equal(s.writes.get('menu:setFlex:width'), 230);
  assert.equal(s.writes.get('last:setPosition'), 'relative');
  assert.equal(s.writes.get('last:setTop'), 'auto');
  assert.equal(s.writes.get('header:setFlex:height'), 40, 'unrelated capture stays active');
  assert.equal(s.__pulpMaterializedMetadataDiagnostics__.layout_dynamic_nodes, 4);
  assert.equal(s.writes.has('extra:setPosition'), false, 'new child retains authored flow');
});

test('removing conditional children restores capture and another insertion releases it again', () => {
  const { sandbox: s, menu, registry, extra } = fixture();
  for (let iteration = 0; iteration < 2; ++iteration) {
    menu._children.push(extra);
    registry.push(extra);
    s.__pulpApplyMaterializedImportMetadata__();
    assert.equal(s.writes.get('menu:setFlex:height'), 'auto');
    menu._children.pop();
    registry.pop();
    s.__pulpApplyMaterializedImportMetadata__();
    assert.equal(s.writes.get('menu:setFlex:height'), 60);
    assert.equal(s.writes.get('last:setTop'), 30);
  }
});

test('a nested shape change invalidates the complete captured state', () => {
  const { sandbox: s, first, registry, extra } = fixture();
  extra.parentElement = first;
  first._children.push(extra);
  registry.push(extra);
  s.__pulpApplyMaterializedImportMetadata__();
  assert.equal(s.writes.get('menu:setFlex:height'), 'auto');
  assert.equal(s.writes.get('first:setPosition'), 'relative');
});

test('restoration preserves imperative style changes made after the capture', () => {
  const { sandbox: s, menu, registry, extra } = fixture();
  menu.style = { width: '80%', top: 55 };
  menu._children.push(extra);
  registry.push(extra);
  s.__pulpApplyMaterializedImportMetadata__();
  assert.equal(s.writes.get('menu:setFlex:width'), '80%');
  assert.equal(s.writes.get('menu:setTop'), 55);
});
