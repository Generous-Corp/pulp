#!/usr/bin/env node
// Regenerate the committed synthetic `.fig` test fixture.
//
// We cannot commit a real Figma export (licensing + size), so this builds a
// tiny but format-faithful `.fig`: a real kiwi schema + message, deflate/zstd
// compressed into a fig-kiwi container, zipped with a bundled raster. Round-trip
// through the decoder is what the fig tests assert against.
//
//   node make_synthetic_fig.mjs [outPath]
//
// Default outPath: ../../../../test/fixtures/imports/fig/synthetic.fig

import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateRawSync, zstdCompressSync } from 'node:zlib';

// ── kiwi writer (inverse of fig/kiwi.mjs reader) ─────────────────────────────
class Writer {
  constructor() {
    this.bytes = [];
  }
  byte(b) {
    this.bytes.push(b & 0xff);
  }
  varUint(v) {
    v >>>= 0;
    do {
      let b = v & 0x7f;
      v >>>= 7;
      if (v) b |= 0x80;
      this.bytes.push(b);
    } while (v);
  }
  varInt(v) {
    this.varUint(v < 0 ? ~(v << 1) : v << 1);
  }
  float(v) {
    // Normalize -0.0 to +0.0: the reader's single-byte-zero fast path keys on a
    // zero low byte, which -0.0 and denormals would also produce, so the fixture
    // only ever emits values that survive that path. Real Figma files don't
    // carry -0.0 coordinates; the decoder's reader matches real kiwi regardless.
    if (v === 0) v = 0;
    const dv = new DataView(new ArrayBuffer(4));
    dv.setFloat32(0, v, true);
    let bits = dv.getUint32(0, true);
    if (bits === 0) {
      this.byte(0);
      return;
    }
    // Reader recovers D = rotl(S, 23); so store S = rotr(D, 23) = rotl(D, 9).
    const s = ((bits >>> 23) | (bits << 9)) >>> 0;
    this.byte(s & 0xff);
    this.byte((s >>> 8) & 0xff);
    this.byte((s >>> 16) & 0xff);
    this.byte((s >>> 24) & 0xff);
  }
  string(str) {
    for (const b of Buffer.from(str, 'utf8')) this.bytes.push(b);
    this.byte(0);
  }
  buffer() {
    return Buffer.from(this.bytes);
  }
}

// ── schema ──────────────────────────────────────────────────────────────────
// Definition kinds: 0 ENUM, 1 STRUCT, 2 MESSAGE. Builtin field types are
// negative (see KIWI_TYPE); positive types index into this table.
const T = { BOOL: -1, BYTE: -2, INT: -3, UINT: -4, FLOAT: -5, STRING: -6 };

// Index assignments (must be stable — fields reference defs by index).
const D = {
  GUID: 0,
  ParentIndex: 1,
  Vector: 2,
  Color: 3,
  Paint: 4,
  ImageRef: 5,
  TextData: 6,
  NodeType: 7,
  NodeChange: 8,
  Message: 9,
};

