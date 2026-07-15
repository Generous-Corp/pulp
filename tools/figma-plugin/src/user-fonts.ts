// User-supplied font cache for the drag-drop escape hatch.
//
// The Figma plugin API intentionally does not expose font byte access (see
// the runtime font-catalog contract). Users with non-system fonts (custom
// foundry faces, restricted-license families like Clash Grotesk) supply the
// `.ttf` / `.otf` file via a drag-drop zone in the plugin UI. The UI reads
// bytes via FileReader and forwards them to the sandbox; this module is the
// sandbox-side store.
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
  ///
  /// Throws if the bytes don't look like a real font file. Two gates:
  ///   1. Bytes must contain at least one full SFNT/WOFF header
  ///      (4 magic + 8 table-count region = 12 bytes minimum). A 0-byte
  ///      or under-12-byte drop is rejected here; the previous behavior
  ///      was to fall through to filename-based mime detection, which
  ///      silently propagated an empty / corrupt blob into the zip.
  ///   2. The bytes' SFNT magic OR a recognisable font-file extension
  ///      must match — at least one must say "this is a font." Pure
  ///      magic-byte rejection would be too strict (older font tools
  ///      occasionally produce non-standard headers); pure
  ///      filename-trust is too loose (any `.ttf`-renamed binary would
  ///      pass). Requiring at least one signal catches the common
  ///      "I dropped the wrong file" UX failure mode.
  async add(
    family: string,
    style: string,
    bytes: Uint8Array,
    original_filename: string,
  ): Promise<UserFontEntry> {
    if (bytes.length === 0) {
      throw new Error(
        `font drop "${original_filename}" is empty (0 bytes); refusing to add to the asset cache.`,
      );
    }
    if (bytes.length < 12) {
      throw new Error(
        `font drop "${original_filename}" is ${bytes.length} bytes — too short to be a valid SFNT/WOFF font (need at least 12 bytes for the header). Refusing to add.`,
      );
    }
    const sniffed = detectFontMime(bytes, original_filename);
    if (sniffed === "application/octet-stream") {
      throw new Error(
        `font drop "${original_filename}" doesn't match any known font format ` +
        `(no SFNT/WOFF magic bytes, filename has no font extension). Refusing to add.`,
      );
    }
    const content_hash = await sha256Hex(bytes);
    const asset_id = `userfont-${content_hash.slice(0, 12)}`;
    const entry: UserFontEntry = {
      family,
      style,
      asset_id,
      content_hash,
      bytes,
      mime: sniffed,
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
