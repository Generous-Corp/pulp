#include "coreaudio_device.hpp"
#include <pulp/runtime/log.hpp>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace pulp::audio::mac {

namespace {

// kAudioDevicePropertyIOThreadOSWorkgroup is declared in the macOS 11+
// SDK, but Apple's docs only guarantee a usable workgroup on macOS 13 /
// iOS 16+, so this path follows that availability floor.
constexpr AudioObjectPropertySelector kIOThreadWorkgroupSelector =
    kAudioDevicePropertyIOThreadOSWorkgroup;

bool read_nominal_sample_rate(AudioDeviceID device_id, double& sample_rate) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioDevicePropertyNominalSampleRate;
    prop.mScope    = kAudioObjectPropertyScopeGlobal;
    prop.mElement  = kAudioObjectPropertyElementMain;

    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    OSStatus status = AudioObjectGetPropertyData(device_id, &prop, 0, nullptr, &size, &rate);
    if (status != noErr) return false;

    sample_rate = static_cast<double>(rate);
    return sample_rate > 0.0;
}

OSStatus set_nominal_sample_rate(AudioDeviceID device_id, double sample_rate) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioDevicePropertyNominalSampleRate;
    prop.mScope    = kAudioObjectPropertyScopeGlobal;
    prop.mElement  = kAudioObjectPropertyElementMain;

    Float64 rate = static_cast<Float64>(sample_rate);
    UInt32 size = sizeof(rate);
    return AudioObjectSetPropertyData(device_id, &prop, 0, nullptr, size, &rate);
}

bool read_buffer_frame_size(AudioDeviceID device_id, UInt32& frames) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioDevicePropertyBufferFrameSize;
    prop.mScope    = kAudioObjectPropertyScopeGlobal;
    prop.mElement  = kAudioObjectPropertyElementMain;

    UInt32 size = sizeof(frames);
    OSStatus status = AudioObjectGetPropertyData(device_id, &prop, 0, nullptr, &size, &frames);
    return status == noErr && frames > 0;
}

OSStatus set_buffer_frame_size(AudioDeviceID device_id, UInt32 frames) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioDevicePropertyBufferFrameSize;
    prop.mScope    = kAudioObjectPropertyScopeGlobal;
    prop.mElement  = kAudioObjectPropertyElementMain;

    UInt32 size = sizeof(frames);
    return AudioObjectSetPropertyData(device_id, &prop, 0, nullptr, size, &frames);
}

bool device_exists(AudioDeviceID device_id) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioObjectPropertyName;
    prop.mScope    = kAudioObjectPropertyScopeGlobal;
    prop.mElement  = kAudioObjectPropertyElementMain;
    return AudioObjectHasProperty(device_id, &prop);
}

void add_unique_sample_rate(std::vector<double>& rates, double rate) {
    if (rate <= 0.0) return;
    auto matches = [rate](double existing) { return std::abs(existing - rate) < 1.0; };
    if (std::find_if(rates.begin(), rates.end(), matches) == rates.end())
        rates.push_back(rate);
}

}  // namespace

// ── CoreAudioDevice ────────────────────────────────────────────────────────

CoreAudioDevice::CoreAudioDevice(AudioDeviceID device_id)
    : device_id_(device_id)
{
}

CoreAudioDevice::~CoreAudioDevice() {
    if (is_running_) stop();
    if (is_open_) close();
}

void CoreAudioDevice::query_callback_workgroup() {
#if defined(__APPLE__)
    workgroup_ = nullptr;
    if (__builtin_available(macOS 13.0, iOS 16.0, *)) {
        AudioObjectPropertyAddress prop{};
        prop.mSelector = kIOThreadWorkgroupSelector;
        prop.mScope    = kAudioObjectPropertyScopeGlobal;
        prop.mElement  = kAudioObjectPropertyElementMain;

        os_workgroup_t wg = nullptr;
        UInt32 size = sizeof(wg);
        OSStatus status = AudioObjectGetPropertyData(
            device_id_, &prop, 0, nullptr, &size, &wg);
        if (status == noErr && wg != nullptr) {
            workgroup_ = wg;
            runtime::log_info(
                "CoreAudio: extracted IO-thread workgroup for device {}",
                static_cast<unsigned>(device_id_));
        } else if (status != noErr) {
            // Device does not publish a workgroup. The render
            // callback's first entry falls back to Mach RT priority.
            runtime::log_debug(
                "CoreAudio: device {} has no IO workgroup (status {})",
                static_cast<unsigned>(device_id_),
                static_cast<int>(status));
        }
    }
#endif
}

