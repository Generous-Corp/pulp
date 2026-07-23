// Unit tests for Figma vector geometry → SVG path lowering. Run: node --test
//
// These assert the CONSUMER's contract, not the producer's convenience keys.
// design_ir_json.cpp reads `path_data` / `viewBox` / `fill` off the envelope
// node and only lowers a node to a native SvgPathWidget when its `type` is one
// of design_fidelity.cpp's vector kinds — so that is what is asserted here.
// Asserting a key the decoder happens to emit but nothing downstream reads is
// how the text regression stayed green while every string was discarded.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { decodePathBlob, applyTransform, boundsOf, toPathData, geometryToPath, geometryToClipPath } from './paths.mjs';
import { materializeFrame, buildScene, findFrame } from './scene.mjs';

// Encode a command stream the way Figma does: [u8 tag][f32 args…] little-endian.
function encode(...cmds) {
  const parts = [];
  for (const [tag, ...args] of cmds) {
    const b = Buffer.alloc(1 + args.length * 4);
    b.writeUInt8(tag, 0);
    args.forEach((v, i) => b.writeFloatLE(v, 1 + i * 4));
    parts.push(b);
  }
  return Buffer.concat(parts);
}

const MOVE = 1, LINE = 2, QUAD = 3, CUBIC = 4, CLOSE = 0;

test('decodePathBlob reads each command with its documented arity', () => {
  const blob = encode(
    [MOVE, 1, 2],
    [LINE, 3, 4],
    [QUAD, 5, 6, 7, 8],
    [CUBIC, 9, 10, 11, 12, 13, 14],
    [CLOSE],
  );
  assert.equal(toPathData(decodePathBlob(blob)), 'M1 2 L3 4 Q5 6 7 8 C9 10 11 12 13 14 Z');
});
test('a wrong arity does not silently parse — the encoding is pinned', () => {
  // A CUBIC's 6 floats must not be readable as anything else. If the arity
  // table drifts, this stream stops landing on the trailing CLOSE tag and the
  // decode either throws or yields different commands. This is the guard that
  // makes the "consumes every real blob exactly" property meaningful.
  const blob = encode([CUBIC, 1, 2, 3, 4, 5, 6], [CLOSE]);
  const cmds = decodePathBlob(blob);
  assert.equal(cmds.length, 2);
  assert.deepEqual(cmds[0], { op: 'C', coords: [1, 2, 3, 4, 5, 6] });
  assert.deepEqual(cmds[1], { op: 'Z', coords: [] });
});

test('an unknown command tag throws instead of truncating', () => {
  // Truncating would emit a subtly-wrong shape, which is far harder to notice
  // than a raised diagnostic.
  assert.throws(() => decodePathBlob(Buffer.from([0x7f, 0, 0, 0, 0])), /unknown path command tag 127/);
});

test('a truncated command throws instead of reading past the end', () => {
  const short = Buffer.concat([Buffer.from([MOVE]), Buffer.alloc(4)]); // one float short
  assert.throws(() => decodePathBlob(short), /truncated/);
});

test('applyTransform bakes a mirror into the geometry', () => {
  // m00 = -1 is the horizontal flip elysium's "Vector 5" actually carries. The
  // import lane has no consumer for IRStyle::transform, so a mirror that is not
  // baked into the points is silently lost and the shape renders unflipped.
  const cmds = decodePathBlob(encode([MOVE, 0, 0], [LINE, 10, 0]));
  const flipped = applyTransform(cmds, { m00: -1, m01: 0, m02: 100, m10: 0, m11: 1, m12: 5 });
  assert.deepEqual(flipped[0].coords, [100, 5]);
  assert.deepEqual(flipped[1].coords, [90, 5]);
});

test('applyTransform maps Bézier control points like on-curve points', () => {
  const cmds = decodePathBlob(encode([CUBIC, 1, 1, 2, 2, 3, 3]));
  const t = applyTransform(cmds, { m00: 2, m01: 0, m02: 0, m10: 0, m11: 2, m12: 0 });
  assert.deepEqual(t[0].coords, [2, 2, 4, 4, 6, 6]);
});

test('boundsOf spans every point, control points included', () => {
  const b = boundsOf(decodePathBlob(encode([MOVE, 0, 0], [CUBIC, -5, 0, 20, 0, 10, 3])));
  assert.deepEqual([b.minX, b.minY, b.maxX, b.maxY], [-5, 0, 20, 3]);
});

