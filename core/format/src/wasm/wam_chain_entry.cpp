// Shared WAMv2 C ABI entry point for a Pulp WASM *rack* (an in-worklet chain of
// N processors running inside ONE wasm module and ONE AudioWorkletProcessor).
//
// This is the parallel of wam_entry.cpp: it defines the SAME wam_* C ABI, but
// against a single process-global WamChainBridge instead of a WamProcessorBridge.
// A rack module links EXACTLY ONE of the two entry TUs (never both — they define
// the same C symbols), so PulpWam.cmake's pulp_add_wam_rack() substitutes this
// file for wam_entry.cpp. Each rack supplies only the stage list:
//
//     std::vector<std::unique_ptr<pulp::format::Processor>> pulp_wam_make_chain();
//
// resolved at link time (exactly one definition per rack executable).
//
// The rack reuses the entire single-plugin wam_* ABI verbatim — NO new export.
// Parameters are addressed "<stage>:<paramId>" (plain "6" still means stage 0);
// state is a "PWR1" container of per-stage "PWS1" blobs; the descriptor is the
// composite of the endpoints. So the four ABI sites PulpWam.cmake tracks
// (this/wam_entry.cpp, the EXPORTED_FUNCTIONS allowlist, wam-runtime.mjs
// makeBridge, wam-processor.js) need no per-rack change: the Node runner and the
// worklet drive a rack through the identical bridge.

#include <pulp/format/web/wam_adapter.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Provided by each rack's entry translation unit (e.g. midi_transpose_mono_synth
// rack.cpp): the ordered stages of the rack.
std::vector<std::unique_ptr<pulp::format::Processor>> pulp_wam_make_chain();

namespace {
// ChainFactory is a plain function pointer; pulp_wam_make_chain decays into it.
// Not invoked until wam_init().
pulp::format::wam::WamChainBridge g_chain(pulp_wam_make_chain);

// Snapshot buffer for the state read ABI (control thread), mirroring wam_entry.
std::vector<uint8_t> g_state_snapshot;
} // namespace

extern "C" {

__attribute__((used, visibility("default")))
int wam_init(double sample_rate, int block_size) {
    return g_chain.initialize(sample_rate, block_size) ? 1 : 0;
}

__attribute__((used, visibility("default")))
void wam_process(const float* input, float* output, int channels, int frames) {
    g_chain.process(input, output, channels, frames);
}

__attribute__((used, visibility("default")))
void wam_set_param(const char* id, float value) {
    g_chain.set_parameter_value(id, value);
}

__attribute__((used, visibility("default")))
float wam_get_param(const char* id) {
    return g_chain.get_parameter_value(id);
}

__attribute__((used, visibility("default")))
void wam_midi(int status, int data1, int data2, int offset) {
    g_chain.schedule_midi(static_cast<uint8_t>(status),
                          static_cast<uint8_t>(data1),
                          static_cast<uint8_t>(data2), offset);
}

// Monotonic count of parameter changes across all stages, so a host can notice
// a plugin rewriting its own parameters (e.g. a preset load) — see
// WamChainBridge::param_epoch.
__attribute__((used, visibility("default")))
unsigned int wam_param_epoch() {
    return g_chain.param_epoch();
}

// Every stage's values, concatenated in wam_parameters() order.
__attribute__((used, visibility("default")))
int wam_read_param_values(float* dst, int capacity) {
    return g_chain.read_param_values(dst, capacity);
}

__attribute__((used, visibility("default")))
int wam_midi_sysex(const uint8_t* data, int size, int offset) {
    return g_chain.schedule_sysex(data, size, offset) ? 1 : 0;
}

__attribute__((used, visibility("default")))
int wam_midi_out_drain(uint8_t* dst, int cap) {
    return g_chain.drain_midi_out(dst, cap);
}

__attribute__((used, visibility("default")))
void wam_reset() {
    g_chain.request_reset();
}

__attribute__((used, visibility("default")))
void wam_prepare(double sample_rate, int block_size) {
    g_chain.prepare(sample_rate, block_size);
}

__attribute__((used, visibility("default")))
int wam_latency_samples() {
    return g_chain.latency_samples();
}

__attribute__((used, visibility("default")))
void wam_set_transport(int is_playing, double bpm, double position_beats,
                       double position_samples, int tsig_num, int tsig_den) {
    g_chain.set_transport(is_playing != 0, bpm, position_beats,
                          position_samples, tsig_num, tsig_den);
}

__attribute__((used, visibility("default")))
const char* wam_descriptor() {
    static std::string json;
    json = g_chain.descriptor_json();
    return json.c_str();
}

__attribute__((used, visibility("default")))
const char* wam_parameters() {
    static std::string json;
    json = g_chain.parameters_json();
    return json.c_str();
}

__attribute__((used, visibility("default")))
int wam_state_size() {
    g_state_snapshot = g_chain.get_state();
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
    return g_chain.set_state(src, static_cast<size_t>(size)) ? 1 : 0;
}

} // extern "C"
