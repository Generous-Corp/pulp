// Focused integration tests for vector layout reconciliation, vector-network
// fallback, and real stroke-channel lowering. Run: node --test
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { geometryToPath } from './paths.mjs';
import { materializeFrame, buildScene, findFrame } from './scene.mjs';

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

const MOVE = 1, LINE = 2, CLOSE = 0;
const CTX = {
  images: new Map(),
  parserVersion: 't',
  compatSchemaVersion: '1',
  exportedAt: 'now',
  fileKey: null,
};

function firstVector(root) {
  const out = [];
  (function walk(n) { out.push(n); (n.children || []).forEach(walk); })(root);
  return out.find((n) => n.type === 'vector');
}

// ── mirrored nodes: min-corner truth, double-flip ink, stroke-slot margins ───

test('a mirrored flex child reports its VISUAL box, and its ink un-mirrors (Env chip)', () => {
  // The Triaz "Reverse" chip: an auto-layout row whose icon frame carries
  // m00 = -1 with m02 on the box's RIGHT edge. Reading m02 as "left" put the
  // sidecar one full icon-width right of where Figma draws the box, so
  // layout_parity reported dx = -10.4 against a flex pass that had placed it
  // exactly right. And because the emitted tree drops a container's mirror,
  // the icon frame's flip no longer cancels its child Union's own baked flip
  // — the reverse arrow rendered pointing forwards.
  const row = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'Row',
    size: { x: 70, y: 16 }, stackMode: 'HORIZONTAL', stackSpacing: 6,
    stackHorizontalPadding: 6, stackPaddingRight: 6 };
  const icon = { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Icon',
    parentIndex: { guid: row.guid, position: '!' },
    size: { x: 10.4, y: 9 },
    transform: { m00: -1, m01: 0, m02: 16.4, m10: 0, m11: 1, m12: 3.5 } };
  // An asymmetric triangle, so a residual mirror cannot hide.
  const union = { guid: { sessionID: 0, localID: 3 }, type: 'VECTOR', name: 'Union',
    parentIndex: { guid: icon.guid, position: '!' },
    size: { x: 10.4, y: 8 },
    transform: { m00: -1, m01: 0, m02: 10.4, m10: 0, m11: 1, m12: 0 },
    fillGeometry: [{ commandsBlob: 0 }],
    fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] };
  const localBlobs = [
    { bytes: encode([MOVE, 0, 0], [LINE, 4, 0], [LINE, 0, 8], [CLOSE]) },
  ];
  const scene = buildScene({ nodeChanges: [row, icon, union], blobs: localBlobs });
  const { envelope, geometry } = materializeFrame(scene, findFrame(scene, 'Row'), CTX);

  // Sidecar truth is the box Figma DRAWS: [m02 - w, m02], not [m02, m02 + w].
  const iconRect = geometry.nodes.find((n) => n.node_id === '0:2');
  assert.equal(iconRect.x, 6, 'mirror-aware min corner: 16.4 - 10.4');
  assert.equal(iconRect.y, 3.5, 'the unmirrored axis keeps its translation');

  // The ink sidecar composes the ancestor mirror the same way. Union's placed
  // path spans [6.4, 10.4] inside Icon; Icon's flip maps that to [6, 10].
  const unionRect = geometry.nodes.find((n) => n.node_id === '0:3');
  assert.equal(unionRect.x, 6, 'ink min corner under a mirrored ancestor');
  assert.equal(unionRect.width, 4);

  // Icon's flip (dropped from the emitted tree) cancels Union's own baked
  // flip: the emitted path must be the RAW triangle again — double-flip is
  // literal cancellation, which is how the reverse arrow points backwards.
  const v = firstVector(envelope.root);
  assert.equal(v.path_data, 'M0 0 L4 0 L0 8 Z');
});

