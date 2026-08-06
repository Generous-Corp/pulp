// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  COMPUTED_STYLES,
  evaluateSemantics,
  semanticExpression,
} from "./semantics.mjs";

test("the capture collects the properties a whole panel needs to be drawn",
  () => {
    // Each of these is a property whose absence renders as a plausible wrong
    // picture rather than an error, so nothing downstream can report the miss.
    for (const property of [
      // A tiled gradient with a hard stop is the standard CSS grid idiom; the
      // repeat length lives only here, so without it eight columns collapse to
      // one hairline.
      "background-size",
      "background-position",
      "background-repeat",
      "background-clip",
      "background-origin",
      // All four edges: reading only the top makes a dashed left border vanish.
      "border-top-style",
      "border-right-style",
      "border-bottom-style",
      "border-left-style",
      "outline-color",
      "outline-style",
      "outline-width",
      "outline-offset",
      "text-decoration-color",
      "text-decoration-style",
      "text-decoration-thickness",
      "text-underline-offset",
      "word-spacing",
      // ::before / ::after inject text that exists in no DOM text node.
      "content",
      // An inline <svg> icon's colour is set three different ways — a
      // presentation attribute, a stylesheet rule, or `currentColor` inherited
      // from the box around it — and only the browser knows which one won.
      // Without these, the geometry arrives with no colour to draw it in.
      "fill",
      "fill-opacity",
      "fill-rule",
      "stroke",
      "stroke-opacity",
      "stroke-width",
      // Read but not drawn: a dashed stroke rendered solid is a wrong picture,
      // so its presence sends the subtree to the element fallback instead.
      "stroke-dasharray",
    ]) {
      assert.ok(COMPUTED_STYLES.includes(property),
        `${property} must be collected; a property the capture skips is one ` +
        "no consumer can draw");
    }
    assert.equal(new Set(COMPUTED_STYLES).size, COMPUTED_STYLES.length,
      "a duplicated property wastes a style column on every node");
  });

test("candidates carry the paint order Chromium reported", async () => {
  // Node 1 and node 3 are laid out; node 2 is not. Element indexes are the
  // nodeType === 1 positions, so they are 0, 1 and 2 respectively.
  const snapshot = {
    documents: [{
      nodes: {
        nodeType: [9, 1, 1, 1],
        backendNodeId: [10, 20, 30, 40],
      },
      layout: {
        nodeIndex: [1, 3],
        paintOrders: [0, 4],
      },
    }],
  };
  const cdp = {
    async call() {
      return {
        result: {
          value: [
            { dom_index: 0, resolved: true, binding_status: "unbound", conflicts: [] },
            { dom_index: 1, resolved: true, binding_status: "unbound", conflicts: [] },
            { dom_index: 2, resolved: true, binding_status: "unbound", conflicts: [] },
          ],
        },
      };
    },
  };
  const report = await evaluateSemantics(cdp, snapshot, { width: 10, height: 10 });
  // Zero is a real paint order -- the bottom-most painted box -- and must not
  // be coalesced into "no data".
  assert.equal(report.candidates[0].paint_order, 0);
  assert.equal(report.candidates[1].paint_order, null,
    "an element with no layout entry paints nothing and says so explicitly");
  assert.equal(report.candidates[2].paint_order, 4);
  assert.equal(report.summary.paint_ordered, 2);
});

test("pseudo-element and shadow nodes do not shift snapshot lookups", async () => {
  // querySelectorAll returns neither a ::before box nor shadow-tree content,
  // but the snapshot emits both as element nodes. Counting them makes every
  // control after the first ::before read the node before its own -- which
  // still resolves, still looks like data, and is the wrong node.
  //
  // Node layout: 0 document, 1 <div>, 2 ::before on it, 3 shadow root,
  // 4 <span> inside that shadow root, 5 <button>. The page sees exactly two
  // elements: the div at 0 and the button at 1.
  const snapshot = {
    strings: ["#document", "DIV", "::before", "#shadow-root", "SPAN", "BUTTON"],
    documents: [{
      nodes: {
        nodeType: [9, 1, 1, 11, 1, 1],
        nodeName: [0, 1, 2, 3, 4, 5],
        parentIndex: [-1, 0, 1, 1, 3, 0],
        backendNodeId: [10, 20, 30, 40, 50, 60],
        pseudoType: { index: [2], value: [0] },
        shadowRootType: { index: [3], value: [0] },
      },
      layout: {
        nodeIndex: [1, 2, 4, 5],
        paintOrders: [1, 2, 3, 9],
      },
    }],
  };
  const cdp = {
    async call() {
      return {
        result: {
          value: [
            { dom_index: 0, tag: "div", resolved: true, binding_status: "unbound", conflicts: [] },
            { dom_index: 1, tag: "button", resolved: true, binding_status: "unbound", conflicts: [] },
          ],
        },
      };
    },
  };
  const report = await evaluateSemantics(cdp, snapshot, { width: 10, height: 10 });
  assert.deepEqual(
    report.candidates.map((candidate) => candidate.backend_node_id),
    [20, 60],
    "the button must resolve to its own node, not the shadow span's");
  assert.deepEqual(
    report.candidates.map((candidate) => candidate.paint_order),
    [1, 9],
    "counting the ::before box would hand the button paint order 3");
});

