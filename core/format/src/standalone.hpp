#pragma once

// Standalone application wrapper for Pulp plugins
// Creates a native window with audio device selection and routes audio
// through the plugin's Processor.

#include <pulp/format/processor.hpp>
#include <pulp/audio/device.hpp>
#include <pulp/midi/device.hpp>
#include <memory>
#include <atomic>
#include <string>

namespace pulp::format {

// Standalone app configuration
struct StandaloneConfig {
    std::string audio_device_id;  // Empty = default
    std::string midi_input_id;    // Empty = first available
    double sample_rate = 48000.0;
    int buffer_size = 256;
    int output_channels = 2;
    int input_channels = 0;       // 0 = no input (instrument mode)
};

// Runs a Pulp plugin as a standalone application
// Handles audio device setup, MIDI routing, and the main run loop
class StandaloneApp {
public:
    StandaloneApp(ProcessorFactory factory);
    ~StandaloneApp();

    // Configure before running
    void set_config(const StandaloneConfig& config) { config_ = config; }

    // Start audio and run (blocks until stop() is called)
    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Start audio and show an AutoUi editor window (blocks until window closes)
    // Uses GPU rendering when use_gpu is true and Skia/Dawn are available
    bool run_with_editor(bool use_gpu = false);

    // Access the processor (for testing or headless use)
    Processor* processor() { return processor_.get(); }
    state::StateStore& state() { return store_; }

private:
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
    std::atomic<bool> running_{false};
};

} // namespace pulp::format
