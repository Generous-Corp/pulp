// Figma scene reconstruction: raw kiwi `nodeChanges` → a navigable node tree,
// a page/frame outline, and a Pulp figma-plugin export envelope for one frame.
//
// The envelope shape produced here is the same one the in-editor plugin emits
// (`tools/figma-plugin/schema/figma-plugin-export-v1.json`), so a decoded frame
// flows through the existing `--from figma-plugin` importer unchanged.

import { geometryToPath, glyphsToPath, isIconFont } from './paths.mjs';
import { isFontAvailable } from './fonts.mjs';

const FRAME_LIKE = new Set(['FRAME', 'COMPONENT', 'INSTANCE', 'COMPONENT_SET']);

// Node types whose shape lives in `fillGeometry`/`strokeGeometry` rather than in
// a box model. Rectangles and ellipses are deliberately NOT here: they already
// round-trip as styled boxes (background + border-radius), which stays crisp at
// any scale and remains themeable, so lowering them to baked paths would be a
// regression rather than a fix.
const VECTOR_LIKE = new Set(['VECTOR', 'STAR', 'REGULAR_POLYGON', 'BOOLEAN_OPERATION', 'LINE']);

function guidKey(g) {
  return g ? `${g.sessionID}:${g.localID}` : null;
}

/**
 * Build the node tree from a decoded message.
 * @returns {{ byGuid: Map, childrenOf: Map, pages: object[] }}
 */
export function buildScene(message) {
  const nodeChanges = message.nodeChanges || [];
  const byGuid = new Map();
  for (const node of nodeChanges) {
    const key = guidKey(node.guid);
    if (key) byGuid.set(key, node);
  }
  const childrenOf = new Map();
  for (const node of nodeChanges) {
    const parent = node.parentIndex && node.parentIndex.guid ? guidKey(node.parentIndex.guid) : null;
    if (!parent) continue;
    if (!childrenOf.has(parent)) childrenOf.set(parent, []);
    childrenOf.get(parent).push(node);
  }
  // Figma orders siblings by a fractional-index string in parentIndex.position.
  for (const list of childrenOf.values()) {
    list.sort((a, b) => {
      const pa = (a.parentIndex && a.parentIndex.position) || '';
      const pb = (b.parentIndex && b.parentIndex.position) || '';
      return pa < pb ? -1 : pa > pb ? 1 : 0;
    });
  }
  const pages = nodeChanges.filter((n) => n.type === 'CANVAS' && !n.internalOnly);
  // Shared styles ARE the design's tokens: named color/text/effect definitions
  // that nodes point at instead of carrying a literal paint. A node referencing
  // one stores `styleIdForFill.assetRef.key`, which matches the style node's own
  // `key` exactly, so resolution is a plain lookup.
  //
  // These are load-bearing, not decorative. Figma caches the resolved color on
  // the referencing node only SOMETIMES: of the instrument tabs in one design,
  // only FOLEY carried a literal fill, and FOLEY was the only tab that rendered
  // its true color — kick/SNARE/TOM/CRASH/RIDE each carried a style ref alone
  // and fell back to their master's fuchsia, so a row of red/yellow/green tabs
  // imported as an unbroken wall of pink.
  const stylesByKey = new Map();
  for (const node of nodeChanges) {
    if (node.styleType && node.key) stylesByKey.set(node.key, node);
  }
  return {
    byGuid, childrenOf, pages, stylesByKey,
    blobs: nodeChanges.length ? message.blobs || [] : [],
  };
}

/**
 * The paints a node actually renders with, resolving a style reference against
 * the file's style table. `which` is 'fill' or 'stroke'.
 *
 * Precedence follows PROVENANCE, not a blanket rule, and getting this backwards
 * is worse than not resolving at all. A literal sitting next to a ref is usually
 * Figma's cache of that style — but on an expanded instance the two can come
 * from different places: the ref may be the MASTER's default while the literal
 * is THIS instance's resolved color. Letting the style win unconditionally
 * repainted every instrument tab with the master's fuchsia, including the one
 * tab that had been correct.
 *
 * So the style wins only when it is the sole source: the node carries no literal
 * of its own, or an override re-pointed it at a style without supplying one
 * (`__fillFromStyle`). Otherwise the literal is the more specific answer.
 */
export function resolvedPaints(scene, node, which = 'fill') {
  const own = which === 'fill' ? node.fillPaints : node.strokePaints;
  const ref = which === 'fill' ? node.styleIdForFill : node.styleIdForStrokeFill;
  const key = ref && ref.assetRef && ref.assetRef.key;
  if (!key || !scene || !scene.stylesByKey) return own;
  const marker = which === 'fill' ? '__fillFromStyle' : '__strokeFromStyle';
  const preferStyle = node[marker] === true || !(own && own.length);
  if (!preferStyle) return own;
  const style = scene.stylesByKey.get(key);
  // An unresolvable ref means the style lives in a library this .fig does not
  // embed. Keep the literal — a stale cached color beats no color at all.
  if (!style) return own;
  const paints = which === 'fill' ? style.fillPaints : style.strokePaints;
  return (paints && paints.length) ? paints : own;
}

/**
 * Recursively count descendants of a node (for outline weight hints). The
 * `seen` set guards against a malformed graph whose parentIndex links form a
 * cycle, which would otherwise recurse without bound.
 */
function countDescendants(scene, node, seen) {
  const key = guidKey(node.guid);
  if (seen.has(key)) return 0;
  seen.add(key);
  const kids = scene.childrenOf.get(key) || [];
  let total = 0;
  for (const k of kids) total += 1 + countDescendants(scene, k, seen);
  return total;
}

/** Count top-level frames (across all pages) whose name matches, for ambiguity. */
export function countFramesByName(scene, name) {
  return framesByName(scene, name).length;
}

/**
 * Every top-level frame whose name matches (case-insensitive), each tagged with
 * its page name and guid. Optionally restricted to a single page. This is what
 * lets the caller list candidates when a name is ambiguous, and the basis for
 * page-scoped lookup.
 */
export function framesByName(scene, name, pageName) {
  if (!name) return [];
  const wanted = name.toLowerCase();
  const wantedPage = pageName ? pageName.toLowerCase() : null;
  const out = [];
  for (const page of scene.pages) {
    if (wantedPage !== null && (page.name || '').toLowerCase() !== wantedPage) continue;
    for (const f of scene.childrenOf.get(guidKey(page.guid)) || []) {
      if ((f.name || '').toLowerCase() === wanted) {
        out.push({ frame: f, page: page.name || '', guid: guidKey(f.guid) });
      }
    }
  }
  return out;
}

/**
 * Read-only inventory: pages → top-level frames with guid, size, and subtree
 * weight. This is what lets a caller pick a frame out of a large file before
 * committing to a full decode.
 */
export function outline(scene, meta) {
  return {
    title: (meta && (meta.file_name || meta.name)) || null,
    pageCount: scene.pages.length,
    pages: scene.pages.map((page) => {
      const frames = (scene.childrenOf.get(guidKey(page.guid)) || []).filter(
        (c) => FRAME_LIKE.has(c.type) || c.type === 'ROUNDED_RECTANGLE',
      );
      return {
        name: page.name,
        guid: guidKey(page.guid),
        frameCount: frames.length,
        frames: frames.map((f) => ({
          name: f.name,
          type: f.type,
          guid: guidKey(f.guid),
          width: f.size ? Math.round(f.size.x) : null,
          height: f.size ? Math.round(f.size.y) : null,
          descendants: countDescendants(scene, f, new Set()),
        })),
      };
    }),
  };
}

/**
 * Every node at any depth whose name matches (case-insensitive), each tagged
 * with its guid. This backs the ambiguity guard for the `findFrame` fallback,
 * which resolves a name to a nested node when no top-level frame matches — so a
 * name shared by several nested nodes is caught rather than silently resolved.
 */
export function nodesByName(scene, name) {
  if (!name) return [];
  const wanted = name.toLowerCase();
  const out = [];
  for (const node of scene.byGuid.values()) {
    if ((node.name || '').toLowerCase() === wanted) out.push({ node, guid: guidKey(node.guid) });
  }
  return out;
}

/**
 * Locate a frame by exact guid, else exact (case-insensitive) name. When
 * `pageName` is given, a name lookup is restricted to that page (a guid is
 * global — it is already unambiguous). Returns null if nothing matches.
 */
export function findFrame(scene, selector, pageName) {
  if (!selector) return null;
  if (scene.byGuid.has(selector)) return scene.byGuid.get(selector);
  const wanted = selector.toLowerCase();
  const wantedPage = pageName ? pageName.toLowerCase() : null;
  for (const page of scene.pages) {
    if (wantedPage !== null && (page.name || '').toLowerCase() !== wantedPage) continue;
    const frames = scene.childrenOf.get(guidKey(page.guid)) || [];
    for (const f of frames) {
      if ((f.name || '').toLowerCase() === wanted) return f;
    }
  }
  // Fall back to any node with a matching name (nested frames included). Only
  // when unscoped — a page restriction means "a top-level frame on this page".
  if (wantedPage === null) {
    for (const node of scene.byGuid.values()) {
      if ((node.name || '').toLowerCase() === wanted) return node;
    }
  }
  return null;
}

// ── style mapping ────────────────────────────────────────────────────────────

function channel(v) {
  return Math.max(0, Math.min(255, Math.round((v || 0) * 255)));
}

/**
 * Figma effects → a CSS `box-shadow` string.
 *
 * Shadows are what make a control look like an object rather than a decal. The
 * design gives its knobs two stacked drop shadows (y=16 blur=6 at 10% black,
 * y=4 blur=4) and an occasional inner shadow; dropping them rendered every knob
 * flat — the single most-noticed difference after color, and one no geometry
 * check can see, because the box is exactly the right size and in exactly the
 * right place. It just has no depth.
 *
 * The IR reads standard CSS syntax via parse_css_box_shadow (design_ir_json),
 * layers comma-separated and `inset` for an inner shadow, so the mapping is
 * direct: offset x/y, radius → blur, spread, color. Figma's blur/inner-glow
 * effects have no box-shadow equivalent and are skipped rather than
 * approximated into something the design never asked for.
 */
function effectsToBoxShadow(effects) {
  const layers = [];
  for (const e of effects || []) {
    if (e.visible === false) continue;
    if (e.type !== 'DROP_SHADOW' && e.type !== 'INNER_SHADOW') continue;
    const x = Math.round((e.offset && e.offset.x) || 0);
    const y = Math.round((e.offset && e.offset.y) || 0);
    const blur = Math.round(e.radius || 0);
    const spread = Math.round(e.spread || 0);
    const col = colorToHex(e.color) || '#00000040';
    const inset = e.type === 'INNER_SHADOW' ? 'inset ' : '';
    layers.push(`${inset}${x}px ${y}px ${blur}px ${spread}px ${col}`);
  }
  return layers.length ? layers.join(', ') : null;
}