test("a page walk that disagrees with the snapshot walk fails loudly",
  async () => {
    // Belt to the pseudo-element brace: any future node kind the snapshot emits
    // and querySelectorAll does not must surface as an error rather than as
    // every control silently wearing its neighbour's data.
    const snapshot = {
      strings: ["#document", "DIV", "BUTTON"],
      documents: [{
        nodes: {
          nodeType: [9, 1, 1],
          nodeName: [0, 1, 2],
          parentIndex: [-1, 0, 0],
          backendNodeId: [10, 20, 30],
        },
        layout: { nodeIndex: [1, 2], paintOrders: [0, 1] },
      }],
    };
    const cdp = {
      async call() {
        return {
          result: {
            value: [{
              dom_index: 1,
              tag: "canvas",
              resolved: true,
              binding_status: "unbound",
              conflicts: [],
            }],
          },
        };
      },
    };
    await assert.rejects(
      evaluateSemantics(cdp, snapshot, { width: 10, height: 10 }),
      (error) => error.code === "capture-node-alignment-mismatch");
  });

test("a laid out snapshot without paint orders fails loudly", async () => {
  // Chromium omits paintOrders only when includePaintOrder was not requested.
  // Resolving that to a tree of nulls would read as "this page has no
  // layering" and every consumer would draw a plausible wrong stack.
  const cdp = {
    async call() {
      return {
        result: {
          value: [{
            dom_index: 0,
            resolved: true,
            binding_status: "unbound",
            conflicts: [],
          }],
        },
      };
    },
  };
  await assert.rejects(
    evaluateSemantics(cdp, {
      documents: [{
        nodes: { nodeType: [9, 1], backendNodeId: [10, 20] },
        layout: { nodeIndex: [1] },
      }],
    }, { width: 10, height: 10 }),
    (error) => error.code === "capture-paint-order-missing");

  await assert.rejects(
    evaluateSemantics(cdp, {
      documents: [{
        nodes: { nodeType: [9, 1, 1], backendNodeId: [10, 20, 30] },
        layout: { nodeIndex: [1, 2], paintOrders: [3] },
      }],
    }, { width: 10, height: 10 }),
    (error) => error.code === "capture-paint-order-mismatch");
});

test("semantic expression receives DOMSnapshot clickable element indexes", () => {
  const expression = semanticExpression([2, 7]);
  assert.match(expression, /new Set\(\s*\[2,7\]\)/);
  assert.match(expression, /dom-snapshot-clickable/);
  assert.match(expression, /onpointerdown/);
  assert.match(expression, /grab/);
});

test("semantic report maps candidates to backend ids and declares canvas limits",
    async () => {
      const cdp = {
        async call(method, params) {
          assert.equal(method, "Runtime.evaluate");
          assert.match(params.expression, /\[1\]/);
          return {
            result: {
              value: [{
                dom_index: 1,
                resolved: false,
                binding_status: "unbound",
                conflicts: [],
              }],
            },
          };
        },
      };
      const snapshot = {
        documents: [{
          nodes: {
            nodeType: [9, 1, 1],
            backendNodeId: [10, 20, 30],
            isClickable: { index: [2] },
          },
        }],
      };
      const report = await evaluateSemantics(
        cdp, snapshot, { width: 100, height: 80 });
      assert.equal(report.candidates[0].backend_node_id, 30);
      assert.equal(report.summary.unresolved, 1);
      assert.match(report.limitations[0], /Canvas and WebGL/);
    });

