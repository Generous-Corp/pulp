// Plan B / B4b: the faithful-vector lane for the Figma plugin. Exercises the
// geometry knob auto-detector + the sandbox-safe SVG byte decode, and asserts
// serialize.ts emits the render_mode / svg_asset_id / interactive_elements the
// C++ materializer (DesignFrameView) consumes. Kept in lockstep with the REST
// lane (tools/import-design/figma_rest_export.py + test_figma_rest_export.py).

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  parseFrameKnobs,
  parsePanelBounds,
  detectOverlayControls,
  decodeSvgBytes,
} from "../src/faithful-vector";
import { serializeExport, type SerializeContext } from "../src/serialize";
import type { ExtractedFigmaNode } from "../src/extract-model";
import { AssetCache } from "../src/assets";

const SVG =
  '<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">' +
  '<defs><linearGradient id="g"><stop offset="0" stop-color="#ebf5ff"/>' +
  '<stop offset="1" stop-color="#717f8e"/></linearGradient></defs>' +
  '<rect x="10" y="10" width="80" height="80" fill="#1c1d1d"/>' +
  '<circle cx="50" cy="50" r="20" fill="url(#g)"/>' + // dome
  '<circle cx="50" cy="50" r="5" fill="#222222"/>' + // inner, non-gradient → ignored
  '<path d="M50 38L50 30" stroke="white" stroke-width="3"/>' + // needle
  '<path d="M20 20L25 25" stroke="#506274" stroke-width="2"/>' + // dark tick → ignored
  "</svg>";

test("parseFrameKnobs: geometry auto-detect finds the knob with the exact needle d", () => {
  const knobs = parseFrameKnobs(SVG);
  assert.equal(knobs.length, 1);
  const k = knobs[0];
  assert.equal(k.kind, "knob");
  assert.equal(k.cx, 50);
  assert.equal(k.cy, 50);
  assert.equal(k.hit_radius, 20);
  assert.equal(k.svg_patch_d, "M50 38L50 30"); // exact d so the needle can rotate
  assert.equal(k.default_value, 0.5);
});

test("parseFrameKnobs: ignores non-knob shapes", () => {
  const plain =
    '<svg xmlns="http://www.w3.org/2000/svg">' +
    '<circle cx="10" cy="10" r="20" fill="#333"/>' + // solid, not a dome
    '<path d="M5 5L9 9" stroke="#506274"/></svg>'; // dark tick
  assert.deepEqual(parseFrameKnobs(plain), []);
});

test("decodeSvgBytes: ASCII round-trip (no TextDecoder in the sandbox)", () => {
  const bytes = new Uint8Array([...SVG].map((c) => c.charCodeAt(0)));
  assert.equal(decodeSvgBytes(bytes), SVG);
});

function baseNode(over: Partial<ExtractedFigmaNode>): ExtractedFigmaNode {
  return {
    type: "frame",
    figma_type: "FRAME",
    name: "ELYSIUM",
    figma_node_id: "3:42",
    parent_id: null,
    z_order: 0,
    absolute_bounds: { x: 0, y: 0, w: 100, h: 100 },
    relative_transform: [[1, 0, 0], [0, 1, 0]],
    visible: true,
    locked: false,
    opacity: 1,
    blend_mode: "NORMAL",
    style: {},
    layout: {},
    children: [],
    ...over,
  };
}

function ctx(over: Partial<SerializeContext> = {}): SerializeContext {
  return {
    fileKey: "abc123",
    rootNodeId: "3:42",
    pluginVersion: "0.1.0",
    assets: new AssetCache(),
    tokens: { colors: {}, dimensions: {}, strings: {} },
    ...over,
  };
}

test("serializeExport: a faithful_svg frame emits render_mode + svg_asset_id + interactive_elements", () => {
  const node = baseNode({
    render_mode: "faithful_svg",
    svg_asset_id: "svg-abc123",
    interactive_elements: parseFrameKnobs(SVG),
  });
  const env = serializeExport([node], [], ctx()) as Record<string, any>;
  const root = env.root as Record<string, any>;
  assert.equal(root.render_mode, "faithful_svg");
  assert.equal(root.svg_asset_id, "svg-abc123");
  assert.equal(root.interactive_elements.length, 1);
  assert.equal(root.interactive_elements[0].svg_patch_d, "M50 38L50 30");
});

test("serializeExport: a normal node omits the faithful-vector keys", () => {
  const env = serializeExport([baseNode({})], [], ctx()) as Record<string, any>;
  const root = env.root as Record<string, any>;
  assert.equal(root.render_mode, undefined);
  assert.equal(root.svg_asset_id, undefined);
  assert.equal(root.interactive_elements, undefined);
});

// ── Overlay detection — lockstep with the REST lane (test_figma_rest_export.py) ──

function txt(s: string): ExtractedFigmaNode {
  return baseNode({ figma_type: "TEXT", name: "t", content: s,
                    absolute_bounds: { x: 0, y: 0, w: 10, h: 5 } });
}
function child(name: string, x: number, y: number, w: number, h: number,
              id: string, kids: ExtractedFigmaNode[]): ExtractedFigmaNode {
  return baseNode({ figma_type: "FRAME", name, figma_node_id: id,
                    absolute_bounds: { x, y, w, h }, children: kids });
}
function icon(name: string): ExtractedFigmaNode {
  return baseNode({ figma_type: "FRAME", name,
                    absolute_bounds: { x: 0, y: 0, w: 8, h: 8 } });
}

