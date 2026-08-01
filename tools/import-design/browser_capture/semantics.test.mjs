// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  evaluateSemantics,
  semanticExpression,
} from "./semantics.mjs";

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
  assert.match(expression, /indicator: indicatorBox\(element\)/);
  // Page coordinates, matching bounds and paint_bounds, so a consumer can
  // relate the pointer to its dial without a second coordinate system.
  assert.match(expression, /left: box.left \+ window.scrollX/);
  // Resolved by the browser, with the attribute's own value as the author's
  // override for a pointer no single computed colour describes.
  assert.match(expression, /opaque\(style.backgroundColor\)/);
  assert.match(expression, /const color = declared \|\|/);
});
