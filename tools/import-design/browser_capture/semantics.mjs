// SPDX-License-Identifier: MIT

// The properties the capture asks Chromium to resolve, in request order.
//
// A property absent from this list is one no consumer can ever draw: the box
// arrives with the appearance silently defaulted, which renders as a plausible
// wrong picture rather than as an error. The list is therefore sized for
// reproducing a whole panel, not for describing a control -- collecting a
// property costs one string per node, while re-capturing a corpus to add one
// later costs every design.
//
// Order is load-bearing in a narrow sense only: `layout.styles` rows are
// positional, so the request order is written into the snapshot as
// `computedStyleNames` and every consumer keys by name from there. Inserting a
// property anywhere in this list is safe; hardcoding a parallel copy is not.
export const COMPUTED_STYLES = [
  "display",
  "visibility",
  "opacity",
  "position",
  "z-index",
  "background-color",
  "background-image",
  "background-blend-mode",
  // A gradient with a hard stop, tiled, is the standard CSS grid idiom. The
  // repeat length lives entirely in background-size: without it the same
  // declaration lowers to one hairline instead of eight columns, and nothing
  // downstream can tell that the value was never read rather than default.
  "background-size",
  "background-position",
  "background-repeat",
  "background-clip",
  "background-origin",
  "border-top-color",
  "border-right-color",
  "border-bottom-color",
  "border-left-color",
  "border-top-width",
  "border-right-width",
  "border-bottom-width",
  "border-left-width",
  // All four edges. Reading only the top edge and applying it to the box makes
  // a dashed left border vanish, and makes a mixed-edge box uniformly wrong in
  // a way that looks like a paint bug rather than a missing input.
  "border-top-style",
  "border-right-style",
  "border-bottom-style",
  "border-left-style",
  "border-radius",
  "box-shadow",
  "text-shadow",
  "outline-color",
  "outline-style",
  "outline-width",
  "outline-offset",
  "filter",
  "backdrop-filter",
  "transform",
  "transform-origin",
  "overflow",
  "clip-path",
  "mask-image",
  "mix-blend-mode",
  "isolation",
  "color",
  "font-family",
  "font-size",
  "font-weight",
  "font-style",
  "font-variation-settings",
  "text-align",
  "letter-spacing",
  "word-spacing",
  "line-height",
  "text-transform",
  "text-decoration-line",
  "text-decoration-color",
  "text-decoration-style",
  "text-decoration-thickness",
  "text-underline-offset",
  "white-space",
  // Generated content. The snapshot reports ::before / ::after as their own
  // nodes with their own layout entries, so the text they inject is only
  // recoverable through this property -- there is no DOM text node to read.
  "content",
  "cursor",
  "pointer-events",
  // SVG paint. An inline `<svg>` icon carries its colour three different ways —
  // a presentation attribute, a stylesheet rule, or `currentColor` inherited
  // from the box around it — and only the browser knows which one won. Reading
  // the authored attribute back off the DOM gets a CSS-styled icon wrong and
  // gets `currentColor` wrong every time, so the resolved value is the only
  // usable input. `fill` / `stroke` are `none` on a non-SVG box, which costs a
  // shared string.
  "fill",
  "fill-opacity",
  "fill-rule",
  "stroke",
  "stroke-opacity",
  "stroke-width",
  // Not drawn by the vector lowering, but READ by it: a dashed or dotted stroke
  // rendered solid is a wrong picture, so its presence sends the subtree to the
  // element fallback instead.
  "stroke-dasharray",
];

