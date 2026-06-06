// Plan B / B4b: the faithful-vector lane for the Figma plugin. Exercises the
// geometry knob auto-detector + the sandbox-safe SVG byte decode, and asserts
// serialize.ts emits the render_mode / svg_asset_id / interactive_elements the
// C++ materializer (DesignFrameView) consumes. Kept in lockstep with the REST
// lane (tools/import-design/figma_rest_export.py + test_figma_rest_export.py).

import { test } from "node:test";
import assert from "node:assert/strict";

import { parseFrameKnobs, decodeSvgBytes } from "../src/faithful-vector";
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
