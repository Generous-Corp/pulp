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
#include <pulp/host/native_handle_visitor.hpp>
#include <pulp/runtime/node_abi.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <functional>
#include <cstdint>

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
    bool is_bypass   = false;  // This parameter IS the plugin's bypass control.
                               // Only a format whose bypass lives in the
                               // parameter list can set it — see BypassSurface
                               // for the formats that carry bypass elsewhere.
    bool rampable    = true;   // Plugin handles per-block linear interpolation.
    bool modulatable = true;   // Plugin accepts per-voice modulation events
                               // (CLAP MOD, etc.) — distinct from automation.
};

// Where a loaded plugin keeps its own bypass control.
//
// `ParamFlags::is_bypass` can only describe a bypass that IS a parameter.
// VST3 (`kIsBypass`) and CLAP (`CLAP_PARAM_IS_BYPASS`) both are, so for those
// formats the flagged `parameters()` entry is the whole story. AU keeps bypass
// off the parameter list entirely, as the unit property
// `kAudioUnitProperty_BypassEffect`; `AudioUnitParameterOptions` carries no
// bypass bit at all, so every AU parameter reports `is_bypass == false` and the
// flag alone cannot answer "does this plugin have a bypass". This enum answers
// that for every format, so a caller never has to guess from parameter names.
//
// Guessing is not a theoretical hazard: Apple's stock AUNBandEQ publishes eight
// parameters named exactly "Bypass", boolean over [0, 1] — per-band controls,
// none of them the unit bypass — so a name/range heuristic reports eight bypass
// parameters for a plugin that has none.
enum class BypassSurface {
    none,           // No bypass control is exposed. PluginSlot::set_bypass() is
                    // emulated entirely host-side.
    parameter,      // A parameter carries bypass: the `parameters()` entry whose
                    // `ParamFlags::is_bypass` is set.
    unit_property,  // A plugin-wide control outside the parameter list carries
                    // bypass (AU `kAudioUnitProperty_BypassEffect`). No parameter
                    // reports `is_bypass`, and none stands in for it.
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

struct PluginLoadResult;

// ── Host identity and per-slot host settings ────────────────────────────

/// How Pulp introduces itself to the plugins it hosts.
///
/// Process-wide, because a plugin may ask during its own initialization, before
/// any slot exists to configure: CLAP reads `clap_host::name/vendor/url/version`
/// and VST3 calls `IHostApplication::getName`. Set it once at startup, before
/// loading plugins; slots that are already loaded keep what they were given.
/// VST3 carries only the name.
struct PluginHostIdentity {
    std::string name = "Pulp";
    std::string vendor = "Pulp";
    std::string url = "https://github.com/Generous-Corp/pulp";
    std::string version = "1";
};

void set_plugin_host_identity(PluginHostIdentity identity);
PluginHostIdentity plugin_host_identity();

/// Per-slot processing settings. Control thread only, and they take effect at
/// the next prepare(): VST3 fixes its process mode and sample size in
/// `setupProcessing`, and CLAP's render mode is a main-thread call.
struct PluginHostSettings {
    /// `Offline` tells the plugin it is rendering faster or slower than wall
    /// clock (VST3 `kOffline`, CLAP `CLAP_RENDER_OFFLINE`), so it may pick its
    /// higher-quality path. A CLAP plugin that declares a hard realtime
    /// requirement stays in realtime mode.
    format::ProcessMode process_mode = format::ProcessMode::Realtime;
    /// Process in 64-bit floating point when the plugin supports it (VST3
    /// `kSample64`, CLAP `CLAP_AUDIO_PORT_SUPPORTS_64BITS` on every port). The
    /// host buffers stay 32-bit; the slot converts through prepared scratch.
    bool prefer_double_precision = false;
    /// Hand the plugin the host transport (tempo, position, play/record/loop
    /// state). Turning this off makes the slot transport-insensitive, which keeps
    /// it eligible for anticipative rendering; the plugin then sees a
    /// free-running clock with no tempo.
    bool deliver_transport = true;
};

/// A parameter change the PLUGIN made: an edit in its own editor, or a value it
/// emitted from its process call. Gestures bracket a user drag so an automation
/// recorder can tell a touch from a single jump.
struct PluginParameterEdit {
    enum class Kind : std::uint8_t { begin_gesture, value, end_gesture };
    Kind kind = Kind::value;
    std::uint32_t id = 0;
    /// Plain (not normalized) value; meaningful for `Kind::value`.
    float plain_value = 0.0f;
};

// ── Plugin Slot ─────────────────────────────────────────────────────────

class PluginSlot {
  public:
    // Result of the most recent audio block. A slot reports this through an
    // audio-thread-safe latch so the graph executor can contain a plugin that
    // rejects a block without changing the existing void process ABI. The
    // status is diagnostic/executor control only; each format retains its
    // explicit output fallback policy.
    enum class ProcessStatus : std::uint8_t { ok, failed };

