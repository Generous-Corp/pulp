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
// Host-provided extensions the web host answers via clap_host.get_extension —
// the services a Pulp CLAP plugin queries during activate/process (parity with
// the native ClapSlot host path). `clap.latency`/`clap.tail` carry PDC through
// request_callback → plugin.on_main_thread → host->…->changed; `clap.log` and
// `clap.thread-check` are the diagnostic/assertion surfaces; the host sides of
// `clap.state` (mark_dirty) and `clap.params` (rescan/clear/request_flush) let
// the plugin tell the host its persisted state or parameter set went stale.
export const CLAP_EXT_LOG = "clap.log";
export const CLAP_EXT_THREAD_CHECK = "clap.thread-check";
export const CLAP_EXT_LATENCY = "clap.latency";
export const CLAP_EXT_TAIL = "clap.tail";

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
// clap_plugin_params_t vtable (wasm32): count@0, get_info@4, get_value@8,
// value_to_text@12, text_to_value@16, flush@20. `value_to_text` is the ONLY
// display source CLAP defines — clap_param_info deliberately carries no unit
// field, so a host that wants "1.50 s" instead of "1.50" must call it.
export const PARAMS_EXT = { count: 0, get_info: 4, get_value: 8, value_to_text: 12, text_to_value: 16, flush: 20 };
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
// Plugin-side latency/tail: uint32_t get(plugin) @0 — the host reads these when
// the plugin signals a change so PDC/tail can be reported.
export const PLUGIN_LATENCY = { get: 0 };
export const PLUGIN_TAIL = { get: 0 };
// Host-extension vtables (the structs the web host allocates + returns from
// clap_host.get_extension). All wasm32, fn pointers 4 bytes.
// clap_host_log_t: void log(host, severity, msg) @0.
export const HOST_LOG = { size: 4, log: 0 };
// clap_host_thread_check_t: bool is_main_thread(host) @0, is_audio_thread @4.
export const HOST_THREAD_CHECK = { size: 8, is_main_thread: 0, is_audio_thread: 4 };
// clap_host_latency_t / clap_host_tail_t: void changed(host) @0.
export const HOST_LATENCY = { size: 4, changed: 0 };
export const HOST_TAIL = { size: 4, changed: 0 };
// clap_host_state_t: void mark_dirty(host) @0.
export const HOST_STATE = { size: 4, mark_dirty: 0 };
// clap_host_params_t: void rescan(host, flags) @0, clear(host, param_id, flags)
// @4, request_flush(host) @8.
export const HOST_PARAMS = { size: 12, rescan: 0, clear: 4, request_flush: 8 };

// ── host-callback trampoline modules ─────────────────────────────────────────
// Each is a ~48-byte wasm module that imports a JS function `h.f` and exports a
// wasm wrapper `fn` forwarding to it — the funcref-identity trick that lets a
// plain JS closure sit in the plugin's indirect function table in every engine
// (no `WebAssembly.Function`, no flag). Keys: "<params>-><results>", letters
// i=i32, I=i64. The stream key "iiI->I" is the clap_ostream.write /
// clap_istream.read signature `(ctx, buffer, uint64 size) -> int64` — new for
// the worklet host's clap.state support; verified to compile + round-trip a
// BigInt i64 by scripts/gen-trampoline (see the PR notes). The void-returning
// "ii->" / "iii->" keys are the host-extension callbacks with no result:
// clap_host_params.rescan(host, flags), and clap_host_log.log(host, severity,
// msg) / clap_host_params.clear(host, id, flags). Every entry is verified to
// compile + forward its exact signature by the package test.
export const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
  "iiI->I": "AGFzbQEAAAABCAFgA39/fgF+AgcBAWgBZgAAAwIBAAcGAQJmbgABCgwBCgAgACABIAIQAAs=",
  "ii->": "AGFzbQEAAAABBgFgAn9/AAIHAQFoAWYAAAMCAQAHBgECZm4AAQoKAQgAIAAgARAACw==",
  "iii->": "AGFzbQEAAAABBwFgA39/fwACBwEBaAFmAAADAgEABwYBAmZuAAEKDAEKACAAIAEgAhAACw==",
};

// ── display unit, recovered from clap_plugin_params.value_to_text ────────────
// The WAM ABI reports a parameter's display unit directly (descriptor JSON:
// {"unit":"%"}); CLAP deliberately does NOT — clap_param_info has no unit field,
// and a host is expected to DISPLAY whatever value_to_text() renders ("35.00 %").
// The Pulp web UI (and the shared player's generated widgets) format the number
// themselves and need the unit SUFFIX, so the WebCLAP hosts probe value_to_text
// at two values and hand the raw strings here.
//
// A probe is { value, text }. The suffix that follows the rendered number IS the
// unit when both probes agree AND each rendered number reads back as the probed
// value. A plugin with a fully custom to_string ("12 o'clock", an enum label,
// "quality=0.75") has no such stable suffix — this then reports "" rather than
// inventing one, and the widget shows a bare number, exactly as before.
const NUMBER_PREFIX = /^\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*(.*)$/;
export function deriveDisplayUnit(probes) {
  let unit = null;
  for (const probe of probes || []) {
    const m = NUMBER_PREFIX.exec(String(probe.text ?? ""));
    if (!m) return "";
    // value_to_text rounds (Pulp's CLAP entry renders 2 fixed decimals), so the
    // read-back only has to land near the probed value; a custom rendering that
    // happens to start with an unrelated number is rejected here.
    const tolerance = 0.01 + Math.abs(probe.value) * 1e-6;
    if (!(Math.abs(parseFloat(m[1]) - probe.value) <= tolerance)) return "";
    const suffix = m[2].trim();
    if (unit === null) unit = suffix;
    else if (unit !== suffix) return "";   // suffix varies with the value ⇒ not a unit
  }
  return unit || "";
}
