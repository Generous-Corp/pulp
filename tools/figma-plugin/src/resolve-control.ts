// Control-resolution self-check for the faithful-vector lane.
//
// The original stumble was a slider/button silently materialized as a `knob`:
// a MISSING VERIFICATION LOOP, not just a missing kind. This module computes the
// control kind from INDEPENDENT signals and cross-checks them, so a mismatch is
// CAUGHT (lowered confidence + a recorded conflict) instead of silently shipped.
// It fills the import-report fields (resolution_rung / confidence_score /
// conflict_signals / verification_pass) the F0 chain carries to the host.
//
// Signals available in this lane: geometry/affordance (node bounds) and
// name/token (the node name). Component identity slots in as rung 1 ahead of
// these when available.
//
// ES5-conservative (the Figma plugin sandbox tsconfig targets an older lib).

import type { InteractiveElementKind } from "./faithful-vector";

export interface ResolutionReport {
  resolution_rung: number;     // 0=unset,1=identity,2=affordance,3=name,4=factory,5=inert
  confidence_score: number;    // 0..1
  conflict_signals: string[];
  verification_pass: boolean;
}

export interface Bounds { w: number; h: number; }

// Whole-word test (mirrors the C++ tokenize_name boundary rule): "knob" matches
// "Main Knob" / "knob_1" but not "doorknob".
export function hasWord(s: string, w: string): boolean {
  return new RegExp("(^|[^a-z0-9])" + w + "([^a-z0-9]|$)").test(s.toLowerCase());
}

// The name/token signal: a kind inferred purely from the node name.
export function kindFromName(name: string): InteractiveElementKind | undefined {
  const s = (name || "").toLowerCase();
  if (hasWord(s, "knob") || hasWord(s, "dial")) return "knob";
  if (hasWord(s, "fader") || hasWord(s, "slider")) return "fader";
  if (hasWord(s, "toggle") || hasWord(s, "switch")) return "toggle";
  if (hasWord(s, "xy") || s.indexOf("xypad") !== -1 || s.indexOf("xy pad") !== -1) return "xy_pad";
  if (hasWord(s, "dropdown") || hasWord(s, "select")) return "dropdown";
  if (hasWord(s, "tab") || hasWord(s, "tabs")) return "tab_group";
  if (hasWord(s, "stepper")) return "stepper";
  if (hasWord(s, "search") || hasWord(s, "field") || hasWord(s, "input")) return "text_field";
  return undefined;
}

// The geometry/affordance signal: the BROAD SHAPE CLASS a node's bounds imply.
// DELIBERATELY coarse — it exists to CONTRADICT a starkly wrong resolution (a
// "knob" that is actually a wide track), not to classify finely. Only two real
// classes plus "unknown": a near-square body vs a clearly stretched one. Faders
// and the box overlays (dropdown/text_field/…) are BOTH stretched, so geometry
// can't tell them apart — and must not pretend to, or it spams false conflicts.
export type ShapeClass = "square" | "stretched" | "unknown";

export function shapeClass(b: Bounds): ShapeClass {
  if (!b || b.w <= 0 || b.h <= 0) return "unknown";
  const aspect = b.w / b.h;
  if (aspect >= 1.6 || aspect <= 0.62) return "stretched";   // a track / wide box
  return "square";                                           // ~1:1: knob / xy_pad / compact control
}

// Which shape a resolved kind EXPECTS, or "any" when its shape is genuinely
// unconstrained (a toggle can be a small square OR a wide pill; an overlay box
// can be any wide rect). "any" never flags a geometry conflict — only knob /
// xy_pad (must be square) and fader (must be stretched) carry a hard expectation.
export type ExpectedShape = ShapeClass | "any";

export function expectedShape(kind: InteractiveElementKind): ExpectedShape {
  switch (kind) {
    case "knob":       return "square";
    case "xy_pad":     return "square";
    case "fader":      return "stretched";
    // The box overlays are always WIDER than tall — a degenerate square one
    // (e.g. a 12x12 "dropdown") is geometry-wrong and must flag.
    case "dropdown":   return "stretched";
    case "text_field": return "stretched";
    case "tab_group":  return "stretched";
    case "stepper":    return "stretched";
    // toggle (square switch OR wide pill), buttons, and value_label genuinely
    // vary — leave them unconstrained rather than manufacture false conflicts.
    default:           return "any";   // toggle / swap / action / value_label
  }
}

// How a control's kind was resolved (sets the import-report rung). identity =
// component/Code-Connect (rung 1); affordance = geometry/structure (rung 2);
// name = name/token (rung 3).
export type ResolvedVia = "identity" | "affordance" | "name";

// True when an expected shape is compatible with an actual one — i.e. NOT a hard
// contradiction. "any" and "unknown" never contradict, so only a square↔stretched
// mismatch on a shape-constrained kind flags.
function shapeCompatible(expected: ExpectedShape, actual: ShapeClass): boolean {
  if (expected === "any" || actual === "unknown") return true;
  return expected === actual;
}

// Cross-signal resolution: given the kind the detector RESOLVED for a node, plus
// the node's name and bounds, compute the import-report. Catches the silent-knob
// stumble — when the name and the geometry disagree, or when the resolved kind's
// expected shape contradicts the node, it lowers confidence and records the
// conflict rather than shipping a wrong control silently.
export function assessResolution(
  resolvedKind: InteractiveElementKind,
  name: string,
  bounds: Bounds,
  resolvedVia?: ResolvedVia,
): ResolutionReport {
  const nameKind = kindFromName(name);
  const geom = shapeClass(bounds);
  const conflicts: string[] = [];
  let confidence = 1.0;
  let pass = true;

  // 1) Name ↔ resolved-kind DISAGREEMENT: the name says one kind but the importer
  //    resolved another (e.g. name "Big Knob" but resolved as dropdown). A direct
  //    semantic conflict regardless of geometry — flag it and demote.
  if (nameKind && nameKind !== resolvedKind) {
    conflicts.push(
      "name '" + name + "' implies " + nameKind + " but resolved as " + resolvedKind);
    confidence = Math.min(confidence, 0.5);
  }

  // 2) Geometry sanity on the RESOLVED kind: a knob/xy_pad must be roughly square,
  //    a fader/box-overlay stretched. If the chosen kind's expected shape
  //    contradicts the bounds (incl. a degenerate square "dropdown"), flag it.
  if (!shapeCompatible(expectedShape(resolvedKind), geom)) {
    conflicts.push(
      "resolved kind " + resolvedKind + " expects " + expectedShape(resolvedKind) +
      " but geometry is " + geom);
    confidence = Math.min(confidence, 0.4);
    pass = false;
  }

  // Rung: prefer the caller's actual resolution method; fall back to inferring
  // from the available signals. identity=1, affordance=2, name=3, inert=5.
  let rung: number;
  if (resolvedVia === "identity") rung = 1;
  else if (resolvedVia === "affordance") rung = 2;
  else if (resolvedVia === "name") rung = 3;
  else if (nameKind) rung = 3;
  else if (geom !== "unknown") rung = 2;
  else rung = 5;

  return {
    resolution_rung: rung,
    confidence_score: confidence,
    conflict_signals: conflicts,
    verification_pass: pass,
  };
}