bool CoreAudioDevice::open(const DeviceConfig& config) {
    config_ = config;
    // AUHAL bus 0 is output, bus 1 is input. An input-only open (input channels
    // requested, output channels zero) must disable output IO and drive the unit
    // from an input callback — a zero-channel output stream format is rejected by
    // AUHAL, and the render callback on bus 0 never fires when output is disabled.
    const bool want_output = config_.output_channels > 0;
    const bool want_input  = config_.input_channels  > 0;
    output_enabled_        = want_output;
    const bool input_only  = want_input && !want_output;

    // A unit with neither input nor output has no IO to drive — reject it rather
    // than build a unit that opens but never delivers a callback.
    if (!want_input && !want_output) {
        runtime::log_error("CoreAudio: open requires at least one input or output channel");
        return false;
    }

    // No explicit device requested AND output-only => use the system DefaultOutput
    // unit, which AUTO-FOLLOWS the system default device (and sample-rate-converts).
    // This is what makes switching to AirPods / headphones mid-session keep playing.
    // A pinned device, or any input-capable config, uses HALOutput instead
    // (DefaultOutput has no input bus).
    follow_default_ =
        (device_id_ == kAudioObjectUnknown) && config_.input_channels == 0;
    if (device_id_ == kAudioObjectUnknown) {
        // An input-only open with no pinned device resolves the default INPUT
        // device; every other configuration resolves the default output.
        device_id_ = CoreAudioSystem::get_default_device(input_only);
        if (device_id_ == kAudioObjectUnknown) {
            runtime::log_error("CoreAudio: no default {} device is available",
                input_only ? "input" : "output");
            return false;
        }
    }

    // Create output audio unit (HALOutput, or DefaultOutput when following the
    // system default so a device switch is handled by the unit itself).
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType =
        follow_default_ ? kAudioUnitSubType_DefaultOutput : kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    auto component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        runtime::log_error("CoreAudio: could not find HAL output component");
        return false;
    }

    auto status = AudioComponentInstanceNew(component, &audio_unit_);
    if (status != noErr) {
        runtime::log_error("CoreAudio: could not create audio unit ({})", static_cast<int>(status));
        return false;
    }

    // EnableIO FIRST, before binding CurrentDevice below.
    //
    // AUHAL starts out with output element 0 ENABLED and input element 1 disabled.
    // Binding an output-enabled unit to a device that has NO output streams — a
    // plain USB microphone, an interface input, an aggregate capture device — is
    // rejected with kAudioUnitErr_InvalidPropertyValue (-10851), so an input-only
    // open of any pure-input device failed. It only appeared to work on machines
    // whose default input happened to be a duplex device.
    //
    // Apple's required order is EnableIO -> CurrentDevice -> stream formats ->
    // AudioUnitInitialize. Setting EnableIO here also still satisfies the
    // "before AudioUnitInitialize" constraint the format code below relies on.

    // Enable input on bus 1 if input channels are requested
    input_enabled_ = false;
    if (want_input) {
        UInt32 enable_input = 1;
        status = AudioUnitSetProperty(audio_unit_,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Input, 1,
            &enable_input, sizeof(enable_input));
        if (status != noErr) {
            if (input_only) {
                // Output is also disabled, so a failed input-enable would leave a
                // unit with no IO at all — fatal for an input-only open.
                runtime::log_error("CoreAudio: could not enable input for input-only open ({})",
                    static_cast<int>(status));
                close();
                return false;
            }
            runtime::log_warn("CoreAudio: could not enable input ({})", static_cast<int>(status));
            // Continue without input — effects will receive silence
        } else {
            input_enabled_ = true;
        }
    }

    // Disable output on bus 0 when no output channels are requested. AUHAL rejects
    // a zero-channel output stream format (kAudioUnitErr_FormatNotSupported,
    // -10868), so an input-only unit must turn output IO off rather than set a
    // degenerate format.
    if (!want_output) {
        UInt32 disable_output = 0;
        status = AudioUnitSetProperty(audio_unit_,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Output, 0,
            &disable_output, sizeof(disable_output));
        if (status != noErr) {
            runtime::log_error("CoreAudio: could not disable output IO ({})", static_cast<int>(status));
            close();
            return false;
        }
    }

    // Set the device. Some output-only standalone apps can still play through
    // the system default even when AUHAL refuses kAudioOutputUnitProperty_
    // CurrentDevice for the selected/default device (for example, virtual or
    // interface-routed outputs). In that case fall back to DefaultOutput before
    // treating startup as fatal. Input-capable configs stay on AUHAL because
    // DefaultOutput has no input bus.
    // DefaultOutput follows the system default automatically — only HALOutput needs
    // (and accepts) an explicit CurrentDevice binding. Skipping the bind here is what
    // lets a follow_default unit move to a newly-selected output device live.
    status = follow_default_ ? noErr : AudioUnitSetProperty(audio_unit_,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, 0,
        &device_id_, sizeof(device_id_));
    if (status != noErr) {
        const auto set_device_status = status;
        if (config_.input_channels > 0) {
            runtime::log_error("CoreAudio: could not set device ({})",
                static_cast<int>(set_device_status));
            close();
            return false;
        }

        runtime::log_warn(
            "CoreAudio: could not bind HAL output to device {} ({}); using system default output unit",
            static_cast<unsigned>(device_id_),
            static_cast<int>(set_device_status));
        AudioComponentInstanceDispose(audio_unit_);
        audio_unit_ = nullptr;

        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
        component = AudioComponentFindNext(nullptr, &desc);
        if (!component) {
            runtime::log_error("CoreAudio: could not find DefaultOutput component after HAL failure ({})",
                static_cast<int>(set_device_status));
            return false;
        }

        status = AudioComponentInstanceNew(component, &audio_unit_);
        if (status != noErr) {
            runtime::log_error(
                "CoreAudio: could not create DefaultOutput audio unit after HAL failure (set device {}, create {})",
                static_cast<int>(set_device_status),
                static_cast<int>(status));
            return false;
        }
        device_id_ = CoreAudioSystem::get_default_device(false);
        if (device_id_ == kAudioObjectUnknown) {
            runtime::log_error("CoreAudio: no default output device is available after HAL failure ({})",
                static_cast<int>(set_device_status));
            close();
            return false;
        }
    }

    double actual_rate = 0.0;
    if (read_nominal_sample_rate(device_id_, actual_rate)) {
        if (config_.sample_rate > 0.0 && std::abs(actual_rate - config_.sample_rate) >= 1.0) {
            OSStatus rate_status = set_nominal_sample_rate(device_id_, config_.sample_rate);
            if (rate_status != noErr) {
                runtime::log_warn("CoreAudio: could not set nominal sample rate to {} Hz ({})",
                    config_.sample_rate, static_cast<int>(rate_status));
            }
            if (!read_nominal_sample_rate(device_id_, actual_rate))
                actual_rate = config_.sample_rate;
        }
        config_.sample_rate = actual_rate;
    }

    // The non-interleaved 32-bit float PCM format shared by both buses; the
    // per-bus channel count is stamped in below.
    AudioStreamBasicDescription stream_desc{};
    stream_desc.mSampleRate = config_.sample_rate;
    stream_desc.mFormatID = kAudioFormatLinearPCM;
    stream_desc.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved;
    stream_desc.mBitsPerChannel = 32;
    stream_desc.mChannelsPerFrame = static_cast<UInt32>(config_.output_channels);
    stream_desc.mFramesPerPacket = 1;
    stream_desc.mBytesPerFrame = sizeof(float);
    stream_desc.mBytesPerPacket = sizeof(float);

    // Set output stream format (bus 0, input scope = what we provide to the device).
    // Skipped entirely when output IO is disabled — AUHAL has no bus-0 output
    // stream to configure in that case.
    if (want_output) {
        status = AudioUnitSetProperty(audio_unit_,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input, 0,
            &stream_desc, sizeof(stream_desc));
        if (status != noErr) {
            runtime::log_error("CoreAudio: could not set output stream format ({})", static_cast<int>(status));
            close();
            return false;
        }
    }

    // Set input stream format (bus 1, output scope = what we receive from the device)
    if (input_enabled_) {
        AudioStreamBasicDescription input_desc = stream_desc;
        input_desc.mChannelsPerFrame = static_cast<UInt32>(config_.input_channels);

        status = AudioUnitSetProperty(audio_unit_,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Output, 1,
            &input_desc, sizeof(input_desc));
        if (status != noErr) {
            if (input_only) {
                // No output to fall back on — an input-only unit that cannot set
                // its capture format has nothing left to deliver.
                runtime::log_error("CoreAudio: could not set input stream format for input-only open ({})",
                    static_cast<int>(status));
                close();
                return false;
            }
            runtime::log_warn("CoreAudio: could not set input stream format ({})", static_cast<int>(status));
            input_enabled_ = false;
        }
    }

    // Set buffer size
    UInt32 buffer_size = static_cast<UInt32>(config_.buffer_size);
    OSStatus buffer_status = buffer_size > 0
        ? set_buffer_frame_size(device_id_, buffer_size)
        : kAudio_ParamError;
    if (buffer_status != noErr) {
        runtime::log_warn("CoreAudio: could not set buffer size to {} ({})",
            config_.buffer_size, static_cast<int>(buffer_status));
    }
    UInt32 actual_buffer_size = 0;
    if (read_buffer_frame_size(device_id_, actual_buffer_size))
        config_.buffer_size = static_cast<int>(actual_buffer_size);

    // Pre-allocate input capture buffers (no allocation in audio callback)
    if (input_enabled_) {
        auto in_ch = static_cast<UInt32>(config_.input_channels);
        auto buf_frames = static_cast<UInt32>(config_.buffer_size);

        input_buffer_storage_.resize(in_ch * buf_frames, 0.0f);
        input_ptrs_.resize(in_ch);

        // Build an AudioBufferList for AudioUnitRender
        input_buffer_list_size_ = offsetof(AudioBufferList, mBuffers) + in_ch * sizeof(AudioBuffer);
        input_buffer_list_ = static_cast<AudioBufferList*>(std::malloc(input_buffer_list_size_));
        input_buffer_list_->mNumberBuffers = in_ch;
        for (UInt32 c = 0; c < in_ch; ++c) {
            input_buffer_list_->mBuffers[c].mNumberChannels = 1;
            input_buffer_list_->mBuffers[c].mDataByteSize = buf_frames * sizeof(float);
            input_buffer_list_->mBuffers[c].mData = input_buffer_storage_.data() + c * buf_frames;
            input_ptrs_[c] = static_cast<float*>(input_buffer_list_->mBuffers[c].mData);
        }
    }

    // Install the callback that drives the unit. With output enabled the device
    // pulls from a render callback on bus 0 (which fills output and, if input is
    // enabled, pulls bus 1 via AudioUnitRender). With output disabled bus 0 never
    // fires, so an input-only unit is driven by an input callback that fires when
    // captured frames are ready; that callback pulls bus 1 the same way and hands
    // the caller an empty output view.
    AURenderCallbackStruct callback_struct{};
    callback_struct.inputProc = render_callback;
    callback_struct.inputProcRefCon = this;

    const AudioUnitPropertyID callback_property = want_output
        ? static_cast<AudioUnitPropertyID>(kAudioUnitProperty_SetRenderCallback)
        : static_cast<AudioUnitPropertyID>(kAudioOutputUnitProperty_SetInputCallback);
    // The render callback lives on bus 0's input scope; the input callback is a
    // global unit property.
    const AudioUnitScope callback_scope = want_output
        ? kAudioUnitScope_Input
        : kAudioUnitScope_Global;

    status = AudioUnitSetProperty(audio_unit_,
        callback_property,
        callback_scope, 0,
        &callback_struct, sizeof(callback_struct));
    if (status != noErr) {
        runtime::log_error("CoreAudio: could not set {} callback ({})",
            want_output ? "render" : "input", static_cast<int>(status));
        close();
        return false;
    }

    status = AudioUnitInitialize(audio_unit_);
    if (status != noErr) {
        runtime::log_error("CoreAudio: could not initialize audio unit ({})", static_cast<int>(status));
        close();
        return false;
    }

    // Query the IO-thread workgroup and arm wg_join_ before the callback
    // starts firing. First callback entry joins on the audio thread.
    query_callback_workgroup();
