// Figma vector geometry → SVG path data.
//
// A vector-ish node carries `fillGeometry` / `strokeGeometry`: arrays of
// `{ windingRule, commandsBlob, styleID }`, where `commandsBlob` indexes the
// root message's `blobs` array. Each such blob is a flat little-endian command
// stream — no header, no length prefix:
//
//     [u8 tag][f32 arg]*arity
//
//     tag 0 = CLOSE_PATH  (0 args)   → Z
//     tag 1 = MOVE_TO     (2 args)   → M x y
//     tag 2 = LINE_TO     (2 args)   → L x y
//     tag 3 = QUAD_TO     (4 args)   → Q x1 y1 x y
//     tag 4 = CUBIC_TO    (6 args)   → C x1 y1 x2 y2 x y
//
// The arity table is load-bearing and was established empirically: it is the
// only assignment that consumes every referenced blob in a real file to the
// byte with nothing left over (perturbing any single arity drops the exact-
// consumption rate from 96/96 to 65/96 or worse). A second, independent signal
// agrees: across two real files the stream contains exactly as many CLOSE tags
// as MOVE tags (278 each), i.e. every subpath opens and closes — which a
// misaligned parse would not produce.
//
// CAVEAT — tags 0/1/2/4 are confirmed against real files; **tag 3 (QUAD) is
// inferred**, never having appeared in any blob observed so far. Its arity of 4
// follows from the shape of the format (one control point + one endpoint), but
// it is unverified. This is why `decodePathBlob` throws rather than truncating:
// if the guess is wrong, the caller raises a `vector-simplified` diagnostic
// naming the node instead of silently emitting a mangled shape. If a file ever
// trips that error on tag 3, this table is the first place to look.
//
// Coordinates are in the node's LOCAL space. Figma pre-flattens the geometry it
// stores here: a BOOLEAN_OPERATION's `fillGeometry` is the already-resolved
// result, and `strokeGeometry` is the stroke expanded into a fillable outline.
// That is why this lane needs no vector-network evaluation and no boolean
// solver — the hard work is already baked into these blobs.

const ARITY = { 0: 0, 1: 2, 2: 2, 3: 4, 4: 6 };
const LETTER = { 0: 'Z', 1: 'M', 2: 'L', 3: 'Q', 4: 'C' };

/**
 * Decode one commands blob into an array of `{ op, coords }` records.
 * @param {Buffer|Uint8Array} bytes
 * @returns {{op: string, coords: number[]}[]}
 */
export function decodePathBlob(bytes) {
  const buf = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
  const out = [];
  let o = 0;
  while (o < buf.length) {
    const tag = buf.readUInt8(o);
    o += 1;
    const arity = ARITY[tag];
    if (arity === undefined) throw new Error(`unknown path command tag ${tag} at byte ${o - 1}`);
    if (o + arity * 4 > buf.length) throw new Error(`truncated path command ${tag} at byte ${o - 1}`);
    const coords = [];
    for (let i = 0; i < arity; i++) {
      coords.push(buf.readFloatLE(o));
      o += 4;
    }
    out.push({ op: LETTER[tag], coords });
  }
  return out;
}

/**
 * Apply a Figma affine transform (`{m00,m01,m02,m10,m11,m12}`) to every point of
 * a decoded command list, mapping node-local coordinates into parent space.
 *
 * Baking the matrix into the geometry is what makes rotated and mirrored vectors
 * survive: the alternative is emitting a CSS/native transform the import lane
 * does not yet consume, which silently flattens the shape back to its unrotated
 * form. An affine map sends Béziers to Béziers, so control points transform
 * exactly like on-curve points and the curve is preserved — no subdivision.
 */
export function applyTransform(commands, t) {
  if (!t) return commands;
  const { m00 = 1, m01 = 0, m02 = 0, m10 = 0, m11 = 1, m12 = 0 } = t;
  return commands.map(({ op, coords }) => {
    const mapped = [];
    for (let i = 0; i + 1 < coords.length; i += 2) {
      const x = coords[i];
      const y = coords[i + 1];
      mapped.push(m00 * x + m01 * y + m02, m10 * x + m11 * y + m12);
    }
    return { op, coords: mapped };
  });
}

