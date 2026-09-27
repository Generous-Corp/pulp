#include "coreaudio_device.hpp"
#include <pulp/runtime/log.hpp>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace pulp::audio::mac {


struct CoreAudioNativeRetirement {
    struct Listener {
        bool installed = false;
        AudioObjectID object = 0;
        AudioObjectPropertyAddress address{};
        AudioObjectPropertyListenerProc callback = nullptr;
        void* context = nullptr;
    };
    CoreAudioNativeOperations ops;
    AudioUnit unit = nullptr;
    bool running = false;
    bool initialized = false;
    std::array<Listener, 8> listeners{};
    CoreAudioNativeRetirement* next = nullptr;
};

namespace {

CoreAudioNativeOperations native_operations() {
    return {
        nullptr,
        [](void*, AudioUnit unit) { return AudioOutputUnitStart(unit); },
        [](void*, AudioUnit unit) { return AudioOutputUnitStop(unit); },
        [](void*, AudioUnit unit) { return AudioUnitUninitialize(unit); },
        [](void*, AudioUnit unit) { return AudioComponentInstanceDispose(unit); },
        [](void*, AudioUnit unit, bool input_only, const AURenderCallbackStruct& callback) {
            return AudioUnitSetProperty(unit,
                input_only ? static_cast<AudioUnitPropertyID>(kAudioOutputUnitProperty_SetInputCallback)
                           : static_cast<AudioUnitPropertyID>(kAudioUnitProperty_SetRenderCallback),
                input_only ? kAudioUnitScope_Global : kAudioUnitScope_Input,
                0, &callback, sizeof(callback));
        },
        [](void*, AudioObjectID object, const AudioObjectPropertyAddress& address,
           AudioObjectPropertyListenerProc callback, void* context) {
            return AudioObjectRemovePropertyListener(object, &address, callback, context);
        },
        [](void*, AudioObjectID object, const AudioObjectPropertyAddress& address,
           AudioObjectPropertyListenerProc callback, void* context) {
            return AudioObjectAddPropertyListener(object, &address, callback, context);
        },
    };
}

void retain_closed_context(std::unique_ptr<CoreAudioCallbackContext> context) noexcept {
    if (!context) return;
    context->close_and_wait();
    // A queued OS listener may enter after unregister. Preserve its small,
    // permanently closed refcon; it can no longer dereference the owner.
    static std::atomic<CoreAudioCallbackContext*> retired{nullptr};
    auto* node = context.release();
    auto* head = retired.load(std::memory_order_relaxed);
    do { node->retired_next = head; }
    while (!retired.compare_exchange_weak(head, node,
        std::memory_order_release, std::memory_order_relaxed));
}

std::mutex& retirement_mutex() { static auto* mutex = new std::mutex; return *mutex; }
CoreAudioNativeRetirement*& retired_native_head() {
    static CoreAudioNativeRetirement* head = nullptr;
    return head;
}

CoreAudioTeardownResult retire_native(CoreAudioNativeRetirement& state) {
    using Stage = CoreAudioTeardownResult::Stage;
    OSStatus listener_error = noErr;
    for (auto& listener : state.listeners) {
        if (!listener.installed) continue;
        const auto status = state.ops.remove_listener(state.ops.context, listener.object,
            listener.address, listener.callback, listener.context);
        if (status == noErr) listener.installed = false;
        else listener_error = status;
    }
    if (state.unit && state.running) {
        const auto status = state.ops.stop(state.ops.context, state.unit);
        if (status != noErr) return {Stage::Stop, status};
        state.running = false;
    }
    if (state.unit && state.initialized) {
        const auto status = state.ops.uninitialize(state.ops.context, state.unit);
        if (status != noErr) return {Stage::Uninitialize, status};
        state.initialized = false;
    }
    if (state.unit) {
        const auto status = state.ops.dispose(state.ops.context, state.unit);
        if (status != noErr) return {Stage::Dispose, status};
        state.unit = nullptr;
    }
    if (listener_error != noErr) return {Stage::Listener, listener_error};
    return {};
}

// kAudioDevicePropertyIOThreadOSWorkgroup is declared in the macOS 11+
// SDK, but Apple's docs only guarantee a usable workgroup on macOS 13 /
// iOS 16+, so this path follows that availability floor.
constexpr AudioObjectPropertySelector kIOThreadWorkgroupSelector =
    kAudioDevicePropertyIOThreadOSWorkgroup;

std::uint64_t mint_route_instance_token() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto token = next.load(std::memory_order_relaxed);
    while (token != 0) {
        constexpr auto max = std::numeric_limits<std::uint64_t>::max();
        const auto successor = token == max ? 0 : token + 1;
        if (next.compare_exchange_weak(
                token, successor, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return token;
        }
    }
    return 0;
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

void add_unique_sample_rate(std::vector<double>& rates, double rate) {
    if (rate <= 0.0) return;
    auto matches = [rate](double existing) { return std::abs(existing - rate) < 1.0; };
    if (std::find_if(rates.begin(), rates.end(), matches) == rates.end())
        rates.push_back(rate);
}

}  // namespace

// ── CoreAudioDevice ────────────────────────────────────────────────────────

#if defined(__APPLE__)
void CoreAudioWorkgroupReference::release_os_workgroup(
    os_workgroup_t value) noexcept {
    os_release(value);
}
#endif

CoreAudioDevice::CoreAudioDevice(AudioDeviceID device_id)
    : native_ops_(native_operations()),
      retirement_(std::make_unique<CoreAudioNativeRetirement>()),
      device_id_(device_id),
      route_instance_token_(mint_route_instance_token())
{
}

CoreAudioDevice::~CoreAudioDevice() {
    (void)close_checked();
    retire_audio_io_timing_listener_context();
    quarantine_native_ownership();
    retain_closed_context(std::move(render_context_));
    retain_closed_context(std::move(listener_context_));
}

void CoreAudioDevice::query_callback_workgroup() {
#if defined(__APPLE__)
    // The kAudioDevicePropertyIOThreadOSWorkgroup contract returns a retained
    // object to the caller. Every successful query therefore contributes
    // exactly one reference. Callers only invoke this after the old callback
    // and every auxiliary client has left, so replacement can release the
    // previous query reference here without racing a join token.
    workgroup_reference_.reset();
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
            workgroup_reference_.adopt(wg);
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
    require_control_thread();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return open_locked(config);
}

bool CoreAudioDevice::open_locked(const DeviceConfig& config) {
    // Never overwrite uncertain ownership from a previous failed close.
    if (audio_unit_ || is_open_ || default_output_listener_installed_ ||
        overload_listener_installed_) return false;
    retain_closed_context(std::move(listener_context_));
    listener_context_ = std::make_unique<CoreAudioCallbackContext>(this);
    workgroup_changes_quiesced_ = false;
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
                (void)close_locked();
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
            (void)close_locked();
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
            (void)close_locked();
            return false;
        }

