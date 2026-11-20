// Base-class-independent AU v2 adapter helpers shared by the effect,
// instrument, and MIDI-processor adapters. See detail/au_v2_shared.hpp.

#include <AudioToolbox/AudioUnitUtilities.h>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/audio_workgroup_client.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/param_group_projection.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/events/plugin_main_thread.hpp>
#include <choc/memory/choc_Endianness.h>

#include <Block.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
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

// AU `Indexed` means the value IS an index into GetParameterValueStrings, so
// the range must be 0..N-1 with step 1. The value list is built as
// min + i*step, so an Integer with a non-zero min or a stride above 1 (or an
// unlabelled 0..127 integer) must stay Generic rather than become a menu.
bool is_index_valued_param(const state::ParamInfo& info) {
    if (!state::is_discrete_param(info))
        return false;
    if (info.kind != state::ParamKind::Enum && info.value_labels.empty())
        return false;
    const float step = info.range.step;
    return info.range.min == 0.0f && (step == 0.0f || step == 1.0f);
}

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

    out_info.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_HasCFNameString;

    // AU has no single "hidden"/"readonly"/"automatable" triple, so each
    // attribute maps to the flag whose documented meaning matches:
    //
    //  * read-only  — withhold IsWritable (the host's only write path, and so
    //                 also its automation path) and add MeterReadOnly, which
    //                 AudioUnitProperties.h defines for exactly this
    //                 display-only, plugin-published value.
    //  * hidden     — ExpertMode, documented as "the parameter is obscure
    //                 (hint to UI to only display in expert mode)". AU has no
    //                 true hide, so this is a hint, not a guarantee.
    //  * non-automatable — NonRealTime, documented as "changing the parameter
    //                 in real-time will cause a glitch or otherwise
    //                 undesirable effect", which is the reason a parameter
    //                 should not be driven by an automation lane.
    if (state::is_read_only_param(*param))
        out_info.flags |= kAudioUnitParameterFlag_MeterReadOnly;
    else
        out_info.flags |= kAudioUnitParameterFlag_IsWritable;

    if (state::is_hidden_param(*param))
        out_info.flags |= kAudioUnitParameterFlag_ExpertMode;

    // IsGlobalMeta tells the host that a change here may move other
    // parameters, so it re-reads them after the write.
    if (state::is_meta_param(*param))
        out_info.flags |= kAudioUnitParameterFlag_IsGlobalMeta;

    if (!state::is_automatable_param(*param))
        out_info.flags |= kAudioUnitParameterFlag_NonRealTime;

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
    } else if (is_index_valued_param(*param)) {
        // An enumerated choice whose value really is an index (0..N-1, step 1;
        // a quantized continuous or offset/strided Integer stays Generic):
        // Indexed tells hosts to show a menu built from
        // GetParameterValueStrings instead of a numeric slider.
        out_info.unit = kAudioUnitParameterUnit_Indexed;
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

OSStatus fill_supports_mpe_property_info(bool supports_mpe, AudioUnitScope scope, UInt32& out_size,
                                         bool& out_writable) {
    if (scope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidScope;
    if (!supports_mpe)
        return kAudioUnitErr_InvalidProperty;
    out_size = sizeof(UInt32);
    out_writable = false;
    return noErr;
}

OSStatus fill_supports_mpe(bool supports_mpe, AudioUnitScope scope, void* out_data) {
    if (scope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidScope;
    if (!supports_mpe)
        return kAudioUnitErr_InvalidProperty;
    if (!out_data)
        return kAudioUnitErr_InvalidProperty;
    *static_cast<UInt32*>(out_data) = 1;
    return noErr;
}

RenderContextObserverProperty::~RenderContextObserverProperty() {
    // A C-style cast: void* <-> block pointer has no named-cast form, and
    // keeping the member a plain pointer keeps the header ARC-neutral.
    if (block_)
        Block_release((AURenderContextObserver)block_);
}

void RenderContextObserverProperty::bind(Processor* processor) {
    client_ = dynamic_cast<AudioWorkgroupClient*>(processor);
    if (!client_ || block_)
        return;
    auto* client = client_;
    AURenderContextObserver observer = ^(const AudioUnitRenderContext* context) {
      // Realtime render thread, before a render whose workgroup changed.
      // Copy only the workgroup value; the context struct is
      // callback-scoped. No os_retain/os_release (see the class comment).
      client->set_audio_workgroup_from_render_context(
          context ? reinterpret_cast<void*>(context->workgroup) : nullptr);
      // The context contract requires auxiliary threads to leave the old
      // workgroup and join the new one within this call; the client's
      // barrier returns only after every worker has done so.
      (void)client->prepare_audio_workgroup_for_render();
    };
    block_ = (void*)Block_copy(observer);
}

OSStatus RenderContextObserverProperty::property_info(AudioUnitScope scope,
                                                      AudioUnitElement element, UInt32& out_size,
                                                      bool& out_writable) const {
    if (scope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidScope;
    if (element != 0)
        return kAudioUnitErr_InvalidElement;
    if (!block_)
        return kAudioUnitErr_InvalidProperty;
    out_size = sizeof(AURenderContextObserver);
    out_writable = false;
    return noErr;
}

OSStatus RenderContextObserverProperty::get(AudioUnitScope scope, AudioUnitElement element,
                                            void* out_data) const {
    if (scope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidScope;
    if (element != 0)
        return kAudioUnitErr_InvalidElement;
    if (!block_ || !out_data)
        return kAudioUnitErr_InvalidProperty;
    *static_cast<AURenderContextObserver*>(out_data) = (AURenderContextObserver)block_;
    return noErr;
}

void RenderContextObserverProperty::prepare_for_render() const noexcept {
    if (client_)
        (void)client_->prepare_audio_workgroup_for_render();
}

void RenderContextObserverProperty::clear_after_render_resources() const noexcept {
    clear_audio_workgroup_after_render_resources(client_);
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

    const auto* param = store.info(static_cast<state::ParamID>(param_id));
    if (!param) return kAudioUnitErr_InvalidParameter;
    if (!state::is_discrete_param(*param))
        return kAudioUnitErr_InvalidPropertyValue;
    // AUBase's GetPropertyInfo asks with a null array to learn whether the
    // parameter has a list before any host can fetch it.
    if (!out_strings)
        return noErr;

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

namespace compact_state {
namespace {

constexpr std::uint8_t kMagic[4] = {'P', 'L', 'C', 'S'};
constexpr std::size_t kMagicSize = sizeof(kMagic);
// magic + format version + flags + state_version
constexpr std::size_t kFixedHeaderSize = kMagicSize + 1 + 1 + 4;
constexpr std::size_t kCrcSize = 4;
// Smallest legal blob: header, zero entries (1), zero-length payload (1), CRC.
constexpr std::size_t kMinBlobSize = kFixedHeaderSize + 1 + 1 + kCrcSize;
constexpr std::size_t kMaxVarintBytes = 10;
// One entry is at least a 1-byte id and a 4-byte value.
constexpr std::size_t kMinEntrySize = 1 + 4;

struct Crc32Tables {
    std::uint32_t t[8][256];
};

constexpr Crc32Tables make_crc32_tables() {
    Crc32Tables tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        tables.t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = tables.t[0][i];
        for (int k = 1; k < 8; ++k) {
            c = tables.t[0][c & 0xFFu] ^ (c >> 8);
            tables.t[k][i] = c;
        }
    }
    return tables;
}

constexpr Crc32Tables kCrc32Tables = make_crc32_tables();

std::uint32_t load_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return choc::memory::readLittleEndian<std::uint32_t>(&v);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t buf[4];
    choc::memory::writeLittleEndian(buf, v);
    out.insert(out.end(), buf, buf + 4);
}

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

std::uint32_t float_bits(float v) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/// Bounds-checked cursor over untrusted bytes.
class Reader {
  public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - pos_;
    }

    bool read_u32(std::uint32_t& out) noexcept {
        if (remaining() < 4)
            return false;
        out = load_u32(bytes_.data() + pos_);
        pos_ += 4;
        return true;
    }

    bool read_varint(std::uint64_t& out) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < kMaxVarintBytes; ++i) {
            if (remaining() == 0)
                return false;
            const std::uint8_t byte = bytes_[pos_++];
            const unsigned shift = static_cast<unsigned>(7 * i);
            // The tenth byte may only contribute the top bit of a 64-bit value.
            if (i == kMaxVarintBytes - 1 && byte > 1)
                return false;
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                out = value;
                return true;
            }
        }
        return false;
    }

    bool skip(std::size_t n) noexcept {
        if (remaining() < n)
            return false;
        pos_ += n;
        return true;
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return pos_;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_ = 0;
};

std::vector<std::uint8_t>
build_store_blob(std::uint32_t state_version,
                 const std::vector<std::pair<state::ParamID, float>>& entries) {
    std::vector<std::uint8_t> blob;
    blob.reserve(16 + entries.size() * 8);
    blob.insert(blob.end(), {'P', 'U', 'L', 'P'});
    put_u32(blob, state_version);
    put_u32(blob, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [id, value] : entries) {
        put_u32(blob, id);
        put_u32(blob, float_bits(value));
    }
    put_u32(blob, crc32(blob));
    return blob;
}

} // namespace

const char* decode_status_name(DecodeStatus status) noexcept {
    switch (status) {
    case DecodeStatus::ok:
        return "ok";
    case DecodeStatus::truncated:
        return "truncated";
    case DecodeStatus::bad_magic:
        return "bad_magic";
    case DecodeStatus::unsupported_format_version:
        return "unsupported_format_version";
    case DecodeStatus::unsupported_flags:
        return "unsupported_flags";
    case DecodeStatus::unsupported_state_version:
        return "unsupported_state_version";
    case DecodeStatus::crc_mismatch:
        return "crc_mismatch";
    case DecodeStatus::malformed:
        return "malformed";
    }
    return "unknown";
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    const auto& t = kCrc32Tables.t;
    std::uint32_t crc = 0xFFFFFFFFu;
    const std::uint8_t* p = bytes.data();
    std::size_t len = bytes.size();
    while (len >= 8) {
        const std::uint32_t a = load_u32(p) ^ crc;
        const std::uint32_t b = load_u32(p + 4);
        crc = t[7][a & 0xFFu] ^ t[6][(a >> 8) & 0xFFu] ^ t[5][(a >> 16) & 0xFFu] ^ t[4][a >> 24] ^
              t[3][b & 0xFFu] ^ t[2][(b >> 8) & 0xFFu] ^ t[1][(b >> 16) & 0xFFu] ^ t[0][b >> 24];
        p += 8;
        len -= 8;
    }
    while (len-- > 0)
        crc = t[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

std::vector<std::uint8_t> encode(const state::StateStore& store,
                                 std::span<const std::uint8_t> plugin_payload) {
    if (plugin_payload.size() > std::numeric_limits<std::uint32_t>::max())
        return {};

    const auto params = store.all_params();
    std::vector<std::pair<state::ParamID, float>> changed;
    for (const auto& info : params) {
        const float value = store.get_value(info.id);
        if (float_bits(value) != float_bits(info.range.default_value))
            changed.emplace_back(info.id, value);
    }

    std::vector<std::uint8_t> out;
    out.reserve(kMinBlobSize + changed.size() * 9 + kMaxVarintBytes + plugin_payload.size());
    out.insert(out.end(), kMagic, kMagic + kMagicSize);
    out.push_back(kFormatVersion);
    out.push_back(0);
    put_u32(out, store.state_version());
    put_varint(out, changed.size());
    for (const auto& [id, value] : changed) {
        put_varint(out, id);
        put_u32(out, float_bits(value));
    }
    put_varint(out, plugin_payload.size());
    out.insert(out.end(), plugin_payload.begin(), plugin_payload.end());
    put_u32(out, crc32(out));
    return out;
}

DecodeStatus decode(std::span<const std::uint8_t> bytes, const state::StateStore& store,
                    Decoded& out) {
    if (bytes.size() < kMagicSize)
        return DecodeStatus::truncated;
    if (std::memcmp(bytes.data(), kMagic, kMagicSize) != 0)
        return DecodeStatus::bad_magic;
    if (bytes.size() < kMinBlobSize)
        return DecodeStatus::truncated;
    if (bytes[kMagicSize] != kFormatVersion)
        return DecodeStatus::unsupported_format_version;
    if (bytes[kMagicSize + 1] != 0)
        return DecodeStatus::unsupported_flags;

    const std::size_t body_size = bytes.size() - kCrcSize;
    if (load_u32(bytes.data() + body_size) != crc32(bytes.first(body_size)))
        return DecodeStatus::crc_mismatch;

    const std::uint32_t saved_version = load_u32(bytes.data() + kMagicSize + 2);
    if (saved_version > store.state_version())
        return DecodeStatus::unsupported_state_version;

    Reader reader(bytes.first(body_size));
    if (!reader.skip(kFixedHeaderSize))
        return DecodeStatus::truncated;

    std::uint64_t count = 0;
    if (!reader.read_varint(count))
        return DecodeStatus::malformed;
    // Bound the count by the bytes that could hold it before reserving.
    if (count > reader.remaining() / kMinEntrySize)
        return DecodeStatus::malformed;

    std::unordered_map<state::ParamID, float> saved;
    saved.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t id = 0;
        std::uint32_t value_bits = 0;
        if (!reader.read_varint(id) || id > std::numeric_limits<state::ParamID>::max() ||
            !reader.read_u32(value_bits))
            return DecodeStatus::malformed;
        float value;
        std::memcpy(&value, &value_bits, sizeof(value));
        if (!saved.emplace(static_cast<state::ParamID>(id), value).second)
            return DecodeStatus::malformed; // the writer never repeats an id
    }

    std::uint64_t payload_size = 0;
    if (!reader.read_varint(payload_size) || payload_size != reader.remaining())
        return DecodeStatus::malformed;
    const std::size_t payload_offset = reader.position();

    std::vector<std::pair<state::ParamID, float>> entries;
    if (saved_version == store.state_version()) {
        // Same schema: an unlisted parameter was at its default when saved.
        const auto params = store.all_params();
        entries.reserve(params.size());
        for (const auto& info : params) {
            const auto it = saved.find(info.id);
            entries.emplace_back(info.id,
                                 it != saved.end() ? it->second : info.range.default_value);
        }
    } else {
        // Older schema: hand only what was saved to the store's migrations. An
        // unlisted parameter keeps its current value, as an unlisted id always
        // did in the legacy format.
        entries.assign(saved.begin(), saved.end());
    }

    out.store_blob = build_store_blob(saved_version, entries);
    out.plugin_payload = bytes.subspan(payload_offset, static_cast<std::size_t>(payload_size));
    return DecodeStatus::ok;
}

} // namespace compact_state

OSStatus save_pulp_state(state::StateStore& store,
                         Processor& processor,
                         CFPropertyListRef* out_data)
{
    if (!out_data) return kAudioUnitErr_InvalidPropertyValue;

    const auto payload = plugin_state_io::plugin_state_payload(processor);
    auto data = compact_state::encode(store, *payload);
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

    CFDictionarySetValue(dict, CFSTR("pulp-state-v2"), cf_data);
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

    if (const void* compact = CFDictionaryGetValue(dict, CFSTR("pulp-state-v2"))) {
        if (CFGetTypeID(compact) != CFDataGetTypeID())
            return kAudioUnitErr_InvalidPropertyValue;
        const auto cf_data = static_cast<CFDataRef>(compact);
        const std::span<const std::uint8_t> bytes(
            CFDataGetBytePtr(cf_data), static_cast<std::size_t>(CFDataGetLength(cf_data)));

        compact_state::Decoded decoded;
        if (compact_state::decode(bytes, store, decoded) != compact_state::DecodeStatus::ok)
            return kAudioUnitErr_InvalidPropertyValue;

        auto restore_lock = gate.lock_for_restore();
        if (!plugin_state_io::deserialize_parts(decoded.store_blob, decoded.plugin_payload, store,
                                                processor))
            return kAudioUnitErr_InvalidPropertyValue;
        return noErr;
    }

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
