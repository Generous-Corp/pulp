// `.fig` container unpacking.
//
// A `.fig` file is either a raw kiwi container or (the common case) a ZIP holding
// `canvas.fig` plus `images/<sha>` rasters and a small `meta.json`. The kiwi
// container itself is: 8-byte ASCII magic ("fig-kiwi"), u32 LE version, then a
// sequence of [u32 LE length][chunk] records. Chunk 0 is the compiled schema
// (raw DEFLATE); chunk 1 is the message (zstd, or raw DEFLATE on older files).
//
// Node's zlib provides both inflateRawSync and zstdDecompressSync (Node >= 22),
// so no third-party decompressors are required.

import { inflateRawSync, zstdDecompressSync } from 'node:zlib';

// Ceiling on any single decompressed chunk. Real Figma files (even large ones)
// decode to tens of MB; this bounds a decompression bomb from OOM-killing the
// process. Node throws RangeError past the limit, which the CLI maps to exit 3.
const MAX_DECOMPRESSED_BYTES = 512 * 1024 * 1024;

const ZIP_LOCAL_MAGIC = 0x04034b50;
const ZIP_EOCD_MAGIC = 0x06054b50;
const ZSTD_MAGIC = 0xfd2fb528; // little-endian 28 b5 2f fd

/** True if the buffer begins with the ZIP local-file signature ("PK\3\4"). */
export function isZip(buffer) {
  return buffer.length >= 2 && buffer[0] === 0x50 && buffer[1] === 0x4b;
}

/**
 * Extract every ZIP entry into a { name: Buffer } map. Handles both the
 * streaming (data-descriptor) and central-directory layouts, supporting stored
 * and DEFLATE methods — the only two Figma emits.
 */
export function unzip(buffer) {
  // If any local header uses a data descriptor (sizes deferred), the sizes in
  // the local headers are zero, so trust the central directory instead.
  let cursor = 0;
  let needsCentral = false;
  const streamed = {};
  while (cursor + 30 <= buffer.length) {
    if (buffer.readUInt32LE(cursor) !== ZIP_LOCAL_MAGIC) break;
    const flags = buffer.readUInt16LE(cursor + 6);
    const method = buffer.readUInt16LE(cursor + 8);
    const compSize = buffer.readUInt32LE(cursor + 18);
    const nameLen = buffer.readUInt16LE(cursor + 26);
    const extraLen = buffer.readUInt16LE(cursor + 28);
    if (flags & 0x08) {
      needsCentral = true;
      break;
    }
    const name = buffer.toString('utf8', cursor + 30, cursor + 30 + nameLen);
    const dataStart = cursor + 30 + nameLen + extraLen;
    const raw = buffer.subarray(dataStart, dataStart + compSize);
    streamed[name] = method === 8 ? inflateRawSync(raw, { maxOutputLength: MAX_DECOMPRESSED_BYTES }) : Buffer.from(raw);
    cursor = dataStart + compSize;
  }
  return needsCentral ? unzipViaCentralDirectory(buffer) : streamed;
}

function unzipViaCentralDirectory(buffer) {
  let eocd = -1;
  for (let i = buffer.length - 22; i >= 0; i--) {
    if (buffer.readUInt32LE(i) === ZIP_EOCD_MAGIC) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error('malformed ZIP: no end-of-central-directory');
  const count = buffer.readUInt16LE(eocd + 10);
  let cd = buffer.readUInt32LE(eocd + 16);
  const entries = {};
  for (let n = 0; n < count; n++) {
    const method = buffer.readUInt16LE(cd + 10);
    const compSize = buffer.readUInt32LE(cd + 20);
    const nameLen = buffer.readUInt16LE(cd + 28);
    const extraLen = buffer.readUInt16LE(cd + 30);
    const commentLen = buffer.readUInt16LE(cd + 32);
    const localOffset = buffer.readUInt32LE(cd + 42);
    const name = buffer.toString('utf8', cd + 46, cd + 46 + nameLen);
    const localNameLen = buffer.readUInt16LE(localOffset + 26);
    const localExtraLen = buffer.readUInt16LE(localOffset + 28);
    const dataStart = localOffset + 30 + localNameLen + localExtraLen;
    const raw = buffer.subarray(dataStart, dataStart + compSize);
    entries[name] = method === 8 ? inflateRawSync(raw, { maxOutputLength: MAX_DECOMPRESSED_BYTES }) : Buffer.from(raw);
    cd += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

/**
 * @typedef {Object} FigContainer
 * @property {string} magic       8-byte container magic (e.g. "fig-kiwi").
 * @property {number} version     Container format version.
 * @property {Buffer} schemaBytes Decompressed kiwi schema (chunk 0).
 * @property {Buffer} messageBytes Decompressed kiwi message (chunk 1).
 * @property {Map<string,Buffer>} images  images/<hash> → raster bytes.
 * @property {Object|null} meta   Parsed meta.json when present.
 */

/**
 * Unpack raw `.fig` file bytes into decompressed schema + message chunks and any
 * bundled raster assets.
 * @param {Buffer} fileBytes
 * @returns {FigContainer}
 */
export function unpackFig(fileBytes) {
  let canvas = fileBytes;
  const images = new Map();
  let meta = null;
  if (isZip(fileBytes)) {
    const entries = unzip(fileBytes);
    const canvasName = Object.keys(entries).find((n) => n.endsWith('.fig'));
    if (!canvasName) throw new Error('.fig ZIP has no inner canvas .fig entry');
    canvas = entries[canvasName];
    for (const [name, bytes] of Object.entries(entries)) {
      if (name.startsWith('images/') && !name.endsWith('/')) {
        images.set(name.slice('images/'.length), bytes);
      }
    }
    if (entries['meta.json']) {
      try {
        meta = JSON.parse(entries['meta.json'].toString('utf8'));
      } catch {
        meta = null;
      }
    }
  }

  const magic = canvas.toString('latin1', 0, 8);
  const version = canvas.readUInt32LE(8);
  let off = 12;
  const chunks = [];
  while (off + 4 <= canvas.length) {
    const len = canvas.readUInt32LE(off);
    off += 4;
    chunks.push(canvas.subarray(off, off + len));
    off += len;
  }
  if (chunks.length < 2) {
    throw new Error(`unexpected .fig container: ${chunks.length} chunk(s)`);
  }

  return {
    magic,
    version,
    schemaBytes: decompressChunk(chunks[0]),
    messageBytes: decompressChunk(chunks[1]),
    images,
    meta,
  };
}

function decompressChunk(chunk) {
  if (chunk.length >= 4 && chunk.readUInt32LE(0) === ZSTD_MAGIC) {
    return zstdDecompressSync(chunk, { maxOutputLength: MAX_DECOMPRESSED_BYTES });
  }
  return inflateRawSync(chunk, { maxOutputLength: MAX_DECOMPRESSED_BYTES });
}
