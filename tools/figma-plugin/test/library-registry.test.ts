// Exercises the REAL recognition logic in src/library-registry.ts against the
// real tools/figma-plugin/library-manifest.json. These are the two functions
// extract.ts calls to decide whether a Figma instance is a Pulp library widget.

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  widgetKindByLibraryKey,
  widgetKindByNamePrefix,
  entryForKind,
  LIBRARY_VERSION,
} from "../src/library-registry";
import manifest from "../library-manifest.json";

test("widgetKindByLibraryKey: real knob component_set_key → 'knob'", () => {
  const knobKey = manifest.widgets.knob.component_set_key;
  assert.equal(widgetKindByLibraryKey(knobKey), "knob");
});

test("widgetKindByLibraryKey: real fader/meter keys resolve to their kinds", () => {
  assert.equal(
    widgetKindByLibraryKey(manifest.widgets.fader.component_set_key),
    "fader",
  );
  assert.equal(
    widgetKindByLibraryKey(manifest.widgets.meter.component_set_key),
    "meter",
  );
});

test("widgetKindByLibraryKey: unknown / nullish keys → undefined", () => {
  assert.equal(widgetKindByLibraryKey("not-a-pulp-key"), undefined);
  assert.equal(widgetKindByLibraryKey(undefined), undefined);
  assert.equal(widgetKindByLibraryKey(null), undefined);
  assert.equal(widgetKindByLibraryKey(""), undefined);
});

test("widgetKindByLibraryKey: placeholder TBD- keys are never registered", () => {
  // Any manifest entry whose key is a TBD- placeholder must NOT be a real
  // collision target. Guard so a future placeholder doesn't silently match.
  for (const w of Object.values(manifest.widgets)) {
    if (w.component_set_key.startsWith("TBD-")) {
      assert.equal(widgetKindByLibraryKey(w.component_set_key), undefined);
    }
  }
});

test("widgetKindByNamePrefix: 'Pulp / Knob …' → 'knob'", () => {
  assert.equal(widgetKindByNamePrefix("Pulp / Knob"), "knob");
  assert.equal(widgetKindByNamePrefix("Pulp / Knob / Large"), "knob");
});

test("widgetKindByNamePrefix: case-insensitive", () => {
  assert.equal(widgetKindByNamePrefix("pulp / knob"), "knob");
  assert.equal(widgetKindByNamePrefix("PULP / KNOB / Cutoff"), "knob");
});

test("widgetKindByNamePrefix: non-Pulp name → undefined", () => {
  assert.equal(widgetKindByNamePrefix("Rectangle 42"), undefined);
  assert.equal(widgetKindByNamePrefix("Some Other / Knob"), undefined);
  assert.equal(widgetKindByNamePrefix(undefined), undefined);
  assert.equal(widgetKindByNamePrefix(""), undefined);
});

test("entryForKind + LIBRARY_VERSION are wired from the manifest", () => {
  assert.equal(LIBRARY_VERSION, manifest.library_version);
  const knob = entryForKind("knob");
  assert.ok(knob);
  assert.equal(knob.component_set_key, manifest.widgets.knob.component_set_key);
  assert.equal(knob.name_prefix, "Pulp / Knob");
});