    virtual ~PluginSlot() = default;

    // Load a plugin from a PluginInfo descriptor.
    // Returns nullptr if the plugin can't be loaded.
    static std::unique_ptr<PluginSlot> load(const PluginInfo& info);

    /// Explicit strict CLAP selection. Legacy load() keeps best-effort behavior.
    static PluginLoadResult load_exact(const PluginSelection& selection);

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

    // Bypass.
    //
    // `set_bypass()` is a HOST-side control and its output guarantee is the
    // same for every format: while bypassed the slot passes input through
    // unchanged, whether or not the plugin has a bypass of its own. Loaders
    // whose plugin exposes a native bypass also mirror the state onto it, so a
    // hosted plugin's own bypass state (and its editor) tracks the host's.
    // Control thread, not the audio thread: mirroring reaches the plugin
    // through its non-realtime property/parameter API.
    virtual void set_bypass(bool bypassed) = 0;
    virtual bool is_bypassed() const = 0;

    // Where this plugin keeps its own bypass control is reported by
    // bypass_surface(), declared at the end of the class.

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
    //   AU     — kAudioUnitProperty_CocoaUI on the v2 AudioUnit handle. Both
    //            AudioUnit and AudioUnitV3 load through the v2 C API
    //            (AudioComponentInstanceNew), so there is no AUAudioUnit object
    //            here and no requestViewControllerWithCompletionHandler.
    //   LV2    — ui: extension + shared-object UI
    //
    // CLAP and VST3 CONSUME a parent view rather than returning one, so those
    // slots hand back a host-owned container as `native_handle`. See
    // hosted_editor_container.hpp.
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