// ── geometryToPath ──────────────────────────────────────────────────────────

const strokeOnlyNode = () => ({
  type: 'VECTOR',
  name: 'line',
  transform: { m00: 1, m01: 0, m02: 40, m10: 0, m11: 1, m12: 7 },
  // Figma emits a fillGeometry even for a stroke-only shape (an open path has a
  // notional fill region); picking it would paint a filled blob where the design
  // shows a hairline.
  fillGeometry: [{ commandsBlob: 0 }],
  strokeGeometry: [{ commandsBlob: 1 }],
  fillPaints: [],
  strokePaints: [{ type: 'SOLID', color: { r: 1, g: 0, b: 0, a: 1 }, visible: true }],
});

const blobs = [
  { bytes: encode([MOVE, 0, 0], [LINE, 0, 20], [CLOSE]) },              // 0: degenerate fill
  { bytes: encode([MOVE, -1, 0], [LINE, 1, 0], [LINE, 1, 20], [LINE, -1, 20], [CLOSE]) }, // 1: stroke outline
];

test('a stroke-only vector resolves to its stroke outline, painted as a fill', () => {
  // The stroke outline is already an expanded, fillable region. Re-stroking it
  // would outline the outline; painting it with the stroke color is correct.
  const r = geometryToPath(strokeOnlyNode(), blobs);
  assert.equal(r.paint, 'stroke');
  assert.equal(r.box.width, 2);
  assert.equal(r.box.height, 20);
  // Placed in parent space by the node transform, then normalized to a 0-origin
  // viewBox — so minX carries the -1 half-stroke overhang past the node's x.
  assert.equal(r.box.minX, 39);
  assert.equal(r.box.minY, 7);
  assert.equal(r.d, 'M0 0 L2 0 L2 20 L0 20 Z');
});

test('a stroke-band vector gets no CSS border — the band is the stroke', () => {
  // geometryToPath already paints the stroke band as a fill. Emitting the node's
  // stroke AGAIN as a `border` strokes that band's outline: two parallel lines
  // where the design has one (the doubled/too-thick triad-pad triangle and every
  // stroked ring). materializeFrame must suppress the border on such a node.
  const scene = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { ...strokeOnlyNode(), name: 'ring',
        guid: { sessionID: 0, localID: 3 },
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        size: { x: 2, y: 20 }, strokeWeight: 2 },
    ],
    blobs,
  });
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Panel'), {
    images: new Map(), fileKey: 'K', parserVersion: 't',
    compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z',
  });
  const ring = envelope.root.children.find((n) => n.name === 'ring');
  assert.equal(ring.type, 'vector');
  assert.ok(ring.fill && ring.fill !== 'none', 'stroke band is painted as a fill');
  assert.ok(!(ring.style && ring.style.border),
    'no redundant CSS border — it would double the outline');

  // A FILLED vector with a separate stroke keeps its border (paint = fill).
  const filled = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { type: 'VECTOR', name: 'blob',
        guid: { sessionID: 0, localID: 3 },
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        transform: { m00: 1, m01: 0, m02: 10, m10: 0, m11: 1, m12: 10 },
        size: { x: 20, y: 20 }, strokeWeight: 2,
        fillGeometry: [{ commandsBlob: 2 }],
        fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 1, a: 1 }, visible: true }],
        strokePaints: [{ type: 'SOLID', color: { r: 1, g: 0, b: 0, a: 1 }, visible: true }] },
    ],
    blobs: [...blobs, { bytes: encode([MOVE, 0, 0], [LINE, 20, 0], [LINE, 20, 20], [LINE, 0, 20], [CLOSE]) }],
  });
  const fv = materializeFrame(filled, findFrame(filled, 'Panel'), {
    images: new Map(), fileKey: 'K', parserVersion: 't',
    compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z',
  }).envelope.root.children.find((n) => n.name === 'blob');
  // A FILLED vector with a separate stroke used to lower it as a CSS border;
  // it now rides the path itself as real stroke channels (SvgPathWidget fills
  // and then strokes the same path), so no border may double it.
  assert.equal(fv.stroke, '#ff0000', 'a real fill + stroke strokes its own path');
  assert.equal(fv.strokeWidth, 2);
  assert.ok(!(fv.style && fv.style.border),
    'no redundant CSS border beside the path stroke — it would double the edge');
});

