// Control-resolution self-check. These tests pin the loop that would have caught
// the original silent-knob stumble.

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  assessResolution,
  kindFromName,
  shapeClass,
  expectedShape,
  hasWord,
} from "../src/resolve-control";

test("hasWord matches whole words, not substrings", () => {
  assert.equal(hasWord("Main Knob", "knob"), true);
  assert.equal(hasWord("knob_1", "knob"), true);
  assert.equal(hasWord("doorknob", "knob"), false);   // the false-positive the C++ rule also rejects
  assert.equal(hasWord("Dialog", "dial"), false);
});

test("kindFromName reads the name signal by whole word", () => {
  assert.equal(kindFromName("Cutoff Knob"), "knob");
  assert.equal(kindFromName("Volume Fader"), "fader");
  assert.equal(kindFromName("Bypass Switch"), "toggle");
  assert.equal(kindFromName("XY Pad"), "xy_pad");
  assert.equal(kindFromName("Reverb"), undefined);     // no signal
});

test("shapeClass classifies bounds into square vs stretched", () => {
  assert.equal(shapeClass({ w: 48, h: 48 }), "square");        // knob/xy_pad
  assert.equal(shapeClass({ w: 12, h: 120 }), "stretched");    // a vertical fader track
  assert.equal(shapeClass({ w: 200, h: 24 }), "stretched");    // a horizontal track
  assert.equal(shapeClass({ w: 18, h: 18 }), "square");        // a compact control
  assert.equal(shapeClass({ w: 120, h: 28 }), "stretched");    // a dropdown box (also stretched)
  assert.equal(shapeClass({ w: 0, h: 0 }), "unknown");
});

test("THE STUMBLE: a node named 'knob' with slider geometry is CAUGHT as a conflict", () => {
  // Exact failure mode: the importer resolved a knob by name, but the bounds are
  // a wide track. assessResolution must flag it and drop confidence, not silently
  // ship a knob.
  const r = assessResolution("knob", "Filter Knob", { w: 220, h: 22 });
  assert.ok(r.conflict_signals.length >= 1, "a conflict must be recorded");
  assert.ok(r.conflict_signals.some((c) => c.indexOf("knob") !== -1 && c.indexOf("stretched") !== -1),
    "the conflict names the knob/geometry mismatch: " + JSON.stringify(r.conflict_signals));
  assert.ok(r.confidence_score <= 0.5, "confidence is demoted, got " + r.confidence_score);
  assert.equal(r.verification_pass, false, "geometry verification fails");
});

test("a dropdown box (stretched) is NOT a false conflict — overlays are unconstrained", () => {
  // The trap the taxonomy was redesigned to avoid: a dropdown's wide box must not
  // be flagged just because it is stretched. expectedShape(dropdown)="stretched",
  // so a 120×28 box is compatible (not a conflict).
  const r = assessResolution("dropdown", "Mode Dropdown", { w: 120, h: 28 });
  assert.deepEqual(r.conflict_signals, []);
  assert.equal(r.confidence_score, 1.0);
  assert.equal(r.verification_pass, true);
});

test("a knob with genuine knob geometry resolves cleanly and confidently", () => {
  const r = assessResolution("knob", "Cutoff Knob", { w: 48, h: 48 });
  assert.deepEqual(r.conflict_signals, []);
  assert.equal(r.confidence_score, 1.0);
  assert.equal(r.verification_pass, true);
  assert.equal(r.resolution_rung, 3);          // resolved by its name token
});

test("geometry sanity catches a fader that isn't elongated", () => {
  // No name signal (so no name↔geometry conflict), but the RESOLVED fader's
  // expected shape (elongated) contradicts a square node — sanity must flag it.
  const r = assessResolution("fader", "Control 7", { w: 50, h: 50 });
  assert.ok(r.conflict_signals.some((c) => c.indexOf("fader") !== -1),
    "the resolved-kind sanity conflict is recorded");
  assert.ok(r.confidence_score < 1.0);
  assert.equal(r.verification_pass, false);
});

test("unknown geometry never manufactures a false conflict", () => {
  // A zero-size / undetermined node can't be judged — assessResolution must not
  // invent a conflict (which would spam the import report).
  const r = assessResolution("knob", "Filter Knob", { w: 0, h: 0 });
  assert.deepEqual(r.conflict_signals, []);
  assert.equal(r.verification_pass, true);
});

test("a switch (small) and a small knob are not treated as contradictory", () => {
  // small ~1:1 and square ~1:1 are compatible families — a compact toggle/switch
  // must not conflict with a small square just because the classes differ.
  const r = assessResolution("toggle", "Power Switch", { w: 20, h: 20 });
  assert.deepEqual(r.conflict_signals, []);
  assert.equal(r.verification_pass, true);
});

test("expectedShape: knob/xy_pad square, fader/box-overlays stretched, the rest 'any'", () => {
  assert.equal(expectedShape("knob"), "square");
  assert.equal(expectedShape("xy_pad"), "square");
  assert.equal(expectedShape("fader"), "stretched");
  assert.equal(expectedShape("dropdown"), "stretched");
  assert.equal(expectedShape("text_field"), "stretched");
  assert.equal(expectedShape("toggle"), "any");      // square switch OR wide pill
  assert.equal(expectedShape("swap"), "any");
});

test("a name that disagrees with the resolved kind is flagged (not just shape clashes)", () => {
  // "Big Knob" resolved as a dropdown: the shapes don't clash (a wide box), but
  // the NAME contradicts the resolution outright — must record a conflict.
  const r = assessResolution("dropdown", "Big Knob", { w: 120, h: 28 });
  assert.ok(r.conflict_signals.some((c) => c.indexOf("knob") !== -1 && c.indexOf("dropdown") !== -1),
    "name-vs-resolution conflict recorded: " + JSON.stringify(r.conflict_signals));
  assert.ok(r.confidence_score <= 0.5);
});

test("a degenerate square 'dropdown' is flagged — box overlays are not unconstrained", () => {
  // The "any" escape-hatch trap: a 12x12 square dropdown is obviously wrong
  // geometry and must NOT pass silently.
  const r = assessResolution("dropdown", "Mode Dropdown", { w: 12, h: 12 });
  assert.ok(r.conflict_signals.some((c) => c.indexOf("dropdown") !== -1 && c.indexOf("square") !== -1),
    "degenerate box flagged: " + JSON.stringify(r.conflict_signals));
  assert.equal(r.verification_pass, false);
});

test("resolvedVia sets the report rung honestly", () => {
  // A name/structure resolution is rung 3 even when the kind isn't in the name
  // vocabulary (e.g. a swap resolved from a 'Swap' name); a geometry-detected
  // knob is rung 2.
  assert.equal(assessResolution("swap", "Swap 2", { w: 60, h: 24 }, "name").resolution_rung, 3);
  assert.equal(assessResolution("knob", "", { w: 40, h: 40 }, "affordance").resolution_rung, 2);
  assert.equal(assessResolution("knob", "Cutoff Knob", { w: 48, h: 48 }, "identity").resolution_rung, 1);
});
