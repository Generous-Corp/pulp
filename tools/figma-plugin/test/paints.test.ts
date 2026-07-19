// Ordered paint-stack lowering for the Figma plugin lane (audit item 7).
// Figma renders `fills` bottom→top; the IR has one color + one gradient + one
// image background slot painted in that order, so a [solid…, gradient?,
// image?] prefix lowers exactly and everything beyond it raises a structured
// diagnostic instead of vanishing behind a first-visible-paint pick. Kept in
// lockstep with the REST lane (tools/import-design/figma_rest_export.py +
// test/test_figma_rest_export.py) and the .fig lane
// (tools/import-design/fig/scene.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  lowerFillPaints,
  compositeSolidPaints,
  scaleModeToObjectFit,
  gradientToCss,
  gradientFallbackFlat,
  paintToColor,
} from "../src/extract-pure";

const solid = (
  r: number, g: number, b: number,
  over: Record<string, unknown> = {},
) => ({ type: "SOLID", color: { r, g, b }, ...over }) as unknown as SolidPaint;

const paints = (...ps: unknown[]) => ps as unknown as readonly Paint[];

test("solid paint opacity folds into the emitted color's alpha", () => {
  // A 50%-opacity black fill must not import fully opaque.
  const out = lowerFillPaints(paints(solid(0, 0, 0, { opacity: 0.5 })));
  assert.equal(out.backgroundColor, "rgba(0, 0, 0, 0.500)");
  assert.deepEqual(out.diagnostics, []);
});

test("a single solid lowers exactly like the legacy paintToColor path", () => {
  const p = solid(0.2, 0.4, 0.6);
  const out = lowerFillPaints(paints(p));
  assert.equal(out.backgroundColor, paintToColor(p));
});

test("gradient paint opacity scales every stop's alpha", () => {
  const grad = {
    type: "GRADIENT_LINEAR",
    opacity: 0.5,
    gradientStops: [
      { position: 0, color: { r: 1, g: 1, b: 1, a: 1 } },
      { position: 1, color: { r: 0, g: 0, b: 0, a: 0.5 } },
    ],
  } as unknown as GradientPaint;
  assert.equal(
    gradientToCss(grad),
    "linear-gradient(to bottom, rgba(255, 255, 255, 0.500), rgba(0, 0, 0, 0.250))",
  );
  assert.equal(gradientFallbackFlat(grad), "rgba(255, 255, 255, 0.500)");
});

test("leading solids composite source-over, bottom to top", () => {
  // #4b4d51 base with white@0.55 over it → #aeafb1, the value Figma's own
  // raster samples (same fixture the .fig lane's compositeSolids documents).
  const css = compositeSolidPaints([
    solid(0x4b / 255, 0x4d / 255, 0x51 / 255),
    solid(1, 1, 1, { opacity: 0.55 }),
  ] as unknown as SolidPaint[]);
  assert.equal(css, "#aeafb1");
  const out = lowerFillPaints(paints(
    solid(0x4b / 255, 0x4d / 255, 0x51 / 255),
    solid(1, 1, 1, { opacity: 0.55 }),
  ));
  assert.equal(out.backgroundColor, "#aeafb1");
  assert.deepEqual(out.diagnostics, []);
});

test("solid below a linear gradient fills both slots — no diagnostic", () => {
  const out = lowerFillPaints(paints(
    solid(0, 0, 0),
    { type: "GRADIENT_LINEAR", gradientStops: [{ position: 0, color: { r: 1, g: 1, b: 1, a: 1 } }] },
  ));
  assert.equal(out.backgroundColor, "#000000");
  assert.equal(out.gradientPaint?.type, "GRADIENT_LINEAR");
  assert.deepEqual(out.diagnostics, []);
});