#if defined(__APPLE__)
    if (workgroup_) {
        wg_join_.set_workgroup(workgroup_);
    }
#endif

    // Install a device-overload listener so we can count xruns. The
    // notification fires on a CoreAudio thread; we only increment an
    // atomic, so it's safe.
    {
        AudioObjectPropertyAddress overload_prop{};
        overload_prop.mSelector = kAudioDeviceProcessorOverload;
        overload_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        overload_prop.mElement  = kAudioObjectPropertyElementMain;
        OSStatus overload_status = AudioObjectAddPropertyListener(
            device_id_, &overload_prop, overload_listener, this);
        overload_listener_installed_ = (overload_status == noErr);
        if (!overload_listener_installed_) {
            runtime::log_debug(
                "CoreAudio: device {} did not accept overload listener ({})",
                static_cast<unsigned>(device_id_),
                static_cast<int>(overload_status));
        }
    }

    // Follow the system default output live: when no device was pinned we track
    // kAudioHardwarePropertyDefaultOutputDevice and re-point the unit when the user
    // picks a different output (AirPods/headphones) WITHOUT relaunching. DefaultOutput
    // alone only resolves the default at open; this listener makes it move live.
    if (follow_default_) {
        AudioObjectPropertyAddress def_prop{};
        def_prop.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        def_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        def_prop.mElement  = kAudioObjectPropertyElementMain;
        OSStatus def_status = AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &def_prop, default_output_changed_listener, this);
        default_output_listener_installed_ = (def_status == noErr);
        if (!default_output_listener_installed_)
            runtime::log_warn("CoreAudio: default-output-device listener not installed ({})",
                static_cast<int>(def_status));
    }

    is_open_ = true;
    runtime::log_info("CoreAudio: opened device '{}' at {} Hz, buffer {}, input {}ch, output {}ch{}",
        info().name, config_.sample_rate, config_.buffer_size,
        input_enabled_ ? config_.input_channels : 0,
        output_enabled_ ? config_.output_channels : 0,
        follow_default_ ? " (follows system default)" : "");
    return true;
}