        runtime::log_warn(
            "CoreAudio: could not bind HAL output to device {} ({}); using system default output unit",
            static_cast<unsigned>(device_id_),
            static_cast<int>(set_device_status));
        if (native_ops_.dispose(native_ops_.context, audio_unit_) != noErr) {
            (void)close_locked();
            return false;
        }
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
            (void)close_locked();
            return false;
        }
    }

    double actual_rate = 0.0;
    if (read_coreaudio_nominal_sample_rate(device_id_, actual_rate)) {
        if (config_.sample_rate > 0.0 && std::abs(actual_rate - config_.sample_rate) >= 1.0) {
            OSStatus rate_status = set_nominal_sample_rate(device_id_, config_.sample_rate);
            if (rate_status != noErr) {
                runtime::log_warn("CoreAudio: could not set nominal sample rate to {} Hz ({})",
                    config_.sample_rate, static_cast<int>(rate_status));
            }
            if (!read_coreaudio_nominal_sample_rate(device_id_, actual_rate))
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
            (void)close_locked();
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
                (void)close_locked();
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
        input_buffer_frames_ = buf_frames;
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
    render_context_ = std::make_unique<CoreAudioCallbackContext>(this);
    callback_struct.inputProcRefCon = render_context_.get();

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
        (void)close_locked();
        return false;
    }

    status = AudioUnitInitialize(audio_unit_);
    if (status != noErr) {
        runtime::log_error("CoreAudio: could not initialize audio unit ({})", static_cast<int>(status));
        (void)close_locked();
        return false;
    }

    unit_initialized_ = true;

    {
        std::lock_guard<std::mutex> timing_lock(switch_mutex_);
        install_audio_io_timing_listeners_locked();
        mark_audio_io_timing_stale();
        refresh_audio_io_timing_locked();
    }

    // Query and retain the IO-thread workgroup before callbacks start firing.
    // The CoreAudio I/O thread is already a member; the cached reference is
    // published only to auxiliary clients and remains valid until they drain.
    query_callback_workgroup();

    // Install a device-overload listener so we can count xruns. The
    // notification fires on a CoreAudio thread; we only increment an
    // atomic, so it's safe.
    {
        AudioObjectPropertyAddress overload_prop{};
        overload_prop.mSelector = kAudioDeviceProcessorOverload;
        overload_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        overload_prop.mElement  = kAudioObjectPropertyElementMain;
        OSStatus overload_status = AudioObjectAddPropertyListener(
            device_id_, &overload_prop, overload_listener, listener_context_.get());
        overload_listener_installed_ = (overload_status == noErr);
        overload_listener_device_id_ = device_id_;
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
            kAudioObjectSystemObject, &def_prop, default_output_changed_listener, listener_context_.get());
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
    // A failed explicit stop closed user admission but left native running
    // uncertain. Do not restart it behind the caller's stop request.
    if (is_running_ && !render_context_) return;
    const AudioDeviceID new_default = CoreAudioSystem::get_default_device(false);
    if (new_default == kAudioObjectUnknown || new_default == device_id_) return;

    const bool was_running = is_running_;
    if (was_running) {
        const OSStatus stop_status = native_ops_.stop(native_ops_.context, audio_unit_);
        if (!coreaudio_stop_allows_device_switch(stop_status)) {
            // Without a proven callback drain we cannot publish removal,
            // release the old query reference, or retarget CurrentDevice.
            runtime::log_warn(
                "CoreAudio: default-output switch could not stop old device ({})",
                static_cast<int>(stop_status));
            return;
        }
        retire_render_context_locked();
        is_running_ = false;
        fallback_priority_configured_.store(false, std::memory_order_release);
    }

    // The old device owns its callback workgroup. Stop rendering, make every
    // auxiliary worker leave it, and wait for that acknowledgment before
    // CurrentDevice can invalidate the borrowed handle.
    publish_workgroup_change_locked(nullptr);

    OSStatus st = AudioUnitSetProperty(audio_unit_,
        kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
        &new_default, sizeof(new_default));
    if (st == noErr) {
        remove_audio_io_timing_listeners_locked();
        device_id_ = new_default;
        install_audio_io_timing_listeners_locked();
        mark_audio_io_timing_stale();
        refresh_audio_io_timing_locked();
        runtime::log_info("CoreAudio: default output changed -> following to device {}",
            static_cast<unsigned>(new_default));
    } else {
        runtime::log_warn("CoreAudio: could not follow to new default output ({})",
            static_cast<int>(st));
    }

    // Re-query even on a failed switch: device_id_ still names the old device,
    // whose workgroup was deliberately removed above. Arm both the callback
    // thread and auxiliary clients before rendering resumes.
    query_callback_workgroup();
    publish_workgroup_change_locked(reinterpret_cast<void*>(workgroup_reference_.get()));
    if (was_running) {
        const bool registered = install_render_context_locked();
        is_running_ = registered; // Start failure leaves native activity uncertain.
        const OSStatus start_status = registered
            ? native_ops_.start(native_ops_.context, audio_unit_) : kAudio_ParamError;
        if (start_status != noErr) {
            retire_render_context_locked();
            // The replacement unit never entered its callback lifetime. Drain
            // auxiliary workers from the workgroup published before start so
            // public running state and scheduling membership stay coherent.
            fallback_priority_configured_.store(false, std::memory_order_release);
            publish_workgroup_change_locked(nullptr);
            callback_ = nullptr;
            runtime::log_warn(
                "CoreAudio: default-output switch could not restart ({})",
                static_cast<int>(start_status));
        }
    }
}

