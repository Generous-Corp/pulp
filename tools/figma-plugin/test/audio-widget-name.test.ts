// audioWidgetKindFromName must match whole words, not substrings, in lockstep
// with the C++ detect_audio_widget (design_import.cpp) and the Python
// widget_kind_from_name. The old substring match produced the false positives
// this pins shut.

import { test } from "node:test";
import assert from "node:assert/strict";

import { audioWidgetKindFromName, tokenizeName } from "../src/extract-pure";

test("tokenizeName splits on separators + camelCase/acronym/digit boundaries", () => {
  assert.deepEqual(tokenizeName("VUMeter"), ["vu", "meter"]);
  assert.deepEqual(tokenizeName("Knob_1"), ["knob", "1"]);
  assert.deepEqual(tokenizeName("Cutoff Knob"), ["cutoff", "knob"]);
  assert.deepEqual(tokenizeName("XY Pad"), ["xy", "pad"]);
  assert.deepEqual(tokenizeName("XYPad"), ["xy", "pad"]);
  assert.deepEqual(tokenizeName("Dialog"), ["dialog"]);
});

test("audioWidgetKindFromName matches whole words (true positives)", () => {
  assert.equal(audioWidgetKindFromName("Cutoff Knob"), "knob");
  assert.equal(audioWidgetKindFromName("Knobs"), "knob");          // plural tolerated
  assert.equal(audioWidgetKindFromName("Volume Fader"), "fader");
  assert.equal(audioWidgetKindFromName("Main Slider"), "fader");
  assert.equal(audioWidgetKindFromName("VUMeter"), "meter");        // acronym split
  assert.equal(audioWidgetKindFromName("XY Pad"), "xy_pad");
  assert.equal(audioWidgetKindFromName("XYPad"), "xy_pad");
  assert.equal(audioWidgetKindFromName("Waveform"), "waveform");
  assert.equal(audioWidgetKindFromName("Spectrum Analyzer"), "spectrum");
});

test("audioWidgetKindFromName rejects substring false positives (the fix)", () => {
  // These all used to mis-resolve under the substring match.
  assert.equal(audioWidgetKindFromName("Dialog"), undefined);       // was knob ("dial")
  assert.equal(audioWidgetKindFromName("Radial"), undefined);       // was knob ("dial")
  assert.equal(audioWidgetKindFromName("Diameter"), undefined);     // was meter ("meter")
  assert.equal(audioWidgetKindFromName("Parameter"), undefined);    // was meter ("meter")
  assert.equal(audioWidgetKindFromName("Medallion"), undefined);    // was knob (substring "dal"? no) — stays clean
  assert.equal(audioWidgetKindFromName("Reverb"), undefined);       // no signal either way
});
