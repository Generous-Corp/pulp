import assert from 'node:assert/strict';
import vm from 'node:vm';

import { buildMaterializedRuntimeEntry } from './materialized_runtime_entry.mjs';

// Native bridge functions the entry calls while mounting its behavior root.
const BRIDGE_STUBS = ['createCol', 'createImage', 'setPosition', 'setLeft', 'setTop', 'setFlex',
  'setVisible', 'setOpacity', 'setPointerEvents', 'setZIndex', 'setBackground', 'setBackgroundGradient',
  'setTransformOrigin', 'setTransform', 'setObjectFit', 'setImageSource'];

// Evaluate the generated entry against deterministic bridge stubs. Keeping this
// fixture shared prevents the performance and correctness suites from silently
// exercising different runtime sources.
export function evaluateEntry({ layoutBindings = [], registryNodes = [],
                         textBindings = [], paintBindings = [], pseudoBindings = [], stateAtlas = [],
                         bridge = {}, productPrelude = '',
                         visualAuthority = null }) {
  const source = buildMaterializedRuntimeEntry({
    capturedCssVariables: {}, presentationTime: 0, requestedState: '',
    textBindings, layoutBindings, paintBindings, pseudoBindings,
    runtimeDocumentAsset: null, sidecar: null, productPrelude,
    surfaceBackground: null, authoredLeft: 0, authoredTop: 0,
    authoredWidth: 100, authoredHeight: 100, authoredTransform: null,
    visualAuthority, stateAtlas, visualWidth: 100, visualHeight: 100,
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

  assert.equal(typeof sandbox.__pulpApplyMaterializedImportMetadata__,
    'function', 'entry did not evaluate far enough to publish the applier');
  return sandbox;
}