// Blend modes that mean "just composite it" — never emitted.
const BLEND_IS_DEFAULT = new Set(['NORMAL', 'PASS_THROUGH']);

// Figma blend mode → the CSS name `normalize_blend_mode` (design_ir_json.cpp:227)
// accepts. Every mode listed is a real CSS mix-blend-mode value.
//
// LINEAR_BURN and LINEAR_DODGE are absent, for DIFFERENT reasons — worth stating
// separately, because "CSS has no equivalent" is true of only one of them:
//
//   LINEAR_DODGE is additive, and our stack CAN express it: `plus-lighter` maps
//     to BlendMode::lighter / Skia kPlus (style_effects_api.cpp:240-245). We
//     simply have not wired or verified it — no file in hand uses it. That is a
//     choice, not a limit, and it should be recorded as one.
//   LINEAR_BURN must stay refused. Its natural spelling `plus-darker` ALSO maps
//     to the additive kPlus in Skia/Chromium (same comment), so emitting it
//     would LIGHTEN a layer the designer asked to darken. A blend that composites
//     backwards is worse than one that composites normally and says so.
const FIGMA_BLEND_CSS = new Set([
  'DARKEN', 'MULTIPLY', 'COLOR_BURN', 'LIGHTEN', 'SCREEN', 'COLOR_DODGE',
  'OVERLAY', 'SOFT_LIGHT', 'HARD_LIGHT', 'DIFFERENCE', 'EXCLUSION',
  'HUE', 'SATURATION', 'COLOR', 'LUMINOSITY',
]);

function blendModeToCss(mode) {
  if (!mode || BLEND_IS_DEFAULT.has(mode) || !FIGMA_BLEND_CSS.has(mode)) return null;
  return mode.toLowerCase().replace(/_/g, '-');
}

function colorToHex(color) {
  if (!color) return null;
  const r = channel(color.r);
  const g = channel(color.g);
  const b = channel(color.b);
  const a = color.a === undefined ? 1 : color.a;
  const hex = `#${[r, g, b].map((x) => x.toString(16).padStart(2, '0')).join('')}`;
  if (a >= 0.999) return hex;
  return hex + channel(a).toString(16).padStart(2, '0');
}

// Audio-widget recognition is intentionally NOT done here. The importer owns a
// single authoritative resolver (component-key + name based); emitting a second
// guess from the decoder would be a competing source of truth. The decoder's
// job is purely structural — geometry, style, text, and assets — so a node's
// name flows through untouched for the importer to classify.

// ── component-instance expansion ─────────────────────────────────────────────
//
// An INSTANCE node has no children in the scene graph. Its content is its
// master SYMBOL's subtree, customized by two per-instance override lists:
//
//   symbolData.symbolOverrides — designer-authored property overrides
//     (visible, fillPaints, textData.characters, fontSize, effects, …)
//   derivedSymbolData          — Figma-computed per-node layout for THIS
//     instance (size, transform, fillGeometry/strokeGeometry, derivedTextData)
//
// Both lists key each entry by `guidPath.guids`. A path element does NOT name
// the subtree node's own guid directly: when a symbol is duplicated or synced
// from a library, its children get fresh guids but keep an `overrideKey` field
// pointing at the guid the overrides were written against. So an element
// resolves to the subtree node whose guid OR overrideKey matches. A multi-
// element path scopes through nested instances: the first element names a
// nested INSTANCE inside the master, and the rest applies within THAT
// instance's own expansion — so those entries are forwarded down, with outer
// (user-set) entries applied after the nested instance's authored ones.

// Keys an entry must never copy onto a node: identity/topology, the override
// machinery itself, and style-id refs the decoder does not resolve.
const OVERRIDE_SKIP_KEYS = new Set([
  'guidPath', 'guid', 'parentIndex', 'type', 'phase',
  'symbolData', 'derivedSymbolData', 'derivedSymbolDataLayoutVersion',
  'overrideKey', 'overrideLevel', 'componentKey', 'componentPropAssignments',
  'variableConsumptionMap', 'parameterConsumptionMap', 'prototypeInteractions',
  'styleIdForText', 'fontVersion',
  'derivedTextData',
]);
// NOTE: styleIdForFill / styleIdForStrokeFill are deliberately NOT skipped.
// They were, back when nothing resolved styles and an unresolvable ref was just
// noise. Now that a ref IS the color, dropping it silently reverts an instance
// to its master's default token: the instrument tabs each override the ref to
// their own color and carry no literal, so skipping it painted kick/SNARE/TOM/
// CRASH/RIDE with the master's "instrument/02 Fuchsia 85%" and turned a
// red/yellow/green tab row into a wall of pink.

// Figma's stack alignment enums → the strings parse_align accepts
// (design_ir_json.cpp:402). BASELINE has no Yoga equivalent and degrades to
// flex-start; MIN is Figma's default and is emitted explicitly rather than left
// implicit, so a round-trip says what the design says.
const FIGMA_STACK_ALIGN = {
  MIN: 'flex-start',
  CENTER: 'center',
  MAX: 'flex-end',
  SPACE_BETWEEN: 'space-between',
  STRETCH: 'stretch',
  BASELINE: 'flex-start',
};

// Props an instance inherits from its master when it doesn't set them itself —
// the auto-layout contract its expanded children flow under, plus the visuals
// Figma stores only on the master.
const SYMBOL_INHERITED_KEYS = [
  'stackMode', 'stackSpacing', 'stackPadding',
  'stackHorizontalPadding', 'stackVerticalPadding',
  'stackPaddingRight', 'stackPaddingBottom',
  'stackPrimaryAlignItems', 'stackCounterAlignItems',
  'stackPrimarySizing', 'stackCounterSizing',
  'fillPaints', 'strokePaints', 'strokeWeight', 'strokeAlign',
  'cornerRadius', 'rectangleCornerRadiiIndependent',
  'rectangleTopLeftCornerRadius', 'rectangleTopRightCornerRadius',
  'rectangleBottomLeftCornerRadius', 'rectangleBottomRightCornerRadius',
];

// Figma's "no style" sentinel: an all-1s guid (0xFFFFFFFF:0xFFFFFFFF). An
// override carrying it is not naming a style — it is DETACHING one.
const NULL_GUID_PART = 0xFFFFFFFF;
function isDetachedStyleRef(ref) {
  const g = ref && ref.guid;
  return !!g && g.sessionID === NULL_GUID_PART && g.localID === NULL_GUID_PART;
}

// Whether a ref still names a resolvable style. This asks for the same key
// resolvedPaints extracts, so provenance and resolution cannot disagree about
// what "attached" means.
function hasAttachedStyleRef(ref) {
  return !!(ref && ref.assetRef && ref.assetRef.key);
}

/**
 * Decide, for ONE override entry, whether the node's paint comes from its style
 * ref or from the literal beside it. Provenance decides; a blanket precedence
 * in either direction is wrong, and this file has a counter-example to each.
 *
 * Three cases, and the third is the one that is easy to get backwards:
 *
 *  1. The entry re-points at a style (`assetRef`) → the style is the answer,
 *     even when a cached literal rides along. That cache is LOSSY: "button /
 *     icon off" is white at 20% paint opacity and the literal beside it is
 *     white at 100% — the transparency simply is not in it. Preferring it
 *     rendered every toolbar icon as hard opaque white over soft grey ones.
 *  2. The entry DETACHES the style (the null-guid sentinel) → the instance cut
 *     the style loose, so its literal is authoritative. This is what keeps
 *     `global / sub navigation`'s OFF tabs white instead of repainting them
 *     with the master's fuchsia.
 *  3. The entry sets a literal and says NOTHING about the style → the style
 *     stays ATTACHED, and an attached style is what Figma renders; the literal
 *     is only Figma's cache of it. Treating that literal as an instance
 *     recolour painted the FILTER switch's dot pure white where the design has
 *     #333537, and the Sync radio's inner dot at 20% opacity where the design
 *     has 65% — invisible enough to be reported as a MISSING dot rather than a
 *     mis-colored one. Verified against the file's own thumbnail: both dots
 *     match the style, not the literal.
 *
 * Case 3 is safe precisely because case 2 runs first for a genuinely detached
 * node: once the ref is gone, `hasAttachedStyleRef` is false and the literal
 * speaks. An outer instance re-setting a literal on an already-detached node
 * therefore keeps its literal.
 */
function applyPaintProvenance(clone, entry, which) {
  const refKey = which === 'fill' ? 'styleIdForFill' : 'styleIdForStrokeFill';
  const paintKey = which === 'fill' ? 'fillPaints' : 'strokePaints';
  const marker = which === 'fill' ? '__fillFromStyle' : '__strokeFromStyle';
  const ref = entry[refKey];
  if (ref) {
    clone[marker] = !isDetachedStyleRef(ref);
    return;
  }
  if (!entry[paintKey]) return;
  clone[marker] = hasAttachedStyleRef(clone[refKey]);
}

function applyOverrideEntry(clone, entry) {
  // A derived size that shrinks the node scales its stroke weight with it —
  // Figma renders a scaled-down nested instance with proportionally thinner
  // strokes, so keeping the master's weight would fatten every outline.
  const dsize = entry.size;
  if (dsize && clone.size && typeof clone.strokeWeight === 'number'
      && clone.size.x && clone.size.y) {
    const scale = Math.min(dsize.x / clone.size.x, dsize.y / clone.size.y);
    if (scale < 0.99) clone.strokeWeight = round2(clone.strokeWeight * scale);
  }
  // Record where the paint came from BEFORE copying, because afterwards the
  // clone cannot tell the master's cached literal from its own.
  applyPaintProvenance(clone, entry, 'fill');
  applyPaintProvenance(clone, entry, 'stroke');
  for (const [k, v] of Object.entries(entry)) {
    if (v === undefined || OVERRIDE_SKIP_KEYS.has(k)) continue;
    clone[k] = v;
  }
  // derivedTextData is layout output (glyph runs), not content; it only stands
  // in for textData when it actually carries the characters.
  //
  // It must also land on the clone AS derivedTextData, because the glyph runs
  // are what an icon font renders from — and they are per-instance. Assigning it
  // to textData alone left every instance holding its MASTER's glyphs, which was
  // invisible while icons rendered as text and became obvious the moment they
  // rendered as outlines: every icon in the toolbar drew the same smiley, the
  // master's placeholder, instead of undo/redo/wrench/info.
  const dtd = entry.derivedTextData;
  if (dtd) {
    clone.derivedTextData = dtd;
    if (typeof dtd.characters === 'string') clone.textData = dtd;
  }
}

function firstSolidFill(node) {
  const paints = node.fillPaints || [];
  return paints.find((p) => p.type === 'SOLID' && p.visible !== false) || null;
}

/**
 * Composite a stack of SOLID paints the way Figma paints them: array order,
 * index 0 at the bottom, each one source-over the result so far. A paint's own
 * `opacity` multiplies its color's alpha, as everywhere else.
 *
 * The order is not a guess. The file's slider thumb declares #4b4d51 then
 * white@0.55; bottom-to-top composites to #aeafb1 and top-to-bottom leaves the
 * opaque #4b4d51 covering everything. Figma's own thumbnail samples #aeafb1
 * there, so bottom-to-top is what Figma does.
 */
