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

import { decodePathBlob, applyTransform, boundsOf, toPathData, geometryToPath } from './paths.mjs';
import { materializeFrame, buildScene } from './scene.mjs';

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
  // would outline the outline; painting it with the stroke colour is correct.
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

test('a gradient-filled vector never renders as a black silhouette', () => {
  // SvgPathWidget defaults fill_color_ to opaque black with has_fill_ on, so a
  // vector whose paint we cannot express MUST say so explicitly. Staying silent
  // paints a black blob — strictly worse than the plain box this lane used to
  // emit, and the one way lowering vectors could regress a real file.
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
  }];
  const { scene, frame } = sceneWith(node);
  const { envelope } = materializeFrame(scene, frame, CTX);
  const v = firstVector(envelope.root);
  assert.ok(v.fill, 'a fill must always be emitted, never left to the black default');
  assert.notEqual(v.fill, '#000000');
  assert.equal(v.fill, '#800080'); // mean of the stops
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
