// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import { buildMaterializedTextBindings } from "./materialized_text_bindings.mjs";

function fixture() {
  return {
    snapshot: {
      strings: ["HTML", "BODY", "DIV", "SPAN", "id", "root", "DYNAMIC"],
      documents: [{
        nodes: {
          parentIndex: [-1, 0, 1, 2],
          nodeType: [1, 1, 1, 1],
          nodeName: [0, 1, 2, 3],
          attributes: [[], [], [4, 5], []],
        },
        layout: {
          nodeIndex: [3],
          text: [6],
          bounds: [[10, 20, 80, 18]],
        },
        textBoxes: {
          layoutIndex: [0],
          bounds: [[10, 20, 58, 18]],
          start: [0],
          length: [7],
        },
      }],
    },
    fonts: {
      runs: [{
        owner_node_index: 3,
        layout_index: 0,
        requested: { font_family: "Captured", font_size: "14px",
          font_weight: "400", font_style: "normal", letter_spacing: "0px" },
        resolved: [{ post_script_name: "Captured-Regular", glyph_count: 7 }],
      }],
    },
  };
}

test("materialized text binding joins a live label by structural path", () => {
  const { snapshot, fonts } = fixture();
  assert.deepEqual(buildMaterializedTextBindings(snapshot, fonts), [{
    index: 0,
    anchor: "#root",
    path: [{ tag: "span", index: 0 }],
    text: "DYNAMIC",
    basis: {
      width: 80,
      resolved_face: "Captured-Regular",
      resolved_faces: [{ family_name: "", post_script_name: "Captured-Regular",
        is_custom_font: false, glyph_count: 7 }],
      requested: { font_family: "Captured", font_size: 14,
        font_weight: 400, font_slant: 0, letter_spacing: 0 },
    },
    boxes: [{ left: 0, top: 0, width: 58, height: 18,
      start: 0, length: 7 }],
  }]);
});

test("invalid computed typography fails closed", () => {
  const { snapshot, fonts } = fixture();
  fonts.runs[0].requested.font_size = "medium";
  assert.deepEqual(buildMaterializedTextBindings(snapshot, fonts), []);
  fonts.runs[0].requested.font_size = "14px";
  fonts.runs[0].requested.font_weight = "bolder";
  assert.deepEqual(buildMaterializedTextBindings(snapshot, fonts), []);
  fonts.runs[0].requested.font_weight = "400";
  fonts.runs[0].requested.letter_spacing = "inherit";
  assert.deepEqual(buildMaterializedTextBindings(snapshot, fonts), []);
});

test("fallback faces retain the dominant native shaping identity", () => {
  const { snapshot, fonts } = fixture();
  fonts.runs[0].resolved.push({ post_script_name: "Fallback", glyph_count: 1 });
  assert.equal(buildMaterializedTextBindings(snapshot, fonts)[0]
    .basis.resolved_face, "Captured-Regular");
  assert.deepEqual(buildMaterializedTextBindings(snapshot, fonts)[0]
    .basis.resolved_faces, [
      { family_name: "", post_script_name: "Captured-Regular",
        is_custom_font: false, glyph_count: 7 },
      { family_name: "", post_script_name: "Fallback",
        is_custom_font: false, glyph_count: 1 },
    ]);

  fonts.runs[0].resolved[1].glyph_count = 20;
  assert.equal(buildMaterializedTextBindings(snapshot, fonts)[0]
    .basis.resolved_face, "Fallback");
});

test("multi-run owners fail closed to independent native shaping", () => {
  const { snapshot, fonts } = fixture();
  fonts.runs.push({ ...fonts.runs[0] });
  assert.deepEqual(buildMaterializedTextBindings(snapshot, fonts), []);
});

test("direct mixed-content text records its renderer target ordinal", () => {
  const { snapshot, fonts } = fixture();
  const document = snapshot.documents[0];
  document.nodes.parentIndex.push(3);
  document.nodes.nodeType.push(3);
  document.nodes.nodeName.push(3);
  document.nodes.attributes.push([]);
  document.nodes.parentIndex.push(3);
  document.nodes.nodeType.push(1);
  document.nodes.nodeName.push(3);
  document.nodes.attributes.push([]);
  document.layout.nodeIndex.push(4);
  document.layout.text.push(6);
  document.layout.bounds.push([10, 20, 58, 18]);
  document.textBoxes.layoutIndex[0] = 1;
  fonts.runs[0].node_index = 4;
  fonts.runs[0].layout_index = 1;

  assert.equal(buildMaterializedTextBindings(snapshot, fonts)[0]
    .anonymous_text_index, 0);
});

