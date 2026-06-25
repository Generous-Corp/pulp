// Asset extraction + content-hash dedup. Runs in the Figma sandbox half.
//
// Strategy:
//   - For image fills: Figma exposes a stable `imageHash`. Multiple nodes can
//     reference the same hash. We cache by imageHash → bytes once.
//   - For VECTOR / BOOLEAN_OPERATION / shape nodes: call exportAsync(SVG_STRING).
//     There's no built-in dedup key, so we compute content_hash from the bytes.
//   - Each emitted asset gets a stable `asset_id` keyed by content_hash. Nodes
//     reference assets via `asset_ref = asset_id`.
//
// content_hash uses SHA-256 when the Figma sandbox exposes crypto.subtle, with
// a deterministic FNV-1a fallback for older/sandboxed runtimes. The hash is for
// deduplication, not cryptographic integrity.

export interface AssetEntry {
  asset_id: string;
  content_hash: string;
  bytes: Uint8Array;          // raw bytes, fed to ZIP packager in UI
  mime: string;
  width?: number;
  height?: number;
  original_uri: string;
  original_uri_aliases: string[];
}

export class AssetCache {
  private byHash = new Map<string, AssetEntry>();
  /// imageHash → asset_id mapping for fast lookup on subsequent image fills
  private byFigmaImageHash = new Map<string, string>();

  size(): number {
    return this.byHash.size;
  }

  entries(): AssetEntry[] {
    return Array.from(this.byHash.values());
  }

  /// Capture an image fill identified by Figma's imageHash. Returns asset_id.
  async captureImageFill(imageHash: string): Promise<string | null> {
    const cached = this.byFigmaImageHash.get(imageHash);
    if (cached) {
      const entry = this.byHash.get(cached);
      if (entry) entry.original_uri_aliases.push(`figma://imageHash/${imageHash}`);
      return cached;
    }

    const image = figma.getImageByHash(imageHash);
    if (!image) return null;

    let bytes: Uint8Array;
    try {
      bytes = await image.getBytesAsync();
    } catch {
      return null;
    }

    const sha = await sha256Hex(bytes);
    const assetId = `img-${sha.slice(0, 12)}`;
    const mime = detectImageMime(bytes);
    const dim = peekImageSize(bytes, mime);
    const entry: AssetEntry = {
      asset_id: assetId,
      content_hash: sha,
      bytes,
      mime,
      width: dim?.w,
      height: dim?.h,
      original_uri: `figma://imageHash/${imageHash}`,
      original_uri_aliases: [],
    };
    this.byHash.set(sha, entry);
    this.byFigmaImageHash.set(imageHash, assetId);
    return assetId;
  }

  /// Capture a vector / shape node by rasterizing or exporting as SVG.
  /// Returns { assetId } on success, { error: <reason> } on failure so the
  /// caller can emit a diagnostic.
  ///
  /// NOTE: we use Figma's "SVG" format (returns Uint8Array bytes directly)
  /// not "SVG_STRING" (returns a JS string). Reason: Figma's plugin sandbox
  /// does not expose TextEncoder at runtime even though TS thinks it does
  /// via the "webworker" lib types. Working with raw bytes avoids the
  /// encoding step entirely.
  async captureExportedNode(
    node: SceneNode,
    format: "SVG" | "PNG" = "SVG",
  ): Promise<{ assetId: string } | { error: string }> {
    if (!("exportAsync" in node)) return { error: "node does not support exportAsync" };
    const exportable = node as ExportMixin;
    let data: Uint8Array;
    let mime: string;
    try {
      // Export PNGs at 2× scale so they stay crisp when Pulp's renderer
      // displays at retina (2×) output. SVG ignores scale (it's vector-
      // resolution-independent already).
      const settings: ExportSettings =
        format === "PNG"
          ? { format: "PNG", constraint: { type: "SCALE", value: 2 } }
          : { format };
      data = await exportable.exportAsync(settings);
      if (!data || data.length < 50) {
        return { error: `${format} export produced ${data?.length ?? 0} bytes; likely empty/degenerate node` };
      }
      mime = format === "SVG" ? "image/svg+xml" : "image/png";
    } catch (err) {
      return { error: err instanceof Error ? err.message : String(err) };
    }
    const sha = await sha256Hex(data);
    const cached = this.byHash.get(sha);
    if (cached) {
      cached.original_uri_aliases.push(`figma://nodeExport/${node.id}/${format}`);
      return { assetId: cached.asset_id };
    }
    const assetId = `${format === "SVG" ? "svg" : "png"}-${sha.slice(0, 12)}`;
    const dim = format === "PNG" ? peekImageSize(data, "image/png") : svgSize(data);
    const entry: AssetEntry = {
      asset_id: assetId,
      content_hash: sha,
      bytes: data,
      mime,
      width: dim?.w,
      height: dim?.h,
      original_uri: `figma://nodeExport/${node.id}/${format}`,
      original_uri_aliases: [],
    };
    this.byHash.set(sha, entry);
    return { assetId };
  }
}