test("design-system component classes are admitted by the candidate gate", () => {
  // A pulp-knob is a styled div whose drag handlers live on an ancestor: no
  // native tag, no ARIA role, no inline handler. Importing a six-knob sampler
  // returned eleven candidates that were all buttons, and not one control that
  // mattered.
  //
  // Two traps this pins, both of which produced a SILENT no-op — candidates
  // unchanged while the importer still printed Similarity 100% / PASS, because
  // the visual lane does not depend on the semantic lane:
  //
  //   1. There are TWO predicates. strongSignal() only feeds strongAncestor();
  //      the real admission gate is the inline `strong` in the candidate loop.
  //      Patching the function alone changes nothing.
  //   2. This region is inside the template literal returned by
  //      semanticExpression() and runs in the page, so a backtick anywhere —
  //      including in a comment — terminates it, and backslash escapes are
  //      eaten before the page ever sees them.
  const expression = semanticExpression([]);

  assert.match(expression, /componentKinds/,
    "the recogniser must reach the injected page expression");
  assert.match(expression, /'pulp-knob', 'knob'/,
    "knobs are the controls this exists for");

  // The gate itself, not merely the helper.
  const gate = expression.slice(expression.indexOf("const strong ="));
  assert.match(gate.slice(0, 200), /component/,
    "the inline admission gate must consult the component recogniser, " +
    "not just strongSignal()");

  // The region is injected as a template literal: a stray backtick or a
  // single-backslash escape silently breaks it in the page.
  const injected = expression.slice(expression.indexOf("componentKinds"),
                                    expression.indexOf("const strong ="));
  assert.ok(!injected.includes("`"), "no backticks inside the injected region");
});

test("a declared knob indicator is carried into the candidate", () => {
  // The pointer is DECLARED for the same reason the paint box is: a capture is
  // one flat picture, so nothing in it distinguishes the moving dot from the
  // face it sits on. Without this the importer has a colour and no geometry,
  // and an imported knob's indicator stands still.
  //
  // These assertions are on the page-evaluated source because that is the only
  // place the contract lives -- the expression is a template literal, so a
  // recogniser that never reaches the emitted candidate object is a silent
  // no-op the CDP round trip cannot see.
  const expression = semanticExpression([]);
  assert.match(expression, /data-pulp-indicator/);
  assert.match(expression, /indicator_bounds: indicatorBox\(element\)/);
  assert.match(expression, /indicator_color: indicatorColor\(element\)/);
  // Page coordinates, matching bounds and paint_bounds, so a consumer can
  // relate the pointer to its dial without a second coordinate system.
  assert.match(expression, /left: box.left \+ window.scrollX/);
  // Resolved by the browser, with the attribute's own value as the author's
  // override for a pointer no single computed colour describes.
  assert.match(expression, /opaque\(style.backgroundColor\)/);
  assert.match(expression, /if \(declared\) return declared;/);
});

test("the pointer is described in its own space as well as on the page", () => {
  // A rotated needle's client rect is the box its diagonal sweeps, not the
  // needle: 4x38 at 38 degrees measures 26.5x32.4, and a width read off that is
  // roughly ten times the truth. So the candidate carries the element's own size
  // and the matrix that places it, beside the rect rather than instead of it --
  // the rect is the painted footprint, which is what the sprite pass has to
  // erase.
  //
  // Asserted on the page-evaluated source for the same reason as the case above:
  // the expression is a template literal, so a field that never reaches the
  // emitted object is a silent no-op no CDP round trip can see.
  const expression = semanticExpression([]);
  assert.match(expression, /bounds.intrinsic = geometry.intrinsic;/);
  assert.match(expression, /bounds.transform = geometry.transform;/);
  // Two sources, because the two shapes answer to different APIs. An SVG shape
  // has a geometry box in user units and a screen CTM; an HTML box has neither.
  assert.match(expression, /marked\.getBBox\(\)/);
  assert.match(expression, /marked\.getScreenCTM\(\)/);
  assert.match(expression, /marked\.offsetWidth/);
  // The stroke inflates the element's OWN box, with no scale applied, because
  // computed stroke-width is in user units however firmly it says px.
  assert.match(expression, /if \(!\(width > 0\)\) \{ x -= stroke \/ 2; width = stroke; \}/);
  // Ancestor transforms scale this element too, and the dial it is measured
  // against sits under the same ones.
  assert.match(expression, /el = el\.parentElement/);
  // Individual rotate's axis-angle grammar is parsed explicitly. Unsupported
  // 3D axes refuse oriented geometry rather than publishing a partial matrix.
  assert.match(expression, /const cssRotateDegrees = value/);
  assert.match(expression, /if \(!local\) return null/);
  assert.match(expression, /matrix\.scaleSelf\(zoom\)/);
  assert.match(expression, /if \(!transformed\.is2D\) return null/);
  assert.match(expression, /style\.offsetPath/);
  assert.match(expression, /non-scaling-stroke/);
  // The intrinsic origin and full transform place asymmetric SVG geometry;
  // its painted footprint need not share the geometry box's centre.
  assert.match(expression, /intrinsic: \{ x, y, width, height \}/);
  // Fractional used CSS sizes win over integer-rounded offset dimensions.
  assert.match(expression, /borderBoxExtent\('width', marked\.offsetWidth\)/);
  // Sprite erasure uses a separate, conservative SVG stroke footprint; the
  // intrinsic geometry above must not be widened to solve a paint problem.
  assert.match(expression, /const svgStrokeFootprint = marked =>/);
  assert.match(expression, /const strokeBox = svgStrokeFootprint\(marked\);/);
});

