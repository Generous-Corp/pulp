// PulpGain WASM entry point — wraps PulpGain as a WAMv2 AudioWorklet module
#include "pulp_gain.hpp"
#include <pulp/format/web/wam_adapter.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

// Create the global WAM bridge with PulpGain factory
static pulp::format::wam::WamProcessorBridge g_bridge(pulp::examples::create_pulp_gain);

// Snapshot buffer for the state read ABI. wam_state_size() serializes once into
// this control-thread buffer; wam_read_state() copies it out. Two-call protocol
// avoids double-serialization and keeps the heap pointer owned by JS.
static std::vector<uint8_t> g_state_snapshot;

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
    g_bridge.schedule_midi(status, data1, data2, offset);
}

__attribute__((used, visibility("default")))
const char* wam_descriptor() {
    static std::string json;
    json = g_bridge.descriptor().to_json();
    return json.c_str();
}

// ── State ABI (control thread) ───────────────────────────────────────────
// Protocol: JS calls wam_state_size() to learn the byte count and snapshot the
// state, allocates that many bytes on the wasm heap, then calls
// wam_read_state(ptr) to copy the snapshot out. wam_write_state(ptr,size)
// restores from a heap buffer.

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