export function semanticExpression(snapshotClickableIndexes = []) {
  return `(() => {
  const elements = [...document.querySelectorAll('*')];
  const snapshotClickable = new Set(
    ${JSON.stringify(snapshotClickableIndexes)});
  const roleKinds = new Map([
    ['button', 'button'],
    ['checkbox', 'toggle'],
    ['combobox', 'select'],
    ['link', 'action'],
    ['menuitem', 'action'],
    ['radio', 'toggle'],
    ['slider', 'slider'],
    ['spinbutton', 'number_input'],
    ['switch', 'toggle'],
    ['textbox', 'text_input']
  ]);
  const nativeKind = element => {
    const tag = element.tagName.toLowerCase();
    const type = (element.getAttribute('type') || '').toLowerCase();
    if (tag === 'button') return 'button';
    if (tag === 'select') return 'select';
    if (tag === 'textarea') return 'text_input';
    if (tag === 'a' && element.hasAttribute('href')) return 'action';
    if (tag !== 'input') return '';
    if (type === 'range') return 'slider';
    if (type === 'checkbox' || type === 'radio') return 'toggle';
    if (type === 'button' || type === 'submit' || type === 'reset') return 'button';
    if (type === 'number') return 'number_input';
    return 'text_input';
  };
  const dataPulp = element => {
    const result = {};
    for (const attribute of element.attributes) {
      if (attribute.name.startsWith('data-pulp-')) {
        result[attribute.name.slice('data-pulp-'.length)] = attribute.value;
      }
    }
    return result;
  };
  // The painted control box, when the author marked it with data-pulp-paint.
  // Null when unmarked: the component box is NOT a safe fallback for value
  // geometry, so a consumer must decide what to do rather than be handed a
  // plausible wrong rectangle. Bounds are page coordinates, same frame as the
  // candidate's own, so the two are directly comparable.
  const paintBox = element => {
    try {
      const painted = element.querySelector('[data-pulp-paint]');
      if (!painted) return null;
      const box = painted.getBoundingClientRect();
      return {
        left: box.left + window.scrollX,
        top: box.top + window.scrollY,
        width: box.width,
        height: box.height
      };
    } catch (e) {
      return null;
    }
  };
  // The design's OWN pointer, when the author marked it with
  // data-pulp-indicator. Same contract as paintBox above and for the same
  // reason: only the author knows which child is the pointer, and inferring it
  // (the thinnest child, the one whose class says "needle") would hard-code one
  // design system's vocabulary into the importer.
  //
  // Null when unmarked, which is the common case and NOT a defect: a design
  // that draws no pointer gets the widget's derived tick instead. Bounds are
  // page coordinates, the same frame as the candidate's own.
  const indicatorBox = element => {
    try {
      const marked = element.querySelector('[data-pulp-indicator]');
      if (!marked) return null;
      const box = marked.getBoundingClientRect();
      return {
        left: box.left + window.scrollX,
        top: box.top + window.scrollY,
        width: box.width,
        height: box.height
      };
    } catch (e) {
      return null;
    }
  };
  // The pointer's resolved colour, read the same way the accent is: from
  // computed style, so a value set anywhere up the tree arrives resolved.
  // Border first, because a hairline pointer is usually a border rather than a
  // fill, and a transparent fill would otherwise win.
  const indicatorColor = element => {
    try {
      const marked = element.querySelector('[data-pulp-indicator]');
      if (!marked) return '';
      const style = window.getComputedStyle(marked);
      const bg = style.getPropertyValue('background-color').trim();
      if (bg && bg !== 'rgba(0, 0, 0, 0)' && bg !== 'transparent') return bg;
      return style.getPropertyValue('border-top-color').trim();
    } catch (e) {
      return '';
    }
  };
  // The accent this control is actually drawn in, resolved by the browser.
  //
  // Reading the PACK's accent token instead is wrong whenever a panel scopes or
  // overrides its palette -- which a good one does -- because the token set is
  // the pack default, not what this control ended up. That mismatch paints a
  // green value arc onto an orange knob: our render comes out worse than the
  // browser's, and every pixel gate still passes because the arc is ours alone.
  //
  // --accent-ring first: a pack that distinguishes the ring from the general
  // accent means the ring colour for the ring. Computed style is what makes
  // this correct -- it resolves inheritance and inline overrides alike, so an
  // accent set anywhere up the tree arrives here already resolved.
  const accentColor = element => {
    try {
      const painted = element.querySelector('[data-pulp-paint]') || element;
      const style = window.getComputedStyle(painted);
      const ring = style.getPropertyValue('--accent-ring').trim();
      if (ring) return ring;
      return style.getPropertyValue('--accent').trim();
    } catch (e) {
      return '';
    }
  };
  // A design-system component states what it is in its class. A pulp-knob is a
  // styled div whose drag handlers sit on an ancestor, so it has no native tag,
  // no ARIA role and no inline handler -- every signal below walks past it, and
  // a six-knob sampler imported as eleven candidates that were all buttons.
  //
  // NOTE: this whole region is inside the template literal returned by
  // semanticExpression() and is evaluated IN THE PAGE. No backticks, and no
  // backslash escapes: the template literal eats them, so a regex written with
  // one arrives stripped. Splitting on a plain space is deliberate -- a class
  // attribute is space separated and needs no escape.
  const componentKinds = new Map([
    ['pulp-knob', 'knob'], ['pulp-fader', 'fader'], ['pulp-slider', 'slider'],
    ['pulp-range', 'range'], ['pulp-switch', 'toggle'], ['pulp-check', 'toggle'],
    ['pulp-radio', 'radio'], ['pulp-stepper', 'stepper'],
    ['pulp-numbox', 'number_input'], ['pulp-numfield', 'number_input'],
    ['pulp-valedit', 'number_input'], ['pulp-meter', 'meter'],
    ['pulp-barmeter', 'meter'], ['pulp-ledmeter', 'meter'],
    ['pulp-spectrum', 'meter'], ['pulp-scope', 'meter'],
    ['pulp-progress', 'meter'], ['pulp-tab', 'tab'],
    ['pulp-combo', 'select'], ['pulp-select', 'select'],
  ]);
  const componentKind = element => {
    try {
      const raw = element && element.getAttribute ? element.getAttribute('class') : '';
      if (!raw) return '';
      const parts = String(raw).split(' ');
      for (let i = 0; i < parts.length; i++) {
        const hit = componentKinds.get(parts[i]);
        if (hit) return hit;
      }
    } catch (e) { return ''; }
    return '';
  };
  const strongSignal = element => {
    const data = dataPulp(element);
    const role = (element.getAttribute('role') || '').toLowerCase();
    return Object.keys(data).length > 0 || nativeKind(element) || componentKind(element) ||
      roleKinds.has(role) || element.hasAttribute('onclick') ||
      element.hasAttribute('onpointerdown') ||
      element.hasAttribute('onmousedown') ||
      element.hasAttribute('onwheel') ||
      element.hasAttribute('onkeydown') ||
      typeof element.onclick === 'function' ||
      typeof element.onpointerdown === 'function' ||
      typeof element.onmousedown === 'function' ||
      typeof element.onwheel === 'function' ||
      typeof element.onkeydown === 'function';
  };
  const strongAncestor = element => {
    for (let parent = element.parentElement; parent; parent = parent.parentElement) {
      if (strongSignal(parent)) return true;
    }
    return false;
  };
  const candidates = [];
  for (let domIndex = 0; domIndex < elements.length; domIndex++) {
    const element = elements[domIndex];
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0.001 ||
        rect.width <= 0.25 || rect.height <= 0.25) continue;

    const data = dataPulp(element);
    const role = (element.getAttribute('role') || '').toLowerCase();
    const native = nativeKind(element);
    const explicitKind = data.kind || '';
    const roleKind = roleKinds.get(role) || '';
    const eventNames = [
      'onclick', 'onpointerdown', 'onmousedown', 'onwheel', 'onkeydown'
    ];
    const hasEvent = eventNames.some(name =>
      element.hasAttribute(name) || typeof element[name] === 'function');
    const snapshotClick = snapshotClickable.has(domIndex);
    const interactiveCursor = [
      'pointer', 'grab', 'grabbing', 'ew-resize', 'ns-resize',
      'col-resize', 'row-resize'
    ].includes(style.cursor);
    const pointerRoot = interactiveCursor &&
      (!element.parentElement ||
       getComputedStyle(element.parentElement).cursor !== style.cursor);
    // NOTE: this inline predicate -- not strongSignal() above, which only feeds
    // strongAncestor -- is the actual admission gate. A recogniser added to the
    // function alone has no effect; that cost one silent no-op round.
    const component = componentKind(element);
    const strong = Object.keys(data).length > 0 || native || roleKind ||
      component || hasEvent || snapshotClick;
    if (!strong && (!pointerRoot || strongAncestor(element))) continue;

    const evidence = [];
    if (Object.keys(data).length) {
      evidence.push({
        kind: 'data-pulp',
        value: JSON.stringify(data),
        strength: 'strong'
      });
    }
    if (native) {
      evidence.push({
        kind: 'native-element',
        value: element.tagName.toLowerCase() +
          (element.getAttribute('type') ? ':' + element.getAttribute('type') : ''),
        strength: 'strong'
      });
    }
    if (roleKind) {
      evidence.push({ kind: 'aria-role', value: role, strength: 'strong' });
    }
    if (hasEvent) {
      evidence.push({ kind: 'event-handler', value: 'present', strength: 'medium' });
    }
    if (snapshotClick) {
      evidence.push({
        kind: 'dom-snapshot-clickable',
        value: 'true',
        strength: 'medium'
      });
    }
    if (!strong && pointerRoot) {
      evidence.push({ kind: 'cursor', value: 'pointer', strength: 'weak' });
    }

    const kind = explicitKind || native || roleKind || component || 'unknown';
    const conflicts = [];
    if (explicitKind && native && explicitKind !== native) {
      conflicts.push('explicit kind differs from native element kind');
    }
    const name = element.getAttribute('aria-label') ||
      element.getAttribute('title') ||
      (element.innerText || element.textContent || '').replace(/\\s+/g, ' ').trim()
        .slice(0, 200);
    // A component class is as declarative as an ARIA role -- the author named
    // the control -- so it scores alongside one rather than falling to the
    // weak-signal floor, which is where these landed when the recogniser fed
    // the gate and the kind but not the score.
    const confidence = explicitKind ? 1 :
      native ? 0.95 : roleKind ? 0.9 : component ? 0.9 : hasEvent ? 0.7 : 0.35;
    candidates.push({
      id: 'browser-node:' + domIndex,
      dom_index: domIndex,
      backend_node_id: null,
      source_node_id: data.node || element.id || '',
      tag: element.tagName.toLowerCase(),
      role,
      name,
      kind,
      bounds: {
        left: rect.left + window.scrollX,
        top: rect.top + window.scrollY,
        width: rect.width,
        height: rect.height
      },
      // The box a live widget must paint its value geometry into, which is NOT
      // the component box: a knob's component includes its caption (measured
      // 116x139.9 where the dial is 116x116), and a fader's track is inset and
      // far narrower (633,274.9,26x150 vs 643,274.9,6x150). Painting a value
      // ring into the component box puts it ~12px below the dial centre -- it
      // renders, it looks almost right, and no gate can see it.
      //
      // The author declares it, exactly as they declare the binding, because
      // only the author knows which child is the painted control. Inferring it
      // would hard-code one design system's class vocabulary into the importer.
      paint_bounds: paintBox(element),
      // The design's own pointer geometry, when it declared one. The lowering
      // turns it into the same knob_ind_* vocabulary the Figma lane already
      // writes, so the consumer has ONE name for the thing rather than one per
      // importer.
      indicator_bounds: indicatorBox(element),
      indicator_color: indicatorColor(element),
      accent: accentColor(element),
      data_pulp: data,
      evidence,
      confidence,
      resolved: kind !== 'unknown',
      binding_status: data.param || data.meter || data.action ? 'bound' : 'unbound',
      conflicts
    });
  }
  return candidates;
})()`;
}

