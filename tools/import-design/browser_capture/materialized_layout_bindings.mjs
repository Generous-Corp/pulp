// SPDX-License-Identifier: MIT

// Renderer-neutral element geometry for the live materialized-import lane.
//
// Chromium remains the generation oracle. Its frozen DOMSnapshot supplies the
// final border boxes after CSS intrinsic sizing, SVG metrics, transforms and
// font layout have settled. The native runtime joins these boxes back to the
// executable React tree by structural DOM path after every commit. Dynamic
// nodes that were not present in the captured state remain under Yoga until a
// captured state supplies its own evidence.

const MAX_BINDINGS = 16384;

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
  if (!values.every(Number.isFinite) || values[2] < 0 || values[3] < 0) return null;
  return values;
}

function isMaterializedElement(nodes, strings, node) {
  if (nodes.nodeType?.[node] !== 1) return false;
  const tag = stringAt(strings, nodes.nodeName?.[node]).toLowerCase();
  return tag.length > 0 && !tag.startsWith("::");
}

function buildElementChildren(nodes, strings) {
  const children = new Map();
  const parent = nodes.parentIndex ?? [];
  const type = nodes.nodeType ?? [];
  for (let node = 0; node < parent.length; ++node) {
    if (!isMaterializedElement(nodes, strings, node)) continue;
    const owner = parent[node];
    if (!children.has(owner)) children.set(owner, []);
    children.get(owner).push(node);
  }
  return children;
}

function structuralPath(nodes, strings, elementChildren, owner) {
  const parent = nodes.parentIndex ?? [];
  const type = nodes.nodeType ?? [];
  const names = nodes.nodeName ?? [];
  const reversed = [];
  let current = owner;
  let anchor = "body";
  let anchorNode = -1;
  let guard = 0;
  while (current >= 0 && current < parent.length && guard++ < parent.length) {
    if (type[current] === 1) {
      const id = attributeValue(nodes, strings, current, "id");
      const tag = stringAt(strings, names[current]).toLowerCase();
      if (id === "root") {
        anchor = "#root";
        anchorNode = current;
        break;
      }
      if (tag === "body") {
        anchor = "body";
        anchorNode = current;
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
  return { anchor, anchorNode, path: reversed.reverse() };
}

export function buildMaterializedLayoutBindings(snapshot) {
  const document = snapshot?.documents?.[0];
  const strings = snapshot?.strings ?? [];
  const nodes = document?.nodes ?? {};
  const layout = document?.layout ?? {};
  if (!document || !Array.isArray(layout.nodeIndex) ||
      !Array.isArray(layout.bounds)) return [];

  const elementChildren = buildElementChildren(nodes, strings);
  const layoutByNode = new Map();
  for (let index = 0; index < layout.nodeIndex.length; ++index) {
    const node = Number(layout.nodeIndex[index]);
    const rect = finiteRect(layout.bounds[index]);
    if (Number.isSafeInteger(node) && rect && !layoutByNode.has(node)) {
      layoutByNode.set(node, rect);
    }
  }

  const parent = nodes.parentIndex ?? [];
  const bindings = [];
  const seenPaths = new Set();
  for (const [node, rect] of layoutByNode) {
    if (bindings.length >= MAX_BINDINGS) break;
    if (!isMaterializedElement(nodes, strings, node)) continue;
    const identity = structuralPath(nodes, strings, elementChildren, node);
    if (!identity) continue;
    const pathKey = `${identity.anchor}:${identity.path
      .map((step) => `${step.tag}[${step.index}]`).join("/")}`;
    // DOMSnapshot can report more than one layout row for one rendered
    // element (notably inline fragments). The first row is the element's
    // border box; subsequent rows belong to text/fragments and are handled by
    // the separate captured-line-box stream.
    if (seenPaths.has(pathKey)) continue;
    seenPaths.add(pathKey);

    // Position against the closest element ancestor that owns a box. This is
    // the same coordinate relationship native absolute children consume and
    // remains valid when browser capture used an offset/cropped paint frame.
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
    const origin = ancestorRect ?? [0, 0, 0, 0];
    bindings.push({
      index: bindings.length,
      anchor: identity.anchor,
      path: identity.path,
      box: {
        left: rect[0] - origin[0],
        top: rect[1] - origin[1],
        width: rect[2],
        height: rect[3],
      },
    });
  }
  return bindings;
}
