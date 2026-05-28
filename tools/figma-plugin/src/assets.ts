// Asset extraction + content-hash dedup. Runs in the Figma sandbox half.
//
// Strategy:
//   - For image fills: Figma exposes a stable `imageHash`. Multiple nodes can
//     reference the same hash. We cache by imageHash → bytes once.
//   - For VECTOR / BOOLEAN_OPERATION / shape nodes: call exportAsync(SVG_STRING).
//     There's no built-in dedup key, so we compute sha256(bytes) ourselves.
//   - Each emitted asset gets a stable `asset_id` keyed by content_hash (sha256
//     hex of the bytes). Nodes reference assets via `asset_ref = asset_id`.
//
// The crypto subtle API is available in Figma's plugin sandbox (modern V8).

export interface AssetEntry {
  asset_id: string;
  content_hash: string;       // sha256 hex
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
  async captureExportedNode(
    node: SceneNode,
    format: "SVG_STRING" | "PNG" = "SVG_STRING",
  ): Promise<{ assetId: string } | { error: string }> {
    if (!("exportAsync" in node)) return { error: "node does not support exportAsync" };
    const exportable = node as ExportMixin;
    let data: Uint8Array;
    let mime: string;
    try {
      if (format === "SVG_STRING") {
        const svgStr = await exportable.exportAsync({ format: "SVG_STRING" });
        if (!svgStr || svgStr.length < 50) {
          return { error: `SVG export produced ${svgStr?.length ?? 0} bytes; likely empty/degenerate node` };
        }
        data = textToBytes(svgStr);
        mime = "image/svg+xml";
      } else {
        data = await exportable.exportAsync({ format: "PNG" });
        if (!data || data.length < 50) {
          return { error: `PNG export produced ${data?.length ?? 0} bytes; likely empty/degenerate node` };
        }
        mime = "image/png";
      }
    } catch (err) {
      return { error: err instanceof Error ? err.message : String(err) };
    }
    const sha = await sha256Hex(data);
    const cached = this.byHash.get(sha);
    if (cached) {
      cached.original_uri_aliases.push(`figma://nodeExport/${node.id}/${format}`);
      return { assetId: cached.asset_id };
    }
    const assetId = `${format === "SVG_STRING" ? "svg" : "png"}-${sha.slice(0, 12)}`;
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

async function sha256Hex(bytes: Uint8Array): Promise<string> {
  // crypto.subtle.digest expects an ArrayBuffer view.
  // TS 5+ refuses Uint8Array<ArrayBufferLike> for BufferSource; slice through ArrayBuffer.
  const ab = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
  const buf = await crypto.subtle.digest("SHA-256", ab);
  const arr = new Uint8Array(buf);
  let hex = "";
  for (let i = 0; i < arr.length; i++) hex += arr[i].toString(16).padStart(2, "0");
  return hex;
}

function textToBytes(s: string): Uint8Array {
  return new TextEncoder().encode(s);
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
  // JPEG / GIF / WebP size parsing skipped — slice-2 plugin doesn't need exact pixel dims.
  return undefined;
}

function svgSize(bytes: Uint8Array): { w: number; h: number } | undefined {
  // Quick regex against the first 200 chars of the SVG header.
  const head = new TextDecoder().decode(bytes.subarray(0, 400));
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