// The snapshot node indexes that correspond, one for one and in order, to the
// elements `document.querySelectorAll('*')` returns in the page. Everything a
// candidate carries from the snapshot -- its backend id, its paint order -- is
// looked up through this correspondence, so a single extra node shifts every
// later element onto its neighbour's data.
//
// nodeType === 1 alone is NOT that correspondence. The snapshot also emits
// pseudo-element boxes (::before / ::after) as element nodes, and
// querySelectorAll cannot return them, so one ::before anywhere on the page
// re-points every control after it at the node before its own -- a control
// that still resolves, still has plausible data, and is simply the wrong node.
// Shadow-tree content is excluded for the same reason: querySelectorAll does
// not pierce a shadow root.
//
// parentIndex is emitted in document order, so a node's parent always appears
// before it and the shadow-tree flag is settled by the time a child is read.
function snapshotElementNodes(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  if (!nodes) return [];
  const total = nodes.nodeType?.length ?? 0;
  const pseudoNodes = new Set(nodes.pseudoType?.index ?? []);
  const shadowRoots = new Set(nodes.shadowRootType?.index ?? []);
  const parents = nodes.parentIndex ?? [];
  const inShadowTree = new Array(total).fill(false);
  const result = [];
  for (let index = 0; index < total; index++) {
    const parent = parents[index] ?? -1;
    inShadowTree[index] = shadowRoots.has(index) ||
      (parent >= 0 && parent < index && inShadowTree[parent]);
    if (nodes.nodeType[index] !== 1) continue;
    if (pseudoNodes.has(index) || inShadowTree[index]) continue;
    result.push(index);
  }
  return result;
}

