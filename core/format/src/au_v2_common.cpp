// Base-class-independent AU v2 adapter helpers shared by the effect,
// instrument, and MIDI-processor adapters. See detail/au_v2_shared.hpp.

#include <AudioToolbox/AudioUnitUtilities.h>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/param_group_projection.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/events/plugin_main_thread.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::format::au {

namespace {

constexpr int kParameterDisplayActivePollIntervalMs = 33;
constexpr int kParameterDisplayIdlePollIntervalMs = 1000;

}  // namespace

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

    const ParamGroupProjection groups(store.all_groups());
    if (groups.find(param->group_id)) {
        out_info.flags |= kAudioUnitParameterFlag_HasClump;
        out_info.clumpID = static_cast<UInt32>(param->group_id);
    } else {
        out_info.clumpID = 0;
    }

    // Advertise author-supplied value strings so the host queries our display
    // formatting. DISCRETE params serve an enumerated list through
    // GetParameterValueStrings; CONTINUOUS params round-trip single values
    // through kAudioUnitProperty_ParameterStringFromValue /
    // ...ValueFromString. Gated on the parameter declaring a converter AND on
    // the adapter actually serving those properties.
    if (advertise_value_strings &&
        (param->to_string || !param->value_labels.empty()))
        out_info.flags |= kAudioUnitParameterFlag_ValuesHaveStrings;

    const std::string display_name =
        store.parameter_display_name(static_cast<state::ParamID>(param_id));
    out_info.cfNameString = CFStringCreateWithCString(
        kCFAllocatorDefault, display_name.c_str(), kCFStringEncodingUTF8);
    strlcpy(reinterpret_cast<char*>(out_info.name),
            display_name.c_str(), sizeof(out_info.name));

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

OSStatus fill_parameter_clump_property_info(const state::StateStore& store,
                                            AudioUnitScope scope,
                                            UInt32& out_size,
                                            bool& out_writable)
{
    if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    if (ParamGroupProjection(store.all_groups()).entries().empty())
        return kAudioUnitErr_InvalidProperty;
    out_size = sizeof(AudioUnitParameterNameInfo);
    out_writable = false;
    return noErr;
}

OSStatus fill_parameter_clump_name(const state::StateStore& store,
                                   AudioUnitScope scope,
                                   void* out_data)
{
    if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    if (!out_data) return kAudioUnitErr_InvalidProperty;
    auto* name = static_cast<AudioUnitParameterNameInfo*>(out_data);
    const ParamGroupProjection groups(store.all_groups());
    const auto* group = groups.find(static_cast<int>(name->inID));
    if (!group) return kAudioUnitErr_InvalidPropertyValue;
    CFStringRef full_name = CFStringCreateWithCString(
        kCFAllocatorDefault, group->path.c_str(), kCFStringEncodingUTF8);
    if (!full_name) return kAudioUnitErr_InvalidPropertyValue;
    // AUBase normalizes kAudioUnitParameterName_Full (-1) to zero before
    // dispatching CopyClumpName, so both non-positive forms request the full
    // name. Only a positive desired length is a bounded request.
    if (name->inDesiredLength > 0 &&
        CFStringGetLength(full_name) > name->inDesiredLength) {
        name->outName = CFStringCreateWithSubstring(
            kCFAllocatorDefault, full_name,
            CFRangeMake(0, name->inDesiredLength));
        CFRelease(full_name);
    } else {
        name->outName = full_name;
    }
    return name->outName ? noErr : kAudioUnitErr_InvalidPropertyValue;
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

struct ParameterDisplayNamePublisher::Impl {
    struct Entry {
        state::ParamID id = 0;
        std::string name;
    };

    struct SharedState {
        std::mutex mutex;
        std::condition_variable idle;
        state::StateStore* store = nullptr;
        Notify notify;
        std::vector<Entry> published;
        std::uint64_t revision = 0;
        std::size_t drains_in_flight = 0;
        bool active = false;
    };

    struct DrainScope {
        explicit DrainScope(SharedState* draining)
            : state(draining), previous(current)
        {
            current = this;
        }

        ~DrainScope() { current = previous; }

        SharedState* state = nullptr;
        DrainScope* previous = nullptr;
        static thread_local DrainScope* current;
    };

    struct InFlightDrain {
        explicit InFlightDrain(std::shared_ptr<SharedState> shared_state)
            : shared(std::move(shared_state))
        {
        }

        InFlightDrain(const InFlightDrain&) = delete;
        InFlightDrain& operator=(const InFlightDrain&) = delete;

        ~InFlightDrain() noexcept {
            std::lock_guard lock(shared->mutex);
            --shared->drains_in_flight;
            if (shared->drains_in_flight == 0)
                shared->idle.notify_all();
        }

        std::shared_ptr<SharedState> shared;
    };

    std::shared_ptr<SharedState> state;
    events::MainThreadDispatcher::Token main_thread_token = 0;

    static bool draining_on_this_thread(const SharedState* shared) noexcept {
        for (auto* scope = DrainScope::current; scope;
             scope = scope->previous) {
            if (scope->state == shared) return true;
        }
        return false;
    }

    static bool drain(const std::shared_ptr<SharedState>& shared) {
        if (!shared) return false;
        if (!events::MainThreadDispatcher::is_main_thread()) return false;
        state::StateStore* store = nullptr;
        std::uint64_t published_revision = 0;
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->active || !shared->store || !shared->notify)
                return false;
            store = shared->store;
            published_revision = shared->revision;
            ++shared->drains_in_flight;
        }
        InFlightDrain in_flight(shared);
        DrainScope drain_scope(shared.get());

        const std::uint64_t current_revision =
            store->parameter_display_revision();
        std::vector<Entry> current;
        if (current_revision != published_revision) {
            current.reserve(store->param_count());
            for (const auto& parameter : store->all_params()) {
                current.push_back(
                    {parameter.id,
                     store->parameter_display_name(parameter.id)});
            }
        }

        Notify notify;
        std::vector<state::ParamID> changed_ids;
        bool revision_advanced = false;
        {
            std::lock_guard lock(shared->mutex);
            if (shared->active && shared->store == store &&
                current_revision != shared->revision) {
                revision_advanced = true;
                notify = shared->notify;
                const std::size_t count =
                    std::min(shared->published.size(), current.size());
                changed_ids.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    if (shared->published[i].id == current[i].id &&
                        shared->published[i].name != current[i].name) {
                        const state::ParamID id = current[i].id;
                        if (std::find(changed_ids.begin(), changed_ids.end(),
                                      id) == changed_ids.end()) {
                            changed_ids.push_back(id);
                        }
                    }
                }
                shared->published = std::move(current);
                shared->revision = current_revision;
            }
        }

        if (notify) {
            for (state::ParamID id : changed_ids) {
                {
                    std::lock_guard lock(shared->mutex);
                    if (!shared->active) break;
                }
                notify(id);
            }
        }
        return revision_advanced;
    }

    static void schedule(const std::shared_ptr<SharedState>& shared,
                         int delay_ms) {
        if (!shared) return;
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->active) return;
        }
        events::MainThreadDispatcher::call_async_after(
            [shared] {
                const bool revision_advanced = drain(shared);
                schedule(
                    shared,
                    revision_advanced
                        ? kParameterDisplayActivePollIntervalMs
                        : kParameterDisplayIdlePollIntervalMs);
            },
            delay_ms);
    }
};

