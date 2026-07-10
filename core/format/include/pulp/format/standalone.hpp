#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/device.hpp>
#if PULP_ENABLE_AUDIO_PROBES
#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#endif
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/test_signal.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/midi/message_collector.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/view/audio_bridge.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::view {
class AudioInspectorWindow;
class CommandRegistry;
}  // namespace pulp::view

namespace pulp::format {

struct StandaloneConfig {
    std::string audio_device_id;
    std::string midi_input_id;
    double sample_rate = 48000.0;
    int buffer_size = 256;
    int output_channels = 2;
    int input_channels = 0;
    // Optional host constraints. When non-empty, the standalone settings UI
    // only offers these values and persisted settings outside the list are
    // clamped back to the first allowed value.
    std::vector<double> allowed_sample_rates;
    std::vector<int> allowed_buffer_sizes;
    // Capability flag for apps/instruments that do not consume audio input.
    // The standalone host derives this from the processor descriptor at
    // startup, and the settings UI uses it to avoid presenting an unusable
    // input-device workflow for instrument-only apps.
    bool supports_audio_input = true;
    // Effects usually want the test signal as input. Instruments have no
    // audio input, so their test tone should exercise the selected output
    // device directly.
    bool route_test_signal_to_output = false;
    // When true, run_with_editor() avoids showing/activating the native
    // window. Use with screenshot_path for CI/test smoke runs.
    bool headless = false;
    // When false, run_with_editor() hosts the editor directly and omits the
    // built-in Settings tab.
    bool show_settings_tab = true;

    // Remember the user's audio/MIDI device selection (+ sample rate, buffer, transport)
    // across launches, keyed by the plugin name. On by default; a developer can set this
    // false to always start from the configured defaults. Saved whenever settings change,
    // restored at startup (the first launch keeps the configured defaults).
    bool persist_settings = true;

    // When non-empty, run_with_editor() installs a one-shot idle callback
    // that captures the first painted frame via WindowHost::capture_png()
    // and writes to this path, then closes the window. Codified in the SDK
    // (not per-app) so every pulp::format::StandaloneApp consumer gets
    // headless screenshot capture for free. Set via set_config() or by
    // parsing `--screenshot=PATH` from argv in main().
    std::string screenshot_path;
    // Frames to wait before capture. Default 30 (~0.5s @60fps) gives the
    // first React-driven layout + effects pass time to settle.
    int screenshot_frame_delay = 30;

    // When non-empty, run_with_editor() arms the same one-shot frame-delay
    // path as `screenshot_path` and, after the delay, writes the live output
    // probe's latest snapshot (peak/RMS/dBFS/clip/NaN/silence counters) as a
    // JSON object to this path, then exits. This is the programmatic readout
    // of the live Audio Inspector for agents and CI — distinct from the
    // offline `pulp audio validate` Doctor. Set via set_config() or by reading
    // PULP_AUDIO_PROBE_JSON. Only meaningful when PULP_ENABLE_AUDIO_PROBES is
    // ON; probes-off standalone builds reject this request before opening the
    // editor so callers do not mistake an unsupported binary for silence.
    std::string audio_probe_json_path;

    // Programmatic live scope capture over the output-boundary probe's copied
    // capture ring. Like audio_probe_json_path, this is a dev/agent readout and
    // is only meaningful when PULP_ENABLE_AUDIO_PROBES is ON.
    std::string audio_scope_json_path;
    int audio_scope_window_samples = 2048;
    std::string audio_scope_trigger = "rising-zero";
    int audio_scope_channel = 0;

    // Programmatic live capture-to-WAV over the same output-boundary probe ring.
    // Writes a WAV the offline `pulp audio validate` verbs can read, then the
    // one-shot exits. Only meaningful when PULP_ENABLE_AUDIO_PROBES is ON.
    // `audio_capture_wav_frames` is the ring window in samples (0 = as much as the
    // ring holds, clamped to the capture cap). Captures all output channels.
    std::string audio_capture_wav_path;
    int audio_capture_wav_frames = 0;

    // Programmatic live ROLLING capture-to-WAV. Unlike audio_capture_wav_path
    // (which dumps the probe FIFO's EARLIEST window), this taps the output
    // boundary into a RollingAudioCaptureBuffer and writes the LAST
    // `audio_capture_rolling_frames` frames as a float WAV the offline `pulp
    // audio validate` verbs can read — the steady-state window `doctor`/`compare`
    // want, with no int16 quantization floor. The one-shot then exits. Only
    // meaningful when PULP_ENABLE_AUDIO_PROBES is ON. Captures all output channels.
    std::string audio_capture_rolling_path;
    int audio_capture_rolling_frames = 0;
    // Rolling capture sample format: false → float32 (default, full precision),
    // true → int24 (≈ −144 dBFS floor, smaller file, universal DAW compatibility).
    bool audio_capture_rolling_int24 = false;

    // Built-in tempo source. The standalone host has no DAW providing
    // transport, so it acts as one: it surfaces `tempo_bpm` / time signature on
    // every ProcessContext block, and advances `position_beats` from
    // `position_samples` when the transport is rolling. Plugins that branch on
    // `is_playing` or read `position_beats` therefore behave the same way in
    // `pulp run` as they do in a host without any per-plugin glue.
    double tempo_bpm = 120.0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;
    bool transport_playing = true;  // default-on so MIDI/tempo plugins are immediately useful
};

namespace detail {

inline constexpr std::size_t kStandaloneMidiInputQueueCapacity = 2048;
inline constexpr std::size_t kStandaloneMidiBufferEventCapacity =
    kStandaloneMidiInputQueueCapacity +
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
    bool run_with_editor(bool use_gpu = false);

    /// Restart audio with a new config (stop → reconfigure → start).
    bool apply_config(const StandaloneConfig& new_config);

    Processor* processor() { return processor_.get(); }
    state::StateStore& state() { return store_; }

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

private:
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

    ProcessorFactory factory_;
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store_;
    std::unique_ptr<Processor> processor_;
    StandaloneConfig config_;
    bool persisted_config_loaded_ = false;  // overlay persisted settings once, not on soft restarts

    /// Previous block's transport state, so the standalone driver derives the
    /// same change flags (`tempo_changed`, `transport_changed`,
    /// `transport_started`, `transport_jump`) the plugin adapters do. Touched
    /// only from the audio callback.
    detail::PlayheadSnapshot playhead_prev_{};

    std::unique_ptr<audio::AudioSystem> audio_system_;
    std::unique_ptr<audio::AudioDevice> audio_device_;
    std::unique_ptr<midi::MidiSystem> midi_system_;
    std::unique_ptr<midi::MidiInput> midi_input_;

    detail::StandaloneMidiInputQueue hardware_midi_queue_;
    midi::MidiBuffer midi_in_;
    midi::MidiBuffer midi_out_;
    midi::MidiMessageCollector<> ui_midi_collector_;
    std::atomic<bool> running_{false};

    // Built-in transport state (the standalone "host").
    // `transport_position_samples_` advances atomically on the audio
    // thread; `transport_block_time_seconds_` is derived by the audio
    // callback (`position_samples / sample_rate`) and is what
    // `MidiMessageCollector::drain_into` uses to align UI MIDI events
    // to sample offsets within the current block.
    std::atomic<int64_t> transport_position_samples_{0};

    TestSignalSource test_signal_;
    view::AudioBridge input_meter_bridge_;
    view::AudioBridge output_meter_bridge_;
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
    std::unique_ptr<view::CommandRegistry> command_registry_;
    std::unique_ptr<view::AudioInspectorWindow> audio_inspector_;
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
