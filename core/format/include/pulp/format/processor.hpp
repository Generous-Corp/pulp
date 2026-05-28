#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/runtime/node_abi.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::view { class ScriptedUiSession; }

namespace pulp::format {

/// Editor size hints (in logical pixels). preferred is used for the
/// initial window size; min/max bound interactive resizing. A zero
/// max dimension means unbounded in that axis.
struct ViewSize {
    uint32_t preferred_width = 400;
    uint32_t preferred_height = 300;
    uint32_t min_width = 0;
    uint32_t min_height = 0;
    uint32_t max_width = 0;   ///< 0 = unbounded
    uint32_t max_height = 0;  ///< 0 = unbounded

    /// When > 0, the host should hold this aspect ratio (width/height)
    /// during interactive resize. Workstream 07 slice 7.5 pairs this
    /// with pulp::view::ResizableShell, which owns the clamp + snap
    /// arithmetic. 0 means "any ratio"; hosts then let the user drag
    /// freely within [min, max]. Typical values: 16.0/9.0, 4.0/3.0,
    /// preferred_width/preferred_height.
    double aspect_ratio = 0.0;
};

/// Build a ViewSize from a design-import preferred size with sensible
/// derived bounds. Used by `Processor::view_size()`'s default when
/// `pulp_add_plugin(... DESIGN_WIDTH N DESIGN_HEIGHT N)` is set, and
/// callable directly by plugins that compute dimensions at runtime.
///
/// Derivation rules (only when the corresponding explicit value is 0):
///   - min = preferred * 2/3
///   - max = preferred * 2
///   - aspect_ratio = preferred_width / preferred_height
///
/// CLAP's `gui_can_resize` requires min > 0 (see clap_entry.hpp), so
/// the derived min is what makes corner-drag resize work across all
/// formats with no per-plugin override. Issue #2784 tracks the
/// auto-sidecar path that will populate the inputs from
/// `pulp import-design`.
constexpr ViewSize view_size_from_design(uint32_t preferred_width,
                                          uint32_t preferred_height,
                                          uint32_t min_width = 0,
                                          uint32_t min_height = 0,
                                          uint32_t max_width = 0,
                                          uint32_t max_height = 0) {
    return ViewSize{
        preferred_width,
        preferred_height,
        min_width > 0 ? min_width : (preferred_width * 2) / 3,
        min_height > 0 ? min_height : (preferred_height * 2) / 3,
        max_width > 0 ? max_width : preferred_width * 2,
        max_height > 0 ? max_height : preferred_height * 2,
        preferred_height > 0
            ? static_cast<double>(preferred_width) / static_cast<double>(preferred_height)
            : 0.0,
    };
}

/// Plugin category — determines bus layout expectations and DAW behavior.
enum class PluginCategory {
    Effect,      ///< Audio effect (takes input, produces output)
    Instrument,  ///< Synth/sampler (receives MIDI, produces audio)
    MidiEffect,  ///< MIDI processor (receives MIDI, produces MIDI)
};

/// Audio bus description — for multi-bus plugins (sidechain, aux, etc.)
///
/// Each bus has a name, default channel count, and whether it's optional.
/// Optional buses (like sidechains) can be deactivated by the host.
struct BusInfo {
    std::string name;
    int default_channels = 2;
    bool optional = false;  ///< true for sidechain buses that can be deactivated
};

/// Capability sidecar for the node ABI. New capability bits should be
/// appended here with false defaults so descriptor aggregate initializers
/// remain source-compatible.
struct NodeCapabilities {
    bool supports_mpe = false;
    bool supports_ump = false;
};

/// Plugin metadata — declared once, immutable.
///
/// Every Processor subclass returns a PluginDescriptor from descriptor().
/// Format adapters use this to register the plugin with the host.
///
/// @code
/// PluginDescriptor descriptor() const override {
///     return {
///         .name = "MyGain",
///         .manufacturer = "Example",
///         .bundle_id = "com.example.mygain",
///         .version = "1.0.0",
///         .category = PluginCategory::Effect,
///     };
/// }
/// @endcode
struct PluginDescriptor {
    std::string name;
    std::string manufacturer;
    std::string bundle_id;
    std::string version;       ///< Semantic version string, e.g. "1.0.0"
    PluginCategory category = PluginCategory::Effect;

