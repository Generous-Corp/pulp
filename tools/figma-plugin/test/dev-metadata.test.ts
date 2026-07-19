// Dev metadata + export settings for the Figma plugin lane: component
// descriptions, dev-ready status, Dev Mode annotations, and authored export
// settings are preserved as namespaced figma:* attributes. PROVENANCE-ONLY by
// design (audit "Dev metadata" / "Export settings" rows) — nothing renders
// from these, and export settings never override Pulp's deterministic PNG/SVG
// capture policy. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs + fig.test.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractDevMetadataAttrs } from "../src/extract-pure";

// extractDevMetadataAttrs reads only the guarded dev-metadata fields; the cast
// keeps each fixture to the fields under test rather than a full SceneNode.
const sceneNode = (over: Record<string, unknown>) =>
  ({ name: "N", type: "FRAME", ...over }) as unknown as SceneNode;

test("a COMPONENT's description and devStatus emit trimmed / lowercased attrs", () => {
  const attrs = extractDevMetadataAttrs(sceneNode({
    type: "COMPONENT",
    description: "  Primary gain knob. Bind to param.gain.  ",
    devStatus: { type: "READY_FOR_DEV" },
  }));
  assert.ok(attrs);
  assert.equal(attrs["figma:description"], "Primary gain knob. Bind to param.gain.");
  assert.equal(attrs["figma:dev_status"], "ready_for_dev");
});

test("a node without dev metadata emits nothing — absence stays silent", () => {
  // Presence checks, not defaults: an empty description, a null devStatus,
  // empty annotation/export arrays all preserve nothing.
  assert.equal(extractDevMetadataAttrs(sceneNode({})), undefined);
  assert.equal(
    extractDevMetadataAttrs(sceneNode({
      type: "COMPONENT",
      description: "   ",
      devStatus: null,
      annotations: [],
      exportSettings: [],
    })),
    undefined,
  );
});

test("annotations emit a compact JSON array with label fallback and camelCase properties", () => {
  const attrs = extractDevMetadataAttrs(sceneNode({
    annotations: [
      { label: "Use the shared knob track", properties: [{ type: "fills" }, { type: "itemSpacing" }] },
      { labelMarkdown: "**Spacing** is a token", categoryId: "cat-7" },
      {}, // nothing to state → dropped, not serialized as an empty object
    ],
  }));
  assert.ok(attrs);
  assert.equal(
    attrs["figma:annotations"],
    '[{"label":"Use the shared knob track","properties":["fills","itemSpacing"]},'
      + '{"label":"**Spacing** is a token","category_id":"cat-7"}]',
  );
});

test("export settings emit format/suffix/constraint/contents_only, defaults silent", () => {
  const attrs = extractDevMetadataAttrs(sceneNode({
    exportSettings: [
      // The everything-default preset: PNG, scale 1, no suffix, contentsOnly
      // true → only the format survives.
      { format: "PNG", constraint: { type: "SCALE", value: 1 }, contentsOnly: true },
      { format: "JPG", suffix: "@2x", constraint: { type: "SCALE", value: 2 }, contentsOnly: false },
      { format: "SVG", constraint: { type: "WIDTH", value: 512 } },
    ],
  }));
  assert.ok(attrs);
  assert.equal(
    attrs["figma:export_settings"],
    '[{"format":"png"},'
      + '{"format":"jpg","suffix":"@2x","constraint":"scale:2","contents_only":false},'
      + '{"format":"svg","constraint":"width:512"}]',
  );
});
