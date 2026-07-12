// state/plugin-state.js — the SDK's host-facing plugin-state container, in JS.
//
// Pulp composes plugin state with ONE format on every host (see
// core/format/src/plugin_state_io.cpp):
//
//   [ "PLST" ][u32 version=1][u32 store_len][u32 plugin_len]   16-byte header
//   [ store bytes (a "PULP" StateStore blob) ][ plugin bytes ]
//   [u32 crc32 over everything above ]                          4-byte footer
//
// When the plugin owns no extra state, serialize() returns the BARE store blob
// with no envelope, so a container read back from a host may be either shape.
//
// This lives in the player package — not in a demo page — because it is
// ABI-AGNOSTIC by construction: WAM and WebCLAP both expose the same opaque
// `getState()` / `setState()` on the HostAdapter, and both carry this same
// container underneath. Anything a page wants to do with a plugin's own blob
// (the SuperConvolver demo swaps in a decoded impulse response; the state-memo
// demo swaps in a free-text note) is therefore ONE implementation working
// identically on both ABIs, and a bug in it cannot be fixed in only one of them.
//
// Splicing a plugin blob into a live plugin's state, without disturbing its
// parameters:
//
//   const { params } = parseContainer(await adapter.getState());
//   adapter.setState(buildContainer(params, myPluginBlob));

const ENV_MAGIC = [0x50, 0x4c, 0x53, 0x54];   // "PLST"
const STORE_MAGIC = [0x50, 0x55, 0x4c, 0x50]; // "PULP"
const ENV_VERSION = 1, ENV_HEADER = 16, ENV_FOOTER = 4;

/** zlib CRC-32 — byte-identical to crc32_simple in core/state/src/store.cpp. */
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let j = 0; j < 8; j++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (~crc) >>> 0;
}

const has4 = (b, m, o = 0) =>
  b.length >= o + 4 && b[o] === m[0] && b[o + 1] === m[1] && b[o + 2] === m[2] && b[o + 3] === m[3];

/** Split a host-state blob into { params, plugin }. Throws on an unknown shape. */
export function parseContainer(bytes) {
  // Bare StateStore blob (plugin owned no extra state): all params, no plugin.
  if (has4(bytes, STORE_MAGIC)) return { params: bytes.slice(), plugin: new Uint8Array(0) };
  if (!has4(bytes, ENV_MAGIC)) throw new Error("not a PLST envelope");
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const storeLen = dv.getUint32(8, true);
  const pluginLen = dv.getUint32(12, true);
  const params = bytes.slice(ENV_HEADER, ENV_HEADER + storeLen);
  const plugin = bytes.slice(ENV_HEADER + storeLen, ENV_HEADER + storeLen + pluginLen);
  return { params, plugin };
}

/** Compose { params, plugin } back into a host-state blob. Mirrors plugin_state_io::serialize. */
export function buildContainer(params, plugin) {
  if (!plugin || plugin.length === 0) return params.slice();
  const out = new Uint8Array(ENV_HEADER + params.length + plugin.length + ENV_FOOTER);
  const dv = new DataView(out.buffer);
  out.set(ENV_MAGIC, 0);
  dv.setUint32(4, ENV_VERSION, true);
  dv.setUint32(8, params.length, true);
  dv.setUint32(12, plugin.length, true);
  out.set(params, ENV_HEADER);
  out.set(plugin, ENV_HEADER + params.length);
  dv.setUint32(out.length - ENV_FOOTER, crc32(out.subarray(0, out.length - ENV_FOOTER)), true);
  return out;
}

/**
 * Replace ONLY the plugin-owned blob of a live plugin's state, preserving its
 * parameters. `adapter` is any HostAdapter (WAM or WebCLAP — the call is the
 * same). Returns the blob that was written.
 */
export async function setPluginBlob(adapter, plugin) {
  const current = await adapter.getState();
  let params = new Uint8Array(0);
  try { params = parseContainer(current).params; } catch { /* bare/empty: keep none */ }
  const next = buildContainer(params, plugin);
  await adapter.setState(next);
  return next;
}

/** Read the plugin-owned blob out of a live plugin's state (empty if none). */
export async function getPluginBlob(adapter) {
  try { return parseContainer(await adapter.getState()).plugin; }
  catch { return new Uint8Array(0); }
}
