#include "coreaudio_device.hpp"
#include <pulp/runtime/log.hpp>

#include <memory>
#include <mutex>
#include <vector>

namespace pulp::audio::mac {

bool coreaudio_device_exists(AudioDeviceID device_id) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioObjectPropertyName;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;
    return AudioObjectHasProperty(device_id, &prop);
}

namespace {

bool read_device_frame_property(
    AudioDeviceID device_id,
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope,
    std::uint32_t& frames) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = selector;
    prop.mScope = scope;
    prop.mElement = kAudioObjectPropertyElementMain;

    UInt32 value = 0;
    UInt32 size = sizeof(value);
    const OSStatus status = AudioObjectGetPropertyData(
        device_id, &prop, 0, nullptr, &size, &value);
    if (status != noErr) return false;
    frames = static_cast<std::uint32_t>(value);
    return true;
}

} // namespace

bool read_coreaudio_nominal_sample_rate(
    AudioDeviceID device_id,
    double& sample_rate) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioDevicePropertyNominalSampleRate;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;

    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    const OSStatus status = AudioObjectGetPropertyData(
        device_id, &prop, 0, nullptr, &size, &rate);
    if (status != noErr) return false;

    sample_rate = static_cast<double>(rate);
    return is_supported_audio_sample_rate(sample_rate);
}

std::optional<AudioIoTiming> CoreAudioDevice::query_audio_io_timing(
    AudioDeviceID device_id,
    std::uint64_t route_instance_token,
    std::uint64_t calibration_generation) {
    if (device_id == kAudioObjectUnknown || route_instance_token == 0 ||
        calibration_generation == 0 || !coreaudio_device_exists(device_id)) {
        return std::nullopt;
    }

    CoreAudioTimingPropertyValues values{};
    if (!read_coreaudio_nominal_sample_rate(
            device_id, values.sample_rate_hz)) {
        return std::nullopt;
    }

    std::uint32_t buffer_frames = 0;
    if (!read_device_frame_property(
            device_id, kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal, buffer_frames) ||
        buffer_frames == 0) {
        return std::nullopt;
    }
    values.io_buffer_frames = buffer_frames;

    std::uint32_t value = 0;
    if (read_device_frame_property(
            device_id, kAudioDevicePropertyLatency,
            kAudioObjectPropertyScopeInput, value)) {
        values.input_latency_frames = value;
    }
    if (read_device_frame_property(
            device_id, kAudioDevicePropertyLatency,
            kAudioObjectPropertyScopeOutput, value)) {
        values.output_latency_frames = value;
    }
    if (read_device_frame_property(
            device_id, kAudioDevicePropertySafetyOffset,
            kAudioObjectPropertyScopeInput, value)) {
        values.input_safety_offset_frames = value;
    }
    if (read_device_frame_property(
            device_id, kAudioDevicePropertySafetyOffset,
            kAudioObjectPropertyScopeOutput, value)) {
        values.output_safety_offset_frames = value;
    }
    return make_coreaudio_audio_io_timing(
        values, route_instance_token, calibration_generation);
}

std::optional<AudioIoTiming> CoreAudioDevice::audio_io_timing() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);
    if (!is_open_) return std::nullopt;
    if (audio_io_timing_dirty_.load(std::memory_order_acquire)) {
        refresh_audio_io_timing_locked();
    }
    return audio_io_timing_;
}

void CoreAudioDevice::mark_audio_io_timing_stale() noexcept {
    if (!advance_coreaudio_timing_generation(calibration_generation_)) {
        calibration_generation_exhausted_.store(
            true, std::memory_order_release);
    }
    audio_io_timing_dirty_.store(true, std::memory_order_release);
}

void CoreAudioDevice::refresh_audio_io_timing_locked() const {
    audio_io_timing_ = refresh_coreaudio_timing_with_listener_coverage(
        calibration_generation_, calibration_generation_exhausted_,
        audio_io_timing_dirty_, timing_listener_installed_,
        [this](std::uint64_t generation) {
            return query_audio_io_timing(
                device_id_, route_instance_token_, generation);
        });
}

void CoreAudioDevice::install_audio_io_timing_listeners_locked() {
    timing_property_addresses_ = {{
        {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
    }};
    if (!timing_listener_context_) {
        timing_listener_context_ = std::make_unique<CoreAudioTimingListenerContext>(
            this, [](void* owner) noexcept {
                static_cast<CoreAudioDevice*>(owner)->mark_audio_io_timing_stale();
            });
    } else {
        timing_listener_context_->attach(this);
    }
    for (std::size_t i = 0; i < timing_property_addresses_.size(); ++i) {
        timing_listener_installed_[i] = AudioObjectAddPropertyListener(
            device_id_, &timing_property_addresses_[i],
            audio_io_timing_changed_listener,
            timing_listener_context_.get()) == noErr;
    }
}

namespace {

void retain_retired_timing_listener_context(
    std::unique_ptr<CoreAudioTimingListenerContext> context) {
    // CoreAudio does not document a synchronous no-late-entry guarantee for
    // AudioObjectRemovePropertyListener. Keep one detached context per device
    // for process lifetime so even a callback dispatched before unregister but
    // entering after the in-flight drain cannot touch freed client-data.
    static auto* mutex = new std::mutex;
    static auto* contexts =
        new std::vector<std::unique_ptr<CoreAudioTimingListenerContext>>;
    std::lock_guard<std::mutex> lock(*mutex);
    contexts->push_back(std::move(context));
}

} // namespace

void CoreAudioDevice::remove_audio_io_timing_listeners_locked() {
    if (!timing_listener_context_) return;

    timing_listener_context_->detach_and_wait();
    const auto device_id = device_id_;
    const bool complete = remove_coreaudio_timing_listener_registrations(
        timing_listener_installed_, [&, device_id](std::size_t i) {
            const auto status = AudioObjectRemovePropertyListener(
                device_id, &timing_property_addresses_[i],
                audio_io_timing_changed_listener,
                timing_listener_context_.get());
            if (status != noErr) {
                runtime::log_warn(
                    "CoreAudio: timing listener {} could not be removed ({})",
                    i, static_cast<int>(status));
            }
            return status == noErr;
        });
    timing_listener_context_->detach_and_wait();
    if (!coreaudio_timing_listener_context_reusable(complete)) {
        runtime::log_warn(
            "CoreAudio: detached timing context retained after incomplete listener removal");
        retain_retired_timing_listener_context(
            std::move(timing_listener_context_));
    }
    timing_listener_installed_.fill(false);
}

void CoreAudioDevice::retire_audio_io_timing_listener_context() {
    if (!timing_listener_context_) return;
    timing_listener_context_->detach_and_wait();
    retain_retired_timing_listener_context(std::move(timing_listener_context_));
}

OSStatus CoreAudioDevice::audio_io_timing_changed_listener(
    AudioObjectID,
    UInt32,
    const AudioObjectPropertyAddress*,
    void* inClientData) {
    if (auto* context =
            static_cast<CoreAudioTimingListenerContext*>(inClientData)) {
        // CoreAudio owns this callback thread. Do not query properties or take
        // the device-switch mutex here: mark the cache stale and let the next
        // control-thread snapshot refresh under the existing serialization.
        context->notify();
    }
    return noErr;
}

} // namespace pulp::audio::mac

namespace pulp::audio {

std::optional<AudioIoTiming> query_audio_io_timing(
    const AudioDevice& device) noexcept {
    const auto* coreaudio = dynamic_cast<const mac::CoreAudioDevice*>(&device);
    return coreaudio ? coreaudio->audio_io_timing() : std::nullopt;
}

} // namespace pulp::audio