void CoreAudioDevice::require_control_thread() const noexcept {
    // Check before acquiring lifecycle/switch locks: another control thread may
    // already own them while waiting for this very callback to leave.
    if (CoreAudioCallbackEntry::owns_on_current_thread(this)) std::terminate();
}

void CoreAudioDevice::publish_workgroup_change_locked(void* workgroup) {
    if (!workgroup_change_callback_) return;
    CoreAudioCallbackContext context(this);
    CoreAudioCallbackEntry entry(&context);
    workgroup_change_callback_(workgroup);
}

void CoreAudioDevice::set_workgroup_change_callback(
    WorkgroupChangeCallback callback) {
    require_control_thread();
    std::lock_guard<std::mutex> lock(switch_mutex_);
    if (!workgroup_changes_quiesced_) {
        workgroup_change_callback_ = std::move(callback);
        publish_workgroup_change_locked(reinterpret_cast<void*>(workgroup_reference_.get()));
    } else if (callback) {
        CoreAudioCallbackContext context(this);
        CoreAudioCallbackEntry entry(&context);
        callback(nullptr);
    }
}

void* CoreAudioDevice::callback_workgroup() const {
    require_control_thread();
    // Compatibility snapshot for direct callers. Production binding uses the
    // transactional callback API above. Mutable serialization is required
    // because a live default-device switch replaces this retained reference.
    std::lock_guard<std::mutex> lock(switch_mutex_);
    return reinterpret_cast<void*>(workgroup_reference_.get());
}

