// Mixed styled text ranges in the plugin lane. The contract under test:
// getStyledTextSegments-backed capture emits ordered per-range style DELTAS
// against the node's dominant style, with [start,end) as UTF-8 BYTE offsets
// into `content` (converted from the API's UTF-16 code-unit indices) — the
// exact shape design_ir_json.cpp::parse_ir_text_runs reads and the REST lane
// (figma_rest_export.py::extract_text_runs) already emits. Homogeneous text
// keeps the flat single-style path (no runs array). Kept in lockstep with the
// .fig lane (tools/import-design/fig/scene.mjs::extractFigTextRuns).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractScene } from "../src/extract";
import { utf16ToUtf8ByteOffsets } from "../src/extract-pure";

// extractScene's token pass reaches for the figma sandbox global; an empty
// variables surface keeps the walk itself pure (same stub as tokens.test.ts).
(globalThis as unknown as { figma: unknown }).figma = {
  variables: { getLocalVariableCollectionsAsync: async () => [] },
};

const bounds = (x: number, y: number, w: number, h: number) => ({
  x, y, width: w, height: h,
});

const solidFill = (r: number, g: number, b: number) => ([
  { type: "SOLID", visible: true, color: { r, g, b }, opacity: 1 },
]);

// A synthetic TextNode whose node-level properties read `figma.mixed` (a
// symbol, matching the real API for mixed nodes) and whose styled segments
// come from the test. "Héllo " is 7 UTF-8 bytes across 6 UTF-16 units — the
// offset conversion is the point of the fixture.
const MIXED = Symbol("figma.mixed");
function mixedTextNode(): SceneNode {
  const characters = "Héllo world";
  return {
    type: "TEXT",
    id: "1:2",
    name: "Caption",
    visible: true,
    absoluteBoundingBox: bounds(0, 0, 120, 16),
    characters,
    fontSize: MIXED,
    fontName: MIXED,
    fontWeight: MIXED,
    letterSpacing: MIXED,
    lineHeight: MIXED,
    textAlignHorizontal: "LEFT",
    textAlignVertical: "CENTER",
    textAutoResize: "NONE",
    fills: MIXED,
    getStyledTextSegments: (_fields: string[]) => [
      {
        characters: "Héllo ",
        start: 0,
        end: 6,   // UTF-16 units
        fontSize: 12,
        fontName: { family: "Inter", style: "Bold" },
        fontWeight: 700,
        fills: solidFill(1, 0, 0),
        letterSpacing: { value: 0, unit: "PIXELS" },
        textDecoration: "NONE",
      },
      {
        characters: "world",
        start: 6,
        end: 11,
        fontSize: 12,
        fontName: { family: "Inter", style: "Regular" },
        fontWeight: 400,
        fills: solidFill(1, 0, 0),
        letterSpacing: { value: 0, unit: "PIXELS" },
        textDecoration: "UNDERLINE",
      },
    ],
  } as unknown as SceneNode;
}

function frameWith(child: unknown): SceneNode {
  return {
    type: "FRAME",
    id: "1:1",
    name: "Panel",
    visible: true,
    absoluteBoundingBox: bounds(0, 0, 400, 300),
    children: [child],
  } as unknown as SceneNode;
}

test("utf16ToUtf8ByteOffsets maps BMP and astral characters to byte offsets", () => {
  // "aé€😀b": a=1 byte/1 unit, é=2/1, €=3/1, 😀=4/2 (surrogate pair), b=1/1.
  const map = utf16ToUtf8ByteOffsets("aé€😀b");
  assert.deepEqual(map, [0, 1, 3, 6, 6, 10, 11]);
});

test("mixed segments become ordered runs with UTF-8 byte offsets and dominant backfill", async () => {
  const res = await extractScene([frameWith(mixedTextNode())], { faithfulVector: false });
  const text = res.roots[0].children[0];

  assert.equal(text.type, "text");
  assert.equal(text.content, "Héllo world");
  // Dominant style backfills from segment 0 where node-level reads were mixed.
  assert.equal(text.style.font_size, 12);
  assert.equal(text.style.font_weight, 700);
  assert.equal(text.style.font_family, "Inter");
  assert.equal(text.style.color, "#ff0000");

  // Segment 0 IS the dominant style → no run; segment 1's delta (weight 400,
  // underline) covers "world" at UTF-8 bytes [7,12) — 6 UTF-16 units would be
  // byte 7 because of the two-byte é.
  assert.ok(text.runs);
  assert.equal(text.runs.length, 1);
  assert.deepEqual(text.runs[0], {
    start: 7,
    end: 12,
    fontWeight: 400,
    textDecoration: "underline",
  });
});

test("textAlignVertical CENTER lands as style.vertical_align middle", async () => {
  const res = await extractScene([frameWith(mixedTextNode())], { faithfulVector: false });
  assert.equal(res.roots[0].children[0].style.vertical_align, "middle");
});

test("homogeneous text keeps the flat path: no runs array", async () => {
  const plain = {
    type: "TEXT",
    id: "1:3",
    name: "Plain",
    visible: true,
    absoluteBoundingBox: bounds(0, 0, 80, 16),
    characters: "Just text",
    fontSize: 12,
    fontName: { family: "Inter", style: "Regular" },
    fontWeight: 400,
    textAlignVertical: "TOP",
    fills: solidFill(0, 0, 0),
    getStyledTextSegments: (_fields: string[]) => [
      {
        characters: "Just text",
        start: 0,
        end: 9,
        fontSize: 12,
        fontName: { family: "Inter", style: "Regular" },
        fontWeight: 400,
        fills: solidFill(0, 0, 0),
        letterSpacing: { value: 0, unit: "PIXELS" },
        textDecoration: "NONE",
      },
    ],
  } as unknown as SceneNode;
  const res = await extractScene([frameWith(plain)], { faithfulVector: false });
  const text = res.roots[0].children[0];
  assert.equal(text.runs, undefined);
  assert.equal(text.style.vertical_align, "top");
});

test("truncation and max-lines are preserved as namespaced attributes", async () => {
  const truncated = {
    type: "TEXT",
    id: "1:4",
    name: "Ellipsized",
    visible: true,
    absoluteBoundingBox: bounds(0, 0, 80, 16),
    characters: "A very long caption",
    fontSize: 12,
    fontName: { family: "Inter", style: "Regular" },
    fills: solidFill(0, 0, 0),
    textAutoResize: "HEIGHT",
    textTruncation: "ENDING",
    maxLines: 2,
  } as unknown as SceneNode;
  const res = await extractScene([frameWith(truncated)], { faithfulVector: false });
  const text = res.roots[0].children[0];
  assert.equal(text.attributes?.["figma:text_auto_resize"], "height");
  assert.equal(text.attributes?.["figma:text_truncation"], "ending");
  assert.equal(text.attributes?.["figma:max_lines"], "2");
});
