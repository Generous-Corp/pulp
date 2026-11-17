#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/device.hpp>
#if PULP_ENABLE_AUDIO_PROBES
#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#endif
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone_config.hpp>
#include <pulp/format/standalone_control_host.hpp>
#include <pulp/runtime/trace_session.hpp>
#include <pulp/format/test_signal.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/midi/message_collector.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/view/audio_bridge.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulp::view {
class AudioInspectorWindow;
class CommandRegistry;
}  // namespace pulp::view

namespace pulp::state {
class PresetManager;
class PresetLibrary;
} // namespace pulp::state

namespace pulp::format {

class PresetProgramRouter;

namespace detail {
class StandaloneMusicalTyping;
}


namespace detail {

inline constexpr std::size_t kStandaloneMidiInputQueueCapacity = 2048;
inline constexpr std::size_t kStandaloneTestMidiQueueCapacity = 256;
inline constexpr std::size_t kStandaloneTestMidiNoteCount = 16 * 128;
inline constexpr std::size_t kStandaloneMidiBufferEventCapacity =
    kStandaloneMidiInputQueueCapacity +
    kStandaloneTestMidiQueueCapacity +
    kStandaloneTestMidiNoteCount +
    midi::MidiMessageCollector<>::capacity() +
    midi::MidiMessageCollector<>::pending_capacity();

using StandaloneMidiInputQueue =
    runtime::SpscQueue<midi::MidiEvent, kStandaloneMidiInputQueueCapacity>;

inline std::size_t drain_standalone_midi_input(StandaloneMidiInputQueue& queue,
                                               midi::MidiBuffer& out) {
    std::size_t popped = 0;
    std::size_t drained = 0;
    while (popped < kStandaloneMidiInputQueueCapacity) {
        auto event = queue.try_pop();
        if (!event) break;
        ++popped;
        if (out.add(*event)) ++drained;
    }
    return drained;
}

enum class StandaloneTestMidiKind : std::uint8_t {
    NoteOn,
    NoteOff,
};

struct StandaloneTestMidiNote {
    StandaloneTestMidiKind kind = StandaloneTestMidiKind::NoteOn;
    /// Protocol-facing MIDI channel number (1..16).
    std::uint8_t channel = 1;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

struct StandaloneTestTransportUpdate {
    std::optional<bool> playing;
    std::optional<std::int64_t> position_samples;
    std::optional<double> tempo_bpm;
};

struct StandaloneTestTransportState {
    bool playing = true;
    double tempo_bpm = 120.0;
    std::int64_t position_samples = 0;
};

enum class StandaloneTestInputResult : std::uint8_t {
    Applied,
    InvalidArgument,
    QueueFull,
};

/// Bounded control-thread to audio-thread bridge for standalone inspector test
/// input. MIDI delivery is generation-scoped so releasing a controller drops
/// stale note-offs, turns accepted queued note-ons into bounded one-block
/// pulses, and schedules note-offs for every note that already reached DSP.
/// Transport commands use a main-to-audio triple buffer; audio observations use
/// an audio-writer sequence lock. Both directions stay coherent without making
/// the audio callback wait for a preempted control-thread writer.
class StandaloneTestInputHost {
  public:
    static constexpr double kMinimumTempoBpm = 20.0;
    static constexpr double kMaximumTempoBpm = 400.0;

    StandaloneTestInputResult inject_note(StandaloneTestMidiNote note);
    StandaloneTestInputResult
    update_transport(const StandaloneTestTransportUpdate& update);

    /// Invalidate all queued input from the current controller and restore the
    /// transport snapshot from before its first update. Safe from any non-audio
    /// thread; the next audio block emits tracked note-offs first.
    void release_test_input() noexcept;

    StandaloneTestTransportState transport_snapshot() const noexcept;
    std::uint64_t midi_overflow_count() const noexcept;

    /// The standalone's own play/stop (Space in its window): the playing
    /// state the next audio block takes. Kept through a test controller's
    /// release, which restores everything else it changed. Any non-audio
    /// thread.
    void set_playing(bool playing) noexcept;
    /// The playing state last commanded, whether or not audio has run since.
    bool commanded_playing() const noexcept;

    /// @internal Standalone audio-driver/test seam. Call prepare only while the
    /// audio callback is stopped; all remaining methods are RT-safe.
    void prepare(bool playing, double tempo_bpm) noexcept;
    std::size_t drain_midi_into(midi::MidiBuffer& out,
                                int block_size_samples) noexcept;
    StandaloneTestTransportState begin_audio_block() noexcept;
    void end_audio_block(int block_size_samples) noexcept;