OSStatus CoreAudioDevice::default_output_changed_listener(
    AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* client) {
    CoreAudioCallbackEntry entry(static_cast<CoreAudioCallbackContext*>(client));
    if (auto* self = static_cast<CoreAudioDevice*>(entry.owner()))
        self->switch_to_default_output();
    return noErr;
}

void CoreAudioDevice::quiesce_workgroup_changes() {
    require_control_thread();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    quiesce_workgroup_changes_locked();
}

void CoreAudioDevice::quiesce_workgroup_changes_locked() {
    // This must run outside switch_mutex_: an admitted default-change callback
    // may need that mutex before it can leave. This path is never called from
    // the default-change callback itself.
    if (listener_context_) listener_context_->close_and_wait();
    // Stop new default-change callbacks from firing, then take switch_mutex_ so any
    // in-flight switch_to_default_output() finishes. That switch may have rebound
    // a replacement workgroup after an earlier external drain, so publish null
    // once more while holding the serialization boundary before close can proceed.
    if (default_output_listener_installed_) {
        AudioObjectPropertyAddress def_prop{};
        def_prop.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        def_prop.mScope    = kAudioObjectPropertyScopeGlobal;
        def_prop.mElement  = kAudioObjectPropertyElementMain;
        const auto status = native_ops_.remove_listener(native_ops_.context,
            kAudioObjectSystemObject, def_prop, default_output_changed_listener,
            listener_context_.get());
        if (status == noErr) default_output_listener_installed_ = false;
        else runtime::log_warn("CoreAudio: default listener removal failed ({})", status);
    }
    std::lock_guard<std::mutex> switch_lock(switch_mutex_);
    workgroup_changes_quiesced_ = true;
    publish_workgroup_change_locked(nullptr);
    workgroup_change_callback_ = nullptr;
}

void CoreAudioDevice::close() {
    const auto result = close_checked();
    if (!result.complete())
        runtime::log_error("CoreAudio: close retained native ownership (stage {}, status {})",
            static_cast<int>(result.stage), result.status);
}