/** Axis-aligned bounds of a command list, or null when it has no points. */
export function boundsOf(commands) {
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const { coords } of commands) {
    for (let i = 0; i + 1 < coords.length; i += 2) {
      minX = Math.min(minX, coords[i]);
      maxX = Math.max(maxX, coords[i]);
      minY = Math.min(minY, coords[i + 1]);
      maxY = Math.max(maxY, coords[i + 1]);
    }
  }
  if (!Number.isFinite(minX)) return null;
  return { minX, minY, maxX, maxY, width: maxX - minX, height: maxY - minY };
}

/** Translate every point by (dx, dy). */
export function translate(commands, dx, dy) {
  return commands.map(({ op, coords }) => ({
    op,
    coords: coords.map((v, i) => (i % 2 === 0 ? v + dx : v + dy)),
  }));
}

// Coordinates are rounded to 1/1000 px. Figma emits full float32 precision, which
// serializes to strings like `0.8954999446868896` — ~19 bytes per number for
// sub-micron detail no display can resolve. Rounding keeps the emitted `d` an
// order of magnitude smaller with no visible change.
function fmt(v) {
  const r = Math.round(v * 1000) / 1000;
  return Object.is(r, -0) ? '0' : String(r);
}

/** Serialize a command list to an SVG path `d` string. */
export function toPathData(commands) {
  return commands
    .map(({ op, coords }) => (coords.length ? op + coords.map(fmt).join(' ') : op))
    .join(' ');
}

/**
 * Resolve a node's geometry into parent-space SVG path data.
 *
 * Figma stores fill and stroke as two independent outlines, and one emitted node
 * carries one path — so a node that has both is emitted as its fill, and the
 * caller is told (`droppedStroke`). SvgPathWidget fills and then strokes the
 * same path (svg_path_widget.cpp), and for a CENTER-aligned stroke the fill
 * outline IS the stroke's centerline, so the caller recovers the stroke by
 * lowering it as a real stroke channel on this same path (scene.mjs does).
 *
 * A stroke-only vector resolves to its stroke outline, which is a *fillable*
 * shape: it must be painted with the stroke color as a FILL, never re-stroked,
 * or the outline would itself be outlined. `paint` names which color applies —
 * and that color may be a GRADIENT, which is how the knob rim highlights render.
 * EXCEPT when `centerline` is true: then the path is the shape's boundary
 * (vector-network fallback, or `forceFill` on a stroke-only node) and a stroke
 * must be STROKED at the declared weight, never filled.
 *
 * CAVEAT — Figma bakes the stroke band of an INSIDE/OUTSIDE-aligned stroke
 * UNCLIPPED: the blob holds the boundary outlined at ±weight (double width,
 * half of it on the wrong side), and render-time clipping against the fill
 * region — which this decoder does not do — is what trims it. Verified on a
 * real file: a `Polygon 5` declaring weight 2 INSIDE carries a 4px band. That
 * is why callers pass `forceFill` for non-center aligns and stroke the
 * boundary instead of filling the band.
 *
 * `mirror` ({x, y} booleans) flips the normalized path in place, about its own
 * box — the box and its placement are untouched. The caller needs this when an
 * ANCESTOR container carries a mirror the emitted tree drops: Figma composes
 * every ancestor's flip into what it draws, so a vector under a net-mirrored
 * chain must bake that flip into its own ink or it renders pointing the wrong
 * way (the Env chip's reverse arrow: the icon frame's flip cancels the Union's
 * own baked flip in Figma, and dropping the container half of that pair left
 * the arrow mirrored).
 *
 * `fillRule` is the geometry's declared winding rule, mapped onto the SVG
 * vocabulary ('nonzero' | 'evenodd'), or null when the source declares none.
 * It is NOT decorative: Figma's baked outlines do not promise direction-
 * corrected contours. A subtracted icon can arrive as N same-direction
 * subpaths under `windingRule: 'ODD'` (the reference design's "Sub" speaker
 * cabinet is exactly this — five same-direction circles-in-a-box), and
 * filling that nonzero paints a solid slab where the design shows a hollow
 * woofer. `mixedWinding` reports geometry segments that disagree about the
 * rule (one path can carry only one), and `subpathCount` lets the caller
 * judge whether a missing rule can matter at all — on a single contour the
 * two rules fill identically.
 *
 * @returns {{ d: string, box: object, paint: 'fill'|'stroke', droppedStroke: boolean,
 *             centerline: boolean,
 *             fillRule: 'nonzero'|'evenodd'|null, mixedWinding: boolean,
 *             subpathCount: number }|null}
 */
