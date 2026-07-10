// Generate + verify CLAP-stream host trampolines needed for the worklet host:
//   iiI->I : (i32 ctx, i32 buf, i64 size) -> i64   [clap_ostream.write / clap_istream.read]
// Emits base64 of a tiny wasm module that imports h.f and exports fn forwarding to it.
// Also regenerates the existing three to confirm the byte-builder matches.

const I32 = 0x7f, I64 = 0x7e;
const vt = { i: I32, I: I64 };

function buildTrampoline(sigKey) {
  // sigKey like "ii->i" or "iiI->I"; letters: i=i32, I=i64; ""/side empty = void
  const [pStr, rStr] = sigKey.split("->");
  const params = [...pStr].map((c) => vt[c]);
  const results = rStr ? [...rStr].map((c) => vt[c]) : [];

  const bytes = [];
  const push = (...b) => bytes.push(...b);
  // magic + version
  push(0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00);

  // section helper
  const section = (id, content) => { push(id, content.length, ...content); };
  const vec = (arr) => [arr.length, ...arr];

  // type section: one functype
  const functype = [0x60, params.length, ...params, results.length, ...results];
  section(1, [1, ...functype]);
  // import section: h.f as func type 0
  const imp = [0x01, 0x68, 0x01, 0x66, 0x00, 0x00]; // "h","f", func, typeidx 0
  section(2, [1, ...imp]);
  // function section: one func of type 0
  section(3, [1, 0x00]);
  // export section: "fn" -> func index 1 (0 is the import)
  const exp = [0x02, 0x66, 0x6e, 0x00, 0x01];
  section(7, [1, ...exp]);
  // code section: body forwards all params then call 0
  const body = [0x00]; // 0 local groups
  for (let i = 0; i < params.length; i++) body.push(0x20, i); // local.get i
  body.push(0x10, 0x00, 0x0b); // call 0 ; end
  const codeEntry = [body.length, ...body];
  section(10, [1, ...codeEntry]);

  return Uint8Array.from(bytes);
}

function b64(u8) {
  return Buffer.from(u8).toString("base64");
}

// Verify the known three match the committed base64 (byte-builder sanity).
const known = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
};
for (const [k, want] of Object.entries(known)) {
  const got = b64(buildTrampoline(k));
  console.log(`${k.padEnd(8)} ${got === want ? "MATCH" : "MISMATCH"}  ${got}`);
}

// The new stream trampoline.
const streamKey = "iiI->I";
const mod = buildTrampoline(streamKey);
const b = b64(mod);
console.log(`${streamKey.padEnd(8)} NEW      ${b}`);

// Prove it compiles + runs (BigInt i64 in/out).
const m = new WebAssembly.Module(mod);
let seen = null;
const inst = new WebAssembly.Instance(m, {
  h: { f: (ctx, buf, size) => { seen = { ctx, buf, size }; return size + 1n; } },
});
const r = inst.exports.fn(7, 99, 123n);
console.log("runtime check:", JSON.stringify({ ...seen, size: seen.size.toString() }), "->", r.toString(),
  (seen.ctx === 7 && seen.buf === 99 && seen.size === 123n && r === 124n) ? "PASS" : "FAIL");
