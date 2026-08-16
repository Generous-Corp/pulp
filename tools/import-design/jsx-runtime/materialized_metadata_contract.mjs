// SPDX-License-Identifier: MIT

import {
  normalizeMaterializedFontBindings,
  runtimeFamilyForText,
} from './materialized_font_bindings.mjs';
import { normalizeRequestedTypography } from './materialized_text_contract.mjs';
import {
  normalizeMaterializedCoordinateSpace,
} from '../browser_capture/materialized_coordinate_space.mjs';

const finiteNumber = (value) =>
  typeof value === 'number' && Number.isFinite(value);

const SVG_PRESENTATION_PRIMITIVES = new Set([
  'path', 'rect', 'line', 'circle', 'ellipse', 'polygon', 'polyline',
]);

function normalizePath(path, label) {
  if (!Array.isArray(path) || path.length === 0 || path.length > 256) {
    throw new Error(`${label} path is invalid`);
  }
  return path.map((step, stepIndex) => {
    if (!step || typeof step !== 'object' ||
        typeof step.tag !== 'string' || !/^[a-z][a-z0-9-]*$/.test(step.tag) ||
        !Number.isSafeInteger(step.index) || step.index < 0) {
      throw new Error(`${label} path ${stepIndex} is invalid`);
    }
    return { tag: step.tag, index: step.index };
  });
}

// `runtime_font_family` is already a CSS family-list fragment. In particular,
// materialized custom faces arrive quoted (for example
// `"JetBrains Mono [pulp-materialized-asset-...]"`). JSON-stringifying that
// fragment a second time leaves literal backslashes/quotes in the first family
// name, so SkParagraph cannot match the registered alias and silently falls
// through to a platform sans face.
export function materializedRuntimeFontStack(binding) {
  const requested = String(binding?.basis?.requested?.font_family || '').trim();
  const runtime = String(binding?.runtime_font_family || '').trim();
  if (!runtime) return requested;
  if (!requested) return runtime;
  return `${runtime}, ${requested}`;
}

// Captured UI states are complete snapshots, but persistent chrome can gain a
// richer typography binding in a newer base capture than older state files
// carry. Retain base bindings by structural renderer identity and let the
// active state override the same target. This keeps typography evidence
// monotonic without applying two caches to one Label.
export function materializedMergedTextBindings(baseBindings, stateBindings) {
  const merged = new Map();
  const append = (binding, runtimeOptional = false) => {
    if (!binding || typeof binding !== 'object' || !Array.isArray(binding.path))
      return;
    const key = JSON.stringify([
      binding.anchor || '', binding.path,
      binding.anonymous_text_index === undefined
        ? null : binding.anonymous_text_index,
    ]);
    merged.set(key, runtimeOptional
      ? { ...binding, runtime_optional: true }
      : binding);
  };
  for (const binding of Array.isArray(baseBindings) ? baseBindings : [])
    append(binding, true);
  for (const binding of Array.isArray(stateBindings) ? stateBindings : [])
    append(binding);
  return [...merged.values()];
}

// Translate browser line boxes owned by an HTML element into the generated
// native Label that actually paints them. Yoga may temporarily pixel-round the
// owner and child widths during the first commit (30/28 for a captured
// 29.03125/27.03125 button/content pair). Preserve the measured native insets,
// but apply them to Chromium's fractional owner width; using the transient
// child width as the cache basis makes first paint reject otherwise exact line
// positions and only "heal" after a later React commit.
export function materializedTextTargetGeometry(
    capturedOwnerWidth, targetMetrics, ownerMetrics) {
  const localX = Number(targetMetrics?.localX);
  const localY = Number(targetMetrics?.localY);
  const targetWidth = Number(targetMetrics?.offsetWidth);
  if (![capturedOwnerWidth, localX, localY, targetWidth].every(Number.isFinite) ||
      capturedOwnerWidth <= 0 || targetWidth <= 0) return null;

  let basisWidth = targetWidth;
  const ownerWidth = Number(ownerMetrics?.offsetWidth);
  const ownerBorderLeft = Number(ownerMetrics?.borderLeftWidth);
  const ownerBorderRight = Number(ownerMetrics?.borderRightWidth);
  if ([ownerBorderLeft, ownerBorderRight].every(Number.isFinite) &&
      ownerBorderLeft >= 0 && ownerBorderRight >= 0) {
    const capturedContentWidth =
      capturedOwnerWidth - ownerBorderLeft - ownerBorderRight;
    if (capturedContentWidth > 0) basisWidth = capturedContentWidth;
  } else if (Number.isFinite(ownerWidth) && ownerWidth > 0) {
    const rightInset = ownerWidth - localX - targetWidth;
    const capturedTargetWidth = capturedOwnerWidth - localX - rightInset;
    if (rightInset >= -0.001 && Number.isFinite(capturedTargetWidth) &&
        capturedTargetWidth > 0) basisWidth = capturedTargetWidth;
  }
  return { localX, localY, basisWidth };
}