export function geometryToPath(node, blobs, forceFill = false, mirror = null) {
  const r = placedGeometry(node, blobs, /* preferFill */ forceFill);
  if (!r) return null;
  const { placed, box, useFill, hasStroke, fillRule, mixedWinding, centerline } = r;

  // Normalize to a (0,0)-origin viewBox: codegen's setSvgViewBox consumes only
  // the width/height pair and ignores minX/minY, so a path carrying negative
  // coordinates would otherwise be silently shifted. The caller places the
  // shape by moving the node to box.minX/minY instead.
  let normalized = translate(placed, -box.minX, -box.minY);
  if (mirror && (mirror.x || mirror.y)) {
    normalized = applyTransform(normalized, {
      m00: mirror.x ? -1 : 1, m01: 0, m02: mirror.x ? box.width : 0,
      m10: 0, m11: mirror.y ? -1 : 1, m12: mirror.y ? box.height : 0,
    });
  }
  return {
    d: toPathData(normalized),
    box,
    paint: useFill ? 'fill' : 'stroke',
    droppedStroke: Boolean(useFill && hasStroke && !centerline),
    // The path is the shape's BOUNDARY (a vector-network centerline or a
    // forced fill outline), not a pre-expanded stroke band. A stroke on such
    // a path must be lowered as a real stroke channel — stroking, never
    // filling — because no band was ever baked for it.
    centerline: Boolean(centerline || (forceFill && !useFillIsReal(node))),
    fillRule,
    mixedWinding,
    subpathCount: placed.filter((c) => c.op === 'M').length,
  };
}

// Whether the node paints its own fill region — the visibility rule
// placedGeometry uses to pick the fill outline naturally.
function useFillIsReal(node) {
  return (node.fillPaints || []).some((p) => p.visible !== false);
}

/**
 * A mask node's clip outline in its PARENT's coordinate space: transform baked,
 * NOT normalized to a (0,0)-origin viewBox. The two callers place their result
 * differently, and that difference is the whole reason this is a second entry
 * point: an emitted vector is re-placed at its own box (so its `d` must start
 * at 0,0), while a CSS clip-path is consumed in the border-box space of the
 * view it clips — the mask's parent — so its coordinates must stay where the
 * design put them.
 *
 * Prefers the fill outline even when no fill paint is visible: what clips is
 * the mask's SHAPE, and paint visibility on a mask changes its alpha
 * semantics, not its outline.
 *
 * @returns {{ d: string, box: object }|null}
 */
export function geometryToClipPath(node, blobs) {
  const r = placedGeometry(node, blobs, /* preferFill */ true);
  if (!r) return null;
  return { d: toPathData(r.placed), box: r.box };
}

// Figma's winding enum → the SVG fill-rule vocabulary the envelope carries.
// Missing/unrecognized maps to null rather than a guessed default: on a
// multi-subpath shape the guess decides which regions are holes, and the
// caller wants to KNOW it is guessing (see the fill-rule diagnostic).
function fillRuleOf(entry) {
  if (entry && entry.windingRule === 'ODD') return 'evenodd';
  if (entry && entry.windingRule === 'NONZERO') return 'nonzero';
  return null;
}

