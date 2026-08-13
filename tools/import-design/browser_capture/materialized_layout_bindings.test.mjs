// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";
import { buildMaterializedLayoutBindings } from "./materialized_layout_bindings.mjs";

test("captures nested Chromium boxes as parent-relative structural evidence", () => {
  const strings = ["HTML", "BODY", "DIV", "BUTTON", "id", "root"];
  const snapshot = {
    strings,
    documents: [{
      nodes: {
        parentIndex: [-1, 0, 1, 2, 3],
        nodeType: [9, 1, 1, 1, 1],
        nodeName: [-1, 0, 1, 2, 3],
        attributes: [[], [], [], [4, 5], []],
      },
      layout: {
        nodeIndex: [2, 3, 4],
        bounds: [[0, 0, 1320, 860], [26, 0, 1228, 800], [666.5, 10, 44.5, 20]],
      },
    }],
  };
  assert.deepEqual(buildMaterializedLayoutBindings(snapshot), [{
    index: 0,
    anchor: "#root",
    path: [{ tag: "button", index: 0 }],
    box: { left: 640.5, top: 10, width: 44.5, height: 20 },
  }]);
});

test("fails closed for absent and non-finite layout evidence", () => {
  assert.deepEqual(buildMaterializedLayoutBindings({}), []);
  const snapshot = {
    strings: ["HTML", "BODY", "DIV", "id", "root"],
    documents: [{
      nodes: {
        parentIndex: [-1, 0, 1, 2], nodeType: [9, 1, 1, 1],
        nodeName: [-1, 0, 1, 2], attributes: [[], [], [], [3, 4]],
      },
      layout: { nodeIndex: [3], bounds: [[0, 0, Number.NaN, 20]] },
    }],
  };
  assert.deepEqual(buildMaterializedLayoutBindings(snapshot), []);
});