test('a mirrored ABSOLUTE node is placed at its visual box, 180deg stays pinned', () => {
  // Same min-corner contract for styleFor's plain-placement branch: a mirror
  // moves the box to the other side of the stored origin. A 180deg rotation
  // (BOTH axes negative) must keep raw m02/m12 placement — that behavior is
  // pinned by the slider-fill lesson and its test in fig.test.mjs.
  const frame = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'F', size: { x: 100, y: 40 } };
  const flipped = { guid: { sessionID: 0, localID: 2 }, type: 'ROUNDED_RECTANGLE', name: 'flipped',
    parentIndex: { guid: frame.guid, position: '!' },
    size: { x: 18, y: 6 },
    transform: { m00: -1, m01: 0, m02: 30, m10: 0, m11: 1, m12: 3 } };
  const scene = buildScene({ nodeChanges: [frame, flipped] });
  const { envelope, geometry } = materializeFrame(scene, frame, CTX);
  const child = envelope.root.children.find((n) => n.name === 'flipped');
  assert.equal(child.style.left, 12, 'visual left = m02 - w under m00 = -1');
  assert.equal(child.style.top, 3);
  const rect = geometry.nodes.find((n) => n.node_id === '0:2');
  assert.equal(rect.x, 12, 'sidecar and render must be the same quantity');
});

test('a mirrored ancestor reflects intermediate containers and their descendants', () => {
  const root = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'Root',
    size: { x: 20, y: 20 } };
  const flipped = { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Flipped',
    parentIndex: { guid: root.guid, position: '!' },
    size: { x: 20, y: 20 },
    transform: { m00: -1, m01: 0, m02: 20, m10: 0, m11: 1, m12: 0 } };
  const middle = { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Middle',
    parentIndex: { guid: flipped.guid, position: '!' },
    size: { x: 8, y: 8 },
    transform: { m00: 1, m01: 0, m02: 2, m10: 0, m11: 1, m12: 3 } };
  const leaf = { guid: { sessionID: 0, localID: 4 }, type: 'ROUNDED_RECTANGLE', name: 'Leaf',
    parentIndex: { guid: middle.guid, position: '!' },
    size: { x: 2, y: 2 },
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 1 } };
  const scene = buildScene({ nodeChanges: [root, flipped, middle, leaf] });
  const { envelope } = materializeFrame(scene, root, CTX);
  const emittedFlipped = envelope.root.children[0];
  const emittedMiddle = emittedFlipped.children[0];
  const emittedLeaf = emittedMiddle.children[0];

  assert.equal(emittedFlipped.style.left, 0);
  assert.equal(emittedMiddle.style.left, 10,
    'the 8px container at x=2 reflects to 20-(2+8)');
  assert.equal(emittedMiddle.style.top, 3);
  assert.equal(emittedLeaf.style.left, 6,
    'the descendant reflects within its emitted 8px parent');
  assert.equal(emittedLeaf.style.top, 1);
});

test('aligned-stroke outline recovery preserves an ancestor mirror', () => {
  const root = {
    guid: { sessionID: 0, localID: 1 },
    type: 'FRAME',
    name: 'Root',
    size: { x: 20, y: 20 },
  };
  const frame = {
    guid: { sessionID: 0, localID: 2 },
    parentIndex: { guid: root.guid, position: '!' },
    type: 'FRAME',
    name: 'Flipped frame',
    size: { x: 20, y: 20 },
    transform: { m00: -1, m01: 0, m02: 20, m10: 0, m11: 1, m12: 0 },
  };
  const vector = {
    guid: { sessionID: 0, localID: 3 },
    parentIndex: { guid: frame.guid, position: '!' },
    type: 'VECTOR',
    name: 'asymmetric',
    size: { x: 4, y: 8 },
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
    strokeWeight: 2,
    strokeAlign: 'INSIDE',
    fillGeometry: [{ commandsBlob: 0 }],
    strokeGeometry: [{ commandsBlob: 1 }],
    fillPaints: [],
    strokePaints: [
      { type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true },
    ],
  };
  const fillOutline = encode(
    [MOVE, 0, 0], [LINE, 4, 0], [LINE, 0, 8], [CLOSE]);
  const strokeBand = encode(
    [MOVE, -2, -2], [LINE, 6, -2], [LINE, 6, 10], [LINE, -2, 10], [CLOSE]);
  const scene = buildScene({
    nodeChanges: [root, frame, vector],
    blobs: [{ bytes: fillOutline }, { bytes: strokeBand }],
  });
  const { envelope } = materializeFrame(scene, root, CTX);
  const path = firstVector(envelope.root);
  assert.equal(path.path_data, 'M4 0 L0 0 L4 8 Z',
    'the recovered fill outline keeps the dropped parent flip');
  assert.equal(path.style.left, 16,
    'the mirrored 4px ink box occupies [16,20] inside its 20px parent');
  assert.equal(path.style.top, 0);
});