// Decode a node's baked outline blobs and place them in the parent's
// coordinate space. Shared by geometryToPath (emitted vectors) and
// geometryToClipPath (mask outlines); only the fill/stroke preference differs.
function placedGeometry(node, blobs, preferFill) {
  const pick = (list) => {
    if (!Array.isArray(list) || !list.length) return null;
    const cmds = [];
    const rules = [];
    for (const g of list) {
      if (typeof g.commandsBlob !== 'number') continue;
      const blob = blobs[g.commandsBlob];
      if (!blob || !blob.bytes) continue;
      cmds.push(...decodePathBlob(blob.bytes));
      rules.push(fillRuleOf(g));
    }
    return cmds.length ? { cmds, rules } : null;
  };

  const fill = pick(node.fillGeometry);
  const stroke = pick(node.strokeGeometry);
  const hasVisibleFill = (node.fillPaints || []).some((p) => p.visible !== false);
  // Prefer the fill outline, but only when the node actually paints one: Figma
  // still emits a fillGeometry for a stroke-only shape (an open path has a
  // notional fill region), and choosing it would render a filled blob where the
  // design shows a thin line. A mask opts out of the paint check (preferFill).
  let useFill = fill && (preferFill || hasVisibleFill);
  let chosen = useFill ? fill : stroke || fill;
  // No baked outline at all. Symbol children can arrive with EMPTY
  // fillGeometry/strokeGeometry on both the master and every instance's
  // derived data (the FX knobs' `Oval` rim is exactly this), leaving only the
  // authored vector network. Decoding that network yields the shape's
  // CENTERLINE path — the boundary itself, not a pre-expanded stroke band —
  // which the caller must stroke, never fill, when the node is stroke-only.
  let centerline = false;
  if (!chosen) {
    const net = networkGeometry(node);
    if (net) {
      chosen = net;
      centerline = true;
      useFill = hasVisibleFill;
    }
  }
  if (!chosen) return null;

  function networkGeometry(n) {
    // Never for a BOOLEAN_OPERATION: its authored network (when present)
    // predates the boolean result, and resolving it would both draw the
    // wrong shape and stop the walk from descending into the operand
    // children — the existing, more faithful fallback for an unresolved BO.
    if (n.type === 'BOOLEAN_OPERATION') return null;
    const vd = n.vectorData;
    if (!vd || typeof vd.vectorNetworkBlob !== 'number') return null;
    const blob = blobs[vd.vectorNetworkBlob];
    if (!blob || !blob.bytes) return null;
    let cmds;
    try {
      cmds = decodeVectorNetworkBlob(blob.bytes);
    } catch {
      return null;  // malformed / unknown layout — fall through to the box lane
    }
    if (!cmds) return null;
    // Network coordinates live in the authored `normalizedSize` space; the
    // node renders at `size`. Scale before the caller bakes node.transform.
    const norm = vd.normalizedSize;
    const size = n.size;
    if (norm && size && norm.x > 0 && norm.y > 0
        && typeof size.x === 'number' && typeof size.y === 'number'
        && (size.x !== norm.x || size.y !== norm.y)) {
      cmds = applyTransform(cmds, {
        m00: size.x / norm.x, m01: 0, m02: 0,
        m10: 0, m11: size.y / norm.y, m12: 0,
      });
    }
    return { cmds, rules: [] };
  }

  // One emitted path carries one fill-rule, but the geometry is a LIST of
  // per-region entries each declaring its own — the "Sub" cabinet arrives as
  // [NONZERO dot, ODD ring, ODD box-with-hole]. When they disagree, evenodd
  // wins: Figma direction-corrects the contours of its NONZERO regions
  // (holes wind opposite their outer), and direction-corrected nesting fills
  // identically under either rule — while an ODD region's same-direction
  // holes fill SOLID under nonzero. So evenodd is correct for both kinds and
  // nonzero only for the first. `mixedWinding` still states the merge: it is
  // an approximation the moment two entries' regions overlap.
  const declared = chosen.rules.filter((r) => r !== null);
  const fillRule = declared.length
    ? (declared.includes('evenodd') ? 'evenodd' : 'nonzero')
    : null;
  const mixedWinding = new Set(declared).size > 1;

  const placed = applyTransform(chosen.cmds, node.transform);
  const box = boundsOf(placed);
  if (!box || !(box.width > 0) || !(box.height > 0)) return null;
  return { placed, box, useFill, hasStroke: Boolean(stroke), fillRule, mixedWinding,
           centerline };
}