  private:
    struct QueuedNote {
        StandaloneTestMidiNote note;
        std::uint64_t generation = 0;
    };
    struct TransportCommand {
        bool playing = true;
        double tempo_bpm = 120.0;
        std::int64_t position_samples = 0;
        std::uint64_t position_revision = 0;
    };

    bool release_active_notes(midi::MidiBuffer& out,
                              int sample_offset) noexcept;

    runtime::SpscQueue<QueuedNote, kStandaloneTestMidiQueueCapacity> midi_queue_;
    std::atomic<std::uint64_t> midi_generation_{1};
    std::uint64_t audio_midi_generation_ = 1;
    std::array<std::array<bool, 128>, 16> active_notes_{};
    bool note_release_pending_ = false;

    TransportCommand control_transport_{};
    mutable std::mutex control_transport_mutex_;
    std::optional<TransportCommand> transport_before_control_;
    runtime::TripleBuffer<TransportCommand> transport_commands_{control_transport_};
    StandaloneTestTransportState audio_transport_{};
    runtime::SeqLock<StandaloneTestTransportState> observed_transport_{audio_transport_};
    std::uint64_t audio_position_revision_ = 0;
    bool prepared_ = false;
};

}  // namespace detail

class StandaloneApp {
public:
    explicit StandaloneApp(ProcessorFactory factory);
    ~StandaloneApp();

    void set_config(const StandaloneConfig& config) { config_ = config; }
    const StandaloneConfig& config() const { return config_; }

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    std::uint64_t audio_xrun_count() const {
        return audio_device_ ? audio_device_->xrun_count() : 0;
    }
    bool run_with_editor(bool use_gpu = false);

    /// Install a handler for files the OS asks this app to open — a Finder
    /// double-click, a drag onto the app icon, or `open -a`. Paths arrive on the
    /// UI thread.
    ///
    /// Set it BEFORE run_with_editor(). The file that launched the app is
    /// reported while the event loop is still starting, so a handler installed
    /// afterwards would miss the common case; paths that arrive early are held
    /// until a handler exists.
    ///
    /// Routing is the other half of this. Nothing reaches here unless the app
    /// bundle declares the document type — pulp_declare_standalone_document_type()
    /// in CMake — so an app that only installs a handler still never opens a file.
    void set_open_files_handler(
        std::function<void(const std::vector<std::string>&)> handler) {
        open_files_handler_ = std::move(handler);
    }

    /// Restart audio with a new config (stop → reconfigure → start).
    bool apply_config(const StandaloneConfig& new_config);

    /// Opens the app's Audio/MIDI settings UI. Returns true when a window is
    /// now showing (or already was). Main thread.
    using SettingsWindowOpener = std::function<bool(StandaloneApp&)>;

    /// Register (or, with an empty function, restore the default of) the
    /// opener `open_settings_window()` calls. The default is the native
    /// Audio/MIDI window of a standalone without a view layer, built on
    /// `StandaloneSettingsModel`; it is available on macOS and declines
    /// elsewhere. An app with its own settings UI replaces it here, and the
    /// contract editor's "Audio/MIDI Settings…" menu item (Cmd+,) and an
    /// editor's `HostAction::open_settings` request both reach it.
    void set_settings_window_opener(SettingsWindowOpener opener) {
        settings_window_opener_ = std::move(opener);
    }

    /// Show the Audio/MIDI settings through the registered opener, or the
    /// default one. False when nothing could be opened.
    bool open_settings_window();

    Processor* processor() { return processor_.get(); }
    state::StateStore& state() { return store_; }

    /// True when this launch skipped the audio backend entirely because it is a
    /// screenshot-only capture (see StandaloneConfig::screenshot_keeps_audio).
    /// No audio system, no device, and no render callback exist for such a run,
    /// so a headless capture never opens an audio device on the host machine.
    bool audio_skipped_for_capture() const { return audio_skipped_for_capture_; }

    TestSignalSource& test_signal() { return test_signal_; }
    view::AudioBridge& input_meter_bridge() { return input_meter_bridge_; }
    view::AudioBridge& output_meter_bridge() { return output_meter_bridge_; }

#if PULP_ENABLE_AUDIO_PROBES
    /// Realtime output-boundary probe. Observes the processor output
    /// at the "standalone processor-output boundary" — immediately after
    /// `processor_->process(...)` and before the device callback returns. This
    /// is the first wired probe stage for the "UI works, no sound" report. A
    /// consumer (UI/test) reads the latest snapshot via
    /// `output_probe().latest()`. Distinct from `input_meter_bridge_`, which is
    /// input-oriented and lacks the snapshot's stage/sequence/NaN/clip fields.
    audio::AudioProbe& output_probe() { return output_probe_; }
#endif
    audio::AudioSystem* audio_system() { return audio_system_.get(); }
    midi::MidiSystem* midi_system() { return midi_system_.get(); }

