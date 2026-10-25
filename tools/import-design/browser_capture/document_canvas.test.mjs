// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  DOCUMENT_CANVAS_SCHEMA,
  evaluateDocumentCanvas,
  opaqueCanvasColor,
} from "./document_canvas.mjs";

test("only an opaque rgb() is a canvas colour", () => {
  assert.equal(opaqueCanvasColor("rgb(18, 18, 18)"), "rgb(18, 18, 18)");
  assert.equal(opaqueCanvasColor("rgb(255, 255, 255)"), "rgb(255, 255, 255)");
  for (const value of ["rgba(18, 18, 18, 0.5)", "rgba(0, 0, 0, 0)",
    "rgb(256, 0, 0)", "Canvas", "", null, undefined, "rgb(1,2,3)"]) {
    assert.equal(opaqueCanvasColor(value), null, String(value));
  }
});

function fakeCdp(answer) {
  const calls = [];
  return {
    calls,
    async call(method, params) {
      calls.push({ method, params });
      return answer();
    },
  };
}

test("the report carries Chromium's answer for the root's color-scheme", async () => {
  const cdp = fakeCdp(() => ({ result: { value: {
    root_color_scheme: "dark", canvas: "rgb(18, 18, 18)" } } }));
  assert.deepEqual(await evaluateDocumentCanvas(cdp), {
    schema: DOCUMENT_CANVAS_SCHEMA, root_color_scheme: "dark",
    canvas_color: "rgb(18, 18, 18)" });
  // The probe asks for the Canvas system colour under the root's scheme and
  // removes itself.
  const expression = cdp.calls[0].params.expression;
  assert.match(expression, /'background-color', 'Canvas', 'important'/u);
  assert.match(expression, /'color-scheme', scheme, 'important'/u);
  assert.match(expression, /probe\.remove\(\)/u);
});

test("an unreadable answer records no canvas colour", async () => {
  for (const answer of [
    () => ({ exceptionDetails: { text: "boom" },
      result: { value: { canvas: "rgb(1, 2, 3)" } } }),
    () => ({ result: { value: null } }),
    () => ({ result: { value: { canvas: "rgba(0, 0, 0, 0)" } } }),
    () => { throw new Error("target closed"); },
  ]) {
    const report = await evaluateDocumentCanvas(fakeCdp(answer));
    assert.equal(report.schema, DOCUMENT_CANVAS_SCHEMA);
    assert.equal(report.canvas_color, null);
  }
});
