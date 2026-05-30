// Exercises the REAL emitter (src/serialize.ts::serializeExport) end-to-end and
// asserts the de-facto wire shape the C++ importer
// (core/view/src/design_ir_json.cpp::parse_ir_node) consumes: root-level
// audio_widget / label / min / max / default, attributes.binding, and the
// figma.library_widget_kind sub-object. It also validates a serialized knob
// against schema/figma-plugin-export-v1.json so emitter ↔ schema can't drift.

import { test } from "node:test";
import assert from "node:assert/strict";

import { serializeExport, type SerializeContext } from "../src/serialize";
import type { ExtractedFigmaNode } from "../src/extract-model";
import { AssetCache } from "../src/assets";
import schema from "../schema/figma-plugin-export-v1.json";

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

function ctx(over: Partial<SerializeContext> = {}): SerializeContext {
  return {
    fileKey: "abc123",
    rootNodeId: "1:2",
    pluginVersion: "0.1.0",
    assets: new AssetCache(),
    tokens: { colors: {}, dimensions: {}, strings: {} },
    ...over,
  };
}

// A recognised Pulp knob, as the extractor would produce it for a published
// "Pulp / Knob" instance (library kind stamped, structured audio props +
// binding/units extracted from component properties).
function knobNode(): ExtractedFigmaNode {
  return baseNode({
    type: "knob",
    figma_type: "INSTANCE",
    name: "Pulp / Knob",
    figma_node_id: "3:74",
    component_key: "f74264ffa9108521fb0d3398dc8f5ea88e23a84e",
    main_component_name: "Pulp / Knob",
    library_widget_kind: "knob",
    library_version: "0.2.0",
    audio_label: "Cutoff",
    audio_min: 20,
    audio_max: 20000,
    audio_default: 1000,
    audio_units: "Hz",
    audio_binding: "cutoff",
    asset_ref: "img-deadbeef0001",
  });
}

function rootOf(out: unknown): Record<string, any> {
  const env = out as Record<string, any>;
  return env.root as Record<string, any>;
}

test("serializeExport: recognised knob emits root-level audio_widget + numeric fields", () => {
  const out = serializeExport([knobNode()], [], ctx());
  const root = rootOf(out);

  assert.equal(root.type, "knob");
  assert.equal(root.audio_widget, "knob");
  assert.equal(root.label, "Cutoff");
  assert.equal(root.min, 20);
  assert.equal(root.max, 20000);
  assert.equal(root.default, 1000);
});

test("serializeExport: binding + units land in attributes (NOT a binding sub-object)", () => {
  const out = serializeExport([knobNode()], [], ctx());
  const root = rootOf(out);

  assert.deepEqual(root.attributes, { units: "Hz", binding: "cutoff" });
  // Make sure the legacy nested shape is NOT emitted.
  assert.equal(root.binding, undefined);
  assert.equal(root.audio, undefined);
});

test("serializeExport: figma sub-object carries library_widget_kind + component_key", () => {
  const out = serializeExport([knobNode()], [], ctx());
  const root = rootOf(out);

  assert.ok(root.figma, "expected a figma sub-object");
  assert.equal(root.figma.library_widget_kind, "knob");
  assert.equal(root.figma.component_key, "f74264ffa9108521fb0d3398dc8f5ea88e23a84e");
  assert.equal(root.figma.library_version, "0.2.0");
  // The six always-present positional fields.
  assert.equal(root.figma.parent_id, null);
  assert.equal(root.figma.z_order, 0);
  assert.equal(root.figma.visible, true);
  assert.equal(root.figma.locked, false);
  assert.equal(root.figma.blend_mode, "NORMAL");
  assert.deepEqual(root.figma.absolute_transform, [
    [1, 0, 0],
    [0, 1, 0],
  ]);
});

test("serializeExport: envelope carries provenance + format_version", () => {
  const out = serializeExport([knobNode()], [], ctx()) as Record<string, any>;
  assert.equal(out.format_version, "2026.05-figma-plugin-v1");
  assert.equal(out.provenance.adapter, "figma-plugin");
  assert.equal(out.provenance.version, "0.1.0");
  assert.equal(out.provenance.source_uri, "figma://abc123/1:2");
});

