#pragma once

// Plugin Slot for Pulp Host
// Wraps a loaded plugin instance with a uniform interface for audio processing.
// Handles plugin lifecycle: load → prepare → process → release.
//
// Usage:
//   auto slot = PluginSlot::load(info);
//   slot->prepare(48000, 512);
//   slot->process(output, input, midi_in, midi_out);
//   slot->release();

#include <pulp/host/scanner.hpp>
#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace pulp::host {

// ── Plugin parameter info ───────────────────────────────────────────────

// Parameter behavior flags. Loaders fill these from their format-native
// parameter descriptors; consumers (automation routing, UI, automation gates)
// honor them. All defaults are conservative — a loader that doesn't know the
// flag value should leave it at its default.
struct ParamFlags {
    bool automatable = true;   // Host may drive this via connect_automation.
    bool read_only   = false;  // Plugin reports it; host must not write.
    bool hidden      = false;  // Don't show in default UIs (developer/internal).
    bool stepped     = false;  // Discrete int-valued (max_value - min_value + 1 steps).
    bool is_bypass   = false;  // Plugin's bypass param (special-cased by host).
    bool rampable    = true;   // Plugin handles per-block linear interpolation.
    bool modulatable = true;   // Plugin accepts per-voice modulation events
                               // (CLAP MOD, etc.) — distinct from automation.
};

struct HostParamInfo {
    uint32_t id = 0;
    std::string name;
    std::string unit;
    // Plain (not normalized) parameter range. PluginSlot::get_parameter() and
    // set_parameter() operate in this domain; loaders that natively normalize
    // (VST3) do the conversion internally.
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    ParamFlags flags;
};

// ── Plugin Slot ─────────────────────────────────────────────────────────

class PluginSlot {
public:
    virtual ~PluginSlot() = default;

    // Load a plugin from a PluginInfo descriptor.
    // Returns nullptr if the plugin can't be loaded.
    static std::unique_ptr<PluginSlot> load(const PluginInfo& info);

    // Plugin metadata
    virtual const PluginInfo& info() const = 0;
    virtual bool is_loaded() const = 0;

    // Lifecycle
    virtual bool prepare(double sample_rate, int max_block_size) = 0;
    virtual void release() = 0;

    // Audio processing.
    //
    // param_events carries sample-accurate parameter changes to deliver to the
    // plugin during this block (sorted by sample_offset). Loaders translate
    // into the plugin's native event stream. A loader that doesn't yet support
    // per-block parameter events may ignore the queue; callers will then see
    // the most recent value set via set_parameter().
    virtual void process(audio::BufferView<float>& output,
                         const audio::BufferView<const float>& input,
                         const midi::MidiBuffer& midi_in,
                         midi::MidiBuffer& midi_out,
                         const ParameterEventQueue& param_events,
                         int num_samples) = 0;

    // Parameters
    virtual std::vector<HostParamInfo> parameters() const = 0;
    virtual float get_parameter(uint32_t id) const = 0;
    virtual void set_parameter(uint32_t id, float normalized_value) = 0;

    // Bypass
    virtual void set_bypass(bool bypassed) = 0;
    virtual bool is_bypassed() const = 0;

    // State (preset save/load)
    virtual std::vector<uint8_t> save_state() const = 0;
    virtual bool restore_state(const std::vector<uint8_t>& data) = 0;

    // Editor
    virtual bool has_editor() const = 0;
    virtual void* create_editor_view() = 0;  // Returns platform-native view handle
    virtual void destroy_editor_view() = 0;

    // Latency
    virtual int latency_samples() const = 0;
    virtual int tail_samples() const = 0;
};

} // namespace pulp::host