CoreAudioTeardownResult CoreAudioDevice::close_checked() {
    require_control_thread();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return close_locked();
}

CoreAudioTeardownResult CoreAudioDevice::close_locked() {
    quiesce_workgroup_changes_locked();
    std::lock_guard<std::mutex> switch_lock(switch_mutex_);
    retire_render_context_locked();
    callback_ = nullptr;
    if (timing_listener_context_) timing_listener_context_->detach_and_wait();

    auto& pending = *retirement_;
    pending.ops = native_ops_;
    pending.unit = audio_unit_;
    pending.running = is_running_;
    pending.initialized = unit_initialized_;
    pending.listeners = {{
        {default_output_listener_installed_, kAudioObjectSystemObject,
         {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain}, default_output_changed_listener,
         listener_context_.get()},
        {overload_listener_installed_, overload_listener_device_id_,
         {kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain}, overload_listener, listener_context_.get()},
    }};
    for (std::size_t i = 0; i < kTimingPropertyCount; ++i) {
        pending.listeners[i + 2] = {timing_listener_installed_[i], device_id_,
            timing_property_addresses_[i], audio_io_timing_changed_listener,
            timing_listener_context_.get()};
    }
    const auto result = retire_native(pending);
    for (std::size_t i = 0; i < kTimingPropertyCount; ++i)
        timing_listener_installed_[i] = pending.listeners[i + 2].installed;
    audio_unit_ = pending.unit;
    is_running_ = pending.running;
    unit_initialized_ = pending.initialized;
    default_output_listener_installed_ = pending.listeners[0].installed;
    overload_listener_installed_ = pending.listeners[1].installed;
    if (!result.complete()) return result;

    workgroup_reference_.reset();
    fallback_priority_configured_.store(false, std::memory_order_relaxed);
    if (input_buffer_list_) {
        std::free(input_buffer_list_);
        input_buffer_list_ = nullptr;
    }
    input_buffer_storage_.clear();
    input_ptrs_.clear();
    input_enabled_ = false;
    output_enabled_ = true;
    audio_io_timing_.reset();
    audio_io_timing_dirty_.store(true, std::memory_order_release);
    is_open_ = false;
    return {};
}

void CoreAudioDevice::retire_render_context_locked() {
    retain_closed_context(std::move(render_context_));
}

bool CoreAudioDevice::install_render_context_locked() {
    retire_render_context_locked();
    auto context = std::make_unique<CoreAudioCallbackContext>(this);
    const AURenderCallbackStruct callback{render_callback, context.get()};
    const auto status = native_ops_.set_callback(native_ops_.context, audio_unit_,
        input_enabled_ && !output_enabled_, callback);
    if (status != noErr) {
        // Native rejection need not prove the callback was never observed.
        retain_closed_context(std::move(context));
        return false;
    }
    render_context_ = std::move(context);
    return true;
}

bool CoreAudioDevice::start(AudioCallback callback) {
    require_control_thread();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> switch_lock(switch_mutex_);
    if (!is_open_ || is_running_ || !unit_initialized_ || workgroup_changes_quiesced_)
        return false;
    if (!install_render_context_locked()) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;
    fallback_priority_configured_.store(false, std::memory_order_release);
    // Treat a failed start as uncertain native activity until a checked stop.
    is_running_ = true;
    const auto status = native_ops_.start(native_ops_.context, audio_unit_);
    if (status != noErr) {
        retire_render_context_locked();
        callback_ = nullptr;
        runtime::log_error("CoreAudio: could not start ({})", static_cast<int>(status));
        return false;
    }
    return true;
}

CoreAudioTeardownResult CoreAudioDevice::stop_locked() {
    retire_render_context_locked();
    // The independent gate, not native stop status, establishes this safety.
    callback_ = nullptr;
    if (audio_unit_ && is_running_) {
        const auto status = native_ops_.stop(native_ops_.context, audio_unit_);
        if (status != noErr)
            return {CoreAudioTeardownResult::Stage::Stop, status};
    }
    is_running_ = false;
    fallback_priority_configured_.store(false, std::memory_order_release);
    return {};
}