test('a filled vector prefers its fill outline and reports the dropped stroke', () => {
  const node = strokeOnlyNode();
  node.fillGeometry = [{ commandsBlob: 1 }];
  node.fillPaints = [{ type: 'SOLID', color: { r: 0, g: 1, b: 0, a: 1 }, visible: true }];
  const r = geometryToPath(node, blobs);
  assert.equal(r.paint, 'fill');
  // One path cannot carry both outlines; the caller is told rather than left to
  // guess that the stroke vanished.
  assert.equal(r.droppedStroke, true);
});

test('the declared winding rule survives onto the resolved path', () => {
  // The rule decides which regions of a multi-subpath path are HOLES, and
  // Figma's baked outlines do not promise direction-corrected contours: the
  // reference design's "Sub" speaker cabinet arrives as five same-direction
  // subpaths whose geometry declares ODD, and filling that nonzero paints a
  // solid slab where the design shows a hollow woofer.
  const node = strokeOnlyNode();
  node.fillGeometry = [{ commandsBlob: 1, windingRule: 'ODD' }];
  node.strokeGeometry = null;
  node.fillPaints = [{ type: 'SOLID', color: { r: 0, g: 1, b: 0, a: 1 }, visible: true }];
  assert.equal(geometryToPath(node, blobs).fillRule, 'evenodd');

  node.fillGeometry = [{ commandsBlob: 1, windingRule: 'NONZERO' }];
  assert.equal(geometryToPath(node, blobs).fillRule, 'nonzero');

  // Missing rule maps to null, NOT a guessed default: the caller diagnoses a
  // multi-subpath shape whose holes it may be filling solid.
  node.fillGeometry = [{ commandsBlob: 1 }];
  assert.equal(geometryToPath(node, blobs).fillRule, null);
});

test('evenodd wins when geometry regions disagree about the winding rule', () => {
  // One node's fillGeometry is a LIST of per-region entries, each with its own
  // rule ("Sub" is [NONZERO dot, ODD ring, ODD box-with-hole]) — and one
  // emitted path carries one rule. Figma direction-corrects its NONZERO
  // regions' contours, which fill identically under either rule, while an ODD
  // region's same-direction holes fill SOLID under nonzero. So evenodd is the
  // faithful merge, and `mixedWinding` states the (overlap-only) approximation.
  const node = strokeOnlyNode();
  node.strokeGeometry = null;
  node.fillPaints = [{ type: 'SOLID', color: { r: 0, g: 1, b: 0, a: 1 }, visible: true }];
  node.fillGeometry = [
    { commandsBlob: 1, windingRule: 'NONZERO' },
    { commandsBlob: 2, windingRule: 'ODD' },
  ];
  const localBlobs = [...blobs, { bytes: encode([MOVE, 0, 0], [LINE, 4, 0], [LINE, 4, 4], [LINE, 0, 4], [CLOSE]) }];
  const r = geometryToPath(node, localBlobs);
  assert.equal(r.fillRule, 'evenodd');
  assert.equal(r.mixedWinding, true);

  // Agreement is not "mixed" — no diagnostic noise on the common case.
  node.fillGeometry = [
    { commandsBlob: 1, windingRule: 'ODD' },
    { commandsBlob: 2, windingRule: 'ODD' },
  ];
  assert.equal(geometryToPath(node, localBlobs).mixedWinding, false);
});

test('a zero-area shape resolves to null rather than an invalid viewBox', () => {
  const node = strokeOnlyNode();
  node.strokeGeometry = null;
  node.fillGeometry = [{ commandsBlob: 0 }]; // width 0
  node.fillPaints = [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }];
  assert.equal(geometryToPath(node, blobs), null);
});

// ── materializeFrame: the end-to-end consumer contract ──────────────────────

function sceneWith(vectorNode) {
  const frame = {
    guid: { sessionID: 0, localID: 1 },
    type: 'FRAME',
    name: 'Frame',
    size: { x: 200, y: 100 },
  };
  const child = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' }, ...vectorNode };
  return {
    scene: buildScene({ nodeChanges: [frame, child], blobs }),
    frame,
  };
}