    /// Bus configuration — defaults to single stereo in/out for compatibility.
    /// Override input_buses/output_buses for multi-bus (sidechain, aux).
    std::vector<BusInfo> input_buses = {{"Main In", 2, false}};
    std::vector<BusInfo> output_buses = {{"Main Out", 2, false}};

    bool accepts_midi = false;   ///< true if plugin receives MIDI input
    bool produces_midi = false;  ///< true if plugin sends MIDI output

    /// Opt in to MPE (MIDI Polyphonic Expression). When true, format
    /// adapters that recognise MPE will run the inbound MIDI stream
    /// through an MpeVoiceTracker, build an MpeBuffer for the block, and
    /// make it available via Processor::mpe_input() during process().
    /// The standard process() signature is unchanged; plugins that don't
    /// set this flag see no MPE-specific behaviour.
    bool supports_mpe = false;

    /// Opt in to the native MIDI 2.0 UMP sidecar. When true, format
    /// adapters that recognise UMP provide a UmpBuffer of full-resolution
    /// channel-voice packets (16-bit velocity, per-note pitch bend,
    /// per-note CCs) through Processor::ump_input() during process().
    /// Adapters without native UMP transport synthesise the buffer by
    /// converting the inbound MIDI 1.0 stream. `supports_mpe` and
    /// `supports_ump` are independent and can both be set.
    bool supports_ump = false;

    /// iOS-only: true when the plugin renders audio that must continue
    /// while the host app is backgrounded (live synth in AUM, looper
    /// that keeps running while the user switches apps, etc.). The
    /// host-app layer uses this flag to decide whether to set the
    /// `audio` UIBackgroundModes entitlement and keep the AVAudioSession
    /// active in background. Workstream 05 slice 5.5.
    ///
    /// Default false — most effects don't need background audio and
    /// setting the entitlement unnecessarily attracts App Store review
    /// scrutiny.
    bool ios_requires_background_audio = false;

    /// Tail time in samples (0 = no tail, -1 = infinite).
    /// Used by hosts to flush reverb/delay tails after playback stops.
    int tail_samples = 0;

    /// Optional contact info — appended here so existing positional
    /// aggregate initializers keep working. Surfaced by VST3
    /// PFactoryInfo::url/email, CLAP manufacturer_url/manufacturer_email,
    /// AU kAudioUnitProperty_URL. Leave empty to skip.
    std::string vendor_url;
    std::string vendor_email;

    /// Node ABI capability bits. This is the forward-compatible capability
    /// model; legacy supports_mpe/supports_ump remain accepted and are OR'd
    /// into effective_capabilities().
    NodeCapabilities node_capabilities;

    NodeCapabilities effective_capabilities() const {
        return {
            .supports_mpe = supports_mpe || node_capabilities.supports_mpe,
            .supports_ump = supports_ump || node_capabilities.supports_ump,
        };
    }

