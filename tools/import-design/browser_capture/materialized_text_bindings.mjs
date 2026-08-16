// SPDX-License-Identifier: MIT

// Renderer-neutral typography evidence for the live materialized-import lane.
//
// Chromium is the generation oracle, not the runtime renderer.  It records
// where each UTF-16 text slice was laid out and which face actually shaped it.
// The native @pulp/react runtime joins this evidence back to the corresponding
// host element by structural DOM path.  Label validates the complete basis
// (text/font/width) before using it; a dynamic edit that invalidates any part of
// that basis falls back to ordinary PreText reflow.

const MAX_BINDINGS = 4096;
const MAX_BOXES_PER_BINDING = 4096;

import { materializedRectToAuthored } from './materialized_coordinate_space.mjs';

function stringAt(strings, index) {
  return Number.isInteger(index) && index >= 0 ? String(strings[index] ?? "") : "";
}

function attributeValue(nodes, strings, nodeIndex, name) {
  const row = nodes.attributes?.[nodeIndex] ?? [];
  for (let index = 0; index + 1 < row.length; index += 2) {
    if (stringAt(strings, row[index]).toLowerCase() === name.toLowerCase()) {
      return stringAt(strings, row[index + 1]);
    }
  }
  return "";
}

function finiteRect(row) {
  if (!Array.isArray(row) || row.length < 4) return null;
  const values = row.slice(0, 4).map(Number);
  if (!values.every(Number.isFinite) || values[2] < 0 || values[3] <= 0) return null;
  return values;
}