// Captured child boxes are parent-border-box relative and already include the
// child's resolved margins. Yoga absolute insets are padding-edge relative and
// add the child's margin during layout, so both contributions must be removed
// before replay or each is visibly applied twice.
export function materializedAbsoluteInsets(box, parentMetrics, targetMetrics) {
  const capturedLeft = Number(box?.left);
  const capturedTop = Number(box?.top);
  if (![capturedLeft, capturedTop].every(Number.isFinite)) return null;
  const parentBorderLeft = Number(parentMetrics?.borderLeftWidth);
  const parentBorderTop = Number(parentMetrics?.borderTopWidth);
  const targetMarginLeft = Number(targetMetrics?.marginLeft);
  const targetMarginTop = Number(targetMetrics?.marginTop);
  return {
    left: capturedLeft -
      (Number.isFinite(parentBorderLeft) ? parentBorderLeft : 0) -
      (Number.isFinite(targetMarginLeft) ? targetMarginLeft : 0),
    top: capturedTop -
      (Number.isFinite(parentBorderTop) ? parentBorderTop : 0) -
      (Number.isFinite(targetMarginTop) ? targetMarginTop : 0),
  };
}

// SVG presentation primitives are deliberately excluded from native Yoga
// layout replay: their coordinates belong to the owning <svg> viewport. A
// captured rect therefore keeps its Chromium-resolved parent-local box as the
// rect geometry itself. Resetting x/y to zero would collapse every repeated
// bar onto the viewport origin because no companion layout binding repositions
// the primitive View.
export function materializedSvgRectGeometry(box) {
  if (!box || ![box.left, box.top, box.width, box.height].every(Number.isFinite))
    return null;
  return { x: box.left, y: box.top, width: box.width, height: box.height };
}