const defs = [];
defs[D.GUID] = { name: 'GUID', kind: 1, fields: [
  { name: 'sessionID', type: T.UINT, isArray: false, value: 0 },
  { name: 'localID', type: T.UINT, isArray: false, value: 0 },
]};
defs[D.ParentIndex] = { name: 'ParentIndex', kind: 1, fields: [
  { name: 'guid', type: D.GUID, isArray: false, value: 0 },
  { name: 'position', type: T.STRING, isArray: false, value: 0 },
]};
defs[D.Vector] = { name: 'Vector', kind: 1, fields: [
  { name: 'x', type: T.FLOAT, isArray: false, value: 0 },
  { name: 'y', type: T.FLOAT, isArray: false, value: 0 },
]};
defs[D.Color] = { name: 'Color', kind: 1, fields: [
  { name: 'r', type: T.FLOAT, isArray: false, value: 0 },
  { name: 'g', type: T.FLOAT, isArray: false, value: 0 },
  { name: 'b', type: T.FLOAT, isArray: false, value: 0 },
  { name: 'a', type: T.FLOAT, isArray: false, value: 0 },
]};
defs[D.ImageRef] = { name: 'ImageRef', kind: 1, fields: [
  { name: 'hash', type: T.STRING, isArray: false, value: 0 },
]};
// Paint as a message (optional fields) mirrors real exports closely enough.
defs[D.Paint] = { name: 'Paint', kind: 2, fields: [
  { name: 'type', type: T.STRING, isArray: false, value: 1 },
  { name: 'color', type: D.Color, isArray: false, value: 2 },
  { name: 'visible', type: T.BOOL, isArray: false, value: 3 },
  { name: 'opacity', type: T.FLOAT, isArray: false, value: 4 },
  { name: 'image', type: D.ImageRef, isArray: false, value: 5 },
]};
defs[D.TextData] = { name: 'TextData', kind: 2, fields: [
  { name: 'characters', type: T.STRING, isArray: false, value: 1 },
]};
defs[D.NodeType] = { name: 'NodeType', kind: 0, fields: [
  { name: 'NONE', type: 0, isArray: false, value: 0 },
  { name: 'CANVAS', type: 0, isArray: false, value: 1 },
  { name: 'FRAME', type: 0, isArray: false, value: 2 },
  { name: 'TEXT', type: 0, isArray: false, value: 3 },
  { name: 'VECTOR', type: 0, isArray: false, value: 4 },
]};
defs[D.NodeChange] = { name: 'NodeChange', kind: 2, fields: [
  { name: 'guid', type: D.GUID, isArray: false, value: 1 },
  { name: 'parentIndex', type: D.ParentIndex, isArray: false, value: 2 },
  { name: 'type', type: D.NodeType, isArray: false, value: 3 },
  { name: 'name', type: T.STRING, isArray: false, value: 4 },
  { name: 'size', type: D.Vector, isArray: false, value: 5 },
  { name: 'fillPaints', type: D.Paint, isArray: true, value: 6 },
  { name: 'cornerRadius', type: T.FLOAT, isArray: false, value: 7 },
  { name: 'textData', type: D.TextData, isArray: false, value: 8 },
  { name: 'internalOnly', type: T.BOOL, isArray: false, value: 9 },
]};
defs[D.Message] = { name: 'Message', kind: 2, fields: [
  { name: 'nodeChanges', type: D.NodeChange, isArray: true, value: 1 },
]};

function writeSchema(w) {
  w.varUint(defs.length);
  for (const def of defs) {
    w.string(def.name);
    w.byte(def.kind);
    w.varUint(def.fields.length);
    for (const f of def.fields) {
      w.string(f.name);
      w.varInt(f.type);
      w.byte(f.isArray ? 1 : 0);
      w.varUint(f.value);
    }
  }
}

// ── message value writers ────────────────────────────────────────────────────
const guid = (w, s, l) => { w.varUint(s); w.varUint(l); };
const vector = (w, x, y) => { w.float(x); w.float(y); };
const color = (w, r, g, b, a) => { w.float(r); w.float(g); w.float(b); w.float(a); };

function solidPaint(w, [r, g, b, a]) {
  w.varUint(1); w.string('SOLID');
  w.varUint(2); color(w, r, g, b, a);
  w.varUint(3); w.byte(1);
  w.varUint(0);
}
function imagePaint(w, hash) {
  w.varUint(1); w.string('IMAGE');
  w.varUint(3); w.byte(1);
  w.varUint(5); w.string(hash);
  w.varUint(0);
}

// A node writer: emits message fields present in `n`.
function writeNode(w, n) {
  w.varUint(1); guid(w, n.s, n.l);
  if (n.parent) { w.varUint(2); guid(w, n.parent[0], n.parent[1]); w.string(n.pos || 'a'); }
  w.varUint(3); w.varUint(n.type); // NodeType enum value
  w.varUint(4); w.string(n.name);
  if (n.size) { w.varUint(5); vector(w, n.size[0], n.size[1]); }
  if (n.fills) {
    w.varUint(6); w.varUint(n.fills.length);
    for (const p of n.fills) (p.image ? imagePaint(w, p.image) : solidPaint(w, p.color));
  }
  if (n.radius !== undefined) { w.varUint(7); w.float(n.radius); }
  if (n.text !== undefined) { w.varUint(8); w.varUint(1); w.string(n.text); w.varUint(0); }
  w.varUint(0); // end NodeChange
}