function elementBackendIds(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  if (!nodes) return [];
  return snapshotElementNodes(snapshot).map(
    (index) => nodes.backendNodeId?.[index] ?? null);
}

// Lowercased element names in the same order, so a candidate can be checked
// against the node it was matched to instead of trusting the correspondence.
function elementTagNames(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  const strings = snapshot.strings;
  if (!nodes || !Array.isArray(strings)) return [];
  return snapshotElementNodes(snapshot).map((index) => {
    const name = strings[nodes.nodeName?.[index]];
    return typeof name === "string" ? name.toLowerCase() : "";
  });
}

// Chromium's own answer to "what paints on top of what", one integer per laid
// out node, taken from the paint order the capture already requests with
// includePaintOrder.
//
// This is consumed, never recomputed. Re-deriving order from z-index is the
// same mistake as re-solving layout from computed styles: stacking contexts,
// opacity, transforms, filters, will-change and position all create or escape
// contexts, so a z-index sort agrees with Chromium on simple pages and diverges
// silently on exactly the layered panels this exists for.
//
// Returns an array parallel to the element ordering the candidate walk uses,
// holding null for elements with no layout entry.
function elementPaintOrders(snapshot) {
  const document = snapshot.documents?.[0];
  const nodes = document?.nodes;
  const layout = document?.layout;
  if (!nodes) return [];
  const nodeIndex = layout?.nodeIndex;
  const paintOrders = layout?.paintOrders;
  // A capture that asked for paint order and did not get it must not resolve to
  // a tree of nulls that reads as "this page has no layering". Chromium omits
  // paintOrders only when includePaintOrder was not requested, so a laid out
  // document without it means the request and the reader have drifted apart.
  if (Array.isArray(nodeIndex) && nodeIndex.length > 0) {
    if (!Array.isArray(paintOrders)) {
      const error = new Error(
        "DOMSnapshot returned layout without paintOrders; the capture must " +
        "request includePaintOrder");
      error.code = "capture-paint-order-missing";
      throw error;
    }
    if (paintOrders.length !== nodeIndex.length) {
      const error = new Error(
        `DOMSnapshot paintOrders length ${paintOrders.length} does not match ` +
        `layout nodeIndex length ${nodeIndex.length}`);
      error.code = "capture-paint-order-mismatch";
      throw error;
    }
  }
  // Layout entries are not one per node: a box that also lays out an inline
  // text box contributes two entries for the same node index (a ::before with
  // generated text does exactly this). The first entry is the node's own box,
  // so keeping it makes the choice defined rather than last-write-wins.
  const orderByNode = new Map();
  for (let entry = 0; entry < (nodeIndex?.length ?? 0); entry++) {
    const order = paintOrders[entry];
    if (typeof order !== "number") continue;
    if (!orderByNode.has(nodeIndex[entry])) {
      orderByNode.set(nodeIndex[entry], order);
    }
  }
  return snapshotElementNodes(snapshot).map(
    (index) => (orderByNode.has(index) ? orderByNode.get(index) : null));
}

