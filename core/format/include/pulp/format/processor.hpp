#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/runtime/node_abi.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>
#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/format/prepare_resources.hpp>
#include <pulp/format/process_context.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::view {
class ScriptedUiSession;
class View;
}

namespace pulp::format {

namespace detail {

/// Backing store for the editor→host resize handler (see
/// `Processor::request_editor_resize`). Kept in a side table keyed by the
/// editor's `this` pointer (as an opaque `const void*`) rather than as a
/// `Processor` DATA MEMBER on purpose: `Processor` is a widely-inherited public
/// base, and growing `sizeof(Processor)` breaks every prebuilt library that
/// already constructs a subclass (it crashed `~Processor()` on a mixed
/// old-lib/new-header SDK). A side table keeps the class layout — and therefore
/// the ABI — frozen while still adding the capability. The key is `const void*`
/// (not `const Processor*`) so this block needs no forward declaration of
/// `Processor`, which would otherwise confuse the node-ABI gate's class parser.
/// Access is main-thread in practice; the mutex only guards a stray off-thread
/// teardown. One instance across TUs (inline-function local statics are merged).
inline std::mutex& editor_resize_mutex() {
    static std::mutex m;
    return m;
}
inline std::unordered_map<const void*,
                          std::function<bool(uint32_t, uint32_t)>>&
editor_resize_handlers() {
    static std::unordered_map<const void*,
                              std::function<bool(uint32_t, uint32_t)>>
        table;
    return table;
}

}  // namespace detail

/// The plugin processor interface.
///
/// This is the central abstraction in Pulp. Plugin developers subclass
/// Processor and implement four methods:
///
/// - descriptor() — returns plugin metadata
/// - define_parameters() — registers parameters with the state store
/// - prepare() — allocates resources at the given sample rate
/// - process() — real-time audio callback
///
/// Format adapters (VST3, AU, CLAP) wrap a Processor instance and
/// translate between the host API and Pulp's interface. The developer
/// writes one processor; the build system creates multiple format targets.
///
/// @code
/// class MyGain : public pulp::format::Processor {
///     PluginDescriptor descriptor() const override { ... }
///     void define_parameters(state::StateStore& store) override { ... }
///     void prepare(const PrepareContext& ctx) override { ... }
///     void process(BufferView<float>& out, const BufferView<const float>& in,
///                  MidiBuffer& midi_in, MidiBuffer& midi_out,
///                  const ProcessContext& ctx) override { ... }
/// };
/// @endcode
///
/// ## Thread Safety
///
/// - process() is called on the **audio thread**. No allocation, no locks,
///   no exceptions, no I/O.
/// - prepare() and release() are called on the **host thread** with the
///   audio thread stopped.
/// - define_parameters() is called once during construction on the host thread.
class Processor {
public:
    virtual ~Processor() {
        // Belt-and-suspenders: adapters clear the editor-resize handler on
        // editor close, but a Processor destroyed while a handler is still
        // installed (abnormal teardown, no polite editor close) would leak a
        // side-table entry keyed by `this` — and since allocators reuse
        // addresses, a later Processor at the same address could fire a stale
        // handler into a freed editor host. Erasing here makes the lifetime
        // structural rather than convention-dependent. No-op when already
        // cleared. Cannot be `= default` for this reason.
        std::lock_guard<std::mutex> lock(detail::editor_resize_mutex());
        detail::editor_resize_handlers().erase(this);
    }

    /// Return the plugin's metadata. Called once during initialization.
    virtual PluginDescriptor descriptor() const = 0;

    /// Register parameters with the state store.
    /// Called once during construction. Add all automatable parameters here.
    virtual void define_parameters(state::StateStore& store) = 0;

    /// Prepare for processing. Allocate buffers, initialize filters.
    /// Called on the host thread with the audio thread stopped.
    virtual void prepare(const PrepareContext& context) = 0;

    /// Release resources. Called on the host thread with audio stopped.
    virtual void release() {}

    /// Pause processing for a heavy main-thread operation (preset load,
    /// convolution kernel reallocation, sample-rate change) that would
    /// otherwise need to either block in @c process() — fatal — or run
    /// in lock-step with @c process(), which is hard to get right. The
    /// host calls @c suspend() before the heavy op and @c resume()
    /// after; while suspended, @c process() is expected to output
    /// silence and skip the heavy state.
    ///
    /// Default no-op so the contract is opt-in: plug-ins that don't
    /// need it pay nothing. Plug-ins that override @c suspend() should
    /// flush their voices / set an internal "suspended" flag, and
    /// override @c resume() to clear it; @c process() then early-
    /// returns or zero-fills while the flag is set.
    ///
    /// Threading: both hooks run on the host / main thread, never
    /// from @c process(). Format adapters do not currently call these
    /// hooks automatically. Adapter integration stays additive once the
    /// canonical "suspend-then-load-preset" surface for each format is
    /// settled. Today the hooks exist so a plug-in can wire its own
    /// UI-thread "loading..." workflow against them.
    ///
    /// Standard suspend/resume guard: while suspended a processor should stop
    /// touching shared real-time state so a host can safely reconfigure it.
    virtual void suspend() {}

