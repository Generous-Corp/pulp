// Effect lowering for the Figma plugin lane: the ordered effect stack maps
// each family to the CSS property the render stack actually consumes —
// shadows to comma-joined box_shadow layers, LAYER_BLUR to filter,
// BACKGROUND_BLUR to backdrop_filter — while everything with no lowering
// (NOISE, TEXTURE, GLASS, newer families) is diagnosed, never silently
// dropped. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { lowerEffects } from "../src/extract-pure";

// lowerEffects reads only the effect-cluster fields; the cast keeps each
// fixture to the fields under test rather than standing up the full typed
// Effect union.
const effects = (list: Array<Record<string, unknown>>) =>
  list as unknown as readonly Effect[];

const black25 = { r: 0, g: 0, b: 0, a: 0.25 };

test("shadows keep array order as comma-joined box_shadow layers", () => {
  const out = lowerEffects(effects([
    { type: "DROP_SHADOW", visible: true, offset: { x: 0, y: 16 }, radius: 6, spread: 0, color: black25 },
    { type: "INNER_SHADOW", visible: true, offset: { x: 0, y: 1 }, radius: 2, spread: 1, color: black25 },
  ]));
  assert.equal(
    out.box_shadow,
    "0px 16px 6px 0px rgba(0, 0, 0, 0.250), inset 0px 1px 2px 1px rgba(0, 0, 0, 0.250)",
  );
  assert.equal(out.filter, undefined);
  assert.deepEqual(out.diagnostics, []);
});

test("a layer blur lowers to filter, a background blur to backdrop_filter", () => {
  const out = lowerEffects(effects([
    { type: "LAYER_BLUR", visible: true, radius: 8, blurType: "NORMAL" },
    { type: "BACKGROUND_BLUR", visible: true, radius: 12, blurType: "NORMAL" },
  ]));
  assert.equal(out.filter, "blur(8px)");
  assert.equal(out.backdrop_filter, "blur(12px)");
  assert.deepEqual(out.diagnostics, []);
});

test("a mixed stack keeps shadow order and blur order independently", () => {
  const out = lowerEffects(effects([
    { type: "DROP_SHADOW", visible: true, offset: { x: 0, y: 2 }, radius: 4, spread: 0, color: black25 },
    { type: "LAYER_BLUR", visible: true, radius: 2, blurType: "NORMAL" },
    { type: "LAYER_BLUR", visible: true, radius: 6, blurType: "NORMAL" },
  ]));
  assert.ok(out.box_shadow, "the shadow still lowers beside the blurs");
  // Two visible layer blurs keep array order as a function sequence — the
  // bridge's setFilter walks it and sums the blur amounts.
  assert.equal(out.filter, "blur(2px) blur(6px)");
});

test("an invisible effect is the designer's off switch — no output, no diagnostic", () => {
  const out = lowerEffects(effects([
    { type: "LAYER_BLUR", visible: false, radius: 8, blurType: "NORMAL" },
    { type: "DROP_SHADOW", visible: false, offset: { x: 0, y: 99 }, radius: 9, spread: 0, color: black25 },
    { type: "NOISE", visible: false },
  ]));
  assert.equal(out.filter, undefined);
  assert.equal(out.box_shadow, undefined);
  assert.deepEqual(out.diagnostics, []);
});

test("a progressive blur keeps its end radius and admits the approximation", () => {
  const out = lowerEffects(effects([
    { type: "LAYER_BLUR", visible: true, radius: 10, blurType: "PROGRESSIVE",
      startRadius: 0, startOffset: { x: 0, y: 0 }, endOffset: { x: 0, y: 1 } },
  ]));
  assert.equal(out.filter, "blur(10px)", "best effort: the end radius as a uniform blur");
  assert.equal(out.diagnostics.length, 1);
  assert.equal(out.diagnostics[0].code, "progressive-blur-approximated");
  assert.equal(out.diagnostics[0].kind, "capture_partial");
});

test("an effect family with no lowering is diagnosed, never dropped in silence", () => {
  const out = lowerEffects(effects([
    { type: "NOISE", visible: true },
    { type: "TEXTURE", visible: true },
    { type: "GLASS", visible: true },
    // A family newer than the pinned typings must land on the same arm.
    { type: "SHADER", visible: true },
  ]));
  assert.equal(out.box_shadow, undefined);
  assert.equal(out.filter, undefined);
  assert.equal(out.diagnostics.length, 4);
  for (const d of out.diagnostics) {
    assert.equal(d.code, "effect-unsupported");
    assert.equal(d.kind, "unsupported_property");
  }
  assert.match(out.diagnostics[0].message, /NOISE/);
  assert.match(out.diagnostics[3].message, /SHADER/);
});