// Live-switch the unit to the current system default output device. Runs on the
// CoreAudio property-listener thread; switch_mutex_ serializes it against
// stop()/close() so the unit is never disposed mid-switch.
void CoreAudioDevice::switch_to_default_output() {
    std::lock_guard<std::mutex> lock(switch_mutex_);
    if (!is_open_ || !audio_unit_ || !follow_default_) return;
    const AudioDeviceID new_default = CoreAudioSystem::get_default_device(false);
    if (new_default == kAudioObjectUnknown || new_default == device_id_) return;

    const bool was_running = is_running_;
    if (was_running) AudioOutputUnitStop(audio_unit_);
    OSStatus st = AudioUnitSetProperty(audio_unit_,
        kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
        &new_default, sizeof(new_default));
    if (st == noErr) {
        device_id_ = new_default;
        runtime::log_info("CoreAudio: default output changed -> following to device {}",
            static_cast<unsigned>(new_default));
    } else {
        runtime::log_warn("CoreAudio: could not follow to new default output ({})",
            static_cast<int>(st));
    }
    if (was_running) AudioOutputUnitStart(audio_unit_);
}

OSStatus CoreAudioDevice::default_output_changed_listener(
    AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* client) {
    if (auto* self = static_cast<CoreAudioDevice*>(client)) self->switch_to_default_output();
    return noErr;
}