const CTX = { images: new Map(), parserVersion: 't', compatSchemaVersion: '1', exportedAt: 'now', fileKey: null };

function firstVector(root) {
  const out = [];
  (function walk(n) { out.push(n); (n.children || []).forEach(walk); })(root);
  return out.find((n) => n.type === 'vector');
}

test('a VECTOR reaches the envelope as a vector-kind node carrying path_data', () => {
  // `type: 'vector'` and `path_data` are exactly what design_codegen.cpp's
  // path branch requires (is_vector_kind(node.type) && attributes["path_data"]).
  // Emitting a 'frame' here — which is what this lane did — drops the shape to
  // an empty box no matter how good the geometry is.
  const { scene, frame } = sceneWith(strokeOnlyNode());
  const { envelope } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  assert.ok(v, 'vector node must reach the envelope');
  assert.equal(v.type, 'vector');
  assert.ok(v.path_data && v.path_data.startsWith('M'), 'path_data must be SVG path data');
  assert.match(v.viewBox, /^0 0 \d/, 'viewBox must be 0-origin: codegen reads only its w/h');
  assert.equal(v.style.position, 'absolute');
});

test('the vector-simplified diagnostic is gone BECAUSE the shape resolved', () => {
  const { scene, frame } = sceneWith(strokeOnlyNode());
  const { envelope, diagnostics } = materializeFrame(scene, frame, CTX);
  // Assert the shape actually rendered FIRST. "No vector-simplified warnings" is
  // also true of a decoder that ignores vectors entirely, so the absence of the
  // diagnostic is only evidence when paired with the presence of the geometry.
  assert.ok(firstVector(envelope.root)?.path_data, 'geometry must have resolved');
  assert.equal(diagnostics.filter((d) => d.code === 'vector-simplified').length, 0);
});

test('an unresolvable vector still reports vector-simplified exactly once', () => {
  const node = strokeOnlyNode();
  node.fillGeometry = null;
  node.strokeGeometry = null;
  const { scene, frame } = sceneWith(node);
  const { diagnostics } = materializeFrame(scene, frame, CTX);
  assert.equal(diagnostics.filter((d) => d.code === 'vector-simplified').length, 1);
});

// A donut in blob space: outer square + inner square, BOTH wound clockwise.
// Under evenodd the inner square is a hole; under the nonzero default it is
// not — this is exactly the shape class the winding rule exists to settle.
const donutBlob = { bytes: encode(
  [MOVE, 0, 0], [LINE, 20, 0], [LINE, 20, 20], [LINE, 0, 20], [CLOSE],
  [MOVE, 5, 5], [LINE, 15, 5], [LINE, 15, 15], [LINE, 5, 15], [CLOSE],
) };

function donutNode(windingExtra) {
  return {
    type: 'VECTOR', name: 'donut',
    transform: { m00: 1, m01: 0, m02: 10, m10: 0, m11: 1, m12: 10 },
    size: { x: 20, y: 20 },
    fillGeometry: [{ commandsBlob: 2, ...windingExtra }],
    fillPaints: [{ type: 'SOLID', color: { r: 1, g: 0, b: 0, a: 1 }, visible: true }],
  };
}

test('an evenodd vector reaches the envelope carrying fillRule', () => {
  // `fillRule` is the exact key design_ir_json reads into svg_fill_rule, which
  // codegen turns into setSvgFillRule — the bridge call SvgPathWidget already
  // consumes. Dropping it here fills the donut's hole solid, silently.
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 100, y: 100 } };
  const child = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' },
                  ...donutNode({ windingRule: 'ODD' }) };
  const scene = buildScene({ nodeChanges: [frame, child], blobs: [...blobs, donutBlob] });
  const { envelope, diagnostics } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  assert.equal(v.fillRule, 'evenodd');
  // Carried faithfully — nothing to confess.
  assert.equal(diagnostics.filter((d) => d.code === 'vector-fill-rule-approximated').length, 0);
});

test('a nonzero vector does not emit fillRule — it is the widget default', () => {
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 100, y: 100 } };
  const child = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' },
                  ...donutNode({ windingRule: 'NONZERO' }) };
  const scene = buildScene({ nodeChanges: [frame, child], blobs: [...blobs, donutBlob] });
  const { envelope } = materializeFrame(scene, frame, CTX);
  assert.equal(firstVector(envelope.root).fillRule, undefined);
});

