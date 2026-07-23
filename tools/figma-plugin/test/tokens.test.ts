// Exercises the REAL token extractor (src/tokens.ts::extractTokens) against a
// minimal Figma Variables API mock. Focus: multi-mode capture — every variable
// mode is emitted (default mode under the bare token name, other modes under a
// "<name>.<mode>" suffix) and aliases resolve per mode so a semantic color that
// points at a different base-palette entry per mode yields the right value.

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractTokens } from "../src/tokens";

type Mode = { modeId: string; name: string };
type Coll = { name: string; defaultModeId: string; modes: Mode[]; variableIds: string[] };
type Var = { id: string; name: string; resolvedType: string; valuesByMode: Record<string, unknown> };

const white = { r: 1, g: 1, b: 1, a: 1 };
const black = { r: 0, g: 0, b: 0, a: 1 };
const alias = (id: string) => ({ type: "VARIABLE_ALIAS", id });

function installFigma(collections: Coll[], variables: Record<string, Var>): void {
  (globalThis as unknown as { figma: unknown }).figma = {
    variables: {
      getLocalVariableCollectionsAsync: async () => collections,
      getVariableByIdAsync: async (id: string) => variables[id] ?? null,
    },
  };
}

test("multi-mode variables: default bare + suffixed non-default modes, per-mode alias resolution", async () => {
  const variables: Record<string, Var> = {
    v_bg: { id: "v_bg", name: "bg", resolvedType: "COLOR", valuesByMode: { m_light: white, m_dark: black } },
    v_radius: { id: "v_radius", name: "radius", resolvedType: "FLOAT", valuesByMode: { m_light: 8, m_dark: 8 } },
    // Semantic color aliases a different base-palette entry per mode.
    v_semantic: {
      id: "v_semantic", name: "semantic", resolvedType: "COLOR",
      valuesByMode: { m_light: alias("v_base_white"), m_dark: alias("v_base_black") },
    },
    // Base palette lives in its own single-mode collection (modeId differs), so
    // resolveValue must fall back to the referent's first/default mode value.
    v_base_white: { id: "v_base_white", name: "base/white", resolvedType: "COLOR", valuesByMode: { m_base: white } },
    v_base_black: { id: "v_base_black", name: "base/black", resolvedType: "COLOR", valuesByMode: { m_base: black } },
  };
  const theme: Coll = {
    name: "Theme",
    defaultModeId: "m_light",
    modes: [{ modeId: "m_light", name: "Light" }, { modeId: "m_dark", name: "Dark" }],
    variableIds: ["v_bg", "v_radius", "v_semantic"],
  };
  installFigma([theme], variables);

  const diagnostics: Array<{ code: string }> = [];
  const tokens = await extractTokens(diagnostics as never);

  // Default (Light) → bare name; Dark → ".dark" suffix.
  assert.equal(tokens.colors["theme.bg"], "#ffffff");
  assert.equal(tokens.colors["theme.bg.dark"], "#000000");

  // FLOAT captured per mode.
  assert.equal(tokens.dimensions["theme.radius"], 8);
  assert.equal(tokens.dimensions["theme.radius.dark"], 8);

  // Per-mode alias resolution: white in light, black in dark.
  assert.equal(tokens.colors["theme.semantic"], "#ffffff");
  assert.equal(tokens.colors["theme.semantic.dark"], "#000000");

  // Style refs bind to the bare (default-mode) token name.
  assert.equal(tokens.variableIdToName["v_bg"], "theme.bg");

  // Every emitted value retains its original Figma identity. The map key uses
  // DesignIR's category-qualified convention, while bound_variables continues
  // to name the bare canonical token ("theme.bg").
  assert.deepEqual(tokens.sourceIdentity["colors.theme.bg"], {
    sourceId: "v_bg",
    sourceCollection: "Theme",
    sourceMode: "Light",
    sourceAdapter: "figma-plugin",
  });
  assert.deepEqual(tokens.sourceIdentity["colors.theme.bg.dark"], {
    sourceId: "v_bg",
    sourceCollection: "Theme",
    sourceMode: "Dark",
    sourceAdapter: "figma-plugin",
  });
  assert.deepEqual(tokens.sourceIdentity["dimensions.theme.radius"], {
    sourceId: "v_radius",
    sourceCollection: "Theme",
    sourceMode: "Light",
    sourceAdapter: "figma-plugin",
  });

  // Multi-mode collections emit the informational expansion diagnostic.
  assert.ok(diagnostics.some((d) => d.code === "variable-multi-mode"));
});

test("single-mode collection: bare names only, no suffix, no multi-mode diagnostic", async () => {
  const variables: Record<string, Var> = {
    v_gap: { id: "v_gap", name: "gap", resolvedType: "FLOAT", valuesByMode: { m_only: 16 } },
  };
  const spacing: Coll = {
    name: "Spacing",
    defaultModeId: "m_only",
    modes: [{ modeId: "m_only", name: "Mode 1" }],
    variableIds: ["v_gap"],
  };
  installFigma([spacing], variables);

  const diagnostics: Array<{ code: string }> = [];
  const tokens = await extractTokens(diagnostics as never);

  assert.equal(tokens.dimensions["spacing.gap"], 16);
  assert.equal(tokens.sourceIdentity["dimensions.spacing.gap"].sourceId, "v_gap");
  assert.ok(!("spacing.gap.mode1" in tokens.dimensions));
  assert.ok(!diagnostics.some((d) => d.code === "variable-multi-mode"));
});