void CoreAudioDevice::close() {
    // Stop new default-change callbacks from firing, then take switch_mutex_ so any
    // in-flight switch_to_default_output() finishes before we dispose the unit.
    if (default_output_listener_installed_) {
        AudioObjectPropertyAddress def_prop{};
        def_prop.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        def_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        def_prop.mElement  = kAudioObjectPropertyElementMain;
        AudioObjectRemovePropertyListener(
            kAudioObjectSystemObject, &def_prop, default_output_changed_listener, this);
        default_output_listener_installed_ = false;
    }
    std::lock_guard<std::mutex> switch_lock(switch_mutex_);

    // Tear the callback path down first. AudioUnitUninitialize blocks
    // until any in-flight render_callback returns — combined with
    // is_running_ being cleared in stop(), this gives the manager's
    // "callback won't re-fire after close()" guarantee.
    if (overload_listener_installed_) {
        AudioObjectPropertyAddress overload_prop{};
        overload_prop.mSelector = kAudioDeviceProcessorOverload;
        overload_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        overload_prop.mElement  = kAudioObjectPropertyElementMain;
        AudioObjectRemovePropertyListener(
            device_id_, &overload_prop, overload_listener, this);
        overload_listener_installed_ = false;
    }
    if (audio_unit_) {
        AudioUnitUninitialize(audio_unit_);
        AudioComponentInstanceDispose(audio_unit_);
        audio_unit_ = nullptr;
    }
    // Leave the workgroup (no-op if never joined). After
    // AudioUnitUninitialize returns no callback can still be in
    // flight, so this is race-free.
    wg_join_.leave();
    wg_joined_.store(false, std::memory_order_relaxed);
#if defined(__APPLE__)
    workgroup_ = nullptr;
#endif
    if (input_buffer_list_) {
        std::free(input_buffer_list_);
        input_buffer_list_ = nullptr;
    }
    input_buffer_storage_.clear();
    input_ptrs_.clear();
    input_enabled_ = false;
    output_enabled_ = true;
    is_open_ = false;
}

