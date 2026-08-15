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
  materializedAbsoluteInsets,
  materializedMergedTextBindings,
  materializedRuntimeFontStack,
  materializedSvgRectGeometry,
  materializedTextTargetGeometry,
  normalizeMaterializedMetadata,
} from './materialized_metadata_contract.mjs';
import {
  MATERIALIZED_BACKGROUND_Z,
  MATERIALIZED_STATE_ATLAS_Z,
  MATERIALIZED_BEHAVIOR_Z,
  validateMaterializedLayerContract,
} from './materialized_layer_contract.mjs';
import { resolveMaterializedFrames } from './materialized_frame_contract.mjs';
import { loadMaterializedStateAtlas } from './materialized_state_atlas.mjs';
import { materializedCssVariables } from './materialized_css_variables.mjs';
import { canonicalizeMaterializedRuntimeDocument } from
  './materialized_runtime_canonicalization.mjs';

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
const entry = `
import * as React from 'react';
import { createRoot as createPulpRoot, render as renderPulp, unmount as unmountPulp } from '@pulp/react';

const g = globalThis;
${materializedRuntimeFontStack.toString()}
${materializedTextTargetGeometry.toString()}
${materializedAbsoluteInsets.toString()}
${materializedSvgRectGeometry.toString()}
// Executable React styles retain authored var(--name) expressions. Restore the
// exact computed custom-property values Chromium captured before React mounts,
// so font families, colours, lengths and other string-valued props reach the
// native prop applier as resolved values instead of silently falling back.
const capturedCssVariables = JSON.parse(${JSON.stringify(
  JSON.stringify(capturedCssVariables))});
g.__pulpCssVars = Object.freeze(Object.assign(
  Object.create(null), g.__pulpCssVars || {}, capturedCssVariables));
// Snapshot the native renderer bridge before the captured browser document
// installs DOM-shaped globals with overlapping names (notably createCanvas).
// @pulp/react consults this immutable namespace first, while the authored
// browser code continues to see the compatibility functions it expects.
g.__pulpNativeBridgeFunctions__ = Object.freeze({
  createCanvas: g.createCanvas,
  setSvgRect: g.setSvgRect,
});
g.React = React;
const behaviorRootId = '__pulp_materialized_behavior__';
const surfaceRootId = '__pulp_materialized_surface__';
const stateAtlasRootId = '__pulp_materialized_state_atlas__';
class NativeRoot {
  constructor() { this.container = createPulpRoot(behaviorRootId, '__behavior_pr_'); }
  render(element) { return renderPulp(element, this.container); }
  unmount() { return unmountPulp(this.container); }
}
let capturedRootElement;
let activeNativeRoot;
function deferredRoot() {
  return {
    render: function (element) {
      capturedRootElement = element;
      if (activeNativeRoot) activeNativeRoot.render(element);
    },
    unmount: function () {
      capturedRootElement = undefined;
      if (activeNativeRoot) activeNativeRoot.unmount();
    }
  };
}
g.ReactDOM = {
  // The captured document owns the application bootstrap. Defer its first
  // render until runtime import has finished replacing document.body and the
  // native behavior root exists. Mounting App again after import duplicates
  // the complete UI and, more subtly, creates two independent state owners.
  createRoot: () => deferredRoot(),
  hydrateRoot: (_container, element) => {
    const root = deferredRoot();
    if (element !== undefined) root.render(element);
    return root;
  },
  render: (element) => {
    const root = deferredRoot();
    root.render(element);
    return root;
  },
  flushSync: (fn) => typeof fn === 'function' ? fn() : undefined
};
// Imported browser applications conventionally repaint a canvas by clearing
// its full backing store at the start of every RAF. Opt this isolated runtime
// into retained-frame replacement before any application canvas is created;
// ordinary Pulp Canvas2D users keep browser command-history semantics.
g.__pulpRetainedCanvasFrames__ = true;
g.__pulpLogicalCanvasScale__ = true;
// Bootstrap at the exact Chromium frame timestamp recorded beside the
// accepted pixels. Starting an analyzer at an unrelated wall-clock phase
// would make a correct executable canvas look different on its first frame.
g.__pulpCapturedPresentationTime__ = ${JSON.stringify(presentationTime)};
// Replay the executable document through the same bounded presentation-time
// interval Chromium observed. This is deliberately a clock contract rather
// than a captured bitmap or a product-specific animation offset: rAF-driven
// canvases receive browser-compatible timestamps until they reach the accepted
// frame. Product-owned live data can subsequently replace the captured demo
// source; the canonical import proof itself stays frozen at the accepted frame
// so repeated screenshots are byte-stable rather than wall-clock dependent.
g.__pulpCapturedReplayClock__ = {
  current: 0,
  target: g.__pulpCapturedPresentationTime__
};
g.__pulpAnimationFrameTimestamp__ = function (_nativeNow) {
  const clock = g.__pulpCapturedReplayClock__;
  if (clock.current < clock.target) {
    clock.current = Math.min(clock.target, clock.current + 50);
  }
  return clock.current;
};
// Keep deterministic capture activation visible to both halves of the
// materialized runtime. The requested state is also injected into the
// captured document before its application scripts, but some browser-compat
// runtimes evaluate classic scripts through a document-owned global proxy.
// Publishing the same immutable value on the native realm prevents that
// proxy boundary from turning a requested dropdown/modal proof into the main
// surface while preserving the original React handler as the activator.
g.__pulpRequestedMaterializedState__ = ${JSON.stringify(requestedState)};
g.performance = {
  now: function () { return g.__pulpCapturedReplayClock__.current; }
};
function materializedDomRegistryValues() {
  const values = [];
  const seen = new Set();
  const append = function (registry) {
    if (!registry || typeof registry.values !== 'function') return;
    for (const node of registry.values()) {
      const id = node && (node.__pulpId || node.id);
      if (id && seen.has(String(id))) continue;
      if (id) seen.add(String(id));
      values.push(node);
    }
  };
  // Captured classic scripts may own a window proxy while @pulp/react host
  // commits publish their public Element shims on the native realm. Read both
  // views of the same registry so conditionally mounted menus and dialogs are
  // observable immediately after React commits them.
  append(g.__pulpReactDomRegistry__);
  if (g !== globalThis) append(globalThis.__pulpReactDomRegistry__);
  // The live React host and captured classic-script realm can be separated by
  // a window proxy. Querying through a tiny callable avoids relying on proxy
  // property mirroring for conditionally committed children.
  const provider = g.__pulpReactDomRegistryValues__ ||
    globalThis.__pulpReactDomRegistryValues__;
  if (typeof provider === 'function') {
    const provided = provider();
    if (Array.isArray(provided)) {
      append({ values: function () { return provided.values(); } });
    }
  }
  return values;
}
const capturedTextBindings = ${JSON.stringify(textBindings)};
const capturedLayoutBindings = ${JSON.stringify(layoutBindings)};
const capturedPaintBindings = ${JSON.stringify(paintBindings)};
const capturedHomeMetadata = {
  layout_bindings: capturedLayoutBindings,
  text_bindings: capturedTextBindings,
  paint_bindings: capturedPaintBindings,
};
let activeCapturedState = '';
// React may commit a conditionally-mounted state over more than one pass.
// Keep the commit hook pointed at the active state's Chromium evidence so a
// later modal/menu subtree commit converges its newly-registered descendants
// instead of accidentally reapplying the home document.
let activeMaterializedMetadata = capturedHomeMetadata;
// Filled after the generated canvas binding table is declared. Keeping this
// callback in the same post-commit hook as captured layout/text evidence lets
// React replace conditional behavior subtrees without leaving the visible
// DesignIR canvas wired to an obsolete callback snapshot.
let syncMaterializedCanvasBehaviorsAfterCommit = null;
function materializedElementChildren(node, registrySet) {
  const children = Array.isArray(node && node._children) ? node._children : [];
  return children.filter(child => child && registrySet.has(child));
}
function materializedNodeTag(node) {
  return String(node && node.tagName || '').toLowerCase();
}
function materializedNodeAtPath(binding, values) {
  const registrySet = new Set(values);
  const roots = values.filter(node => {
    const parent = node && (node.parentElement || node._parentElement);
    return !parent || !registrySet.has(parent);
  });
  let siblings = roots;
  let node = null;
  for (const step of binding.path) {
    node = siblings[step.index] || null;
    if (!node || materializedNodeTag(node) !== step.tag) return null;
    siblings = materializedElementChildren(node, registrySet);
  }
  return node;
}
function materializedOptionalTextNode(binding, values) {
  const matches = values.filter(node => {
    const anonymousTargets = Array.isArray(node && node.__pulpAnonymousTextTargets)
      ? node.__pulpAnonymousTextTargets : [];
    if (binding.anonymous_text_index !== undefined)
      return anonymousTargets.some(target =>
        String(target && target.text || '') === binding.text);
    return String(node && node.textContent || '') === binding.text;
  });
  return matches.length === 1 ? matches[0] : null;
}
function applyMaterializedImportMetadata(metadata) {
  const values = materializedDomRegistryValues();
  const activeLayoutBindings = Array.isArray(metadata && metadata.layout_bindings)
    ? metadata.layout_bindings : [];
  const activeTextBindings = Array.isArray(metadata && metadata.text_bindings)
    ? metadata.text_bindings : [];
  const activePaintBindings = Array.isArray(metadata && metadata.paint_bindings)
    ? metadata.paint_bindings : [];
  let applied = 0;
  const diagnostics = {
    state_id: typeof activeCapturedState === 'string' ? activeCapturedState : '',
    layout_expected: activeLayoutBindings.length,
    layout_applied: 0,
    layout_node_miss: 0,
    text_expected: activeTextBindings.filter(binding =>
      !binding.runtime_optional).length,
    text_applied: 0,
    text_node_miss: 0,
    text_content_mismatch: 0,
    text_target_miss: 0,
    text_optional_expected: activeTextBindings.filter(binding =>
      binding.runtime_optional).length,
    text_optional_applied: 0,
    text_optional_miss: 0,
    paint_expected: activePaintBindings.length,
    paint_applied: 0,
    paint_node_miss: 0,
    paint_unsupported: 0,
    paint_nodes: [],
  };
  // Freeze captured elements to Chromium's resolved parent-relative boxes.
  // This is generation evidence, not a screenshot at runtime: React remains
  // executable and newly-created dynamic nodes still flow through Yoga until
  // their captured state contributes an equivalent binding.
  if (typeof g.setPosition === 'function' && typeof g.setFlex === 'function') {
    for (const binding of activeLayoutBindings) {
      const node = materializedNodeAtPath(binding, values);
      const id = node && (node.__pulpId || node.id);
      if (!id) {
        ++diagnostics.layout_node_miss;
        continue;
      }
      // Browser capture stores each child border box relative to its parent's
      // border box. Yoga resolves absolute left/top from the parent's padding
      // edge, so replaying the browser number verbatim adds the parent's
      // border a second time (a 1 CSS-px / 2 Retina-px shift in compact rails
      // and segmented controls). Translate coordinate spaces explicitly.
      const parent = node.parentElement || node._parentElement;
      const parentId = parent && (parent.__pulpId || parent.id);
      const parentMetrics = parentId && typeof g.getLayoutBoxMetrics === 'function'
        ? g.getLayoutBoxMetrics(String(parentId)) : null;
      const targetMetrics = typeof g.getLayoutBoxMetrics === 'function'
        ? g.getLayoutBoxMetrics(String(id)) : null;
      const insets = materializedAbsoluteInsets(
        binding.box, parentMetrics, targetMetrics);
      const left = insets ? insets.left : binding.box.left;
      const top = insets ? insets.top : binding.box.top;
      g.setPosition(String(id), 'absolute');
      g.setLeft(String(id), left);
      g.setTop(String(id), top);
      g.setFlex(String(id), 'width', binding.box.width);
      g.setFlex(String(id), 'height', binding.box.height);
      ++applied;
      ++diagnostics.layout_applied;
    }
  }
  // Apply Chromium's resolved SVG paint after React has committed authored
  // attributes. This intentionally wins over currentColor and stylesheet
  // tokens: the frozen computed value is the visual authority for this state.
  for (const binding of activePaintBindings) {
    const node = materializedNodeAtPath(binding, values);
    const id = node && (node.__pulpId || node.id);
    if (!id) {
      ++diagnostics.paint_node_miss;
      continue;
    }
    diagnostics.paint_nodes.push({
      index: binding.index,
      tag: binding.tag,
      id: String(id),
    });
    if (typeof g.setOpacity === 'function')
      g.setOpacity(String(id), binding.paint.opacity);
    if (typeof g.setTextColor === 'function' && binding.paint.color)
      g.setTextColor(String(id), binding.paint.color);
    if (binding.tag !== 'svg') {
      if (binding.paint.stroke_dasharray !== 'none') {
        ++diagnostics.paint_unsupported;
        continue;
      }
      if (typeof g.setSvgFill === 'function')
        g.setSvgFill(String(id), binding.paint.fill);
      if (typeof g.setSvgStroke === 'function')
        g.setSvgStroke(String(id), binding.paint.stroke);
      if (typeof g.setSvgStrokeWidth === 'function')
        g.setSvgStrokeWidth(String(id), binding.paint.stroke_width);
      // SVG primitives are excluded from Yoga layout replay. Apply the
      // Chromium-resolved parent-local box directly as rect geometry; zeroing
      // x/y here collapses repeated bars because no layout binding moves the
      // primitive View back to its captured position.
      if (binding.tag === 'rect' && binding.box &&
          typeof g.__pulpNativeBridgeFunctions__.setSvgRect === 'function') {
        const rect = materializedSvgRectGeometry(binding.box);
        if (rect) g.__pulpNativeBridgeFunctions__.setSvgRect(String(id),
          rect.x, rect.y, rect.width, rect.height);
      }
    }
    ++applied;
    ++diagnostics.paint_applied;
  }
  if (typeof g.setCapturedLineBoxes !== 'function') return applied;
  for (const binding of activeTextBindings) {
    const optional = binding.runtime_optional === true;
    const node = materializedNodeAtPath(binding, values)
      || (optional ? materializedOptionalTextNode(binding, values) : null);
    if (!node) {
      if (optional) ++diagnostics.text_optional_miss;
      else ++diagnostics.text_node_miss;
      continue;
    }
    // HTML controls remain semantic/styled containers in the native tree.
    // Their text may be painted by a generated Label child; the React host
    // publishes that renderer-neutral target without exposing it to authored
    // DOM behavior or making the importer know widget-specific ids.
    const anonymousTargets = Array.isArray(node.__pulpAnonymousTextTargets)
      ? node.__pulpAnonymousTextTargets : [];
    const anonymousTarget = binding.anonymous_text_index === undefined
      ? null : anonymousTargets[binding.anonymous_text_index];
    if (anonymousTarget) {
      if (String(anonymousTarget.text || '') !== binding.text) {
        if (optional) ++diagnostics.text_optional_miss;
        else ++diagnostics.text_content_mismatch;
        continue;
      }
      // Mixed-content text is represented by a synthetic Label. Make its
      // captured coordinate space the complete owner box; it must not take a
      // second Yoga slot beside authored inline children.
      if (typeof g.setPosition === 'function' && typeof g.setFlex === 'function') {
        g.setPosition(String(anonymousTarget.id), 'absolute');
        g.setLeft(String(anonymousTarget.id), 0);
        g.setTop(String(anonymousTarget.id), 0);
        g.setFlex(String(anonymousTarget.id), 'width', binding.basis.width);
        const maxBottom = binding.boxes.reduce((value, box) =>
          Math.max(value, box.top + box.height), 0);
        g.setFlex(String(anonymousTarget.id), 'height', maxBottom);
      }
    } else if (String(node.textContent || '') !== binding.text) {
      if (optional) ++diagnostics.text_optional_miss;
      else ++diagnostics.text_content_mismatch;
      continue;
    }
    const id = anonymousTarget?.id ||
      node.__pulpTextTargetId || node.__pulpId || node.id;
    if (!id) {
      if (optional) ++diagnostics.text_optional_miss;
      else ++diagnostics.text_target_miss;
      continue;
    }
    // Generated anonymous Labels do not participate in the authored DOM
    // cascade. Reapply Chromium's computed typography to the actual native
    // paint target rather than letting it inherit the bridge defaults.
    // Pure Label owners receive the same resolved values, making this path
    // deterministic for both HTML controls and ordinary text elements.
    if (typeof g.setFontFamily === 'function') {
      g.setFontFamily(String(id), materializedRuntimeFontStack(binding));
    }
    if (typeof g.setFontSize === 'function')
      g.setFontSize(String(id), binding.basis.requested.font_size);
    if (typeof g.setFontWeight === 'function')
      g.setFontWeight(String(id), binding.basis.requested.font_weight);
    if (typeof g.setFontStyle === 'function')
      g.setFontStyle(String(id), binding.basis.requested.font_slant === 1
        ? 'italic' : binding.basis.requested.font_slant === 2
          ? 'oblique' : 'normal');
    if (typeof g.setLetterSpacing === 'function' &&
        Number.isFinite(binding.basis.requested.letter_spacing)) {
      g.setLetterSpacing(String(id), binding.basis.requested.letter_spacing);
    }
    let targetBoxes = binding.boxes;
    let targetBasisWidth = binding.basis.width;
    if (!anonymousTarget && node.__pulpTextTargetId &&
        typeof g.getLayoutBoxMetrics === 'function') {
      const metrics = g.getLayoutBoxMetrics(String(id));
      const ownerId = node.__pulpId || node.id;
      const ownerMetrics = ownerId
        ? g.getLayoutBoxMetrics(String(ownerId)) : null;
      const geometry = materializedTextTargetGeometry(
        binding.basis.width, metrics, ownerMetrics);
      if (geometry) {
        targetBoxes = binding.boxes.map(box => ({
          ...box,
          left: box.left - geometry.localX,
          top: box.top - geometry.localY,
        }));
        targetBasisWidth = geometry.basisWidth;
      }
    }
    g.setCapturedLineBoxes(String(id), targetBoxes, targetBasisWidth,
      binding.basis.resolved_face, false);
    ++applied;
    if (optional) ++diagnostics.text_optional_applied;
    else ++diagnostics.text_applied;
  }
  g.__pulpMaterializedMetadataDiagnostics__ = diagnostics;
  return applied;
}
g.__pulpApplyMaterializedImportMetadata__ = function () {
  const applied = applyMaterializedImportMetadata(activeMaterializedMetadata);
  if (typeof syncMaterializedCanvasBehaviorsAfterCommit === 'function')
    syncMaterializedCanvasBehaviorsAfterCommit();
  return applied;
};
function materializedMatches(node, selector) {
  if (!node || typeof selector !== 'string') return false;
  // The web-compat Element shim intentionally supports only a bounded CSS
  // selector subset. A false result is not authoritative for semantic
  // data/ARIA attributes added by the native React host, so fall through to
  // the deterministic fallback matcher.
  if (typeof node.matches === 'function' && node.matches(selector)) return true;
  let remaining = selector.trim();
  const attributes = [];
  remaining = remaining.replace(
    /\\[([A-Za-z0-9_:-]+)(?:=(?:"([^"]*)"|'([^']*)'|([^\\]]+)))?\\]/g,
    function (_all, name, doubleQuoted, singleQuoted, bare) {
      attributes.push({ name: name, value: doubleQuoted !== undefined
        ? doubleQuoted : singleQuoted !== undefined ? singleQuoted
        : bare !== undefined ? String(bare).trim() : null });
      return '';
    });
  const idMatch = remaining.match(/#([A-Za-z0-9_-]+)/);
  const classMatches = Array.from(remaining.matchAll(/\\.([A-Za-z0-9_-]+)/g));
  const tag = remaining.replace(/#[A-Za-z0-9_-]+/g, '')
    .replace(/\\.[A-Za-z0-9_-]+/g, '').trim().toLowerCase();
  if (tag && String(node.tagName || '').toLowerCase() !== tag) return false;
  if (idMatch && String(node.id || node.getAttribute?.('id') || '') !== idMatch[1]) {
    return false;
  }
  const classes = String(node.className || node.getAttribute?.('class') || '')
    .split(/\\s+/).filter(Boolean);
  for (const match of classMatches) if (!classes.includes(match[1])) return false;
  for (const attribute of attributes) {
    if (typeof node.getAttribute !== 'function') return false;
    const actual = node.getAttribute(attribute.name);
    if (actual === null || (attribute.value !== null && actual !== attribute.value)) {
      return false;
    }
  }
  return true;
}
function materializedClosest(node, selector) {
  let current = node;
  while (current) {
    if (materializedMatches(current, selector)) return current;
    current = current.parentElement || current._parentElement || null;
  }
  return null;
}
function materializedLastDescendantSplit(selector) {
  let bracketDepth = 0;
  let quote = '';
  let split = -1;
  for (let index = 0; index < selector.length; ++index) {
    const ch = selector[index];
    if (quote) {
      if (ch === quote) quote = '';
      continue;
    }
    if ((ch === '"' || ch === "'") && bracketDepth > 0) {
      quote = ch;
    } else if (ch === '[') {
      ++bracketDepth;
    } else if (ch === ']' && bracketDepth > 0) {
      --bracketDepth;
    } else if (/\\s/.test(ch) && bracketDepth === 0) {
      split = index;
    }
  }
  return split;
}
g.__pulpFindMaterializedElement__ = function (selector, ancestor) {
  if (typeof selector !== 'string' || selector.length === 0) return null;
  if (g.document && typeof g.document.querySelector === 'function') {
    const browserNode = g.document.querySelector(selector);
    if (browserNode && (!ancestor || materializedClosest(browserNode, ancestor))) {
      return browserNode;
    }
  }
  let targetSelector = selector.trim();
  let effectiveAncestor = ancestor || '';
  let directParentSelector = '';
  const directParts = targetSelector.split(/\\s*>\\s*/).filter(Boolean);
  if (directParts.length > 1) {
    targetSelector = directParts.pop();
    directParentSelector = directParts.pop();
    if (!effectiveAncestor && directParts.length > 0) {
      effectiveAncestor = directParts.join(' > ');
    }
  }
  if (!effectiveAncestor) {
    const split = materializedLastDescendantSplit(targetSelector);
    if (split > 0) {
      effectiveAncestor = targetSelector.slice(0, split).trim();
      targetSelector = targetSelector.slice(split + 1).trim();
    }
  }
  for (const node of materializedDomRegistryValues()) {
    const parent = node && (node.parentElement || node._parentElement || null);
    if (materializedMatches(node, targetSelector) &&
        (!directParentSelector || materializedMatches(parent, directParentSelector)) &&
        (!effectiveAncestor || materializedClosest(
          directParentSelector ? parent : node, effectiveAncestor))) return node;
  }
  return null;
};
g.__pulpActivateMaterializedElement__ = function (selector, eventName, eventData) {
  const node = g.__pulpFindMaterializedElement__(selector);
  if (!node) return false;
  const event = eventName || 'click';
  const callbacks = g.__pulpReactEventCallbacks__;
  const callbackFor = function (candidate) {
    const id = candidate && (candidate.__pulpId || candidate.id);
    return callbacks && typeof callbacks.get === 'function'
      ? callbacks.get(String(id) + ':' + event) : null;
  };
  let target = node;
  let callback = callbackFor(target);
  // DOM events bubble from a semantic leaf (for example an editor canvas)
  // to a handler on its containing interaction surface. Preserve that
  // authored relationship before looking for wrapper-owned descendants.
  if (typeof callback !== 'function') {
    let ancestor = node.parentElement || node._parentElement || null;
    while (ancestor) {
      const ancestorCallback = callbackFor(ancestor);
      if (typeof ancestorCallback === 'function') {
        target = ancestor;
        callback = ancestorCallback;
        break;
      }
      ancestor = ancestor.parentElement || ancestor._parentElement || null;
    }
  }
  // Some DOM-compatible layouts place semantic identity on a wrapper while
  // React's callback lives on a separately materialized child whose native
  // attachment cannot express that browser-only wrapper relationship. This
  // is still ordinary hit testing: use the semantic node's own centre point
  // and choose the smallest callback-bearing surface under it. No generated
  // widget id or product coordinate is involved.
  if (typeof callback !== 'function' &&
      typeof node.getBoundingClientRect === 'function') {
    const rect = node.getBoundingClientRect();
    const left = Number(rect && rect.left);
    const top = Number(rect && rect.top);
    const width = Number(rect && rect.width);
    const height = Number(rect && rect.height);
    if ([left, top, width, height].every(Number.isFinite) &&
        width > 0 && height > 0) {
      const centreX = left + width * 0.5;
      const centreY = top + height * 0.5;
      let bestArea = Infinity;
      for (const candidate of materializedDomRegistryValues()) {
        const candidateCallback = callbackFor(candidate);
        if (typeof candidateCallback !== 'function' ||
            typeof candidate.getBoundingClientRect !== 'function') continue;
        const candidateRect = candidate.getBoundingClientRect();
        const candidateLeft = Number(candidateRect && candidateRect.left);
        const candidateTop = Number(candidateRect && candidateRect.top);
        const candidateWidth = Number(candidateRect && candidateRect.width);
        const candidateHeight = Number(candidateRect && candidateRect.height);
        if (![candidateLeft, candidateTop, candidateWidth, candidateHeight]
              .every(Number.isFinite) || candidateWidth <= 0 ||
            candidateHeight <= 0 || centreX < candidateLeft ||
            centreX > candidateLeft + candidateWidth || centreY < candidateTop ||
            centreY > candidateTop + candidateHeight) continue;
        const area = candidateWidth * candidateHeight;
        if (area < bestArea) {
          bestArea = area;
          target = candidate;
          callback = candidateCallback;
        }
      }
    }
  }
  // Browser hit testing may route an event from a semantic canvas/SVG leaf
  // to a React handler owned by an overlapping interaction surface even when
  // the bounded public-element shim cannot reconstruct that DOM ancestry.
  // Resolve that case geometrically from the authored event point, preferring
  // the smallest handler-bearing surface that contains it. This stays generic
  // and preserves the original React callback instead of teaching the importer
  // about a product-specific context menu.
  if (typeof callback !== 'function' && eventData &&
      Number.isFinite(Number(eventData.clientX)) &&
      Number.isFinite(Number(eventData.clientY))) {
    const pointX = Number(eventData.clientX);
    const pointY = Number(eventData.clientY);
    let bestArea = Infinity;
    for (const candidate of materializedDomRegistryValues()) {
      const candidateCallback = callbackFor(candidate);
      if (typeof candidateCallback !== 'function' ||
          typeof candidate.getBoundingClientRect !== 'function') continue;
      const rect = candidate.getBoundingClientRect();
      const left = Number(rect && rect.left);
      const top = Number(rect && rect.top);
      const width = Number(rect && rect.width);
      const height = Number(rect && rect.height);
      if (![left, top, width, height].every(Number.isFinite) ||
          width <= 0 || height <= 0 || pointX < left || pointX > left + width ||
          pointY < top || pointY > top + height) continue;
      const area = width * height;
      if (area < bestArea) {
        bestArea = area;
        target = candidate;
        callback = candidateCallback;
      }
    }
  }
  // Captured JSX frequently puts semantic identity on a wrapper (for
  // example a span used to anchor a dropdown) and the handler on its child
  // button. This is a last resort when neither ancestry nor authored point
  // geometry identifies the handler: selecting the first descendant before
  // hit testing can route a canvas gesture to an unrelated sibling control.
  if (typeof callback !== 'function') {
    for (const candidate of materializedDomRegistryValues()) {
      let parent = candidate;
      while (parent && parent !== node) {
        parent = parent.parentElement || parent._parentElement || null;
      }
      if (parent !== node) continue;
      const descendantCallback = callbackFor(candidate);
      if (typeof descendantCallback === 'function') {
        target = candidate;
        callback = descendantCallback;
        break;
      }
    }
  }
  if (typeof callback === 'function') {
    callback(Object.assign({ type: event, target: node,
      currentTarget: target }, eventData || {}));
    return true;
  }
  if (event === 'click' && typeof node.click === 'function') {
    node.click();
    return true;
  }
  return false;
};
if (typeof g.__pulpRuntimeImport__ !== 'function') {
  throw new Error('materialized runtime import capability is unavailable');
}
${runtimeDocumentAsset ? `
if (typeof g.__loadAssetSync__ !== 'function') {
  throw new Error('materialized runtime document asset loading is unavailable');
}
const runtimeDocumentAsset = g.__loadAssetSync__(
  ${JSON.stringify(runtimeDocumentAsset)});
