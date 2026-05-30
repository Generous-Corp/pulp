// #43c — User-supplied font cache for the drag-drop escape hatch.
//
// The Figma plugin API intentionally does not expose font byte access (see
// #43a-rev redesign note). Users with non-system fonts (custom foundry
// faces, restricted-license families like Clash Grotesk) supply the
// `.ttf` / `.otf` file via a drag-drop zone in the plugin UI. The UI
// reads bytes via FileReader and forwards them to the sandbox; this
// module is the sandbox-side store.
//
// Storage shape: keyed by `family|style` (the same tuple
// `font_family_assets` entries are deduped on), value carries the
// bytes plus a content-hashed asset_id stable across re-imports.
//
// At envelope serialize time, every `font_family_assets[i]` entry whose
// (family, style) matches a cache entry gets `asset_id = "userfont-<hash>"`
// stamped on it; the bytes are added to the asset_manifest so the zip
// writer (UI side) packages them as `assets/<hash>.ttf`.

import { sha256Hex } from "./assets";

export interface UserFontEntry {
  family: string;
  style: string;
  asset_id: string;
  content_hash: string;
  bytes: Uint8Array;
  mime: string;
  original_filename: string;
}

export class UserFontCache {
  private byKey = new Map<string, UserFontEntry>();

  private static makeKey(family: string, style: string): string {
    return `${family}|${style}`;
  }

  size(): number {
    return this.byKey.size;
  }

  entries(): UserFontEntry[] {
    return Array.from(this.byKey.values());
  }

  /// Look up an entry by (family, style). Used at serialize time to
  /// decide whether a font_family_assets entry should be stamped with
  /// an asset_id.
  lookup(family: string, style: string): UserFontEntry | undefined {
    return this.byKey.get(UserFontCache.makeKey(family, style));
  }

  /// Add a user-supplied font, returning the asset_id. Idempotent on the
  /// (family, style) key — re-adding overwrites the previous bytes.
  async add(
    family: string,
    style: string,
    bytes: Uint8Array,
    original_filename: string,
  ): Promise<UserFontEntry> {
    const content_hash = await sha256Hex(bytes);
    const asset_id = `userfont-${content_hash.slice(0, 12)}`;
    const mime = detectFontMime(bytes, original_filename);
    const entry: UserFontEntry = {
      family,
      style,
      asset_id,
      content_hash,
      bytes,
      mime,
      original_filename,
    };
    this.byKey.set(UserFontCache.makeKey(family, style), entry);
    return entry;
  }
}

/// SFNT magic-number sniff with filename fallback. Order matters:
/// fonts share a common 4-byte tag at offset 0 (`OTTO` for OpenType
/// CFF, `\0\1\0\0` for TrueType, `true` for legacy TT, `wOFF`/`wOF2`
/// for WOFF). If bytes are too short, fall back to the filename
/// extension — the file came from the user, so we trust their naming.
function detectFontMime(bytes: Uint8Array, filename: string): string {
  if (bytes.length >= 4) {
    const b0 = bytes[0], b1 = bytes[1], b2 = bytes[2], b3 = bytes[3];
    // OpenType (CFF): "OTTO"
    if (b0 === 0x4f && b1 === 0x54 && b2 === 0x54 && b3 === 0x4f) return "font/otf";
    // TrueType: 0x00010000
    if (b0 === 0x00 && b1 === 0x01 && b2 === 0x00 && b3 === 0x00) return "font/ttf";
    // Legacy "true"
    if (b0 === 0x74 && b1 === 0x72 && b2 === 0x75 && b3 === 0x65) return "font/ttf";
    // WOFF / WOFF2
    if (b0 === 0x77 && b1 === 0x4f && b2 === 0x46 && b3 === 0x46) return "font/woff";
    if (b0 === 0x77 && b1 === 0x4f && b2 === 0x46 && b3 === 0x32) return "font/woff2";
  }
  const lower = filename.toLowerCase();
  if (lower.endsWith(".otf")) return "font/otf";
  if (lower.endsWith(".ttf")) return "font/ttf";
  if (lower.endsWith(".woff2")) return "font/woff2";
  if (lower.endsWith(".woff")) return "font/woff";
  return "application/octet-stream";
}
