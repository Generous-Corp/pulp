// Shared WAMv2 C ABI export definitions — the single source for every wam_*
// entry point.
//
// Included as TEXT by wam_entry.cpp (single plugin, one global
// WamProcessorBridge) and wam_chain_entry.cpp (rack, one global WamChainBridge)
// after each file #defines PULP_WAM_BRIDGE to name its process-global bridge.
// Both bridges expose the SAME method surface (including descriptor_json()), so
// the ~150 lines of wam_* exports live here once instead of being copy-pasted
// and hand-kept-in-sync between the two entries — which is exactly the drift the
// "four ABI sites must stay in sync" note in PulpWam.cmake warned about. The
// remaining three sites (EXPORTED_FUNCTIONS, wam-runtime.mjs makeBridge,
// wam-processor.js moduleExports) are unaffected by which entry a plugin links.
//
// This file is not a compiled translation unit; PulpWam.cmake still compiles
// exactly one of wam_entry.cpp / wam_chain_entry.cpp per plugin. Including it
// twice in one program would define the wam_* C symbols twice, so a plugin links
// exactly one entry (never both — they own the same C ABI).

#ifndef PULP_WAM_BRIDGE
#error "define PULP_WAM_BRIDGE (the process-global bridge) before including wam_entry_abi.inc"
#endif

namespace {
// Snapshot buffer for the state read ABI (control thread). wam_state_size()
// serializes once into this buffer; wam_read_state() copies it out.
std::vector<uint8_t> g_wam_state_snapshot;
} // namespace

extern "C" {

__attribute__((used, visibility("default")))
int wam_init(double sample_rate, int block_size) {
    return PULP_WAM_BRIDGE.initialize(sample_rate, block_size) ? 1 : 0;
}

// PLANAR channel-pointer ABI (matches WCLAP / native CLAP): `input` and `output`
// each point at an array of `channels` channel pointers, one contiguous float
// buffer per channel. No interleave — the processor renders straight through.
__attribute__((used, visibility("default")))
void wam_process(const float* const* input, float* const* output,
                 int channels, int frames) {
    PULP_WAM_BRIDGE.process(input, output, channels, frames);
}

__attribute__((used, visibility("default")))
void wam_set_param(const char* id, float value) {
    PULP_WAM_BRIDGE.set_parameter_value(id, value);
}

__attribute__((used, visibility("default")))
float wam_get_param(const char* id) {
    return PULP_WAM_BRIDGE.get_parameter_value(id);
}

__attribute__((used, visibility("default")))
void wam_midi(int status, int data1, int data2, int offset) {
    PULP_WAM_BRIDGE.schedule_midi(static_cast<uint8_t>(status),
                                  static_cast<uint8_t>(data1),
                                  static_cast<uint8_t>(data2), offset);
}

// Monotonic counter of parameter changes, including the plugin's own writes
// from inside process(). A host polls this once per block and only re-reads
// values when it moves — see the bridge's param_epoch().
__attribute__((used, visibility("default")))
unsigned int wam_param_epoch() {
    return PULP_WAM_BRIDGE.param_epoch();
}

// Bulk parameter read in wam_parameters() order. Writes min(count, capacity)
// floats; returns the total count (> capacity means truncated).
__attribute__((used, visibility("default")))
int wam_read_param_values(float* dst, int capacity) {
    return PULP_WAM_BRIDGE.read_param_values(dst, capacity);
}

// Variable-length SysEx input (full F0 .. F7 payload). Returns 0 when the
// message was dropped (pool exhausted or payload too large), 1 on success.
__attribute__((used, visibility("default")))
int wam_midi_sysex(const uint8_t* data, int size, int offset) {
    return PULP_WAM_BRIDGE.schedule_sysex(data, size, offset) ? 1 : 0;
}

// Drain the MIDI produced by the last wam_process() call. Returns the number of
// bytes AVAILABLE; copies min(cap, available) into dst. See the bridge's
// drain_midi_out for the record layout.
__attribute__((used, visibility("default")))
int wam_midi_out_drain(uint8_t* dst, int cap) {
    return PULP_WAM_BRIDGE.drain_midi_out(dst, cap);
}

// One-shot DSP-state reset. Processor has no reset() virtual — the reset
// contract is ProcessContext::reset_requested — so this flips a bridge flag
// that raises reset_requested for exactly the next block. RT-safe.
__attribute__((used, visibility("default")))
void wam_reset() {
    PULP_WAM_BRIDGE.request_reset();
}

// Re-prepare for a real sample-rate / block-size change (a Web Audio context
// can run at 44.1 kHz). Dispatched from the worklet's port.onmessage, which in
// an AudioWorkletProcessor runs ON THE AUDIO RENDER THREAD — so this executes
// *between* render quanta, never concurrently with wam_process(), but NOT on a
// separate control thread. It re-runs Processor::prepare() (which may
// allocate), so it must never be called from within wam_process().
__attribute__((used, visibility("default")))
void wam_prepare(double sample_rate, int block_size) {
    PULP_WAM_BRIDGE.prepare(sample_rate, block_size);
}

// Processor-reported delay-compensation latency in samples. Control thread.
__attribute__((used, visibility("default")))
int wam_latency_samples() {
    return PULP_WAM_BRIDGE.latency_samples();
}

// Host-supplied transport snapshot (Web Audio has no transport). Copied into
// ProcessContext at the top of each process(). Control thread.
__attribute__((used, visibility("default")))
void wam_set_transport(int is_playing, double bpm, double position_beats,
                       double position_samples, int tsig_num, int tsig_den) {
    PULP_WAM_BRIDGE.set_transport(is_playing != 0, bpm, position_beats,
                                  position_samples, tsig_num, tsig_den);
}

__attribute__((used, visibility("default")))
const char* wam_descriptor() {
    static std::string json;
    json = PULP_WAM_BRIDGE.descriptor_json();
    return json.c_str();
}

__attribute__((used, visibility("default")))
const char* wam_parameters() {
    static std::string json;
    json = PULP_WAM_BRIDGE.parameters_json();
    return json.c_str();
}

// ── State ABI (control thread) ───────────────────────────────────────────
// Protocol: JS calls wam_state_size() to learn the byte count and snapshot the
// state, allocates that many bytes on the wasm heap, then wam_read_state(ptr)
// copies the snapshot out. wam_write_state(ptr,size) restores from a heap
// buffer.

__attribute__((used, visibility("default")))
int wam_state_size() {
    g_wam_state_snapshot = PULP_WAM_BRIDGE.get_state();
    return static_cast<int>(g_wam_state_snapshot.size());
}

__attribute__((used, visibility("default")))
void wam_read_state(uint8_t* dst) {
    if (dst && !g_wam_state_snapshot.empty())
        std::memcpy(dst, g_wam_state_snapshot.data(), g_wam_state_snapshot.size());
}

__attribute__((used, visibility("default")))
int wam_write_state(const uint8_t* src, int size) {
    if (!src || size < 0) return 0;
    return PULP_WAM_BRIDGE.set_state(src, static_cast<size_t>(size)) ? 1 : 0;
}

} // extern "C"
