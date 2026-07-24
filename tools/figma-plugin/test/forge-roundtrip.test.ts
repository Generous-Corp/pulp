import { test } from "node:test";
import assert from "node:assert/strict";

import {
  decodeForgeDesignForFigma,
  encodeFigmaSelectionForForge,
  FORGE_CLIPBOARD_FORMAT,
} from "../src/forge-roundtrip";

test("Figma export clipboard embeds the scene and content-addressed assets", () => {
  const text = encodeFigmaSelectionForForge(
    "{\"format_version\":\"2026.05-figma-plugin-v1\"}",
    [{
      content_hash: "abc123",
      mime: "image/png",
      bytes: [0, 1, 2, 253, 254, 255],
    }],
  );
  const parsed = JSON.parse(text);
  assert.equal(parsed.format, FORGE_CLIPBOARD_FORMAT);
  assert.equal(parsed.kind, "figma-plugin-export");
  assert.equal(
    parsed.scene_json,
    "{\"format_version\":\"2026.05-figma-plugin-v1\"}",
  );
  assert.deepEqual(parsed.assets, [{
    content_hash: "abc123",
    mime: "image/png",
    base64: "AAEC/f7/",
  }]);
});

test("Forge DesignIR becomes an editable Figma plan with hierarchy and bindings intact", () => {
  const designIr = JSON.stringify({
    version: 1,
    root: {
      type: "frame",
      name: "Synth",
      style: { width: 640, height: 360, backgroundColor: "#101820" },
      layout: { direction: "row", gap: 16, paddingLeft: 20 },
      source_node_id: "1:1",
      children: [{
        type: "knob",
        name: "Cutoff",
        audioWidget: "knob",
        label: "Cutoff",
        min: 20,
        max: 20000,
        default: 880,
        style: { width: 72, height: 72, left: 24, top: 40 },
        layout: {},
        attributes: {
          binding: "filter.cutoff",
          pulpParamKey: "filter.cutoff",
          units: "Hz",
        },
        stable_anchor_id: "figma-plugin:2:4",
      }, {
        type: "text",
        name: "Heading",
        content: "FILTER",
        style: { width: 120, height: 24, color: "#ffffff", fontSize: 14 },
        layout: {},
      }],
    },
  });
  const plan = decodeForgeDesignForFigma(JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: designIr,
  }));

  assert.equal(plan.nodeCount, 3);
  assert.equal(plan.audioWidgetCount, 1);
  assert.equal(plan.root.name, "Synth");
  assert.equal(plan.root.layoutMode, "HORIZONTAL");
  assert.equal(plan.root.children[0].audioWidget, "knob");
  assert.equal(plan.root.children[0].binding, "filter.cutoff");
  assert.equal(plan.root.children[0].audioLabel, "Cutoff");
  assert.equal(plan.root.children[0].audioMin, 20);
  assert.equal(plan.root.children[0].audioMax, 20000);
  assert.equal(plan.root.children[0].audioDefault, 880);
  assert.equal(plan.root.children[0].audioUnits, "Hz");
  assert.equal(plan.root.children[0].stableAnchorId, "figma-plugin:2:4");
  assert.equal(plan.root.children[1].content, "FILTER");
});

test("Forge clipboard rejects ordinary JSON and unsupported widget claims", () => {
  assert.throws(
    () => decodeForgeDesignForFigma("{\"root\":{}}"),
    /does not contain/,
  );
  const designIr = JSON.stringify({
    root: {
      type: "frame",
      style: {},
      layout: {},
      audioWidget: "secret_widget",
    },
  });
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: designIr,
    })),
    /Unsupported audio widget/,
  );
});
