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
  normalizeMaterializedFontBindings,
  runtimeFamilyForText,
} from './materialized_font_bindings.mjs';
import { normalizeRequestedTypography } from './materialized_text_contract.mjs';
import {
  MATERIALIZED_BACKGROUND_Z,
  MATERIALIZED_STATE_ATLAS_Z,
  MATERIALIZED_BEHAVIOR_Z,
  validateMaterializedLayerContract,
} from './materialized_layer_contract.mjs';
import { resolveMaterializedFrames } from './materialized_frame_contract.mjs';
import { loadMaterializedStateAtlas } from './materialized_state_atlas.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.includes('--help') || args.includes('-h')) {
  console.log(`Usage: materialized-runtime-transform.mjs \\
  --in <materialized-document.json> --design-ir <panel.ir.json> \\
  [--prelude <product-runtime.js>]... \\
  [--visual-authority reference|native] \\
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
const finiteNumber = (value) => typeof value === 'number' && Number.isFinite(value);
const fontBindings = normalizeMaterializedFontBindings(parsed.font_bindings);
const layoutBindings = Array.isArray(parsed.layout_bindings)
  ? parsed.layout_bindings.map((binding, bindingIndex) => {
      if (!binding || typeof binding !== 'object' ||
          (binding.anchor !== '#root' && binding.anchor !== 'body') ||
          !Array.isArray(binding.path) || binding.path.length === 0 ||
          binding.path.length > 256 || !binding.box ||
          !finiteNumber(binding.box.left) || !finiteNumber(binding.box.top) ||
          !finiteNumber(binding.box.width) || binding.box.width < 0 ||
          !finiteNumber(binding.box.height) || binding.box.height < 0) {
        throw new Error(`materialized layout binding ${bindingIndex} is invalid`);
      }
      const path = binding.path.map((step, stepIndex) => {
        if (!step || typeof step !== 'object' ||
            typeof step.tag !== 'string' || !/^[a-z][a-z0-9-]*$/.test(step.tag) ||
            !Number.isSafeInteger(step.index) || step.index < 0) {
          throw new Error(
            `materialized layout binding ${bindingIndex} path ${stepIndex} is invalid`);
        }
        return { tag: step.tag, index: step.index };
      });
      return { anchor: binding.anchor, path, box: {
        left: binding.box.left, top: binding.box.top,
        width: binding.box.width, height: binding.box.height,
      } };
    }) : [];
if (layoutBindings.length > 16384) {
  throw new Error('materialized document contains too many layout bindings');
}
const textBindings = Array.isArray(parsed.text_bindings)
  ? parsed.text_bindings.map((binding, bindingIndex) => {
      const requestedTypography = normalizeRequestedTypography(
        binding?.basis?.requested);
      if (!binding || typeof binding !== 'object' ||
          (binding.anchor !== '#root' && binding.anchor !== 'body') ||
          !Array.isArray(binding.path) || binding.path.length === 0 ||
          binding.path.length > 256 || typeof binding.text !== 'string' ||
          binding.text.length === 0 || !binding.basis ||
          (binding.anonymous_text_index !== undefined &&
           (!Number.isSafeInteger(binding.anonymous_text_index) ||
            binding.anonymous_text_index < 0)) ||
          !finiteNumber(binding.basis.width) || binding.basis.width <= 0 ||
          typeof binding.basis.resolved_face !== 'string' ||
          binding.basis.resolved_face.length === 0 ||
          !requestedTypography ||
          !Array.isArray(binding.boxes) || binding.boxes.length === 0 ||
          binding.boxes.length > 4096) {
        throw new Error(`materialized text binding ${bindingIndex} is invalid`);
      }
      const path = binding.path.map((step, stepIndex) => {
        if (!step || typeof step !== 'object' ||
            typeof step.tag !== 'string' || !/^[a-z][a-z0-9-]*$/.test(step.tag) ||
            !Number.isSafeInteger(step.index) || step.index < 0) {
          throw new Error(
            `materialized text binding ${bindingIndex} path ${stepIndex} is invalid`);
        }
        return { tag: step.tag, index: step.index };
      });
      let previousEnd = 0;
      const boxes = binding.boxes.map((box, boxIndex) => {
        if (!box || typeof box !== 'object' ||
            !finiteNumber(box.left) || !finiteNumber(box.top) ||
            !finiteNumber(box.width) || box.width < 0 ||
            !finiteNumber(box.height) || box.height <= 0 ||
            !Number.isSafeInteger(box.start) || box.start < previousEnd ||
            !Number.isSafeInteger(box.length) || box.length <= 0 ||
            box.start + box.length > binding.text.length) {
          throw new Error(
            `materialized text binding ${bindingIndex} box ${boxIndex} is invalid`);
        }
        previousEnd = box.start + box.length;
        return { left: box.left, top: box.top, width: box.width,
          height: box.height, start: box.start, length: box.length };
      });
      return { anchor: binding.anchor, path, text: binding.text,
        ...(binding.anonymous_text_index === undefined ? {} : {
          anonymous_text_index: binding.anonymous_text_index,
        }),
        basis: { width: binding.basis.width,
          resolved_face: binding.basis.resolved_face,
          requested: requestedTypography }, boxes };
    }) : [];
if (textBindings.length > 4096) {
  throw new Error('materialized document contains too many text bindings');
}
for (const binding of textBindings) {
  binding.runtime_font_family = runtimeFamilyForText(fontBindings, binding);
}
const stateAtlas = loadMaterializedStateAtlas(stateAtlasArg, {
  visualAuthority,
  runtimeBase: portableStateAssets ? dirname(output) : '',
});
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
const runtimeDocument = { ...parsed, html: injectPrelude(parsed.html) };
const sidecar = JSON.stringify(runtimeDocument);

const canvasBindings = Array.isArray(parsed.canvas_bindings)
  ? parsed.canvas_bindings : [];
let behaviorCanvasAnchors = Array.isArray(parsed.behavior_canvas_anchors)
  ? parsed.behavior_canvas_anchors : [];
const ir = JSON.parse(readFileSync(designIrArg, 'utf8'));
// The visible DesignIR root is the full capture surface. The executable
// behavior tree is the authored panel INSIDE that surface. Prefer the explicit
// registration rect; old uncropped captures legitimately fall back to the
// root extent.
const materializedFrames = resolveMaterializedFrames(ir);
const { width: visualWidth, height: visualHeight } = materializedFrames.visual;
const {
  left: authoredLeft, top: authoredTop,
  width: authoredWidth, height: authoredHeight,
} = materializedFrames.behavior;
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
const entry = `
import * as React from 'react';
import { createRoot as createPulpRoot, render as renderPulp, unmount as unmountPulp } from '@pulp/react';

const g = globalThis;
// Snapshot the native renderer bridge before the captured browser document
// installs DOM-shaped globals with overlapping names (notably createCanvas).
// @pulp/react consults this immutable namespace first, while the authored
// browser code continues to see the compatibility functions it expects.
g.__pulpNativeBridgeFunctions__ = Object.freeze({
  createCanvas: g.createCanvas,
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
g.__pulpApplyMaterializedImportMetadata__ = function () {
  const values = materializedDomRegistryValues();
  let applied = 0;
  const diagnostics = {
    layout_applied: 0,
    layout_node_miss: 0,
    text_applied: 0,
    text_node_miss: 0,
    text_content_mismatch: 0,
    text_target_miss: 0,
  };
  // Freeze captured elements to Chromium's resolved parent-relative boxes.
  // This is generation evidence, not a screenshot at runtime: React remains
  // executable and newly-created dynamic nodes still flow through Yoga until
  // their captured state contributes an equivalent binding.
  if (typeof g.setPosition === 'function' && typeof g.setFlex === 'function') {
    for (const binding of capturedLayoutBindings) {
      const node = materializedNodeAtPath(binding, values);
      const id = node && (node.__pulpId || node.id);
      if (!id) {
        ++diagnostics.layout_node_miss;
        continue;
      }
      g.setPosition(String(id), 'absolute');
      g.setLeft(String(id), binding.box.left);
      g.setTop(String(id), binding.box.top);
      g.setFlex(String(id), 'width', binding.box.width);
      g.setFlex(String(id), 'height', binding.box.height);
      ++applied;
      ++diagnostics.layout_applied;
    }
  }
  if (typeof g.setCapturedLineBoxes !== 'function') return applied;
  for (const binding of capturedTextBindings) {
    const node = materializedNodeAtPath(binding, values);
    if (!node) {
      ++diagnostics.text_node_miss;
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
        ++diagnostics.text_content_mismatch;
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
      ++diagnostics.text_content_mismatch;
      continue;
    }
    const id = anonymousTarget?.id ||
      node.__pulpTextTargetId || node.__pulpId || node.id;
    if (!id) {
      ++diagnostics.text_target_miss;
      continue;
    }
    // Generated anonymous Labels do not participate in the authored DOM
    // cascade. Reapply Chromium's computed typography to the actual native
    // paint target rather than letting it inherit the bridge defaults.
    // Pure Label owners receive the same resolved values, making this path
    // deterministic for both HTML controls and ordinary text elements.
    if (typeof g.setFontFamily === 'function') {
      const capturedFamily = binding.runtime_font_family;
      g.setFontFamily(String(id), capturedFamily
        ? JSON.stringify(capturedFamily) + ', ' + binding.basis.requested.font_family
        : binding.basis.requested.font_family);
    }
    if (typeof g.setFontSize === 'function')
      g.setFontSize(String(id), binding.basis.requested.font_size);
    if (typeof g.setFontWeight === 'function')
      g.setFontWeight(String(id), binding.basis.requested.font_weight);
    if (typeof g.setFontStyle === 'function')
      g.setFontStyle(String(id), binding.basis.requested.font_slant === 1
        ? 'italic' : binding.basis.requested.font_slant === 2
          ? 'oblique' : 'normal');
    g.setCapturedLineBoxes(String(id), binding.boxes, binding.basis.width,
      binding.basis.resolved_face, false);
    ++applied;
    ++diagnostics.text_applied;
  }
  g.__pulpMaterializedMetadataDiagnostics__ = diagnostics;
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
    } else if (/\s/.test(ch) && bracketDepth === 0) {
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
  const directParts = targetSelector.split(/\s*>\s*/).filter(Boolean);
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
  // Captured JSX frequently puts semantic identity on a wrapper (for
  // example a span used to anchor a dropdown) and the handler on its child
  // button. Activate the original nearest descendant handler instead of
  // inventing a second product-specific selector.
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
g.__pulpRuntimeImport__(${JSON.stringify(sidecar)}, 'materialized-browser');
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
let activeCapturedState = '';
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
  }
  return activeCapturedState;
};
function refreshCapturedStateFrame() {
  g.__pulpRefreshMaterializedState__();
  requestAnimationFrame(refreshCapturedStateFrame);
}
if (capturedPaintStates.length > 0) refreshCapturedStateFrame();
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