test("mixed-content owners preserve each direct text run independently", () => {
  const { snapshot, fonts } = fixture();
  const document = snapshot.documents[0];
  snapshot.strings.push("SCULPT", " ▾", "svg");
  const sculpt = snapshot.strings.indexOf("SCULPT");
  const disclosure = snapshot.strings.indexOf(" ▾");
  const svg = snapshot.strings.indexOf("svg");

  document.nodes.parentIndex.push(3, 3, 3);
  document.nodes.nodeType.push(1, 3, 3);
  document.nodes.nodeName.push(svg, 3, 3);
  document.nodes.attributes.push([], [], []);
  document.layout.nodeIndex = [3, 5, 6];
  document.layout.text = [-1, sculpt, disclosure];
  document.layout.bounds = [
    [10, 20, 106, 18], [49, 20, 49, 18], [98, 20, 18, 18],
  ];
  document.textBoxes = {
    layoutIndex: [1, 2],
    bounds: [[49, 20, 49, 18], [98, 20, 18, 18]],
    start: [0, 0],
    length: [6, 2],
  };
  fonts.runs = [
    { ...fonts.runs[0], node_index: 5, layout_index: 1,
      resolved: [{ post_script_name: "Captured-Regular", glyph_count: 6 }] },
    { ...fonts.runs[0], node_index: 6, layout_index: 2,
      resolved: [{ post_script_name: "Captured-Regular", glyph_count: 2 }] },
  ];

  const bindings = buildMaterializedTextBindings(snapshot, fonts);
  assert.deepEqual(bindings.map(binding => ({
    text: binding.text,
    anonymous_text_index: binding.anonymous_text_index,
    left: binding.boxes[0].left,
  })), [
    { text: "SCULPT", anonymous_text_index: 0, left: 39 },
    { text: " ▾", anonymous_text_index: 1, left: 88 },
  ]);
});

test("adjacent single-line text nodes merge into one native Label binding", () => {
  const { snapshot, fonts } = fixture();
  const document = snapshot.documents[0];
  snapshot.strings.push("SCULPT", " ▾");
  const sculpt = snapshot.strings.indexOf("SCULPT");
  const disclosure = snapshot.strings.indexOf(" ▾");

  document.nodes.parentIndex.push(3, 3);
  document.nodes.nodeType.push(3, 3);
  document.nodes.nodeName.push(3, 3);
  document.nodes.attributes.push([], []);
  document.layout.nodeIndex = [3, 4, 5];
  document.layout.text = [-1, sculpt, disclosure];
  document.layout.bounds = [
    [10, 20, 106, 18], [49, 20, 49, 18], [98, 20, 18, 18],
  ];
  document.textBoxes = {
    layoutIndex: [1, 2],
    bounds: [[49, 20, 49, 18], [98, 20, 18, 18]],
    start: [0, 0],
    length: [6, 2],
  };
  fonts.runs = [
    { ...fonts.runs[0], node_index: 4, layout_index: 1,
      resolved: [{ post_script_name: "Captured-Regular", glyph_count: 8 }] },
    { ...fonts.runs[0], node_index: 5, layout_index: 2,
      resolved: [{ post_script_name: "Captured-Regular", glyph_count: 8 }] },
  ];

  const bindings = buildMaterializedTextBindings(snapshot, fonts);
  assert.equal(bindings.length, 1);
  assert.equal(bindings[0].text, "SCULPT ▾");
  assert.equal(bindings[0].anonymous_text_index, undefined);
  assert.deepEqual(bindings[0].boxes, [
    { left: 39, top: 0, width: 67, height: 18, start: 0, length: 8 },
  ]);
  assert.deepEqual(bindings[0].basis.resolved_faces.map(face =>
    [face.post_script_name, face.glyph_count]), [
    ["Captured-Regular", 8],
  ]);
});