bool CoreAudioDevice::start(AudioCallback callback) {
    if (!is_open_) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;

    auto status = AudioOutputUnitStart(audio_unit_);
    if (status != noErr) {
        runtime::log_error("CoreAudio: could not start ({})", static_cast<int>(status));
        return false;
    }
    is_running_ = true;
    return true;
}

void CoreAudioDevice::stop() {
    // Serialize against switch_to_default_output() so a default-change can't restart
    // the unit after we stop it (or operate on it as we tear down).
    std::lock_guard<std::mutex> switch_lock(switch_mutex_);
    if (audio_unit_ && is_running_) {
        // AudioOutputUnitStop blocks until the I/O thread observes
        // the stop request. Subsequent callbacks observe
        // `is_running_ == false` and bail before invoking `callback_`.
        AudioOutputUnitStop(audio_unit_);
    }
    is_running_ = false;
    callback_ = nullptr;
}

DeviceInfo CoreAudioDevice::info() const {
    return CoreAudioSystem::query_device_info(device_id_);
}

OSStatus CoreAudioDevice::render_callback(
    void* inRefCon,
    AudioUnitRenderActionFlags* ioActionFlags,
    const AudioTimeStamp* inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList* ioData)
{
    auto* self = static_cast<CoreAudioDevice*>(inRefCon);

    // First-entry: join the audio device's workgroup on the render
    // thread. acquire/release on `wg_joined_` keeps the join visible
    // to subsequent renders without a CAS hot path. The boolean also
    // means join is at most once per render thread.
    if (!self->wg_joined_.load(std::memory_order_acquire)) {
        // join_from_audio_thread() returns immediately if there's no
        // workgroup set; it falls back to Mach realtime priority in
        // that case so the thread is still RT-tagged.
        //
        // Only flip `wg_joined_` when the join (or fallback RT-priority
        // bump) actually succeeded. The previous unconditional store
        // permanently disabled all future join attempts after the first
        // transient failure (e.g. workgroup setup hadn't completed yet
        // on the first render), so the audio thread could run the entire
        // session without workgroup membership or any RT priority bump.
        if (self->wg_join_.join_from_audio_thread()) {
            self->wg_joined_.store(true, std::memory_order_release);
        }
    }

    // An input-only unit is driven by an input callback: ioData is null (there is
    // no output buffer to fill), and the callback receives an empty output view.
    // A render-callback (output-enabled) unit always supplies ioData.
    const bool has_output = (ioData != nullptr);

    if (!self->callback_) {
        // Silence the output buffer when there is one; an input-only callback has
        // nothing to zero.
        if (has_output) {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i) {
                std::memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
            }
        }
        return noErr;
    }

    // Build output buffer view from CoreAudio's buffer list. Left empty (zero
    // channels) for an input-only unit.
    BufferView<float> output_view;
    if (has_output) {
        self->output_ptrs_.resize(ioData->mNumberBuffers);
        for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i) {
            self->output_ptrs_[i] = static_cast<float*>(ioData->mBuffers[i].mData);
        }
        output_view = BufferView<float>(self->output_ptrs_.data(),
            self->output_ptrs_.size(), inNumberFrames);
    }

    // Capture input from bus 1 if enabled
    BufferView<const float> input_view;
    if (self->input_enabled_ && self->input_buffer_list_) {
        // Reset buffer sizes in case CoreAudio changed them
        auto in_ch = static_cast<UInt32>(self->config_.input_channels);
        for (UInt32 c = 0; c < in_ch; ++c) {
            self->input_buffer_list_->mBuffers[c].mDataByteSize = inNumberFrames * sizeof(float);
        }

        // Pull input with its own action-flags slot so flags written by the
        // bus-1 input render never leak back into the bus-0 output render on the
        // duplex path.
        AudioUnitRenderActionFlags input_flags = 0;
        OSStatus input_status = AudioUnitRender(
            self->audio_unit_, &input_flags, inTimeStamp,
            1,  // Bus 1 = input
            inNumberFrames,
            self->input_buffer_list_);

        if (input_status == noErr) {
            input_view = BufferView<const float>(
                const_cast<const float**>(self->input_ptrs_.data()),
                self->input_ptrs_.size(), inNumberFrames);
        }
        // On error, input_view remains empty (silence) — don't crash
    }

    CallbackContext ctx;
    ctx.sample_rate = self->config_.sample_rate;
    ctx.buffer_size = static_cast<int>(inNumberFrames);
    ctx.sample_position = self->sample_position_;

    self->callback_(input_view, output_view, ctx);
    self->sample_position_ += inNumberFrames;

    return noErr;
}