function clickableElementIndexes(snapshot) {
  const clickableNodeIndexes = new Set(
    snapshot.documents?.[0]?.nodes?.isClickable?.index ?? []);
  const result = [];
  snapshotElementNodes(snapshot).forEach((nodeIndex, elementIndex) => {
    if (clickableNodeIndexes.has(nodeIndex)) result.push(elementIndex);
  });
  return result;
}

export async function evaluateSemantics(cdp, snapshot, viewport) {
  const result = await cdp.call("Runtime.evaluate", {
    expression: semanticExpression(clickableElementIndexes(snapshot)),
    returnByValue: true,
  });
  const candidates = result.result?.value ?? [];
  const backendIds = elementBackendIds(snapshot);
  const paintOrders = elementPaintOrders(snapshot);
  const tagNames = elementTagNames(snapshot);
  for (const candidate of candidates) {
    // The page walk and the snapshot walk have to agree on which element index
    // N is, and when they disagree every value below is a neighbour's. The
    // element name is the cheap witness both sides already carry, so the
    // mismatch surfaces here rather than as a control wearing another node's
    // paint order.
    const snapshotTag = tagNames[candidate.dom_index];
    if (snapshotTag && candidate.tag && snapshotTag !== candidate.tag) {
      const error = new Error(
        `page element ${candidate.dom_index} is <${candidate.tag}> but the ` +
        `DOM snapshot has <${snapshotTag}> at that index; the element walks ` +
        "have drifted apart");
      error.code = "capture-node-alignment-mismatch";
      throw error;
    }
    candidate.backend_node_id = backendIds[candidate.dom_index] ?? null;
    // Explicitly null rather than absent when the element has no layout entry,
    // so a consumer can distinguish "not painted" from "order not collected".
    candidate.paint_order = paintOrders[candidate.dom_index] ?? null;
    candidate.source_index = candidate.dom_index;
    delete candidate.dom_index;
  }
  const resolved = candidates.filter((candidate) => candidate.resolved).length;
  return {
    schema: "pulp-browser-semantics-v1",
    version: 1,
    viewport,
    summary: {
      candidates: candidates.length,
      resolved,
      unresolved: candidates.length - resolved,
      bound: candidates.filter(
        (candidate) => candidate.binding_status === "bound").length,
      conflicted: candidates.filter(
        (candidate) => candidate.conflicts.length > 0).length,
      // Visible in the artifact so a wholesale paint-order miss shows up as a
      // count of zero next to a non-zero candidate count, rather than as a
      // field every reader quietly treats as optional.
      paint_ordered: candidates.filter(
        (candidate) => candidate.paint_order !== null).length,
    },
    limitations: [
      "Canvas and WebGL internal hit regions are not recoverable from the DOM; " +
      "their host surface is reported as one unresolved candidate when clickable.",
    ],
    candidates,
  };
}