test("parsePanelBounds: picks the largest panel-fraction rect", () => {
  const svg =
    '<svg width="1146" height="746">' +
    '<rect x="0" y="0" width="20" height="20"/>' +          // tiny → ignored
    '<rect x="73" y="50" width="1000" height="646"/>' +     // the panel
    '<rect x="0" y="0" width="1146" height="746"/>' +       // ~full frame → > 0.97, ignored
    "</svg>";
  const p = parsePanelBounds(svg);
  assert.deepEqual(p, [73, 50, 1000, 646]);
});

test("detectOverlayControls: dropdown (expand_more) vs stepper (Frame 41) vs placeholder", () => {
  const root = baseNode({
    figma_node_id: "3:42",
    absolute_bounds: { x: 0, y: 0, w: 1000, h: 600 },
    children: [
      // real dropdown: expand_more child + a real value
      child("Dropdown", 700, 480, 103, 27, "d1",
            [txt("1/4 Delay"), icon("expand_more_FILL0 1")]),
      // < > stepper: Frame 41 chevron pair, not a down-chevron
      child("Dropdown", 100, 440, 220, 20, "s1",
            [txt("Short Plucks"), icon("Frame 41")]),
      // unconfigured placeholder: expand_more but text == "Dropdown" → skip
      child("Dropdown", 320, 520, 103, 27, "p1",
            [txt("Dropdown"), icon("expand_more 2")]),
    ],
  });
  const els = detectOverlayControls(root, [0, 0], [73, 50]);
  const dropdowns = els.filter((e) => e.kind === "dropdown");
  const steppers = els.filter((e) => e.kind === "stepper");
  assert.equal(dropdowns.length, 1);
  assert.equal(dropdowns[0].source_node_id, "d1");
  // node (700,480) → svg (+73,+50) = (773,530); options = only the real value
  assert.deepEqual([dropdowns[0].x, dropdowns[0].y, dropdowns[0].w, dropdowns[0].h],
                   [773, 530, 103, 27]);
  assert.deepEqual(dropdowns[0].options, ["1/4 Delay"]);
  assert.equal(steppers.length, 1);
  assert.equal(steppers[0].source_node_id, "s1");
  assert.deepEqual(steppers[0].options, ["Short Plucks"]);
});

test("detectOverlayControls: finds a stepper from an explicit left/right chevron pair", () => {
  const root = baseNode({
    absolute_bounds: { x: 0, y: 0, w: 1000, h: 600 },
    children: [
      child("Dropdown", 400, 120, 160, 22, "st2",
            [txt("Sine"), icon("chevron_left"), icon("chevron_right")]),
    ],
  });
  const els = detectOverlayControls(root, [0, 0], [73, 50]);
  assert.equal(els.length, 1);
  assert.equal(els[0].kind, "stepper");
  assert.deepEqual(els[0].options, ["Sine"]);
});

test("detectOverlayControls: finds a search text_field and a tab group", () => {
  const root = baseNode({
    absolute_bounds: { x: 0, y: 0, w: 1000, h: 600 },
    children: [
      // search field group: filled box + leading magnifier + placeholder TEXT.
      // The overlay insets past the icon (text x) and carries the box bg color.
      baseNode({ name: "Group 59", figma_type: "GROUP", figma_node_id: "g59",
        absolute_bounds: { x: 21, y: 73, w: 184, h: 26 },
        children: [
          baseNode({ name: "Box", figma_type: "RECTANGLE",
            absolute_bounds: { x: 21, y: 73, w: 184, h: 26 },
            style: { background_color: "#252626" } }),
          baseNode({ name: "ic:round-search", figma_type: "FRAME",
            absolute_bounds: { x: 27, y: 76, w: 15, h: 15 } }),
          baseNode({ name: "Search", figma_type: "TEXT", content: "Search",
            absolute_bounds: { x: 44, y: 78, w: 43, h: 17 } }),
        ] }),
      // segmented control: 4 short-label buttons in a row; #3 carries a fill
      child("Pager", 220, 76, 120, 20, "tg", [
        child("Button", 220, 76, 29, 20, "b1", [txt("1")]),
        child("Button", 249, 76, 29, 20, "b2", [txt("2")]),
        baseNode({ figma_type: "FRAME", name: "Button", figma_node_id: "b3",
                   absolute_bounds: { x: 279, y: 76, w: 29, h: 20 },
                   style: { background_color: "#3c3d3d" }, children: [txt("3")] }),
        child("Button", 308, 76, 29, 20, "b4", [txt("4")]),
      ]),
    ],
  });
  const els = detectOverlayControls(root, [0, 0], [73, 50]);
  const search = els.filter((e) => e.kind === "text_field");
  const tabs = els.filter((e) => e.kind === "tab_group");
  assert.equal(search.length, 1);
  assert.equal(search[0].placeholder, "Search");
  assert.equal(search[0].source_node_id, "g59");          // the parent group
  // inset past the icon: x = 44+73 = 117, w = 21+184-44 = 161
  assert.deepEqual([search[0].x, search[0].y, search[0].w, search[0].h],
                   [117, 123, 161, 26]);
  assert.equal(search[0].bg_color, "#252626");            // box bg → seamless inset
  assert.equal(tabs.length, 1);
  assert.deepEqual(tabs[0].options, ["1", "2", "3", "4"]);
  assert.equal(tabs[0].selected_index, 2);  // the filled button
});