CoreAudioTeardownResult CoreAudioDevice::stop_checked() {
    require_control_thread();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> switch_lock(switch_mutex_);
    return stop_locked();
}

void CoreAudioDevice::stop() {
    const auto result = stop_checked();
    if (!result.complete())
        runtime::log_error("CoreAudio: callback stopped; native stop failed ({})", result.status);
}

void CoreAudioDevice::quarantine_native_ownership() noexcept {
    if (!retirement_ || (!retirement_->unit &&
        std::none_of(retirement_->listeners.begin(), retirement_->listeners.end(),
                     [](const auto& listener) { return listener.installed; }))) return;
    std::lock_guard<std::mutex> lock(retirement_mutex());
    retirement_->next = retired_native_head();
    retired_native_head() = retirement_.release();
    // No admitted callback can access these pointers now, including late native
    // entries. Auxiliary workgroup clients were drained before close attempted.
    if (input_buffer_list_) {
        std::free(input_buffer_list_);
        input_buffer_list_ = nullptr;
    }
}

std::size_t retry_coreaudio_native_retirements() {
    std::lock_guard<std::mutex> lock(retirement_mutex());
    auto** cursor = &retired_native_head();
    std::size_t remaining = 0;
    while (*cursor) {
        auto* entry = *cursor;
        if (retire_native(*entry).complete()) {
            *cursor = entry->next;
            delete entry;
        } else {
            ++remaining;
            cursor = &entry->next;
        }
    }
    return remaining;
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
    CoreAudioCallbackEntry entry(static_cast<CoreAudioCallbackContext*>(inRefCon));
    auto* self = static_cast<CoreAudioDevice*>(entry.owner());
    if (!self) {
        if (ioData) {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i)
                if (ioData->mBuffers[i].mData)
                    std::memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    // Do not join the callback to callback_workgroup(): Apple's property is the
    // workgroup the device I/O thread already belongs to. Joining it again
    // creates an unnecessary token whose only legal owner/consumer is this
    // callback thread and makes stop/switch cleanup impossible to prove. The
    // retained property handle exists solely for auxiliary render workers.
#if defined(__APPLE__)
    if (!self->workgroup_reference_.get())
#endif
    {
        // A priority policy is thread state rather than workgroup membership;
        // configure it once instead of repeating the Mach call every block.
        // Publish success only. A transient failure is retried on the next
        // callback, and stop/start or a successful device switch resets this
        // per-callback-thread lifetime state.
        (void)configure_coreaudio_fallback_priority_once(
            self->fallback_priority_configured_, [] {
                return AudioWorkgroup::set_realtime_priority();
            });
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
        // Reset buffer sizes in case CoreAudio changed them — CLAMPED to what
        // was actually allocated.
        //
        // This declared `inNumberFrames` of space unconditionally. The storage
        // is allocated once for the configured block size, so a device that
        // asks for more frames than that had AudioUnitRender memcpy past the
        // end of the heap buffer. AddressSanitizer caught it as a 1880-byte
        // heap-buffer-overflow inside AudioUnitRender, on the audio thread;
        // the process then aborted later in whatever unrelated code allocated
        // next — CoreText, CFPreferences, CoreGraphics — which is why it read
        // as three different bugs.
        //
        // Clamping is the only response available here: growing the buffer
        // means allocating on the audio thread, which is worse than dropping
        // the extra frames of INPUT for one callback.
        auto in_ch = static_cast<UInt32>(self->config_.input_channels);
        const UInt32 in_frames =
            clamped_input_frames(inNumberFrames, self->input_buffer_frames_);
        for (UInt32 c = 0; c < in_ch; ++c) {
            self->input_buffer_list_->mBuffers[c].mDataByteSize =
                in_frames * sizeof(float);
        }

        // Pull input with its own action-flags slot so flags written by the
        // bus-1 input render never leak back into the bus-0 output render on the
        // duplex path.
        AudioUnitRenderActionFlags input_flags = 0;
        // ASKED for what there is room for, not for what was requested.
        //
        // Declaring the clamped size in the buffer list is not enough on its
        // own: the frame count passed here is the REQUEST, and asking for more
        // frames than the list has room for is what let AudioUnitRender write
        // past the end. Request and capacity have to agree.
        OSStatus input_status = AudioUnitRender(
            self->audio_unit_, &input_flags, inTimeStamp,
            1,  // Bus 1 = input
            in_frames,
            self->input_buffer_list_);

        if (input_status == noErr) {
            // The view is `in_frames` long, which may be shorter than the
            // output block. A view claiming frames that were never captured
            // hands the processor whatever the buffer held last.
            input_view = BufferView<const float>(
                const_cast<const float**>(self->input_ptrs_.data()),
                self->input_ptrs_.size(), in_frames);
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
    CoreAudioCallbackEntry entry(static_cast<CoreAudioCallbackContext*>(inClientData));
    auto* self = static_cast<CoreAudioDevice*>(entry.owner());
    if (self) {
        self->xrun_counter_.fetch_add(1, std::memory_order_relaxed);
    }
    return noErr;
}

// ── CoreAudioSystem ────────────────────────────────────────────────────────

CoreAudioSystem::CoreAudioSystem()
    : native_ops_(native_operations()),
      retirement_(std::make_unique<CoreAudioNativeRetirement>()),
      listener_context_(std::make_unique<CoreAudioCallbackContext>(this)) {}

CoreAudioSystem::~CoreAudioSystem() {
    listener_context_->close_and_wait();
    auto& pending = *retirement_;
    pending.ops = native_ops_;
    pending.listeners = {{
        {listener_installed_, kAudioObjectSystemObject,
         {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain}, device_list_changed, listener_context_.get()},
        {default_output_listener_installed_, kAudioObjectSystemObject,
         {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain}, default_device_changed, listener_context_.get()},
        {default_input_listener_installed_, kAudioObjectSystemObject,
         {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain}, default_device_changed, listener_context_.get()},
    }};
    const auto result = retire_native(pending);
    if (!result.complete()) {
        runtime::log_warn("CoreAudio: system listener removal retained ({})", result.status);
        std::lock_guard<std::mutex> lock(retirement_mutex());
        retirement_->next = retired_native_head();
        retired_native_head() = retirement_.release();
    }
    retain_closed_context(std::move(listener_context_));
}

namespace {
template<class Function, class Publish>
void replace_notification(std::mutex& mutex,
    std::shared_ptr<CoreAudioNotificationSlot<Function>>& current,
    std::vector<std::shared_ptr<CoreAudioNotificationSlot<Function>>>& retired,
    Function callback, bool reentrant, void (*drain_hook)(void*), void* hook_context,
    Publish publish_base) {
    auto replacement = callback
        ? std::make_shared<CoreAudioNotificationSlot<Function>>(std::move(callback)) : nullptr;
    std::vector<std::shared_ptr<CoreAudioNotificationSlot<Function>>> draining;
    {
        std::lock_guard<std::mutex> lock(mutex);
        publish_base();
        if (current) retired.push_back(std::move(current));
        current = std::move(replacement);
        // Include previously retired generations: a prior self-clear cannot
        // hide an active delivery from a later external unregister barrier.
        for (auto& slot : retired) slot->admission.close();
        draining = retired;
        std::erase_if(retired, [](const auto& slot) { return slot->admission.admitted() == 0; });
    }
    // Never hold the setter mutex across drain. A callback may clear/replace
    // itself; that supported path closes admission but cannot wait for itself
    // or another concurrently self-clearing notification.
    if (!reentrant) {
        if (drain_hook) drain_hook(hook_context);
        for (auto& slot : draining) slot->admission.close_and_wait();
        std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(retired, [](const auto& slot) { return slot->admission.admitted() == 0; });
    }
}
} // namespace

void CoreAudioSystem::ensure_listener_registration(
    std::size_t index, AudioObjectPropertySelector selector,
    AudioObjectPropertyListenerProc callback) {
    bool* installed = index == 0 ? &listener_installed_
                    : index == 1 ? &default_output_listener_installed_
                                 : &default_input_listener_installed_;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (*installed || listener_registration_pending_[index]) return;
        listener_registration_pending_[index] = true;
    }
    const AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    const auto status = native_ops_.add_listener(native_ops_.context, kAudioObjectSystemObject,
        property, callback, listener_context_.get());
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        *installed = status == noErr;
        listener_registration_pending_[index] = false;
    }
    if (status != noErr) runtime::log_warn("CoreAudio: system listener registration failed ({})", status);
}

void CoreAudioSystem::set_device_change_callback(DeviceChangeCallback cb) {
    const bool enabled = static_cast<bool>(cb);
    auto base_callback = cb;
    replace_notification(callback_mutex_, device_change_cb_, retired_device_callbacks_,
        std::move(cb), listener_context_->entered_on_current_thread(),
        notification_drain_hook_, notification_drain_hook_context_,
        [this, callback = std::move(base_callback)]() mutable {
            AudioSystem::set_device_change_callback(std::move(callback));
        });
    if (enabled) ensure_listener_registration(0, kAudioHardwarePropertyDevices, device_list_changed);
}

void CoreAudioSystem::set_default_device_change_callback(DefaultDeviceChangeCallback cb) {
    const bool enabled = static_cast<bool>(cb);
    replace_notification(callback_mutex_, default_device_change_cb_, retired_default_callbacks_,
        std::move(cb), listener_context_->entered_on_current_thread(),
        notification_drain_hook_, notification_drain_hook_context_, [] {});
    if (enabled) {
        ensure_listener_registration(1, kAudioHardwarePropertyDefaultOutputDevice, default_device_changed);
        ensure_listener_registration(2, kAudioHardwarePropertyDefaultInputDevice, default_device_changed);
    }
}

OSStatus CoreAudioSystem::device_list_changed(
    AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* inClientData) {
    CoreAudioCallbackEntry owner_entry(static_cast<CoreAudioCallbackContext*>(inClientData));
    auto* self = static_cast<CoreAudioSystem*>(owner_entry.owner());
    if (!self) return noErr;
    std::shared_ptr<CoreAudioNotificationSlot<DeviceChangeCallback>> slot;
    {
        std::lock_guard<std::mutex> lock(self->callback_mutex_);
        slot = self->device_change_cb_;
    }
    if (self->notification_snapshot_hook_)
        self->notification_snapshot_hook_(self->notification_snapshot_hook_context_);
    if (slot) {
        CoreAudioCallbackEntry delivery(&slot->admission);
        if (delivery.owner()) slot->callback();
    }
    return noErr;
}

OSStatus CoreAudioSystem::default_device_changed(
    AudioObjectID, UInt32 count, const AudioObjectPropertyAddress* addresses, void* inClientData) {
    CoreAudioCallbackEntry owner_entry(static_cast<CoreAudioCallbackContext*>(inClientData));
    auto* self = static_cast<CoreAudioSystem*>(owner_entry.owner());
    if (!self) return noErr;
    std::shared_ptr<CoreAudioNotificationSlot<DefaultDeviceChangeCallback>> slot;
    {
        std::lock_guard<std::mutex> lock(self->callback_mutex_);
        slot = self->default_device_change_cb_;
    }
    if (self->notification_snapshot_hook_)
        self->notification_snapshot_hook_(self->notification_snapshot_hook_context_);
    if (slot) {
        CoreAudioCallbackEntry delivery(&slot->admission);
        if (delivery.owner()) {
            bool input = false;
            for (UInt32 i = 0; i < count; ++i)
                input = input || addresses[i].mSelector == kAudioHardwarePropertyDefaultInputDevice;
            slot->callback(input);
        }
    }
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
    if (read_coreaudio_nominal_sample_rate(device_id, current_rate))
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
            if (coreaudio_device_exists(parsed)) {
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