function compositeSolids(solids) {
  let r = 0, g = 0, b = 0, a = 0;   // accumulated, non-premultiplied
  for (const p of solids) {
    const sa = (p.color?.a ?? 1) * (p.opacity ?? 1);
    if (sa <= 0) continue;
    const na = sa + a * (1 - sa);
    if (na <= 0) continue;
    r = ((p.color?.r ?? 0) * sa + r * a * (1 - sa)) / na;
    g = ((p.color?.g ?? 0) * sa + g * a * (1 - sa)) / na;
    b = ((p.color?.b ?? 0) * sa + b * a * (1 - sa)) / na;
    a = na;
  }
  return { r, g, b, a };
}

function firstGradient(paints) {
  return (paints || []).find(
    (p) => typeof p.type === 'string' && p.type.startsWith('GRADIENT') && p.visible !== false,
  ) || null;
}


// Invert a Figma affine paint transform, or null when it is degenerate.
function invertPaintTransform(t) {
  if (!t) return null;
  const det = t.m00 * t.m11 - t.m01 * t.m10;
  if (!Number.isFinite(det) || Math.abs(det) < 1e-12) return null;
  const i00 = t.m11 / det;
  const i01 = -t.m01 / det;
  const i10 = -t.m10 / det;
  const i11 = t.m00 / det;
  return {
    m00: i00, m01: i01, m02: -(i00 * t.m02 + i01 * t.m12),
    m10: i10, m11: i11, m12: -(i10 * t.m02 + i11 * t.m12),
  };
}

function applyPaintTransform(t, x, y) {
  return { x: t.m00 * x + t.m01 * y + t.m02, y: t.m10 * x + t.m11 * y + t.m12 };
}

// Sample a stop list at parameter `t`, with Figma's clamp-at-the-ends
// behavior (the first/last stop's color extends past it).
function sampleStops(stops, t) {
  if (t <= stops[0].position) return stops[0].color;
  const last = stops[stops.length - 1];
  if (t >= last.position) return last.color;
  for (let i = 1; i < stops.length; i++) {
    const a = stops[i - 1];
    const b = stops[i];
    if (t <= b.position) {
      const span = b.position - a.position;
      const f = span <= 1e-9 ? 0 : (t - a.position) / span;
      return {
        r: a.color.r + (b.color.r - a.color.r) * f,
        g: a.color.g + (b.color.g - a.color.g) * f,
        b: a.color.b + (b.color.b - a.color.b) * f,
        a: (a.color.a ?? 1) + ((b.color.a ?? 1) - (a.color.a ?? 1)) * f,
      };
    }
  }
  return last.color;
}

/**
 * A Figma GRADIENT_LINEAR paint → a CSS `linear-gradient(...)` string that
 * SvgPathWidget / setBackgroundGradient can paint, or null when the paint
 * cannot be expressed and the caller must fall back to the flattened mean.
 *
 * WHICH KINDS ARE EXPRESSIBLE DEPENDS ON THE CONSUMER, and this comment used to
 * get that wrong. It said "Only LINEAR is expressible" while naming
 * setBackgroundGradient one line above as a consumer — and that consumer parses
 * radial AND conic end to end: `apply_css_background_gradient` matches
 * `radial-gradient(` (css_gradient.cpp:192) and `conic-gradient(` (:221), View
 * paints them corner-radius-aware (view.cpp:563-568), and all three canvases
 * implement the primitive. The claim was true only of the OTHER consumer
 * (`parse_svg_linear_gradient`, svg_path_widget.cpp:157, which matches the
 * literal `linear-gradient(` and nothing else).
 *
 * The cost of that sentence: the design's xy pad carries a 167x119 GRADIENT_RADIAL
 * vignette that flattened to a uniform 12%-black wash instead of darkening at the
 * edges — a visible loss, deferred on a capability we already had. Same error as
 * concluding setSvgFillGradient did not exist from a grep that could not reach
 * its subdirectory: the survey missed a consumer, not the code.
 *
 * So: LINEAR everywhere; RADIAL only where the box branch will paint it (a
 * `background_gradient` on a frame/rect). DIAMOND and ANGULAR keep flattening —
 * conic exists in the canvas but Figma's ANGULAR sweep origin needs verifying
 * against a real file before claiming it, and no file in hand has one.
 *
 * Two corrections the obvious implementation gets wrong, both verified
 * against Figma's own export of this file rather than reasoned about:
 *
 *  1. The paint `transform` maps the node's normalized box INTO gradient
 *     space, where the ramp runs (0,0)→(1,0). The axis in box space is the
 *     INVERSE image of those points. Using the matrix forward renders every
 *     gradient 180° flipped — the knob rim highlight lights from below.
 *  2. The axis must be scaled into PIXEL space before its angle is taken.
 *     A normalized-space angle is wrong on any non-square box.
 *
 * The widget derives its endpoints from the box's half-diagonal rather than
 * the CSS gradient-line length, and Figma's axis is free to start and end
 * outside the box (the rim highlight runs y=-0.26→0.67). Neither matches CSS
 * `<angle>` semantics, so the stops are RESAMPLED onto the widget's own axis:
 * the emitted 0% / 100% carry the color the source ramp actually has where
 * the widget's line enters and leaves. That keeps the paint faithful without
 * depending on the widget and CSS agreeing about extent.
 */
/**
 * A Figma GRADIENT_RADIAL paint → a CSS `radial-gradient(...)` string.
 *
 * ONLY for the box branch (`style.background_gradient`): that consumer parses
 * radial (css_gradient.cpp:192) and paints it corner-radius-aware. The SvgPath
 * fill branch does NOT — `parse_svg_linear_gradient` matches the literal
 * `linear-gradient(` — so callers on that path must keep flattening. Passing a
 * radial string to the wrong consumer would silently render nothing.
 *
 * Figma's radial paint transform maps the unit circle onto an ellipse in the
 * node's normalized box; the ramp runs from the center out to radius 1. The
 * consumer's radial is a CIRCLE with its radius a fraction of max(w,h), so an
 * elliptical or rotated paint is APPROXIMATED — which is why the caller keeps a
 * downgraded diagnostic instead of dropping it. Far closer than a flat wash: the
 * design's xy-pad vignette darkens at the edges either way, which a uniform mean
 * cannot do at all.
 */
function radialPaintToCss(paint, w, h) {
  if (!paint || paint.type !== 'GRADIENT_RADIAL') return null;
  const stops = (paint.stops || [])
    .filter((s) => s && s.color && typeof s.position === 'number')
    .slice()
    .sort((a, b) => a.position - b.position);
  if (stops.length < 2) return null;
  if (!(w > 0) || !(h > 0)) return null;

  const inv = invertPaintTransform(paint.transform);
  if (!inv) return null;
  // Center of the ramp, and the two radius vectors, in normalized box space.
  const c = applyPaintTransform(inv, 0.5, 0.5);
  const rx = applyPaintTransform(inv, 1.0, 0.5);
  const ry = applyPaintTransform(inv, 0.5, 1.0);
  // Radii in PIXELS — a normalized radius is wrong on any non-square box.
  const r1 = Math.hypot((rx.x - c.x) * w, (rx.y - c.y) * h);
  const r2 = Math.hypot((ry.x - c.x) * w, (ry.y - c.y) * h);
  if (!(r1 > 0) && !(r2 > 0)) return null;
  // The consumer takes ONE radius as a fraction of max(w,h). Average the two
  // ellipse radii: it is the closest single circle to the designer's ellipse.
  const radiusFrac = ((r1 + r2) / 2) / Math.max(w, h);
  if (!Number.isFinite(radiusFrac) || radiusFrac <= 0) return null;

  // Stop alpha and the paint's own opacity both fold into the emitted color,
  // exactly as the linear and solid paths do. This vignette is black at alpha
  // 0..0.24 with paint opacity 0.24 — drop either and it becomes an opaque
  // black disc instead of a shadow at the edges.
  const opacity = paint.opacity ?? 1;
  const parts = stops.map((s) => {
    const hex = colorToHex({ ...s.color, a: (s.color.a ?? 1) * opacity });
    return `${hex} ${round2(s.position * 100)}%`;
  });
  // `circle at X% Y%` — the shape keyword is what makes the parser read the
  // position segment at all (css_gradient.cpp:199).
  return `radial-gradient(circle at ${round2(c.x * 100)}% ${round2(c.y * 100)}%, ${parts.join(', ')})`;
}

function gradientPaintToCss(paint, w, h) {
  if (!paint || paint.type !== 'GRADIENT_LINEAR') return null;
  const stops = (paint.stops || [])
    .filter((s) => s && s.color && typeof s.position === 'number')
    .slice()
    .sort((a, b) => a.position - b.position);
  if (stops.length < 2) return null;
  if (!(w > 0) || !(h > 0)) return null;

  const inv = invertPaintTransform(paint.transform);
  if (!inv) return null;
  const a0 = applyPaintTransform(inv, 0, 0);
  const a1 = applyPaintTransform(inv, 1, 0);
  // Normalized box space → pixels, before any angle is taken.
  const p0 = { x: a0.x * w, y: a0.y * h };
  const p1 = { x: a1.x * w, y: a1.y * h };
  const dx = p1.x - p0.x;
  const dy = p1.y - p0.y;
  const len2 = dx * dx + dy * dy;
  if (!Number.isFinite(len2) || len2 < 1e-9) return null;

  // Screen y is down: dx=1,dy=0 → 90deg (`to right`); dx=0,dy=1 → 180deg
  // (`to bottom`).
  let deg = (Math.atan2(dy, dx) * 180) / Math.PI + 90;
  deg = ((deg % 360) + 360) % 360;

  // The widget's own gradient line, mirrored from parse_svg_linear_gradient.
  const rad = ((deg - 90) * Math.PI) / 180;
  const halfDiag = 0.5 * Math.sqrt(w * w + h * h);
  const ex = Math.cos(rad) * halfDiag;
  const ey = Math.sin(rad) * halfDiag;
  const s = { x: w / 2 - ex, y: h / 2 - ey };
  const e = { x: w / 2 + ex, y: h / 2 + ey };
  const sedx = e.x - s.x;
  const sedy = e.y - s.y;
  const seLen2 = sedx * sedx + sedy * sedy;
  if (seLen2 < 1e-9) return null;

  // Widget-axis parameter → source-ramp parameter, so each emitted stop
  // carries the color the source actually has at that point on the line.
  const sourceParamAt = (nt) => {
    const qx = s.x + sedx * nt - p0.x;
    const qy = s.y + sedy * nt - p0.y;
    return (qx * dx + qy * dy) / len2;
  };
  const widgetParamOf = (t) => {
    const qx = p0.x + dx * t - s.x;
    const qy = p0.y + dy * t - s.y;
    return (qx * sedx + qy * sedy) / seLen2;
  };

  const out = [{ pos: 0, color: sampleStops(stops, sourceParamAt(0)) }];
  for (const st of stops) {
    const nt = widgetParamOf(st.position);
    if (nt > 1e-4 && nt < 1 - 1e-4) out.push({ pos: nt, color: st.color });
  }
  out.push({ pos: 1, color: sampleStops(stops, sourceParamAt(1)) });
  out.sort((a, b) => a.pos - b.pos);

  const opacity = paint.opacity ?? 1;
  const parts = out.map((st) => {
    // Stop alpha and the paint's own opacity both fold into the emitted
    // color, exactly as the solid paths do — the rim highlight is white at
    // alpha 0.24, and dropping either makes it a hard white ring.
    const hex = colorToHex({ ...st.color, a: (st.color.a ?? 1) * opacity });
    return `${hex} ${round2(st.pos * 100)}%`;
  });
  return `linear-gradient(${round2(deg)}deg, ${parts.join(', ')})`;
}

