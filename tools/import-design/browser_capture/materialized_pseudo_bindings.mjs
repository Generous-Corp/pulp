// SPDX-License-Identifier: MIT

// Bounded pseudo-element paint evidence for materialized imports. Chromium
// exposes ::before/::after as DOMSnapshot nodes, but they are not authored
// React elements and therefore cannot be joined to the native tree by path.
// Publish only a renderer-neutral gradient plane. Unsupported generated
// content, images, filters and blend modes remain explicit diagnostics.
import {
  buildMaterializedElementChildren,
  materializedAttributeValue,
  materializedFiniteRect,
  materializedStringAt,
  materializedStructuralPath,
} from './materialized_layout_bindings.mjs';
import { materializedRectToAuthored } from './materialized_coordinate_space.mjs';

const MAX_BINDINGS = 256;
const MAX_DIAGNOSTICS = 1024;
const PSEUDO_NAMES = new Set(['::before', '::after']);

function styleValue(strings, row, indexes, name) {
  const index = indexes.get(name);
  return Number.isInteger(index) && Array.isArray(row)
    ? materializedStringAt(strings, row[index]) : '';
}
function finiteOpacity(value) {
  const number = Number(String(value ?? '').trim());
  return Number.isFinite(number) && number >= 0 && number <= 1 ? number : null;
}
function gradient(value) {
  const text = String(value ?? '').trim();
  if (!text || text === 'none' || text.length > 4096 || /[;{}<>]/.test(text)) return null;
  return /^(?:repeating-)?(?:linear|radial|conic)-gradient\(/i.test(text) ? text : null;
}
function nonTransparentColor(value) {
  const text = String(value ?? '').trim();
  if (!text || text === 'transparent' || /^rgba\([^)]*,\s*0\s*\)$/i.test(text)) return '';
  return text.length <= 256 && !/[;{}<>]/.test(text) ? text : '';
}
function integerOrZero(value) {
  if (String(value ?? '').trim() === 'auto') return 0;
  const n = Number(value);
  return Number.isInteger(n) && n >= -2147483648 && n <= 2147483647 ? n : null;
}

export function buildMaterializedPseudoBindings(snapshot, coordinateSpace = null) {
  const document = snapshot?.documents?.[0];
  const strings = snapshot?.strings ?? [];
  const styleNames = snapshot?.computedStyleNames ?? [];
  const nodes = document?.nodes ?? {};
  const layout = document?.layout ?? {};
  const diagnostics = [];
  const result = [];
  if (!document || !Array.isArray(layout.nodeIndex) ||
      !Array.isArray(layout.bounds) || !Array.isArray(layout.styles)) {
    return { bindings: result, diagnostics };
  }
  const indexes = new Map(styleNames.map((name, index) => [String(name), index]));
  const parent = nodes.parentIndex ?? [];
  const elementChildren = buildMaterializedElementChildren(nodes, strings);
  const addDiagnostic = (node, reason) => {
    if (diagnostics.length < MAX_DIAGNOSTICS)
      diagnostics.push({ node: Number(node), reason });
  };
  for (let layoutIndex = 0; layoutIndex < layout.nodeIndex.length; ++layoutIndex) {
    if (result.length >= MAX_BINDINGS) break;
    const node = Number(layout.nodeIndex[layoutIndex]);
    const name = materializedStringAt(strings, nodes.nodeName?.[node]).toLowerCase();
    if (!Number.isSafeInteger(node) || !PSEUDO_NAMES.has(name)) continue;
    const host = parent[node] ?? -1;
    const hostIdentity = host >= 0
      ? materializedStructuralPath(nodes, strings, elementChildren, host) : null;
    const hostTag = materializedStringAt(strings, nodes.nodeName?.[host]).toLowerCase();
    const hostId = materializedAttributeValue(nodes, strings, host, 'id');
    // Body is the document anchor and has no authored path. The runtime places
    // its plane in the capture surface using the absolute captured box.
    const identity = hostTag === 'body'
      ? { anchor: 'body', path: [], pulp_id: materializedAttributeValue(nodes, strings, host, 'data-pulp-id') }
      : hostId === 'root'
        ? { anchor: '#root', path: [], pulp_id: materializedAttributeValue(nodes, strings, host, 'data-pulp-id') }
        : hostIdentity;
    const captured = materializedFiniteRect(layout.bounds[layoutIndex]);
    const box = captured ? materializedRectToAuthored(captured, coordinateSpace) : null;
    if (!identity || !box) { addDiagnostic(node, 'missing-host-or-box'); continue; }
    const row = layout.styles[layoutIndex];
    const opacity = finiteOpacity(styleValue(strings, row, indexes, 'opacity'));
    const image = styleValue(strings, row, indexes, 'background-image');
    const color = nonTransparentColor(styleValue(strings, row, indexes, 'background-color'));
    const generated = styleValue(strings, row, indexes, 'content').trim();
    const gradientValue = gradient(image);
    const zIndex = integerOrZero(styleValue(strings, row, indexes, 'z-index'));
    const pointerEvents = styleValue(strings, row, indexes, 'pointer-events').trim() || 'none';
    if (generated && generated !== 'none' && generated !== 'normal' && generated !== '""' && generated !== "''")
      addDiagnostic(node, 'generated-content-unsupported');
    if (image && image !== 'none' && !gradientValue)
      addDiagnostic(node, 'background-image-unsupported');
    if (opacity === null) { addDiagnostic(node, 'opacity-invalid'); continue; }
    if (zIndex === null) { addDiagnostic(node, 'z-index-invalid'); continue; }
    if (!gradientValue && !color) { addDiagnostic(node, 'no-supported-paint'); continue; }
    result.push({
      index: result.length,
      anchor: identity.anchor,
      path: identity.path,
      ...(identity.pulp_id ? { pulp_id: identity.pulp_id } : {}),
      pseudo: name.slice(2),
      box: { left: box[0], top: box[1], width: box[2], height: box[3] },
      ...(gradientValue ? { background_gradient: gradientValue } : {}),
      ...(color ? { background_color: color } : {}),
      opacity,
      z_index: zIndex,
      pointer_events: pointerEvents === 'auto' ? 'none' : pointerEvents,
    });
  }
  return { bindings: result, diagnostics };
}
