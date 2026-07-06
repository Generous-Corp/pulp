// Kiwi wire-format reader for Figma `.fig` payloads.
//
// Kiwi is a compact, self-describing binary format (Evan Wallace, MIT). A `.fig`
// file embeds its own compiled schema (definition list) ahead of the message, so
// this reader is generic: it decodes whatever schema the file carries rather than
// hard-coding Figma's node shapes. That is what lets a single decoder survive the
// Figma schema evolving between file versions.
//
// Format reference (all public):
//   - varuint / varint (zig-zag) are LEB128.
//   - float uses a bit-rotation so that whole numbers stay small; 0.0 is one byte.
//   - string is NUL-terminated UTF-8.
//   - schema := varuint(defCount) then per def: name, kind byte
//     (0=ENUM, 1=STRUCT, 2=MESSAGE), varuint(fieldCount), then per field:
//     name, varint(type), byte(isArray), varuint(value).
//   - a MESSAGE is a stream of varuint field ids terminated by 0; a STRUCT is its
//     fields in order; an ENUM is a single varuint mapped to a member name.

// Builtin type ids (negative). Anything >= 0 indexes into the definition table.
export const KIWI_TYPE = {
  BOOL: -1,
  BYTE: -2,
  INT: -3,
  UINT: -4,
  FLOAT: -5,
  STRING: -6,
  INT64: -7,
  UINT64: -8,
};

const DEF_ENUM = 0;
const DEF_STRUCT = 1;
const DEF_MESSAGE = 2;

/** Sequential little-endian reader over a Buffer. */
export class ByteReader {
  constructor(buffer) {
    this.b = buffer;
    this.i = 0;
  }

  byte() {
    return this.b[this.i++];
  }

  varUint() {
    let value = 0;
    let shift = 0;
    let byte;
    do {
      byte = this.b[this.i++];
      value |= (byte & 0x7f) << shift;
      shift += 7;
    } while (byte & 0x80 && shift < 35);
    return value >>> 0;
  }

  varInt() {
    const v = this.varUint();
    return v & 1 ? ~(v >>> 1) : v >>> 1;
  }

  varUint64() {
    let value = 0n;
    let shift = 0n;
    let byte;
    do {
      byte = this.b[this.i++];
      value |= BigInt(byte & 0x7f) << shift;
      shift += 7n;
    } while (byte & 0x80 && shift < 70n);
    return value;
  }

  varInt64() {
    const v = this.varUint64();
    return v & 1n ? ~(v >> 1n) : v >> 1n;
  }

  float() {
    const first = this.b[this.i];
    if (first === 0) {
      this.i += 1;
      return 0;
    }
    let bits =
      first |
      (this.b[this.i + 1] << 8) |
      (this.b[this.i + 2] << 16) |
      (this.b[this.i + 3] << 24);
    this.i += 4;
    // Undo the encoder's left-rotate-by-23 so common integers round-trip.
    bits = ((bits << 23) | (bits >>> 9)) >>> 0;
    FLOAT_VIEW.setUint32(0, bits, true);
    return FLOAT_VIEW.getFloat32(0, true);
  }

  string() {
    const start = this.i;
    // Bound the scan to the buffer: a truncated file whose final string has no
    // NUL terminator would otherwise spin forever (b[i] past end is undefined,
    // and undefined !== 0).
    while (this.i < this.b.length && this.b[this.i] !== 0) this.i += 1;
    if (this.i >= this.b.length) throw new Error('unterminated string at end of buffer');
    const s = this.b.toString('utf8', start, this.i);
    this.i += 1;
    return s;
  }

  get done() {
    return this.i >= this.b.length;
  }
}

const FLOAT_VIEW = new DataView(new ArrayBuffer(4));

/**
 * Read a compiled kiwi schema into a flat definition table.
 * @returns {{name:string, kind:number, fields:Array}[]}
 */
export function readSchema(reader) {
  const defCount = reader.varUint();
  const defs = new Array(defCount);
  for (let d = 0; d < defCount; d++) {
    const name = reader.string();
    const kind = reader.byte();
    const fieldCount = reader.varUint();
    const fields = new Array(fieldCount);
    for (let f = 0; f < fieldCount; f++) {
      fields[f] = {
        name: reader.string(),
        type: reader.varInt(),
        isArray: reader.byte() !== 0,
        value: reader.varUint(),
      };
    }
    defs[d] = { name, kind, fields };
  }
  return defs;
}

/**
 * Build a decoder bound to a schema. Returns helpers plus the resolved index of
 * the root `Message` definition (the top-level type of a `.fig` data chunk).
 */
export function makeDecoder(defs) {
  const indexByName = new Map(defs.map((def, i) => [def.name, i]));
  // Field lookup by id is hot in message decoding; precompute per definition.
  const fieldById = defs.map((def) => {
    if (def.kind !== DEF_MESSAGE) return null;
    const map = new Map();
    for (const field of def.fields) map.set(field.value, field);
    return map;
  });

  function decodeValue(reader, type) {
    switch (type) {
      case KIWI_TYPE.BOOL:
        return reader.byte() !== 0;
      case KIWI_TYPE.BYTE:
        return reader.byte();
      case KIWI_TYPE.INT:
        return reader.varInt();
      case KIWI_TYPE.UINT:
        return reader.varUint();
      case KIWI_TYPE.FLOAT:
        return reader.float();
      case KIWI_TYPE.STRING:
        return reader.string();
      case KIWI_TYPE.INT64:
        return reader.varInt64();
      case KIWI_TYPE.UINT64:
        return reader.varUint64();
      default: {
        const def = defs[type];
        if (def.kind === DEF_ENUM) {
          const raw = reader.varUint();
          const member = def.fields.find((m) => m.value === raw);
          return member ? member.name : raw;
        }
        if (def.kind === DEF_STRUCT) return decodeStruct(reader, type);
        return decodeMessage(reader, type);
      }
    }
  }

  function decodeField(reader, field) {
    if (field.isArray) {
      const n = reader.varUint();
      const arr = new Array(n);
      for (let k = 0; k < n; k++) arr[k] = decodeValue(reader, field.type);
      return arr;
    }
    return decodeValue(reader, field.type);
  }

  function decodeStruct(reader, typeIndex) {
    const def = defs[typeIndex];
    const out = {};
    for (const field of def.fields) out[field.name] = decodeField(reader, field);
    return out;
  }

  function decodeMessage(reader, typeIndex) {
    const byId = fieldById[typeIndex];
    const out = {};
    for (;;) {
      const id = reader.varUint();
      if (id === 0) break;
      const field = byId.get(id);
      if (!field) {
        throw new Error(
          `unknown field id ${id} in message ${defs[typeIndex].name}`,
        );
      }
      out[field.name] = decodeField(reader, field);
    }
    return out;
  }

  return {
    indexByName,
    rootIndex: indexByName.get('Message'),
    decodeMessage,
  };
}