/**
 * A single representative color for a paint list, for consumers that can only
 * express a solid fill.
 *
 * A gradient collapses to the mean of its stops. That is a real approximation,
 * not a fidelity claim — but it is the difference between a shape reading as
 * roughly the right color and reading as a black hole, because SvgPathWidget
 * defaults `fill_color_` to opaque black with `has_fill_` on. Returns null when
 * no color can be derived, which the caller must lower to an explicit
 * `fill: 'none'` rather than leaving the widget on its black default.
 */
function approximatePaintColor(paints) {
  // Figma's fillPaints is a STACK, painted in array order with index 0 at the
  // bottom, and taking `.find(SOLID)` off the top of it silently threw the rest
  // away. The slider thumb declares two: a #4b4d51 base with white at 55% over
  // it. Figma composites those to #aeafb1 — a light thumb — and we painted the
  // bare base, so it came out dark. That does not present as a dropped property;
  // it presents as a wrong COLOR, which sends you hunting through style refs and
  // override precedence for a bug that is neither.
  //
  // Verified against the file's own thumbnail: Figma's raster samples #aeafb1
  // there, exactly what compositing the two declared paints predicts.
  const solids = (paints || []).filter((p) => p.type === 'SOLID' && p.visible !== false);
  if (solids.length) {
    return colorToHex(compositeSolids(solids));
  }
  const grad = firstGradient(paints);
  if (grad && Array.isArray(grad.stops) && grad.stops.length) {
    const n = grad.stops.length;
    const sum = grad.stops.reduce(
      (acc, s) => ({
        r: acc.r + (s.color?.r ?? 0),
        g: acc.g + (s.color?.g ?? 0),
        b: acc.b + (s.color?.b ?? 0),
        a: acc.a + (s.color?.a ?? 1),
      }),
      { r: 0, g: 0, b: 0, a: 0 },
    );
    return colorToHex({
      r: sum.r / n,
      g: sum.g / n,
      b: sum.b / n,
      a: (sum.a / n) * (grad.opacity ?? 1),
    });
  }
  return null;
}

function firstImageFill(node) {
  const paints = node.fillPaints || [];
  return paints.find((p) => p.type === 'IMAGE' && p.visible !== false) || null;
}

function cornerRadius(node) {
  if (typeof node.cornerRadius === 'number') return Math.round(node.cornerRadius);
  const corners = [
    node.rectangleTopLeftCornerRadius,
    node.rectangleTopRightCornerRadius,
    node.rectangleBottomRightCornerRadius,
    node.rectangleBottomLeftCornerRadius,
  ].filter((v) => typeof v === 'number');
  if (corners.length && corners.every((v) => v === corners[0])) return Math.round(corners[0]);
  return null;
}

/**
 * Figma's four per-corner radii, when they are NOT all equal.
 *
 * `cornerRadius()` above collapses to one number and returns null the moment the
 * corners differ, which then raised `corner-radius-simplified` — an honest
 * diagnostic for a loss that did not have to happen. The whole chain below can
 * express this: IRStyle carries all four (`border_top_left_radius` …), the JSON
 * reads them, and the bridge takes `setCornerRadius(id, "TopLeft", r)`
 * (widget_bridge/border_box_api.cpp:54). Only codegen never emitted them, so a
 * card rounded on two corners imported rounded on four — or, when the uniform
 * value was absent, on none.
 *
 * Returns null when the corners ARE uniform, so the single-value path stays the
 * one that runs; there is no reason to emit four calls where one is exact.
 */
function perCornerRadii(node) {
  if (typeof node.cornerRadius === 'number') return null;
  const tl = node.rectangleTopLeftCornerRadius;
  const tr = node.rectangleTopRightCornerRadius;
  const br = node.rectangleBottomRightCornerRadius;
  const bl = node.rectangleBottomLeftCornerRadius;
  const all = [tl, tr, br, bl];
  if (!all.some((v) => typeof v === 'number')) return null;
  if (all.every((v) => typeof v === 'number') && all.every((v) => v === all[0])) return null;
  // A corner Figma omits is square, not inherited.
  const n = (v) => Math.round(typeof v === 'number' ? v : 0);
  return { tl: n(tl), tr: n(tr), br: n(br), bl: n(bl) };
}

const IDENTITY = { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 };

/** Compose two Figma affine transforms (parent ∘ local), as a 2×3 matrix. */
function composeTransform(p, l) {
  return {
    m00: p.m00 * l.m00 + p.m01 * l.m10,
    m01: p.m00 * l.m01 + p.m01 * l.m11,
    m02: p.m00 * l.m02 + p.m01 * l.m12 + p.m02,
    m10: p.m10 * l.m00 + p.m11 * l.m10,
    m11: p.m10 * l.m01 + p.m11 * l.m11,
    m12: p.m10 * l.m02 + p.m11 * l.m12 + p.m12,
  };
}

function localTransform(node) {
  const t = node.transform;
  if (!t || typeof t.m02 !== 'number' || typeof t.m12 !== 'number') return IDENTITY;
  return {
    m00: typeof t.m00 === 'number' ? t.m00 : 1,
    m01: typeof t.m01 === 'number' ? t.m01 : 0,
    m02: t.m02,
    m10: typeof t.m10 === 'number' ? t.m10 : 0,
    m11: typeof t.m11 === 'number' ? t.m11 : 1,
    m12: t.m12,
  };
}

/**
 * Materialize one frame subtree into the export envelope.
 *
 * `geometry` is a second, independent product of the same walk: Figma's OWN
 * solved rect for every emitted node, in frame-relative design px. It is not
 * derived from `style` — deliberately. `styleFor` DROPS an auto-layout child's
 * coordinates (they flow; emitting them would fight the flex pass), which is
 * correct for the importer and useless for validation. But the `.fig` carries
 * Figma's already-solved layout for those children anyway (a Transport frame's
 * children sit at m02 = 0, 38, 76, 166), so composing transforms down the tree
 * recovers where Figma actually put each node. Comparing that against where
 * Pulp's Yoga pass puts the same node id is a pixel-free parity check: no
 * renderer, no thresholds fighting anti-aliasing, and a failure names a node and
 * an exact delta instead of "the screenshot looks off".
 *
 * @returns {{ envelope: object, geometry: object, assetHashes: Set<string>, diagnostics: object[] }}
 */