const IMAGE_HASH = 'a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0';
// One 1×1 PNG so the asset-extraction path has real bytes.
const PNG_1x1 = Buffer.from(
  '89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d4944415478da63f8cf' +
  'c0f01f0005000201f6a4f0e70000000049454e44ae426082',
  'hex',
);

const NODES = [
  { s: 0, l: 0, type: 0, name: 'Document' },
  { s: 0, l: 1, type: 1, name: 'Page One', parent: [0, 0] },
  { s: 0, l: 2, type: 2, name: 'Plugin UI', parent: [0, 1], pos: 'a',
    size: [320, 200], radius: 8, fills: [{ color: [0.08, 0.09, 0.11, 1] }] },
  { s: 0, l: 3, type: 2, name: 'Knob — Cutoff', parent: [0, 2], pos: 'a',
    size: [64, 64], radius: 32, fills: [{ image: IMAGE_HASH }] },
  { s: 0, l: 4, type: 3, name: 'Label', parent: [0, 2], pos: 'b',
    size: [80, 16], text: 'CUTOFF', fills: [{ color: [1, 1, 1, 1] }] },
  { s: 0, l: 5, type: 1, name: 'Empty Page', parent: [0, 0], pos: 'b' },
];

function buildCanvasFig() {
  const schemaW = new Writer();
  writeSchema(schemaW);
  const msgW = new Writer();
  msgW.varUint(1); // Message.nodeChanges field id
  msgW.varUint(NODES.length);
  for (const n of NODES) writeNode(msgW, n);
  msgW.varUint(0); // end Message

  const schemaChunk = deflateRawSync(schemaW.buffer());
  const msgChunk = zstdCompressSync(msgW.buffer());

  const head = Buffer.alloc(12);
  head.write('fig-kiwi', 0, 'latin1');
  head.writeUInt32LE(106, 8);
  const parts = [head];
  for (const chunk of [schemaChunk, msgChunk]) {
    const len = Buffer.alloc(4);
    len.writeUInt32LE(chunk.length, 0);
    parts.push(len, chunk);
  }
  return Buffer.concat(parts);
}

// ── minimal ZIP writer (stored entries) ──────────────────────────────────────
function zip(entries) {
  const crcTable = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  const crc32 = (buf) => {
    let c = 0xffffffff;
    for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const locals = [];
  const central = [];
  let offset = 0;
  for (const [name, data] of entries) {
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4);
    lh.writeUInt16LE(0, 8); // stored
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18);
    lh.writeUInt32LE(data.length, 22);
    lh.writeUInt16LE(nameBuf.length, 26);
    locals.push(lh, nameBuf, data);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0, 10);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(data.length, 20);
    ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt32LE(offset, 42);
    central.push(ch, nameBuf);
    offset += lh.length + nameBuf.length + data.length;
  }
  const centralStart = offset;
  const centralBuf = Buffer.concat(central);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralBuf.length, 12);
  eocd.writeUInt32LE(centralStart, 16);
  return Buffer.concat([...locals, centralBuf, eocd]);
}

const here = dirname(fileURLToPath(import.meta.url));
const outPath = process.argv[2]
  ? resolve(process.argv[2])
  : resolve(here, '../../../../test/fixtures/imports/fig/synthetic.fig');
mkdirSync(dirname(outPath), { recursive: true });
const figBytes = zip([
  ['canvas.fig', buildCanvasFig()],
  ['meta.json', Buffer.from(JSON.stringify({ file_name: 'Synthetic Fixture' }))],
  [`images/${IMAGE_HASH}`, PNG_1x1],
]);
writeFileSync(outPath, figBytes);
process.stderr.write(`wrote ${outPath} (${figBytes.length} bytes)\n`);