test('a flowing vector reconciles stroke-inflated ink to its node box with margins', () => {
  // Figma's auto-layout never sees a stroke, but the emitted ink box includes
  // it (a CENTER stroke band overhangs the node box by half its weight). The
  // env chip's 12px arrow carries a 2px stroke: letting the 14px ink ride the
  // flex row pushed the "env" label 2px right. The widget keeps the exact ink
  // (nothing rescales); margins hand Yoga back the node box.
  const row = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'Row',
    size: { x: 48, y: 16 }, stackMode: 'HORIZONTAL', stackSpacing: 6,
    stackHorizontalPadding: 6, stackPaddingRight: 6 };
  const vec = { guid: { sessionID: 0, localID: 2 }, type: 'VECTOR', name: 'arrow',
    parentIndex: { guid: row.guid, position: '!' },
    size: { x: 12, y: 6 }, strokeWeight: 2,
    transform: { m00: 1, m01: 0, m02: 6, m10: 0, m11: 1, m12: 5 },
    fillGeometry: [],
    strokeGeometry: [{ commandsBlob: 0 }],
    fillPaints: [],
    strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] };
  const localBlobs = [
    // Stroke band overhanging the 12x6 node box by 1px on every side.
    { bytes: encode([MOVE, -1, -1], [LINE, 13, -1], [LINE, 13, 7], [LINE, -1, 7], [CLOSE]) },
  ];
  const scene = buildScene({ nodeChanges: [row, vec], blobs: localBlobs });
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Row'), CTX);
  const v = firstVector(envelope.root);
  assert.equal(v.style.width, 14, 'the widget keeps the exact ink');
  assert.deepEqual(
    [v.layout.marginLeft, v.layout.marginRight, v.layout.marginTop, v.layout.marginBottom],
    [-1, -1, -1, -1],
    'margins hand the flex row back the 12x6 node box');
});

test('a flowing vector swaps asymmetric ink margins under an ancestor mirror', () => {
  const root = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'Root',
    size: { x: 48, y: 16 } };
  const row = { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Mirrored row',
    parentIndex: { guid: root.guid, position: '!' },
    size: { x: 48, y: 16 }, stackMode: 'HORIZONTAL',
    transform: { m00: -1, m01: 0, m02: 48, m10: 0, m11: 1, m12: 0 } };
  const vec = { guid: { sessionID: 0, localID: 3 }, type: 'VECTOR', name: 'arrow',
    parentIndex: { guid: row.guid, position: '!' },
    size: { x: 12, y: 6 }, strokeWeight: 2,
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
    fillGeometry: [],
    strokeGeometry: [{ commandsBlob: 0 }],
    fillPaints: [],
    strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] };
  const localBlobs = [
    // One pixel overhang on the left, two on the right.
    { bytes: encode([MOVE, -1, 0], [LINE, 14, 0], [LINE, 14, 6], [LINE, -1, 6], [CLOSE]) },
  ];
  const scene = buildScene({ nodeChanges: [root, row, vec], blobs: localBlobs });
  const { envelope } = materializeFrame(scene, root, CTX);
  const v = firstVector(envelope.root);
  assert.deepEqual(
    [v.layout.marginLeft, v.layout.marginRight],
    [-2, -1],
    'the mirrored ink swaps its left/right overhang reconciliation');
});

