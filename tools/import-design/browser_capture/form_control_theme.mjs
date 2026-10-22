// SPDX-License-Identifier: MIT

// Whether Chromium's native theme actually painted each form control.
//
// Computed `appearance` is not the answer. Blink keeps a control's computed
// `appearance: auto` and still drops the theme when the AUTHOR styled the box:
// a button or progress bar with an author background, border, or border radius
// is painted from CSS, and a text field or textarea is too, with an author
// box-shadow as well. Checkboxes, radios, ranges and the other picker controls
// keep the theme whatever the author sets, and a closed select keeps its
// theme-drawn arrow while its box falls to CSS. The rule is per control type,
// and it keys on WHO declared the property — an author value equal to the user
// agent default still counts — which no computed style can say. The capture
// therefore asks `CSS.getMatchedStylesForNode`, which reports each matched
// rule's origin, and records the author-declared box properties beside the
// verdict so the rule can be audited against the facts it read.

/// Nodes to interrogate before the report stops enumerating. Recorded in the
/// report so a truncated census never reads as complete.
const MAX_INTERROGATED_CONTROLS = 2000;

const CONTROL_TAGS = new Set([
  "input", "button", "select", "textarea", "meter", "progress"]);

const BUTTON_INPUT_TYPES = new Set(["button", "submit", "reset"]);
const TEXT_INPUT_TYPES = new Set([
  "", "text", "search", "email", "url", "tel", "password", "number"]);

/// The author-declared properties that decide the rule, by family.
function propertyFamily(name) {
  if (name.startsWith("background")) return "background";
  if (name === "border-radius" || /^border-.*-radius$/u.test(name))
    return "border_radius";
  if (name === "border" || name.startsWith("border-")) return "border";
  if (name === "box-shadow") return "box_shadow";
  return null;
}

/// Which families can take the theme away from a control, or null when no
/// author styling can (the control is always themed).
export function themeDroppingFamilies(tag, type) {
  if (tag === "button" || (tag === "input" && BUTTON_INPUT_TYPES.has(type)) ||
      tag === "progress") {
    return ["background", "border", "border_radius"];
  }
  if (tag === "textarea" || (tag === "input" && TEXT_INPUT_TYPES.has(type))) {
    return ["background", "border", "border_radius", "box_shadow"];
  }
  if (tag === "select") return ["background", "border", "border_radius"];
  return null;
}

/// The verdict for one control from its computed appearance and the families
/// an author declared.
///
///   `none`         appearance is none; CSS paints the box.
///   `native`       the native theme paints the control.
///   `author`       appearance is not none, but author styling dropped the
///                  theme and CSS paints the box.
///   `author-arrow` a select whose box falls to CSS while the theme still
///                  draws its arrow.
export function themeVerdict(tag, type, appearance, authorFamilies) {
  if ((appearance ?? "").trim().toLowerCase() === "none") return "none";
  const dropping = themeDroppingFamilies(tag, type);
  if (!dropping) return "native";
  const styled = dropping.some((family) => authorFamilies.includes(family));
  if (!styled) return "native";
  return tag === "select" ? "author-arrow" : "author";
}

function laidOutControls(snapshot, styleNames) {
  const document = snapshot.documents?.[0];
  if (!document) return [];
  const strings = snapshot.strings ?? [];
  const nodes = document.nodes ?? {};
  const layout = document.layout ?? {};
  const names = nodes.nodeName ?? [];
  const attributes = nodes.attributes ?? [];
  const backendNodeId = nodes.backendNodeId ?? [];
  const appearanceAt = (styleNames ?? []).indexOf("appearance");
  const found = [];
  const seen = new Set();
  (layout.nodeIndex ?? []).forEach((node, layoutIndex) => {
    if (seen.has(node)) return;
    const tag = String(strings[names[node]] ?? "").toLowerCase();
    if (!CONTROL_TAGS.has(tag)) return;
    seen.add(node);
    let type = "";
    const pairs = attributes[node] ?? [];
    for (let i = 0; i + 1 < pairs.length; i += 2) {
      if (String(strings[pairs[i]] ?? "").toLowerCase() === "type")
        type = String(strings[pairs[i + 1]] ?? "").trim().toLowerCase();
    }
    const row = layout.styles?.[layoutIndex] ?? [];
    const appearance = appearanceAt >= 0 && appearanceAt < row.length
      ? String(strings[row[appearanceAt]] ?? "") : null;
    found.push({
      node_index: node,
      backend_node_id: backendNodeId[node],
      tag,
      type: tag === "input" ? (type || "text") : null,
      appearance,
    });
  });
  return found;
}

function authorFamiliesFrom(matched) {
  const families = new Set();
  const take = (style) => {
    for (const property of style?.cssProperties ?? []) {
      if (property.disabled === true || property.parsedOk === false) continue;
      if (!property.value && property.value !== 0) continue;
      const family = propertyFamily(String(property.name ?? ""));
      if (family) families.add(family);
    }
  };
  take(matched.inlineStyle);
  for (const match of matched.matchedCSSRules ?? []) {
    // `regular` is the author origin. User-agent rules are what the theme
    // replaces; injected and inspector rules are not the page's.
    if (match.rule?.origin === "regular") take(match.rule.style);
  }
  return [...families].sort();
}

/// Interrogate each laid-out form control and return the report the capture
/// writes into the DOM snapshot, so it is bound by the snapshot's own hash.
export async function evaluateFormControlTheme(cdp, snapshot, styleNames) {
  const candidates = laidOutControls(snapshot, styleNames);
  const truncated = candidates.length > MAX_INTERROGATED_CONTROLS;
  const interrogated = truncated
    ? candidates.slice(0, MAX_INTERROGATED_CONTROLS) : candidates;
  const report = {
    schema: "pulp-form-control-theme-v1",
    controls_found: candidates.length,
    truncated,
    controls: [],
  };
  if (interrogated.length === 0) return report;
  await cdp.call("DOM.enable");
  await cdp.call("CSS.enable");
  await cdp.call("DOM.getDocument", { depth: -1 });
  const backends = interrogated.map((control) => control.backend_node_id);
  let nodeIds = [];
  try {
    const pushed = await cdp.call(
      "DOM.pushNodesByBackendIdsToFrontend", { backendNodeIds: backends });
    nodeIds = pushed.nodeIds ?? [];
  } catch {
    // Every control is then reported unresolved, never as unstyled.
  }
  for (let i = 0; i < interrogated.length; i += 1) {
    const control = interrogated[i];
    const nodeId = nodeIds[i];
    let families = null;
    if (nodeId) {
      try {
        families = authorFamiliesFrom(
          await cdp.call("CSS.getMatchedStylesForNode", { nodeId }));
      } catch {
        families = null;
      }
    }
    report.controls.push({
      ...control,
      author_families: families,
      // Unresolved author styling leaves the verdict unknown rather than
      // guessing either way.
      theme: (control.appearance ?? "").trim().toLowerCase() === "none"
        ? "none"
        : families === null
        ? null : themeVerdict(control.tag, control.type ?? "",
          control.appearance, families),
    });
  }
  return report;
}