    /// Channel count of the first (main) input bus.
    int default_input_channels() const {
        return input_buses.empty() ? 0 : input_buses[0].default_channels;
    }
    /// Channel count of the first (main) output bus.
    int default_output_channels() const {
        return output_buses.empty() ? 0 : output_buses[0].default_channels;
    }
};

/// Prepare context — passed once before processing starts.
///
/// Contains the host's audio configuration. Use this to allocate
/// buffers, initialize filters at the correct sample rate, etc.
struct PrepareContext {
    double sample_rate = 48000.0;
    int max_buffer_size = 512;
    int input_channels = 2;
    int output_channels = 2;
};

/// SMPTE video frame rate (item 1.3 macOS plan).
///
/// Used by `ProcessContext::frame_rate` so plugins that drive video sync
/// or display SMPTE timecode can format positions correctly. `unknown` is
/// the documented sentinel for hosts that do not provide a frame rate
/// (e.g. CLAP `clap_event_transport` has no frame-rate field; AU
/// `kAudioUnitProperty_HostCallbacks` only exposes it inside
/// `HostCallback_GetTransportState2`'s `outCurrentSampleInTimeLine`
/// indirectly via the project session — adapters report `unknown` if
/// the host does not surface it).
enum class FrameRate {
    unknown = 0,
    fps_24,           ///< Film. VST3 `kFrameRate24fps`.
    fps_25,           ///< PAL. VST3 `kFrameRate25fps`.
    fps_29_97,        ///< NTSC non-drop. VST3 `kFrameRate2997fps`.
    fps_29_97_drop,   ///< NTSC drop-frame. VST3 `kFrameRate2997DropFps`.
    fps_30,           ///< NTSC integer. VST3 `kFrameRate30fps`.
    fps_30_drop,      ///< 30 drop-frame. VST3 `kFrameRate30DropFps`.
    fps_60,           ///< High-rate. VST3 `kFrameRate60fps`.
};

/// Process context — passed every audio callback with transport state.
///
/// Fields are populated by the host. Not all hosts provide all fields —
/// check is_playing before using position data.
///
/// Adapter sourcing (item 1.3 of the macOS plan; struct-only slice
/// landed first, adapter wiring deferred to cross-cuts J / K / L / M):
///
/// - VST3 — `Vst::ProcessContext` (`SystemTime` →
///   `host_time_ns`, `ProjectTimeMusic` → `position_beats`,
///   `CycleStartMusic` / `CycleEndMusic` → `loop_start_beats` /
///   `loop_end_beats`, `kCycleActive` → `is_looping`,
///   `frameRate.framesPerSecond` → `frame_rate`).
/// - AU v3 / v2 — `kAudioUnitProperty_HostCallbacks` (HostBeatAndTempo
///   → `tempo_bpm` / `position_beats`; HostTransport →
///   `is_playing` / `is_recording` / `is_looping` /
///   `loop_start_beats` / `loop_end_beats`; MusicalTimeLocation →
///   `time_sig_numerator` / `time_sig_denominator`; `mach_absolute_time`
///   → `host_time_ns`). AU has no frame-rate field; `frame_rate`
///   stays `FrameRate::unknown`.
/// - CLAP — `clap_event_transport` (`flags` for is_playing /
///   is_recording / is_looping; `loop_start_beats` /
///   `loop_end_beats` directly; `tsig_num` / `tsig_denom` for time
///   signature). CLAP does not provide frame rate; `frame_rate`
///   stays `FrameRate::unknown`.
/// - AAX (optional Avid SDK) — `IACFTransport` (`GetCurrentTickPosition`
///   for beats; `GetCurrentLoopPosition` for loop range; transport
///   state flags). Frame rate via `IACFTransport::GetTimeCodeInfo`
///   when present, else `FrameRate::unknown`.
///
/// Change-flags (`tempo_changed`, `time_sig_changed`,
/// `transport_changed`) are computed by the adapter once per block by
/// diffing against the previous block's snapshot, so processors can
/// branch on transitions only without re-reading every field.
struct ProcessContext {
    double sample_rate = 0;
    int num_samples = 0;
    bool is_playing = false;
    bool is_recording = false;
    double tempo_bpm = 120.0;
    double position_beats = 0.0;   ///< Position in quarter notes
    int64_t position_samples = 0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;

    // --- item 1.3 extensions (struct-only; adapter wiring deferred to
    // J/K/L/M cross-cut). New fields default to "host did not provide"
    // sentinels so existing adapters that don't populate them keep the
    // pre-extension behaviour exactly. ---

