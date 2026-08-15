#!/usr/bin/env node
// Compile a deterministic materialized-browser sidecar into an executable
// @pulp/react bundle.  The captured React program remains the UI authority;
// this wrapper only installs the native reconciler and hands the exact captured
// document/assets to WidgetBridge's bounded runtime-import API.

import { build } from 'esbuild';
import { existsSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  materializedMergedTextBindings,
  normalizeMaterializedMetadata,
} from './materialized_metadata_contract.mjs';
import {
  validateMaterializedLayerContract,
} from './materialized_layer_contract.mjs';
import { resolveMaterializedFrames } from './materialized_frame_contract.mjs';
import { loadMaterializedStateAtlas } from './materialized_state_atlas.mjs';
import { materializedCssVariables } from './materialized_css_variables.mjs';
import { canonicalizeMaterializedRuntimeDocument } from
  './materialized_runtime_canonicalization.mjs';
import { buildMaterializedRuntimeEntry } from './materialized_runtime_entry.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.includes('--help') || args.includes('-h')) {
  console.log(`Usage: materialized-runtime-transform.mjs \\
  --in <materialized-document.json> --design-ir <panel.ir.json> \\
  [--prelude <product-runtime.js>]... \\
  [--visual-authority reference|native] \\
  [--runtime-document-asset <relative-path>] \\
  [--state-atlas <captured-states.json>] [--portable-state-assets] \
  [--activate-state <id>] \
  --out <behavior.js>

Compiles Chromium's captured executable document into an @pulp/react tree.
Reference authority keeps DesignIR/captured-state pixels visible while the
live tree supplies behavior. Native authority makes the live Yoga/PreText/
Skia tree both the pixel and behavior authority and embeds no captured-state
images. A product prelude may install analyzer/state/host services before the
captured application scripts load.`);
  process.exit(0);
}
const value = (name) => {
  const at = args.indexOf(name);
  if (at < 0 || at + 1 >= args.length) throw new Error(`${name} is required`);
  return args[at + 1];
};
const values = (name) => {
  const found = [];
  for (let index = 0; index < args.length; ++index) {
    if (args[index] !== name) continue;
    if (index + 1 >= args.length || args[index + 1].startsWith('--')) {
      throw new Error(`${name} requires a value`);
    }
    found.push(args[index + 1]);
  }
  return found;
};
const input = resolve(value('--in'));
const output = resolve(value('--out'));
const designIrArg = resolve(value('--design-ir'));
const visualAuthority = args.includes('--visual-authority')
  ? String(value('--visual-authority')) : 'reference';
if (visualAuthority !== 'reference' && visualAuthority !== 'native') {
  throw new Error('--visual-authority must be reference or native');
}
const preludeArgs = values('--prelude').map((path) => resolve(path));
const runtimeDocumentAsset = args.includes('--runtime-document-asset')
  ? String(value('--runtime-document-asset')) : '';
if (runtimeDocumentAsset &&
    (runtimeDocumentAsset.startsWith('/') || runtimeDocumentAsset.includes('..') ||
     !/^[A-Za-z0-9._/-]+$/.test(runtimeDocumentAsset))) {
  throw new Error('--runtime-document-asset must be a safe relative path');
}
const stateAtlasArg = args.includes('--state-atlas')
  ? resolve(value('--state-atlas')) : '';
const portableStateAssets = args.includes('--portable-state-assets');
if (portableStateAssets && !stateAtlasArg) {
  throw new Error('--portable-state-assets requires --state-atlas');
}
const requestedState = args.includes('--activate-state')
  ? String(value('--activate-state')) : '';
const productPrelude = preludeArgs
  .map((path) => readFileSync(path, 'utf8'))
  .join('\n');
const requestedStatePrelude = requestedState
  ? `globalThis.__pulpRequestedMaterializedState__ = ${JSON.stringify(requestedState)};\n`
  : '';
validateMaterializedLayerContract();
const parsed = JSON.parse(readFileSync(input, 'utf8'));
if (parsed.schema !== 'pulp-materialized-browser-document-v1' || parsed.version !== 1) {
  throw new Error('input is not a materialized browser document v1');
}
const presentationTime = Number(parsed.presentation_time_ms ?? 0);
if (!Number.isFinite(presentationTime) || presentationTime < 0) {
  throw new Error('materialized browser presentation time is invalid');
}
if (productPrelude.includes('</script')) {
  throw new Error('product runtime prelude must not contain a closing script tag');
}
const surfaceBackground = String(
  parsed.surface_style?.background_color ?? '').trim();
