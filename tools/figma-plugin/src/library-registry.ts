// Pulp Figma library registry.
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

// ── Custom-control resolution ───────────────────────────────────────────────
// A shared package (registry-schema `design_controls`) maps a Figma identity to
// the `factory_id` its pulp::view::View registers (register_design_control_
// factory). The importer MERGES the installed packages' fragments into one list
// and resolves a node's identity to a factory_id — emitting a kind=custom
// interactive element so the materializer builds the package's control. This is
// the same precedence the built-in resolver uses: component-set key is
// authoritative; the name prefix is the fallback.
export interface DesignControlEntry {
  factory_id: string;
  component_set_key?: string;
  name_prefix?: string;
}

/// Resolve a node's identity to a registered custom-control factory_id, or
/// undefined when no package fragment matches. `entries` is the merged list from
/// the installed packages' `design_controls`. Key match wins; name-prefix match
/// (case-insensitive, prefix) is the fallback.
export function customControlFactoryId(
  componentKey: string | undefined | null,
  name: string | undefined | null,
  entries: ReadonlyArray<DesignControlEntry>,
): string | undefined {
  if (componentKey) {
    for (const e of entries) {
      if (e.component_set_key && e.component_set_key === componentKey) {
        return e.factory_id;
      }
    }
  }
  if (name) {
    const lower = name.toLowerCase();
    for (const e of entries) {
      if (e.name_prefix && lower.indexOf(e.name_prefix.toLowerCase()) === 0) {
        return e.factory_id;
      }
    }
  }
  return undefined;
}