test('a multi-subpath vector with no winding rule is diagnosed, never silent', () => {
  // The nonzero fallback can fill this donut's hole solid, and a solid slab
  // raises no error anywhere downstream — the diagnostic is the only witness.
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 100, y: 100 } };
  const child = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' },
                  ...donutNode({}) };
  const scene = buildScene({ nodeChanges: [frame, child], blobs: [...blobs, donutBlob] });
  const { diagnostics } = materializeFrame(scene, frame, CTX);
  const diags = diagnostics.filter((d) => d.code === 'vector-fill-rule-approximated');
  assert.equal(diags.length, 1);
  assert.equal(diags[0].severity, 'warning', 'info is dropped by both consumers');

  // A single-contour shape fills identically under either rule — a missing
  // declaration there is not a loss and must not cry wolf.
  const single = strokeOnlyNode();
  single.fillGeometry = [{ commandsBlob: 1 }];
  single.strokeGeometry = null;
  single.fillPaints = [{ type: 'SOLID', color: { r: 0, g: 1, b: 0, a: 1 }, visible: true }];
  const s2 = sceneWith(single);
  const r2 = materializeFrame(s2.scene, s2.frame, CTX);
  assert.equal(r2.diagnostics.filter((d) => d.code === 'vector-fill-rule-approximated').length, 0);
});

// Figma's paint transform maps the node's box INTO gradient space, so this is
// the inverse of the axis it describes: a plain top→bottom ramp.
const TOP_TO_BOTTOM = { m00: 0, m01: 1, m02: 0, m10: -1, m11: 0, m12: 1 };

function gradientVector(paintExtra = {}) {
  const node = strokeOnlyNode();
  node.fillGeometry = [{ commandsBlob: 1 }];
  node.strokeGeometry = null;
  node.fillPaints = [{
    type: 'GRADIENT_LINEAR',
    visible: true,
    opacity: 1,
    stops: [
      { color: { r: 1, g: 0, b: 0, a: 1 }, position: 0 },
      { color: { r: 0, g: 0, b: 1, a: 1 }, position: 1 },
    ],
    ...paintExtra,
  }];
  const { scene, frame } = sceneWith(node);
  const { envelope } = materializeFrame(scene, frame, CTX);
  return firstVector(envelope.root);
}