test("colliding and empty mode slugs retain every mode without replacing the default", async () => {
  const variables: Record<string, Var> = {
    v_gap: {
      id: "v_gap",
      name: "gap",
      resolvedType: "FLOAT",
      valuesByMode: { light: 1, spaced: 2, compact: 3, emoji: 4 },
    },
  };
  installFigma([{
    name: "Spacing",
    defaultModeId: "light",
    modes: [
      { modeId: "light", name: "Light" },
      { modeId: "spaced", name: "Dark Mode" },
      { modeId: "compact", name: "darkmode" },
      { modeId: "emoji", name: "🌙" },
    ],
    variableIds: ["v_gap"],
  }], variables);

  const tokens = await extractTokens([] as never);
  const entries = Object.entries(tokens.dimensions)
    .filter(([name]) => name.startsWith("spacing.gap"));

  assert.equal(tokens.dimensions["spacing.gap"], 1);
  assert.equal(entries.length, 4);
  assert.deepEqual(new Set(entries.map(([, value]) => value)), new Set([1, 2, 3, 4]));
  for (const [name] of entries) {
    assert.equal(tokens.sourceIdentity[`dimensions.${name}`].sourceId, "v_gap");
  }
  assert.equal(tokens.variableIdToName.v_gap, "spacing.gap");
});

test("failed assignment cannot steal provenance from a colliding earlier token", async () => {
  const variables: Record<string, Var> = {
    valid: {
      id: "valid", name: "same", resolvedType: "COLOR",
      valuesByMode: { m: white },
    },
    invalid: {
      id: "invalid", name: "same", resolvedType: "COLOR",
      // Wrong literal type: assignToken must fail closed.
      valuesByMode: { m: "not-a-color" },
    },
  };
  installFigma([{
    name: "Theme",
    defaultModeId: "m",
    modes: [{ modeId: "m", name: "Light" }],
    variableIds: ["valid", "invalid"],
  }], variables);

  const tokens = await extractTokens([] as never);
  assert.equal(tokens.colors["theme.same"], "#ffffff");
  assert.equal(tokens.sourceIdentity["colors.theme.same"].sourceId, "valid");
  assert.equal(tokens.variableIdToName.valid, "theme.same");
  assert.equal(tokens.variableIdToName.invalid, undefined);
});

test("a valid canonical-name collision leaves only the current token owner bindable", async () => {
  const variables: Record<string, Var> = {
    first: {
      id: "first", name: "same", resolvedType: "COLOR",
      valuesByMode: { m: white },
    },
    winner: {
      id: "winner", name: "same", resolvedType: "COLOR",
      valuesByMode: { m: black },
    },
  };
  installFigma([{
    name: "Theme",
    defaultModeId: "m",
    modes: [{ modeId: "m", name: "Light" }],
    variableIds: ["first", "winner"],
  }], variables);

  const tokens = await extractTokens([] as never);
  assert.equal(tokens.colors["theme.same"], "#000000");
  assert.equal(tokens.sourceIdentity["colors.theme.same"].sourceId, "winner");
  assert.equal(tokens.variableIdToName.first, undefined);
  assert.equal(tokens.variableIdToName.winner, "theme.same");
});

test("same canonical name in different token categories keeps both ids bindable", async () => {
  const variables: Record<string, Var> = {
    color: {
      id: "color", name: "same", resolvedType: "COLOR",
      valuesByMode: { m: white },
    },
    dimension: {
      id: "dimension", name: "same", resolvedType: "FLOAT",
      valuesByMode: { m: 8 },
    },
  };
  installFigma([{
    name: "Theme",
    defaultModeId: "m",
    modes: [{ modeId: "m", name: "Light" }],
    variableIds: ["color", "dimension"],
  }], variables);

  const tokens = await extractTokens([] as never);
  assert.equal(tokens.sourceIdentity["colors.theme.same"].sourceId, "color");
  assert.equal(tokens.sourceIdentity["dimensions.theme.same"].sourceId, "dimension");
  assert.equal(tokens.variableIdToName.color, "theme.same");
  assert.equal(tokens.variableIdToName.dimension, "theme.same");
});

test("mode-suffix/base collisions invalidate only the owner of the overwritten path", async () => {
  const variables: Record<string, Var> = {
    bg: {
      id: "bg", name: "bg", resolvedType: "COLOR",
      valuesByMode: { light: white, dark: black },
    },
    bgDark: {
      id: "bgDark", name: "bg/dark", resolvedType: "COLOR",
      valuesByMode: { light: white, dark: white },
    },
  };
  const collection = (order: string[]): Coll => ({
    name: "Theme",
    defaultModeId: "light",
    modes: [
      { modeId: "light", name: "Light" },
      { modeId: "dark", name: "Dark" },
    ],
    variableIds: order,
  });

  // bg/dark's DEFAULT overwrites bg's non-default suffix; bg's bare default
  // remains a valid binding.
  installFigma([collection(["bg", "bgDark"])], variables);
  let tokens = await extractTokens([] as never);
  assert.equal(tokens.variableIdToName.bg, "theme.bg");
  assert.equal(tokens.variableIdToName.bgDark, "theme.bg.dark");

  // In reverse order bg's non-default suffix overwrites bg/dark's DEFAULT,
  // so bg/dark must stop resolving while bg's bare default remains valid.
  installFigma([collection(["bgDark", "bg"])], variables);
  tokens = await extractTokens([] as never);
  assert.equal(tokens.variableIdToName.bg, "theme.bg");
  assert.equal(tokens.variableIdToName.bgDark, undefined);
});
