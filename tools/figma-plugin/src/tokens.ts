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
  /// Canonical DesignIR token path → original Figma variable provenance.
  /// The category prefix matches IRTokens.source_identity
  /// ("colors.<name>", "dimensions.<name>", or "strings.<name>").
  sourceIdentity: Record<string, {
    sourceId: string;
    sourceCollection: string;
    sourceMode: string;
    sourceAdapter: string;
  }>;
  /// Map of Figma variable id → canonical token name. Lets style extraction
  /// emit token references where a variable is bound to a style property.
  variableIdToName: Record<string, string>;
  /// Per-variable mode id → canonical token name, used with a node's inherited
  /// resolvedVariableModes so bindings name the value actually rendered.
  variableIdToModeName?: Record<string, Record<string, string>>;
  variableIdToCollectionId?: Record<string, string>;
}

export async function extractTokens(diagnostics: ExtractedDiagnostic[]): Promise<ExtractedTokens> {
  const out: ExtractedTokens = {
    colors: {},
    dimensions: {},
    strings: {},
    sourceIdentity: {},
    variableIdToName: {},
    variableIdToModeName: {},
    variableIdToCollectionId: {},
  };
  // Internal join ownership. A suffixed mode token can collide with another
  // variable's canonical base name; this tells us whether an overwritten path
  // was actually the previous variable's default (bindable) token.
  const defaultIdentityKeyByVariableId: Record<string, string> = {};

  // Local collections first
  let localCollections: VariableCollection[] = [];
  try {
    localCollections = await figma.variables.getLocalVariableCollectionsAsync();
  } catch {
    return out;
  }

  for (const coll of localCollections) {
    await ingestCollection(coll, out, diagnostics, defaultIdentityKeyByVariableId);
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
  defaultIdentityKeyByVariableId: Record<string, string>,
): Promise<void> {
  const defaultModeId = coll.defaultModeId;
  const modeSuffixes = buildModeSuffixes(coll.modes, defaultModeId);
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
    for (const mode of coll.modes) {
      const raw = v.valuesByMode[mode.modeId];
      if (raw === undefined) continue;
      // Default mode keeps the bare name (back-compat + the name style refs
      // resolve to); every other mode is captured under "<name>.<mode>" so
      // light/dark (and any multi-mode) values survive import.
      const slug = modeSuffixes[mode.modeId] ?? modeSlug(mode.name);
      const name = mode.modeId === defaultModeId
        ? baseName
        : `${baseName}.${slug}`;
      const identityKey = await assignToken(out, name, raw, v.resolvedType, mode.modeId);
      if (identityKey) {
        const previousOwner = out.sourceIdentity[identityKey]?.sourceId;
        if (previousOwner && previousOwner !== v.id &&
            defaultIdentityKeyByVariableId[previousOwner] === identityKey) {
          delete out.variableIdToName[previousOwner];
          delete defaultIdentityKeyByVariableId[previousOwner];
        }
        if (previousOwner && previousOwner !== v.id) {
          const previousModes = out.variableIdToModeName![previousOwner];
          if (previousModes) {
            for (const [previousModeId, previousName] of Object.entries(previousModes)) {
              if (previousName === name) delete previousModes[previousModeId];
            }
            if (Object.keys(previousModes).length === 0)
              delete out.variableIdToModeName![previousOwner];
          }
        }
        out.sourceIdentity[identityKey] = {
          sourceId: v.id,
          sourceCollection: coll.name,
          sourceMode: mode.name,
          sourceAdapter: "figma-plugin",
        };
        (out.variableIdToModeName![v.id] ??= {})[mode.modeId] = name;
        out.variableIdToCollectionId![v.id] = coll.id;
        if (mode.modeId === defaultModeId) {
          // A property binding names the default-mode token. Keep the join map
          // fail-closed: an invalid variable gets no entry, and when two
          // variables canonicalize to the same name only the variable whose
          // value/provenance currently owns that token may resolve to it.
          defaultIdentityKeyByVariableId[v.id] = identityKey;
          out.variableIdToName[v.id] = baseName;
        }
      }
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

function buildModeSuffixes(
  modes: ReadonlyArray<{ modeId: string; name: string }>,
  defaultModeId: string,
): Record<string, string> {
  const baseById: Record<string, string> = {};
  const counts: Record<string, number> = {};
  for (const mode of modes) {
    if (mode.modeId === defaultModeId) continue;
    const base = modeSlug(mode.name);
    baseById[mode.modeId] = base;
    counts[base] = (counts[base] ?? 0) + 1;
  }

  const suffixes: Record<string, string> = {};
  const used = new Set<string>();
  for (const mode of modes) {
    if (mode.modeId === defaultModeId) continue;
    const base = baseById[mode.modeId];
    let suffix = base && counts[base] === 1
      ? base
      : `${base || "mode"}-${stableModeId(mode.modeId)}`;
    let collision = 2;
    while (used.has(suffix)) suffix = `${base || "mode"}-${stableModeId(mode.modeId)}-${collision++}`;
    used.add(suffix);
    suffixes[mode.modeId] = suffix;
  }
  return suffixes;
}

function stableModeId(modeId: string): string {
  // FNV-1a gives arbitrary Plugin API ids a compact token-safe suffix.
  let hash = 0x811c9dc5;
  for (let i = 0; i < modeId.length; ++i) {
    hash ^= modeId.charCodeAt(i);
    hash = Math.imul(hash, 0x01000193);
  }
  return (hash >>> 0).toString(16).padStart(8, "0");
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
): Promise<string | undefined> {
  const value = await resolveValue(raw, modeId);
  switch (resolvedType) {
    case "COLOR":
      if (typeof value === "object" && value && "r" in value) {
        out.colors[name] = renderColorValue(value);
        return `colors.${name}`;
      }
      break;
    case "FLOAT":
      if (typeof value === "number") {
        out.dimensions[name] = value;
        return `dimensions.${name}`;
      }
      break;
    case "STRING":
      if (typeof value === "string") {
        out.strings[name] = value;
        return `strings.${name}`;
      }
      break;
    case "BOOLEAN":
      // Booleans don't fit IRTokens cleanly; encode as "true" / "false" strings.
      if (typeof value === "boolean") {
        out.strings[name] = value ? "true" : "false";
        return `strings.${name}`;
      }
      break;
  }
  return undefined;
}