// Chrome's computed defaults for the properties indicatorColor consults whose
// default stands alone. border-top-color is NOT here: its initial value is
// currentColor, so it is derived per node in computed() below. Two of these
// carry the whole test:
//
//   fill defaults to opaque BLACK on every element rather than to `none`, which
//   is why the SVG branch is namespace-guarded and tag-filtered. A stub that
//   defaults it to "" or to "none" would let the guard tests pass with the
//   guard deleted.
//
//   color is the measured value from the defect this pins -- the near-white a
//   near-black SVG needle was reported as -- so a regression that drops the SVG
//   branch reproduces the original bug exactly rather than some other failure.
//
// These are camelCase because the page reads style.backgroundColor and
// style.stroke, not getPropertyValue('background-color'). A stub that only
// implements getPropertyValue passes while exercising nothing.
const COMPUTED_DEFAULTS = {
  display: "block",
  visibility: "visible",
  opacity: "1",
  cursor: "auto",
  backgroundColor: "rgba(0, 0, 0, 0)",
  color: "rgb(232, 232, 238)",
  fill: "rgb(0, 0, 0)",
  stroke: "none",
};

const SVG_NS = "http://www.w3.org/2000/svg";
const HTML_NS = "http://www.w3.org/1999/xhtml";

function element({ tag = "div", ns = HTML_NS, attrs = {}, style = {},
                   children = [] }) {
  const entries = new Map(Object.entries(attrs));
  const node = {
    tagName: tag.toUpperCase(),
    namespaceURI: ns,
    id: entries.get("id") ?? "",
    parentElement: null,
    childNodes: [],
    textContent: "",
    style,
    getAttribute: (name) => (entries.has(name) ? entries.get(name) : null),
    hasAttribute: (name) => entries.has(name),
    get attributes() {
      return [...entries].map(([name, value]) => ({ name, value }));
    },
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 40, height: 40 }),
    // Only the [attribute] form the page actually uses.
    querySelector(selector) {
      const wanted = selector.slice(1, -1);
      const search = (parent) => {
        for (const child of parent.childNodes) {
          if (child.hasAttribute(wanted)) return child;
          const deeper = search(child);
          if (deeper) return deeper;
        }
        return null;
      };
      return search(node);
    },
  };
  for (const child of children) {
    child.parentElement = node;
    node.childNodes.push(child);
  }
  return node;
}

// Run the REAL injected expression against that DOM. indicatorColor lives
// inside the template literal semanticExpression() returns and is evaluated in
// the page, so a regex over that string cannot distinguish a working precedence
// from a broken one. Only executing it can.
function runSemanticExpression(roots) {
  const flat = [];
  const walk = (node) => {
    flat.push(node);
    node.childNodes.forEach(walk);
  };
  roots.forEach(walk);

  const computed = (node) => ({
    ...COMPUTED_DEFAULTS,
    // border-*-color's initial value is currentColor, so an element that
    // authors no border colour computes it to its own text colour -- on an SVG
    // node exactly as on a div. That is the whole reason the SVG branch has to
    // sit BEFORE borderTopColor rather than being appended after it: appended,
    // an SVG pointer is caught here and reports the text colour, and the new
    // branch never runs at all.
    borderTopColor: node.style.color ?? COMPUTED_DEFAULTS.color,
    ...node.style,
    getPropertyValue: (name) => node.style[name] ?? "",
  });

  const saved = {
    document: globalThis.document,
    window: globalThis.window,
    getComputedStyle: globalThis.getComputedStyle,
  };
  globalThis.document = { querySelectorAll: () => flat };
  globalThis.getComputedStyle = computed;
  globalThis.window = { getComputedStyle: computed, scrollX: 0, scrollY: 0 };
  try {
    return (0, eval)(semanticExpression([]));
  } finally {
    Object.assign(globalThis, saved);
  }
}