test('an instance-scale ink gap is NOT "corrected" by margins', () => {
  // Through instance expansion the ink is solved at INSTANCE scale while
  // node.size can still be the master's. That gap is a scale delta, not a
  // stroke overhang; margins would move the child by it. The gate: overhangs
  // beyond strokeWeight + 1 leave the layout alone.
  const row = { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'Row',
    size: { x: 40, y: 40 }, stackMode: 'HORIZONTAL', stackSpacing: 10,
    stackHorizontalPadding: 10, stackPaddingRight: 10 };
  const vec = { guid: { sessionID: 0, localID: 2 }, type: 'VECTOR', name: 'stale-box',
    parentIndex: { guid: row.guid, position: '!' },
    size: { x: 32, y: 20 }, strokeWeight: 1,
    transform: { m00: 1, m01: 0, m02: 4, m10: 0, m11: 1, m12: 10 },
    fillGeometry: [{ commandsBlob: 0 }],
    fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] };
  const localBlobs = [
    // Ink solved at 0.8 instance scale: 25.6 wide vs the stale 32 node box.
    { bytes: encode([MOVE, 4, 10], [LINE, 29.6, 10], [LINE, 29.6, 26], [LINE, 4, 26], [CLOSE]) },
  ];
  const scene = buildScene({ nodeChanges: [row, vec], blobs: localBlobs });
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Row'), CTX);
  const v = firstVector(envelope.root);
  assert.ok(!v.layout || v.layout.marginLeft === undefined,
    'a scale-delta gap must not become margins');
});

// ── vector-network fallback + stroke channels ───────────────────────────────

// Encode a vector-network blob the way Figma does (see paths.mjs):
// header [u32 v][u32 s][u32 regions], then 12-byte vertices and 28-byte
// segments, all little-endian.
function encodeNetwork(vertices, segments, regionCount = 0) {
  const buf = Buffer.alloc(12 + vertices.length * 12 + segments.length * 28);
  buf.writeUInt32LE(vertices.length, 0);
  buf.writeUInt32LE(segments.length, 4);
  buf.writeUInt32LE(regionCount, 8);
  vertices.forEach(([x, y], i) => {
    const o = 12 + i * 12;
    buf.writeFloatLE(x, o + 4);
    buf.writeFloatLE(y, o + 8);
  });
  segments.forEach(([start, tx0, ty0, end, tx1, ty1], i) => {
    const o = 12 + vertices.length * 12 + i * 28;
    buf.writeUInt32LE(start, o + 4);
    buf.writeFloatLE(tx0, o + 8);
    buf.writeFloatLE(ty0, o + 12);
    buf.writeUInt32LE(end, o + 16);
    buf.writeFloatLE(tx1, o + 20);
    buf.writeFloatLE(ty1, o + 24);
  });
  return buf;
}

// A unit diamond as a closed 4-line loop (zero tangents = straight lines).
const diamondNetwork = () => encodeNetwork(
  [[1, 0.5], [0.5, 1], [0, 0.5], [0.5, 0]],
  [
    [0, 0, 0, 1, 0, 0],
    [1, 0, 0, 2, 0, 0],
    [2, 0, 0, 3, 0, 0],
    [3, 0, 0, 0, 0, 0],
  ],
);

test('an open vector-network walk starts at an endpoint even when segments are unordered', () => {
  const node = {
    type: 'VECTOR', name: 'open-chain',
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
    size: { x: 2, y: 1 },
    vectorData: { vectorNetworkBlob: 0, normalizedSize: { x: 2, y: 1 } },
    fillPaints: [],
    strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }],
  };
  // The first serialized segment starts in the middle. Walking from vertex 1
  // would produce two subpaths and change the join/cap/dash semantics there.
  const network = encodeNetwork(
    [[0, 0], [1, 1], [2, 0]],
    [
      [1, 0, 0, 2, 0, 0],
      [0, 0, 0, 1, 0, 0],
    ],
  );
  const r = geometryToPath(node, [{ bytes: network }]);
  assert.equal(r.d, 'M2 0 L1 1 L0 0');
  assert.equal((r.d.match(/\bM/g) || []).length, 1, 'one connected chain is one subpath');
});