    // UI-thread MIDI injection. Virtual-keyboard widgets, scripting
    // hooks, and test harnesses push MIDI events through this collector;
    // the audio callback drains them into each block's MidiBuffer at the
    // correct sample offsets. Identical thread-safety surface that
    // pulp::midi::MidiMessageCollector documents — push_now is non-blocking.
    midi::MidiMessageCollector<>& ui_midi_collector() { return ui_midi_collector_; }
    detail::StandaloneTestInputHost& test_input_host() { return test_input_host_; }
    const detail::StandaloneTestInputHost& test_input_host() const {
        return test_input_host_;
    }

    // Persist + restore StandaloneConfig under
    // ApplicationProperties so `pulp run` opens the user's last device,
    // sample-rate, buffer-size, MIDI input, and built-in transport
    // settings on the next launch. Returns false when the storage layer
    // failed to write (e.g. read-only profile, missing app name).
    // Overlays any persisted keys onto `base` and returns it — so unsaved fields keep the
    // caller's defaults (the first launch, with no file, returns `base` unchanged).
    static StandaloneConfig load_persisted_config(std::string_view app_name,
                                                  StandaloneConfig base = {});
    static bool save_persisted_config(std::string_view app_name,
                                      const StandaloneConfig& config);

    // The plug-in state blob kept next to the persisted device selection (the
    // host-facing blob plugin_state_io produces, base64 in the settings file).
    // load returns nullopt when nothing is saved or the stored text is not valid
    // base64; save returns false for an empty app name, a blob over
    // kMaxPersistedStateBytes, or a settings location that cannot be written.
    static constexpr std::size_t kMaxPersistedStateBytes = 8u * 1024u * 1024u;
    static std::optional<std::vector<uint8_t>> load_persisted_state(std::string_view app_name);
    static bool save_persisted_state(std::string_view app_name, std::span<const uint8_t> blob);

