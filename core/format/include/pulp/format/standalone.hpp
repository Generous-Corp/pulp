#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/device.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/test_signal.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/midi/message_collector.hpp>
#include <pulp/view/audio_bridge.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace pulp::format {

struct StandaloneConfig {
    std::string audio_device_id;
    std::string midi_input_id;
    double sample_rate = 48000.0;
    int buffer_size = 256;
    int output_channels = 2;
    int input_channels = 0;
    // When true, run_with_editor() avoids showing/activating the native
    // window. Use with screenshot_path for CI/test smoke runs.
    bool headless = false;
    // When false, run_with_editor() hosts the editor directly and omits the
    // built-in Settings tab.
    bool show_settings_tab = true;
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

    // Item 3.5 (macOS plan) — built-in tempo source. The standalone host has
    // no DAW providing transport, so it acts as one: it surfaces
    // `tempo_bpm` / time signature on every ProcessContext block, and
    // advances `position_beats` from `position_samples` when the transport
    // is rolling. Plugins that branch on `is_playing` or read
    // `position_beats` therefore behave the same way in `pulp run` as they
    // do in a host (item 1.3 wiring) without any per-plugin glue.
    double tempo_bpm = 120.0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;
    bool transport_playing = true;  // default-on so MIDI/tempo plugins are immediately useful
};

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
    audio::AudioSystem* audio_system() { return audio_system_.get(); }
    midi::MidiSystem* midi_system() { return midi_system_.get(); }

    // Item 3.5 — UI-thread MIDI injection. Virtual-keyboard widgets, scripting
    // hooks, and test harnesses push MIDI events through this collector;
    // the audio callback drains them into each block's MidiBuffer at the
    // correct sample offsets. Identical thread-safety surface that
    // pulp::midi::MidiMessageCollector documents — push_now is non-blocking.
    midi::MidiMessageCollector<>& ui_midi_collector() { return ui_midi_collector_; }

    // Item 3.5 — Persist + restore StandaloneConfig under
    // ApplicationProperties so `pulp run` opens the user's last device,
    // sample-rate, buffer-size, MIDI input, and built-in transport
    // settings on the next launch. Returns false when the storage layer
    // failed to write (e.g. read-only profile, missing app name).
    static StandaloneConfig load_persisted_config(std::string_view app_name);
    static bool save_persisted_config(std::string_view app_name,
                                      const StandaloneConfig& config);

private:
    // Tear down the audio + MIDI devices but KEEP the processor instance alive.
    // apply_config() uses this so a settings change does not recreate the
    // Processor out from under an editor ViewBridge holding a Processor& (#2693).
    void stop_audio_keep_processor();

    ProcessorFactory factory_;
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;
    StandaloneConfig config_;

    std::unique_ptr<audio::AudioSystem> audio_system_;
    std::unique_ptr<audio::AudioDevice> audio_device_;
    std::unique_ptr<midi::MidiSystem> midi_system_;
    std::unique_ptr<midi::MidiInput> midi_input_;

    midi::MidiBuffer pending_midi_;
    std::mutex midi_mutex_;
    midi::MidiMessageCollector<> ui_midi_collector_;
    std::atomic<bool> running_{false};

    // Item 3.5 — built-in transport state (the standalone "host").
    // `transport_position_samples_` advances atomically on the audio
    // thread; `transport_block_time_seconds_` is derived by the audio
    // callback (`position_samples / sample_rate`) and is what
    // `MidiMessageCollector::drain_into` uses to align UI MIDI events
    // to sample offsets within the current block.
    std::atomic<int64_t> transport_position_samples_{0};

    TestSignalSource test_signal_;
    view::AudioBridge input_meter_bridge_;
    audio::Buffer<float> test_buffer_;        // Pre-allocated for audio callback
    audio::Buffer<float> silence_buffer_;    // Pre-allocated silence for missing input
    std::vector<float*> test_ptrs_;           // Pre-allocated channel pointers
    std::vector<const float*> silence_ptrs_;  // Pre-allocated silence channel pointers
    std::vector<const float*> meter_ptrs_;    // Pre-allocated for meter analysis
};

} // namespace pulp::format
