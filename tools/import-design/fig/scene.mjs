// Figma scene reconstruction: raw kiwi `nodeChanges` → a navigable node tree,
// a page/frame outline, and a Pulp figma-plugin export envelope for one frame.
//
// The envelope shape produced here is the same one the in-editor plugin emits
// (`tools/figma-plugin/schema/figma-plugin-export-v1.json`), so a decoded frame
// flows through the existing `--from figma-plugin` importer unchanged.

import { geometryToPath, glyphsToPath, isIconFont } from './paths.mjs';

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
  // Shared styles ARE the design's tokens: named colour/text/effect definitions
  // that nodes point at instead of carrying a literal paint. A node referencing
  // one stores `styleIdForFill.assetRef.key`, which matches the style node's own
  // `key` exactly, so resolution is a plain lookup.
  //
  // These are load-bearing, not decorative. Figma caches the resolved colour on
  // the referencing node only SOMETIMES: of the instrument tabs in one design,
  // only FOLEY carried a literal fill, and FOLEY was the only tab that rendered
  // its true colour — kick/SNARE/TOM/CRASH/RIDE each carried a style ref alone
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
 * is THIS instance's resolved colour. Letting the style win unconditionally
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
  // embed. Keep the literal — a stale cached colour beats no colour at all.
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
// master SYMBOL's subtree, customised by two per-instance override lists:
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
// noise. Now that a ref IS the colour, dropping it silently reverts an instance
// to its master's default token: the instrument tabs each override the ref to
// their own colour and carry no literal, so skipping it painted kick/SNARE/TOM/
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
  // clone cannot tell the master's cached literal from its own. An override
  // that re-points a node at a style WITHOUT supplying a literal means the
  // style is the answer and the inherited literal is the master's stale colour;
  // an override that supplies a literal means the opposite.
  if (entry.styleIdForFill && !entry.fillPaints) clone.__fillFromStyle = true;
  if (entry.fillPaints) clone.__fillFromStyle = false;
  if (entry.styleIdForStrokeFill && !entry.strokePaints) clone.__strokeFromStyle = true;
  if (entry.strokePaints) clone.__strokeFromStyle = false;
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

function firstGradient(paints) {
  return (paints || []).find(
    (p) => typeof p.type === 'string' && p.type.startsWith('GRADIENT') && p.visible !== false,
  ) || null;
}

/**
 * A single representative colour for a paint list, for consumers that can only
 * express a solid fill.
 *
 * A gradient collapses to the mean of its stops. That is a real approximation,
 * not a fidelity claim — but it is the difference between a shape reading as
 * roughly the right colour and reading as a black hole, because SvgPathWidget
 * defaults `fill_color_` to opaque black with `has_fill_` on. Returns null when
 * no colour can be derived, which the caller must lower to an explicit
 * `fill: 'none'` rather than leaving the widget on its black default.
 */
function approximatePaintColor(paints) {
  const solid = (paints || []).find((p) => p.type === 'SOLID' && p.visible !== false);
  if (solid) {
    return colorToHex({ ...solid.color, a: (solid.color?.a ?? 1) * (solid.opacity ?? 1) });
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
  const shouldOutlineText = (family) => {
    if (textMode === 'never') return false;
    if (textMode === 'always') return true;
    return isIconFont(family);
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
      const x = node.transform.m02;
      const y = node.transform.m12;
      if (typeof x === 'number' && typeof y === 'number') {
        style.position = 'absolute';
        style.left = Math.round(x);
        style.top = Math.round(y);
      }
    }
    // On a TEXT node the solid fill is the glyph color, applied as `color` in the
    // text branch — not a background.
    //
    // A GRADIENT fill counts. Reading only the solid fill dropped every
    // gradient-filled box on the floor: Figma's `knob base` is an ELLIPSE with a
    // GRADIENT_LINEAR body, so it arrived with no fill at all and fell back to an
    // empty frame — the mystery square behind every knob. The same drop is why a
    // near-white gradient panel imported as a dark hole. `background_color` can
    // only carry one colour, so a gradient collapses to the mean of its stops:
    // an honest approximation (diagnosed below), and far closer than nothing.
    // Real multi-stop gradients need a paint-server lowering — tracked separately.
    if (node.type !== 'TEXT') {
      const hex = approximatePaintColor(node.fillPaints);
      if (hex) style.background_color = hex;
      if (!firstSolidFill(node) && firstGradient(node.fillPaints)) {
        pushDiag('gradient-approximated', node,
                 `${node.type} gradient fill flattened to its mean stop colour`);
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
    // colour the design actually specifies. Doing it at each call site instead
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
    geometryNodes.push({
      node_id: key,
      parent_id: parentId || null,
      name: node.name || '',
      type,
      x: round2(abs.m02),
      y: round2(abs.m12),
      width: node.size ? round2(node.size.x) : null,
      height: node.size ? round2(node.size.y) : null,
    });

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
        // the stroke's colour. Re-stroking it would outline the outline.
        const paints = resolved.paint === 'fill' ? node.fillPaints : node.strokePaints;
        const hex = approximatePaintColor(paints);
        // Always emit a fill, including the explicit 'none'. SvgPathWidget
        // defaults to opaque black, so staying silent about an unknown paint
        // renders a black silhouette — strictly worse than the plain box this
        // lane used to emit, and the one way this change could regress a file.
        out.fill = hex || 'none';
        if (!hex && firstGradient(paints)) {
          pushDiag('gradient-approximated', node, `${type} gradient has no stops; fill cleared`);
        } else if (hex && firstGradient(paints)) {
          pushDiag('gradient-approximated', node, `${type} gradient flattened to its mean stop colour`);
        }
        delete style.background_color;
        if (resolved.droppedStroke) {
          pushDiag('vector-simplified', node, `${type} stroke dropped: fill and stroke cannot both render on one path`);
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
          // The glyph paints in the text's own colour.
          out.fill = out.style.color || 'none';
          delete out.style.color;
          // The outline is already baked into parent space, so its own bounds
          // place it — and only when styleFor already chose absolute, or we
          // would yank an icon out of the flex pass meant to position it.
          if (out.style.position === 'absolute') {
            out.style.left = round2(glyphs.box.minX);
            out.style.top = round2(glyphs.box.minY);
          }
          out.style.width = round2(glyphs.box.width);
          out.style.height = round2(glyphs.box.height);
        } else {
          // Say so rather than leave a mystery: the literal name will render.
          pushDiag('icon-font-required', node,
            `${node.fontName.family} glyph outlines missing; "${characters}" renders as text`);
        }
      }
    }

    const gradient = (node.fillPaints || []).find(
      (p) => typeof p.type === 'string' && p.type.startsWith('GRADIENT') && p.visible !== false,
    );
    if (gradient) pushDiag('gradient-approximated', node, gradient.type);

    if ((node.strokePaints || []).length && typeof node.strokeWeight === 'number' && node.strokeWeight > 0) {
      const s = firstSolidStroke(node);
      if (s) out.style.border = `${Math.round(node.strokeWeight)}px solid ${colorToHex(s.color)}`;
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
  return { envelope, geometry, assetHashes, diagnostics };
}

function firstSolidStroke(node) {
  return (node.strokePaints || []).find((p) => p.type === 'SOLID' && p.visible !== false) || null;
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

export const DIAGNOSTIC_SEVERITY = {
  'vector-simplified': 'warning',
  'gradient-approximated': 'warning',
  'asset-missing': 'warning',
  'external-component': 'warning',
  'unresolved-token': 'warning',
};