    /// Bar index derived from `position_beats` + the active time
    /// signature. Hosts may already publish a precomputed bar (VST3
    /// `Vst::ProcessContext::barPositionMusic`) — in that case the
    /// adapter uses the host value directly. Otherwise the adapter
    /// derives `bar = floor(position_beats * (time_sig_denominator /
    /// 4) / time_sig_numerator)` and writes it here so processors that
    /// just need "what bar am I in" do not recompute it per block.
    /// Default 0 matches a stopped-at-origin playhead.
    int64_t bar = 0;

    /// True when the host's transport is in cycle/loop mode. VST3
    /// exposes this as the `kCycleActive` flag on `ProcessContext`;
    /// AU surfaces it via `HostTransport`'s `outIsCycling`; CLAP via
    /// the `CLAP_TRANSPORT_IS_LOOP_ACTIVE` bit on
    /// `clap_event_transport::flags`; AAX via `IACFTransport`'s loop
    /// state. Default false (no loop).
    bool is_looping = false;

    /// Loop start in quarter notes, only meaningful when `is_looping`
    /// is true. Mirrors `position_beats`'s PPQ convention. Sources:
    /// VST3 `Vst::ProcessContext::cycleStartMusic`; AU
    /// `HostTransport::outCycleStartBeat`; CLAP
    /// `clap_event_transport::loop_start_beats` (converted from CLAP's
    /// fixed-point `clap_beattime`); AAX
    /// `IACFTransport::GetCurrentLoopPosition`. Default 0.
    double loop_start_beats = 0.0;

    /// Loop end in quarter notes, only meaningful when `is_looping`
    /// is true. Same source list as `loop_start_beats`. Default 0
    /// (loop_start == loop_end => no loop range yet).
    double loop_end_beats = 0.0;

    /// Host clock timestamp matching the start of this block, in
    /// nanoseconds since an epoch the host chooses. Used for video
    /// sync against the OS clock. Sources:
    /// VST3 `Vst::ProcessContext::systemTime` (already nanoseconds on
    /// Apple; otherwise host-defined); AU `mach_absolute_time()`
    /// converted via `mach_timebase_info` (the AU adapter performs the
    /// conversion before writing here); CLAP has no host-time field
    /// today, so the adapter leaves the value at 0 (sentinel = "not
    /// provided"); AAX `IACFTransport`'s sample-position only — host
    /// time is unavailable, so leave at 0.
    int64_t host_time_ns = 0;

    /// SMPTE frame rate when the host exposes one. Default
    /// `FrameRate::unknown` is the documented sentinel meaning "host
    /// did not provide" — plugins must check before using.
    FrameRate frame_rate = FrameRate::unknown;

    /// True when the host's reported `tempo_bpm` differs from the
    /// previous block. Lets processors recompute tempo-dependent
    /// derived state (sample-domain envelope rates, delay-line beat
    /// lengths) only on transitions instead of every block. Computed
    /// by the adapter as `current.tempo_bpm != previous.tempo_bpm`.
    /// Default false (no change relative to a hypothetical previous
    /// block, which matches the initial-state contract).
    bool tempo_changed = false;

    /// True when the host's reported time signature
    /// (`time_sig_numerator` or `time_sig_denominator`) differs from
    /// the previous block. Lets processors rebuild bar-grid state on
    /// transitions only. Default false.
    bool time_sig_changed = false;

    /// True when any transport state field (`is_playing`,
    /// `is_recording`, `is_looping`) flipped since the previous
    /// block. Lets processors reset playback-only DSP state (e.g.
    /// flush a reverb tail when transport stops) on transitions only.
    /// Default false.
    bool transport_changed = false;
};

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
    virtual ~Processor() = default;

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

