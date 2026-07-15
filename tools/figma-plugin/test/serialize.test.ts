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

// A recognized Pulp knob, as the extractor would produce it for a published
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

test("serializeExport: recognized knob emits root-level audio_widget + numeric fields", () => {
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
  if ("const" in sch && value !== sch.const) {
    errors.push(`${path}: value ${JSON.stringify(value)} !== const ${JSON.stringify(sch.const)}`);
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
  // allOf — every subschema must hold (used for the interactive_element
  // per-kind required-field segmentation).
  if (Array.isArray(sch.allOf)) {
    for (const sub of sch.allOf) validate(value, sub, root, path, errors);
  }
  // if/then/else — when `if` validates clean, apply `then`; otherwise `else`.
  if (sch.if) {
    const condErrors: string[] = [];
    validate(value, sch.if, root, path, condErrors);
    if (condErrors.length === 0) {
      if (sch.then) validate(value, sch.then, root, path, errors);
    } else if (sch.else) {
      validate(value, sch.else, root, path, errors);
    }
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

// ---------------------------------------------------------------------------
// The interactive_element schema must accept every kind the producers emit. This
// catches drift where the schema rejects overlay elements that faithful-vector.ts
// / figma_rest_export.py emit. Validates element objects directly against
// $defs.interactive_element, including the per-kind required-field segmentation
// (knob -> cx/cy/hit_radius; overlays -> x/y/w/h).
// ---------------------------------------------------------------------------

function ieErrors(el: any): string[] {
  const errors: string[] = [];
  validate(el, (schema as any).$defs.interactive_element, schema as any, "ie", errors);
  return errors;
}

test("interactive_element schema accepts every producer-emitted kind", () => {
  const knob = { kind: "knob", cx: 50, cy: 50, hit_radius: 22, svg_patch_d: "M50 38L50 30", default_value: 0.5, source_node_id: "1:1" };
  const fader = { kind: "fader", x: 10, y: 10, w: 8, h: 80, svg_patch_d: "M14 80L14 70", default_value: 0.3, cx: 14, cy: 75, source_node_id: "1:2" };
  const toggle = { kind: "toggle", x: 10, y: 10, w: 40, h: 20, default_value: 1, flash: true, source_node_id: "1:3" };
  const switchEl = { kind: "toggle", x: 10, y: 10, w: 40, h: 20, svg_patch_d: "M14 20a3 3 0 106 0", cx: 17, cy: 20, default_value: 0, source_node_id: "1:4" };
  const dropdown = { kind: "dropdown", x: 0, y: 0, w: 60, h: 20, options: ["Sine"], selected_index: 0, source_node_id: "1:5" };
  const textField = { kind: "text_field", x: 0, y: 0, w: 80, h: 16, placeholder: "Search", bg_color: "#1c1d1d", source_node_id: "1:6" };
  const tabGroup = { kind: "tab_group", x: 0, y: 0, w: 120, h: 24, options: ["A", "B"], selected_index: 1, source_node_id: "1:7" };
  const stepper = { kind: "stepper", x: 0, y: 0, w: 90, h: 20, options: ["Preset 1"], selected_index: 0, source_node_id: "1:8" };
  const swap = { kind: "swap", x: 0, y: 0, w: 60, h: 24, target_frame: 2, source_node_id: "1:9" };
  const action = { kind: "action", x: 0, y: 0, w: 30, h: 24, action: "octave_up", source_node_id: "1:10" };
  const xyPad = { kind: "xy_pad", x: 0, y: 0, w: 100, h: 100, default_value: 0.3, default_value_y: 0.7, source_node_id: "1:11" };
  const valueLabel = { kind: "value_label", x: 0, y: 0, w: 80, h: 16, text: "-6.0 dB", value_left_align: true, source_node_id: "1:12" };
  const custom = { kind: "custom", x: 0, y: 0, w: 60, h: 40, factory_id: "acme.spinner", custom_props: "{\"max\":11}", source_node_id: "1:13" };

  for (const el of [knob, fader, toggle, switchEl, dropdown, textField, tabGroup, stepper, swap, action, xyPad, valueLabel, custom]) {
    assert.deepEqual(ieErrors(el), [], `kind '${el.kind}' should validate but didn't`);
  }
});

test("interactive_element schema requires factory_id for a custom control", () => {
  // A custom control with no factory_id can't be built — the schema must reject it.
  const errs = ieErrors({ kind: "custom", x: 0, y: 0, w: 60, h: 40 });
  assert.ok(errs.some((e) => e.includes("'factory_id'")),
    `custom must require factory_id, got:\n${errs.join("\n")}`);
});

test("interactive_element schema accepts the import-report fields", () => {
  // A control carrying full resolution provenance validates against the schema.
  const conflicted = {
    kind: "knob", cx: 50, cy: 50, hit_radius: 20,
    resolution_rung: 2, confidence_score: 0.55,
    conflict_signals: ["name=knob but geometry is a wide track+thumb"],
    verification_pass: false, source_node_id: "9:9",
  };
  assert.deepEqual(ieErrors(conflicted), [], "report-bearing element should validate");
  // conflict_signals must be an array of strings — a bare string is rejected.
  const bad = { kind: "knob", cx: 0, cy: 0, hit_radius: 1, conflict_signals: "oops" };
  assert.ok(ieErrors(bad).some((e) => e.includes("expected type array")),
    "conflict_signals must be an array");
});

test("interactive_element schema rejects an unknown kind", () => {
  const errs = ieErrors({ kind: "wormhole", x: 0, y: 0, w: 10, h: 10 });
  assert.ok(errs.some((e) => e.includes("not in enum")), `expected enum rejection, got:\n${errs.join("\n")}`);
});

test("interactive_element schema enforces per-kind required fields", () => {
  // A knob missing its cx/cy/hit_radius fails (the knob allOf branch).
  const knobMissing = ieErrors({ kind: "knob" });
  assert.ok(knobMissing.some((e) => e.includes("'cx'")), `knob should require cx, got:\n${knobMissing.join("\n")}`);
  // An overlay kind missing its x/y/w/h box fails (the overlay allOf branch).
  const ddMissing = ieErrors({ kind: "dropdown", options: ["x"] });
  assert.ok(ddMissing.some((e) => e.includes("'x'")), `dropdown should require x, got:\n${ddMissing.join("\n")}`);
  // additionalProperties:false still bites a typo'd field.
  const typo = ieErrors({ kind: "toggle", x: 0, y: 0, w: 10, h: 10, wobble: 3 });
  assert.ok(typo.some((e) => e.includes("additionalProperties")), `expected additionalProperties rejection, got:\n${typo.join("\n")}`);
});

// ---------------------------------------------------------------------------
// User-supplied font bundling. When a UserFontCache carries a TTF for a (family,
// style) tuple matching a font_family_assets entry, the serializer must:
//   1. Stamp the entry with the cache's `asset_id`.
//   2. Add a corresponding entry to `asset_manifest.assets` so the zip
//      writer packages the bytes as `assets/<hash>.ttf`.
//   3. Leave NON-matching entries untouched (metadata-only).
// ---------------------------------------------------------------------------

import { UserFontCache } from "../src/user-fonts";

test("serializeExport stamps user-supplied font asset_id (#43c)", async () => {
  const cache = new UserFontCache();
  // SFNT TrueType magic (0x00010000) + filler bytes to clear the 12-byte minimum header.
  const bytes = new Uint8Array(16);
  bytes[0] = 0x00; bytes[1] = 0x01; bytes[2] = 0x00; bytes[3] = 0x00;
  for (let i = 4; i < 16; i++) bytes[i] = 0xab;
  const entry = await cache.add("Clash Grotesk", "Medium", bytes, "ClashGrotesk-Medium.ttf");

  const fontFamilyAssets = [
    { family: "Inter", style: "Regular", weight: 400 },
    { family: "Clash Grotesk", style: "Medium", weight: 500 },
    { family: "Clash Grotesk", style: "Bold", weight: 700 }, // intentionally unmatched
  ];

  const out = serializeExport([knobNode()], [], ctx({
    fontFamilyAssets,
    userFonts: cache,
  })) as Record<string, any>;

  // 1 — asset_id stamped on the matching (family, style) tuple only.
  const stamped = out.font_family_assets as Array<Record<string, any>>;
  assert.equal(stamped[0].asset_id, undefined, "Inter Regular had no user-supplied font");
  assert.equal(stamped[1].asset_id, entry.asset_id, "Clash Grotesk Medium matched");
  assert.equal(stamped[2].asset_id, undefined, "Clash Grotesk Bold had no user-supplied font");

  // 2 — bytes flow into asset_manifest with local_path ending in .ttf.
  const fontAssets = out.asset_manifest.assets.filter(
    (a: any) => a.asset_id === entry.asset_id,
  );
  assert.equal(fontAssets.length, 1, "exactly one asset_manifest entry for the font");
  assert.equal(fontAssets[0].mime, "font/ttf");
  assert.equal(fontAssets[0].content_hash, entry.content_hash);
  assert.ok(
    fontAssets[0].local_path.endsWith(".ttf"),
    `local_path should end in .ttf, got ${fontAssets[0].local_path}`,
  );

  // The metadata-only path still works when no cache is given.
  const noCacheOut = serializeExport([knobNode()], [], ctx({
    fontFamilyAssets,
  })) as Record<string, any>;
  const noStamp = noCacheOut.font_family_assets as Array<Record<string, any>>;
  for (const f of noStamp) {
    assert.equal(f.asset_id, undefined, `no cache → no asset_id stamp (got ${f.asset_id})`);
  }
});

test("UserFontCache sniffs SFNT magic for font/ttf vs font/otf (#43c)", async () => {
  const cache = new UserFontCache();

  // TrueType magic: 0x00 0x01 0x00 0x00 + enough bytes to clear the 12-byte minimum.
  const ttfBytes = new Uint8Array(16);
  ttfBytes[0] = 0x00; ttfBytes[1] = 0x01; ttfBytes[2] = 0x00; ttfBytes[3] = 0x00;
  const ttf = await cache.add("F", "R", ttfBytes, "any.ttf");
  assert.equal(ttf.mime, "font/ttf");

  // OpenType CFF: "OTTO" (0x4f 0x54 0x54 0x4f).
  const otfBytes = new Uint8Array(16);
  otfBytes[0] = 0x4f; otfBytes[1] = 0x54; otfBytes[2] = 0x54; otfBytes[3] = 0x4f;
  const otf = await cache.add("F", "B", otfBytes, "any.otf");
  assert.equal(otf.mime, "font/otf");

  // Filename fallback when bytes don't match a known magic but the filename
  // has a font extension — still accepted (older font tools or transcoded
  // assets can produce non-standard headers).
  const fallbackBytes = new Uint8Array(16); // arbitrary non-magic bytes
  const fallback = await cache.add("F", "I", fallbackBytes, "no-magic.otf");
  assert.equal(fallback.mime, "font/otf");
});

// ---------------------------------------------------------------------------
// User-font input validation hardening. Pulp's "no silent failures" rule:
// drag-drop UX gaps that let an empty / corrupt blob into the asset cache get
// caught here, not at runtime three systems away.
// ---------------------------------------------------------------------------

test("UserFontCache rejects 0-byte drops (#43c hardening)", async () => {
  const cache = new UserFontCache();
  await assert.rejects(
    async () => cache.add("Garbage", "Regular", new Uint8Array(0), "empty.ttf"),
    /empty/i,
    "0-byte drop must throw, not silently succeed",
  );
  assert.equal(cache.size(), 0, "rejected drop must not enter the cache");
});

test("UserFontCache rejects too-short drops (#43c hardening)", async () => {
  const cache = new UserFontCache();
  // 5 bytes — has SFNT magic prefix but truncated before the 12-byte header
  // SFNT needs (magic + numTables + searchRange + entrySelector + rangeShift =
  // 12 bytes minimum). Anything shorter is definitely not a font, regardless
  // of filename.
  const truncated = new Uint8Array([0x00, 0x01, 0x00, 0x00, 0xff]);
  await assert.rejects(
    async () => cache.add("Garbage", "Regular", truncated, "short.ttf"),
    /too short/i,
    "5-byte drop must throw — too short for any valid font header",
  );
  assert.equal(cache.size(), 0);
});

test("UserFontCache rejects non-font drops with no magic + no font extension (#43c hardening)", async () => {
  const cache = new UserFontCache();
  // Plain binary, no SFNT/WOFF magic, no font extension.
  const blob = new Uint8Array(64);
  blob.fill(0xab);
  await assert.rejects(
    async () => cache.add("Suspicious", "Regular", blob, "screenshot.png"),
    /doesn't match any known font format/i,
    "non-font binary with non-font filename must throw",
  );
  assert.equal(cache.size(), 0);
});

test("serializeExport emits userfont-orphan diagnostic for unmatched user fonts (#43c hardening)", async () => {
  const cache = new UserFontCache();
  const orphanBytes = new Uint8Array(16);
  orphanBytes[0] = 0x00; orphanBytes[1] = 0x01; orphanBytes[2] = 0x00; orphanBytes[3] = 0x00;
  await cache.add("Clash Grotesk", "Medium", orphanBytes, "ClashGrotesk-Medium.ttf");

  // The font_family_assets catalog is empty — the user dropped a TTF for a
  // family that no text node in this selection references (drop happened
  // before scan, or selection changed before export).
  const diagnostics: any[] = [];
  const out = serializeExport([knobNode()], diagnostics, ctx({
    fontFamilyAssets: [],
    userFonts: cache,
  })) as Record<string, any>;

  const env_diags = out.diagnostics as Array<Record<string, any>>;
  const orphan = env_diags.find((d) => d.code === "userfont-orphan");
  assert.ok(orphan, `expected userfont-orphan diagnostic, got: ${JSON.stringify(env_diags)}`);
  assert.equal(orphan.severity, "info");
  assert.equal(orphan.kind, "fallback_used");
  assert.match(orphan.message, /Clash Grotesk Medium/, "diagnostic identifies the orphan family");
  assert.match(orphan.message, /ClashGrotesk-Medium\.ttf/, "diagnostic identifies the source filename");

  // The bytes still appear in asset_manifest — orphan is a warning, not a drop.
  const assetEntry = (out.asset_manifest.assets as Array<Record<string, any>>).find(
    (a) => a.asset_id.startsWith("userfont-"),
  );
  assert.ok(assetEntry, "orphan bytes still ride in asset_manifest by content_hash");
});

test("serializeExport does NOT emit userfont-orphan when every cached font is referenced (#43c hardening)", async () => {
  const cache = new UserFontCache();
  const okBytes = new Uint8Array(16);
  okBytes[0] = 0x00; okBytes[1] = 0x01; okBytes[2] = 0x00; okBytes[3] = 0x00;
  await cache.add("Inter", "Regular", okBytes, "Inter-Regular.ttf");

  const diagnostics: any[] = [];
  const out = serializeExport([knobNode()], diagnostics, ctx({
    fontFamilyAssets: [{ family: "Inter", style: "Regular", weight: 400 }],
    userFonts: cache,
  })) as Record<string, any>;

  const env_diags = out.diagnostics as Array<Record<string, any>>;
  assert.equal(
    env_diags.filter((d) => d.code === "userfont-orphan").length,
    0,
    "no orphan diagnostic when every cached font matches a catalog entry",
  );
});