test('a gradient-filled vector paints its gradient, and never a black silhouette', () => {
  // Two contracts in one, because they failed together. SvgPathWidget defaults
  // fill_color_ to opaque black with has_fill_ on, so a vector whose paint we
  // cannot express MUST say so explicitly — staying silent paints a black blob.
  // And the paint we express has to be the gradient the designer drew: this
  // used to flatten to the mean of the stops, which is what made the knob rim
  // highlight a flat grey ring.
  const v = gradientVector({ transform: TOP_TO_BOTTOM });
  assert.ok(v.fillGradient, 'a lowerable gradient must survive AS a gradient');
  assert.match(v.fillGradient, /^linear-gradient\(180deg,/);
  // The solid rides along as the widget's parse-failure fallback, so the black
  // default can never surface — but it must not be the mean pretending to be
  // the paint.
  assert.ok(v.fill, 'a fill must always be emitted, never left to the black default');
  assert.notEqual(v.fill, '#000000');
});

test('a gradient we cannot express falls back to the mean rather than guessing', () => {
  // No transform means no axis, and there is no honest default direction: a
  // guessed `to bottom` is 180° wrong half the time and nothing would report
  // it. The mean is the truthful answer here, so assert it ON PURPOSE — a
  // flattened fill in THIS case is the contract, not the bug 6.7 deleted.
  // (The same reasoning covers RADIAL, which parse_svg_linear_gradient cannot
  // read at all.)
  const v = gradientVector();
  assert.equal(v.fillGradient, undefined, 'no axis ⇒ no gradient claim');
  assert.equal(v.fill, '#800080'); // mean of the stops
  assert.notEqual(v.fill, '#000000');
});

test("a stroke-only vector's GRADIENT stroke reaches the path as a gradient", () => {
  // The knob rim highlight, and the single busiest paint path in the reference
  // design: 41 of its 236 resolved vectors paint from `strokePaints`, and every
  // one of the 40 `Oval` rims is a GRADIENT_LINEAR stroke — white@0.24 fading to
  // transparent — on a node with NO fill paint at all.
  //
  // Nothing covered it, and that gap cost real time: material_audit.mjs scores a
  // declared stroke by looking for `style.border`, which a vector never emits
  // (its stroke rides on the PATH, as fill/fillGradient), so the audit reports
  // all 40 as silent drops. That false positive was read as a real one and
  // chased to a fix for a bug that was not there — the geometry pick, which
  // `hasVisibleFill` already gets right for exactly these nodes. The audit
  // cannot see this path; this test is the only thing that can.
  const node = strokeOnlyNode();          // fillPaints: [] — stroke geometry wins
  node.strokePaints = [{
    type: 'GRADIENT_LINEAR',
    visible: true,
    opacity: 1,
    transform: TOP_TO_BOTTOM,
    stops: [
      { color: { r: 1, g: 1, b: 1, a: 0.24 }, position: 0 },
      { color: { r: 0, g: 0, b: 0, a: 0 }, position: 1 },
    ],
  }];
  const { scene, frame } = sceneWith(node);
  const { envelope } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  // The stroke paint must be the one that lands, not the empty fill: an empty
  // fillPaints resolves to no color, so reading the wrong paint list here emits
  // `fill: 'none'` and the rim disappears silently.
  assert.ok(v.fillGradient, "a stroke gradient must survive AS a gradient, not flatten");
  assert.match(v.fillGradient, /^linear-gradient\(180deg,/);
  // The ramp's own colors, alpha included — a rim emitted opaque is a rim that
  // reads as a hard ring instead of a highlight.
  assert.match(v.fillGradient, /#ffffff3d/, 'the white@0.24 stop must keep its alpha');
  assert.match(v.fillGradient, /#00000000/, 'the transparent stop must stay transparent');
  // The mean rides along only as the widget's parse-failure fallback.
  assert.ok(v.fill && v.fill !== 'none', 'the flattened fallback must still be emitted');
});

test("a stroke paint's own opacity folds into its gradient stops", () => {
  // Figma multiplies paint.opacity by each stop's alpha. Reading only the stop
  // renders a rim the designer set to half strength at FULL strength — which
  // does not look like a dropped property, it looks like the design just has a
  // harder edge than it should, so nobody calls it a bug. The solid stroke path
  // takes this product already; so must this one.
  const node = strokeOnlyNode();
  node.strokePaints = [{
    type: 'GRADIENT_LINEAR',
    visible: true,
    opacity: 0.5,
    transform: TOP_TO_BOTTOM,
    stops: [
      { color: { r: 1, g: 1, b: 1, a: 1 }, position: 0 },
      { color: { r: 1, g: 1, b: 1, a: 1 }, position: 1 },
    ],
  }];
  const { scene, frame } = sceneWith(node);
  const { envelope } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  // 1.0 stop alpha x 0.5 paint opacity = 0x80, NOT 0xff.
  assert.match(v.fillGradient, /#ffffff80/, 'paint opacity x stop alpha, not the stop alone');
  assert.doesNotMatch(v.fillGradient, /#ffffffff/);
});

test('a vector with no expressible paint clears the fill rather than defaulting to black', () => {
  const node = strokeOnlyNode();
  node.fillGeometry = [{ commandsBlob: 1 }];
  node.strokeGeometry = null;
  node.fillPaints = [{ type: 'IMAGE', visible: true }];
  const { scene, frame } = sceneWith(node);
  const { envelope } = materializeFrame(scene, frame, CTX);
  assert.equal(firstVector(envelope.root).fill, 'none');
});

test('a vector inside an auto-layout parent keeps flowing', () => {
  // Vector placement overrides styleFor's left/top, but only for shapes styleFor
  // already pinned. Under an auto-layout parent the flex pass owns placement, so
  // pinning the shape here would yank it out of the flow it belongs in.
  const frame = {
    guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F',
    size: { x: 200, y: 100 }, stackMode: 'HORIZONTAL',
  };
  const child = {
    guid: { sessionID: 0, localID: 2 },
    parentIndex: { guid: frame.guid, position: '!' },
    ...strokeOnlyNode(),
  };
  const scene = buildScene({ nodeChanges: [frame, child], blobs });
  const { envelope } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  assert.ok(v.path_data, 'the shape still resolves');
  assert.equal(v.style.position, undefined, 'must not be pinned inside a flex parent');
  assert.equal(v.style.left, undefined);
});

test('a resolved vector is terminal — flattened operands are not re-emitted', () => {
  // A BOOLEAN_OPERATION's fillGeometry is the already-unioned result; its
  // children are the pre-union inputs. Emitting both draws the shape twice.
  const bool = {
    type: 'BOOLEAN_OPERATION',
    name: 'Union',
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
    fillGeometry: [{ commandsBlob: 1 }],
    fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }],
  };
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 200, y: 100 } };
  const b = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' }, ...bool };
  const operand = {
    guid: { sessionID: 0, localID: 3 },
    parentIndex: { guid: b.guid, position: '!' },
    type: 'VECTOR', name: 'operand', size: { x: 5, y: 5 },
  };
  const scene = buildScene({ nodeChanges: [frame, b, operand], blobs });
  const { envelope } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  assert.equal(v.name, 'Union');
  assert.ok(!v.children, 'a resolved boolean must not re-emit its operands');
});

// ── the geometry sidecar's rect for a resolved vector ────────────────────────

test("the geometry sidecar reports a vector's INK, not its node box", () => {
  // The sidecar is layout_parity.py's ground truth, and for every other node
  // kind it is the node's transform + size. A resolved vector is the exception:
  // its path is baked into parent space, so styleFor positions it by the path's
  // BOUNDS, and Figma's translation column can sit a long way from the geometry
  // it names. On a real file one `Bg PAnel` carries m02 = 462 while its path
  // starts at 350 — filling the 112-wide parent that ENDS at 462.
  //
  // Reading the node box made the parity tool report "112px misplaced" against
  // a vector Pulp had placed exactly right, and those phantoms were the three
  // WORST findings on an otherwise clean import — a checker crying wolf about
  // the very thing it exists to adjudicate. Both quantities are Figma's own
  // decoding; the bug was comparing one against the other.
  const node = {
    type: 'VECTOR',
    name: 'Bg PAnel',
    // The node box claims x=200 …
    transform: { m00: 1, m01: 0, m02: 200, m10: 0, m11: 1, m12: 0 },
    size: { x: 10, y: 10 },
    // … while the ink sits 100 to its LEFT and is 30 wide: nothing about the
    // box predicts either number.
    fillGeometry: [{ commandsBlob: 2 }],
    fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }],
  };
  const localBlobs = [
    ...blobs,
    { bytes: encode([MOVE, -100, 5], [LINE, -70, 5], [LINE, -70, 25], [LINE, -100, 25], [CLOSE]) },
  ];
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 400, y: 100 } };
  const child = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' }, ...node };
  const scene = buildScene({ nodeChanges: [frame, child], blobs: localBlobs });
  const { envelope, geometry } = materializeFrame(scene, frame, CTX);

  const v = firstVector(envelope.root);
  assert.ok(v && v.path_data, 'the vector resolved (otherwise this proves nothing)');

  const rect = geometry.nodes.find((n) => n.node_id === '0:2');
  // The ink: 200 + (-100) = 100, y = 5, and 30x20 — NOT the box's 200,0,10x10.
  assert.deepEqual([rect.x, rect.y, rect.width, rect.height], [100, 5, 30, 20]);
  // And it agrees with what the importer emitted, which is the whole point:
  // these two must be the same quantity or every vector reads as misplaced.
  assert.equal(rect.x, v.style.left);
  assert.equal(rect.y, v.style.top);
  assert.equal(rect.width, v.style.width);
});