function knob(label, indicator) {
  return element({
    attrs: { "aria-label": label, "data-pulp-kind": "knob" },
    children: [indicator],
  });
}

test("an indicator reports the colour it is actually painted in", () => {
  const candidates = runSemanticExpression([
    element({
      children: [
        // POSITIVE CONTROL. The path that already worked: a pointer drawn as a
        // filled box. If the SVG branch ever runs for a plain box, this is the
        // first thing that breaks.
        knob("DivBackground", element({
          attrs: { "data-pulp-indicator": "" },
          style: { backgroundColor: "rgb(255, 136, 0)" },
        })),
        // POSITIVE CONTROL, and the namespace guard's test. A hairline pointer
        // drawn as a border has a transparent background, so the walk falls
        // through -- and fill computes to opaque black on this box. This
        // asserts the border colour rather than merely "not empty", because an
        // unguarded fill read returns a plausible black here.
        knob("DivBorder", element({
          attrs: { "data-pulp-indicator": "" },
          style: { borderTopColor: "rgb(12, 34, 56)" },
        })),
        // THE DEFECT. A line is painted by stroke and carries no background and
        // no border, so it fell through to the inherited text colour: an
        // authored near-black needle arriving near-white.
        knob("SvgStroke", element({
          tag: "line", ns: SVG_NS,
          attrs: { "data-pulp-indicator": "" },
          style: { stroke: "rgb(16, 16, 20)" },
        })),
        // A filled arrowhead paints with fill and leaves stroke none.
        knob("SvgFill", element({
          tag: "polygon", ns: SVG_NS,
          attrs: { "data-pulp-indicator": "" },
          style: { fill: "rgb(255, 0, 204)" },
        })),
        // Both painted, on a shape a fill does paint. Stroke wins: the outline
        // of a stroked needle is the pointer, and the fill behind it is the
        // body it is drawn on. Without this case the ordering is unpinned --
        // every other case leaves one of the two properties unset, so swapping
        // them stays green.
        knob("SvgStrokedPath", element({
          tag: "path", ns: SVG_NS,
          attrs: { "data-pulp-indicator": "" },
          style: { stroke: "rgb(16, 16, 20)", fill: "rgb(255, 0, 204)" },
        })),
        // A paint server is a gradient or a pattern, not a colour. Absent for
        // the same reason the vector lowering refuses it, rather than handing a
        // consumer the literal string url(#g) to parse as a colour.
        knob("SvgPaintServer", element({
          tag: "path", ns: SVG_NS,
          attrs: { "data-pulp-indicator": "" },
          style: { stroke: "url(#g)", fill: "none" },
        })),
        // An <svg> root marked as the pointer paints nothing itself, and fill
        // computes to opaque black on it. Reporting that black would be the
        // original defect wearing a different colour.
        knob("SvgRootMarked", element({
          tag: "svg", ns: SVG_NS,
          attrs: { "data-pulp-indicator": "" },
        })),
        // A stroke-only shape with no authored stroke paints nothing; its
        // default black fill is not a pointer colour.
        knob("SvgUnpaintedLine", element({
          tag: "line", ns: SVG_NS,
          attrs: { "data-pulp-indicator": "" },
        })),
      ],
    }),
  ]);

  const colourOf = (name) => {
    const found = candidates.find((candidate) => candidate.name === name);
    assert.ok(found, `${name} must reach the candidate list at all`);
    return found.indicator_color;
  };

  assert.equal(colourOf("DivBackground"), "rgb(255, 136, 0)",
    "a background-painted pointer must keep reporting its background");
  assert.equal(colourOf("DivBorder"), "rgb(12, 34, 56)",
    "a border-painted pointer must report its border, not the black that " +
    "fill computes to on a plain box");
  assert.equal(colourOf("SvgStroke"), "rgb(16, 16, 20)",
    "an SVG line pointer is painted by stroke, not by the inherited text " +
    "colour it used to report");
  assert.equal(colourOf("SvgFill"), "rgb(255, 0, 204)",
    "a filled SVG arrowhead is painted by fill");
  assert.equal(colourOf("SvgStrokedPath"), "rgb(16, 16, 20)",
    "stroke outranks fill on a shape that paints both");
  assert.equal(colourOf("SvgPaintServer"), "",
    "url(...) paint is a paint server, not a colour");
  assert.equal(colourOf("SvgRootMarked"), "",
    "an svg root paints nothing itself; its default black fill is not a colour");
  assert.equal(colourOf("SvgUnpaintedLine"), "",
    "a stroke-only shape with no stroke reports nothing, not its default fill");
});
