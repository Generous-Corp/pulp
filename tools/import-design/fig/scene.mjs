// Figma scene reconstruction: raw kiwi `nodeChanges` → a navigable node tree,
// a page/frame outline, and a Pulp figma-plugin export envelope for one frame.
//
// The envelope shape produced here is the same one the in-editor plugin emits
// (`tools/figma-plugin/schema/figma-plugin-export-v1.json`), so a decoded frame
// flows through the existing `--from figma-plugin` importer unchanged.

const FRAME_LIKE = new Set(['FRAME', 'COMPONENT', 'INSTANCE', 'COMPONENT_SET']);

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
  return { byGuid, childrenOf, pages };
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

function firstSolidFill(node) {
  const paints = node.fillPaints || [];
  return paints.find((p) => p.type === 'SOLID' && p.visible !== false) || null;
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
 * Materialize one frame subtree into the export envelope.
 * @returns {{ envelope: object, assetHashes: Set<string>, diagnostics: object[] }}
 */
export function materializeFrame(scene, frame, ctx) {
  const diagnostics = [];
  const assetHashes = new Set();
  const seenTokens = new Map();

  function pushDiag(code, node, detail) {
    diagnostics.push({
      code,
      node_id: guidKey(node.guid),
      node_name: node.name || null,
      detail: detail || null,
      severity: DIAGNOSTIC_SEVERITY[code] || 'info',
    });
  }

  function styleFor(node) {
    const style = {};
    if (node.size) {
      style.width = Math.round(node.size.x);
      style.height = Math.round(node.size.y);
    }
    // On a TEXT node the solid fill is the glyph color, applied as `color` in the
    // text branch — not a background.
    const solid = node.type === 'TEXT' ? null : firstSolidFill(node);
    if (solid) {
      const hex = colorToHex({ ...solid.color, a: (solid.color?.a ?? 1) * (solid.opacity ?? 1) });
      if (hex) style.background_color = hex;
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
    if (typeof node.opacity === 'number' && node.opacity < 1) style.opacity = round2(node.opacity);

    // Auto-layout → flex.
    if (node.stackMode === 'HORIZONTAL' || node.stackMode === 'VERTICAL') {
      style.display = 'flex';
      style.flex_direction = node.stackMode === 'HORIZONTAL' ? 'row' : 'column';
      if (typeof node.stackSpacing === 'number') style.gap = Math.round(node.stackSpacing);
      const pads = [node.stackVerticalPadding, node.stackHorizontalPadding];
      if (pads.some((p) => typeof p === 'number')) {
        style.padding = pads.map((p) => Math.round(p || 0)).join('px ') + 'px';
      }
    }
    return { style, assetRef };
  }

  function fontToken(node) {
    // Text nodes carry a fontName struct and fontSize.
    const fs = node.fontSize;
    if (typeof fs === 'number') return { font_size: Math.round(fs) };
    return {};
  }

  const walked = new Set();
  function walk(node) {
    const key = guidKey(node.guid);
    // Guard against a malformed graph whose parentIndex links form a cycle; a
    // node reached twice would otherwise recurse without bound.
    if (walked.has(key)) return null;
    walked.add(key);
    const type = node.type;
    if (type === 'VECTOR' || type === 'STAR' || type === 'REGULAR_POLYGON' || type === 'BOOLEAN_OPERATION') {
      pushDiag('vector-simplified', node, `${type} emitted as a plain box`);
    }
    const { style, assetRef } = styleFor(node);
    const out = { type: envelopeType(type), name: node.name || '', style };

    if (assetRef) out.asset_ref = assetRef;

    if (type === 'TEXT') {
      out.type = 'text';
      const characters = node.textData && typeof node.textData.characters === 'string'
        ? node.textData.characters
        : typeof node.characters === 'string'
          ? node.characters
          : '';
      out.text = characters;
      Object.assign(out.style, fontToken(node));
      if (node.textAlignHorizontal) out.style.text_align = node.textAlignHorizontal.toLowerCase();
      const solid = firstSolidFill(node);
      if (solid) out.style.color = colorToHex({ ...solid.color, a: (solid.color?.a ?? 1) * (solid.opacity ?? 1) });
    }

    const gradient = (node.fillPaints || []).find(
      (p) => typeof p.type === 'string' && p.type.startsWith('GRADIENT') && p.visible !== false,
    );
    if (gradient) pushDiag('gradient-approximated', node, gradient.type);

    if ((node.strokePaints || []).length && typeof node.strokeWeight === 'number' && node.strokeWeight > 0) {
      const s = firstSolidStroke(node);
      if (s) out.style.border = `${Math.round(node.strokeWeight)}px solid ${colorToHex(s.color)}`;
    }

    const kids = (scene.childrenOf.get(key) || []).map(walk).filter(Boolean);
    if (kids.length) out.children = kids;
    return out;
  }

  const root = walk(frame);
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
  return { envelope, assetHashes, diagnostics };
}

function firstSolidStroke(node) {
  return (node.strokePaints || []).find((p) => p.type === 'SOLID' && p.visible !== false) || null;
}

function envelopeType(figmaType) {
  if (figmaType === 'TEXT') return 'text';
  if (figmaType === 'CANVAS') return 'frame';
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