// ── Vector-network decoding ─────────────────────────────────────────────────
//
// `vectorData.vectorNetworkBlob` indexes a flat little-endian blob:
//
//     [u32 vertexCount][u32 segmentCount][u32 regionCount]
//     vertexCount  x [u32 styleID][f32 x][f32 y]
//     segmentCount x [u32 styleID][u32 startVertex][f32 tanX][f32 tanY]
//                    [u32 endVertex][f32 tanX][f32 tanY]
//
// Like the path-command arity table above, this layout is validated by exact
// byte consumption: 12 + 12v + 28s must equal the blob length or the decode
// refuses. The tangents are RELATIVE control-point offsets from their vertex
// (a circle's four quarter-arcs carry +-kappa*r, the classic cubic circle);
// a zero pair on both ends is a straight line.
//
// Scope is deliberately narrow: regionCount must be 0 (regions carry per-loop
// fill styles this decoder has never observed and cannot verify), and no
// vertex may join more than two segments — a branching network has no single
// drawing order, and chaining it greedily would draw a plausible-looking
// WRONG shape. Anything unexpected returns null and the caller falls back to
// the plain-box lane with its existing diagnostic.

const NET_HEADER_BYTES = 12;
const NET_VERTEX_BYTES = 12;   // [u32 styleID][f32 x][f32 y]
const NET_SEGMENT_BYTES = 28;  // [u32 styleID][u32 v][f32 tx][f32 ty][u32 v][f32 tx][f32 ty]

/**
 * Decode a vector-network blob into path commands in normalizedSize space,
 * or null when the blob does not match the verified layout. Throws only on
 * a structurally impossible buffer (caller treats throw and null alike).
 */
export function decodeVectorNetworkBlob(bytes) {
  const buf = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
  if (buf.length < NET_HEADER_BYTES) return null;
  const vertexCount = buf.readUInt32LE(0);
  const segmentCount = buf.readUInt32LE(4);
  const regionCount = buf.readUInt32LE(8);
  if (regionCount !== 0) return null;
  if (vertexCount === 0 || segmentCount === 0) return null;
  if (buf.length !== NET_HEADER_BYTES + vertexCount * NET_VERTEX_BYTES
      + segmentCount * NET_SEGMENT_BYTES) return null;

  const verts = [];
  for (let i = 0; i < vertexCount; i++) {
    const o = NET_HEADER_BYTES + i * NET_VERTEX_BYTES;
    verts.push({ x: buf.readFloatLE(o + 4), y: buf.readFloatLE(o + 8) });
  }
  const segs = [];
  const segBase = NET_HEADER_BYTES + vertexCount * NET_VERTEX_BYTES;
  for (let i = 0; i < segmentCount; i++) {
    const o = segBase + i * NET_SEGMENT_BYTES;
    const start = buf.readUInt32LE(o + 4);
    const end = buf.readUInt32LE(o + 16);
    if (start >= vertexCount || end >= vertexCount) return null;
    segs.push({
      start,
      st: { x: buf.readFloatLE(o + 8), y: buf.readFloatLE(o + 12) },
      end,
      et: { x: buf.readFloatLE(o + 20), y: buf.readFloatLE(o + 24) },
      used: false,
    });
  }

  // A vertex joining 3+ segments makes the drawing order ambiguous — refuse
  // rather than guess a shape (see the scope note above).
  const degree = new Array(vertexCount).fill(0);
  for (const s of segs) {
    degree[s.start]++;
    if (s.end !== s.start) degree[s.end]++;
  }
  if (degree.some((d) => d > 2)) return null;

  // Chain segments into runs. Open components start at a degree-1 endpoint;
  // otherwise an arbitrary middle segment can split one connected chain into
  // two subpaths, changing joins, caps, and dash phase. Closed cycles may
  // start anywhere. Each run extends by matching the current endpoint; a
  // segment may be traversed
  // reversed (swap vertices AND tangents). A run that returns to its first
  // vertex closes with Z.
  const out = [];
  let unused = segmentCount;
  const isZero = (t) => t.x === 0 && t.y === 0;
  while (unused > 0) {
    const first = segs.find((s) => !s.used
      && (degree[s.start] === 1 || degree[s.end] === 1))
      || segs.find((s) => !s.used);
    let current = degree[first.start] === 1 ? first.start
      : degree[first.end] === 1 ? first.end
      : first.start;
    const runStart = current;
    out.push({ op: 'M', coords: [verts[current].x, verts[current].y] });
    for (;;) {
      const seg = segs.find((s) => !s.used && (s.start === current || s.end === current));
      if (!seg) break;
      seg.used = true;
      unused--;
      const forward = seg.start === current;
      const from = forward ? seg.start : seg.end;
      const to = forward ? seg.end : seg.start;
      const t0 = forward ? seg.st : seg.et;
      const t1 = forward ? seg.et : seg.st;
      const a = verts[from];
      const b = verts[to];
      if (isZero(t0) && isZero(t1)) {
        out.push({ op: 'L', coords: [b.x, b.y] });
      } else {
        out.push({ op: 'C', coords: [a.x + t0.x, a.y + t0.y,
                                     b.x + t1.x, b.y + t1.y, b.x, b.y] });
      }
      current = to;
      if (current === runStart) {
        out.push({ op: 'Z', coords: [] });
        break;
      }
    }
  }
  return out;
}

