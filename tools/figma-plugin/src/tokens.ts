// Variable / token extraction. Runs in the Figma sandbox.
//
// Walks all local + remote variable collections referenced by nodes in the
// scene and emits a flat tokens map in the shape of IRTokens:
//   tokens.colors[name]     → "#hex" or "rgba(…)"
//   tokens.dimensions[name] → number (px)
//   tokens.strings[name]    → string
//
// Variable modes: a variable can have different values per mode (light/dark,
// breakpoints). We capture EVERY mode — the default mode keeps the bare token
// name and each other mode is emitted under a "<name>.<mode>" suffix (e.g.
// "color.bg" + "color.bg.dark") so themed values survive import into Pulp's
// flat theme maps. Aliases are resolved per mode, so a semantic color that
// points at a different base-palette entry per mode yields the right value.

import type { ExtractedDiagnostic } from "./extract-model";

export interface ExtractedTokens {
  colors: Record<string, string>;
  dimensions: Record<string, number>;
  strings: Record<string, string>;
  /// Map of Figma variable id → canonical token name. Lets style extraction
  /// emit token references where a variable is bound to a style property.
  variableIdToName: Record<string, string>;
}

export async function extractTokens(diagnostics: ExtractedDiagnostic[]): Promise<ExtractedTokens> {
  const out: ExtractedTokens = {
    colors: {},
    dimensions: {},
    strings: {},
    variableIdToName: {},
  };

  // Local collections first
  let localCollections: VariableCollection[] = [];
  try {
    localCollections = await figma.variables.getLocalVariableCollectionsAsync();
  } catch {
    return out;
  }

  for (const coll of localCollections) {
    await ingestCollection(coll, out, diagnostics);
  }

  // Remote (library) collections — only those referenced by something in the
  // current document are actually fetchable. We skip exhaustive enumeration to
  // avoid hitting the library subscriber API by accident.

  return out;
}

async function ingestCollection(
  coll: VariableCollection,
  out: ExtractedTokens,
  diagnostics: ExtractedDiagnostic[],
): Promise<void> {
  const defaultModeId = coll.defaultModeId;
  if (coll.modes.length > 1) {
    const defaultName = coll.modes.find((m) => m.modeId === defaultModeId)?.name ?? "?";
    diagnostics.push({
      severity: "info",
      code: "variable-multi-mode",
      kind: "capture_partial",
      message: `Variable collection "${coll.name}" has ${coll.modes.length} modes; all are captured — the default mode "${defaultName}" uses the bare token name and each other mode is suffixed (e.g. "token.dark"). Cross-collection alias values resolve against the referent's default mode.`,
      path: `/tokens/collection/${coll.name}`,
    });
  }

  for (const varId of coll.variableIds) {
    let v: Variable | null = null;
    try {
      v = await figma.variables.getVariableByIdAsync(varId);
    } catch {
      continue;
    }
    if (!v) continue;
    const baseName = canonicalName(coll.name, v.name);
    // Style references bind to the default-mode (bare) token name.
    out.variableIdToName[v.id] = baseName;
    for (const mode of coll.modes) {
      const raw = v.valuesByMode[mode.modeId];
      if (raw === undefined) continue;
      // Default mode keeps the bare name (back-compat + the name style refs
      // resolve to); every other mode is captured under "<name>.<mode>" so
      // light/dark (and any multi-mode) values survive import.
      const slug = modeSlug(mode.name);
      const name = mode.modeId === defaultModeId || slug.length === 0
        ? baseName
        : `${baseName}.${slug}`;
      await assignToken(out, name, raw, v.resolvedType, mode.modeId);
    }
  }
}

function canonicalName(collectionName: string, varName: string): string {
  // Figma variable names often use "/" as a grouping separator. Normalize to dotted lowercase.
  const compose = `${collectionName}/${varName}`;
  return compose
    .toLowerCase()
    .replace(/\s+/g, "")
    .replace(/\//g, ".")
    .replace(/[^a-z0-9._-]/g, "");
}

function renderColorValue(raw: VariableValue): string {
  if (typeof raw === "object" && raw && "r" in raw && "g" in raw && "b" in raw) {
    const { r, g, b, a } = raw as RGBA;
    const rh = Math.round(r * 255).toString(16).padStart(2, "0");
    const gh = Math.round(g * 255).toString(16).padStart(2, "0");
    const bh = Math.round(b * 255).toString(16).padStart(2, "0");
    if (a === undefined || a >= 1) return `#${rh}${gh}${bh}`;
    return `rgba(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)}, ${a.toFixed(3)})`;
  }
  return "#000000";
}

/** Sanitize a Figma mode name into a token-name-safe slug (same rules as canonicalName). */
function modeSlug(modeName: string): string {
  return modeName.toLowerCase().replace(/\s+/g, "").replace(/[^a-z0-9._-]/g, "");
}

/**
 * Follow VARIABLE_ALIAS references for a given mode until a concrete value is
 * reached (bounded to avoid cycles). Resolving per-mode is what makes
 * multi-mode capture meaningful: a semantic variable usually aliases a
 * different base-palette entry in each mode (light vs dark). A referenced
 * variable in another collection won't share this collection's modeId, so we
 * fall back to the referent's own first/default mode value.
 */
async function resolveValue(
  raw: VariableValue,
  modeId: string,
  depth = 0,
): Promise<VariableValue> {
  if (depth >= 10) return raw;
  if (typeof raw === "object" && raw && "type" in raw && (raw as VariableAlias).type === "VARIABLE_ALIAS") {
    const referent = await figma.variables.getVariableByIdAsync((raw as VariableAlias).id);
    if (!referent) return raw;
    const next = referent.valuesByMode[modeId] ?? Object.values(referent.valuesByMode)[0];
    if (next === undefined) return raw;
    return resolveValue(next, modeId, depth + 1);
  }
  return raw;
}

/** Resolve + assign one variable's value for a given mode into the token maps. */
async function assignToken(
  out: ExtractedTokens,
  name: string,
  raw: VariableValue,
  resolvedType: Variable["resolvedType"],
  modeId: string,
): Promise<void> {
  const value = await resolveValue(raw, modeId);
  switch (resolvedType) {
    case "COLOR":
      if (typeof value === "object" && value && "r" in value) out.colors[name] = renderColorValue(value);
      break;
    case "FLOAT":
      if (typeof value === "number") out.dimensions[name] = value;
      break;
    case "STRING":
      if (typeof value === "string") out.strings[name] = value;
      break;
    case "BOOLEAN":
      // Booleans don't fit IRTokens cleanly; encode as "true" / "false" strings.
      if (typeof value === "boolean") out.strings[name] = value ? "true" : "false";
      break;
  }
}
