// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  evaluateFormControlTheme,
  themeVerdict,
} from "./form_control_theme.mjs";

test("author box styling drops the theme per control type", () => {
  // A button: background, border and radius each take the theme away; a
  // box-shadow does not.
  assert.equal(themeVerdict("button", "", "auto", []), "native");
  assert.equal(themeVerdict("button", "", "auto", ["background"]), "author");
  assert.equal(themeVerdict("button", "", "auto", ["border"]), "author");
  assert.equal(themeVerdict("button", "", "auto", ["border_radius"]), "author");
  assert.equal(themeVerdict("button", "", "auto", ["box_shadow"]), "native");
  assert.equal(themeVerdict("input", "submit", "auto", ["border"]), "author");
  // A text field also yields to a box-shadow.
  assert.equal(themeVerdict("input", "text", "auto", ["box_shadow"]), "author");
  assert.equal(themeVerdict("textarea", null, "auto", ["background"]), "author");
  // A select's box falls to CSS, its arrow stays the theme's.
  assert.equal(themeVerdict("select", null, "auto", ["border"]), "author-arrow");
  assert.equal(themeVerdict("select", null, "auto", []), "native");
  // Toggles and sliders keep the theme whatever the author sets.
  for (const type of ["checkbox", "radio", "range", "color"]) {
    assert.equal(
      themeVerdict("input", type, "auto",
        ["background", "border", "border_radius", "box_shadow"]),
      "native", type);
  }
  assert.equal(themeVerdict("input", "checkbox", "none", []), "none");
});

function snapshotWith(controls) {
  // controls: [{tag, type, appearance}]
  const strings = ["DIV", "type", "auto", "none"];
  const index = (value) => {
    let at = strings.indexOf(value);
    if (at < 0) { strings.push(value); at = strings.length - 1; }
    return at;
  };
  const nodeName = [0];
  const attributes = [[]];
  const backendNodeId = [100];
  const styles = [[index("none")]];
  const layoutNodes = [0];
  controls.forEach((control, i) => {
    nodeName.push(index(control.tag.toUpperCase()));
    attributes.push(control.type ? [1, index(control.type)] : []);
    backendNodeId.push(101 + i);
    layoutNodes.push(i + 1);
    styles.push([index(control.appearance)]);
  });
  return {
    strings,
    documents: [{
      nodes: { nodeName, attributes, backendNodeId },
      layout: { nodeIndex: layoutNodes, styles },
    }],
  };
}

function fakeCdp(matchedByBackend) {
  return {
    async call(method, params) {
      if (method === "DOM.pushNodesByBackendIdsToFrontend")
        return { nodeIds: params.backendNodeIds.map((id) => id + 1000) };
      if (method === "CSS.getMatchedStylesForNode")
        return matchedByBackend[params.nodeId - 1000] ?? {};
      return {};
    },
  };
}

test("only author-origin declarations count as author styling", async () => {
  const snapshot = snapshotWith([
    { tag: "button", appearance: "auto" },
    { tag: "button", appearance: "auto" },
    { tag: "button", appearance: "auto" },
  ]);
  const uaBorder = {
    rule: { origin: "user-agent", style: { cssProperties: [
      { name: "border-top-style", value: "outset" }] } },
  };
  const report = await evaluateFormControlTheme(fakeCdp({
    // The user agent's own border is what the theme replaces, not author
    // styling.
    101: { matchedCSSRules: [uaBorder] },
    // An author rule with a background.
    102: { matchedCSSRules: [uaBorder, { rule: { origin: "regular", style: {
      cssProperties: [{ name: "background-color", value: "red" }] } } }] },
    // An inline style border.
    103: { inlineStyle: { cssProperties: [
      { name: "border-top-width", value: "1px" }] } },
  }), snapshot, ["appearance"]);
  assert.deepEqual(report.controls.map((c) => c.theme),
    ["native", "author", "author"]);
  assert.deepEqual(report.controls[1].author_families, ["background"]);
});

test("an unresolved control is unknown, never unstyled", async () => {
  const snapshot = snapshotWith([{ tag: "button", appearance: "auto" }]);
  const cdp = {
    async call(method) {
      if (method === "CSS.getMatchedStylesForNode") throw new Error("gone");
      if (method === "DOM.pushNodesByBackendIdsToFrontend")
        return { nodeIds: [5] };
      return {};
    },
  };
  const report = await evaluateFormControlTheme(cdp, snapshot, ["appearance"]);
  assert.equal(report.controls[0].theme, null);
  assert.equal(report.controls[0].author_families, null);
});