/**
 * The outline of a text node's rendered glyphs, as one path in node space.
 *
 * This is what makes an icon font renderable with no font installed. Icon fonts
 * address their glyphs by LIGATURE: the designer types "lock" and Font Awesome
 * substitutes a padlock. Without the font the characters render literally, so a
 * toolbar of icons imports as the word salad "lockquestion" / "und redo" — not a
 * parser bug, and not fixable by shipping a font we have no license to. Figma
 * bakes each glyph's outline into `derivedTextData.glyphs[].commandsBlob`, in the
 * very format the vector lane already decodes, so the icons are already in the
 * file. Every icon-font text node in the reference design carries one.
 *
 * Glyph outlines are in EM units — the box is 1x1 — with the origin on the
 * baseline and y pointing UP, while node space is y-DOWN from the top-left. So
 * each glyph scales by its own fontSize and flips about its baseline, then
 * translates to its pen position. That is a plain affine, which applyTransform
 * already does exactly.
 */
export function glyphsToPath(node, blobs) {
  const glyphs = node.derivedTextData && node.derivedTextData.glyphs;
  if (!Array.isArray(glyphs) || !glyphs.length) return null;
  const cmds = [];
  for (const g of glyphs) {
    if (typeof g.commandsBlob !== 'number') continue;
    const blob = (blobs || [])[g.commandsBlob];
    if (!blob || !blob.bytes) continue;
    const size = typeof g.fontSize === 'number' ? g.fontSize : 1;
    cmds.push(...applyTransform(decodePathBlob(blob.bytes), {
      m00: size, m01: 0, m02: (g.position && g.position.x) || 0,
      m10: 0, m11: -size, m12: (g.position && g.position.y) || 0,
    }));
  }
  if (!cmds.length) return null;
  // Keep the text node's OWN box; do not shrink to the glyph's ink bounds, and
  // do not bake node.transform (the caller already places the node from it).
  //
  // This is the opposite of geometryToPath, and deliberately so. Vector geometry
  // is authored in a space whose only meaning is the transformed result, so its
  // ink bounds ARE the shape. A glyph is different: it is drawn INSIDE a text
  // box the designer sized and the layout centers, and the ink is smaller than
  // that box by the font's own side bearings. Normalizing to ink threw the box
  // away — an icon lost its padding, then got re-centered on its ink, and layout
  // parity caught it exactly: `fg-icon misplaced: dx=+6.5 dw=-12`. A toolbar
  // icon looked too big and sat off-center in its button.
  const w = (node.size && node.size.x) || 0;
  const h = (node.size && node.size.y) || 0;
  if (!(w > 0) || !(h > 0)) return null;
  return {
    d: toPathData(cmds),
    // The box the design gave this node, in node-local space. The caller keeps
    // whatever position styleFor derived from the transform.
    box: { minX: 0, minY: 0, width: w, height: h },
    paint: 'fill',
    droppedStroke: false,
  };
}

/** Fonts whose "characters" are ligature names, not readable text. */
export function isIconFont(family) {
  return typeof family === 'string' && /^Font Awesome|Material Icons|Material Symbols/i.test(family);
}