test('an UNRESOLVED vector keeps its node box in the sidecar', () => {
  // The bound on the fix: no path means no ink to prefer, and the node box is
  // the only thing Figma has said about where this node is. Falling back to
  // nothing would drop the node from the sidecar and turn a real dropped-node
  // check into a silent skip.
  const node = {
    type: 'VECTOR', name: 'unreadable',
    transform: { m00: 1, m01: 0, m02: 60, m10: 0, m11: 1, m12: 12 },
    size: { x: 8, y: 9 },
    fillGeometry: null, strokeGeometry: null,
  };
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 200, y: 100 } };
  const child = { guid: { sessionID: 0, localID: 2 }, parentIndex: { guid: frame.guid, position: '!' }, ...node };
  const scene = buildScene({ nodeChanges: [frame, child], blobs });
  const { geometry } = materializeFrame(scene, frame, CTX);
  const rect = geometry.nodes.find((n) => n.node_id === '0:2');
  assert.deepEqual([rect.x, rect.y, rect.width, rect.height], [60, 12, 8, 9]);
});

// ── mask lowering ───────────────────────────────────────────────────────────

test('geometryToClipPath stays in parent space — no viewBox normalization', () => {
  // The consumer difference IS the contract: an emitted vector's d is
  // re-placed at its own box (0,0-origin), but a clip-path is consumed in the
  // clipped view's border-box space, so the transform-baked coordinates must
  // survive. Note the fill outline wins even with no visible fill paint: a
  // mask's SHAPE is what clips.
  const node = {
    type: 'VECTOR', name: 'mask',
    transform: { m00: 1, m01: 0, m02: 5, m10: 0, m11: 1, m12: 3 },
    fillGeometry: [{ commandsBlob: 2 }],
    fillPaints: [],
  };
  const clipBlobs = [...blobs, { bytes: encode([MOVE, 0, 0], [LINE, 30, 0], [LINE, 30, 10], [LINE, 15, 5], [LINE, 0, 10], [CLOSE]) }];
  const r = geometryToClipPath(node, clipBlobs);
  assert.equal(r.d, 'M5 3 L35 3 L35 13 L20 8 L5 13 Z');
});