test('a geometry-less vector falls back to its vector network as a centerline', () => {
  // The FX knobs' `Oval` rim: master and every instance carry EMPTY
  // fillGeometry/strokeGeometry, and the only shape in the file is the
  // authored network. The decode scales normalizedSize → node size and marks
  // the result `centerline` so the caller STROKES it rather than filling.
  const node = {
    type: 'VECTOR', name: 'Oval',
    transform: { m00: 1, m01: 0, m02: 8, m10: 0, m11: 1, m12: 8 },
    size: { x: 20, y: 20 },
    vectorData: { vectorNetworkBlob: 0, normalizedSize: { x: 1, y: 1 } },
    strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }],
    strokeWeight: 2,
  };
  const r = geometryToPath(node, [{ bytes: diamondNetwork() }]);
  assert.ok(r, 'network resolves');
  assert.equal(r.centerline, true);
  assert.equal(r.paint, 'stroke');
  // Scaled 1x1 → 20x20 and placed by the transform.
  assert.equal(r.box.width, 20);
  assert.equal(r.box.height, 20);
  assert.equal(r.box.minX, 8);
  // The closing segment is emitted explicitly before Z — a curved closer
  // NEEDS its cubic, and a straight one is merely redundant with Z.
  assert.equal(r.d, 'M20 10 L10 20 L0 10 L10 0 L20 10 Z');
});

test('curved network segments decode as cubics with vertex-relative tangents', () => {
  // One quarter-arc: tangents are OFFSETS from their vertices (the classic
  // kappa*r circle construction), so control points = vertex + tangent.
  const blob = encodeNetwork(
    [[10, 0], [0, 10]],
    [[0, 0, 5.5, 1, 5.5, 0]],
  );
  const node = {
    type: 'VECTOR', name: 'arc', size: { x: 10, y: 10 },
    vectorData: { vectorNetworkBlob: 0, normalizedSize: { x: 10, y: 10 } },
    strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }],
    strokeWeight: 1,
  };
  const r = geometryToPath(node, [{ bytes: blob }]);
  assert.ok(r);
  assert.match(r.d, /C10 5.5 5.5 10 0 10/);
});

test('the network decode refuses what it cannot verify', () => {
  const node = (blobBytes) => ({
    type: 'VECTOR', name: 'x', size: { x: 10, y: 10 },
    vectorData: { vectorNetworkBlob: 0, normalizedSize: { x: 1, y: 1 } },
    strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }],
    strokeWeight: 1,
  });
  // Truncated / wrong length → refused (exact byte consumption is the guard).
  assert.equal(geometryToPath(node(), [{ bytes: diamondNetwork().subarray(0, 40) }]), null);
  // Regions present → refused (unverified layout).
  const withRegions = encodeNetwork([[0, 0], [1, 1]], [[0, 0, 0, 1, 0, 0]], 1);
  assert.equal(geometryToPath(node(), [{ bytes: withRegions }]), null);
  // A branching vertex (3 segments meet) → refused; chaining it greedily
  // would draw a plausible-looking wrong shape.
  const branching = encodeNetwork(
    [[0, 0], [1, 0], [1, 1], [0, 1]],
    [
      [0, 0, 0, 1, 0, 0],
      [1, 0, 0, 2, 0, 0],
      [1, 0, 0, 3, 0, 0],
    ],
  );
  assert.equal(geometryToPath(node(), [{ bytes: branching }]), null);
  // A BOOLEAN_OPERATION never takes the network fallback — its operands (the
  // children the walk descends into) are the faithful rendering.
  const bo = {
    type: 'BOOLEAN_OPERATION', name: 'union', size: { x: 10, y: 10 },
    vectorData: { vectorNetworkBlob: 0, normalizedSize: { x: 1, y: 1 } },
  };
  assert.equal(geometryToPath(bo, [{ bytes: diamondNetwork() }]), null);
});