    /// Tier B Slice 15 of planning/2026-05-18-rt-safety-and-debug-dx.md.
    ///
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
    /// hooks automatically — that comes in a follow-up slice once the
    /// canonical "suspend-then-load-preset" surface for each adapter
    /// is settled. Today the hooks exist so a plug-in can wire its
    /// own UI-thread "loading…" workflow against them and the adapter
    /// integration is purely additive.
    ///
    /// Mirrors JUCE's @c AudioProcessor::suspendProcessing per
    /// sudara "Big List of JUCE Tips" #30.
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
    /// notifications + Android TrimMemory. Workstream 05 slice 5.3.
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
    /// state/sync-strategy guidance for cache invalidation.
    ///
    /// Wiring:
    ///   iOS       — PulpAudioSessionBridge routes didReceiveMemoryWarning
    ///               (slice 5.1 hooks this into the running processor).
    ///   macOS     — no-op today; host apps can still invoke manually to
    ///               test plugin behaviour.
    ///   Android   — ComponentCallbacks2.onTrimMemory (future slice).
    ///   Windows   — CreateMemoryResourceNotification (future slice).
    virtual void on_memory_pressure(MemoryPressure /*level*/) {}

    /// Latency in samples introduced by this processor (default 0).
    /// Override for plugins that buffer or lookahead (e.g., compressors,
    /// linear-phase EQs). Hosts use this for delay compensation.
    virtual int latency_samples() const { return 0; }

    /// Proposed bus layout passed to is_bus_layout_supported().
    ///
    /// Each entry's index matches the descriptor's input_buses /
    /// output_buses index, and each value is the number of channels the
    /// host is proposing for that bus. Sidechain buses appear at index 1
    /// (input side) when the descriptor declared one. Empty per-side
    /// vectors mean "the host did not propose buses on that side"
    /// (rare; treat as 'no opinion').
    ///
    /// Workstream 03 item 3.7. Format adapters call this on the host
    /// thread before applying the layout. Returning false rejects the
    /// proposal and the adapter is expected to refuse the host's
    /// `setBusArrangements` / equivalent call.
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
    /// which preserves the pre-item-3.7 behaviour exactly.
    virtual bool is_bus_layout_supported(const BusesLayout& layout) const {
        const auto desc = descriptor();
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

    /// Item 3.11 — cross-adapter latency / tail change notifications.
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
    /// Workstream 03 item 3.11.
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
    virtual std::unique_ptr<view::View> create_view() { return nullptr; }

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

    /// Called when the host's transport state transitions between
    /// playing and stopped, or jumps to a new position. Default no-op.
    /// Workstream 01 slice 1.11.
    virtual void on_host_transport_changed(bool /*is_playing*/,
                                           double /*position_seconds*/) {}
    /// Called when the host's transport tempo changes. Default no-op.
    /// Override for plugins that care about tempo outside of a process()
    /// call — delay sync recomputation, tempo-synced LFO rate caches,
    /// UI BPM read-outs. Runs on the main/UI thread; the audio thread
    /// keeps reading the current tempo from ProcessContext as usual.
    /// Workstream 01 slice 1.10.
    virtual void on_host_tempo_changed(double /*new_tempo_bpm*/) {}

    /// Optional ARA 2.x document-controller factory (workstream 06 slice 6.3).
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

    /// Access the parameter state store.
    /// Use state().get_value(id) to read parameter values in process().
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

private:
    state::StateStore* state_store_ = nullptr;
    const audio::BufferView<const float>* sidechain_ = nullptr;
    const midi::MpeBuffer* mpe_input_ = nullptr;
    const midi::UmpBuffer* ump_input_ = nullptr;
    const state::ParameterEventQueue* param_events_ = nullptr;
    // Item 3.11 — RT-safe pending flags published from process() and
    // consumed by adapters on the host / main thread.
    std::atomic<bool> latency_changed_{false};
    std::atomic<bool> tail_changed_{false};
};

/// Factory function type — plugins provide this to create processor instances.
using ProcessorFactory = std::unique_ptr<Processor>(*)();

} // namespace pulp::format
