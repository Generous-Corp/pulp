// Resize-constraint extraction for the Figma plugin lane. Constraints are the
// resize contract of a node positioned in its parent's coordinate space
// (MIN/MAX/CENTER/STRETCH/SCALE per axis); the extractor passes the Plugin
// API's spelling through untranslated and the C++ importer
// (design_ir_json.cpp) owns normalization. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractConstraints } from "../src/extract-pure";

// extractConstraints reads only `constraints`; the cast keeps the fixture to
// the field under test rather than standing up a full SceneNode.
const sceneNode = (over: Record<string, unknown>) => over as unknown as SceneNode;

test("extractConstraints passes the Plugin API spelling through untranslated", () => {
  const c = extractConstraints(
    sceneNode({ constraints: { horizontal: "STRETCH", vertical: "SCALE" } }),
  );
  assert.deepEqual(c, { horizontal: "STRETCH", vertical: "SCALE" });
});

test("extractConstraints guards node types without the property", () => {
  // GROUP / SLICE nodes have no `constraints` member at all.
  assert.equal(extractConstraints(sceneNode({ type: "GROUP" })), undefined);
  // A present-but-null value (defensive: plugin API typings say non-null, the
  // wire has surprised us before) must not emit.
  assert.equal(extractConstraints(sceneNode({ constraints: null })), undefined);
  // Non-string members never become `constraints: {}` — the serializer's
  // truthy check would pass an empty object through to the envelope.
  assert.equal(extractConstraints(sceneNode({ constraints: {} })), undefined);
});

test("extractConstraints keeps a single present axis", () => {
  const c = extractConstraints(sceneNode({ constraints: { horizontal: "CENTER" } }));
  assert.deepEqual(c, { horizontal: "CENTER" });
});
