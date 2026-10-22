// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";
import vm from "node:vm";

import {
  evaluatePlatformFonts,
  LINE_METRICS_FUNCTION,
} from "./platform_fonts.mjs";

// Two text runs: one owned by an ordinary element, one by a ::marker pseudo
// element hosted by node 0, whose computed font is not its host's.
function snapshotWithPseudo(pseudoName = "marker") {
  return {
    strings: [pseudoName],
    documents: [{
      nodes: {
        parentIndex: [-1, 0, 1, 0, 3],
        nodeType: [1, 1, 3, 1, 3],
        backendNodeId: [1, 10, 11, 20, 21],
        pseudoType: { index: [3], value: [0] },
      },
      layout: {
        nodeIndex: [1, 2, 3, 4],
        text: [-1, 0, -1, 0],
        styles: [[], [], [], []],
      },
    }],
  };
}

function fakeCdp(measure) {
  const calls = [];
  const cdp = {
    calls,
    async call(method, params = {}) {
      calls.push({ method, params });
      switch (method) {
        case "DOM.pushNodesByBackendIdsToFrontend":
          return { nodeIds: params.backendNodeIds.map((id) => id + 1000) };
        case "CSS.getPlatformFontsForNode":
          return { fonts: [{ familyName: "Times", postScriptName: "Times-Roman",
                             isCustomFont: false, glyphCount: 4 }] };
        case "Page.getFrameTree":
          return { frameTree: { frame: { id: "main" } } };
        case "Page.createIsolatedWorld":
          return { executionContextId: 77 };
        case "DOM.resolveNode":
          return { object: { objectId: `obj-${params.backendNodeId}` } };
        case "Runtime.callFunctionOn":
          return { result: { value: measure } };
        default:
          return {};
      }
    },
  };
  return cdp;
}

test("platform fonts record the measured line metrics per run", async () => {
  const cdp = fakeCdp({ ascent: 14, descent: 4 });
  const report = await evaluatePlatformFonts(cdp, snapshotWithPseudo(), []);
  assert.equal(report.runs.length, 2);
  const plain = report.runs.find((run) => run.backend_node_id === 10);
  const marker = report.runs.find((run) => run.backend_node_id === 20);
  assert.deepEqual(plain.line_metrics, { ascent: 14, descent: 4 });
  assert.deepEqual(marker.line_metrics, { ascent: 14, descent: 4 });

  // The ordinary run is measured on its own element; the marker on its host,
  // asking for the pseudo-element's style rather than the host's.
  const measured = cdp.calls
    .filter((c) => c.method === "DOM.resolveNode")
    .map((c, i) => [c.params.backendNodeId,
                    cdp.calls.filter((x) => x.method === "Runtime.callFunctionOn")[i]
                      .params.arguments[0].value]);
  assert.deepEqual(measured, [[10, ""], [1, "::marker"]]);
  // Measured in an isolated world, never the page's own.
  assert.ok(cdp.calls.filter((c) => c.method === "DOM.resolveNode")
    .every((c) => c.params.executionContextId === 77));
  // Measuring must not mutate the document: only reads are issued.
  const mutating = cdp.calls.filter((c) =>
    /^(DOM\.(set|remove|insert|move|copy|undo|redo)|CSS\.(set|add|create)|Runtime\.evaluate|Page\.(navigate|reload))/.test(c.method));
  assert.deepEqual(mutating, []);
});

test("a pseudo-element the canvas cannot address records null", async () => {
  const cdp = fakeCdp({ ascent: 14, descent: 4 });
  const report = await evaluatePlatformFonts(
    cdp, snapshotWithPseudo("backdrop"), []);
  const pseudo = report.runs.find((run) => run.backend_node_id === 20);
  assert.equal(pseudo.line_metrics, null);
});

test("a run whose metrics cannot be measured records null, not a guess", async () => {
  const cdp = fakeCdp(null);
  const report = await evaluatePlatformFonts(cdp, snapshotWithPseudo(), []);
  for (const run of report.runs) assert.equal(run.line_metrics, null);
});

function runMeasure(style, canvasFont = {}) {
  const context = {
    Element: class {},
    getComputedStyle: (element, pseudo) => {
      context.lastPseudo = pseudo;
      return style;
    },
    OffscreenCanvas: class {
      getContext() {
        const ctx = {
          _font: "10px sans-serif",
          fontStretch: "normal",
          get font() { return this._font; },
          set font(value) {
            // A rejecting canvas still takes the probe font, which any real
            // canvas parses.
            if (canvasFont.reject && value !== "13px serif") return;
            // A real canvas reads back the request in canonical form.
            this._font = canvasFont.normalize ? canvasFont.normalize(value) : value;
          },
          measureText() {
            return { fontBoundingBoxAscent: 15, fontBoundingBoxDescent: 4 };
          },
        };
        return ctx;
      }
    },
  };
  vm.createContext(context);
  const fn = vm.runInContext(`(${LINE_METRICS_FUNCTION})`, context);
  const element = new context.Element();
  return fn.call(element);
}

const baseStyle = {
  fontStyle: "normal", fontWeight: "400", fontSize: "16px",
  fontFamily: "Times", fontStretch: "100%", fontSizeAdjust: "none",
};

test("line metrics come from the canvas primary-font bounding box", () => {
  // The function runs in another realm; compare its plain values.
  assert.deepEqual({ ...runMeasure(baseStyle) }, { ascent: 15, descent: 4 });
});

test("a font request the canvas rejects measures nothing", () => {
  assert.equal(runMeasure(baseStyle, { reject: true }), null);
});

test("a stretch or size adjustment the canvas cannot express measures nothing", () => {
  assert.equal(runMeasure({ ...baseStyle, fontStretch: "93%" }), null);
  assert.equal(runMeasure({ ...baseStyle, fontSizeAdjust: "0.5" }), null);
});

test("a request that serializes to the canvas default still measures", () => {
  // "normal 400 10px sans-serif" reads back as "10px sans-serif", the very
  // font a fresh canvas starts with, yet it was accepted.
  const normalize = (value) =>
    value.replace(/^normal (?:normal )?400 /u, "");
  const style = { ...baseStyle, fontSize: "10px", fontFamily: "sans-serif" };
  assert.deepEqual({ ...runMeasure(style, { normalize }) },
    { ascent: 15, descent: 4 });
  // Nor is a request equal to the second probe font mistaken for a rejection.
  const serif = { ...baseStyle, fontSize: "13px", fontFamily: "serif" };
  assert.deepEqual({ ...runMeasure(serif, { normalize }) },
    { ascent: 15, descent: 4 });
  // Control: the same canonicalising canvas still rejects what it rejects.
  assert.equal(runMeasure(style, { normalize, reject: true }), null);
});