    /// Main thread only. Saves the plug-in state when it has changed and then
    /// stayed unchanged for `state_save_debounce_ms`. The built-in editors call
    /// this from their idle loop; a host that runs its own loop calls it a few
    /// times a second. Cheap when nothing changed. `now` is a test seam.
    void service_state_persistence(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /// Main thread only. When the plug-in's descriptor sets
    /// `supports_program_change`, loads the preset a Bank Select + Program
    /// Change selected since the last call. The audio callback only latches the
    /// request; the preset is read and applied here. The built-in editors call
    /// this from their idle loop (throttled to about 30 Hz); a host that runs
    /// its own loop calls it a few times a second. Cheap when nothing is
    /// pending. `now` is a test seam.
    void service_program_changes(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /// Main thread only. Saves the plug-in state now when persistence is active.
    /// Returns true when it wrote the file. Called on quit.
    bool save_plugin_state_now();

    /// True when this run restores and saves the plug-in state (see
    /// StandaloneConfig::persist_plugin_state). Valid after start().
    bool plugin_state_persistence_active() const noexcept {
        return state_persist_.active;
    }

    /// How many times this run has written the plug-in state to the settings
    /// file.
    std::uint32_t plugin_state_save_count() const noexcept {
        return state_persist_.saves;
    }

  private:
    // run_with_editor() for a processor with an editor factory: the editor-host
    // contract editor in its own native window (detail/standalone_contract_editor.hpp).
    bool run_contract_editor(EditorFactory& factory);

    // The view layer. standalone_view_layer.cpp defines these for
    // pulp::standalone and pulp::standalone-native; standalone_view_layer_none.cpp
    // defines them for pulp::standalone-contract, the standalone of a UI package
    // plug-in, which links no Pulp view, canvas or render code (its editor is
    // the package's, opened through the editor-host contract).
    void attach_view_layer();
    void prepare_view_input(const StandaloneConfig& config);
    // Audio thread: append the musical typing keyboard's recovery events.
    void drain_view_input(midi::MidiBuffer& midi, int frames) noexcept;
    void shutdown_view_input() noexcept;
    int view_waveform_capacity() const noexcept;
    // run_with_editor() for a processor without an editor factory: its
    // create_view() editor in a Pulp window.
    bool run_view_editor(bool use_gpu, const StandaloneConfig& effective_config);
    std::function<void(const std::vector<std::string>&)> open_files_handler_;
    SettingsWindowOpener settings_window_opener_;
    // An apply failed and could not restore the previous config, so audio is
    // stopped; the next apply starts it again.
    bool restart_pending_ = false;

    // Declared FIRST so it is destroyed LAST: the final detach cancels and
    // joins the tracing auto-flush timer and writes the .pftrace, which must
    // happen after every span this app can still emit. Tracing used to be
    // wired into VST3 ONLY, so a Perfetto capture of a standalone run
    // recorded nothing. No-op unless PULP_TRACING=ON.
    runtime::ScopedTracingAttachment tracing_;

    // Tear down the audio + MIDI devices but KEEP the processor instance alive.
    // apply_config() uses this so a settings change does not recreate the
    // Processor out from under an editor ViewBridge holding a Processor&.
    void stop_audio_keep_processor();

    // Device-independent render-state preparation: sizes the processor and every
    // audio-thread scratch buffer / probe to `max_callback_block_`. Factored out
    // of start() (which calls it after the device refreshes config_) so a test
    // can prepare a StandaloneApp for a render without opening real hardware.
    void prepare_render_state();

    // The body of the audio device callback, hoisted verbatim out of the start()
    // lambda so it is reachable off the device thread. Drives one block: MIDI
    // drain, transport derivation, and the ScopedNoAlloc-guarded
    // Processor::process(). RT-safe once prepare_render_state() has run.
    void render_audio_block(const audio::BufferView<const float>& input,
                            audio::BufferView<float>& output,
                            const audio::CallbackContext& ctx);

    // Test-only accessor (defined in test/test_standalone_rt.cpp) that drives
    // prepare_render_state() + render_audio_block() headlessly to assert the
    // render path is allocation/lock-free. Mirrors the @internal hook precedent.
    friend struct StandaloneRenderTestAccess;

    // Test-only accessor (defined in test/test_standalone_capture_audio.cpp)
    // that injects `audio_system_factory_` so the device lifecycle can be
    // asserted without opening real hardware.
    friend struct StandaloneAudioDeviceTestAccess;

    // Test-only accessor (defined in test/test_standalone_settings_model.cpp)
    // that injects `midi_system_factory_` and reads the opened MIDI input.
    friend struct StandaloneMidiTestAccess;

    ProcessorFactory factory_;
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store_;
    std::unique_ptr<Processor> processor_;
    // Present only when the descriptor sets supports_program_change. Declared
    // after processor_ so they are destroyed first; the library holds a
    // reference to the manager, which holds one to store_.
    std::unique_ptr<state::PresetManager> preset_manager_;
    std::unique_ptr<state::PresetLibrary> preset_library_;
    std::unique_ptr<PresetProgramRouter> program_router_;
    std::chrono::steady_clock::time_point program_change_polled_{};
    void init_program_change();
    // Panic requests (Processor::request_panic), serviced before each block.
    std::shared_ptr<PanicState> panic_state_;
    // Present in every StandaloneApp, but populated only by an executable that
    // explicitly links and registers a canonical control host adapter.
    std::unique_ptr<StandaloneControlHost> control_host_;
    StandaloneConfig config_;
    bool persisted_config_loaded_ = false;  // overlay persisted settings once, not on soft restarts

    // Plug-in state persistence. Main thread only.
    struct StatePersistence {
        bool active = false;
        bool pending = false;                // a change is waiting out its debounce
        std::uint32_t saves = 0;             // times the settings file was written
        std::uint64_t saved_fingerprint = 0; // what the settings file holds
        std::uint64_t observed_fingerprint = 0;
        std::chrono::steady_clock::time_point first_change{};
        std::chrono::steady_clock::time_point last_change{};
        std::chrono::steady_clock::time_point last_poll{};
    } state_persist_;
    void restore_persisted_state();
    std::uint64_t state_fingerprint() const;

    /// Previous block's transport state, so the standalone driver derives the
    /// same change flags (`tempo_changed`, `transport_changed`,
    /// `transport_started`, `transport_jump`) the plugin adapters do. Touched
    /// only from the audio callback.
    detail::PlayheadSnapshot playhead_prev_{};

    std::unique_ptr<audio::AudioSystem> audio_system_;
    std::unique_ptr<audio::AudioDevice> audio_device_;
    // Set by start(): this launch is a screenshot-only capture, so it created
    // no audio system and no device. Reported by audio_skipped_for_capture().
    bool audio_skipped_for_capture_ = false;
    // Test seam: when set, start() builds the audio system from this factory
    // instead of audio::create_audio_system(). Never set in shipped code.
    std::function<std::unique_ptr<audio::AudioSystem>()> audio_system_factory_;
    // Test seam: when set, start() builds the MIDI system from this factory
    // instead of midi::create_midi_system(). Never set in shipped code.
    std::function<std::unique_ptr<midi::MidiSystem>()> midi_system_factory_;
    std::unique_ptr<midi::MidiSystem> midi_system_;
    std::unique_ptr<midi::MidiInput> midi_input_;

    detail::StandaloneMidiInputQueue hardware_midi_queue_;
    midi::MidiBuffer midi_in_;
    midi::MidiBuffer midi_out_;
    midi::MidiMessageCollector<> ui_midi_collector_;
    detail::StandaloneTestInputHost test_input_host_;
    std::atomic<bool> running_{false};

    TestSignalSource test_signal_;
    view::AudioBridge input_meter_bridge_;
    view::AudioBridge output_meter_bridge_;
    // View-layer objects, owned through a deleter the view layer supplies, so
    // a StandaloneApp built without a view layer (pulp::standalone-contract)
    // never names their destructors. Null there.
    template <typename T> using ViewLayerPtr = std::unique_ptr<T, void (*)(T*)>;
    ViewLayerPtr<view::CommandRegistry> command_registry_{nullptr, nullptr};
    ViewLayerPtr<detail::StandaloneMusicalTyping> musical_typing_{nullptr, nullptr};
#if PULP_ENABLE_AUDIO_PROBES
    // Realtime output-boundary probe. prepare()d in start() with the
    // device's channel/buffer/rate; analyze_output() is called from the audio
    // callback right after processor render. RT-safe (scalar-only).
    audio::AudioProbe output_probe_;
    // Pre-allocated channel-pointer array for the probe view (no audio-thread
    // allocation). Sized in start() to the output channel count.
    std::vector<const float*> output_probe_ptrs_;

    // Rolling last-N output capture, separate from the probe FIFO. prepare()d in
    // start() only when audio_capture_rolling_path is set; the audio callback
    // append()s each output block (RT-safe), and the one-shot materializes the
    // last window to a float WAV. `rolling_capture_active_` gates the
    // audio-thread append so there is zero added work when the flag is off.
    audio::RollingAudioCaptureBuffer rolling_capture_;
    bool rolling_capture_active_ = false;
    // Channel count the audio callback actually delivers (set on the audio
    // thread). The rolling ring is prepared to the configured output_channels;
    // if the device under-delivers, the writer trims the WAV to this so it has
    // no phantom silent channels. Relaxed: a single int published once per run.
    std::atomic<int> rolling_capture_channels_{0};

    // Developer Audio Inspector tool window. A separate floating
    // window that reads `output_probe_.latest()` each UI tick and renders the
    // live meters / waveform / probe-stage status. Lives on the app so it
    // outlives the idle callback that polls it; `output_probe_` is declared
    // before it so the probe outlives the window on teardown (the window holds
    // a raw probe pointer). Constructed in run_with_editor() only when a real
    // window exists, behind a shared CommandRegistry routed by
    // `route_global_keys` (Cmd/Ctrl+Shift+A). Opened when PULP_AUDIO_INSPECTOR
    // is set in the environment.
    ViewLayerPtr<view::AudioInspectorWindow> audio_inspector_{nullptr, nullptr};
#endif
    // The MAXIMUM frames the audio callback may deliver in one block. This is
    // NOT the nominal buffer_size: when the device runs at a different sample
    // rate than the app, CoreAudio (and other backends) insert a resampler that
    // pulls the render callback in variable, larger-than-nominal blocks. The
    // processor and every scratch buffer are sized to THIS, and the callback
    // guards against any block beyond it, so an oversized pull can never trip
    // Processor::process()'s `num_samples <= max_block` assert or overflow the
    // pre-allocated buffers.
    int max_callback_block_ = 0;
    audio::Buffer<float> test_buffer_;        // Pre-allocated for audio callback
    audio::Buffer<float> silence_buffer_;    // Pre-allocated silence for missing input
    std::vector<float*> test_ptrs_;           // Pre-allocated channel pointers
    std::vector<float*> direct_output_ptrs_;  // Pre-allocated for output test signal
    std::vector<const float*> silence_ptrs_;  // Pre-allocated silence channel pointers
    std::vector<const float*> meter_ptrs_;    // Pre-allocated for meter analysis
    std::vector<const float*> output_meter_ptrs_;
};

} // namespace pulp::format