function parseCssPixels(value) {
  const match = String(value ?? "").trim().match(/^(-?(?:\d+\.?\d*|\.\d+))px$/i);
  if (!match) return null;
  const parsed = Number(match[1]);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

function parseCssWeight(value) {
  const text = String(value ?? "").trim().toLowerCase();
  if (text === "normal") return 400;
  if (text === "bold") return 700;
  const parsed = Number(text);
  return Number.isFinite(parsed) && parsed >= 1 && parsed <= 1000
    ? Math.round(parsed) : null;
}

function parseCssSlant(value) {
  const text = String(value ?? "").trim().toLowerCase();
  if (text === "normal") return 0;
  if (text === "italic") return 1;
  if (text.startsWith("oblique")) return 2;
  return null;
}

function parseCssLetterSpacing(value) {
  const text = String(value ?? "").trim().toLowerCase();
  if (text === "normal") return 0;
  const match = text.match(/^(-?(?:\d+\.?\d*|\.\d+))px$/i);
  if (!match) return null;
  const parsed = Number(match[1]);
  return Number.isFinite(parsed) && Math.abs(parsed) <= 4096 ? parsed : null;
}

function buildElementChildren(nodes) {
  const children = new Map();
  const parent = nodes.parentIndex ?? [];
  const type = nodes.nodeType ?? [];
  for (let node = 0; node < parent.length; ++node) {
    if (type[node] !== 1) continue;
    const owner = parent[node];
    if (!children.has(owner)) children.set(owner, []);
    children.get(owner).push(node);
  }
  return children;
}

function directTextIndex(nodes, textNode, owner) {
  const parent = nodes.parentIndex ?? [];
  const type = nodes.nodeType ?? [];
  if (type[textNode] !== 3 || parent[textNode] !== owner) return null;
  let ordinal = 0;
  for (let node = 0; node < textNode; ++node) {
    if (type[node] === 3 && parent[node] === owner) ++ordinal;
  }
  return ordinal;
}

function structuralPath(nodes, strings, elementChildren, owner) {
  const parent = nodes.parentIndex ?? [];
  const type = nodes.nodeType ?? [];
  const names = nodes.nodeName ?? [];
  const reversed = [];
  let current = owner;
  let anchor = "body";
  let guard = 0;
  while (current >= 0 && current < parent.length && guard++ < parent.length) {
    if (type[current] === 1) {
      const id = attributeValue(nodes, strings, current, "id");
      const tag = stringAt(strings, names[current]).toLowerCase();
      if (id === "root") {
        anchor = "#root";
        break;
      }
      if (tag === "body") {
        anchor = "body";
        break;
      }
      const siblings = elementChildren.get(parent[current]) ?? [];
      const index = siblings.indexOf(current);
      if (index < 0 || !tag) return null;
      reversed.push({ tag, index });
    }
    current = parent[current] ?? -1;
  }
  if (reversed.length === 0) return null;
  return { anchor, path: reversed.reverse() };
}

function lineBoxesFor(layoutIndex, textBoxes, ownerBounds, coordinateSpace) {
  const layoutIndices = textBoxes.layoutIndex ?? [];
  const bounds = textBoxes.bounds ?? [];
  const starts = textBoxes.start ?? [];
  const lengths = textBoxes.length ?? [];
  const boxes = [];
  for (let index = 0; index < layoutIndices.length; ++index) {
    if (layoutIndices[index] !== layoutIndex) continue;
    const captured = finiteRect(bounds[index]);
    const rect = captured
      ? materializedRectToAuthored(captured, coordinateSpace) : null;
    const start = Number(starts[index]);
    const length = Number(lengths[index]);
    if (!rect || !Number.isSafeInteger(start) || start < 0 ||
        !Number.isSafeInteger(length) || length <= 0) return null;
    boxes.push({
      left: rect[0] - ownerBounds[0],
      top: rect[1] - ownerBounds[1],
      width: rect[2],
      height: rect[3],
      start,
      length,
    });
    if (boxes.length > MAX_BOXES_PER_BINDING) return null;
  }
  return boxes.length > 0 ? boxes : null;
}

function mergeableOwnerRunGroup(group, nodes, elementChildren) {
  if (group.length < 2) return false;
  const owner = Number(group[0]?.owner_node_index);
  if (!Number.isSafeInteger(owner) ||
      (elementChildren.get(owner) ?? []).length !== 0) return false;
  const ordinals = group.map(run =>
    directTextIndex(nodes, Number(run?.node_index), owner));
  if (ordinals.some(index => index === null) ||
      new Set(ordinals).size !== ordinals.length) return false;
  const requested = JSON.stringify(group[0]?.requested ?? {});
  return group.every(run => Number(run?.owner_node_index) === owner &&
    JSON.stringify(run?.requested ?? {}) === requested);
}

function mergeResolvedFaces(group) {
  const first = group[0]?.resolved;
  if (!Array.isArray(first)) return null;
  // CSS.getPlatformFontsForNode reports the complete owner census for every
  // adjacent layout run. Those answers must agree; summing them would double
  // the glyph counts and make the face evidence false.
  const identity = JSON.stringify(first);
  if (!group.every(run => Array.isArray(run?.resolved) &&
      JSON.stringify(run.resolved) === identity)) return null;
  return first.map(face => ({ ...face }));
}

function mergedSingleLineBox(group, textBoxes, ownerBounds, coordinateSpace,
                             textLength) {
  const boxes = group.map(run => lineBoxesFor(
    Number(run.layout_index), textBoxes, ownerBounds, coordinateSpace));
  if (boxes.some(value => !value || value.length !== 1)) return null;
  const lines = boxes.map(value => value[0]);
  const first = lines[0];
  if (lines.some(line => Math.abs(line.top - first.top) > 0.25 ||
      Math.abs(line.height - first.height) > 0.25)) return null;
  for (let index = 1; index < lines.length; ++index) {
    if (lines[index].left + 0.25 < lines[index - 1].left)
      return null;
  }
  const left = Math.min(...lines.map(line => line.left));
  const right = Math.max(...lines.map(line => line.left + line.width));
  const top = Math.min(...lines.map(line => line.top));
  const bottom = Math.max(...lines.map(line => line.top + line.height));
  return [{ left, top, width: right - left, height: bottom - top,
    start: 0, length: textLength }];
}

export function buildMaterializedTextBindings(snapshot, platformFontReport,
                                              coordinateSpace = null) {
  const document = snapshot?.documents?.[0];
  const strings = snapshot?.strings ?? [];
  const nodes = document?.nodes ?? {};
  const layout = document?.layout ?? {};
  const textBoxes = document?.textBoxes ?? {};
  if (!document || !Array.isArray(layout.nodeIndex) ||
      !Array.isArray(platformFontReport?.runs)) return [];

  const elementChildren = buildElementChildren(nodes);
  const layoutIndexByNode = new Map();
  for (let index = 0; index < layout.nodeIndex.length; ++index) {
    if (!layoutIndexByNode.has(layout.nodeIndex[index])) {
      layoutIndexByNode.set(layout.nodeIndex[index], index);
    }
  }

  // Multiple independent runs owned by one element cannot be represented by
  // one native Label cache without attributed-cluster metadata.  Skip them
  // rather than letting the last run silently overwrite the first.
  const ownerCounts = new Map();
  for (const run of platformFontReport.runs) {
    const owner = Number(run?.owner_node_index);
    if (Number.isSafeInteger(owner)) ownerCounts.set(owner, (ownerCounts.get(owner) ?? 0) + 1);
  }

  const runsByOwner = new Map();
  for (const run of platformFontReport.runs) {
    const owner = Number(run?.owner_node_index);
    if (!Number.isSafeInteger(owner)) continue;
    if (!runsByOwner.has(owner)) runsByOwner.set(owner, []);
    runsByOwner.get(owner).push(run);
  }
  const candidateRuns = [];
  for (const group of runsByOwner.values()) {
    if (mergeableOwnerRunGroup(group, nodes, elementChildren)) {
      candidateRuns.push({ ...group[0], __materialized_merged_runs: group });
    } else {
      candidateRuns.push(...group);
    }
  }

  const bindings = [];
  for (const run of candidateRuns) {
    if (bindings.length >= MAX_BINDINGS) break;
    const owner = Number(run?.owner_node_index);
    const layoutIndex = Number(run?.layout_index);
    if (!Number.isSafeInteger(owner) || !Number.isSafeInteger(layoutIndex))
      continue;
    const mergedRuns = Array.isArray(run?.__materialized_merged_runs)
      ? run.__materialized_merged_runs : null;
    const resolved = mergedRuns ? mergeResolvedFaces(mergedRuns)
      : Array.isArray(run?.resolved) ? run.resolved : [];
    if (!resolved) continue;
    // Chromium may report a fallback face for one glyph (for example a star
    // or disclosure marker) even though the requested face shapes the run.
    // Captured line boxes describe the breaks; native shaping still owns the
    // glyphs. Validate the dominant face rather than discarding the complete
    // run merely because fallback participated.
    const dominant = resolved
      .filter(face => Number(face?.glyph_count ?? 0) > 0)
      .sort((a, b) => Number(b.glyph_count) - Number(a.glyph_count))[0];
    const resolvedFace = String(
      dominant?.post_script_name || dominant?.family_name || "");
    if (!resolvedFace) continue;
    const resolvedFaces = resolved
      .filter(face => Number(face?.glyph_count ?? 0) > 0)
      .map(face => ({
        family_name: String(face?.family_name || ""),
        post_script_name: String(face?.post_script_name || ""),
        is_custom_font: face?.is_custom_font === true,
        glyph_count: Number(face.glyph_count),
      }))
      .filter(face => face.family_name || face.post_script_name);
    if (resolvedFaces.length === 0 || resolvedFaces.length > 64) continue;
    const ownerLayoutIndex = layoutIndexByNode.get(owner);
    const capturedOwnerBounds = finiteRect(layout.bounds?.[ownerLayoutIndex]);
    const ownerBounds = capturedOwnerBounds
      ? materializedRectToAuthored(capturedOwnerBounds, coordinateSpace) : null;
    const text = mergedRuns
      ? mergedRuns.sort((a, b) => Number(a.node_index) - Number(b.node_index))
          .map(item => stringAt(strings, layout.text?.[Number(item.layout_index)]))
          .join('')
      : stringAt(strings, layout.text?.[layoutIndex]);
    const identity = structuralPath(nodes, strings, elementChildren, owner);
    if (!ownerBounds || !text || !identity) continue;
    const boxes = mergedRuns
      ? mergedSingleLineBox(mergedRuns, textBoxes, ownerBounds,
          coordinateSpace, text.length)
      : lineBoxesFor(layoutIndex, textBoxes, ownerBounds, coordinateSpace);
    if (!boxes) continue;
    const requested = run?.requested ?? {};
    const fontSize = parseCssPixels(requested.font_size);
    const fontWeight = parseCssWeight(requested.font_weight);
    const fontSlant = parseCssSlant(requested.font_style);
    const letterSpacing = parseCssLetterSpacing(requested.letter_spacing);
    const requestedFamily = String(requested.font_family ?? "").trim();
    if (!requestedFamily || fontSize === null || fontWeight === null ||
        fontSlant === null || letterSpacing === null) continue;
    // A direct text node needs a separate native renderer target only when
    // its owner also contains element children. Pure `<span>text</span>` and
    // `<button>text</button>` already paint through the owner's Label target.
    const anonymousTextIndex = (elementChildren.get(owner) ?? []).length > 0
      ? directTextIndex(nodes, Number(run?.node_index), owner) : null;
    // Several direct text nodes may share a mixed-content owner (for example
    // `<span><svg/>SCULPT ▾</span>`). Each has its own synthetic native Label,
    // so each captured run is independently representable. A multi-run pure
    // Label still cannot express per-run clusters and must fail closed.
    if (ownerCounts.get(owner) !== 1 && anonymousTextIndex === null &&
        !mergedRuns) continue;
    bindings.push({
      index: bindings.length,
      anchor: identity.anchor,
      path: identity.path,
      ...(anonymousTextIndex === null ? {} : {
        anonymous_text_index: anonymousTextIndex,
      }),
      text,
      basis: {
        width: ownerBounds[2],
        resolved_face: resolvedFace,
        resolved_faces: resolvedFaces,
        requested: {
          font_family: requestedFamily,
          font_size: fontSize,
          font_weight: fontWeight,
          font_slant: fontSlant,
          letter_spacing: letterSpacing,
        },
      },
      boxes,
    });
  }
  return bindings;
}