thread_local ParameterDisplayNamePublisher::Impl::DrainScope*
    ParameterDisplayNamePublisher::Impl::DrainScope::current = nullptr;

ParameterDisplayNamePublisher::ParameterDisplayNamePublisher()
    : impl_(std::make_unique<Impl>())
{
}

ParameterDisplayNamePublisher::~ParameterDisplayNamePublisher()
{
    stop();
}

void ParameterDisplayNamePublisher::start(state::StateStore& store,
                                          Notify notify)
{
    stop();

    auto shared = std::make_shared<Impl::SharedState>();
    shared->store = &store;
    shared->notify = std::move(notify);
    shared->published.reserve(store.param_count());
    for (const auto& parameter : store.all_params()) {
        shared->published.push_back(
            {parameter.id, store.parameter_display_name(parameter.id)});
    }
    shared->revision = store.parameter_display_revision();
    shared->active = true;

    impl_->main_thread_token = events::register_plugin_backend();
    impl_->state = shared;
    Impl::schedule(shared, kParameterDisplayActivePollIntervalMs);
}

void ParameterDisplayNamePublisher::stop() noexcept
{
    if (!impl_) return;
    if (impl_->state) {
        std::unique_lock lock(impl_->state->mutex);
        impl_->state->active = false;
        if (!Impl::draining_on_this_thread(impl_->state.get())) {
            impl_->state->idle.wait(
                lock, [state = impl_->state] {
                    return state->drains_in_flight == 0;
                });
        }
        impl_->state->notify = {};
        impl_->state->store = nullptr;
    }
    if (impl_->main_thread_token != 0) {
        events::unregister_plugin_backend(impl_->main_thread_token);
        impl_->main_thread_token = 0;
    }
    impl_->state.reset();
}

void ParameterDisplayNamePublisher::poll_main_thread()
{
    if (!impl_) return;
    auto shared = impl_->state;
    Impl::drain(shared);
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
                            StateRestoreGate& gate,
                            CFPropertyListRef plist)
{
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return noErr;

    auto dict = static_cast<CFDictionaryRef>(plist);
    auto cf_data = static_cast<CFDataRef>(
        CFDictionaryGetValue(dict, CFSTR("pulp-state")));
    if (!cf_data || CFGetTypeID(cf_data) != CFDataGetTypeID()) return noErr;

    const auto* bytes = CFDataGetBytePtr(cf_data);
    const auto length = CFDataGetLength(cf_data);
    auto restore_lock = gate.lock_for_restore();
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