if (!runtimeDocumentAsset || !runtimeDocumentAsset.ok ||
    typeof runtimeDocumentAsset.text !== 'string') {
  throw new Error('materialized runtime document asset could not be loaded');
}
g.__pulpRuntimeImport__(runtimeDocumentAsset.text, 'materialized-browser');
` : `g.__pulpRuntimeImport__(${JSON.stringify(sidecar)}, 'materialized-browser');`}
if (g.__pulpRuntimeImportErr__) throw new Error(String(g.__pulpRuntimeImportErr__));
// The captured document may replace its browser-compatible window object
// while materializing.  Re-run an explicitly supplied, idempotent service
// prelude against that final realm so closures used by executable canvas
// programs and event handlers resolve the same services they saw in Chromium.
// This is deliberately a generic lifecycle seam: the transformer neither
// knows nor enumerates any product-specific service names.
${productPrelude ? `(function replayMaterializedProductPrelude(g) {
  const globalThis = g;
  const window = g.window || g;
${productPrelude}
})(g);` : ''}
// Product preludes may publish browser-facing services before web-compat has
// constructed the captured document's final window object. Rebind only the
// explicitly exported service table after bootstrap; this is renderer-neutral
// and avoids teaching the importer product-specific global names.
if (g.window && g.__pulpMaterializedWindowGlobals__ &&
    typeof g.__pulpMaterializedWindowGlobals__ === 'object') {
  for (const [name, value] of Object.entries(g.__pulpMaterializedWindowGlobals__)) {
    g.window[name] = value;
  }
}
// Runtime import materializes the captured document, which is allowed to
// replace document.body while bootstrapping. Create the native behavior root
// only after that replacement so it cannot be detached by the original page.
${surfaceBackground ? `
createCol(surfaceRootId, '');
setPosition(surfaceRootId, 'absolute');
// Chromium paints the document background across the entire viewport, not
// only inside a centered fixed-aspect application frame.  Preserve that
// distinction so native letterbox/gutter pixels inherit the captured page
// surface instead of the screenshot host's fallback color.
setLeft(surfaceRootId, 0);
setTop(surfaceRootId, 0);
setFlex(surfaceRootId, 'width', '100%');
setFlex(surfaceRootId, 'height', '100%');
setBackground(surfaceRootId, ${JSON.stringify(surfaceBackground)});
setPointerEvents(surfaceRootId, 'none');
// This is only the captured page's background plane.  Keep it beneath both
// the DesignIR reconstruction and any Chromium state-atlas paint authority;
// placing it near the behavior plane silently hides the reference image while
// leaving semantic state activation apparently successful.
setZIndex(surfaceRootId, ${MATERIALIZED_BACKGROUND_Z});` : ''}
createCol(behaviorRootId, '');
// Canvas programs read their laid-out CSS boxes while drawing. display:none
// collapses those boxes and produces the exact NaN/zero geometry this import
// is designed to avoid. Reference mode keeps a real authored-size behavior
// surface above the visible DesignIR sibling. Native mode makes this same
// live tree the sole visual authority.
setPosition(behaviorRootId, 'absolute');
setLeft(behaviorRootId, ${JSON.stringify(authoredLeft)});
setTop(behaviorRootId, ${JSON.stringify(authoredTop)});
setFlex(behaviorRootId, 'width', ${JSON.stringify(authoredWidth)});
setFlex(behaviorRootId, 'height', ${JSON.stringify(authoredHeight)});
${authoredTransform ? `
// Chromium resolved layout, typography, Canvas client dimensions, and border
// widths in authored CSS pixels, then transformed the application surface.
// Native must replay those spaces in that order as well. Applying this matrix
// to already-transformed boxes is the clipping bug this contract prevents.
setTransformOrigin(behaviorRootId, 0, 0);
setTransform(behaviorRootId,
  ${JSON.stringify(authoredTransform.a)}, ${JSON.stringify(authoredTransform.b)},
  ${JSON.stringify(authoredTransform.c)}, ${JSON.stringify(authoredTransform.d)},
  ${JSON.stringify(authoredTransform.e)}, ${JSON.stringify(authoredTransform.f)});` : ''}
setVisible(behaviorRootId, true);
setOpacity(behaviorRootId, ${visualAuthority === 'native' ? '1' : '0'});
// The root itself never becomes a catch-all hit target. In reference mode its
// transparent children receive the same pointer geometry over Chromium pixels;
// in native mode those children also paint the complete UI.
setPointerEvents(behaviorRootId, 'box-none');
setZIndex(behaviorRootId, ${MATERIALIZED_BEHAVIOR_Z});
// Commit exactly the element supplied by the captured document's own
// ReactDOM.createRoot(...).render(...) call. A declaration-only export may
// omit that call, so retain one bounded compatibility fallback to App, but
// never mount both paths.
if (capturedRootElement === undefined) {
  if (typeof g.App !== 'function') {
    throw new Error('materialized browser document did not bootstrap or define App');
  }
  capturedRootElement = React.createElement(g.App);
}
activeNativeRoot = new NativeRoot();
activeNativeRoot.render(capturedRootElement);
if (typeof g.__pulpRuntimeSettle__ === 'function') g.__pulpRuntimeSettle__(8);
const capturedStates = ${JSON.stringify(stateAtlas)};
const capturedPaintStates = ${visualAuthority === 'reference'
  ? 'capturedStates' : '[]'};
if (capturedPaintStates.length > 0) {
  createCol(stateAtlasRootId, '');
  setPosition(stateAtlasRootId, 'absolute');
  setLeft(stateAtlasRootId, 0);
  setTop(stateAtlasRootId, 0);
  setFlex(stateAtlasRootId, 'width', ${JSON.stringify(visualWidth)});
  setFlex(stateAtlasRootId, 'height', ${JSON.stringify(visualHeight)});
  setPointerEvents(stateAtlasRootId, 'none');
  setZIndex(stateAtlasRootId, ${MATERIALIZED_STATE_ATLAS_Z});
  for (const state of capturedPaintStates) {
    const id = '__pulp_materialized_state_' + state.id;
    createImage(id, stateAtlasRootId);
    setPosition(id, 'absolute');
    setLeft(id, 0);
    setTop(id, 0);
    setFlex(id, 'width', ${JSON.stringify(visualWidth)});
    setFlex(id, 'height', ${JSON.stringify(visualHeight)});
    setObjectFit(id, 'fill');
    setPointerEvents(id, 'none');
    setVisible(id, false);
  }
}
const loadedCapturedStates = new Set();
const requestedCapturedStateId = ${JSON.stringify(requestedState)};
const requestedCapturedState = requestedCapturedStateId === '' ? null
  : capturedStates.find(state => state.id === requestedCapturedStateId);
if (requestedCapturedStateId !== '' && !requestedCapturedState) {
  throw new Error('requested materialized state is unavailable');
}
let requestedActivationIndex = 0;
let requestedActivationAttempts = 0;
let requestedMatchAttempts = 0;
g.__pulpRequestedMaterializedActivation__ = function () {
  return { state: requestedCapturedStateId, index: requestedActivationIndex,
    attempts: requestedActivationAttempts, matchAttempts: requestedMatchAttempts };
};
function driveRequestedCapturedState() {
  if (!requestedCapturedState ||
      requestedActivationIndex >= requestedCapturedState.activate.length) return;
  const step = requestedCapturedState.activate[requestedActivationIndex];
  const eventData = Object.assign({
    type: step.event,
    preventDefault: function () {},
    stopPropagation: function () {},
    stopImmediatePropagation: function () {},
  }, step.data || {});
  if (g.__pulpActivateMaterializedElement__(
      step.selector, step.event, eventData)) {
    ++requestedActivationIndex;
    requestedActivationAttempts = 0;
    if (typeof g.__pulpRuntimeSettle__ === 'function') g.__pulpRuntimeSettle__(8);
  } else if (++requestedActivationAttempts > 128) {
    throw new Error('could not activate materialized state ' +
      requestedCapturedState.id + ' at step ' + requestedActivationIndex +
      ' selector ' + step.selector);
  }
  requestAnimationFrame(driveRequestedCapturedState);
}
if (requestedCapturedState) requestAnimationFrame(driveRequestedCapturedState);
function resolveCapturedStateFromAtlas() {
  for (let index = capturedStates.length - 1; index >= 0; --index) {
    const state = capturedStates[index];
    if (state.match && g.__pulpFindMaterializedElement__(
        state.match.selector, state.match.ancestor)) return state.id;
  }
  return '';
}
g.__pulpRefreshMaterializedState__ = function () {
  let next = resolveCapturedStateFromAtlas();
  if (requestedCapturedState && requestedActivationIndex >=
      requestedCapturedState.activate.length &&
      requestedCapturedState.match && next !== requestedCapturedState.id) {
    if (++requestedMatchAttempts > 128) {
      throw new Error('materialized state ' + requestedCapturedState.id +
        ' activation completed without satisfying its semantic match');
    }
  } else {
    requestedMatchAttempts = 0;
  }
  if (typeof g.__pulpMaterializedStateResolver__ === 'function') {
    const resolved = g.__pulpMaterializedStateResolver__();
    if (resolved != null) next = String(resolved);
  }
  if (next !== '' && !capturedStates.some((state) => state.id === next)) {
    throw new Error('materialized state resolver returned unknown state ' + next);
  }
  if (next !== activeCapturedState) {
    for (const state of capturedPaintStates) {
      if (state.id === next && !loadedCapturedStates.has(state.id)) {
        setImageSource('__pulp_materialized_state_' + state.id, state.image);
        loadedCapturedStates.add(state.id);
      }
      setVisible('__pulp_materialized_state_' + state.id, state.id === next);
    }
    activeCapturedState = next;
    const state = capturedStates.find(candidate => candidate.id === next);
    activeMaterializedMetadata = state && state.metadata
      ? state.metadata : capturedHomeMetadata;
    applyMaterializedImportMetadata(activeMaterializedMetadata);
  }
  return activeCapturedState;
};
// Native visual authority deliberately has no captured paint planes, but its
// semantic states can still carry Chromium-measured layout and text evidence.
// Resolve once after bootstrap; @pulp/react refreshes again after every later
// commit, when semantic state can actually have changed.
if (capturedStates.length > 0) g.__pulpRefreshMaterializedState__();
const canvasBindings = ${JSON.stringify(canvasBindings)};
const behaviorCanvasAnchors = ${JSON.stringify(behaviorCanvasAnchors)};
const capturedPaintAuthorityAnchors = ${JSON.stringify(capturedPaintAuthorityAnchors)};
function materializedCanvasBehaviorOwnerId(index) {
  const binding = canvasBindings[index];
  const canvases = materializedDomRegistryValues().filter(node =>
    String(node && node.tagName || '').toLowerCase() === 'canvas');
  let candidate = canvases[index] || null;
  const callbackMaps = [g.__pulpReactEventCallbacks__,
    globalThis.__pulpReactEventCallbacks__].filter((map, index, maps) =>
      map instanceof Map && maps.indexOf(map) === index);
  while (candidate) {
    const id = String(candidate.__pulpId || candidate.id || '');
    if (id && callbackMaps.some(callbacks =>
        [...callbacks.keys()].some(key => key.startsWith(id + ':pointer'))))
      return id;
    candidate = candidate.parentElement || candidate._parentElement || null;
  }
  const owners = new Set();
  const eventsByOwner = new Map();
  for (const callbacks of callbackMaps) {
    for (const key of callbacks.keys()) {
      const separator = String(key).lastIndexOf(':');
      if (separator > 0) {
        const id = String(key).slice(0, separator);
        const event = String(key).slice(separator + 1);
        if (!eventsByOwner.has(id)) eventsByOwner.set(id, new Set());
        eventsByOwner.get(id).add(event);
        if (event.startsWith('pointer')) owners.add(id);
      }
    }
  }
  if (owners.size === 1) return [...owners][0];
  const fullGestureOwners = [...eventsByOwner].filter(([, events]) =>
    ['pointerdown', 'pointermove', 'pointerup', 'pointerleave', 'wheel']
      .every(event => events.has(event))).map(([id]) => id);
  if (fullGestureOwners.length === 1) return fullGestureOwners[0];
  if (!binding || !binding.bounds) return '';
  const centerX = binding.bounds.left + binding.bounds.width * 0.5;
  const centerY = binding.bounds.top + binding.bounds.height * 0.5;
  const nodesById = new Map(materializedDomRegistryValues().map(node =>
    [String(node && (node.__pulpId || node.id) || ''), node]));
  const candidates = [...owners].map(id => {
    const node = nodesById.get(id);
    const rect = typeof getLayoutRect === 'function'
      ? getLayoutRect(id)
      : node && typeof node.getBoundingClientRect === 'function'
        ? node.getBoundingClientRect() : null;
    return { id, rect };
  }).filter(({ rect }) => {
    return rect && [rect.left, rect.top, rect.width, rect.height]
      .every(Number.isFinite) && rect.width > 0 && rect.height > 0 &&
      centerX >= rect.left && centerX <= rect.left + rect.width &&
      centerY >= rect.top && centerY <= rect.top + rect.height;
  }).sort((a, b) =>
    a.rect.width * a.rect.height - b.rect.width * b.rect.height ||
    a.id.localeCompare(b.id));
  if (candidates.length === 0) return '';
  if (candidates.length > 1) {
    const firstArea = candidates[0].rect.width * candidates[0].rect.height;
    const secondArea = candidates[1].rect.width * candidates[1].rect.height;
    if (Math.abs(firstArea - secondArea) < 0.01) return '';
  }
  return candidates[0].id;
}
g.__pulpApplyMaterializedVisualAuthority__ = function () {
  if (${JSON.stringify(visualAuthority)} !== 'native') return 0;
  for (const anchor of capturedPaintAuthorityAnchors) {
    if (typeof setVisibleAtAnchor !== 'function' ||
        !setVisibleAtAnchor(anchor, false)) {
      throw new Error('could not retire captured paint authority ' + anchor);
    }
  }
  return capturedPaintAuthorityAnchors.length;
};
g.__pulpBindMaterializedCanvases__ = function () {
  const resolvedOwners = [];
  for (const binding of canvasBindings) {
    const visualAnchor = behaviorCanvasAnchors[binding.index];
    if (!visualAnchor) {
      throw new Error('missing visual anchor for live canvas ' + binding.index);
    }
    const bindType = typeof bindCanvasBehaviorAt;
    const behaviorOwnerId = materializedCanvasBehaviorOwnerId(binding.index);
    resolvedOwners[binding.index] = behaviorOwnerId;
    const bound = bindType === 'function'
      ? bindCanvasBehaviorAt(
          visualAnchor, behaviorRootId, binding.index, behaviorOwnerId) : false;
    if (!bound) {
      throw new Error('could not bind live canvas ' + binding.index +
        ' to ' + visualAnchor + ' bind=' + bindType);
    }
  }
  g.__pulpMaterializedCanvasBehaviorOwners__ = resolvedOwners;
  return canvasBindings.length;
};
syncMaterializedCanvasBehaviorsAfterCommit = function () {
  const resolvedOwners = [];
  const boundCanvases = [];
  // The DesignIR sibling is attached by the product after the initial React
  // commit, so absence here is expected and must not abort the realm. The
  // product performs one strict __pulpBindMaterializedCanvases__ call after
  // attach; subsequent React commits refresh callback snapshots here.
  for (const binding of canvasBindings) {
    const visualAnchor = behaviorCanvasAnchors[binding.index];
    const behaviorOwnerId = materializedCanvasBehaviorOwnerId(binding.index);
    resolvedOwners[binding.index] = behaviorOwnerId;
    boundCanvases[binding.index] = Boolean(visualAnchor &&
      typeof bindCanvasBehaviorAt === 'function' &&
      bindCanvasBehaviorAt(visualAnchor, behaviorRootId, binding.index,
        behaviorOwnerId));
  }
  g.__pulpMaterializedCanvasBehaviorOwners__ = resolvedOwners;
  g.__pulpMaterializedCanvasBindingsReady__ = boundCanvases;
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