OSStatus CoreAudioDevice::overload_listener(
    AudioObjectID /*inObjectID*/,
    UInt32 /*inNumberAddresses*/,
    const AudioObjectPropertyAddress* /*inAddresses*/,
    void* inClientData)
{
    auto* self = static_cast<CoreAudioDevice*>(inClientData);
    if (self) {
        self->xrun_counter_.fetch_add(1, std::memory_order_relaxed);
    }
    return noErr;
}

// ── CoreAudioSystem ────────────────────────────────────────────────────────

CoreAudioSystem::CoreAudioSystem() = default;

CoreAudioSystem::~CoreAudioSystem() {
    if (listener_installed_) {
        AudioObjectPropertyAddress prop{};
        prop.mSelector = kAudioHardwarePropertyDevices;
        prop.mScope = kAudioObjectPropertyScopeGlobal;
        prop.mElement = kAudioObjectPropertyElementMain;
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &prop,
                                          device_list_changed, this);
    }
    if (default_listener_installed_) {
        AudioObjectPropertyAddress out_prop{};
        out_prop.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        out_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        out_prop.mElement  = kAudioObjectPropertyElementMain;
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &out_prop,
                                          default_device_changed, this);

        AudioObjectPropertyAddress in_prop = out_prop;
        in_prop.mSelector = kAudioHardwarePropertyDefaultInputDevice;
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &in_prop,
                                          default_device_changed, this);
    }
}

void CoreAudioSystem::set_device_change_callback(DeviceChangeCallback cb) {
    device_change_cb_ = std::move(cb);
    // Also store the callback in the base class so
    // `AudioSystem::fire_device_change()` finds it for cross-backend
    // helpers (e.g. AudioDeviceManager wiring).
    AudioSystem::set_device_change_callback(device_change_cb_);

    if (device_change_cb_ && !listener_installed_) {
        AudioObjectPropertyAddress prop{};
        prop.mSelector = kAudioHardwarePropertyDevices;
        prop.mScope = kAudioObjectPropertyScopeGlobal;
        prop.mElement = kAudioObjectPropertyElementMain;
        auto status = AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &prop, device_list_changed, this);
        if (status == noErr) {
            listener_installed_ = true;
        }
    }
}

void CoreAudioSystem::set_default_device_change_callback(DefaultDeviceChangeCallback cb) {
    default_device_change_cb_ = std::move(cb);

    if (default_device_change_cb_ && !default_listener_installed_) {
        AudioObjectPropertyAddress out_prop{};
        out_prop.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        out_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        out_prop.mElement  = kAudioObjectPropertyElementMain;
        OSStatus s1 = AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &out_prop, default_device_changed, this);

        AudioObjectPropertyAddress in_prop = out_prop;
        in_prop.mSelector = kAudioHardwarePropertyDefaultInputDevice;
        OSStatus s2 = AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &in_prop, default_device_changed, this);

        if (s1 == noErr || s2 == noErr) {
            default_listener_installed_ = true;
        }
    }
}

OSStatus CoreAudioSystem::device_list_changed(
    AudioObjectID, UInt32,
    const AudioObjectPropertyAddress*, void* inClientData)
{
    auto* self = static_cast<CoreAudioSystem*>(inClientData);
    if (self->device_change_cb_) {
        self->device_change_cb_();
    }
    return noErr;
}

OSStatus CoreAudioSystem::default_device_changed(
    AudioObjectID,
    UInt32 nAddresses,
    const AudioObjectPropertyAddress* addresses,
    void* inClientData)
{
    auto* self = static_cast<CoreAudioSystem*>(inClientData);
    if (!self || !self->default_device_change_cb_) return noErr;

    bool is_input = false;
    for (UInt32 i = 0; i < nAddresses; ++i) {
        if (addresses[i].mSelector == kAudioHardwarePropertyDefaultInputDevice) {
            is_input = true;
            break;
        }
    }
    self->default_device_change_cb_(is_input);
    return noErr;
}

AudioDeviceID CoreAudioSystem::get_default_device(bool input) {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = input ? kAudioHardwarePropertyDefaultInputDevice
                           : kAudioHardwarePropertyDefaultOutputDevice;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;

    AudioDeviceID device_id = kAudioObjectUnknown;
    UInt32 size = sizeof(device_id);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, &device_id);
    return device_id;
}