export function materializeFrame(scene, frame, ctx) {
  const diagnostics = [];
  const assetHashes = new Set();
  const geometryNodes = [];
  const materialNodes = [];
  const seenTokens = new Map();
  // How much text to lower to glyph outlines. Figma bakes an outline for EVERY
  // text node, so this is a fidelity/liveness trade, not a capability one:
  //
  //   auto (default) — outline only icon fonts, whose "characters" are ligature
  //                    names and are never real text. Everything else stays a
  //                    live label: an imported knob caption has to remain
  //                    editable, themeable and reflowable to be worth anything.
  //   always         — outline every string. Pixel-faithful to what Figma drew,
  //                    including designs whose font we do not have, at the cost
  //                    of every label. The logo reads "TRI  Z" and relies on
  //                    Sofia Pro's metrics to reserve the gap its A-mark sits
  //                    in; with a fallback font the gap collapses and the Z
  //                    lands on top of the A. Outlines place it exactly.
  //   never          — no outlining at all, icon fonts included.
  const textMode = (ctx && ctx.textAsOutlines) || process.env.PULP_FIG_TEXT_AS_OUTLINES || 'auto';
  const fontAvailable = (ctx && ctx.isFontAvailable) || isFontAvailable;
  const outlinedFamilies = new Set();
  const shouldOutlineText = (family) => {
    if (textMode === 'never') return false;
    if (textMode === 'always') return true;
    // auto: an icon font is never real text; and text whose font we do not have
    // cannot be laid out correctly as live text, because the substitute face's
    // advance widths are not the ones the design was measured with. The logo
    // reads "TRI  Z" and those two spaces are the gap its A-mark occupies —
    // set in a fallback the gap collapses and the Z lands on top of the mark.
    // Outlines are exact and need no font.
    if (isIconFont(family)) return true;
    if (family && !fontAvailable(family)) {
      outlinedFamilies.add(family);
      return true;
    }
    return false;
  };
  // Every font family the frame references. Reported at the end so a
  // missing font is a stated result, not a mystery in the pixels.
  const fontsSeen = new Set();

  function pushDiag(code, node, detail) {
    diagnostics.push({
      code,
      node_id: guidKey(node.guid),
      node_name: node.name || null,
      detail: detail || null,
      severity: DIAGNOSTIC_SEVERITY[code] || 'info',
    });
  }

  // Swap a node's style-referenced paints for the style's real ones. Returns
  // the node untouched when it references nothing, so the common path allocates
  // nothing; otherwise a shallow copy keeps the shared master node — reused by
  // every instance of it — free of per-walk mutation.
  const styledNodes = new Set();
  function withResolvedPaints(node) {
    if (!node.styleIdForFill && !node.styleIdForStrokeFill) return node;
    const fill = resolvedPaints(scene, node, 'fill');
    const stroke = resolvedPaints(scene, node, 'stroke');
    if (fill === node.fillPaints && stroke === node.strokePaints) return node;
    styledNodes.add(guidKey(node.guid));
    return { ...node, fillPaints: fill, strokePaints: stroke };
  }

  // `parent` decides whether this node's own coordinates matter. A Figma frame
  // with `stackMode` is auto-layout: its children FLOW, so their transforms are
  // layout output, not input, and emitting them would fight the flex pass. A
  // plain frame positions every child absolutely — that is the common case for
  // hand-designed plugin UIs, and dropping those coordinates collapses the whole
  // design into the parent's content origin.
  function styleFor(node, parent) {
    const style = {};
    if (node.size) {
      style.width = Math.round(node.size.x);
      style.height = Math.round(node.size.y);
    }

    // Absolute placement from the node's affine transform. m02/m12 are the
    // translation column — frame-relative x/y, exactly what left/top want.
    // `stackPositioning === 'ABSOLUTE'` opts a child out of its parent's
    // auto-layout, so it is absolute even inside a flex parent.
    const parentIsAutoLayout =
      parent && (parent.stackMode === 'HORIZONTAL' || parent.stackMode === 'VERTICAL');
    const optedOut = node.stackPositioning === 'ABSOLUTE';
    if (parent && node.transform && (!parentIsAutoLayout || optedOut)) {
      const t = node.transform;
      const x = t.m02;
      const y = t.m12;
      if (typeof x === 'number' && typeof y === 'number') {
        style.position = 'absolute';
        // The affine transform's rotation column was dropped here, keeping only
        // the translation (m02/m12). A rotated layer — e.g. a knob's value
        // needle, stored as a thin rect rotated to the value angle — then
        // rendered as an axis-aligned bar at the rotated origin: a vertical stub
        // floating off-center instead of a radial pointer. Recover the rotation.
        const angle = Math.atan2(t.m10, t.m00);   // radians
        const deg = angle * 180 / Math.PI;
        // Only a NON-orthogonal rotation makes an axis-aligned box non-axis-
        // aligned and needs the transform (a knob's value needle at 43.4deg).
        // A multiple of 90deg keeps a rect axis-aligned, and for a solid fill a
        // 180deg spin is a visual no-op — applying it (plus the center-pivot
        // compensation below) only shifts the box off its intended row, which is
        // exactly what floated a slider's 180deg-rotated fill above its track.
        // Orthogonal rotations fall through to plain m02/m12 placement.
        const mod90 = Math.abs(deg) % 90;
        const nonOrthogonal = mod90 > 0.5 && mod90 < 89.5;
        // Scope to box-model nodes (frames / rounded-rects / ellipses). A
        // VECTOR_LIKE node is re-lowered to `path_data`, and Figma bakes the
        // layer rotation into that path — re-applying it here would double-rotate
        // the glyph. The reported bug is a knob's value needle, a ROUNDED_RECTANGLE.
        if (nonOrthogonal && !VECTOR_LIKE.has(node.type)) {
          // Figma rotates the layer around its LOCAL origin (0,0), landing that
          // origin at (m02, m12). The renderer applies `rotate()` around the
          // element's CENTER (default transform-origin). Compensate left/top so
          // the center-pivot rotation reproduces Figma's origin-pivot placement:
          // element-center = (m02,m12) + R(theta)*(w/2,h/2), and the renderer
          // pivots on (left+w/2, top+h/2), so solve for left/top.
          const w = typeof style.width === 'number' ? style.width : 0;
          const h = typeof style.height === 'number' ? style.height : 0;
          const c = Math.cos(angle), s = Math.sin(angle);
          const cx = c * (w / 2) - s * (h / 2);   // R(theta)*(w/2,h/2)
          const cy = s * (w / 2) + c * (h / 2);
          style.left = Math.round(x + cx - w / 2);
          style.top = Math.round(y + cy - h / 2);
          // CSS-compatible rotate() the native codegen lowers to setRotation();
          // the renderer's default center transform-origin matches the
          // compensation above.
          style.transform = `rotate(${deg.toFixed(2)}deg)`;
        } else {
          style.left = Math.round(x);
          style.top = Math.round(y);
        }
      }
    }
    // On a TEXT node the solid fill is the glyph color, applied as `color` in the
    // text branch — not a background.
    //
    // A GRADIENT fill counts. Reading only the solid fill dropped every
    // gradient-filled box on the floor: Figma's `knob base` is an ELLIPSE with a
    // GRADIENT_LINEAR body, so it arrived with no fill at all and fell back to an
    // empty frame — the mystery square behind every knob. The same drop is why a
    // near-white gradient panel imported as a dark hole.
    //
    // A LINEAR gradient lowers to a real `background_gradient`. Anything else
    // (radial / angular / diamond) has no lowering downstream, so it still
    // collapses to the mean of its stops and still says so: an honest
    // approximation, and far closer than nothing.
    if (node.type !== 'TEXT') {
      const grad = firstGradient(node.fillPaints);
      // The BOX branch, which paints via setBackgroundGradient — a consumer that
      // parses radial as well as linear (css_gradient.cpp:192). Radial was
      // deferred here on the claim that "only LINEAR is expressible", which was
      // true of the SvgPath fill branch and false of this one; the design's
      // 167x119 xy-pad vignette flattened to a uniform wash because of it.
      // A radial may ONLY go to a node that will actually take the box branch.
      // A VECTOR_LIKE node is re-lowered below to path_data + setSvgFill, whose
      // consumer parses `linear-gradient(` and nothing else — handing it a
      // radial paints NOTHING. A test caught exactly that leak here, which is
      // the whole point of asserting per-consumer rather than per-kind.
      const boxPainted = !VECTOR_LIKE.has(node.type);
      const css = firstSolidFill(node)
        ? null
        : (gradientPaintToCss(grad, style.width, style.height)
           || (boxPainted ? radialPaintToCss(grad, style.width, style.height) : null));
      if (css) {
        style.background_gradient = css;
        // A radial still loses something: the consumer's radial is a CIRCLE,
        // and Figma's paint may be an ellipse or rotated. Say so — a quieter
        // loss is still a loss, and this is the diagnostic that survives the fix.
        if (grad && grad.type === 'GRADIENT_RADIAL') {
          pushDiag('gradient-approximated', node,
                   `${node.type} GRADIENT_RADIAL painted as a circle; an elliptical or rotated ramp is approximated`);
        }
      } else {
        const hex = approximatePaintColor(node.fillPaints);
        if (hex) style.background_color = hex;
        if (!firstSolidFill(node) && grad) {
          pushDiag('gradient-approximated', node,
                   `${node.type} ${grad.type} flattened to its mean stop color`);
        }
      }
    }
    const image = firstImageFill(node);
    let assetRef = null;
    if (image && image.image && image.image.hash) {
      const hash = hashToHex(image.image.hash);
      if (hash && ctx.images.has(hash)) {
        assetHashes.add(hash);
        assetRef = hash;
      } else {
        pushDiag('asset-missing', node, `image hash ${hash || '?'} not in bundle`);
      }
    }
    const radius = cornerRadius(node);
    if (radius !== null) style.border_radius = radius;
    else {
      // Four corners that disagree used to collapse to nothing and raise
      // `corner-radius-simplified`, on the stated grounds that "IRStyle holds
      // ONE border_radius, so lowering this properly means widening the IR, its
      // JSON, and codegen".
      //
      // That was FALSE, and it is the third comment on this branch to assert
      // exactly the property its code lacks. IRStyle carries all four
      // (border_top_left_radius …, design_ir.hpp:95-98), parse_ir_style reads
      // all four (borderTopLeftRadius …), and the bridge takes
      // setCornerRadius(id, 'TopLeft', r). The only missing link was codegen —
      // four lines — so an asymmetric card imported with square corners while a
      // diagnostic explained that fixing it was a three-layer project.
      const corners = perCornerRadii(node);
      if (corners) {
        style.border_top_left_radius = corners.tl;
        style.border_top_right_radius = corners.tr;
        style.border_bottom_right_radius = corners.br;
        style.border_bottom_left_radius = corners.bl;
      }
    }
    // A node's own opacity — EXCEPT the frame we are importing. A top-level
    // frame's opacity composites it against the Figma canvas; it is not part of
    // the UI, and Figma itself ignores it when rendering/exporting that frame
    // (which is why the .fig's own thumbnail shows this design at full strength
    // while its root frame is set to 0.5). Applying it dimmed the ENTIRE import
    // by half — every panel, knob and label washed out — for a value the
    // designer set to see through the frame while working. `parent` is null only
    // for the frame being imported, so this is exactly the root.
    if (typeof node.opacity === 'number' && node.opacity < 1 && parent) {
      style.opacity = round2(node.opacity);
    }

    // Blend mode. A layer that COMPOSITES differently is not a cosmetic detail:
    // this design lays a 912x300 noise texture over its panels at opacity 0.10,
    // blendMode MULTIPLY. The texture's mean luminance is 229/255 — it is light.
    // Multiplied it DARKENS the panel (75 -> ~74); composited NORMAL, the same
    // light pixels LIGHTEN it (75 -> ~90). We read no blend mode at all, so every
    // panel imported ~+25/255 too bright, uniformly, over exactly the region the
    // texture covers. Nothing warned: a blend mode that is silently ignored still
    // paints something.
    //
    // The rest of the chain has been there all along — setMixBlendMode
    // (widget_bridge/style_effects_api.cpp), IRStyle::mix_blend_mode parsed from
    // `mixBlendMode` via normalize_blend_mode (design_ir_json.cpp:227,293), and
    // codegen emitting it for every node kind from emit_node_visual_overrides.
    // Only the decoder never read it.
    const blend = blendModeToCss(node.blendMode);
    if (blend) style.mix_blend_mode = blend;
    else if (node.blendMode && !BLEND_IS_DEFAULT.has(node.blendMode)) {
      // Figma has modes CSS does not (LINEAR_BURN / LINEAR_DODGE). Say so rather
      // than approximate: a wrong blend paints confidently wrong pixels.
      pushDiag('blend-unsupported', node, `${node.blendMode} is not lowered; composited normally`);
    }


    // Shadows. `box_shadow` is what parse_ir_style reads (design_ir_json.cpp:312
    // resolves boxShadow -> box_shadow), and it takes CSS syntax directly.
    const shadow = effectsToBoxShadow(node.effects);
    if (shadow) style.box_shadow = shadow;
    // Everything effectsToBoxShadow walked past. It `continue`s on any type that
    // is not a shadow, so a LAYER_BLUR or BACKGROUND_BLUR — a real, visible
    // instruction — left no shadow, no style, and no word about either. A blur
    // that renders sharp is a design decision the importer made on the user's
    // behalf; the least it can do is admit to it.
    for (const e of node.effects || []) {
      if (e.visible === false) continue;
      if (e.type === 'DROP_SHADOW' || e.type === 'INNER_SHADOW') continue;
      pushDiag('effect-unsupported', node,
        `${e.type} has no lowering in the render stack; the node composites without it`);
    }

    // Auto-layout → flex. This MUST land on a sibling `layout` object, not in
    // `style`: parse_ir_layout reads node["layout"] (design_ir_json.cpp:1042)
    // and parse_ir_style has no flex fields at all, so a `style.flex_direction`
    // matches nothing and is dropped without a word. Auto-layout children are
    // deliberately position-less (they are meant to flow), so discarding the
    // flex left them stacked on the parent's origin — that is why a Tone/EQ row
    // crammed six knobs into the far left with its labels overlapping.
    let layout;
    if (node.stackMode === 'HORIZONTAL' || node.stackMode === 'VERTICAL') {
      layout = {
        display: 'flex',
        direction: node.stackMode === 'HORIZONTAL' ? 'row' : 'column',
      };
      if (typeof node.stackSpacing === 'number') layout.gap = Math.round(node.stackSpacing);
      // Figma's padding model is asymmetric and oddly named: the *Vertical* /
      // *Horizontal* fields are the TOP and LEFT edges, with bottom and right
      // carried separately and only when they differ. Reading just the first
      // two and mirroring them renders every uneven inset wrong.
      const pad = {
        top: Math.round(node.stackVerticalPadding || 0),
        right: Math.round(node.stackPaddingRight ?? node.stackHorizontalPadding ?? 0),
        bottom: Math.round(node.stackPaddingBottom ?? node.stackVerticalPadding ?? 0),
        left: Math.round(node.stackHorizontalPadding || 0),
      };
      if (Object.values(pad).some((v) => v)) layout.padding = pad;
      // Alignment is half of what auto-layout means. Dropping it pins every row
      // to flex-start regardless of what the designer chose.
      const justify = FIGMA_STACK_ALIGN[node.stackPrimaryAlignItems];
      if (justify) layout.justify = justify;
      const align = FIGMA_STACK_ALIGN[node.stackCounterAlignItems];
      if (align) layout.align = align;
    }
    // align-self is a property of the CHILD, and only means anything inside an
    // auto-layout parent.
    if (parentIsAutoLayout && FIGMA_STACK_ALIGN[node.stackChildAlignSelf]) {
      layout = layout || {};
      layout.alignSelf = FIGMA_STACK_ALIGN[node.stackChildAlignSelf];
    }
    // Figma auto-layout has no flex-shrink: a child is FIXED, HUG, or FILL, and
    // none of those let it render narrower than the size Figma solved for it. It
    // overflows its parent instead — the file's own toolbar buttons declare
    // width 20 with 11+10 padding and Figma still draws the 12px icon, centered
    // on the resulting negative content box (x=4.5). Yoga defaults flex-shrink
    // to 1, so that same child collapsed to width 0 and its absolutely-placed
    // glyph then painted from the empty box's origin — the icon appeared shoved
    // right, which read as a positioning bug rather than a sizing one.
    //
    // We are replaying a SOLVED layout, not re-solving one, so this holds for
    // every flowed child: `style.width` above is the width Figma already
    // committed to, and any shrink can only move us away from it.
    if (parentIsAutoLayout && !optedOut) {
      layout = layout || {};
      layout.flexShrink = 0;
    }
    return { style, assetRef, layout };
  }

  function fontToken(node) {
    // Text nodes carry a fontName struct and fontSize. Both matter: dropping the
    // family did not just lose typography, it made a whole class of failure
    // unexplainable. Icon fonts render glyphs from LIGATURES — the designer
    // types "lock" and Font Awesome substitutes a padlock — so without the font
    // the text renders literally. That is why an import showed "lockquestion"
    // where the design has two icons: not a parser bug, a missing font. Carrying
    // the family lets the importer SAY so instead of leaving mystery text.
    const out = {};
    const fs = node.fontSize;
    if (typeof fs === 'number') out.font_size = Math.round(fs);
    const family = node.fontName && node.fontName.family;
    if (typeof family === 'string' && family) {
      out.font_family = family;
      const style = node.fontName.style;
      if (typeof style === 'string' && style && style !== 'Regular') out.font_style = style;
      fontsSeen.add(style && style !== 'Regular' ? `${family} ${style}` : family);
    }
    return out;
  }

  // Clone the master subtree for one instance, applying every override entry
  // whose (resolved) guidPath lands on a node, and forwarding deeper-scoped
  // entries into nested INSTANCE clones via `__overrides`. Clones carry a
  // synthetic `__key` (instance-path–prefixed) so two instances of the same
  // master never collide in the walk's cycle guard, and `__children` so the
  // walk descends the clone tree instead of the (empty) scene graph.
  function cloneSymbolChildren(origParentKey, prefix, entries) {
    const out = [];
    for (const orig of scene.childrenOf.get(origParentKey) || []) {
      const clone = { ...orig };
      clone.__key = `${prefix}/${guidKey(orig.guid)}`;
      const own = guidKey(orig.guid);
      const alias = guidKey(orig.overrideKey);
      const forwarded = [];
      for (const e of entries) {
        const guids = e.guidPath?.guids || [];
        if (!guids.length) continue;
        const head = guidKey(guids[0]);
        if (head !== own && head !== alias) continue;
        if (guids.length === 1) applyOverrideEntry(clone, e);
        else forwarded.push({ ...e, guidPath: { guids: guids.slice(1) } });
      }
      // Overrides first, visibility second: a master may hide a variant part
      // that the instance turns on (or vice versa), so only the post-override
      // state decides whether the node exists in this instance.
      if (clone.visible === false) continue;
      if (forwarded.length) clone.__overrides = [...(clone.__overrides || []), ...forwarded];
      clone.__children = cloneSymbolChildren(own, clone.__key, entries);
      out.push(clone);
    }
    return out;
  }

  // Symbols currently being expanded, so a symbol that (via nesting) contains
  // an instance of itself cannot recurse without bound.
  const expandStack = [];

  function expandInstance(inst) {
    const masterKey = guidKey(inst.symbolData?.symbolID);
    const master = masterKey ? scene.byGuid.get(masterKey) : null;
    if (!master) {
      if (masterKey) {
        pushDiag('external-component', inst,
          `master ${masterKey} not in file; instance kept as a plain box`);
      }
      return null;
    }
    if (expandStack.includes(masterKey)) return null;
    const instKey = inst.__key || guidKey(inst.guid);
    // Outer-scoped entries (from an enclosing instance) go LAST: a value the
    // user set on the outermost instance beats the nested component's own
    // authored default for the same node — that is the value Figma renders.
    const entries = [
      ...(inst.symbolData?.symbolOverrides || []),
      ...(inst.derivedSymbolData || []),
      ...(inst.__overrides || []),
    ];
    const merged = { ...inst, __key: instKey, __masterKey: masterKey };
    for (const k of SYMBOL_INHERITED_KEYS) {
      if (merged[k] === undefined && master[k] !== undefined) merged[k] = master[k];
    }
    merged.__children = cloneSymbolChildren(masterKey, instKey, entries);
    return merged;
  }

  const walked = new Set();
  function walk(node, parent, parentAbs, parentId) {
    // Hidden layers do not render in Figma; emitting them would paint hidden
    // variant parts and label slots over the visible design.
    if (node.visible === false) return null;
    if (node.type === 'INSTANCE') {
      const expanded = expandInstance(node);
      if (expanded) node = expanded;
    }
    // Resolve style tokens ONCE, here, so every paint reader below sees the
    // color the design actually specifies. Doing it at each call site instead
    // would mean a dozen chances to forget one.
    node = withResolvedPaints(node);
    const key = node.__key || guidKey(node.guid);
    // Guard against a malformed graph whose parentIndex links form a cycle; a
    // node reached twice would otherwise recurse without bound.
    if (walked.has(key)) return null;
    walked.add(key);
    const type = node.type;
    const { style, assetRef, layout } = styleFor(node, parent);
    const out = { type: envelopeType(type), name: node.name || '', style };
    if (layout) out.layout = layout;

    // The node's identity, carried through to the IR (design_ir_json's
    // parse_ir_identity_fields reads `node_id` into source_node_id, which the
    // `adapter` anchor strategy turns into the view's anchor). Without it every
    // node-keyed check downstream — fidelity_diff, layout parity — has nothing
    // to join on and silently skips. `key` (not the raw guid) is what makes this
    // unique: an expanded instance's children reuse their MASTER's guids, so two
    // instances of one component would otherwise share every child's id.
    out.node_id = key;

    // Figma's solved rect for this node, in frame-relative design px. The frame
    // under import anchors the space, so its own canvas transform is dropped and
    // it sits at the origin — the same space `style.left`/`top` are expressed in.
    const abs = parent ? composeTransform(parentAbs, localTransform(node)) : IDENTITY;
    // Held, not just pushed: a resolved vector's ink is not where its node box
    // says, and the vector branch below corrects this entry once it knows.
    const geomEntry = {
      node_id: key,
      parent_id: parentId || null,
      name: node.name || '',
      type,
      x: round2(abs.m02),
      y: round2(abs.m12),
      width: node.size ? round2(node.size.x) : null,
      height: node.size ? round2(node.size.y) : null,
    };
    geometryNodes.push(geomEntry);
    const declared = declaredMaterials(node);
    if (declared) materialNodes.push({ node_id: key, name: node.name || '', type, declared });

    if (assetRef) out.asset_ref = assetRef;

    // Expanded component content must never be name-guessed into a built-in
    // widget — a layer named "knob base" IS the designer's knob art, and
    // promoting it would paint Pulp's stock knob over the design. An explicit
    // audio_widget "none" opts the node out of the importer's name heuristic
    // while leaving the component-identity resolver in charge: the instance
    // carries its master's key below, so a matched library component still
    // becomes a real widget through that path.
    if (node.__masterKey || expandStack.length) out.audio_widget = 'none';
    if (node.__masterKey) {
      const master = scene.byGuid.get(node.__masterKey);
      const figma = {};
      if (typeof master.componentKey === 'string' && master.componentKey) {
        figma.component_key = master.componentKey;
      }
      if (master.name) figma.main_component_name = master.name;
      if (Object.keys(figma).length) out.figma = figma;
    }

    // Vector geometry → SVG path data. `path_data` + `viewBox` + fill/stroke is
    // the contract design_ir_json already reads (it lowers any vector-kind node
    // carrying path-data to a native SvgPathWidget), so resolving the shape here
    // is the whole fix — nothing downstream needs to change.
    let vectorResolved = false;
    if (VECTOR_LIKE.has(type)) {
      let resolved = null;
      let failure = null;
      try {
        resolved = geometryToPath(node, scene.blobs || []);
        if (!resolved) failure = 'no resolvable geometry';
      } catch (err) {
        // A blob we cannot parse is worth surfacing loudly: it means this file
        // uses a command encoding the decoder does not know, and every shape
        // sharing that blob will be missing rather than merely approximated.
        failure = `geometry unreadable: ${err.message}`;
      }
      if (failure) pushDiag('vector-simplified', node, `${type} ${failure}; emitted as a plain box`);
      if (resolved) {
        vectorResolved = true;
        out.type = 'vector';
        out.path_data = resolved.d;
        out.viewBox = `0 0 ${round2(resolved.box.width)} ${round2(resolved.box.height)}`;
        // A vector's INK is where its baked path is, not where its node box is,
        // so the sidecar's rect has to move with it. Figma's translation column
        // can sit far from the geometry it names — one `Bg PAnel` reports
        // m02 = 462 while its path starts at 350, filling a 112-wide parent
        // that ends at 462. Reading the box made the parity tool cry "112px
        // misplaced!" at a vector Pulp had placed exactly right, and those
        // phantoms were the three worst findings on a clean import. A checker
        // that cries wolf gets ignored, which is the failure mode this whole
        // tool exists to avoid — so use the same ground truth the renderer
        // does. The bounds are Figma's own decoded geometry either way; what
        // changes is only that we stop comparing two different quantities.
        const inkAbs = composeTransform(parentAbs || IDENTITY, {
          m00: 1, m01: 0, m02: resolved.box.minX,
          m10: 0, m11: 1, m12: resolved.box.minY,
        });
        geomEntry.x = round2(inkAbs.m02);
        geomEntry.y = round2(inkAbs.m12);
        geomEntry.width = round2(resolved.box.width);
        geomEntry.height = round2(resolved.box.height);
        // The path is already baked into parent space (transform included), so
        // an absolutely-placed vector is positioned by its own bounds, which
        // supersede the left/top styleFor derived from the transform: a mirrored
        // or rotated shape's bounds are not its translation column, and a stroke
        // outline overhangs the node box by half the stroke weight.
        //
        // Only when styleFor already chose absolute. A vector flowing inside an
        // auto-layout parent must keep flowing — pinning it here would yank it
        // out of the flex pass that is supposed to place it.
        if (style.position === 'absolute') {
          style.left = round2(resolved.box.minX);
          style.top = round2(resolved.box.minY);
        }
        style.width = round2(resolved.box.width);
        style.height = round2(resolved.box.height);
        // A stroke outline is a fillable region, so it is painted as a fill in
        // the stroke's color. Re-stroking it would outline the outline.
        const paints = resolved.paint === 'fill' ? node.fillPaints : node.strokePaints;
        const hex = approximatePaintColor(paints);
        // Always emit a fill, including the explicit 'none'. SvgPathWidget
        // defaults to opaque black, so staying silent about an unknown paint
        // renders a black silhouette — strictly worse than the plain box this
        // lane used to emit, and the one way this change could regress a file.
        out.fill = hex || 'none';
        const grad = firstGradient(paints);
        // The gradient rides the path alongside the flattened fill rather than
        // replacing it: SvgPathWidget prefers the gradient and only falls back
        // to fill_color_ when the string fails to parse, so the mean stays as
        // the safety net and neither ordering nor a stale solid can bite.
        //
        // The box here is the geometry's, which a stroke outline overhangs by
        // half the stroke weight, so the axis is off by that much against
        // Figma's node box. That is sub-pixel on these rims and far smaller
        // than the error it replaces.
        const css = gradientPaintToCss(grad, style.width, style.height);
        if (css) {
          out.fillGradient = css;
        } else if (!hex && grad) {
          pushDiag('gradient-approximated', node, `${type} ${grad.type} has no stops; fill cleared`);
        } else if (hex && grad) {
          pushDiag('gradient-approximated', node, `${type} ${grad.type} flattened to its mean stop color`);
        }
        delete style.background_color;
        if (resolved.droppedStroke) {
          // The reason is DECODER-side, not widget-side. SvgPathWidget fills and
          // then strokes the same path (svg_path_widget.cpp:728 / :762), so
          // "fill and stroke cannot both render on one path" — what this said
          // until now — is false, and it sent a reader looking for a widget
          // limit that does not exist. The real constraint: Figma's fill and
          // stroke are two DIFFERENT outlines (strokeGeometry is the stroke
          // already expanded into a fillable region, not the fill's path), and
          // one emitted node carries one `path_data`. Expressing both means
          // emitting the stroke outline as a SIBLING vector — which is exactly
          // what a stroke-only node already does — not setting a stroke on this
          // one. Fires for a single node in the reference design.
          pushDiag('vector-simplified', node,
            `${type} stroke dropped: fill and stroke are separate outlines and one node carries one path; the stroke outline would need a sibling vector`);
        }
      }
    }

    // PROTOTYPE: an INSTANCE has no children in the scene graph — its content
    // lives in the SYMBOL master, and the per-instance text is carried as a
    // `symbolOverrides[].textData.characters` entry keyed by guidPath. A knob
    // component's caption is exactly one such override, so lifting it onto
    // `label` feeds the existing audio_label path (design_ir_json.cpp:949 →
    // design_codegen.cpp:655 / text_for_node in design_import_native_common).
    // A component can nest a label slot inside another label slot (the fx knob
    // wraps a plain knob, and BOTH carry a caption). Figma keys each override by
    // guidPath, whose LENGTH is the slot's depth in the expanded symbol tree.
    // The shallow slot is the hidden one (`0:82 visible=false`); the deep slot is
    // the rendered caption. So the deepest override wins — taking the first would
    // caption 12 of 45 knobs "threshold" instead of Attack/Ratio/Release.
    if (type === 'INSTANCE') {
      let best = null;
      let bestDepth = -1;
      for (const ov of node.symbolData?.symbolOverrides || []) {
        const chars = ov.textData && ov.textData.characters;
        if (typeof chars !== 'string' || !chars.length) continue;
        const depth = (ov.guidPath?.guids || []).length;
        if (depth > bestDepth) {
          bestDepth = depth;
          best = chars;
        }
      }
      if (best !== null) out.label = best;
    }

    if (type === 'TEXT') {
      out.type = 'text';
      const characters = node.textData && typeof node.textData.characters === 'string'
        ? node.textData.characters
        : typeof node.characters === 'string'
          ? node.characters
          : '';
      // `content` is the canonical key: parse_ir_node reads `content`
      // (design_ir_json.cpp) and the IR writer emits `content`. Emitting
      // `text` here meant every string in every .fig import was silently
      // discarded — labels arrived empty and Yoga measured width=nan.
      out.content = characters;
      Object.assign(out.style, fontToken(node));
      if (node.textAlignHorizontal) out.style.text_align = node.textAlignHorizontal.toLowerCase();
      const solid = firstSolidFill(node);
      if (solid) out.style.color = colorToHex({ ...solid.color, a: (solid.color?.a ?? 1) * (solid.opacity ?? 1) });

      // An icon font's "characters" are LIGATURE NAMES, not text: the designer
      // types "lock" and Font Awesome substitutes a padlock. Emitting them as
      // text is always wrong — it rendered a toolbar of icons as the word salad
      // "lockquestion" and "und redo" — and no font we can ship fixes it, since
      // the font is licensed and absent. But Figma bakes each glyph's outline
      // into the file, so the icons are already here: lower them to the same
      // vector path a VECTOR node uses and they render with no font at all.
      if (shouldOutlineText(node.fontName && node.fontName.family)) {
        let glyphs = null;
        try {
          glyphs = glyphsToPath(node, scene.blobs || []);
        } catch (err) {
          pushDiag('vector-simplified', node, `glyph outline unreadable: ${err.message}`);
        }
        if (glyphs) {
          // Same contract the VECTOR branch emits below — path_data + viewBox +
          // fill is what design_ir_json already lowers to an SvgPathWidget.
          out.type = 'vector';
          out.path_data = glyphs.d;
          out.viewBox = `0 0 ${round2(glyphs.box.width)} ${round2(glyphs.box.height)}`;
          delete out.content;                 // a ligature name is not content
          delete out.style.font_family;
          delete out.style.font_size;
          delete out.style.font_style;
          delete out.style.text_align;
          // The glyph paints in the text's own color.
          out.fill = out.style.color || 'none';
          delete out.style.color;
          // Keep the box and position styleFor already derived from the node's
          // size and transform: the glyph is drawn inside the designer's text
          // box, and re-placing the node on the glyph's ink would strip the
          // font's side bearings — the icon's padding — and shift it off-center
          // in its button. Only the viewBox is asserted, so the path maps into
          // the box it was measured in.
        } else {
          // Say so rather than leave a mystery: the literal name will render.
          pushDiag('icon-font-required', node,
            `${node.fontName.family} glyph outlines missing; "${characters}" renders as text`);
        }
      }
    }

    // Only when nothing above managed to express it. This used to fire for
    // every gradient unconditionally, double-counting each node the fill
    // branch had already diagnosed — and now that a linear gradient really is
    // lowered, an unconditional warning would be a lie.
    const gradient = (node.fillPaints || []).find(
      (p) => typeof p.type === 'string' && p.type.startsWith('GRADIENT') && p.visible !== false,
    );
    if (gradient && !out.fillGradient && !out.style?.background_gradient) {
      pushDiag('gradient-approximated', node, gradient.type);
    }

    if ((node.strokePaints || []).length && typeof node.strokeWeight === 'number' && node.strokeWeight > 0) {
      const s = firstSolidStroke(node);
      // Figma multiplies a paint's own `opacity` by its color's alpha — the
      // same product the fill and text paths already take. Reading only
      // `color.a` here rendered a stroke the designer set to 20% at FULL
      // strength, which does not look like a dropped property; it looks like the
      // design just has a harder edge than it should, so nobody calls it a bug.
      if (s) {
        const hex = colorToHex({ ...s.color, a: (s.color?.a ?? 1) * (s.opacity ?? 1) });
        out.style.border = `${Math.round(node.strokeWeight)}px solid ${hex}`;
      }
    }

    // A resolved vector is terminal. Figma already flattened the operands into
    // the geometry we just emitted, so a BOOLEAN_OPERATION's children are the
    // pre-union inputs — emitting them too would draw the shape twice, once
    // unioned and once as its raw parts. Codegen's path branch is likewise
    // terminal, so not descending keeps the two lanes agreeing.
    let kids = [];
    if (!vectorResolved) {
      if (node.__masterKey) expandStack.push(node.__masterKey);
      kids = (node.__children || scene.childrenOf.get(key) || [])
        .map((c) => walk(c, node, abs, key))
        .filter(Boolean);
      if (node.__masterKey) expandStack.pop();
    }
    if (kids.length) out.children = kids;
    return out;
  }

  const root = walk(frame);
  // Report the fonts this frame needs. An importer cannot know what is
  // installed on the machine that will RENDER the result, so this states the
  // requirement rather than guessing at availability — and calls out icon fonts
  // specifically, because their failure mode is silent and confusing: the
  // ligature does not resolve, so "lock" renders as the word "lock" instead of
  // a padlock, and the import looks like it mangled the text.
  if (fontsSeen.size) {
    const fonts = [...fontsSeen].sort();
    const iconish = fonts.filter((f) => /awesome|icon|material symbols|material icons|glyph/i.test(f));
    pushDiag('fonts-required', { name: '<frame>' },
             `text uses ${fonts.length} font(s): ${fonts.join(', ')} — each must be installed where this UI renders, or text falls back`);
    for (const f of iconish) {
      pushDiag('icon-font-required', { name: '<frame>' },
               `"${f}" is an ICON font: glyphs come from LIGATURES, so without it every icon renders as its literal name (e.g. "lock" instead of a padlock). Install it, or the icons will read as words.`);
    }
  }

  const tokens = collectVariableTokens(scene, seenTokens, pushDiag);

  const source = ctx.fileKey ? `figma://${ctx.fileKey}/${guidKey(frame.guid)}` : null;
  const envelope = {
    $schema: 'https://pulp.dev/schemas/figma-plugin-export-v1.json',
    format_version: '2026.05-figma-plugin-v1',
    parser_version: ctx.parserVersion,
    compat_schema_version: ctx.compatSchemaVersion,
    provenance: {
      adapter: 'figma-plugin',
      version: ctx.parserVersion,
      source_uri: source || 'figma://local/0:0',
      exported_at: ctx.exportedAt,
    },
    tokens,
    asset_manifest: {
      version: 1,
      assets: [...assetHashes].map((hash) => assetEntry(hash, ctx.images.get(hash))),
    },
    diagnostics,
    root,
  };
  const geometry = {
    $schema: 'https://pulp.dev/schemas/fig-geometry-v1.json',
    source: source || 'figma://local/0:0',
    units: 'design-px',
    frame: {
      node_id: guidKey(frame.guid),
      name: frame.name || '',
      width: frame.size ? round2(frame.size.x) : null,
      height: frame.size ? round2(frame.size.y) : null,
    },
    nodes: geometryNodes,
  };
  // The material inventory rides alongside geometry for the same reason geometry
  // does: it is what the design DECLARES, kept separate from what we emitted, so
  // the two can be counted against each other by a tool that trusts neither.
  const materials = {
    $schema: 'https://pulp.dev/schemas/fig-materials-v1.json',
    source: geometry.source,
    frame: { node_id: guidKey(frame.guid), name: frame.name || '' },
    nodes: materialNodes,
  };
  return { envelope, geometry, materials, assetHashes, diagnostics };
}