test('a filled vector lowers its stroke as real stroke channels on the same path', () => {
  // Sub-case (b) of the stroke-survival fix: SvgPathWidget fills and then
  // strokes the SAME path, and for a CENTER-aligned stroke the fill outline
  // IS the stroke centerline — so nothing is dropped and no border doubles it.
  const scene = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { type: 'VECTOR', name: 'blob',
        guid: { sessionID: 0, localID: 3 },
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        transform: { m00: 1, m01: 0, m02: 10, m10: 0, m11: 1, m12: 10 },
        size: { x: 20, y: 20 }, strokeWeight: 1.5, strokeAlign: 'CENTER',
        fillGeometry: [{ commandsBlob: 0 }],
        fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 1, a: 1 }, visible: true }],
        strokePaints: [{ type: 'SOLID', color: { r: 1, g: 0, b: 0, a: 1 }, opacity: 0.5, visible: true }] },
    ],
    blobs: [{ bytes: encode([MOVE, 0, 0], [LINE, 20, 0], [LINE, 20, 20], [LINE, 0, 20], [CLOSE]) }],
  });
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Panel'), CTX);
  const v = envelope.root.children.find((n) => n.name === 'blob');
  assert.equal(v.fill, '#0000ff');
  // Paint opacity folds into the emitted alpha, exactly as fills do.
  assert.equal(v.stroke, '#ff000080');
  assert.equal(v.strokeWidth, 1.5);
  assert.ok(!(v.style && v.style.border), 'no border beside the path stroke');
  // CENTER align is exact — nothing to confess.
  assert.ok(!diagnostics.some((d) => d.code === 'stroke-align-approximated'));
});

test('an INSIDE stroke band is re-lowered as a centered stroke on the shape outline', () => {
  // Sub-case (c): Figma bakes INSIDE/OUTSIDE bands UNCLIPPED — the boundary
  // outlined at ±weight, DOUBLE the declared width (a real file's Polygon 5,
  // weight 2 INSIDE, carries a 4px band). Filling that band paints fat and
  // bright. The decoder prefers the fill outline + a centered stroke channel,
  // and says so.
  const fillOutline = encode([MOVE, 0, 0], [LINE, 20, 0], [LINE, 20, 20], [LINE, 0, 20], [CLOSE]);
  const fatBand = encode(
    [MOVE, -2, -2], [LINE, 22, -2], [LINE, 22, 22], [LINE, -2, 22], [CLOSE],
    [MOVE, 2, 2], [LINE, 18, 2], [LINE, 18, 18], [LINE, 2, 18], [CLOSE],
  );
  const mkScene = (strokeAlign) => buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { type: 'VECTOR', name: 'tri',
        guid: { sessionID: 0, localID: 3 },
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        transform: { m00: 1, m01: 0, m02: 10, m10: 0, m11: 1, m12: 10 },
        size: { x: 20, y: 20 }, strokeWeight: 2, strokeAlign,
        fillGeometry: [{ commandsBlob: 0 }],
        strokeGeometry: [{ commandsBlob: 1 }],
        fillPaints: [],
        strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, opacity: 0.35, visible: true }] },
    ],
    blobs: [{ bytes: fillOutline }, { bytes: fatBand }],
  });

  const inside = materializeFrame(mkScene('INSIDE'), findFrame(mkScene('INSIDE'), 'Panel'), CTX);
  const tri = inside.envelope.root.children.find((n) => n.name === 'tri');
  // The path is the 20x20 shape outline, not the 24x24 unclipped band.
  assert.equal(tri.style.width, 20);
  assert.equal(tri.fill, 'none');
  assert.equal(tri.stroke, '#ffffff59');
  assert.equal(tri.strokeWidth, 2);
  assert.ok(inside.diagnostics.some((d) => d.code === 'stroke-align-approximated'),
    'the half-weight approximation is stated');

  // CENTER keeps the exact baked band — filled, never re-stroked.
  const center = materializeFrame(mkScene('CENTER'), findFrame(mkScene('CENTER'), 'Panel'), CTX);
  const band = center.envelope.root.children.find((n) => n.name === 'tri');
  assert.equal(band.style.width, 24);
  assert.equal(band.fill, '#ffffff59');
  assert.equal(band.stroke, undefined);
});

