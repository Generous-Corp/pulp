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
  assert.ok(!("spacing.gap.mode1" in tokens.dimensions));
  assert.ok(!diagnostics.some((d) => d.code === "variable-multi-mode"));
});
