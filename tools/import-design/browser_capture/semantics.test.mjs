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