function firstSolidStroke(node) {
  return (node.strokePaints || []).find((p) => p.type === 'SOLID' && p.visible !== false) || null;
}

/**
 * What this node's MATERIAL properties say, read straight off the resolved node
 * — deliberately without consulting styleFor, envelopeType, or any other
 * emitter. That independence is the whole point: every checker we own declares a
 * blind spot, and each one has been green through a real bug. `layout_parity`
 * compares boxes, so it cannot see the ink inside them; `thumb_parity` cannot
 * resolve a 2px arc; `fidelity_diff` was once blind to our own opt-out sentinel.
 * A COUNT has no equivalent excuse — 16 declared drop shadows against 1 emitted
 * is a number, and it sat in the file all evening with nothing to say it.
 *
 * So this function must never ask "what does the decoder look at?", only "what
 * does the design SAY?". A property the decoder forgot entirely still gets
 * declared here, and that is exactly the case the audit exists to catch.
 *
 * Returns null for a node that declares no material at all, so the sidecar stays
 * proportional to the design rather than to the node count.
 */
function declaredMaterials(node) {
  const d = {};

  // The fill STACK, not just its first paint. Figma composites the whole list;
  // a reader that takes one of them renders a color that is wrong rather than
  // missing, which is the hardest kind to attribute — the slider thumb read as a
  // precedence bug and then as a dropped node before anyone counted the paints.
  const fills = (node.fillPaints || []).filter((p) => p.visible !== false);
  if (fills.length) {
    d.fill = fills.map((p) => ({
      type: p.type || null,
      opacity: p.opacity ?? 1,
      color_alpha: p.color && typeof p.color.a === 'number' ? p.color.a : 1,
      // The paint's own RGB, so the audit can tell a composited result from the
      // bare bottom paint. Alpha alone cannot: the correct composite and the bug
      // are both opaque.
      rgb: p.color ? colorToHex({ ...p.color, a: 1 }).slice(0, 7) : null,
      // A PAINT-level blend mode. The node-level one is recorded below, and
      // recording only that left this triple-silent: the decoder does not read
      // it, no diagnostic mentions it, and the sidecar did not record it — so
      // the one tool whose entire thesis is "a declared property must survive or
      // be named" could not see this property at all. A hole in the audit is
      // worse than a hole in the decoder, because it is the thing that is
      // supposed to find the holes.
      blend_mode: typeof p.blendMode === 'string' && !BLEND_IS_DEFAULT.has(p.blendMode)
        ? p.blendMode : null,
    }));
  }

  // Visible stroke paints, with each paint's own opacity kept SEPARATE from its
  // color's alpha. Figma multiplies the two; a reader that takes only
  // `color.a` silently renders a 20%-opacity stroke at full strength.
  const strokes = (node.strokePaints || []).filter((p) => p.visible !== false);
  if (strokes.length && typeof node.strokeWeight === 'number' && node.strokeWeight > 0) {
    d.stroke = strokes.map((p) => ({
      type: p.type || null,
      opacity: p.opacity ?? 1,
      color_alpha: p.color && typeof p.color.a === 'number' ? p.color.a : 1,
    }));
  }

  // Every visible effect, by type — not just the two that lower to box-shadow.
  // Listing only the supported ones would make an unsupported effect invisible
  // to the count, which is the bug.
  const effects = (node.effects || []).filter((e) => e.visible !== false);
  if (effects.length) d.effects = effects.map((e) => e.type || 'UNKNOWN');

  // Corner radii, always as four corners. `cornerRadius()` collapses to a single
  // number and returns null when the four disagree, so a design's asymmetric
  // corners vanish; recording all four means the audit sees what was thrown away.
  const uniform = typeof node.cornerRadius === 'number' ? node.cornerRadius : null;
  const corners = [
    node.rectangleTopLeftCornerRadius,
    node.rectangleTopRightCornerRadius,
    node.rectangleBottomRightCornerRadius,
    node.rectangleBottomLeftCornerRadius,
  ];
  if (corners.some((v) => typeof v === 'number')) {
    d.corner_radius = corners.map((v) => (typeof v === 'number' ? v : uniform ?? 0));
  } else if (uniform) {
    d.corner_radius = [uniform, uniform, uniform, uniform];
  }

  // A non-default blend mode is a compositing instruction, not decoration: the
  // file's single MULTIPLY noise layer composited NORMAL lightened every panel.
  if (typeof node.blendMode === 'string' && !BLEND_IS_DEFAULT.has(node.blendMode)) {
    d.blend_mode = node.blendMode;
  }

  return Object.keys(d).length ? d : null;
}

