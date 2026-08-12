#!/usr/bin/env node
// Compile a deterministic materialized-browser sidecar into an executable
// @pulp/react bundle.  The captured React program remains the UI authority;
// this wrapper only installs the native reconciler and hands the exact captured
// document/assets to WidgetBridge's bounded runtime-import API.

import { build } from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.includes('--help') || args.includes('-h')) {
  console.log(`Usage: materialized-runtime-transform.mjs \\
  --in <materialized-document.json> --design-ir <panel.ir.json> \\
  [--prelude <product-runtime.js>] --out <behavior.js>

Compiles Chromium's captured executable document into a hidden @pulp/react
behavior tree. The DesignIR remains the visible pixel authority. A product
prelude may install analyzer/state/host services after captured scripts load
and before the captured App mounts.`);
  process.exit(0);
}
const value = (name) => {
  const at = args.indexOf(name);
  if (at < 0 || at + 1 >= args.length) throw new Error(`${name} is required`);
  return args[at + 1];
};
const input = resolve(value('--in'));
const output = resolve(value('--out'));
const designIrArg = args.includes('--design-ir') ? resolve(value('--design-ir')) : '';
const preludeArg = args.includes('--prelude') ? resolve(value('--prelude')) : '';
const productPrelude = preludeArg ? readFileSync(preludeArg, 'utf8') : '';
const sidecar = readFileSync(input, 'utf8');
const parsed = JSON.parse(sidecar);
if (parsed.schema !== 'pulp-materialized-browser-document-v1' || parsed.version !== 1) {
  throw new Error('input is not a materialized browser document v1');
}

const canvasBindings = Array.isArray(parsed.canvas_bindings)
  ? parsed.canvas_bindings : [];
let behaviorCanvasAnchors = Array.isArray(parsed.behavior_canvas_anchors)
  ? parsed.behavior_canvas_anchors : [];
