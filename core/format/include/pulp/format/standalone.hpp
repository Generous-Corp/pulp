#pragma once

#include <pulp/audio/device.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/device.hpp>

#include <atomic>
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
};

class StandaloneApp {
public:
    explicit StandaloneApp(ProcessorFactory factory);
    ~StandaloneApp();

    void set_config(const StandaloneConfig& config) { config_ = config; }

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    bool run_with_editor(bool use_gpu = false);

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