test('a mask child clips the siblings above it and paints nowhere', () => {
  // Figma never renders a mask's own fill — it clips the siblings painted
  // AFTER it. Materializing the flag as a normal child painted an opaque
  // notched panel over the accent tab it was clipping a texture to. The
  // lowering: siblings above the mask move into a synthetic wrapper spanning
  // the parent that carries the mask outline as `clip_path` (the contract
  // parse_ir_style('clipPath') → setClipPath already consumes); the sibling
  // BELOW stays outside, unclipped — exactly Figma's scope.
  const scene = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { guid: { sessionID: 0, localID: 3 }, type: 'ROUNDED_RECTANGLE', name: 'below',
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
        size: { x: 10, y: 10 },
        transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
        fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }] },
      { guid: { sessionID: 0, localID: 4 }, type: 'VECTOR', name: 'notch', mask: true,
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        size: { x: 30, y: 10 },
        transform: { m00: 1, m01: 0, m02: 5, m10: 0, m11: 1, m12: 3 },
        fillGeometry: [{ commandsBlob: 2 }],
        fillPaints: [{ type: 'SOLID', color: { r: 0.3, g: 0.3, b: 0.3, a: 1 }, opacity: 1, visible: true }] },
      { guid: { sessionID: 0, localID: 5 }, type: 'ROUNDED_RECTANGLE', name: 'above',
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'c' },
        size: { x: 50, y: 50 },
        transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
        fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] },
    ],
    blobs: [...blobs, { bytes: encode([MOVE, 0, 0], [LINE, 30, 0], [LINE, 30, 10], [LINE, 15, 5], [LINE, 0, 10], [CLOSE]) }],
  });
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Panel'), CTX);
  const kids = envelope.root.children;
  // The mask's own fill must never reach the output, under any name or shape.
  const all = [];
  (function collect(n) { all.push(n); (n.children || []).forEach(collect); })(envelope.root);
  assert.ok(!all.some((n) => n.name === 'notch'), 'mask painted as content');
  assert.deepEqual(kids.map((n) => n.name), ['below', 'notch (mask scope)']);
  const scope = kids[1];
  // Parent-space outline, spanning wrapper — the clip lands where the design
  // put the mask while the masked sibling keeps its own coordinates.
  assert.equal(scope.style.clip_path, 'path("M5 3 L35 3 L35 13 L20 8 L5 13 Z")');
  assert.deepEqual([scope.style.left, scope.style.top, scope.style.width, scope.style.height], [0, 0, 100, 100]);
  assert.equal(scope.audio_widget, 'none');
  assert.equal(scope.node_id, '0:4/mask-scope');
  assert.deepEqual(scope.children.map((n) => n.name), ['above']);
  // An opaque solid alpha mask lowers exactly — no approximation to confess.
  assert.ok(!diagnostics.some((d) => d.code === 'mask-approximated'),
    'exact lowering must not raise mask-approximated');
});
