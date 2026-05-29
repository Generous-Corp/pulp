// Variable / token extraction. Runs in the Figma sandbox.
//
// Walks all local + remote variable collections referenced by nodes in the
// scene and emits a flat tokens map in the shape of IRTokens:
//   tokens.colors[name]     → "#hex" or "rgba(…)"
//   tokens.dimensions[name] → number (px)
//   tokens.strings[name]    → string
//
// Variable modes: a variable can have different values per mode (light/dark,
// breakpoints). For v1 we capture the DEFAULT mode only and log a
// `capture_partial` diagnostic when other modes exist. Multi-mode support is a
// follow-up slice.

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
    diagnostics.push({
      severity: "info",
      code: "variable-multi-mode",
      kind: "capture_partial",
      message: `Variable collection "${coll.name}" has ${coll.modes.length} modes; only the default mode "${coll.modes.find((m) => m.modeId === defaultModeId)?.name ?? "?"}" was captured.`,
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
    const name = canonicalName(coll.name, v.name);
    out.variableIdToName[v.id] = name;
    const raw = v.valuesByMode[defaultModeId];
    if (raw === undefined) continue;
    switch (v.resolvedType) {
      case "COLOR":
        out.colors[name] = renderColorValue(raw, defaultModeId);
        break;
      case "FLOAT":
        if (typeof raw === "number") out.dimensions[name] = raw;
        else out.dimensions[name] = await resolveNumberAlias(raw, defaultModeId);
        break;
      case "STRING":
        if (typeof raw === "string") out.strings[name] = raw;
        else out.strings[name] = String(await resolveAlias(raw, defaultModeId));
        break;
      case "BOOLEAN":
        // Booleans don't fit IRTokens cleanly; encode as "true" / "false" strings.
        if (typeof raw === "boolean") out.strings[name] = raw ? "true" : "false";
        break;
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

function renderColorValue(raw: VariableValue, modeId: string): string {
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

async function resolveAlias(raw: VariableValue, modeId: string): Promise<unknown> {
  if (typeof raw === "object" && raw && "type" in raw && (raw as VariableAlias).type === "VARIABLE_ALIAS") {
    const referent = await figma.variables.getVariableByIdAsync((raw as VariableAlias).id);
    if (referent) {
      const next = referent.valuesByMode[modeId];
      if (next !== undefined) return next;
    }
  }
  return raw;
}

async function resolveNumberAlias(raw: VariableValue, modeId: string): Promise<number> {
  const resolved = await resolveAlias(raw, modeId);
  if (typeof resolved === "number") return resolved;
  return 0;
}
