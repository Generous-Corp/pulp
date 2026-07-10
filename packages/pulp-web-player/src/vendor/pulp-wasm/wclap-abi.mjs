// wclap-abi.mjs — the WebCLAP (CLAP 1.2.x, wasm32) ABI in one place.
//
// This is the single source of truth for the CLAP struct offsets, event-type
// constants, extension ids, and host-callback trampoline modules that the Pulp
// WebCLAP hosts marshal against. It is the worklet-safe twin of the constants
// hand-inlined at the top of the classic worklet bundle
// (./wclap-processor.js) and the ones in the main-thread offline host
// (core/format/src/wasm/wclap-host.mjs). Because an AudioWorklet module loaded
// via `addModule()` is a CLASSIC script and cannot `import`, wclap-processor.js
// cannot import this module at runtime — it inlines an identical copy. The
// package test `test/wclap-abi-parity.test.mjs` extracts the inlined numbers
// from the worklet source and asserts they equal these, so the two can never
// silently drift. Keep this module and that inlined block byte-for-byte in step.
//
// All offsets are the wasm32 ABI (pointers 4 bytes, `double` 8-byte aligned)
// pinned to the CLAP 1.2.x layout the WebCLAP module is built against.

// ── extension ids ──────────────────────────────────────────────────────────
export const CLAP_PLUGIN_FACTORY_ID = "clap.plugin-factory";
export const CLAP_EXT_PARAMS = "clap.params";
export const CLAP_EXT_STATE = "clap.state";
export const CLAP_EXT_AUDIO_PORTS = "clap.audio-ports";
export const CLAP_EXT_NOTE_PORTS = "clap.note-ports";

// ── event spaces + types ─────────────────────────────────────────────────────
export const CLAP_CORE_EVENT_SPACE_ID = 0;
export const CLAP_EVENT_NOTE_ON = 0;
export const CLAP_EVENT_NOTE_OFF = 1;
export const CLAP_EVENT_PARAM_VALUE = 5;
export const CLAP_EVENT_MIDI = 10;
export const CLAP_EVENT_MIDI_SYSEX = 11;

// ── clap_param_info flags (clap/ext/params.h) ────────────────────────────────
export const CLAP_PARAM_IS_STEPPED = 1 << 0;

// ── struct offsets ───────────────────────────────────────────────────────────
// clap_plugin_t fn-pointer table (wasm32):
export const PLUGIN = {
  init: 8, destroy: 12, activate: 16, deactivate: 20,
  start_processing: 24, stop_processing: 28, reset: 32, process: 36,
  get_extension: 40, on_main_thread: 44,
};
// clap_plugin_factory_t: get_plugin_count@0, get_plugin_descriptor@4, create_plugin@8
export const FACTORY = { count: 0, descriptor: 4, create: 8 };
// clap_plugin_entry_t: init@12, get_factory@20 (after clap_version{4}+deinit ptr)
export const ENTRY = { init: 12, get_factory: 20 };
// clap_plugin_descriptor_t: id@12, name@16 (after clap_version + id/name ptrs)
export const DESC = { id: 12, name: 16 };
// clap_param_info_t: id@0, flags@4, cookie@8, name[256]@12, module[1024]@268,
// min@1296, max@1304, default@1312. Size 1320.
export const PARAM_INFO = { size: 1320, id: 0, flags: 4, name: 12, min: 1296, max: 1304, def: 1312 };
// clap_event_header_t: size@0, time@4, space_id@8, type@10, flags@12. Size 16.
export const EVENT_HEADER = { size: 0, time: 4, space_id: 8, type: 10, flags: 12 };
// clap_event_param_value_t (48): +param_id@16 +cookie@20 +note_id@24 +port@28
// +channel@30 +key@32 (pad) +value@40.
export const PARAM_EVENT = { size: 48, param_id: 16, cookie: 20, note_id: 24, port: 28, channel: 30, key: 32, value: 40 };
// clap_event_note_t (40): +note_id@16 +port@20 +channel@22 +key@24 (pad) +velocity@32.
export const NOTE_EVENT = { size: 40, note_id: 16, port: 20, channel: 22, key: 24, velocity: 32 };
// clap_event_midi_t (24): +port_index@16 +data[3]@18.
export const MIDI_EVENT = { size: 24, port: 16, data: 18 };
// clap_event_midi_sysex_t (28): +port_index@16 (pad) +buffer@20 +size@24.
export const SYSEX_EVENT = { size: 28, port: 16, buffer: 20, bytes: 24 };
// clap_input_events_t (12): ctx@0, size(fn)@4, get(fn)@8.
export const IN_EVENTS = { size: 12, ctx: 0, count: 4, get: 8 };
// clap_output_events_t (8): ctx@0, try_push(fn)@4.
export const OUT_EVENTS = { size: 8, ctx: 0, try_push: 4 };
// clap_process_t (40): steady_time(i64)@0, frames_count@8, transport@12,
// audio_inputs@16, audio_outputs@20, in_count@24, out_count@28,
// in_events@32, out_events@36.
export const PROCESS = { size: 40, frames: 8, transport: 12, audio_in: 16, audio_out: 20, in_count: 24, out_count: 28, in_events: 32, out_events: 36 };
// clap_audio_buffer_t (24): data32@0, data64@4, channel_count@8, latency@12, constant_mask@16.
export const AUDIO_BUFFER = { size: 24, data32: 0, data64: 4, channels: 8, latency: 12, constant_mask: 16 };
// clap_host_t (48): clap_version@0..12, host_data@12, name@16, vendor@20, url@24,
// version@28, get_extension@32, request_restart@36, request_process@40, request_callback@44.
export const HOST = { size: 48, name: 16, vendor: 20, url: 24, version: 28, get_extension: 32, request_restart: 36, request_process: 40, request_callback: 44 };
// clap_plugin_state_t: save@0, load@4.  clap_ostream_t: ctx@0, write@4.
// clap_istream_t: ctx@0, read@4.
export const STATE_EXT = { save: 0, load: 4 };
export const OSTREAM = { size: 8, ctx: 0, write: 4 };
export const ISTREAM = { size: 8, ctx: 0, read: 4 };

// ── host-callback trampoline modules ─────────────────────────────────────────
// Each is a ~48-byte wasm module that imports a JS function `h.f` and exports a
// wasm wrapper `fn` forwarding to it — the funcref-identity trick that lets a
// plain JS closure sit in the plugin's indirect function table in every engine
// (no `WebAssembly.Function`, no flag). Keys: "<params>-><results>", letters
// i=i32, I=i64. The stream key "iiI->I" is the clap_ostream.write /
// clap_istream.read signature `(ctx, buffer, uint64 size) -> int64` — new for
// the worklet host's clap.state support; verified to compile + round-trip a
// BigInt i64 by scripts/gen-trampoline (see the PR notes).
export const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
  "iiI->I": "AGFzbQEAAAABCAFgA39/fgF+AgcBAWgBZgAAAwIBAAcGAQJmbgABCgwBCgAgACABIAIQAAs=",
};