if (designIrArg) {
  const ir = JSON.parse(readFileSync(designIrArg, 'utf8'));
  const byAsset = new Map();
  const visit = (node) => {
    if (!node || typeof node !== 'object') return;
    const asset = node.attributes?.asset_ref;
    const anchor = node.stable_anchor_id;
    if (typeof asset === 'string' && asset.startsWith('canvas:') &&
        typeof anchor === 'string' && anchor.length > 0) {
      byAsset.set(asset, anchor);
    }
    for (const child of Array.isArray(node.children) ? node.children : []) visit(child);
  };
  visit(ir.root);
  behaviorCanvasAnchors = canvasBindings.map((binding) => {
    const backendId = String(binding.anchor || '').split(':').pop();
    const visualAnchor = byAsset.get(`canvas:${backendId}`);
    if (!visualAnchor) {
      throw new Error(`DesignIR is missing captured canvas:${backendId}`);
    }
    return visualAnchor;
  });
}
const entry = `
import * as React from 'react';
import { createRoot as createPulpRoot, render as renderPulp, unmount as unmountPulp } from '@pulp/react';

const g = globalThis;
g.React = React;
const behaviorRootId = '__pulp_materialized_behavior__';
class NativeRoot {
  constructor() { this.container = createPulpRoot(behaviorRootId, '__behavior_pr_'); }
  render(element) { return renderPulp(element, this.container); }
  unmount() { return unmountPulp(this.container); }
}
g.ReactDOM = {
  createRoot: () => new NativeRoot(),
  hydrateRoot: (_container, element) => {
    const root = new NativeRoot();
    if (element !== undefined) root.render(element);
    return root;
  },
  render: (element) => {
    const root = new NativeRoot();
    root.render(element);
    return root;
  },
  flushSync: (fn) => typeof fn === 'function' ? fn() : undefined
};
if (typeof g.__pulpRuntimeImport__ !== 'function') {
  throw new Error('materialized runtime import capability is unavailable');
}
g.__pulpRuntimeImport__(${JSON.stringify(sidecar)}, 'materialized-browser');
if (g.__pulpRuntimeImportErr__) throw new Error(String(g.__pulpRuntimeImportErr__));
// Product-owned runtime services (analyzer/state/host bridge) are injected
// explicitly. They are not inferred from or hard-coded into generic import
// tooling. The captured scripts have now installed their public helpers, while
// the App itself has not mounted yet.
${productPrelude}
// Runtime import materializes the captured document, which is allowed to
// replace document.body while bootstrapping. Create the native behavior root
// only after that replacement so it cannot be detached by the original page.
createCol(behaviorRootId, '');
// Canvas programs read their laid-out CSS boxes while drawing. display:none
// collapses those boxes and produces the exact NaN/zero geometry this import
// is designed to avoid. Keep a real authored-size layout surface, but make its
// pixels and hit testing inert; the DesignIR sibling is the only visible tree.
setPosition(behaviorRootId, 'absolute');
setLeft(behaviorRootId, 0);
setTop(behaviorRootId, 0);
setFlex(behaviorRootId, 'width', 1320);
setFlex(behaviorRootId, 'height', 860);
setVisible(behaviorRootId, false);
setPointerEvents(behaviorRootId, 'none');
// The materialized browser document ends with its own ReactDOM.createRoot
// bootstrap. Browser-side Babel evaluates that as a classic script; the
// native runtime evaluates the captured blocks indirectly, where the App
// declaration survives but the final implicit mount is not portable across
// engines. Mount that exact captured App once through the native reconciler.
// No UI component, state, or style is reconstructed here.
if (typeof g.App !== 'function') {
  throw new Error('materialized browser document did not define App');
}
const nativeRoot = new NativeRoot();
nativeRoot.render(React.createElement(g.App));
if (typeof g.__pulpRuntimeSettle__ === 'function') g.__pulpRuntimeSettle__(8);
const canvasBindings = ${JSON.stringify(canvasBindings)};
const behaviorCanvasAnchors = ${JSON.stringify(behaviorCanvasAnchors)};
g.__pulpBindMaterializedCanvases__ = function () {
  for (const binding of canvasBindings) {
    const visualAnchor = behaviorCanvasAnchors[binding.index];
    if (!visualAnchor) {
      throw new Error('missing visual anchor for live canvas ' + binding.index);
    }
    const bindType = typeof bindCanvasBehaviorAt;
    const bound = bindType === 'function'
      ? bindCanvasBehaviorAt(
          visualAnchor, behaviorRootId, binding.index) : false;
    if (!bound) {
      throw new Error('could not bind live canvas ' + binding.index +
        ' to ' + visualAnchor + ' bind=' + bindType);
    }
  }
  return canvasBindings.length;
};
`;

const deps = resolve(here, 'node_modules');
const pulpReact = resolve(here, '..', '..', '..', 'packages', 'pulp-react');
const result = await build({
  stdin: { contents: entry, resolveDir: dirname(input), sourcefile: 'materialized-browser-entry.js' },
  bundle: true,
  format: 'iife',
  target: ['es2020'],
  platform: 'browser',
  write: false,
  minify: false,
  define: { 'process.env.NODE_ENV': '"production"' },
  nodePaths: [deps, resolve(pulpReact, 'node_modules')],
  alias: {
    'react': resolve(deps, 'react'),
    'react/jsx-runtime': resolve(deps, 'react/jsx-runtime.js'),
    '@pulp/react': resolve(pulpReact, 'src', 'index.ts'),
    'react-reconciler': resolve(deps, 'react-reconciler'),
    'react-reconciler/constants.js': resolve(deps, 'react-reconciler/constants.js'),
    'scheduler': resolve(deps, 'scheduler'),
  },
});
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, result.outputFiles[0].contents);
console.log(`Wrote ${output} (${result.outputFiles[0].contents.length} bytes)`);