    /// Resume processing after a prior @c suspend(). Default no-op;
    /// the symmetric counterpart of @c suspend() above.
    virtual void resume() {}

    /// Serialize plugin-owned state that is not part of StateStore.
    ///
    /// Use this for opaque state that must survive host/session recall but
    /// should not be exposed as flat automatable parameters. The returned
    /// bytes travel alongside StateStore through the format adapters'
    /// save/load paths.
    ///
    /// Called on a host/main thread, never from process().
    virtual std::vector<uint8_t> serialize_plugin_state() const { return {}; }

    /// Publish an immutable custom-state snapshot from a non-audio thread.
    /// Once a snapshot has been published, host save callbacks copy it instead
    /// of invoking serialize_plugin_state(). This keeps large sampler,
    /// wavetable, and analysis payload serialization out of host-controlled
    /// save timing while preserving serialize_plugin_state() as a compatibility
    /// fallback for plugins that have not opted in.
    void publish_plugin_state_snapshot(std::vector<uint8_t> bytes) {
        auto snapshot = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        std::atomic_store_explicit(&published_plugin_state_, std::move(snapshot),
                                   std::memory_order_release);
    }

    void publish_plugin_state_snapshot(std::span<const uint8_t> bytes) {
        publish_plugin_state_snapshot(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }

    /// Returns null until the plugin opts into published snapshots. A non-null
    /// empty vector is meaningful: the plugin explicitly published no custom
    /// payload and serialize_plugin_state() must not be called.
    std::shared_ptr<const std::vector<uint8_t>> published_plugin_state_snapshot() const {
        return std::atomic_load_explicit(&published_plugin_state_,
                                         std::memory_order_acquire);
    }

    /// Restore plugin-owned state previously returned by
    /// serialize_plugin_state().
    ///
    /// An empty span means the host blob carried no plugin-owned payload
    /// (legacy state or a processor that saved only StateStore data). Plugins
    /// that override this hook should treat empty input as "reset persisted
    /// plugin-owned state to defaults". Return false to reject malformed or
    /// incompatible payloads.
    ///
    /// Called on a host/main thread with the audio thread stopped.
    virtual bool deserialize_plugin_state(std::span<const uint8_t>) { return true; }

    /// Memory-pressure levels a host can surface to a plugin. Mirrors the
    /// broad shape of iOS didReceiveMemoryWarning + Windows low-memory
    /// notifications + Android TrimMemory.
    enum class MemoryPressure {
        /// Hint only — trim obviously disposable caches, keep working set.
        Advisory,
        /// Serious — drop every cache the plugin can rebuild on demand
        /// (image atlases, analysis buffers, undo history beyond the last
        /// N entries). Audio rendering must continue.
        Critical,
    };

    /// Called on the main/UI thread when the host observes memory
    /// pressure. Default is a no-op — plugins that cache decoded images,
    /// analysis buffers, or paged samples override this to drop caches.
    /// Implementations MUST NOT block the audio thread; use the existing
    /// host-thread rules guidance in docs/reference/host-thread-rules.md
    /// for cache invalidation.
    ///
    /// Wiring:
    ///   iOS       — the platform layer receives didReceiveMemoryWarning;
    ///               routing that signal into processors is host-owned.
    ///   macOS     — the platform layer observes DISPATCH_MEMORYPRESSURE;
    ///               routing that signal into processors is host-owned.
    ///   Android   — the platform layer receives ComponentCallbacks2.onTrimMemory;
    ///               routing that signal into processors is host-owned.
    ///   Windows   — the platform layer samples GlobalMemoryStatusEx(); routing
    ///               that signal into processors is host-owned.
    virtual void on_memory_pressure(MemoryPressure /*level*/) {}

    /// Latency in samples introduced by this processor (default 0).
    /// Override for plugins that buffer or lookahead (e.g., compressors,
    /// linear-phase EQs). Hosts use this for delay compensation.
    virtual int latency_samples() const { return 0; }

    /// ── Opt-in DSP-state carry across a hot-reload (reload lane, item 1.6) ──
    /// On a hot-reload the new processor binds to the live StateStore (params +
    /// values preserved), but its DSP-internal state — delay lines, filter
    /// history, reverb tails, oscillator phase — starts COLD. Override this pair
    /// to carry that state so a delay's repeats or an LFO's phase survive the
    /// swap seamlessly instead of resetting (the crossfade masks a cold start,
    /// but the tail itself is otherwise lost).
    ///
    /// Contract:
    ///   * `serialize_dsp_state()` returns an opaque blob of the OLD processor's
    ///     DSP state (empty = "nothing to carry"; the default).
    ///   * `restore_dsp_state(blob)` loads it into the NEW processor and returns
    ///     true on success, false if the blob is unrecognized/incompatible.
    ///   * The format is the PLUGIN's own private concern. It is only ever read
    ///     back by the same plugin source across a hot-reload — the reload lane's
    ///     build-fingerprint + parameter-contract gates guarantee the two sides
    ///     are the same build, so a naive layout is safe (no versioning needed
    ///     for the hot-reload case; version it if you also persist it elsewhere).
    ///   * COLD-START FALLBACK IS ALWAYS SAFE: an empty blob or a `false` return
    ///     just leaves the new processor freshly prepared — it must NEVER fail
    ///     the swap. Called by ProcessorHotSwapSlot::swap() under its writer lock
    ///     (no audio reader is inside the old processor), so both calls run
    ///     off the audio thread; keep the blob bounded (it is copied while the
    ///     lock is held, which briefly stalls the audio thread into passthrough).
    ///
    /// NOTE: the two virtuals are DECLARED at the end of this class (after
    /// process(ProcessBuffers&)) — not here — to keep the Processor vtable
    /// additive-only (node_abi_gate): new virtuals must be appended after every
    /// pre-existing one. See the appended block below.

    /// Proposed bus layout passed to is_bus_layout_supported().
    ///
    /// Each entry's index matches the descriptor's input_buses /
    /// output_buses index, and each value is the number of channels the
    /// host is proposing for that bus. Sidechain buses appear at index 1
    /// (input side) when the descriptor declared one. Empty per-side
    /// vectors mean "the host did not propose buses on that side"
    /// (rare; treat as 'no opinion').
    ///
    /// Format adapters call this on the host thread before applying the
    /// layout. Returning false rejects the proposal and the adapter is
    /// expected to refuse the host's `setBusArrangements` / equivalent call.
    struct BusesLayout {
        std::vector<int> inputs;
        std::vector<int> outputs;
    };

    /// Validate a proposed bus layout. Default acceptance policy:
    ///
    ///   * Per-side bus count matches the descriptor.
    ///   * Each proposed channel count is in {1, 2} (mono / stereo).
    ///
    /// Override for plugins that need a tighter contract (e.g. an
    /// instrument that only renders stereo out, a sidechain compressor
    /// that requires sidechain channels == main channels, surround
    /// processors that accept >2 channels). The format adapter MUST
    /// call this on the host thread — never from process().
    ///
    /// Adapters fall back to the descriptor's declared bus count + the
    /// mono/stereo policy when a plugin doesn't override this hook,
    /// which preserves the prior default-acceptance behavior exactly.
    virtual bool is_bus_layout_supported(const BusesLayout& layout) const {
        const auto desc = descriptor();
        if (!desc.supported_bus_layouts.empty()) {
            return std::any_of(desc.supported_bus_layouts.begin(),
                               desc.supported_bus_layouts.end(),
                               [&](const auto& declared) {
                                   return declared.inputs == layout.inputs &&
                                          declared.outputs == layout.outputs;
                               });
        }
        if (!layout.inputs.empty() &&
            layout.inputs.size() != desc.input_buses.size())
            return false;
        if (!layout.outputs.empty() &&
            layout.outputs.size() != desc.output_buses.size())
            return false;
        auto channels_ok = [](int n) { return n == 1 || n == 2; };
        for (int n : layout.inputs)  if (!channels_ok(n)) return false;
        for (int n : layout.outputs) if (!channels_ok(n)) return false;
        return true;
    }

    /// Cross-adapter latency / tail change notifications.
    ///
    /// Called from `process()` on the audio thread when a plugin's
    /// latency or tail length changes mid-render (e.g. a linear-phase
    /// EQ flipping between FIR taps, a reverb extending its decay).
    /// The Processor sets an `std::atomic<bool>` pending-flag; the
    /// format adapter polls the flag on the host / main thread and
    /// pushes the notification to the host (`restartComponent` for
    /// VST3, `kAudioUnitProperty_LatencySamples` for AU,
    /// `clap_host_latency->changed()` for CLAP, `SetSignalLatency` for
    /// AAX).
    ///
    /// **Audio-thread-safe.** Never call a host API from `process()`.
    ///
    void flag_latency_changed() noexcept {
        latency_changed_.store(true, std::memory_order_release);
    }
    void flag_tail_changed() noexcept {
        tail_changed_.store(true, std::memory_order_release);
    }

    /// Adapter-side polling helper. Returns true exactly once per
    /// `flag_*_changed()` call. The adapter calls this on the
    /// host / main thread; on `true` it must republish the latest
    /// `latency_samples()` / `tail_samples` to the host.
    bool consume_latency_changed_flag() noexcept {
        return latency_changed_.exchange(false, std::memory_order_acq_rel);
    }
    bool consume_tail_changed_flag() noexcept {
        return tail_changed_.exchange(false, std::memory_order_acq_rel);
    }

    /// Non-mutating peek used by adapters that need to decide whether
    /// to ping the host for a main-thread callback without losing the
    /// pending edge (e.g. CLAP's `request_callback`). Does not clear
    /// the flag; the host-thread callback must still call
    /// `consume_*_changed_flag()` to drain it.
    bool latency_change_pending() const noexcept {
        return latency_changed_.load(std::memory_order_acquire);
    }
    bool tail_change_pending() const noexcept {
        return tail_changed_.load(std::memory_order_acquire);
    }

    /// Process one buffer of audio. Called on the real-time audio thread.
    ///
    /// @param audio_output  Output buffer to fill (main bus)
    /// @param audio_input   Input buffer to read (main bus)
    /// @param midi_in       Incoming MIDI events (sample-accurate)
    /// @param midi_out      Outgoing MIDI events (for MIDI effects)
    /// @param context       Transport state and timing
    ///
    /// For sidechain input, call sidechain_input() which returns nullptr
    /// if no sidechain is connected.
    virtual void process(
        audio::BufferView<float>& audio_output,
        const audio::BufferView<const float>& audio_input,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const ProcessContext& context) = 0;

    /// Editor support. By default, all processors have an auto-generated editor
    /// built from their parameter definitions (using AutoUi). Override these to
    /// customize or disable the editor.

    /// Whether this processor has a GUI editor. Default true (AutoUi).
    virtual bool has_editor() const { return true; }

    /// Preferred editor window size in logical pixels.
    virtual std::pair<uint32_t, uint32_t> editor_size() const { return {400, 300}; }

    /// Return the full view size hints (preferred/min/max).
    ///
    /// Resolution order (first non-zero wins):
    /// 1. `PULP_PLUGIN_DESIGN_W` / `_H` compile-defs injected by
    ///    `pulp_add_plugin(... DESIGN_WIDTH N DESIGN_HEIGHT N)`. When set,
    ///    min is derived as preferred * 2/3, max as preferred * 2, and
    ///    aspect_ratio as W/H — so CLAP's `gui_can_resize` (which requires
    ///    min > 0) works without per-plugin overrides. Explicit
    ///    `DESIGN_MIN_*` / `DESIGN_MAX_*` args override the derived values.
    /// 2. `editor_size()` with no min/max bounds (legacy default).
    ///
    /// Override this method when a plugin needs runtime-computed bounds
    /// (e.g., a dynamically generated UI). Most imported-design plugins
    /// should use the CMake args instead — see `import-design` skill.
    virtual ViewSize view_size() const {
#ifdef PULP_PLUGIN_DESIGN_W
        return view_size_from_design(
            PULP_PLUGIN_DESIGN_W, PULP_PLUGIN_DESIGN_H,
            PULP_PLUGIN_DESIGN_MIN_W, PULP_PLUGIN_DESIGN_MIN_H,
            PULP_PLUGIN_DESIGN_MAX_W, PULP_PLUGIN_DESIGN_MAX_H);
#else
        auto [w, h] = editor_size();
        return ViewSize{w, h, 0, 0, 0, 0};
#endif
    }

    /// Create a custom view for this processor. Default returns nullptr,
    /// which signals the framework to build the default editor (scripted UI
    /// if configured, otherwise AutoUi from registered parameters).
    ///
    /// Override to return a fully custom `view::View` tree. The returned
    /// view is owned by the `ViewBridge` and destroyed when the editor
    /// closes. This method may be called multiple times during the lifetime
    /// of the processor (one per attached editor window).
    ///
    virtual std::unique_ptr<view::View> create_view();

    /// A custom settings tab this plugin contributes to the host's Settings UI.
    /// (The matching virtual `settings_sections()` is appended at the end of this class
    /// to preserve additive-only vtable ordering.)
    struct SettingsSection {
        SettingsSection();
        SettingsSection(std::string title, std::unique_ptr<view::View> view);
        ~SettingsSection();
        SettingsSection(SettingsSection&&) noexcept;
        SettingsSection& operator=(SettingsSection&&) noexcept;
        SettingsSection(const SettingsSection&) = delete;
        SettingsSection& operator=(const SettingsSection&) = delete;

        std::string title;                 ///< Tab label, e.g. "Models".
        std::unique_ptr<view::View> view;  ///< Tab content (built by the plugin).
    };

    /// Called after a view has been constructed and attached. Runs on the
    /// host/UI thread. Safe to read state and register UI listeners.
    virtual void on_view_opened(view::View& /*view*/) {}

    /// Called immediately before a view is destroyed. Runs on the UI thread.
    /// Use to unregister listeners; do not assume the view is still usable
    /// for drawing after this returns.
    virtual void on_view_closed(view::View& /*view*/) {}

    /// Called when the host resizes the editor window. Dimensions are in
    /// logical pixels. Runs on the UI thread.
    virtual void on_view_resized(view::View& /*view*/, uint32_t /*w*/, uint32_t /*h*/) {}

    /// Editor-INITIATED host-window resize (the plugin↔host direction that
    /// `on_view_resized` does not cover). The editor calls this to ask the
    /// DAW/standalone host to resize the plugin window to (w × h) LOGICAL
    /// pixels AND to re-pin the design viewport + aspect ratio to the same
    /// size, so the content fills the new window with no letterbox / dark
    /// fill and no squish. Use it when the editor's own natural size changes
    /// at runtime — e.g. a chrome-hiding "player" mode that wants a smaller,
    /// differently-shaped window than its full authoring layout.
    ///
    /// The format adapter installs the actual host call (CLAP
    /// `clap_host_gui->request_resize`, VST3 `IPlugFrame::resizeView`, AU
    /// `preferredContentSize`, standalone `WindowHost::request_content_size`)
    /// via set_editor_resize_handler() when the editor opens, and clears it on
    /// close. Returns true when a handler is installed AND the host accepted
    /// the request; false when no editor is open, the host exposes no resize
    /// path, or the host refused (the editor should keep its current size).
    /// Main-thread / UI-thread only.
    ///
    /// The handler lives in a side table (detail::editor_resize_handlers),
    /// NOT a data member, so this capability adds nothing to `sizeof(Processor)`
    /// and stays ABI-compatible with prebuilt libraries.
    bool request_editor_resize(uint32_t width, uint32_t height) {
        std::function<bool(uint32_t, uint32_t)> handler;
        {
            std::lock_guard<std::mutex> lock(detail::editor_resize_mutex());
            auto& table = detail::editor_resize_handlers();
            auto it = table.find(this);
            if (it == table.end() || !it->second) return false;
            handler = it->second;  // copy so the host call runs unlocked
        }
        return handler(width, height);
    }

    /// Called when the host's transport state transitions between
    /// playing and stopped, or jumps to a new position. Default no-op.
    virtual void on_host_transport_changed(bool /*is_playing*/,
                                           double /*position_seconds*/) {}
    /// Called when the host's transport tempo changes. Default no-op.
    /// Override for plugins that care about tempo outside of a process()
    /// call — delay sync recomputation, tempo-synced LFO rate caches,
    /// UI BPM read-outs. Runs on the main/UI thread; the audio thread
    /// keeps reading the current tempo from ProcessContext as usual.
    virtual void on_host_tempo_changed(double /*new_tempo_bpm*/) {}

    /// Optional ARA 2.x document-controller factory.
    /// Plugins that opt in to ARA return a new AraDocumentController from
    /// this method; the format-adapter companion factory (VST3 / AU /
    /// CLAP) owns the instance and tears it down with the plugin.
    /// Default returns nullptr — the plugin is not ARA-aware.
    /// Forward-declared so plugin TUs that don't implement ARA don't
    /// need to pull `pulp/format/ara.hpp`.
    virtual std::unique_ptr<class AraDocumentController>
    create_ara_document_controller();

    /// Return the active scripted UI session when a custom create_view()
    /// path owns one. The default framework editor path is tracked by
    /// ViewBridge directly; processor-owned sessions use this hook so format
    /// adapters can still select scripted/GPU hosting and poll the session.
    virtual view::ScriptedUiSession* active_scripted_ui() { return nullptr; }
    virtual const view::ScriptedUiSession* active_scripted_ui() const { return nullptr; }

    /// Settings tabs this plugin contributes, composed by the host alongside its own
    /// host-owned tabs (e.g. Audio/MIDI device selection in the standalone). This keeps
    /// device selection a host concern — correct in a DAW, where the host owns the audio
    /// device — while letting a plugin surface its own settings (e.g. a model picker) in
    /// one unified Settings panel. Called when the settings UI is built; may be called
    /// again if it is rebuilt. (Appended last to preserve additive-only vtable ordering.)
    virtual std::vector<SettingsSection> settings_sections() { return {}; }

    /// Estimate storage the next prepare() will allocate or reserve.
    ///
    /// Default is unknown/zero for source compatibility. Processors with large
    /// prepared resources should override this so hosts and tests can reject
    /// oversized configurations before allocation. Called on the host thread,
    /// never from process(). Appended to preserve additive-only vtable ordering.
    virtual PrepareResourceUsage estimate_prepare_resources(
        const PrepareContext&) const {
        return {};
    }

    /// Check the processor's estimate against the host-supplied prepare limits.
    ///
    /// Returning `None` means the estimate fits every non-zero limit. Hosts that
    /// want fail-closed prepare behavior can call this before `prepare()`.
    /// Appended to preserve additive-only vtable ordering.
    virtual PrepareResourceLimit check_prepare_resource_limits(
        const PrepareContext& context) const {
        return first_exceeded_prepare_resource_limit(
            estimate_prepare_resources(context), context.resource_limits);
    }

    /// Additive multi-bus process entry point.
    ///
    /// The default implementation preserves the existing plugin-author
    /// contract: it projects the active main output, optional main input, and
    /// optional sidechain input from `ProcessBuffers`, then calls the original
    /// main-in/main-out `process()` callback. Plugins that need direct access
    /// to auxes, multi-output instruments, or surround buses can override this
    /// method while older processors continue to work unchanged. Appended to
    /// preserve additive-only vtable ordering.
    virtual void process(
        ProcessBuffers& audio,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const ProcessContext& context) {
        auto* output = audio.main_output();
        if (!output) return;

        audio::BufferView<const float> empty_input;
        auto* input = audio.main_input();
        auto* previous_sidechain = sidechain_;
        sidechain_ = audio.sidechain_input();
        process(*output, input ? *input : empty_input, midi_in, midi_out, context);
        sidechain_ = previous_sidechain;
    }

    /// Opt-in DSP-state carry across a hot-reload (reload lane, item 1.6).
    /// Documented in full next to `latency_samples()` above; DECLARED here (at the
    /// end of the vtable) so the Processor virtual order stays additive-only
    /// (node_abi_gate) after `is_bus_layout_supported`/`process` were frozen on
    /// main. serialize returns the OLD processor's opaque DSP-state blob (empty =
    /// nothing to carry); restore loads it into the NEW processor (false = cold
    /// start, always safe). Appended to preserve additive-only vtable ordering.
    virtual std::vector<std::byte> serialize_dsp_state() const { return {}; }
    virtual bool restore_dsp_state(const std::vector<std::byte>& /*blob*/) { return false; }

    /// Editor live-reload support (live-swap 1.9). A processor whose editor
    /// should rebuild IN PLACE while it is open — e.g. a `ReloadableShell` whose
    /// logic hot-swaps its `create_view()` — returns true here. The `ViewBridge`
    /// then hosts the editor under a STABLE root container so its content can be
    /// replaced live (the host keeps referencing the same root `View`) without
    /// the DAW re-instantiating the plugin. Default false: normal plugins are
    /// unaffected — no wrapper, no polling cost. Appended to preserve
    /// additive-only vtable ordering.
    virtual bool supports_editor_reload() const { return false; }

    /// Monotonic counter that increments each time this processor's editor
    /// content should be rebuilt (after a successful logic hot-swap). The editor
    /// idle tick polls this; when it changes, the `ViewBridge` rebuilds the
    /// primary view from `create_view()`. Only meaningful when
    /// `supports_editor_reload()` is true. Must be safe to call from the UI
    /// thread. Appended to preserve additive-only vtable ordering.
    virtual std::uint64_t editor_reload_generation() const { return 0; }

    /// Prepare scratch used by the default process_f64() fallback. Format
    /// adapters call this after the plugin's prepare() while audio is stopped, so
    /// a plugin that opts into f64 before overriding process_f64() can still run
    /// without allocating on the audio thread.
    void prepare_f64_fallback_scratch(const PrepareContext& context);

    /// Native double-precision process entry point.
    ///
    /// Default implementation preserves compatibility for plugins that opt into
    /// f64 before porting their DSP: it converts the double main input to the
    /// prepared f32 fallback scratch, calls the original f32 process(), then
    /// converts the f32 main output back to double. The fallback never allocates
    /// during process(); if the adapter did not prepare enough scratch, it emits
    /// silence rather than growing buffers on the audio thread.
    virtual void process_f64(
        audio::BufferView<double>& audio_output,
        const audio::BufferView<const double>& audio_input,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const ProcessContext& context);

    /// Additive double-precision richer-surface process entry point. The default
    /// routes simple main-bus blocks through process_f64() so processors that
    /// override the main-bus f64 virtual still run natively. For sidechain or aux
    /// blocks, it falls back through the f32 richer surface using prepared
    /// scratch so legacy sidechain/aux semantics are preserved without allocating
    /// on the audio thread.
    virtual void process_f64(
        ProcessBuffers64& audio,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const ProcessContext& context);

    /// Non-realtime maintenance tick. Default: no-op.
    ///
    /// A processor whose control changes need work that process() must never do
    /// — decode, resample, FFT-plan, allocate — normally does it on its own
    /// background thread. Some hosts give it no thread to run on:
    ///
    ///   * WAM v2 in the browser. The entire module lives inside an
    ///     AudioWorklet; there is no second thread and no `std::thread`.
    ///   * WebCLAP in the browser, likewise (its `on_main_thread` callback is
    ///     the closest thing to a control thread).
    ///
    /// Those adapters call this hook from a NON-RENDER context after anything
    /// that could have dirtied the processor's derived state — a parameter
    /// write, a state restore, a re-prepare. A processor that has a real worker
    /// thread should do nothing here, or the worker and the host would race.
    ///
    /// WHO ACTUALLY CALLS IT (the contract is narrower than "any host"):
    ///
    ///   * WAM: the adapter marks the processor dirty on a control write and
    ///     calls this ONCE per block, right after the render call — so a burst
    ///     of control messages (a knob drag delivers many in one turn) collapses
    ///     into a single pass over the LATEST values. Do not rely on one tick
    ///     per parameter write; rely on "eventually, with the latest values".
    ///   * CLAP — including, but NOT limited to, WebCLAP: `clap_on_main_thread`
    ///     and `state.load` call it unconditionally. So a NATIVE CLAP plugin
    ///     gets this hook too (harmlessly: a processor with a worker returns
    ///     immediately). Do not assume "this only fires in a browser".
    ///   * VST3, AU, and the standalone host do NOT call it at all. A processor
    ///     that relies on it for correctness must ALSO have a worker (or do the
    ///     work in prepare()), or it will never reconcile in those formats.
    ///
    /// It is NOT an audio-thread callback and is never called from inside
    /// process(). In a worklet-only host it does run on the same OS thread as
    /// the render callback (just not inside it), so a long tick can still make
    /// the next quantum late — keep the work bounded, and prefer work that is
    /// proportional to what actually changed.
    ///
    /// Appended to preserve additive-only vtable ordering (node_abi_gate).
    virtual void on_non_realtime_tick() {}

    /// RT-safe query: "I have non-realtime work waiting". Default false.
    ///
    /// Called from process() by adapters that can only reach the control thread
    /// by ASKING for it — CLAP (and therefore WebCLAP) delivers parameter
    /// changes as events inside process(), and the only way out is
    /// `clap_host->request_callback()` followed by `on_main_thread()`. The CLAP
    /// adapter peeks this flag each block, exactly as it already does for
    /// latency/tail changes, and requests the callback when it is set.
    ///
    /// MUST be realtime-safe: no locks, no allocation. Read atomics and compare.
    ///
    /// The WAM adapter also consults it once per block, so work a processor
    /// discovers for ITSELF (not just work a control write announced) still gets
    /// serviced there.
    ///
    /// Appended to preserve additive-only vtable ordering (node_abi_gate).
    virtual bool non_realtime_tick_pending() const { return false; }

    /// Access the parameter state store.
    /// Use state().get_value(id) to read parameter values in process().
    ///
    /// The store belongs to the host, not to the Processor, and the Processor may
    /// reach it for its whole lifetime — including from its destructor, and from
    /// any worker thread that destructor is about to join. A host must therefore
    /// keep the store alive until the Processor is gone: declare the store *before*
    /// the `unique_ptr<Processor>`, since members are destroyed in reverse. Every
    /// host in this directory does; a host that gets it backwards hands a running
    /// thread a freed store, which crashes on plug-in close and nowhere else.
    state::StateStore& state() { return *state_store_; }
    const state::StateStore& state() const { return *state_store_; }

    /// Access sidechain input buffer (set by format adapters before process).
    /// Returns nullptr if no sidechain is connected or the bus is inactive.
    const audio::BufferView<const float>* sidechain_input() const { return sidechain_; }

    /// Access the per-note MPE expression buffer for this block. Returns
    /// nullptr unless the plugin declared MPE via PluginDescriptor legacy
    /// flags or node capabilities and the host/format adapter populated it.
    const midi::MpeBuffer* mpe_input() const { return mpe_input_; }

    /// Access the MIDI 2.0 UMP buffer for this block. Returns nullptr
    /// unless the plugin declared UMP via PluginDescriptor legacy flags or
    /// node capabilities and the host/format adapter populated it.
    const midi::UmpBuffer* ump_input() const { return ump_input_; }

    /// Access sample-accurate parameter automation for this block. Returns
    /// nullptr when the current host path did not provide a parameter-event
    /// queue; an empty queue means the adapter participated but the block had
    /// no parameter events.
    const state::ParameterEventQueue* param_events() const { return param_events_; }

    /// Emit a sample-accurate parameter change to the host from `process()`.
    ///
    /// Call this when the plugin itself changes a parameter mid-block (e.g. an
    /// LFO-driven macro, a modeled control) and you want the host to record the
    /// change at its exact sample offset rather than as a single coalesced
    /// value at block start. `value` is in the plain parameter domain;
    /// `sample_offset` is relative to the start of the current block. The host
    /// adapter drains these events after `process()` and clamps the offset to
    /// the block length. Returns false when no output queue is wired (the
    /// current format/host path does not carry sample-accurate output) or the
    /// per-block queue is full — in either case the adapter's end-of-block
    /// snapshot still reports the parameter's final value, so no change is lost.
    ///
    /// Realtime-safe: pushes into a fixed-capacity queue, never allocates or
    /// locks. Only valid to call from within `process()`.
    bool push_output_param_event(state::ParamID id, float value,
                                 std::uint32_t sample_offset) noexcept {
        if (output_param_events_ == nullptr) return false;
        return output_param_events_->push(state::ParameterEvent{
            id, static_cast<int32_t>(sample_offset), value, 0});
    }

    /// Access the output parameter-event queue, if the adapter wired one.
    const state::ParameterEventQueue* output_param_events() const {
        return output_param_events_;
    }

    /// @internal Framework sets these during initialization / processing.
    void set_state_store(state::StateStore* store) { state_store_ = store; }
    /// @internal
    void set_sidechain(const audio::BufferView<const float>* sc) { sidechain_ = sc; }
    /// @internal Called by format adapters before process() when MPE is on.
    void set_mpe_input(const midi::MpeBuffer* mpe) { mpe_input_ = mpe; }
    /// @internal Called by format adapters before process() when UMP is on.
    void set_ump_input(const midi::UmpBuffer* ump) { ump_input_ = ump; }
    /// @internal Called by format adapters before process().
    void set_param_events(const state::ParameterEventQueue* events) { param_events_ = events; }
    /// @internal Called by format adapters before process() to receive
    /// sample-accurate output parameter events pushed via
    /// push_output_param_event(). The adapter owns the queue, clears it before
    /// each block, and drains it after process().
    void set_output_param_events(state::ParameterEventQueue* events) { output_param_events_ = events; }

    /// @internal Installed by the format adapter when the editor opens (and
    /// cleared with a null handler on close — MUST be cleared before the
    /// captured editor host / bridge is destroyed). The handler performs the
    /// host-specific window resize AND re-pins the design viewport / aspect to
    /// the requested (w, h). Stored in a side table keyed by `this` (see
    /// detail::editor_resize_handlers) so `Processor`'s layout is unchanged.
    void set_editor_resize_handler(
        std::function<bool(uint32_t, uint32_t)> handler) {
        std::lock_guard<std::mutex> lock(detail::editor_resize_mutex());
        auto& table = detail::editor_resize_handlers();
        if (handler) {
            table[this] = std::move(handler);
        } else {
            table.erase(this);
        }
    }

private:
    std::shared_ptr<const std::vector<uint8_t>> published_plugin_state_;
    static constexpr std::size_t kF64FallbackMaxBuses = 16;

    static std::size_t fallback_bus_channels(const std::vector<BusInfo>& buses,
                                             std::size_t index,
                                             std::size_t prepared_main_channels);

    static bool requires_rich_f64_fallback(const ProcessBuffers64& audio);

    bool f64_fallback_capacity_ok(std::size_t input_channels,
                                  std::size_t output_channels,
                                  std::size_t frames) const;

    bool f64_fallback_process_buffers_capacity_ok(
        const ProcessBuffers64& audio) const;

    static void copy_f64_to_f32(audio::BufferView<float> dst,
                                const audio::BufferView<const double>& src);

    static void copy_f32_to_f64(audio::BufferView<double> dst,
                                const audio::BufferView<const float>& src);

    static void clear_active_outputs(ProcessBuffers64& audio);

    void process_f64_via_f32_process_buffers(ProcessBuffers64& audio,
                                             midi::MidiBuffer& midi_in,
                                             midi::MidiBuffer& midi_out,
                                             const ProcessContext& context);

    state::StateStore* state_store_ = nullptr;
    const audio::BufferView<const float>* sidechain_ = nullptr;
    const midi::MpeBuffer* mpe_input_ = nullptr;
    const midi::UmpBuffer* ump_input_ = nullptr;
    const state::ParameterEventQueue* param_events_ = nullptr;
    state::ParameterEventQueue* output_param_events_ = nullptr;
    audio::Buffer<float> f64_fallback_input_scratch_;
    audio::Buffer<float> f64_fallback_output_scratch_;
    std::array<audio::Buffer<float>, kF64FallbackMaxBuses> f64_fallback_input_bus_scratch_{};
    std::array<audio::Buffer<float>, kF64FallbackMaxBuses> f64_fallback_output_bus_scratch_{};
    std::array<ProcessBusBufferView<const float>, kF64FallbackMaxBuses> f64_fallback_input_views_{};
    std::array<ProcessBusBufferView<float>, kF64FallbackMaxBuses> f64_fallback_output_views_{};
    // RT-safe pending flags published from process() and consumed by adapters
    // on the host / main thread.
    std::atomic<bool> latency_changed_{false};
    std::atomic<bool> tail_changed_{false};
};

/// Factory function type — plugins provide this to create processor instances.
using ProcessorFactory = std::unique_ptr<Processor>(*)();

} // namespace pulp::format