if (surfaceBackground.length > 128 ||
    /[;{}<>]/.test(surfaceBackground)) {
  throw new Error('materialized browser surface background is invalid');
}
const mainMetadata = normalizeMaterializedMetadata(parsed);
const fontBindings = mainMetadata.font_bindings;
const layoutBindings = mainMetadata.layout_bindings;
const textBindings = mainMetadata.text_bindings;
const paintBindings = mainMetadata.paint_bindings;
const stateAtlas = loadMaterializedStateAtlas(stateAtlasArg, {
  visualAuthority,
  runtimeBase: portableStateAssets ? dirname(output) : '',
});
for (const state of stateAtlas) {
  if (!state.metadata) continue;
  state.metadata = {
    ...state.metadata,
    text_bindings: materializedMergedTextBindings(
      textBindings, state.metadata.text_bindings),
  };
}
if (requestedState && !stateAtlas.some((state) => state.id === requestedState)) {
  throw new Error(`--activate-state ${requestedState} is not present in the state atlas`);
}
// Put explicitly supplied product services in the same captured-document
// execution realm as the application. Injecting around __pulpRuntimeImport__
// is subtly wrong: browser-compatible globals such as `window` may be lexical
// bindings owned by that document, and application modules may snapshot them
// while their top-level declarations run.
const injectPrelude = (html) => {
  if (!productPrelude) return html;
  if (!/<head(?:\s[^>]*)?>/i.test(html)) {
    throw new Error('materialized browser document is missing a head element');
  }
  return html.replace(
    /<head([^>]*)>/i,
    `<head$1><script>\n${requestedStatePrelude}${productPrelude}\n</script>`);
};
const runtimeDocument = canonicalizeMaterializedRuntimeDocument({
  ...parsed, html: injectPrelude(parsed.html),
});
const sidecar = JSON.stringify(runtimeDocument);
if (runtimeDocumentAsset) {
  const assetOutput = resolve(dirname(output), runtimeDocumentAsset);
  mkdirSync(dirname(assetOutput), { recursive: true });
  writeFileSync(assetOutput, sidecar);
}

const canvasBindings = Array.isArray(parsed.canvas_bindings)
  ? parsed.canvas_bindings : [];
let behaviorCanvasAnchors = Array.isArray(parsed.behavior_canvas_anchors)
  ? parsed.behavior_canvas_anchors : [];
const ir = JSON.parse(readFileSync(designIrArg, 'utf8'));
const capturedCssVariables = materializedCssVariables(ir.tokens);
// The visible DesignIR root is the full capture surface. The executable
// behavior tree is the authored panel INSIDE that surface. Prefer the explicit
// registration rect; old uncropped captures legitimately fall back to the
// root extent.
const materializedFrames = resolveMaterializedFrames(
  ir, mainMetadata.coordinate_space);
const { width: visualWidth, height: visualHeight } = materializedFrames.visual;
const {
  left: authoredLeft, top: authoredTop,
  width: authoredWidth, height: authoredHeight,
} = materializedFrames.behavior;
const authoredTransform = materializedFrames.behavior.transform || null;
const byAsset = new Map();
const capturedPaintAuthorityAnchors = [];
const visit = (node) => {
  if (!node || typeof node !== 'object') return;
  const asset = node.attributes?.asset_ref;
  const anchor = node.stable_anchor_id;
  if (typeof asset === 'string' && asset.startsWith('canvas:') &&
      typeof anchor === 'string' && anchor.length > 0) {
    byAsset.set(asset, anchor);
  }
  if (node.attributes?.materialized_role === 'captured-paint-authority' &&
      typeof anchor === 'string' && anchor.length > 0) {
    capturedPaintAuthorityAnchors.push(anchor);
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
const entry = buildMaterializedRuntimeEntry({
  capturedCssVariables,
  presentationTime,
  requestedState,
  textBindings,
  layoutBindings,
  paintBindings,
  runtimeDocumentAsset,
  sidecar,
  productPrelude,
  surfaceBackground,
  authoredLeft,
  authoredTop,
  authoredWidth,
  authoredHeight,
  authoredTransform,
  visualAuthority,
  stateAtlas,
  visualWidth,
  visualHeight,
  canvasBindings,
  behaviorCanvasAnchors,
  capturedPaintAuthorityAnchors,
});

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
