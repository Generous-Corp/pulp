// Which font families this machine can actually render.
//
// This exists because a design's text is only safe to keep as LIVE text when we
// have the font it was measured with. Figma stores the string plus the glyph
// positions it laid out; if we re-lay the string with a substitute face, every
// advance width changes and the text drifts inside its own box.
//
// The design's logo is the clean example: its text is literally "TRI  Z", and
// the two spaces are the gap its A-mark sits in. Set in Sofia Pro that gap is
// exactly A-width; set in a fallback the gap collapses and the Z renders on top
// of the A. Nothing is broken — the mark is where the design put it — the Z is
// simply in the wrong place, and no amount of geometry fixing can fix it while
// the wrong font is doing the measuring.
//
// So availability decides: font present -> live text (editable, themeable,
// reflowable, which is the entire point of importing to a real UI). Font absent
// -> the glyph outlines Figma baked into the file, which are pixel-exact and
// need no font at all. The trade is made per family, from evidence, rather than
// globally from a guess.

import fs from 'node:fs';
import path from 'node:path';

// Where each platform keeps faces. A family is judged installed by FILENAME,
// which is a heuristic: a face's file is conventionally named after its family
// ("Roboto-Regular.ttf", "SofiaProRegular.otf"), but nothing enforces it, and
// reading the `name` table of every font on the machine to be certain would
// cost more than this decision is worth.
//
// The heuristic fails SAFE on purpose. A false "missing" outlines text that
// could have stayed live: the render is still exact, only less editable. A
// false "present" would keep live text measured by the wrong font — silent,
// wrong pixels, and the failure we are here to prevent. Prefer the loud, safe
// one.
const FONT_DIRS = process.platform === 'darwin'
  ? [
      path.join(process.env.HOME || '', 'Library/Fonts'),
      '/Library/Fonts',
      '/System/Library/Fonts',
      '/System/Library/Fonts/Supplemental',
    ]
  : process.platform === 'win32'
    ? ['C:\\Windows\\Fonts']
    : [
        path.join(process.env.HOME || '', '.fonts'),
        path.join(process.env.HOME || '', '.local/share/fonts'),
        '/usr/share/fonts',
        '/usr/local/share/fonts',
      ];

const FONT_EXT = /\.(ttf|otf|ttc|otc|dfont|woff2?)$/i;

/** "Sofia Pro" -> "sofiapro". Families and filenames only agree once case, */
/** spaces, dashes and underscores are gone.                                */
function normalize(name) {
  return String(name || '').toLowerCase().replace(/[\s_-]+/g, '');
}

let cache = null;

/** Every normalized face-file stem this machine has, scanned once. */
function installedStems() {
  if (cache) return cache;
  cache = new Set();
  for (const dir of FONT_DIRS) {
    if (!dir) continue;
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue; // a directory that does not exist is not an error, just empty
    }
    for (const e of entries) {
      if (e.isDirectory()) {
        // One level down covers the common per-family folder layout without
        // walking a deep tree on every import.
        try {
          for (const f of fs.readdirSync(path.join(dir, e.name))) {
            if (FONT_EXT.test(f)) cache.add(normalize(f.replace(FONT_EXT, '')));
          }
        } catch { /* unreadable subdir: skip */ }
        continue;
      }
      if (FONT_EXT.test(e.name)) cache.add(normalize(e.name.replace(FONT_EXT, '')));
    }
  }
  return cache;
}

/**
 * Can this machine render `family`?
 *
 * A face file is matched when its stem STARTS WITH the normalized family, so
 * "Roboto" matches Roboto-Regular / Roboto-Bold / RobotoCondensed. That last one
 * is a deliberate over-match: a family we might render slightly wrong is a far
 * better outcome than outlining every label on the machine because the weights
 * are named unusually.
 */
export function isFontAvailable(family) {
  const want = normalize(family);
  if (!want) return true; // no family named: nothing to be missing
  for (const stem of installedStems()) {
    if (stem.startsWith(want)) return true;
  }
  return false;
}

/** Reset the scan cache. Tests install/remove faces; nothing else should care. */
export function resetFontCache() {
  cache = null;
}
