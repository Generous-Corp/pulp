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
#include <pulp/host/extensions_visitor.hpp>
#include <pulp/runtime/node_abi.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/state/parameter.hpp>
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
    state::ParamRate rate = state::ParamRate::ControlRate;
};

using ParamRate = state::ParamRate;

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

    // Editor handle API.
    //
    // `create_hosted_editor()` is the typed surface for embedding native
    // plugin editors. `pulp::view::WindowHost::attach_native_child_view` is
    // the host-side bridge and `pulp::view::EditorAttachment` wires the two
    // together. Slots may still override these void* entry points when they
    // have not moved to the typed surface; the default `create_hosted_editor()`
    // fallback keeps those subclasses working.
    virtual bool has_editor() const = 0;
    [[deprecated("Override create_hosted_editor() instead — see pulp::view::EditorAttachment "
                 "for typed hosted editor attachment.")]]
    virtual void* create_editor_view() = 0;  // Returns platform-native view handle
    [[deprecated("Override destroy_hosted_editor() instead — see pulp::view::EditorAttachment "
                 "for typed hosted editor attachment.")]]
    virtual void destroy_editor_view() = 0;

    // Hosted editor.
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
    // Format-specific editor surfaces:
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
    /// void* path so every slot already compiled against
    /// create_editor_view() still works; new slots override this directly.
    virtual std::unique_ptr<HostedEditor>
    create_hosted_editor(void* /*parent_window*/) {
        if (!has_editor()) return nullptr;
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        void* h = create_editor_view();
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
        if (!h) return nullptr;
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = h;
        return ed;
    }

    /// Tear down a hosted editor. Default routes to destroy_editor_view().
    virtual void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) {
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (ed) destroy_editor_view();
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    }

    // Latency
    virtual int latency_samples() const = 0;
    virtual int tail_samples() const = 0;

    /// Whether `latency_samples()` is a fact from the plugin, or a placeholder.
    /// See `latency_query()`, which is declared at the end of the class so the
    /// vtable stays append-only.
    enum class LatencyQuery {
        Available,    ///< The value is the plugin's own report. Zero means zero.
        Unsupported,  ///< This backend cannot ask. The value is meaningless.
        QueryFailed,  ///< The backend asked and the plugin errored.
    };

    // Extensions visitor.
    //
    // Typed plugin introspection: subclass ExtensionsVisitor, override
    // the visit_* methods you care about, then call
    //   slot.accept(visitor)
    // The slot dispatches to the matching visit_* overload (visit_clap,
    // visit_vst3, ...) with a populated handle struct. The default
    // implementation here calls visit_unknown so placeholder/unresolved
    // slots and slots whose format adapter was not compiled in degrade
    // gracefully without leaking a stale handle.
    virtual void accept(ExtensionsVisitor& visitor) const {
        visitor.visit_unknown(*this, ExtensionFormat::Unknown);
    }

    // Additive host-side multi-bus processing entry point. Keep appended to
    // preserve the existing PluginSlot virtual ordering.
    //
    // The default projection preserves the original PluginSlot contract by
    // selecting the active main output and optional main input from
    // ProcessBuffers, then calling the original main-in/main-out process()
    // callback. Slot implementations that need direct access to sidechain,
    // aux, surround, or multi-output host buses can override this method.
    virtual void process(format::ProcessBuffers& audio,
                         const midi::MidiBuffer& midi_in,
                         midi::MidiBuffer& midi_out,
                         const ParameterEventQueue& param_events,
                         int num_samples) {
        auto* output = audio.main_output();
        audio::BufferView<float> empty_output;
        audio::BufferView<const float> empty_input;
        auto* input = audio.main_input();
        process(output ? *output : empty_output, input ? *input : empty_input, midi_in, midi_out,
                param_events, num_samples);
    }

    // ── Opt-in host transport (additive) ────────────────────────────────────
    //
    // A slot whose output depends on the host timeline (playhead, tempo, loop,
    // process-mode) opts in by overriding wants_transport() to return true and
    // overriding the transport-carrying process() overload below to read the
    // supplied ProcessContext. A slot that ignores transport leaves both at
    // their defaults and is byte-for-byte unchanged from before this seam.
    //
    // INVARIANT (prepare-stable): wants_transport() is resolved ONCE when the
    // owning graph compiles its snapshot (cached into the routed binding and the
    // anticipation-eligibility analysis from the SAME read), never re-polled per
    // block on the audio thread. A slot must therefore settle its transport
    // capability before prepare(); a slot that changes the value later requires
    // a re-prepare for the graph to observe the new value. The cached bit is
    // also the precondition that lets a transport-sensitive slot coexist with
    // anticipative rendering: such a node is excluded from the ahead-rendered
    // interior so it always runs live and can safely observe transport.
    virtual bool wants_transport() const { return false; }

    // Transport-carrying multi-bus process entry point. Appended last to
    // preserve the existing PluginSlot virtual ordering. The default forwards to
    // the transport-less overload above, so a slot that does not override it is
    // unaffected. Only invoked by the host when wants_transport() returned true
    // and a transport is available for the block; otherwise the host calls the
    // transport-less overload.
    virtual void process(format::ProcessBuffers& audio,
                         const midi::MidiBuffer& midi_in,
                         midi::MidiBuffer& midi_out,
                         const ParameterEventQueue& param_events,
                         int num_samples,
                         const format::ProcessContext& /*transport*/) {
        process(audio, midi_in, midi_out, param_events, num_samples);
    }

    /// Whether `latency_samples()` is a fact from the plugin, or a placeholder.
    /// Appended last to preserve the existing PluginSlot virtual ordering.
    ///
    /// `latency_samples()` returns an int either way, so on its own it cannot
    /// distinguish "this plugin reports zero latency" from "this backend has no
    /// way to ask." Those are completely different claims, and collapsing them
    /// turns an unanswered question into a confident zero — which a latency
    /// proof would then happily certify. Ask this before trusting the number.
    ///
    /// Defaults to `Available`: the CLAP, VST3, and AU backends all read a real
    /// value from the plugin. The LV2 backend overrides it, because Pulp's
    /// hosted LV2 slot cannot currently read the plugin's latency port.
    virtual LatencyQuery latency_query() const { return LatencyQuery::Available; }
};

} // namespace pulp::host