export function normalizeMaterializedMetadata(document, label = 'materialized') {
  if (!document || typeof document !== 'object') {
    throw new Error(`${label} metadata document is invalid`);
  }
  const fontBindings = normalizeMaterializedFontBindings(document.font_bindings);
  const coordinateSpace = document.coordinate_space === undefined
    ? null : normalizeMaterializedCoordinateSpace(
      document.coordinate_space, `${label} coordinate space`);
  const layoutBindings = Array.isArray(document.layout_bindings)
    ? document.layout_bindings.map((binding, bindingIndex) => {
        const bindingLabel = `${label} layout binding ${bindingIndex}`;
        if (!binding || typeof binding !== 'object' ||
            (binding.anchor !== '#root' && binding.anchor !== 'body') ||
            !binding.box || !finiteNumber(binding.box.left) ||
            !finiteNumber(binding.box.top) ||
            !finiteNumber(binding.box.width) || binding.box.width < 0 ||
            !finiteNumber(binding.box.height) || binding.box.height < 0) {
          throw new Error(`${bindingLabel} is invalid`);
        }
        return {
          anchor: binding.anchor,
          path: normalizePath(binding.path, bindingLabel),
          box: {
            left: binding.box.left,
            top: binding.box.top,
            width: binding.box.width,
            height: binding.box.height,
          },
        };
      }).filter(binding => !(binding.anchor === 'body' &&
        binding.path.length === 1 && binding.path[0].tag === 'html'))
        // Backward compatibility for already-captured documents: older
        // capture emitted SVG ink bounds into layout_bindings. Never replay
        // those as native Yoga boxes; the owning <svg> viewport provides the
        // primitive's layout and viewBox transform.
        .filter(binding => !SVG_PRESENTATION_PRIMITIVES.has(
          binding.path[binding.path.length - 1].tag)) : [];
  if (layoutBindings.length > 16384) {
    throw new Error(`${label} contains too many layout bindings`);
  }

  const textBindings = Array.isArray(document.text_bindings)
    ? document.text_bindings.map((binding, bindingIndex) => {
        const bindingLabel = `${label} text binding ${bindingIndex}`;
        const requestedTypography = normalizeRequestedTypography(
          binding?.basis?.requested);
        const resolvedFaces = binding?.basis?.resolved_faces;
        if (!binding || typeof binding !== 'object' ||
            (binding.anchor !== '#root' && binding.anchor !== 'body') ||
            typeof binding.text !== 'string' || binding.text.length === 0 ||
            !binding.basis ||
            (binding.anonymous_text_index !== undefined &&
             (!Number.isSafeInteger(binding.anonymous_text_index) ||
              binding.anonymous_text_index < 0)) ||
            !finiteNumber(binding.basis.width) || binding.basis.width <= 0 ||
            typeof binding.basis.resolved_face !== 'string' ||
            binding.basis.resolved_face.length === 0 || !requestedTypography ||
            (resolvedFaces !== undefined &&
             (!Array.isArray(resolvedFaces) || resolvedFaces.length === 0 ||
              resolvedFaces.length > 64)) ||
            !Array.isArray(binding.boxes) || binding.boxes.length === 0 ||
            binding.boxes.length > 4096) {
          throw new Error(`${bindingLabel} is invalid`);
        }
        let previousEnd = 0;
        const boxes = binding.boxes.map((box, boxIndex) => {
          if (!box || typeof box !== 'object' ||
              !finiteNumber(box.left) || !finiteNumber(box.top) ||
              !finiteNumber(box.width) || box.width < 0 ||
              !finiteNumber(box.height) || box.height <= 0 ||
              !Number.isSafeInteger(box.start) || box.start < previousEnd ||
              !Number.isSafeInteger(box.length) || box.length <= 0 ||
              box.start + box.length > binding.text.length) {
            throw new Error(`${bindingLabel} box ${boxIndex} is invalid`);
          }
          previousEnd = box.start + box.length;
          return {
            left: box.left, top: box.top, width: box.width,
            height: box.height, start: box.start, length: box.length,
          };
        });
        const normalized = {
          anchor: binding.anchor,
          path: normalizePath(binding.path, bindingLabel),
          text: binding.text,
          ...(binding.anonymous_text_index === undefined ? {} : {
            anonymous_text_index: binding.anonymous_text_index,
          }),
          basis: {
            width: binding.basis.width,
            resolved_face: binding.basis.resolved_face,
            ...(resolvedFaces === undefined ? {} : {
              resolved_faces: resolvedFaces.map((face, faceIndex) => {
                if (!face || typeof face !== 'object' ||
                    typeof face.family_name !== 'string' ||
                    typeof face.post_script_name !== 'string' ||
                    typeof face.is_custom_font !== 'boolean' ||
                    !Number.isSafeInteger(face.glyph_count) ||
                    face.glyph_count <= 0 ||
                    (!face.family_name && !face.post_script_name)) {
                  throw new Error(
                    `${bindingLabel} resolved face ${faceIndex} is invalid`);
                }
                return {
                  family_name: face.family_name,
                  post_script_name: face.post_script_name,
                  is_custom_font: face.is_custom_font,
                  glyph_count: face.glyph_count,
                };
              }),
            }),
            requested: requestedTypography,
          },
          boxes,
        };
        normalized.runtime_font_family = runtimeFamilyForText(
          fontBindings, normalized);
        return normalized;
      }) : [];
  if (textBindings.length > 4096) {
    throw new Error(`${label} contains too many text bindings`);
  }
  const paintBindings = Array.isArray(document.paint_bindings)
      ? document.paint_bindings.map((binding, bindingIndex) => {
        const bindingLabel = `${label} paint binding ${bindingIndex}`;
        const paint = binding?.paint;
        const box = binding?.box;
        const allowedTags = new Set(['svg', 'path', 'rect', 'line', 'circle']);
        if (!binding || typeof binding !== 'object' ||
            (binding.anchor !== '#root' && binding.anchor !== 'body') ||
            typeof binding.tag !== 'string' || !allowedTags.has(binding.tag) ||
            !paint || typeof paint !== 'object' ||
            typeof paint.color !== 'string' || typeof paint.fill !== 'string' ||
            typeof paint.stroke !== 'string' ||
            !finiteNumber(paint.opacity) || paint.opacity < 0 || paint.opacity > 1 ||
            !finiteNumber(paint.fill_opacity) || paint.fill_opacity < 0 ||
            paint.fill_opacity > 1 || !finiteNumber(paint.stroke_opacity) ||
            paint.stroke_opacity < 0 || paint.stroke_opacity > 1 ||
            !finiteNumber(paint.stroke_width) || paint.stroke_width < 0 ||
            typeof paint.stroke_dasharray !== 'string') {
          throw new Error(`${bindingLabel} is invalid`);
        }
        if (box !== undefined &&
            (!box || typeof box !== 'object' || binding.tag !== 'rect' ||
             !finiteNumber(box.left) || !finiteNumber(box.top) ||
             !finiteNumber(box.width) || box.width < 0 ||
             !finiteNumber(box.height) || box.height < 0)) {
          throw new Error(`${bindingLabel} box is invalid`);
        }
        return {
          anchor: binding.anchor,
          path: normalizePath(binding.path, bindingLabel),
          tag: binding.tag,
          paint: {
            color: paint.color,
            fill: paint.fill,
            stroke: paint.stroke,
            opacity: paint.opacity,
            fill_opacity: paint.fill_opacity,
            stroke_opacity: paint.stroke_opacity,
            stroke_width: paint.stroke_width,
            stroke_dasharray: paint.stroke_dasharray,
          },
          ...(box === undefined ? {} : { box: {
            left: box.left, top: box.top,
            width: box.width, height: box.height,
          } }),
        };
      }) : [];
  if (paintBindings.length > 16384) {
    throw new Error(`${label} contains too many paint bindings`);
  }
  return { coordinate_space: coordinateSpace, font_bindings: fontBindings,
    layout_bindings: layoutBindings, text_bindings: textBindings,
    paint_bindings: paintBindings };
}
