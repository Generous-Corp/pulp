// Shared WAMv2 C ABI entry point for Pulp WASM plugins.
//
// Defines every wam_* export ONCE against a single global WamProcessorBridge,
// so plugin entry points do not each re-implement the (previously copy-pasted)
// init/process/param/midi/descriptor/state boilerplate. PulpWam.cmake compiles
// this TU into every WAM plugin; each plugin supplies only the factory:
//
//     std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor();
//
// which is resolved at link time (exactly one definition per plugin
// executable). This is also why the WAMv2 state ABI is now available to every
// plugin uniformly, not just whichever entry point happened to implement it.
//
// The wam_* ABI defined below must stay in sync with three other lists (see
// tools/cmake/PulpWam.cmake): the EXPORTED_FUNCTIONS allowlist, the makeBridge
// methods in wam-runtime.mjs, and the moduleExports adapter in wam-processor.js.

#include <pulp/format/web/wam_adapter.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Provided by each plugin's entry translation unit.
std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor();

namespace {
// ProcessorFactory is a plain function pointer; pulp_wam_make_processor decays
// into it. The factory is not invoked until wam_init().
pulp::format::wam::WamProcessorBridge g_bridge(pulp_wam_make_processor);

// Snapshot buffer for the state read ABI (control thread). wam_state_size()
// serializes once into this buffer; wam_read_state() copies it out.
std::vector<uint8_t> g_state_snapshot;
} // namespace

extern "C" {

__attribute__((used, visibility("default")))
int wam_init(double sample_rate, int block_size) {
    return g_bridge.initialize(sample_rate, block_size) ? 1 : 0;
}

__attribute__((used, visibility("default")))
void wam_process(const float* input, float* output, int channels, int frames) {
    g_bridge.process(input, output, channels, frames);
}

__attribute__((used, visibility("default")))
void wam_set_param(const char* id, float value) {
    g_bridge.set_parameter_value(id, value);
}

__attribute__((used, visibility("default")))
float wam_get_param(const char* id) {
    return g_bridge.get_parameter_value(id);
}

__attribute__((used, visibility("default")))
void wam_midi(int status, int data1, int data2, int offset) {
    g_bridge.schedule_midi(static_cast<uint8_t>(status),
                           static_cast<uint8_t>(data1),
                           static_cast<uint8_t>(data2), offset);
}

// Monotonic counter of parameter changes, including the plugin's own writes
// from inside process(). A host polls this once per block and only re-reads
// values when it moves — see WamProcessorBridge::param_epoch.
__attribute__((used, visibility("default")))
unsigned int wam_param_epoch() {
    return g_bridge.param_epoch();
}

// Bulk parameter read in wam_parameters() order. Writes min(count, capacity)
// floats; returns the total count (> capacity means truncated).
__attribute__((used, visibility("default")))
int wam_read_param_values(float* dst, int capacity) {
    return g_bridge.read_param_values(dst, capacity);
}

// Variable-length SysEx input (full F0 .. F7 payload). Returns 0 when the
// message was dropped (pool exhausted or payload too large), 1 on success.
__attribute__((used, visibility("default")))
int wam_midi_sysex(const uint8_t* data, int size, int offset) {
    return g_bridge.schedule_sysex(data, size, offset) ? 1 : 0;
}

// Drain the MIDI produced by the last wam_process() call. Returns the number of
// bytes AVAILABLE; copies min(cap, available) into dst. See
// WamProcessorBridge::drain_midi_out for the record layout.
__attribute__((used, visibility("default")))
int wam_midi_out_drain(uint8_t* dst, int cap) {
    return g_bridge.drain_midi_out(dst, cap);
}

// One-shot DSP-state reset. Processor has no reset() virtual — the reset
// contract is ProcessContext::reset_requested — so this flips a bridge flag
// that raises reset_requested for exactly the next block. RT-safe.
__attribute__((used, visibility("default")))
void wam_reset() {
    g_bridge.request_reset();
}

// Re-prepare for a real sample-rate / block-size change (a Web Audio context
// can run at 44.1 kHz). CONTROL THREAD ONLY — this re-runs Processor::prepare()
// and may resize buffers, so it must never be called from wam_process().
__attribute__((used, visibility("default")))
void wam_prepare(double sample_rate, int block_size) {
    g_bridge.prepare(sample_rate, block_size);
}

// Processor-reported delay-compensation latency in samples. Control thread.
__attribute__((used, visibility("default")))
int wam_latency_samples() {
    return g_bridge.latency_samples();
}

// Host-supplied transport snapshot (Web Audio has no transport). Copied into
// ProcessContext at the top of each process(). Control thread.
__attribute__((used, visibility("default")))
void wam_set_transport(int is_playing, double bpm, double position_beats,
                       double position_samples, int tsig_num, int tsig_den) {
    g_bridge.set_transport(is_playing != 0, bpm, position_beats,
                           position_samples, tsig_num, tsig_den);
}

__attribute__((used, visibility("default")))
const char* wam_descriptor() {
    static std::string json;
    json = g_bridge.descriptor().to_json();
    return json.c_str();
}

__attribute__((used, visibility("default")))
const char* wam_parameters() {
    static std::string json;
    json = g_bridge.parameters_json();
    return json.c_str();
}

// ── State ABI (control thread) ───────────────────────────────────────────
// Protocol: JS calls wam_state_size() to learn the byte count and snapshot the
// state, allocates that many bytes on the wasm heap, then wam_read_state(ptr)
// copies the snapshot out. wam_write_state(ptr,size) restores from a heap
// buffer.

__attribute__((used, visibility("default")))
int wam_state_size() {
    g_state_snapshot = g_bridge.get_state();
    return static_cast<int>(g_state_snapshot.size());
}

__attribute__((used, visibility("default")))
void wam_read_state(uint8_t* dst) {
    if (dst && !g_state_snapshot.empty())
        std::memcpy(dst, g_state_snapshot.data(), g_state_snapshot.size());
}

__attribute__((used, visibility("default")))
int wam_write_state(const uint8_t* src, int size) {
    if (!src || size < 0) return 0;
    return g_bridge.set_state(src, static_cast<size_t>(size)) ? 1 : 0;
}

} // extern "C"