test("serializeExport: a plain frame serializes WITHOUT audio_widget (generic)", () => {
  const frame = baseNode({
    type: "frame",
    name: "Panel",
    figma_node_id: "5:9",
  });
  const out = serializeExport([frame], [], ctx());
  const root = rootOf(out);

  assert.equal(root.type, "frame");
  assert.equal(root.audio_widget, undefined);
  assert.equal(root.label, undefined);
  assert.equal(root.min, undefined);
  assert.equal(root.max, undefined);
  assert.equal(root.default, undefined);
  assert.equal(root.attributes, undefined);
});

// ---------------------------------------------------------------------------
// Emitter ↔ schema agreement. A tiny structural validator over the subset of
// JSON Schema the export uses (type, enum, required, properties, $ref,
// additionalProperties for the node-level objects we assert on). Proves the
// serialized knob conforms to the updated schema/figma-plugin-export-v1.json.
// ---------------------------------------------------------------------------

function resolveRef(ref: string, root: any): any {
  // Only local "#/$defs/<name>" refs are used in this schema.
  const m = /^#\/\$defs\/(.+)$/.exec(ref);
  if (!m) throw new Error(`unsupported $ref: ${ref}`);
  return root.$defs[m[1]];
}

function validate(value: any, sch: any, root: any, path: string, errors: string[]): void {
  if (sch.$ref) {
    validate(value, resolveRef(sch.$ref, root), root, path, errors);
    return;
  }
  const types = Array.isArray(sch.type) ? sch.type : sch.type ? [sch.type] : [];
  if (types.length > 0) {
    const ok = types.some((t: string) => matchesType(value, t));
    if (!ok) {
      errors.push(`${path}: expected type ${types.join("|")}, got ${value === null ? "null" : typeof value}`);
      return;
    }
  }
  if (sch.enum && !sch.enum.includes(value)) {
    errors.push(`${path}: value ${JSON.stringify(value)} not in enum ${JSON.stringify(sch.enum)}`);
  }
  if (matchesType(value, "object") && value !== null) {
    if (Array.isArray(sch.required)) {
      for (const r of sch.required) {
        if (!(r in value)) errors.push(`${path}: missing required property '${r}'`);
      }
    }
    const props = sch.properties ?? {};
    for (const [k, v] of Object.entries(value)) {
      if (props[k]) {
        validate(v, props[k], root, `${path}/${k}`, errors);
      } else if (sch.additionalProperties === false) {
        errors.push(`${path}: unexpected property '${k}' (additionalProperties:false)`);
      } else if (sch.additionalProperties && typeof sch.additionalProperties === "object") {
        validate(v, sch.additionalProperties, root, `${path}/${k}`, errors);
      }
    }
  }
  if (sch.type === "array" && Array.isArray(value) && sch.items) {
    value.forEach((el: any, i: number) => validate(el, sch.items, root, `${path}[${i}]`, errors));
  }
}

function matchesType(value: any, t: string): boolean {
  switch (t) {
    case "object": return typeof value === "object" && value !== null && !Array.isArray(value);
    case "array": return Array.isArray(value);
    case "string": return typeof value === "string";
    case "number": return typeof value === "number";
    case "integer": return typeof value === "number" && Number.isInteger(value);
    case "boolean": return typeof value === "boolean";
    case "null": return value === null;
    default: return true;
  }
}

test("serialized knob validates against figma-plugin-export-v1 schema", () => {
  const out = serializeExport([knobNode()], [], ctx());
  const errors: string[] = [];
  validate(out, schema as any, schema as any, "$", errors);
  assert.deepEqual(errors, [], `schema violations:\n${errors.join("\n")}`);

  // And specifically the node-level shape we care about.
  const nodeErrors: string[] = [];
  validate(rootOf(out), (schema as any).$defs.node, schema as any, "root", nodeErrors);
  assert.deepEqual(nodeErrors, [], `node schema violations:\n${nodeErrors.join("\n")}`);
});
