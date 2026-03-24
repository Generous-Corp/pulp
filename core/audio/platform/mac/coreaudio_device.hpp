#pragma once

#include <pulp/audio/device.hpp>
#include <AudioToolbox/AudioToolbox.h>

namespace pulp::audio::mac {

class CoreAudioDevice : public AudioDevice {
public:
    CoreAudioDevice(AudioDeviceID device_id);
    ~CoreAudioDevice() override;

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return is_running_; }
    DeviceInfo info() const override;
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }

private:
    static OSStatus render_callback(
        void* inRefCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp* inTimeStamp,
        UInt32 inBusNumber,
        UInt32 inNumberFrames,
        AudioBufferList* ioData);

    AudioDeviceID device_id_;
    AudioComponentInstance audio_unit_ = nullptr;
    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    bool is_running_ = false;
    uint64_t sample_position_ = 0;

    // Buffers for the callback
    std::vector<float*> output_ptrs_;
    std::vector<float*> input_ptrs_;
};

class CoreAudioSystem : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;

    static DeviceInfo query_device_info(AudioDeviceID device_id);
    static AudioDeviceID get_default_device(bool input);
};

} // namespace pulp::audio::mac
