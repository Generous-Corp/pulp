// Exercises the REAL binding resolver
// (src/extract-pure.ts::extractBoundVariableBindings) and the serializer's
// figma.bound_variables emission. The gap this pins: tokens.ts built
// variableIdToName and nothing ever read node.boundVariables, so token
// DEFINITIONS survived export while every per-property BINDING was lost.

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractBoundVariableBindings } from "../src/extract-pure";
import { serializeExport, type SerializeContext } from "../src/serialize";
import type { ExtractedFigmaNode } from "../src/extract-model";
import { AssetCache } from "../src/assets";

const alias = (id: string) => ({ type: "VARIABLE_ALIAS", id });

const idToName = {
  "VariableID:1:1": "theme.brand.primary",
  "VariableID:1:2": "theme.radius.md",
  "VariableID:1:3": "theme.brand.secondary",
  "VariableID:1:4": "theme.label.gain",
};

test("single aliases and alias arrays resolve to token names", () => {
  const out = extractBoundVariableBindings(
    {
      // Array-valued property (fills/strokes carry one alias per paint).
      fills: [alias("VariableID:1:1"), alias("VariableID:1:3")],
      strokes: [alias("VariableID:1:3")],
      effects: [alias("VariableID:1:1")],
      // Scalar properties are single aliases.
      topLeftRadius: alias("VariableID:1:2"),
      // Nested alias map (componentProperties, textRangeFills).
      componentProperties: { "label#9:0": alias("VariableID:1:4") },
      textRangeFills: { "0:4": alias("VariableID:1:1") },
    },
    idToName,
  );
  assert.ok(out);
  assert.deepEqual(out.bindings, {
    // Index 0 keeps the bare property key; later entries are suffixed.
    fills: "theme.brand.primary",
    "fills.1": "theme.brand.secondary",
    strokes: "theme.brand.secondary",
    effects: "theme.brand.primary",
    topLeftRadius: "theme.radius.md",
    "componentProperties.label#9:0": "theme.label.gain",
    "textRangeFills.0:4": "theme.brand.primary",
  });
  assert.deepEqual(out.unresolved, []);
});

test("an unresolvable id is skipped and reported, never emitted dangling", () => {
  const out = extractBoundVariableBindings(
    {
      fills: [alias("VariableID:9:9")], // remote/deleted — not in the token map
      cornerRadius: alias("VariableID:1:2"),
    },
    idToName,
  );
  assert.ok(out);
  assert.deepEqual(out.bindings, { cornerRadius: "theme.radius.md" });
  assert.deepEqual(out.unresolved, ["VariableID:9:9"]);
});

test("no boundVariables (or nothing alias-shaped) yields undefined", () => {
  assert.equal(extractBoundVariableBindings(undefined, idToName), undefined);
  assert.equal(extractBoundVariableBindings(null, idToName), undefined);
  assert.equal(extractBoundVariableBindings({}, idToName), undefined);
  // Non-alias values (defensive: unknown future shapes) are ignored.
  assert.equal(extractBoundVariableBindings({ fills: "nonsense", other: 3 }, idToName), undefined);
});

// ── serializer emission ─────────────────────────────────────────────────────

function baseNode(over: Partial<ExtractedFigmaNode>): ExtractedFigmaNode {
  return {
    type: "frame",
    figma_type: "FRAME",
    name: "Node",
    figma_node_id: "1:2",
    parent_id: null,
    z_order: 0,
    absolute_bounds: { x: 0, y: 0, w: 100, h: 100 },
    relative_transform: [
      [1, 0, 0],
      [0, 1, 0],
    ],
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

function ctx(): SerializeContext {
  return {
    fileKey: "abc123",
    rootNodeId: "1:2",
    pluginVersion: "0.1.0",
    assets: new AssetCache(),
    tokens: {
      colors: { "theme.brand.primary": "#ff0000" },
      dimensions: { "theme.radius.md": 8 },
      strings: {},
      sourceIdentity: {
        "colors.theme.brand.primary": {
          sourceId: "VariableID:1:1",
          sourceCollection: "Theme",
          sourceMode: "Light",
          sourceAdapter: "figma-plugin",
        },
      },
      variableIdToName: idToName,
    },
  };
}

test("serializeExport emits figma.bound_variables only when bindings exist", () => {
  const bound = baseNode({
    name: "Bound",
    bound_variables: { fills: "theme.brand.primary", cornerRadius: "theme.radius.md" },
    children: [baseNode({ name: "Plain", figma_node_id: "1:3" })],
  });
  const env = serializeExport([bound], [], ctx()) as {
    tokens: {
      colors: Record<string, string>;
    };
    token_source_identity: Record<string, { sourceId: string }>;
    root: { figma: Record<string, unknown>; children: { figma: Record<string, unknown> }[] };
  };
  assert.deepEqual(env.root.figma.bound_variables, {
    fills: "theme.brand.primary",
    cornerRadius: "theme.radius.md",
  });
  // The binding names an entry in the envelope-level tokens maps.
  assert.equal(env.tokens.colors["theme.brand.primary"], "#ff0000");
  assert.equal(
    env.token_source_identity["colors.theme.brand.primary"].sourceId,
    "VariableID:1:1",
  );
  // A node without bindings must not grow the key.
  assert.ok(!("bound_variables" in env.root.children[0].figma));
});

test("multi-root wrapper never borrows the first child's Figma identity", () => {
  const first = baseNode({ figma_node_id: "1:2", name: "First" });
  const second = baseNode({ figma_node_id: "1:3", name: "Second" });
  const env = serializeExport([first, second], [], ctx()) as {
    root: {
      synthetic?: boolean;
      figma_node_id?: string;
      children: Array<{ figma_node_id: string }>;
    };
  };
  assert.equal(env.root.synthetic, true);
  assert.equal(env.root.figma_node_id, undefined);
  assert.deepEqual(
    env.root.children.map((child) => child.figma_node_id),
    ["1:2", "1:3"],
  );
});