function envelopeType(figmaType) {
  if (figmaType === 'TEXT') return 'text';
  if (figmaType === 'CANVAS') return 'frame';
  // An ELLIPSE is a circle, not a box. Collapsing it to `frame` made every
  // round thing in a design square: a knob's `knob base` became the mystery
  // square behind the knob, a toggle's handle became a square nub in its pill,
  // and a slider's thumb became a square block. The IR already has `ellipse`
  // (is_synthesizable_primitive), and synthesize_primitive_paths gives it a real
  // path — the decoder just never said what the node was.
  if (figmaType === 'ELLIPSE') return 'ellipse';
  return 'frame';
}

function round2(v) {
  return Math.round(v * 100) / 100;
}

function hashToHex(hash) {
  // Figma image hashes arrive as a 20-byte array or an already-hex string.
  // The result becomes a filename, so reject anything that isn't plain hex to
  // keep a crafted file from steering a write outside the assets directory.
  let hex = null;
  if (typeof hash === 'string') hex = hash;
  else if (Array.isArray(hash)) hex = hash.map((b) => (b & 0xff).toString(16).padStart(2, '0')).join('');
  else if (hash && hash.length !== undefined) {
    hex = Array.from(hash, (b) => (b & 0xff).toString(16).padStart(2, '0')).join('');
  }
  return hex && /^[0-9a-f]+$/i.test(hex) ? hex : null;
}