test("solid + gradient + image is the full representable prefix", () => {
  const out = lowerFillPaints(paints(
    solid(0, 0, 0),
    { type: "GRADIENT_LINEAR", gradientStops: [] },
    { type: "IMAGE", imageHash: "abc", scaleMode: "FILL" },
  ));
  assert.equal(out.backgroundColor, "#000000");
  assert.ok(out.gradientPaint);
  assert.equal((out.imagePaint as ImagePaint | undefined)?.imageHash, "abc");
  assert.deepEqual(out.diagnostics, []);
});

test("paints beyond the slots raise multi-paint-flattened, never drop silently", () => {
  const out = lowerFillPaints(paints(
    { type: "GRADIENT_LINEAR", gradientStops: [] },
    // A SEMI-TRANSPARENT solid above a gradient has no slot (an opaque one
    // would trim the stack instead — covered by the test above).
    solid(1, 0, 0, { opacity: 0.5 }),
    { type: "GRADIENT_RADIAL", gradientStops: [] },
  ));
  assert.equal(out.gradientPaint?.type, "GRADIENT_LINEAR");
  assert.equal(out.backgroundColor, undefined);
  const flat = out.diagnostics.find((d) => d.code === "multi-paint-flattened");
  assert.ok(flat, "expected a multi-paint-flattened diagnostic");
  assert.equal(flat.kind, "capture_partial");
  assert.match(flat.message, /SOLID, GRADIENT_RADIAL/);
});

test("an opaque solid above a gradient hides it exactly — no diagnostic", () => {
  // Trimming at the last fully opaque solid is exact: the paints below it are
  // invisible in Figma too, so nothing is owed a diagnostic.
  const out = lowerFillPaints(paints(
    { type: "GRADIENT_LINEAR", gradientStops: [] },
    solid(0.5, 0.5, 0.5),
  ));
  assert.equal(out.backgroundColor, "#808080");
  assert.equal(out.gradientPaint, undefined);
  assert.deepEqual(out.diagnostics, []);
});

test("hidden paints are ignored entirely", () => {
  const out = lowerFillPaints(paints(
    solid(1, 0, 0, { visible: false }),
    solid(0, 1, 0),
  ));
  assert.equal(out.backgroundColor, "#00ff00");
  assert.deepEqual(out.diagnostics, []);
});

test("VIDEO / PATTERN paints diagnose as unsupported and never shadow a solid", () => {
  const out = lowerFillPaints(paints(
    solid(0, 0, 1),
    { type: "VIDEO" },
    { type: "PATTERN" },
  ));
  assert.equal(out.backgroundColor, "#0000ff");
  const un = out.diagnostics.find((d) => d.code === "unsupported-paint-type");
  assert.ok(un, "expected an unsupported-paint-type diagnostic");
  assert.equal(un.kind, "unsupported_property");
  assert.match(un.message, /VIDEO, PATTERN/);
});

test("non-NORMAL paint blend modes are stated, and the paint still lowers", () => {
  const out = lowerFillPaints(paints(solid(1, 1, 1, { blendMode: "MULTIPLY" })));
  assert.equal(out.backgroundColor, "#ffffff");
  const blend = out.diagnostics.find((d) => d.code === "paint-blend-unsupported");
  assert.ok(blend, "expected a paint-blend-unsupported diagnostic");
  assert.match(blend.message, /MULTIPLY/);
});

test("image scale modes map to object-fit: FILL→cover FIT→contain exactly", () => {
  assert.deepEqual(scaleModeToObjectFit("FILL"), { fit: "cover", exact: true });
  assert.deepEqual(scaleModeToObjectFit(undefined), { fit: "cover", exact: true });
  assert.deepEqual(scaleModeToObjectFit("FIT"), { fit: "contain", exact: true });
  // CROP's transform-defined window has no CSS equivalent; cover is the
  // aspect-preserving approximation and the caller diagnoses it.
  assert.deepEqual(scaleModeToObjectFit("CROP"), { fit: "cover", exact: false });
  // TILE cannot be painted by the image widget; the stretch default stands.
  assert.deepEqual(scaleModeToObjectFit("TILE"), { exact: false });
});