DeviceInfo CoreAudioSystem::query_device_info(AudioDeviceID device_id) {
    DeviceInfo info;
    info.id = std::to_string(device_id);

    // Get name
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioObjectPropertyName;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;

    CFStringRef name_ref = nullptr;
    UInt32 size = sizeof(name_ref);
    if (AudioObjectGetPropertyData(device_id, &prop, 0, nullptr, &size, &name_ref) == noErr && name_ref) {
        char buf[256];
        CFStringGetCString(name_ref, buf, sizeof(buf), kCFStringEncodingUTF8);
        info.name = buf;
        CFRelease(name_ref);
    }

    // Get output channel count
    prop.mSelector = kAudioDevicePropertyStreamConfiguration;
    prop.mScope = kAudioObjectPropertyScopeOutput;
    size = 0;
    AudioObjectGetPropertyDataSize(device_id, &prop, 0, nullptr, &size);
    if (size > 0) {
        std::vector<uint8_t> buf(size);
        auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
        if (AudioObjectGetPropertyData(device_id, &prop, 0, nullptr, &size, list) == noErr) {
            for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
                info.max_output_channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
            }
        }
    }

    // Get input channel count
    prop.mScope = kAudioObjectPropertyScopeInput;
    size = 0;
    AudioObjectGetPropertyDataSize(device_id, &prop, 0, nullptr, &size);
    if (size > 0) {
        std::vector<uint8_t> buf(size);
        auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
        if (AudioObjectGetPropertyData(device_id, &prop, 0, nullptr, &size, list) == noErr) {
            for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
                info.max_input_channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
            }
        }
    }

    // Check if default
    info.is_default_output = (device_id == get_default_device(false));
    info.is_default_input = (device_id == get_default_device(true));

    // Get supported sample rates
    prop.mSelector = kAudioDevicePropertyAvailableNominalSampleRates;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    size = 0;
    AudioObjectGetPropertyDataSize(device_id, &prop, 0, nullptr, &size);
    if (size > 0) {
        static constexpr double kCommonRates[] = {
            44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
        };
        auto count = size / sizeof(AudioValueRange);
        std::vector<AudioValueRange> ranges(count);
        if (AudioObjectGetPropertyData(device_id, &prop, 0, nullptr, &size, ranges.data()) == noErr) {
            for (const auto& r : ranges) {
                if (r.mMinimum == r.mMaximum) {
                    add_unique_sample_rate(info.sample_rates, r.mMinimum);
                } else {
                    for (double common : kCommonRates) {
                        if (common >= r.mMinimum && common <= r.mMaximum)
                            add_unique_sample_rate(info.sample_rates, common);
                    }
                }
            }
        }
    }
    double current_rate = 0.0;
    if (read_nominal_sample_rate(device_id, current_rate))
        add_unique_sample_rate(info.sample_rates, current_rate);
    std::sort(info.sample_rates.begin(), info.sample_rates.end());

    return info;
}

std::vector<DeviceInfo> CoreAudioSystem::enumerate_devices() {
    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioHardwarePropertyDevices;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;

    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);

    auto count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> device_ids(count);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, device_ids.data());

    std::vector<DeviceInfo> devices;
    devices.reserve(count);
    for (auto id : device_ids) {
        devices.push_back(query_device_info(id));
    }
    return devices;
}

std::unique_ptr<AudioDevice> CoreAudioSystem::create_device(const std::string& device_id) {
    // Leave the id as kAudioObjectUnknown for the empty/invalid (no explicit pin)
    // case: CoreAudioDevice::open() then resolves the current default AND marks the
    // unit follow_default_, so it tracks the system default output LIVE (AirPods /
    // headphones mid-session). Resolving to a concrete id here would pin the unit to
    // whatever was default at launch and it would only "follow" on relaunch.
    AudioDeviceID id = kAudioObjectUnknown;
    if (!device_id.empty()) {
        try {
            auto parsed = static_cast<AudioDeviceID>(std::stoul(device_id));
            if (device_exists(parsed)) {
                id = parsed;  // explicit, valid pin
            } else {
                runtime::log_warn("CoreAudio: saved device '{}' is unavailable; following the default output",
                    device_id);
            }
        } catch (...) {
            runtime::log_warn("CoreAudio: saved device '{}' is invalid; following the default output",
                device_id);
        }
    }
    return std::make_unique<CoreAudioDevice>(id);
}

DeviceInfo CoreAudioSystem::default_output_device() {
    return query_device_info(get_default_device(false));
}

DeviceInfo CoreAudioSystem::default_input_device() {
    return query_device_info(get_default_device(true));
}

} // namespace pulp::audio::mac

// Factory function
namespace pulp::audio {

std::unique_ptr<AudioSystem> create_audio_system() {
    return std::make_unique<mac::CoreAudioSystem>();
}

} // namespace pulp::audio