function assetEntry(hash, bytes) {
  const mime = sniffMime(bytes);
  return {
    asset_id: hash,
    local_path: `assets/${hash}${mimeExt(mime)}`,
    mime,
    hash,
  };
}

function sniffMime(bytes) {
  if (!bytes || bytes.length < 4) return 'application/octet-stream';
  if (bytes[0] === 0x89 && bytes[1] === 0x50) return 'image/png';
  if (bytes[0] === 0xff && bytes[1] === 0xd8) return 'image/jpeg';
  if (bytes[0] === 0x47 && bytes[1] === 0x49) return 'image/gif';
  if (bytes.length > 12 && bytes.toString('latin1', 8, 12) === 'WEBP') return 'image/webp';
  return 'application/octet-stream';
}

function mimeExt(mime) {
  return { 'image/png': '.png', 'image/jpeg': '.jpg', 'image/gif': '.gif', 'image/webp': '.webp' }[mime] || '';
}

// Figma variables → token maps (colors/dimensions/strings). Aliases are resolved
// one hop; deeper alias chains are approximated and flagged.
function collectVariableTokens(scene, seen, pushDiag) {
  const colors = {};
  const dimensions = {};
  const strings = {};
  for (const node of scene.byGuid.values()) {
    if (node.type !== 'VARIABLE' || !node.name) continue;
    const data = node.variableData || node.value;
    if (!data) continue;
    // The concrete shape varies by schema version; store what we can classify.
    if (data.colorValue) {
      const hex = colorToHex(data.colorValue);
      if (hex) colors[node.name] = hex;
    } else if (typeof data.floatValue === 'number') {
      dimensions[node.name] = data.floatValue;
    } else if (typeof data.textValue === 'string') {
      strings[node.name] = data.textValue;
    }
  }
  return { colors, dimensions, strings };
}

// EVERY code pushDiag can emit must appear here. An unregistered code falls
// through `DIAGNOSTIC_SEVERITY[code] || 'info'` to 'info', and BOTH consumers
// drop info: fig_decode's "N warning(s)" count excludes it, and
// print_import_diagnostics skips it outright. So a diagnostic written precisely
// to make a loss visible becomes invisible — the failure it was reporting AND
// the report both vanish, silently, because a table entry was missing.
//
// This bit four codes at once (corner-radius-simplified, effect-unsupported,
// fonts-required, icon-font-required). The sharpest was icon-font-required: an
// icon font missing AND its glyph outlines unreadable renders the ligature name
// as literal text ("lockquestion"), and the one diagnostic that explains why was
// downgraded to info and shown nowhere.
//
// `assertDiagnosticCodesRegistered` below is the guard, because a convention
// that lives only in a comment is how this got to four.
export const DIAGNOSTIC_SEVERITY = {
  'vector-simplified': 'warning',
  'gradient-approximated': 'warning',
  'blend-unsupported': 'warning',
  'asset-missing': 'warning',
  'external-component': 'warning',
  'effect-unsupported': 'warning',
  'fonts-required': 'warning',
  'icon-font-required': 'warning',
};

/**
 * Every code this module can emit, read out of the source itself.
 *
 * A test asserts this equals DIAGNOSTIC_SEVERITY's keys, in both directions:
 * an emitted-but-unregistered code silently downgrades to info (four did), and a
 * registered-but-never-emitted code is a promise nobody keeps ('unresolved-token'
 * was one — declared here while `collectVariableTokens` never raised it, so a
 * design with an unresolvable token said nothing and the table implied it would).
 */
export function emittedDiagnosticCodes(source) {
  return [...new Set([...source.matchAll(/pushDiag\('([a-z-]+)'/g)].map((m) => m[1]))].sort();
}
