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
 * caller is told (`droppedStroke`) rather than left to guess.
 *
 * Note what the limit is NOT: SvgPathWidget fills and then strokes the same path
 * (svg_path_widget.cpp:728 / :762), so this is not a widget that "renders only a
 * fill". Nor would setting a stroke on the fill path recover the drop —
 * `strokeGeometry` is the stroke ALREADY EXPANDED into its own fillable region,
 * a different shape from the fill's path, and it may carry its own gradient.
 * Expressing both means emitting that outline as a SIBLING vector. Worth knowing
 * before reaching for it: in the reference design exactly one node has both a
 * stroke outline and a visible fill, because `hasVisibleFill` below already
 * hands every stroke-only shape its stroke.
 *
 * A stroke-only vector resolves to its stroke outline, which is a *fillable*
 * shape: it must be painted with the stroke color as a FILL, never re-stroked,
 * or the outline would itself be outlined. `paint` names which color applies —
 * and that color may be a GRADIENT, which is how the knob rim highlights render.
 *
 * @returns {{ d: string, box: object, paint: 'fill'|'stroke', droppedStroke: boolean }|null}
 */
export function geometryToPath(node, blobs) {
  const pick = (list) => {
    if (!Array.isArray(list) || !list.length) return null;
    const cmds = [];
    for (const g of list) {
      if (typeof g.commandsBlob !== 'number') continue;
      const blob = blobs[g.commandsBlob];
      if (!blob || !blob.bytes) continue;
      cmds.push(...decodePathBlob(blob.bytes));
    }
    return cmds.length ? cmds : null;
  };

  const fill = pick(node.fillGeometry);
  const stroke = pick(node.strokeGeometry);
  const hasVisibleFill = (node.fillPaints || []).some((p) => p.visible !== false);
  // Prefer the fill outline, but only when the node actually paints one: Figma
  // still emits a fillGeometry for a stroke-only shape (an open path has a
  // notional fill region), and choosing it would render a filled blob where the
  // design shows a thin line.
  const useFill = fill && hasVisibleFill;
  const chosen = useFill ? fill : stroke || fill;
  if (!chosen) return null;

  const placed = applyTransform(chosen, node.transform);
  const box = boundsOf(placed);
  if (!box || !(box.width > 0) || !(box.height > 0)) return null;

  return {
    // Normalize to a (0,0)-origin viewBox: codegen's setSvgViewBox consumes only
    // the width/height pair and ignores minX/minY, so a path carrying negative
    // coordinates would otherwise be silently shifted. The caller places the
    // shape by moving the node to box.minX/minY instead.
    d: toPathData(translate(placed, -box.minX, -box.minY)),
    box,
    paint: useFill ? 'fill' : 'stroke',
    droppedStroke: Boolean(useFill && stroke),
  };
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
