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

test("does not publish browser-only html scaffolding as native evidence", () => {
  const snapshot = {
    strings: ["#document", "HTML", "BODY"],
    documents: [{
      nodes: {
        parentIndex: [-1, 0, 1], nodeType: [9, 1, 1],
        nodeName: [0, 1, 2], attributes: [[], [], []],
      },
      layout: { nodeIndex: [1, 2], bounds: [[0, 0, 100, 50], [0, 0, 100, 50]] },
    }],
  };
  const bindings = buildMaterializedLayoutBindings(snapshot);
  assert.equal(bindings.some(binding => binding.anchor === "body" &&
    binding.path.length === 1 && binding.path[0].tag === "html"), false);
});

test("does not replay SVG primitive ink bounds as Yoga layout boxes", () => {
  const strings = ["#document", "HTML", "BODY", "DIV", "SVG", "PATH", "RECT",
    "id", "root"];
  const snapshot = {
    strings,
    documents: [{
      nodes: {
        parentIndex: [-1, 0, 1, 2, 3, 4, 4],
        nodeType: [9, 1, 1, 1, 1, 1, 1],
        nodeName: [0, 1, 2, 3, 4, 5, 6],
        attributes: [[], [], [], [7, 8], [], [], []],
      },
      layout: {
        nodeIndex: [3, 4, 5, 6],
        bounds: [
          [0, 0, 100, 40], [11, 6, 22, 16],
          [14.859375, 7.8125, 14.28125, 8],
          [12, 8, 4, 6],
        ],
      },
    }],
  };

  const bindings = buildMaterializedLayoutBindings(snapshot);
  assert.deepEqual(bindings.map(binding => binding.path.at(-1).tag), ["svg"]);
  assert.deepEqual(bindings[0].box,
    { left: 11, top: 6, width: 22, height: 16 });
});
