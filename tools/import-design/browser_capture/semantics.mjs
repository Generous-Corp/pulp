// SPDX-License-Identifier: MIT

export const COMPUTED_STYLES = [
  "display",
  "visibility",
  "opacity",
  "position",
  "z-index",
  "background-color",
  "background-image",
  "background-blend-mode",
  "border-top-color",
  "border-right-color",
  "border-bottom-color",
  "border-left-color",
  "border-top-width",
  "border-right-width",
  "border-bottom-width",
  "border-left-width",
  "border-top-style",
  "border-radius",
  "box-shadow",
  "text-shadow",
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
  "line-height",
  "text-transform",
  "text-decoration-line",
  "white-space",
  "cursor",
  "pointer-events",
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

function elementBackendIds(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  if (!nodes) return [];
  const result = [];
  for (let index = 0; index < (nodes.nodeType?.length ?? 0); index++) {
    if (nodes.nodeType[index] === 1) {
      result.push(nodes.backendNodeId?.[index] ?? null);
    }
  }
  return result;
}

function clickableElementIndexes(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  if (!nodes) return [];
  const clickableNodeIndexes = new Set(nodes.isClickable?.index ?? []);
  const result = [];
  let elementIndex = 0;
  for (let nodeIndex = 0;
       nodeIndex < (nodes.nodeType?.length ?? 0);
       nodeIndex++) {
    if (nodes.nodeType[nodeIndex] !== 1) continue;
    if (clickableNodeIndexes.has(nodeIndex)) result.push(elementIndex);
    elementIndex++;
  }
  return result;
}

export async function evaluateSemantics(cdp, snapshot, viewport) {
  const result = await cdp.call("Runtime.evaluate", {
    expression: semanticExpression(clickableElementIndexes(snapshot)),
    returnByValue: true,
  });
  const candidates = result.result?.value ?? [];
  const backendIds = elementBackendIds(snapshot);
  for (const candidate of candidates) {
    candidate.backend_node_id = backendIds[candidate.dom_index] ?? null;
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
    },
    limitations: [
      "Canvas and WebGL internal hit regions are not recoverable from the DOM; " +
      "their host surface is reported as one unresolved candidate when clickable.",
    ],
    candidates,
  };
}
