// widgets/index.js — factory dispatch + shared re-exports.

import { createKnob } from "./knob.js";
import { createFader } from "./fader.js";
import { createToggle } from "./toggle.js";
import { createCombo } from "./combo.js";
import { createMeter } from "./meter.js";
export { createKnob, createFader, createToggle, createCombo, createMeter };
export { formatValue, quantStep } from "./base.js";

// Choose a widget kind from a parameter descriptor (+ optional per-demo override).
// Native default (ink_signal_editor.hpp) is Knob for continuous params; boolean
// → Toggle, choice → Combo. An override may force "fader" (etc.) per param.
export function kindFor(param, override) {
  if (override) return override;
  if (param.type === "boolean") return "toggle";
  if (param.type === "choice" && param.labels && param.labels.length) return "combo";
  return "knob";
}

export function createWidget(kind, opts) {
  switch (kind) {
    case "toggle": return createToggle(opts);
    case "combo": return createCombo(opts);
    case "fader": return createFader(opts);
    case "meter": return createMeter(opts);
    case "knob":
    default: return createKnob(opts);
  }
}