// ── crypto + format helpers ──────────────────────────────────────────────

// Prefer SHA-256 when crypto.subtle is available; fall back to a fast
// non-cryptographic FNV-1a 64-bit hash (encoded as 16 hex chars) for
// content-addressable dedupe in runtimes without WebCrypto.
export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  try {
    if (typeof crypto !== "undefined" && crypto.subtle && typeof crypto.subtle.digest === "function") {
      const ab = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
      const buf = await crypto.subtle.digest("SHA-256", ab);
      const arr = new Uint8Array(buf);
      let hex = "";
      for (let i = 0; i < arr.length; i++) hex += arr[i].toString(16).padStart(2, "0");
      return hex;
    }
  } catch {
    /* fall through */
  }
  return fnv1a64Hex(bytes);
}

function fnv1a64Hex(bytes: Uint8Array): string {
  // FNV-1a 64-bit using two 32-bit halves (JS doesn't have native uint64).
  // High and low halves multiplied by FNV prime (1099511628211 = 0x100000001b3),
  // which is 0x000001b3 << 32 plus 0x00000001b3 modulo nothing.
  let hLo = 0xe7c8b3a1 | 0;
  let hHi = 0xcbf29ce4 | 0;
  for (let i = 0; i < bytes.length; i++) {
    hLo ^= bytes[i];
    // Multiply by 0x100000001b3 split into low (0x000001b3) and a bit of high.
    const cLo = Math.imul(hLo, 0x000001b3);
    const cHi = Math.imul(hHi, 0x000001b3) + Math.imul(hLo, 0x00000001) | 0;
    hLo = cLo | 0;
    hHi = cHi | 0;
  }
  const toHex = (n: number) => (n >>> 0).toString(16).padStart(8, "0");
  return toHex(hHi) + toHex(hLo);
}

function detectImageMime(bytes: Uint8Array): string {
  if (bytes.length >= 4 && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47) return "image/png";
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) return "image/jpeg";
  if (bytes.length >= 4 && bytes[0] === 0x47 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x38) return "image/gif";
  if (bytes.length >= 4 && bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46) return "image/webp";
  return "application/octet-stream";
}

function peekImageSize(bytes: Uint8Array, mime: string): { w: number; h: number } | undefined {
  if (mime === "image/png" && bytes.length >= 24) {
    // PNG: IHDR width/height at bytes 16..23 (big-endian uint32)
    const w = (bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19];
    const h = (bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23];
    return { w: w >>> 0, h: h >>> 0 };
  }
  // JPEG / GIF / WebP size parsing skipped — importer only needs exact PNG dims.
  return undefined;
}

function svgSize(bytes: Uint8Array): { w: number; h: number } | undefined {
  // Manual ASCII decode of the SVG header (Figma sandbox has no TextDecoder).
  // SVG headers are pure ASCII so this is safe.
  const head = asciiHeader(bytes, 600);
  const widthMatch = head.match(/<svg[^>]*\bwidth=["']?(\d+(?:\.\d+)?)/);
  const heightMatch = head.match(/<svg[^>]*\bheight=["']?(\d+(?:\.\d+)?)/);
  if (widthMatch && heightMatch) {
    return { w: parseFloat(widthMatch[1]), h: parseFloat(heightMatch[1]) };
  }
  const vbMatch = head.match(/<svg[^>]*\bviewBox=["']?\s*([\d.\s-]+)/);
  if (vbMatch) {
    const parts = vbMatch[1].trim().split(/\s+/);
    if (parts.length === 4) return { w: parseFloat(parts[2]), h: parseFloat(parts[3]) };
  }
  return undefined;
}

function asciiHeader(bytes: Uint8Array, maxLen: number): string {
  const n = Math.min(bytes.length, maxLen);
  let s = "";
  for (let i = 0; i < n; i++) {
    const c = bytes[i];
    if (c === 0) break;
    s += String.fromCharCode(c < 128 ? c : 63 /* '?' for non-ASCII */);
  }
  return s;
}
