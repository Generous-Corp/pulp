// library-registry.ts — Phase 3 (planning/2026-05-28-pulp-figma-plugin-strategy.md §8)
//
// Loads tools/figma-plugin/library-manifest.json at build time and exposes
// typed lookups for:
//   (a) component_set_key → AudioWidgetKind  (authoritative, key-based)
//   (b) name-prefix       → AudioWidgetKind  (fallback for unpublished libraries)
//
// The manifest is the single source of truth for which Pulp library widgets
// the importer recognises and which Figma component-set keys / name prefixes
// they map to. Adding a new widget = adding one entry to the JSON.

import manifest from "../library-manifest.json";
import type { AudioWidgetKind } from "./extract-model";

export interface LibraryWidgetEntry {
  kind: AudioWidgetKind;
  component_set_key: string;
  name_prefix: string;
  supported_property_definitions: ReadonlyArray<string>;
  supported_variant_axes: ReadonlyArray<string>;
  since_library_version: string;
}

const widgetEntries: ReadonlyArray<LibraryWidgetEntry> = (() => {
  const out: LibraryWidgetEntry[] = [];
  const widgets = manifest.widgets as Record<string, {
    component_set_key: string;
    name_prefix: string;
    supported_property_definitions: string[];
    supported_variant_axes: string[];
    since_library_version: string;
  }>;
  for (const kind of Object.keys(widgets)) {
    const w = widgets[kind];
    out.push({
      kind: kind as AudioWidgetKind,
      component_set_key: w.component_set_key,
      name_prefix: w.name_prefix,
      supported_property_definitions: w.supported_property_definitions,
      supported_variant_axes: w.supported_variant_axes,
      since_library_version: w.since_library_version,
    });
  }
  return out;
})();

// Build a key → kind lookup at module load. Entries whose key starts with
// "TBD-" (placeholder until the library publishes) are excluded so a real
// Figma key collision is impossible.
const keyToKind: Record<string, AudioWidgetKind> = {};
for (const e of widgetEntries) {
  if (e.component_set_key && !e.component_set_key.startsWith("TBD-")) {
    keyToKind[e.component_set_key] = e.kind;
  }
}

export const LIBRARY_VERSION: string = manifest.library_version;

/// Authoritative key-based recognition. Returns the AudioWidgetKind for a
/// known Pulp-library component_set_key (or undefined for non-Pulp instances).
/// `componentKey` is the `key` field of the INSTANCE node's main-component or
/// the main-component's parent component-set in Figma.
export function widgetKindByLibraryKey(
  componentKey: string | undefined | null,
): AudioWidgetKind | undefined {
  if (!componentKey) return undefined;
  return keyToKind[componentKey];
}

/// Fallback name-prefix recognition for designs that use a layer naming
/// convention like "Pulp / Knob" but haven't pulled in the published library.
/// Case-insensitive prefix match. Returns the first matching widget kind.
export function widgetKindByNamePrefix(
  name: string | undefined | null,
): AudioWidgetKind | undefined {
  if (!name) return undefined;
  const lower = name.toLowerCase();
  for (const e of widgetEntries) {
    if (lower.startsWith(e.name_prefix.toLowerCase())) {
      return e.kind;
    }
  }
  return undefined;
}

/// Return the manifest entry for a kind, used by callers that need to
/// validate property definitions / variant axes.
export function entryForKind(
  kind: AudioWidgetKind,
): LibraryWidgetEntry | undefined {
  return widgetEntries.find((e) => e.kind === kind);
}