    // Native-handle visitor.
    //
    // Typed plugin introspection: subclass NativeHandleVisitor, override
    // the visit_* methods you care about, then call
    //   slot.accept(visitor)
    // The slot dispatches to the matching visit_* overload (visit_clap,
    // visit_vst3, ...) with a populated handle struct. The default
    // implementation here calls visit_unknown so placeholder/unresolved
    // slots and slots whose format adapter was not compiled in degrade
    // gracefully without leaking a stale handle.
    virtual void accept(NativeHandleVisitor& visitor) const {
        visitor.visit_unknown(*this, NativeHandleFormat::Unknown);
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

    // Editor resize negotiation. Both appended last to preserve the existing
    // PluginSlot virtual ordering; the defaults make every slot that does not
    // override them behave exactly as before.
    //
    // Main thread only, and only meaningful between create_hosted_editor() and
    // destroy_hosted_editor().

    /// Called when the plugin asks the host to resize its editor. Return true
    /// if the host honored the request. Refusing is legal in every format —
    /// the plugin must cope with a denial.
    using EditorResizeRequestHandler = std::function<bool(uint32_t width, uint32_t height)>;

    /// Install the handler invoked on a plugin-initiated resize request. Pass a
    /// null handler to clear it. The handler must outlive the open editor.
    /// Slots that cannot report resize requests ignore this.
    virtual void set_editor_resize_request_handler(EditorResizeRequestHandler /*handler*/) {}

    /// Host-initiated resize: ask the plugin to adopt (width, height). The
    /// plugin may snap to its own constraints, so the accepted size is written
    /// back through `width`/`height`. Returns false when the slot has no open
    /// editor, the format cannot resize, or the plugin refused.
    virtual bool set_hosted_editor_size(uint32_t& /*width*/, uint32_t& /*height*/) {
        return false;
    }

    /// Where this plugin keeps its own bypass control. A loader that cannot tell
    /// reports `none` rather than guessing; a wrapper slot reports what it wraps.
    /// Appended last to preserve the existing PluginSlot virtual ordering.
    virtual BypassSurface bypass_surface() const { return BypassSurface::none; }

    // Optional: the channel widths the caller will actually render, supplied
    // before prepare(). Keep appended to preserve the existing PluginSlot
    // virtual ordering.
    //
    // A format that negotiates its stream shape at initialize time (AU) cannot
    // infer the width from buffers handed to it later. Without this the AU is
    // initialized at its descriptor/default width and a render at any other
    // width is rejected as a malformed shape: output zeroed,
    // kAudioUnitRenderAction_OutputIsSilence set, status noErr -- a SILENT
    // render reported as success. Slots that take their width from the
    // per-block buffers (VST3, CLAP, LV2) need nothing and do not override it.
    virtual void set_preferred_channel_layout(int /*inputs*/, int /*outputs*/) {}

    /// Optional realtime DSP-history carry for a prepared replacement.  This
    /// is deliberately separate from save_state()/restore_state(): preset
    /// state describes user-facing parameters, while this opaque blob carries
    /// delay lines, tails, waveguide state, or physical-model integrators.
    /// The default is cold-start safe for every existing slot implementation.
    virtual std::string retained_history_key() const {
        return {};
    }
    virtual std::vector<uint8_t> serialize_dsp_state() const {
        return {};
    }
    virtual bool restore_dsp_state(const std::vector<uint8_t>&) {
        return false;
    }
    virtual bool reseed_dsp_state(std::uint64_t) {
        return false;
    }

    // Appended after the original PluginSlot virtual surface to preserve ABI
    // ordering for existing slot implementations. The graph executor calls
    // begin_process() before a routed block; adapters also clear their latch at
    // process() entry so direct slot callers observe the same per-block result.
    virtual void begin_process() noexcept {}

    // Status for the most recent process block. Implementations must provide an
    // allocation-free, nonblocking read suitable for the audio thread. The
    // status does not replace a format's output fallback policy.
    virtual ProcessStatus process_status() const noexcept {
        return ProcessStatus::ok;
    }

    /// Detached strict-load receipt; unavailable for legacy or unsupported loaders.
    virtual std::optional<PluginResolvedIdentity> resolved_identity() const {
        return std::nullopt;
    }

    // ── Host settings, plugin feedback, and restart requests ────────────────
    // Appended last to preserve the existing PluginSlot virtual ordering.

    /// Apply per-slot host settings; see PluginHostSettings. Control thread,
    /// effective at the next prepare(). Returns false when this slot cannot
    /// honor host settings at all (the default).
    virtual bool set_host_settings(const PluginHostSettings& /*settings*/) {
        return false;
    }
    virtual PluginHostSettings host_settings() const {
        return {};
    }

    /// Move parameter edits the plugin made since the last call into `out`, in
    /// the order they happened, and return how many were written. Control
    /// thread. Edits that arrive from the audio thread travel through a bounded
    /// lock-free queue; when it is full the newest edits are dropped and counted
    /// in `dropped_parameter_edit_count()`.
    virtual std::size_t drain_parameter_edits(std::span<PluginParameterEdit> /*out*/) {
        return 0;
    }
    virtual std::uint64_t dropped_parameter_edit_count() const noexcept {
        return 0;
    }

    /// Monotonic count of plugin requests that only a re-prepare can honor: a
    /// latency change (VST3 `restartComponent(kLatencyChanged)`, CLAP
    /// `clap_host_latency::changed`), an I/O change, or an explicit restart
    /// request. Safe from any thread. A host compares it against the value it
    /// saw at prepare(); SignalGraph does so in `plugin_restart_pending()` and
    /// rebuilds its latency compensation in `apply_plugin_restarts()`.
    virtual std::uint64_t restart_request_count() const noexcept {
        return 0;
    }

    /// Monotonic count of plugin notices that its saved state changed in a way
    /// no parameter edit describes (CLAP `clap_host_state::mark_dirty`, VST3
    /// `IComponentHandler2::setDirty(true)`). A host uses it to mark a session
    /// as needing a save. Safe from any thread.
    virtual std::uint64_t state_change_count() const noexcept {
        return 0;
    }
};

/// Exactly one loaded slot or typed refusal. A successful slot owns its receipt.
struct PluginLoadResult {
    std::variant<std::unique_ptr<PluginSlot>, PluginIdentityError> value;
};

} // namespace pulp::host