test('an ellipse with a gradient stroke lowers it to the strokeGradient channel', () => {
  // The knob-base rim: an ELLIPSE (box branch — its path is synthesized
  // downstream) whose GRADIENT_LINEAR stroke used to be dropped with "no
  // solid paint to flatten to".
  const scene = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { type: 'ELLIPSE', name: 'base',
        guid: { sessionID: 0, localID: 3 },
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        transform: { m00: 1, m01: 0, m02: 10, m10: 0, m11: 1, m12: 10 },
        size: { x: 22, y: 22 }, strokeWeight: 0.94, strokeAlign: 'OUTSIDE',
        fillPaints: [{ type: 'SOLID', color: { r: 0.17, g: 0.17, b: 0.18, a: 1 }, opacity: 0.8, visible: true }],
        strokePaints: [{ type: 'GRADIENT_LINEAR', opacity: 1, visible: true,
          transform: { m00: 0, m01: 1, m02: 0, m10: -1, m11: 0, m12: 1 },
          stops: [
            { color: { r: 1, g: 1, b: 1, a: 0.25 }, position: 0 },
            { color: { r: 0.19, g: 0.19, b: 0.19, a: 0.24 }, position: 1 },
          ] }] },
    ],
  });
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Panel'), CTX);
  const base = envelope.root.children.find((n) => n.name === 'base');
  assert.equal(base.type, 'ellipse');
  assert.match(base.strokeGradient, /^linear-gradient\(/);
  assert.equal(base.strokeWidth, 0.94);
  assert.ok(!(base.style && base.style.border), 'no flattened border beside the gradient channel');
  assert.ok(diagnostics.some((d) => d.code === 'stroke-align-approximated'),
    'OUTSIDE align approximation is stated');
  assert.ok(!diagnostics.some((d) => d.code === 'complex-stroke-flattened'),
    'the gradient is carried, not flattened');
});

test('same-path stroke recovery diagnoses unsupported cap, join, dash, and miter geometry', () => {
  const outline = encode([MOVE, 0, 0], [LINE, 20, 0], [LINE, 20, 20], [LINE, 0, 20], [CLOSE]);
  const scene = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { type: 'VECTOR', name: 'ornate',
        guid: { sessionID: 0, localID: 3 },
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
        size: { x: 20, y: 20 }, strokeWeight: 2, strokeAlign: 'CENTER',
        strokeCap: 'ROUND', strokeJoin: 'BEVEL', miterLimit: 8, dashPattern: [4, 2],
        fillGeometry: [{ commandsBlob: 0 }],
        fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }],
        strokePaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] },
    ],
    blobs: [{ bytes: outline }],
  });
  const { diagnostics } = materializeFrame(scene, findFrame(scene, 'Panel'), CTX);
  const diag = diagnostics.find((d) => d.code === 'complex-stroke-flattened');
  assert.ok(diag, 'unsupported stroke geometry must be stated, not silently flattened');
  assert.match(diag.detail, /dash pattern/);
  assert.match(diag.detail, /ROUND caps/);
  assert.match(diag.detail, /BEVEL joins/);
  assert.match(diag.detail, /miter limit 8/);
});

test('diagnostics carry the instance-path node id, not the master guid', () => {
  // pushDiag used to record guidKey(node.guid) — the MASTER's id — for
  // symbol-expansion clones, while the envelope and materials sidecar name
  // the clone ('<instance>/<child>'). The material audit joins per node_id,
  // so every expanded diagnostic was invisible to it and an honest, out-loud
  // degradation scored as a SILENT drop.
  const scene = buildScene({
    nodeChanges: [
      { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
      { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
        size: { x: 100, y: 100 } },
      { guid: { sessionID: 0, localID: 10 }, type: 'SYMBOL', name: 'Widget',
        parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'b' },
        size: { x: 30, y: 30 } },
      // A vector child with NO geometry at all → 'vector-simplified'.
      { guid: { sessionID: 0, localID: 11 }, type: 'VECTOR', name: 'ghost',
        parentIndex: { guid: { sessionID: 0, localID: 10 }, position: 'a' },
        size: { x: 10, y: 10 },
        fillPaints: [{ type: 'SOLID', color: { r: 1, g: 0, b: 0, a: 1 }, visible: true }] },
      { guid: { sessionID: 0, localID: 20 }, type: 'INSTANCE', name: 'W1',
        parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
        symbolData: { symbolID: { sessionID: 0, localID: 10 } },
        size: { x: 30, y: 30 } },
    ],
  });
  const { diagnostics } = materializeFrame(scene, findFrame(scene, 'Panel'), CTX);
  const diag = diagnostics.find((d) => d.code === 'vector-simplified' && d.node_name === 'ghost');
  assert.ok(diag, 'the geometry-less clone is diagnosed');
  assert.equal(diag.node_id, '0:20/0:11');
});
