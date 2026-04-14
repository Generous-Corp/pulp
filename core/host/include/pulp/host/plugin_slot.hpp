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

    // Editor — legacy void* handle API. Kept for slots that still need it
    // while slices migrate to the richer HostedEditor API below.
    virtual bool has_editor() const = 0;
    virtual void* create_editor_view() = 0;  // Returns platform-native view handle
    virtual void destroy_editor_view() = 0;

    // ── Hosted editor (workstream 03 slice 3.4) ────────────────────────
    //
    // Typed replacement for the void* editor API. Slot implementations
    // fill in a platform-native parent-window handle, initial size, and
    // resizable-ness so the host can lay the view out without guessing.
    //
    // Lifecycle:
    //   auto ed = slot->create_hosted_editor(parent_window);
    //   if (ed) { /* host attaches ed->native_handle into its window */ }
    //   // ... on close ...
    //   slot->destroy_hosted_editor(std::move(ed));
    //
    // Format wiring per slice (6.3..6.5 / 3.4 follow-ups):
    //   CLAP   — clap_plugin_gui: create / set_parent / show / get_size
    //   VST3   — IEditController::createView("editor") + IPlugView::attached
    //   AU     — AUAudioUnit.requestViewControllerWithCompletionHandler
    //   LV2    — ui: extension + shared-object UI
    struct HostedEditor {
        void* native_handle = nullptr;   ///< NSView*, HWND, GdkWindow*, clap_plugin_gui* etc.
        uint32_t width = 0;
        uint32_t height = 0;
        bool resizable = false;
    };

    /// Create the hosted editor. Default implementation preserves the
    /// legacy void* path so every slot already compiled against
    /// create_editor_view() still works; new slots override this directly.
    virtual std::unique_ptr<HostedEditor>
    create_hosted_editor(void* /*parent_window*/) {
        if (!has_editor()) return nullptr;
        void* h = create_editor_view();
        if (!h) return nullptr;
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = h;
        return ed;
    }

    /// Tear down a hosted editor. Default routes to destroy_editor_view().
    virtual void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) {
        if (ed) destroy_editor_view();
    }

    // Latency
    virtual int latency_samples() const = 0;
    virtual int tail_samples() const = 0;
};

} // namespace pulp::host
