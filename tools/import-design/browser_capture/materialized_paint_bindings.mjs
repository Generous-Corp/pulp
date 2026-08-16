// SPDX-License-Identifier: MIT

// Resolved paint evidence for executable materialized imports.
//
// Chromium is the CSS cascade oracle. In particular, an SVG primitive authored
// with `stroke="currentColor"` cannot be reproduced by replaying the authored
// token after the DOM has been lowered to native widgets: the winning color may
// come from an external stylesheet or an inherited state selector. DOMSnapshot
// records the final computed values in the same frozen frame as geometry and
// text. This bounded stream joins those values back to the executable native
// tree without making Chromium a runtime dependency.

import {
  buildMaterializedElementChildren,
  isMaterializedElement,
  materializedFiniteRect,
  materializedStringAt,
  materializedStructuralPath,
} from './materialized_layout_bindings.mjs';
import { materializedRectToAuthored } from './materialized_coordinate_space.mjs';

const MAX_BINDINGS = 16384;
const SVG_TAGS = new Set(['svg', 'path', 'rect', 'line', 'circle']);

function finiteCssNumber(value, minimum, maximum) {
  const parsed = Number(String(value ?? '').trim());
  return Number.isFinite(parsed) && parsed >= minimum && parsed <= maximum
    ? parsed : null;
}

function finiteCssPixels(value) {
  const match = String(value ?? '').trim().match(
    /^(-?(?:\d+\.?\d*|\.\d+))px$/i);
  if (!match) return null;
  const parsed = Number(match[1]);
  return Number.isFinite(parsed) && parsed >= 0 && parsed <= 65536
    ? parsed : null;
}

function styleValue(strings, row, indexes, name) {
  const index = indexes.get(name);
  return Number.isInteger(index) && Array.isArray(row)
    ? materializedStringAt(strings, row[index]) : '';
}

function usefulPaint(tag, paint) {
  // Ordinary HTML paint is already replayed by the captured stylesheet. This
  // stream exists for SVG computed paint, whose native primitives otherwise
  // see only authored presentation attributes and cannot resolve the browser
  // cascade/currentColor. Keeping the scope SVG-only also makes unsupported
  // paint fail visibly instead of creating a second partial CSS interpreter.
  return SVG_TAGS.has(tag);
}

export function buildMaterializedPaintBindings(snapshot, coordinateSpace = null) {
  const document = snapshot?.documents?.[0];
  const strings = snapshot?.strings ?? [];
  const styleNames = snapshot?.computedStyleNames ?? [];
  const nodes = document?.nodes ?? {};
  const layout = document?.layout ?? {};
  if (!document || !Array.isArray(layout.nodeIndex) ||
      !Array.isArray(layout.styles) || !Array.isArray(styleNames)) return [];

  const indexes = new Map(styleNames.map((name, index) => [String(name), index]));
  const required = ['color', 'fill', 'stroke', 'opacity', 'fill-opacity',
    'stroke-opacity', 'stroke-width', 'stroke-dasharray'];
  if (!required.every(name => indexes.has(name))) return [];

  const elementChildren = buildMaterializedElementChildren(nodes, strings);
  const layoutByNode = new Map();
  for (let index = 0; index < layout.nodeIndex.length; ++index) {
    const node = Number(layout.nodeIndex[index]);
    const captured = materializedFiniteRect(layout.bounds?.[index]);
    const rect = captured
      ? materializedRectToAuthored(captured, coordinateSpace) : null;
    if (Number.isSafeInteger(node) && rect && !layoutByNode.has(node))
      layoutByNode.set(node, rect);
  }
  const parent = nodes.parentIndex ?? [];
  const seenPaths = new Set();
  const bindings = [];
  for (let layoutIndex = 0; layoutIndex < layout.nodeIndex.length; ++layoutIndex) {
    if (bindings.length >= MAX_BINDINGS) break;
    const node = Number(layout.nodeIndex[layoutIndex]);
    if (!Number.isSafeInteger(node) ||
        !isMaterializedElement(nodes, strings, node)) continue;
    const identity = materializedStructuralPath(
      nodes, strings, elementChildren, node);
    if (!identity) continue;
    if (identity.anchor === 'body' && identity.path.length === 1 &&
        identity.path[0].tag === 'html') continue;
    const pathKey = `${identity.anchor}:${identity.path
      .map(step => `${step.tag}[${step.index}]`).join('/')}`;
    if (seenPaths.has(pathKey)) continue;
    seenPaths.add(pathKey);

    const tag = materializedStringAt(strings, nodes.nodeName?.[node]).toLowerCase();
    const row = layout.styles[layoutIndex];
    const opacity = finiteCssNumber(
      styleValue(strings, row, indexes, 'opacity'), 0, 1);
    const fillOpacity = finiteCssNumber(
      styleValue(strings, row, indexes, 'fill-opacity'), 0, 1);
    const strokeOpacity = finiteCssNumber(
      styleValue(strings, row, indexes, 'stroke-opacity'), 0, 1);
    const strokeWidth = finiteCssPixels(
      styleValue(strings, row, indexes, 'stroke-width'));
    if (opacity === null || fillOpacity === null || strokeOpacity === null ||
        strokeWidth === null) continue;
    const paint = {
      color: styleValue(strings, row, indexes, 'color'),
      fill: styleValue(strings, row, indexes, 'fill'),
      stroke: styleValue(strings, row, indexes, 'stroke'),
      opacity,
      fill_opacity: fillOpacity,
      stroke_opacity: strokeOpacity,
      stroke_width: strokeWidth,
      stroke_dasharray: styleValue(strings, row, indexes, 'stroke-dasharray'),
    };
    if (!usefulPaint(tag, paint)) continue;
    const binding = {
      index: bindings.length,
      anchor: identity.anchor,
      path: identity.path,
      tag,
      paint,
    };
    // A captured SVG primitive already owns Chromium's final border box in
    // the native tree. Retaining authored rect x/y inside that positioned box
    // applies the SVG transform twice and clips small marks. Publish the
    // resolved parent-relative box so native replay can normalize local rect
    // geometry without knowing anything about the source design.
    if (tag === 'rect') {
      const rect = layoutByNode.get(node);
      let ancestor = parent[node] ?? -1;
      let ancestorRect = identity.anchorNode >= 0
        ? layoutByNode.get(identity.anchorNode) : null;
      let guard = 0;
      while (ancestor >= 0 && guard++ < parent.length) {
        const candidate = layoutByNode.get(ancestor);
        if (candidate) {
          ancestorRect = candidate;
          break;
        }
        if (ancestor === identity.anchorNode) break;
        ancestor = parent[ancestor] ?? -1;
      }
      if (rect) {
        const origin = ancestorRect ?? [0, 0, 0, 0];
        binding.box = {
          left: rect[0] - origin[0],
          top: rect[1] - origin[1],
          width: rect[2],
          height: rect[3],
        };
      }
    }
    bindings.push(binding);
  }
  return bindings;
}
