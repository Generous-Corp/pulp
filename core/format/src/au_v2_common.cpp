// Base-class-independent AU v2 adapter helpers shared by the effect,
// instrument, and MIDI-processor adapters. See detail/au_v2_shared.hpp.

#include <AudioToolbox/AudioUnitUtilities.h>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/plugin_state_io.hpp>

#include <cstring>
#include <string>

namespace pulp::format::au {

// Cross-TU Cocoa-view hook (see au_v2_common.hpp). Hidden visibility keeps it
// per-loaded-image so two Pulp AU components in one host don't share state.
// Installed by au_v2_cocoa_view.mm's static-init when a *_AU target links it
// (PULP_AU_GUI); null for CLAP / Standalone / headless builds of pulp-format.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
CocoaViewInfoFiller g_cocoa_view_info_filler = nullptr;

midi::MidiEvent decode_midi_event(uint8_t inStatus,
                                  uint8_t inChannel,
                                  uint8_t inData1,
                                  uint8_t inData2) noexcept
{
    // AUMIDIBase::MIDIEvent (AudioUnitSDK 1.4 AUMIDIBase.h) splits the
    // wire-format status byte unconditionally:
    //
    //     strippedStatus = inStatus & 0xF0   // top nibble  -> our inStatus
    //     channel        = inStatus & 0x0F   // low nibble  -> our inChannel
    //     HandleMIDIEvent(strippedStatus, channel, ...)
    //
    // The split is the SAME for every status byte the host delivers to
    // this callback — channel-voice (0x80-0xEF) AND system (0xF0-0xFF).
    // For system common (0xF1-0xF7) and system realtime (0xF8-0xFF), the
    // low nibble carries the system-message subtype rather than a channel,
    // but the bit-layout reassembly is identical: status = top | low.
    //
    // Returning inStatus unchanged for system messages would turn every
    // system message into 0xF0 (sysex start), so MIDI clock / start / stop /
    // continue / song-position / quarter-frame would arrive at the Processor
    // with the wrong status byte. The unit test in test_au_v2_effect.cpp
    // mirrors the SDK splitting so the regression cannot reappear.
    const uint8_t status_byte =
        static_cast<uint8_t>((inStatus & 0xF0) | (inChannel & 0x0F));
    midi::MidiEvent ev{
        choc::midi::ShortMessage(status_byte, inData1, inData2),
        /*sample_offset=*/0,
        /*timestamp=*/0.0,
    };
    return ev;
}

namespace {

// Set while the HOST writes a parameter (SetParameter). The store's UI-push
// listener fires inline on the same thread, so it consults this to skip
// echoing the host's own write back at it. Thread-local: the guard is scoped
// to the writing thread only.
thread_local bool g_host_writing_param = false;

void notify_host_parameter_event(AudioUnit unit,
                                 AudioUnitEventType type,
                                 state::ParamID id) {
    AudioUnitEvent event;
    std::memset(&event, 0, sizeof(event));
    event.mEventType = type;
    event.mArgument.mParameter.mAudioUnit = unit;
    event.mArgument.mParameter.mParameterID =
        static_cast<AudioUnitParameterID>(id);
    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
    event.mArgument.mParameter.mElement = 0;
    AUEventListenerNotify(nullptr, nullptr, &event);
}

}  // namespace

OSStatus fill_parameter_list(const state::StateStore& store,
                             AudioUnitScope scope,
                             AudioUnitParameterID* out_list,
                             UInt32& out_count)
{
    if (scope != kAudioUnitScope_Global) {
        out_count = 0;
        return noErr;
    }
    out_count = static_cast<UInt32>(store.param_count());
    if (out_list) {
        auto params = store.all_params();
        for (std::size_t i = 0; i < params.size(); ++i)
            out_list[i] = static_cast<AudioUnitParameterID>(params[i].id);
    }
    return noErr;
}

OSStatus fill_parameter_info(const state::StateStore& store,
                             AudioUnitScope scope,
                             AudioUnitParameterID param_id,
                             AudioUnitParameterInfo& out_info,
                             bool advertise_value_strings)
{
    if (scope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidParameter;

    const auto* param = store.info(static_cast<state::ParamID>(param_id));
    if (!param) return kAudioUnitErr_InvalidParameter;

    out_info.flags = kAudioUnitParameterFlag_IsWritable
                   | kAudioUnitParameterFlag_IsReadable
                   | kAudioUnitParameterFlag_HasCFNameString;

    // Advertise author-supplied value strings so the host queries our display
    // formatting. DISCRETE params serve an enumerated list through
    // GetParameterValueStrings; CONTINUOUS params round-trip single values
    // through kAudioUnitProperty_ParameterStringFromValue /
    // ...ValueFromString. Gated on the parameter declaring a converter AND on
    // the adapter actually serving those properties.
    if (advertise_value_strings &&
        (param->to_string || !param->value_labels.empty()))
        out_info.flags |= kAudioUnitParameterFlag_ValuesHaveStrings;

    out_info.cfNameString = CFStringCreateWithCString(
        kCFAllocatorDefault, param->name.c_str(), kCFStringEncodingUTF8);
    strlcpy(reinterpret_cast<char*>(out_info.name),
            param->name.c_str(), sizeof(out_info.name));

    out_info.minValue = param->range.min;
    out_info.maxValue = param->range.max;
    out_info.defaultValue = param->range.default_value;

    if (param->unit == "dB") {
        out_info.unit = kAudioUnitParameterUnit_Decibels;
    } else if (param->unit == "Hz") {
        out_info.unit = kAudioUnitParameterUnit_Hertz;
    } else if (param->unit == "%") {
        out_info.unit = kAudioUnitParameterUnit_Percent;
    } else if (state::is_boolean_param(*param)) {
        out_info.unit = kAudioUnitParameterUnit_Boolean;
    } else {
        out_info.unit = kAudioUnitParameterUnit_Generic;
    }
    return noErr;
}

OSStatus fill_parameter_value_strings(const state::StateStore& store,
                                      AudioUnitScope scope,
                                      AudioUnitParameterID param_id,
                                      CFArrayRef* out_strings)
{
    if (scope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidParameter;
    if (!out_strings) return kAudioUnitErr_InvalidPropertyValue;

    const auto* param = store.info(static_cast<state::ParamID>(param_id));
    if (!param) return kAudioUnitErr_InvalidParameter;
    if (!state::is_discrete_param(*param))
        return kAudioUnitErr_InvalidPropertyValue;

    const int count = static_cast<int>(state::param_value_count(*param));
    CFMutableArrayRef strings =
        CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks);
    for (int i = 0; i < count; ++i) {
        const float step = param->range.step > 0.0f ? param->range.step : 1.0f;
        const float value = param->range.min + static_cast<float>(i) * step;
        const auto str = format_parameter_text(*param, value);
        CFStringRef cf_str = CFStringCreateWithCString(
            kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(strings, cf_str);
        CFRelease(cf_str);
    }
    *out_strings = strings;
    return noErr;
}

OSStatus parameter_string_from_value(const state::StateStore& store,
                                     void* out_data)
{
    if (!out_data) return kAudioUnitErr_InvalidProperty;
    auto* sfv = static_cast<AudioUnitParameterStringFromValue*>(out_data);
    const auto* param = store.info(static_cast<state::ParamID>(sfv->inParamID));
    if (!param) return kAudioUnitErr_InvalidPropertyValue;
    const float value = sfv->inValue
        ? static_cast<float>(*sfv->inValue)
        : store.get_value(static_cast<state::ParamID>(sfv->inParamID));
    const std::string text = format_parameter_text(*param, value);
    sfv->outString = CFStringCreateWithCString(
        kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
    return sfv->outString ? noErr : kAudioUnitErr_InvalidPropertyValue;
}

OSStatus parameter_value_from_string(const state::StateStore& store,
                                     void* out_data)
{
    if (!out_data) return kAudioUnitErr_InvalidProperty;
    auto* vfs = static_cast<AudioUnitParameterValueFromString*>(out_data);
    const auto* param = store.info(static_cast<state::ParamID>(vfs->inParamID));
    if (!param || !vfs->inString) return kAudioUnitErr_InvalidPropertyValue;
    char buf[256] = {0};
    if (!CFStringGetCString(vfs->inString, buf, sizeof(buf),
                            kCFStringEncodingUTF8))
        return kAudioUnitErr_InvalidPropertyValue;
    const auto parsed = parse_parameter_text(*param, buf);
    if (!parsed) return kAudioUnitErr_InvalidPropertyValue;
    vfs->outValue = *parsed;
    return noErr;
}

bool host_is_writing_param() noexcept { return g_host_writing_param; }

ScopedHostParamWrite::ScopedHostParamWrite() noexcept
{
    g_host_writing_param = true;
}

ScopedHostParamWrite::~ScopedHostParamWrite() noexcept
{
    g_host_writing_param = false;
}

void wire_host_parameter_bridge(state::StateStore& store,
                                AudioUnit unit,
                                state::ListenerToken& out_listener)
{
    store.set_gesture_callbacks(
        [unit](state::ParamID id) {
            notify_host_parameter_event(
                unit, kAudioUnitEvent_BeginParameterChangeGesture, id);
        },
        [unit](state::ParamID id) {
            notify_host_parameter_event(
                unit, kAudioUnitEvent_EndParameterChangeGesture, id);
        });

    // Registered as an Audio listener so it runs INLINE, synchronously, on
    // whichever thread wrote the store: an editor edit fires it on the message
    // thread (correct); a host write fires it inside ScopedHostParamWrite, so
    // the echo is suppressed. The render thread never writes the store, so
    // this never notifies from the render thread.
    out_listener = store.add_listener(
        [unit](state::ParamID id, float /*value*/) {
            if (host_is_writing_param()) return;
            notify_host_parameter_event(
                unit, kAudioUnitEvent_ParameterValueChange, id);
        },
        state::ListenerThread::Audio);
}

OSStatus save_pulp_state(state::StateStore& store,
                         Processor& processor,
                         CFPropertyListRef* out_data)
{
    if (!out_data) return kAudioUnitErr_InvalidPropertyValue;

    auto data = plugin_state_io::serialize(store, processor);
    CFDataRef cf_data = CFDataCreate(kCFAllocatorDefault, data.data(),
                                     static_cast<CFIndex>(data.size()));
    if (!cf_data) return noErr;

    CFMutableDictionaryRef dict = nullptr;
    if (*out_data && CFGetTypeID(*out_data) == CFDictionaryGetTypeID()) {
        dict = CFDictionaryCreateMutableCopy(
            kCFAllocatorDefault, 0, static_cast<CFDictionaryRef>(*out_data));
    } else {
        dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                         &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks);
    }
    if (*out_data) CFRelease(*out_data);

    CFDictionarySetValue(dict, CFSTR("pulp-state"), cf_data);
    *out_data = dict;
    CFRelease(cf_data);
    return noErr;
}

OSStatus restore_pulp_state(state::StateStore& store,
                            Processor& processor,
                            CFPropertyListRef plist)
{
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return noErr;

    auto dict = static_cast<CFDictionaryRef>(plist);
    auto cf_data = static_cast<CFDataRef>(
        CFDictionaryGetValue(dict, CFSTR("pulp-state")));
    if (!cf_data || CFGetTypeID(cf_data) != CFDataGetTypeID()) return noErr;

    const auto* bytes = CFDataGetBytePtr(cf_data);
    const auto length = CFDataGetLength(cf_data);
    if (!plugin_state_io::deserialize(
            {bytes, static_cast<std::size_t>(length)}, store, processor))
        return kAudioUnitErr_InvalidPropertyValue;
    return noErr;
}

OSStatus MidiOutputCallbackPublisher::publish(const void* in_data,
                                              UInt32 in_data_size) noexcept
{
    if (!in_data || in_data_size < sizeof(AUMIDIOutputCallbackStruct))
        return kAudioUnitErr_InvalidPropertyValue;
    const auto* in = static_cast<const AUMIDIOutputCallbackStruct*>(in_data);
    const std::uint8_t slot = write_slot_.load(std::memory_order_relaxed);
    slots_[slot].callback = in->midiOutputCallback;
    slots_[slot].user_data = in->userData;
    // Publish the freshly written slot, then flip the write cursor so the next
    // publish writes the other slot — never the one the render thread may
    // still be reading.
    active_.store(&slots_[slot], std::memory_order_release);
    write_slot_.store(static_cast<std::uint8_t>(slot ^ 1),
                      std::memory_order_relaxed);
    return noErr;
}

OSStatus MidiOutputCallbackPublisher::reflect(void* out_data) const noexcept
{
    if (!out_data) return kAudioUnitErr_InvalidProperty;
    auto* out = static_cast<AUMIDIOutputCallbackStruct*>(out_data);
    const Pair* pair = active_.load(std::memory_order_acquire);
    out->midiOutputCallback = pair ? pair->callback : nullptr;
    out->userData = pair ? pair->user_data : nullptr;
    return noErr;
}

CFArrayRef make_midi_output_names(const char* stream_name)
{
    CFStringRef name = CFStringCreateWithCString(
        kCFAllocatorDefault,
        stream_name && *stream_name ? stream_name : "MIDI Out",
        kCFStringEncodingUTF8);
    CFStringRef values[1] = {name};
    CFArrayRef array = CFArrayCreate(kCFAllocatorDefault,
                                     reinterpret_cast<const void**>(values), 1,
                                     &kCFTypeArrayCallBacks);
    if (name) CFRelease(name);  // the array retained it
    return array;
}

} // namespace pulp::format::au
