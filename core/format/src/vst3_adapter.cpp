// VST3 Adapter implementation
// Uses SingleComponentEffect pattern: combined processor + controller
// Parameters are registered with the VST3 parameter system and synced
// with the Pulp StateStore during processing

#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/detail/midi_out_offset.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/detail/vst3_frame_rate.hpp>
#include <pulp/format/detail/vst3_midi_mapping.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/format/max_block_contract.hpp>
#include <pulp/format/mpe_expression.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/vst3_plug_view.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/base/ustring.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pulp::format::vst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// Per-note expression types Pulp's VST3 adapter declares to the host when the
// plug-in opts into MPE. The mapping VST3 type -> MPE dimension is a clean-room
// choice derived from the VST3 note-expression value ranges (ivstnoteexpression.h)
// and the MPE spec's three per-note axes (pitch bend, pressure, timbre):
//
//   kTuningTypeID     -> per-note pitch bend  (VST3 plain = 240*(norm-0.5) st)
//   kVolumeTypeID     -> per-note pressure    (loudness axis -> channel pressure)
//   kBrightnessTypeID -> per-note timbre      (CC74, the MPE timbre controller)
//   kPanTypeID        -> declared for host completeness; not an MPE axis, so it
//                        is accepted but not routed into the sidecar.
//
// The host queries these via getNoteExpressionInfo(); process() consumes the
// matching kNoteExpressionValueEvent stream.
struct NoteExprTypeEntry {
    NoteExpressionTypeID type_id;
    const char* title;
    const char* short_title;
    const char* units;
    NoteExpressionValue default_value;  // normalized [0,1]
    bool bipolar;
};

constexpr std::array<NoteExprTypeEntry, 4> kNoteExprTypes{{
    {kTuningTypeID,     "Tuning",     "Tun", "semitones", 0.5, true},
    {kVolumeTypeID,     "Volume",     "Vol", "",          1.0, false},
    {kBrightnessTypeID, "Brightness", "Brt", "",          0.5, false},
    {kPanTypeID,        "Pan",        "Pan", "",          0.5, true},
}};

// Convert a normalized VST3 tuning value to a signed semitone offset.
// ivstnoteexpression.h: plain = 240 * (norm - 0.5), so the full [0,1] range
// spans -120 .. +120 semitones (ten octaves either way), 0.5 == no detune.
inline double vst3_tuning_norm_to_semitones(double norm) {
    return 240.0 * (std::clamp(norm, 0.0, 1.0) - 0.5);
}

// True iff `id` is one of the note-expression types this adapter declares.
inline bool is_declared_note_expr_type(NoteExpressionTypeID id) {
    for (const auto& entry : kNoteExprTypes) {
        if (entry.type_id == id) return true;
    }
    return false;
}

// Decode the host's VST3 process context into Pulp's neutral ProcessContext and
// raise this block's transport change flags.
//
// `playhead_prev` is the adapter's previous-block transport snapshot and is
// advanced in place, so a first block after construction raises no spurious
// change flags. A host that supplies no process context leaves every transport
// field at its ProcessContext default; only the change-flag diff still runs.
pulp::format::ProcessContext build_process_context(
    const ProcessData& data,
    const ProcessSetup& setup,
    int32 num_samples,
    pulp::format::detail::PlayheadSnapshot& playhead_prev) {
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = setup.sampleRate;
    ctx.num_samples = num_samples;
    ctx.process_mode = setup.processMode == Steinberg::Vst::kOffline
        ? pulp::format::ProcessMode::Offline
        : pulp::format::ProcessMode::Realtime;
    ctx.render_speed_hint = ctx.process_mode == pulp::format::ProcessMode::Offline
        ? pulp::format::RenderSpeedHint::FasterThanRealtime
        : pulp::format::RenderSpeedHint::Realtime;
    if (data.processContext) {
        const auto* pc = data.processContext;
        const uint32_t state = pc->state;
        ctx.is_playing = (state & Steinberg::Vst::ProcessContext::kPlaying) != 0;
        ctx.is_recording = (state & Steinberg::Vst::ProcessContext::kRecording) != 0;
        ctx.position_samples = pc->projectTimeSamples;
        if (state & Steinberg::Vst::ProcessContext::kTempoValid) {
            ctx.tempo_bpm = pc->tempo;
        }
        if (state & Steinberg::Vst::ProcessContext::kProjectTimeMusicValid) {
            ctx.position_beats = pc->projectTimeMusic;
        }
        if (state & Steinberg::Vst::ProcessContext::kTimeSigValid) {
            ctx.time_sig_numerator = pc->timeSigNumerator;
            ctx.time_sig_denominator = pc->timeSigDenominator;
        }

        // kCycleValid covers cycleStartMusic + cycleEndMusic; kCycleActive
        // indicates the host is currently looping. Both must be set for
        // the loop range to be meaningful.
        ctx.is_looping = (state & Steinberg::Vst::ProcessContext::kCycleActive) != 0;
        if (state & Steinberg::Vst::ProcessContext::kCycleValid) {
            ctx.loop_start_beats = pc->cycleStartMusic;
            ctx.loop_end_beats = pc->cycleEndMusic;
        }

        // Host clock for video sync.
        if (state & Steinberg::Vst::ProcessContext::kSystemTimeValid) {
            ctx.host_time_ns = pc->systemTime;
        }

        // SMPTE frame rate enum. VST3 reports it as an integer
        // framesPerSecond plus pulldown / drop flags. Map the documented
        // combinations from the VST3 FrameRate docs onto Pulp's enum.
        if (state & Steinberg::Vst::ProcessContext::kSmpteValid) {
            const auto fps = pc->frameRate.framesPerSecond;
            const auto fr_flags = pc->frameRate.flags;
            const bool pulldown =
                (fr_flags & Steinberg::Vst::FrameRate::kPullDownRate) != 0;
            const bool drop =
                (fr_flags & Steinberg::Vst::FrameRate::kDropRate) != 0;
            // Mapping table is extracted to vst3_frame_rate.hpp so it is
            // unit-testable without pulling the Steinberg VST3 SDK into
            // the test binary. Critically, 59.94 (= 60 + pulldown) MUST
            // NOT map to fps_60 — that bug broke SMPTE math in plugins
            // that trust ctx.frame_rate.
            ctx.frame_rate = pulp::format::detail::vst3_frame_rate(
                static_cast<int>(fps), pulldown, drop);
        }

        // Derive bar index from position_beats + time-sig. VST3 also
        // exposes `barPositionMusic` directly when kBarPositionValid is
        // set, but that is the quarter-note position of the last bar
        // start, not a bar index. Deriving matches `ProcessContext::bar`
        // and stays consistent with the AU/CLAP paths.
        pulp::format::detail::derive_bar_from_beats(ctx);
    }

    // Diff against the previous block to populate the transport change flags.
    pulp::format::detail::compute_playhead_changes(ctx, playhead_prev);
    return ctx;
}

// Bridge the processor's outbound MIDI back to the host's event list. Only note
// on/off cross this boundary; other message types the processor emits have no
// VST3 Event representation here and are not forwarded.
void write_midi_output(const ProcessData& data, const midi::MidiBuffer& midi_out) {
    if (!data.outputEvents || midi_out.empty()) return;
    for (const auto& me : midi_out) {
        Event evt{};
        // Shared cross-format outbound-offset contract (offset N in -> N out;
        // see detail/midi_out_offset.hpp + test_midi_out_offset_parity.cpp).
        evt.sampleOffset =
            pulp::format::detail::vst3_output_offset(me.sample_offset);
        if (me.is_note_on()) {
            evt.type = Event::kNoteOnEvent;
            evt.noteOn.channel = me.channel();
            evt.noteOn.pitch = me.note();
            evt.noteOn.velocity = me.velocity() / 127.0f;
            data.outputEvents->addEvent(evt);
        } else if (me.is_note_off()) {
            evt.type = Event::kNoteOffEvent;
            evt.noteOff.channel = me.channel();
            evt.noteOff.pitch = me.note();
            evt.noteOff.velocity = me.velocity() / 127.0f;
            data.outputEvents->addEvent(evt);
        }
    }
}

inline void resize_sample_scratch(std::vector<std::vector<float>>& scratch,
                                  int channels,
                                  int frames) {
    const auto channel_count = channels > 0 ? static_cast<std::size_t>(channels) : 0u;
    const auto frame_count = frames > 0 ? static_cast<std::size_t>(frames) : 0u;
    scratch.resize(channel_count);
    for (auto& channel : scratch)
        channel.assign(frame_count, 0.0f);
}

}  // namespace

PulpVst3Processor::PulpVst3Processor(ProcessorFactory factory)
    : factory_(factory)
{
}

tresult PLUGIN_API PulpVst3Processor::queryInterface(const TUID iid, void** obj) {
    // Expose the interfaces this subclass adds on top of SingleComponentEffect
    // (IMidiMapping for MIDI controllers, INoteExpressionController for per-note
    // expression), then delegate everything else to the base so its IComponent /
    // IAudioProcessor / IEditController surface stays intact.
    QUERY_INTERFACE(iid, obj, IMidiMapping::iid, IMidiMapping)
    QUERY_INTERFACE(iid, obj, INoteExpressionController::iid, INoteExpressionController)
    return SingleComponentEffect::queryInterface(iid, obj);
}

bool PulpVst3Processor::is_registered_controller(state::ParamID id) const {
    if (!detail::is_vst3_midi_cc_param(id)) return false;
    const auto index = static_cast<std::size_t>(id - detail::kVst3MidiCcParamBase);
    return index < registered_controller_ids_.size() &&
           registered_controller_ids_[index];
}

tresult PLUGIN_API PulpVst3Processor::getMidiControllerAssignment(
    int32 busIndex, int16 channel, CtrlNumber midiControllerNumber, ParamID& id) {
    // VST3 controllers reach the plug-in only as parameter changes: the host
    // calls this to learn which ParamID a given controller maps to. Decline
    // when this plug-in does not accept MIDI input, when the controller is
    // outside the standard range, or for any bus/channel beyond the single
    // 16-channel MIDI input bus the adapter registers.
    if (!midi_controller_mapping_active_) return kResultFalse;
    if (busIndex != 0) return kResultFalse;
    if (channel < 0 || channel >= detail::kVst3MidiChannels) return kResultFalse;
    if (midiControllerNumber < 0 ||
        midiControllerNumber >= detail::kVst3ControllersPerChannel) {
        return kResultFalse;
    }
    const auto candidate = detail::vst3_midi_cc_param_id(
        static_cast<int>(channel), static_cast<int>(midiControllerNumber));
    // Never hand the host a ParamID that collided with a real plug-in
    // parameter and was therefore NOT registered as a controller. The host
    // owns that ID as an automatable parameter; reporting it as a controller
    // would make the same ID both a parameter and a controller assignment.
    if (!is_registered_controller(candidate)) return kResultFalse;
    id = candidate;
    return kResultTrue;
}

// ── INoteExpressionController ──────────────────────────────────────────────

int32 PLUGIN_API PulpVst3Processor::getNoteExpressionCount(
    int32 busIndex, int16 channel) {
    // Per-note expression only makes sense for an MPE-aware instrument. A
    // plug-in that did not opt in declares zero types, so a host never offers
    // its tracks per-note lanes and process() does no MPE work.
    if (!mpe_enabled_) return 0;
    // Note expression is carried on the single event input bus (index 0). Any
    // other bus has no note-expression surface. The same type set applies to
    // every channel of that bus.
    if (busIndex != 0) return 0;
    if (channel < 0 || channel >= detail::kVst3MidiChannels) return 0;
    return static_cast<int32>(kNoteExprTypes.size());
}

tresult PLUGIN_API PulpVst3Processor::getNoteExpressionInfo(
    int32 busIndex, int16 channel, int32 noteExpressionIndex,
    NoteExpressionTypeInfo& info /*out*/) {
    if (!mpe_enabled_) return kResultFalse;
    if (busIndex != 0) return kResultFalse;
    if (channel < 0 || channel >= detail::kVst3MidiChannels) return kResultFalse;
    if (noteExpressionIndex < 0 ||
        noteExpressionIndex >= static_cast<int32>(kNoteExprTypes.size())) {
        return kResultFalse;
    }

    const auto& entry =
        kNoteExprTypes[static_cast<std::size_t>(noteExpressionIndex)];

    info = NoteExpressionTypeInfo{};
    info.typeId = entry.type_id;
    Steinberg::UString(info.title, 128).fromAscii(entry.title);
    Steinberg::UString(info.shortTitle, 128).fromAscii(entry.short_title);
    Steinberg::UString(info.units, 128).fromAscii(entry.units);
    info.unitId = Steinberg::Vst::kRootUnitId;
    info.valueDesc.defaultValue = entry.default_value;
    info.valueDesc.minimum = 0.0;
    info.valueDesc.maximum = 1.0;
    info.valueDesc.stepCount = 0;  // continuous
    // No global parameter is associated with these note-expression types, so
    // use the SDK's "no parameter" sentinel rather than a literal 0 — a host
    // must not associate the type with real plug-in parameter id 0. The
    // kAssociatedParameterIDValid flag stays clear, so this field is advisory.
    info.associatedParameterId = Steinberg::Vst::kNoParamId;
    info.flags = entry.bipolar
                     ? NoteExpressionTypeInfo::kIsBipolar
                     : 0;
    return kResultTrue;
}

tresult PLUGIN_API PulpVst3Processor::getNoteExpressionStringByValue(
    int32 busIndex, int16 channel, NoteExpressionTypeID id,
    NoteExpressionValue valueNormalized /*in*/, String128 string /*out*/) {
    // Decline anything outside the surface the plug-in actually declares:
    // not MPE, wrong bus, a channel outside the event bus, or a type id that
    // is not one of the declared kNoteExprTypes.
    if (!mpe_enabled_ || busIndex != 0) return kResultFalse;
    if (channel < 0 || channel >= detail::kVst3MidiChannels) return kResultFalse;
    if (!is_declared_note_expr_type(id)) return kResultFalse;

    // Tuning gets a semitone read-out; the remaining types fall back to the
    // normalized value. A minimal identity-style conversion is acceptable per
    // the interface contract — the value events drive the audio, not the string.
    char buf[128] = {0};
    if (id == kTuningTypeID) {
        std::snprintf(buf, sizeof(buf), "%.2f",
                      vst3_tuning_norm_to_semitones(valueNormalized));
    } else {
        std::snprintf(buf, sizeof(buf), "%.4f",
                      std::clamp(static_cast<double>(valueNormalized), 0.0, 1.0));
    }
    Steinberg::UString(string, 128).fromAscii(buf);
    return kResultTrue;
}

tresult PLUGIN_API PulpVst3Processor::getNoteExpressionValueByString(
    int32 busIndex, int16 channel, NoteExpressionTypeID id,
    const TChar* string /*in*/, NoteExpressionValue& valueNormalized /*out*/) {
    if (!mpe_enabled_ || busIndex != 0 || string == nullptr) return kResultFalse;
    if (channel < 0 || channel >= detail::kVst3MidiChannels) return kResultFalse;
    if (!is_declared_note_expr_type(id)) return kResultFalse;

    // Parse the ASCII rendering of the string back to a normalized value, the
    // inverse of getNoteExpressionStringByValue.
    char ascii[128] = {0};
    Steinberg::UString(const_cast<TChar*>(string), 128).toAscii(ascii, sizeof(ascii));
    char* end = nullptr;
    const double parsed = std::strtod(ascii, &end);
    if (end == ascii) return kResultFalse;  // not a number

    if (id == kTuningTypeID) {
        // Inverse of plain = 240*(norm-0.5): norm = plain/240 + 0.5.
        valueNormalized = std::clamp(parsed / 240.0 + 0.5, 0.0, 1.0);
    } else {
        valueNormalized = std::clamp(parsed, 0.0, 1.0);
    }
    return kResultTrue;
}

// ── Parameter value <-> display string ──────────────────────────────────────
//
// VST3 hands these methods a NORMALIZED [0,1] value; the plugin author's
// ParamInfo::to_string / from_string work in PLAIN (min..max) units, so we
// denormalize/normalize through the parameter's ParamRange on the way in/out.
// When a parameter declares no converter, defer to SingleComponentEffect so
// the host's stock formatting is preserved (zero behavior change for existing
// plugins). The hidden MIDI-controller parameters have no ParamInfo, so they
// take the base path automatically.

tresult PLUGIN_API PulpVst3Processor::getParamStringByValue(
    ParamID tag, ParamValue valueNormalized, String128 string) {
    const auto* info = store_.info(static_cast<state::ParamID>(tag));
    if (info) {
        const float plain =
            info->range.denormalize(static_cast<float>(valueNormalized));
        const std::string text = format_parameter_text(*info, plain);
        if (text.empty()) return kResultFalse;
        // fromAscii matches the rest of this adapter's String128 handling
        // (parameter titles/units are converted the same way). Typical display
        // units (dB, Hz, %, semitones) are ASCII.
        Steinberg::UString(string, 128).fromAscii(text.c_str());
        return kResultTrue;
    }
    return SingleComponentEffect::getParamStringByValue(tag, valueNormalized, string);
}

tresult PLUGIN_API PulpVst3Processor::getParamValueByString(
    ParamID tag, TChar* string, ParamValue& valueNormalized) {
    const auto* info = store_.info(static_cast<state::ParamID>(tag));
    if (info && string) {
        char ascii[256] = {0};
        Steinberg::UString(string, 128).toAscii(ascii, sizeof(ascii));
        if (const auto plain = parse_parameter_text(*info, ascii)) {
            valueNormalized = info->range.normalize(*plain);
            return kResultTrue;
        }
        return kResultFalse;
    }
    return SingleComponentEffect::getParamValueByString(tag, string, valueNormalized);
}

// ── noteId -> (channel, note) linkage ──────────────────────────────────────

bool PulpVst3Processor::note_id_map_insert(int32 note_id, uint8_t channel,
                                           uint8_t note) {
    // A host that supplies no noteId (-1) is not a drop: there is simply
    // nothing to track, and a later expression with noteId -1 would not match
    // anyway. Report success so the caller does not count it as a failure.
    if (note_id < 0) return true;
    // Reuse an existing slot for the same noteId (a retrigger), else claim a
    // free slot. If the table is full the mapping is dropped (RT-safe); the
    // expression for that note simply won't route — never an allocation. The
    // caller bumps note_expression_drops_ on a false return.
    NoteIdSlot* free_slot = nullptr;
    for (auto& slot : note_id_map_) {
        if (slot.active && slot.note_id == note_id) {
            slot.channel = channel;
            slot.note = note;
            return true;
        }
        if (!slot.active && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    if (free_slot) {
        free_slot->active = true;
        free_slot->note_id = note_id;
        free_slot->channel = channel;
        free_slot->note = note;
        return true;
    }
    return false;  // table full — mapping dropped
}

void PulpVst3Processor::note_id_map_erase(int32 note_id) {
    if (note_id < 0) return;
    for (auto& slot : note_id_map_) {
        if (slot.active && slot.note_id == note_id) {
            slot.active = false;
            slot.note_id = -1;
            return;
        }
    }
}

const PulpVst3Processor::NoteIdSlot*
PulpVst3Processor::note_id_map_find(int32 note_id) const {
    if (note_id < 0) return nullptr;
    for (const auto& slot : note_id_map_) {
        if (slot.active && slot.note_id == note_id) return &slot;
    }
    return nullptr;
}

void PulpVst3Processor::note_id_map_clear() {
    for (auto& slot : note_id_map_) {
        slot.active = false;
        slot.note_id = -1;
    }
    // Reset the saturating drop counter at session boundaries (setup /
    // deactivate) so it reflects the current activation, not stale history.
    // Not reset per block — a host polls it to learn it exceeded capacity.
    note_expression_drops_.store(0, std::memory_order_relaxed);
}

tresult PLUGIN_API PulpVst3Processor::initialize(FUnknown* context) {
    auto result = SingleComponentEffect::initialize(context);
    if (result != kResultOk) return result;

    // An ARA-aware VST3 host (Cubase, Studio One) queries
    // IHostApplication from `context` and publishes its ARA factory under
    // the well-known key pulp::format::kVst3AraFactoryContextKey. Plugins
    // opt in to ARA by overriding Processor::create_ara_document_controller();
    // the adapter surfaces Pulp's companion factory through VST3 host
    // context negotiation.
    if (context) {
        FUnknownPtr<IHostApplication> host_app(context);
        if (host_app) {
            runtime::log_info(
                "VST3: IHostApplication present; publishing ARA factory "
                "under kVst3AraFactoryContextKey='{}'",
                pulp::format::kVst3AraFactoryContextKey);
            // Surface our factory. When non-null (PULP_HAS_ARA build) this
            // is what an ARA-aware host receives via its setHostContext
            // negotiation — the string key matches Celemony's convention.
            (void)pulp::format::ara_companion_factory_for(nullptr);
        }
    }

    // Create the Pulp processor
    processor_ = factory_();
    if (!processor_) return kInternalError;

    // Resolve host accommodations once. Cross-host defenses such as
    // clamp_latency_to_nonneg apply regardless of the detected DAW;
    // host-specific quirks key off the detected host. The runtime policy
    // applies here via resolved_quirks().
    {
        const auto host_info = detect_host_info();
        quirks_ = resolved_quirks(host_info.type, host_info.version);
    }

    auto desc = processor_->descriptor();
    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);

    // Wire the MPE sidecar when the plug-in opts in. The tracker's callbacks
    // append per-note expression deltas to mpe_buffer_; bind them once here on
    // the host thread (never on the audio thread). The buffer is reserved +
    // capacity-limited in setupProcessing(). mpe_enabled_ also gates the
    // INoteExpressionController surface so a non-MPE plug-in advertises nothing.
    mpe_enabled_ = desc.effective_capabilities().supports_mpe;
    if (mpe_enabled_) {
        midi::bind_tracker_to_buffer(
            mpe_tracker_, mpe_buffer_, mpe_current_sample_offset_);
        runtime::log_info("VST3: MPE note-expression sidecar enabled for '{}'",
                          desc.name);
    }

    // If a host accommodation synthesizes a bypass parameter, inject it
    // before parameter registration so the loop below tags it kIsBypass
    // and caches bypass_param_id_. No-op when the accommodation is off.
    maybe_synthesize_bypass(store_, quirks_);

    // Wire gesture callbacks to VST3 host
    store_.set_gesture_callbacks(
        [this](state::ParamID id) {
            beginEdit(static_cast<ParamID>(id));
        },
        [this](state::ParamID id) {
            endEdit(static_cast<ParamID>(id));
        }
    );

    // Add audio buses from descriptor (supports multi-bus: main, sidechain, aux)
    for (const auto& bus : desc.input_buses) {
        Steinberg::Vst::String128 busName;
        Steinberg::UString(busName, 128).fromAscii(bus.name.c_str());
        addAudioInput(busName,
            bus.default_channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo,
            bus.optional ? Steinberg::Vst::BusTypes::kAux : Steinberg::Vst::BusTypes::kMain,
            bus.optional ? 0 : Steinberg::Vst::BusInfo::kDefaultActive);
    }
    for (const auto& bus : desc.output_buses) {
        Steinberg::Vst::String128 busName;
        Steinberg::UString(busName, 128).fromAscii(bus.name.c_str());
        addAudioOutput(busName,
            bus.default_channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo,
            bus.optional ? Steinberg::Vst::BusTypes::kAux : Steinberg::Vst::BusTypes::kMain,
            bus.optional ? 0 : Steinberg::Vst::BusInfo::kDefaultActive);
    }

    // Add event buses for MIDI. The second argument is the advertised MIDI
    // channel count; hosts query controller mappings and outputs per channel.
    if (desc.accepts_midi) {
        addEventInput(STR16("MIDI In"), 16);
    }
    if (desc.produces_midi) {
        addEventOutput(STR16("MIDI Out"), 16);
    }

    // Register Pulp parameters with the VST3 parameter system
    for (const auto& param : store_.all_params()) {
        int32 flags = ParameterInfo::kCanAutomate;

        int32 step_count = 0;
        if (state::is_discrete_param(param)) {
            const auto count = state::param_value_count(param);
            step_count = count > 0 ? static_cast<int32>(count - 1u) : 0;
        }

        // Tag the bypass param via the shared designation-first contract: a
        // declared `Bypass` designation wins, otherwise the legacy
        // boolean-"Bypass" name/range heuristic applies (see is_bypass_param).
        if (state::is_bypass_param(param)) {
            flags |= ParameterInfo::kIsBypass;
            // kIsBypass must advertise a toggle. A declared `Bypass`
            // designation is interpreted as off<0.5 / on>=0.5 regardless of its
            // ParamRange, and process() short-circuits on that same threshold,
            // so FORCE a 2-state step count here even when the range is
            // continuous. (The legacy heuristic only ever matches a boolean
            // range, so this is a no-op for it.) Hosts must see a bypass control
            // as a toggle, not a continuous knob.
            step_count = 1;
            // Remember which ParamID carries kIsBypass so process()
            // can short-circuit to pass-through audio without
            // invoking the Processor when the host sets it.
            bypass_param_id_ = param.id;
        }

        // Build ParameterInfo with unit assignment
        Steinberg::Vst::ParameterInfo pinfo{};
        pinfo.id = static_cast<ParamID>(param.id);
        Steinberg::UString(pinfo.title, 128).fromAscii(param.name.c_str());
        Steinberg::UString(pinfo.units, 128).fromAscii(param.unit.c_str());
        pinfo.stepCount = step_count;
        pinfo.defaultNormalizedValue = static_cast<ParamValue>(
            param.range.normalize(param.range.default_value));
        pinfo.flags = flags;
        pinfo.unitId = static_cast<Steinberg::Vst::UnitID>(param.group_id);

        parameters.addParameter(pinfo);
    }

    // Register hidden MIDI-controller parameters so the host accepts the
    // IMidiMapping assignments. VST3 requires every ParamID returned from
    // getMidiControllerAssignment to be a registered parameter; these are
    // flagged kIsHidden (NOT kCanAutomate) so they carry controller traffic
    // without cluttering the host's automation lanes. Only registered when
    // the plug-in accepts MIDI input, mirroring the event-bus gating — an
    // effect that ignores MIDI keeps its exact author-declared parameter
    // set, so existing parameter-count contracts are unaffected.
    //
    // Count: 16 channels × 130 controllers (128 CCs + channel aftertouch +
    // pitch bend) = 2080 hidden parameters, in the reserved ParamID range
    // [kVst3MidiCcParamBase, kVst3MidiCcParamEnd). The range is far above
    // any author-assigned ID; the per-ID collision guard below makes the
    // no-overlap guarantee hold even for a pathological high-ID plug-in.
    if (desc.accepts_midi) {
        midi_controller_mapping_active_ = true;
        // Membership bitmap: one bit per reserved controller slot. A slot is
        // set only when its ID was actually registered as a hidden controller
        // — i.e. it did not collide with a real plug-in parameter. This is the
        // single predicate used by getMidiControllerAssignment() and the
        // process()-side diversion below.
        registered_controller_ids_.assign(
            static_cast<std::size_t>(detail::kVst3MidiChannels *
                                     detail::kVst3ControllersPerChannel),
            false);
        for (int channel = 0; channel < detail::kVst3MidiChannels; ++channel) {
            for (int controller = 0;
                 controller < detail::kVst3ControllersPerChannel; ++controller) {
                const auto cc_id =
                    detail::vst3_midi_cc_param_id(channel, controller);
                // Never shadow a real plug-in parameter. The reserved range
                // cannot collide in practice, but if a plug-in ever picked an
                // ID inside it, the real parameter wins: we leave the bitmap
                // slot clear so this controller is neither registered, nor
                // reported by getMidiControllerAssignment, nor diverted from
                // store_ in process(). One consistent predicate, no corruption.
                if (store_.info(static_cast<state::ParamID>(cc_id)) != nullptr) {
                    runtime::log_info(
                        "VST3: MIDI controller ParamID {} collides with a "
                        "plugin parameter; skipping controller mapping for "
                        "channel {} controller {}",
                        cc_id, channel, controller);
                    continue;
                }
                Steinberg::Vst::ParameterInfo cinfo{};
                cinfo.id = cc_id;
                cinfo.stepCount = 0;
                cinfo.defaultNormalizedValue = 0.0;
                cinfo.flags = ParameterInfo::kIsHidden;
                cinfo.unitId = Steinberg::Vst::kRootUnitId;
                parameters.addParameter(cinfo);
                registered_controller_ids_[static_cast<std::size_t>(
                    cc_id - detail::kVst3MidiCcParamBase)] = true;
            }
        }
    }

    runtime::log_info("VST3: initialized '{}' with {} parameters",
                      desc.name, store_.param_count());

    // Opt this plugin instance into the process-wide MainThreadDispatcher
    // backend. On macOS this installs or refcounts a Cocoa backend
    // posting to `dispatch_get_main_queue`, so host callbacks and view
    // code can post work to the DAW's main thread.
    main_thread_token_ = pulp::events::register_plugin_backend();

    // Liveness token shared with every main-thread lambda the restart path
    // posts, and the active flag that gates the paced poll loop. Created here
    // so a callback posted before terminate() but run after it sees a false
    // flag and skips touching this (now-destroyed) instance.
    drain_alive_ = std::make_shared<std::atomic<bool>>(true);
    poll_active_ = std::make_shared<std::atomic<bool>>(false);

    return kResultOk;
}

tresult PLUGIN_API PulpVst3Processor::terminate() {
    // Invalidate the liveness token FIRST so any main-thread lambda still
    // queued on the host run loop (a one-shot drain or a paced poll tick)
    // becomes a no-op and cannot dereference this instance after it is freed.
    // unregister_plugin_backend() additionally waits for in-flight backend
    // callbacks to drain, but the flag is the authoritative guard.
    if (poll_active_) poll_active_->store(false, std::memory_order_release);
    if (drain_alive_) drain_alive_->store(false, std::memory_order_release);

    // Symmetric teardown of the MainThreadDispatcher backend installed in
    // initialize().
    if (main_thread_token_ != 0) {
        pulp::events::unregister_plugin_backend(main_thread_token_);
        main_thread_token_ = 0;
    }
    processor_.reset();
    return SingleComponentEffect::terminate();
}

IPlugView* PLUGIN_API PulpVst3Processor::createView(FIDString name) {
    // VST3 hosts call createView("editor") to get the plugin's GUI
    if (name && strcmp(name, ViewType::kEditor) == 0) {
#ifdef PULP_VST3_GUI
        if (pulp::format::detail::editor_launch_blocked_by_environment()) {
            runtime::log_info("VST3 editor: disabled in headless/CI/test environment");
            return nullptr;
        }
        if (processor_ && processor_->has_editor()) {
            auto* view = new PulpPlugView(*processor_, store_);
            return view;
        }
#endif
    }
    return nullptr;
}

tresult PLUGIN_API PulpVst3Processor::setBusArrangements(
    SpeakerArrangement* inputs, int32 numIns,
    SpeakerArrangement* outputs, int32 numOuts)
{
    // VST3 hosts call this when the project's channel layout changes
    // (e.g. loading a stereo project over a mono plugin slot) and
    // expect the plug-in to either accept the request and apply it
    // to every bus or reject the whole proposal with kResultFalse.
    // Our prior impl just delegated to the parent, which left the
    // plug-in's view of `processor_->descriptor()` out of sync with
    // the actual VST3 bus channel counts.
    //
    // Strategy:
    //   * Reject if counts don't match what the descriptor declared.
    //   * Otherwise update every bus's ChannelCount in place and
    //     re-propagate to the base class. The descriptor's bus count
    //     is authoritative for bus identity; the *channel* count
    //     within each bus may shift mono↔stereo.
    if (!processor_) return kResultFalse;
    auto desc = processor_->descriptor();

    if (numIns != static_cast<int32>(desc.input_buses.size()) ||
        numOuts != static_cast<int32>(desc.output_buses.size())) {
        return kResultFalse;
    }

    // Only mono + stereo are translatable to a Processor::BusesLayout
    // proposal today; any other arrangement is unsupported by definition.
    // The Processor also gets a veto on the proposed mono/stereo layout.
    auto is_mono_stereo = [](SpeakerArrangement a) {
        return a == SpeakerArr::kMono || a == SpeakerArr::kStereo;
    };
    auto channel_count = [](SpeakerArrangement a) -> int {
        if (a == SpeakerArr::kMono)   return 1;
        if (a == SpeakerArr::kStereo) return 2;
        return 0;
    };

    bool all_mono_stereo = true;
    for (int32 i = 0; i < numIns;  ++i) if (!is_mono_stereo(inputs[i]))  all_mono_stereo = false;
    for (int32 i = 0; i < numOuts; ++i) if (!is_mono_stereo(outputs[i])) all_mono_stereo = false;

    bool natively_supported = false;
    if (all_mono_stereo) {
        Processor::BusesLayout proposal;
        proposal.inputs.reserve(static_cast<std::size_t>(numIns));
        proposal.outputs.reserve(static_cast<std::size_t>(numOuts));
        for (int32 i = 0; i < numIns;  ++i) proposal.inputs.push_back(channel_count(inputs[i]));
        for (int32 i = 0; i < numOuts; ++i) proposal.outputs.push_back(channel_count(outputs[i]));
        natively_supported = processor_->is_bus_layout_supported(proposal);
    }

    // Apply channel counts to the AudioBus objects the parent registered
    // from descriptor() during initialize(). setArrangement is the VST3
    // SDK's canonical in-place mutator for a bus's channel layout.
    auto apply_arrangements = [&] {
        for (int32 i = 0; i < numIns; ++i)
            if (auto* bus = FCast<AudioBus>(audioInputs.at(i))) bus->setArrangement(inputs[i]);
        for (int32 i = 0; i < numOuts; ++i)
            if (auto* bus = FCast<AudioBus>(audioOutputs.at(i))) bus->setArrangement(outputs[i]);
    };

    if (!natively_supported) {
        // When the arrangement is a mono/stereo layout the processor
        // explicitly vetoed, honor that veto. It encodes a real contract
        // such as linked main/sidechain channel counts or stereo-only
        // output, and there are no extra channels the silence
        // accommodation could neutralize.
        if (all_mono_stereo) {
            runtime::log_info(
                "VST3 setBusArrangements: rejected processor-vetoed mono/stereo "
                "layout ({} in / {} out buses) — honoring is_bus_layout_supported",
                numIns, numOuts);
            return kResultFalse;
        }

        // The arrangement is not expressible as mono/stereo, such as 5.1,
        // but the host policy can request graceful silence instead of
        // rejection. process() clamps the processor's views to prepared
        // descriptor-default counts and zero-fills the host's surplus
        // channels so the processor never reads or writes past what
        // prepare() allocated.
        if (!quirks_.silence_unsupported_bus_arrangements) {
            runtime::log_info(
                "VST3 setBusArrangements: rejected unsupported layout "
                "({} in / {} out buses) — silence accommodation off",
                numIns, numOuts);
            return kResultFalse;
        }
        apply_arrangements();
        silence_unsupported_active_ = true;
        runtime::log_info(
            "VST3 setBusArrangements: accepted UNSUPPORTED layout via silence "
            "accommodation ({} in / {} out buses); extra channels silenced",
            numIns, numOuts);
        return kResultTrue;
    }

    silence_unsupported_active_ = false;
    apply_arrangements();
    runtime::log_info(
        "VST3 setBusArrangements: accepted {} in / {} out buses", numIns, numOuts);
    return kResultTrue;
}

tresult PLUGIN_API PulpVst3Processor::canProcessSampleSize(int32 symbolicSampleSize) {
    if (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64)
        return kResultTrue;
    return kResultFalse;
}

tresult PLUGIN_API PulpVst3Processor::setupProcessing(ProcessSetup& setup) {
    if (!processor_) return kInternalError;

    // Query channel counts from the plugin descriptor
    auto desc = processor_->descriptor();
    int in_ch = desc.default_input_channels();
    int out_ch = desc.default_output_channels();

    PrepareContext ctx;
    ctx.sample_rate = setup.sampleRate;
    ctx.max_buffer_size = setup.maxSamplesPerBlock;
    ctx.input_channels = in_ch;
    ctx.output_channels = out_ch;

    processor_->prepare(ctx);
    processor_->prepare_f64_fallback_scratch(ctx);
    native_f64_enabled_ = desc.effective_capabilities().supports_f64_audio;
    selected_sample_size_ = setup.symbolicSampleSize == kSample64 ? kSample64 : kSample32;

    // Cache what the processor's buffers are prepared for; the silence
    // accommodation clamps process() views to these counts.
    native_in_ = in_ch;
    native_out_ = out_ch;

    // Cache the prepared block size so process() can clamp an oversized
    // render down to it (defensive guard; well-behaved hosts never exceed
    // the advertised max).
    max_block_size_ = setup.maxSamplesPerBlock;

    // Pre-allocate buffer pointer arrays for real-time safety
    input_ptrs_.resize(ctx.input_channels);
    output_ptrs_.resize(ctx.output_channels);
    input64_ptrs_.resize(ctx.input_channels);
    output64_ptrs_.resize(ctx.output_channels);
    sidechain64_ptrs_.resize(
        desc.input_buses.size() > 1 ? desc.input_buses[1].default_channels : 0);
    resize_sample_scratch(f64_input_scratch_, ctx.input_channels, ctx.max_buffer_size);
    resize_sample_scratch(f64_output_scratch_, ctx.output_channels, ctx.max_buffer_size);
    resize_sample_scratch(f64_sidechain_scratch_,
                          desc.input_buses.size() > 1 ? desc.input_buses[1].default_channels : 0,
                          ctx.max_buffer_size);

    // Pre-size per-bus channel-pointer storage for the secondary (aux) output
    // buses the descriptor declares (everything past the main bus at index 0).
    // Each sub-vector is sized to the ACCEPTED VST3 bus arrangement — which
    // setBusArrangements() may have shifted mono↔stereo away from the descriptor
    // default — so the routing path in process() reuses this storage, never
    // allocates on the audio thread, and never drops a channel the host
    // negotiated. We size to max(accepted, declared) so a host that presents
    // the descriptor default after a wider negotiation still fits. The declared
    // count is recorded separately for the aux view's declared_channels.
    const std::size_t declared_output_buses = desc.output_buses.size();
    const std::size_t aux_bus_count =
        declared_output_buses > 0 ? declared_output_buses - 1 : 0;
    aux_output_ptrs_.assign(aux_bus_count, {});
    aux_output64_ptrs_.assign(aux_bus_count, {});
    declared_aux_channels_.assign(aux_bus_count, 0);
    for (std::size_t b = 1; b < declared_output_buses; ++b) {
        const int declared = desc.output_buses[b].default_channels;
        declared_aux_channels_[b - 1] = declared;
        int accepted = declared;
        if (auto* bus = Steinberg::FCast<Steinberg::Vst::AudioBus>(
                audioOutputs.at(static_cast<int32>(b)))) {
            accepted = static_cast<int>(
                Steinberg::Vst::SpeakerArr::getChannelCount(bus->getArrangement()));
        }
        const int storage = (std::max)(declared, accepted);
        aux_output_ptrs_[b - 1].assign(
            storage > 0 ? static_cast<std::size_t>(storage) : 0, nullptr);
        aux_output64_ptrs_[b - 1].assign(
            storage > 0 ? static_cast<std::size_t>(storage) : 0, nullptr);
    }
    f64_aux_output_scratch_.assign(aux_bus_count, {});
    for (std::size_t b = 0; b < aux_bus_count; ++b) {
        f64_aux_output_scratch_[b].resize(aux_output_ptrs_[b].size());
        for (auto& channel : f64_aux_output_scratch_[b]) {
            channel.assign(
                ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 0u,
                0.0f);
        }
    }

    // Pre-allocate the per-block MIDI buffers and switch them to
    // realtime-capacity mode so add()/add_sysex_copy() in process() never grow
    // (and therefore never heap-allocate) on the audio thread. The worst-case
    // counts are generous for a single block; anything beyond is dropped
    // (RT-safe) rather than reallocated. 64 SysEx payloads × 512 bytes covers
    // typical MIDI-CI / device-inquiry traffic without a per-event allocation.
    constexpr std::size_t kMaxEventsPerBlock = 2048;
    constexpr std::size_t kMaxSysexPerBlock = 64;
    constexpr std::size_t kMaxSysexPayloadBytes = 512;
    midi_in_.reserve(kMaxEventsPerBlock, kMaxSysexPerBlock, kMaxSysexPayloadBytes);
    midi_out_.reserve(kMaxEventsPerBlock, kMaxSysexPerBlock, kMaxSysexPayloadBytes);
    midi_in_.set_realtime_capacity_limit(true);
    midi_out_.set_realtime_capacity_limit(true);

    // Reserve the MPE sidecar so process() never allocates: one inbound MIDI
    // event can fan out to several MPE callbacks (note-on + bend + pressure +
    // timbre), so size the buffer at the same worst-case event ceiling as the
    // MIDI buffers and pin it to realtime-capacity mode. Reset the noteId map
    // and tracker so a fresh activation starts with no stale per-note state.
    mpe_buffer_.reserve(kMaxEventsPerBlock);
    mpe_buffer_.set_realtime_capacity_limit(true);
    mpe_tracker_.reset();
    note_id_map_clear();

    // Size the per-channel dry-delay used by the bypass pass-through. The
    // host compensates the plugin path by getLatencySamples(), so the
    // bypassed dry copy must be delayed by exactly that many samples to stay
    // aligned with the host's plugin-delay-compensation. Allocate here, never
    // in process(); a 0 latency leaves the delay lines unused so the bypass
    // pass-through stays a zero-copy memcpy.
    bypass_.prepare(static_cast<int>(
        reported_latency_samples(processor_->latency_samples(), quirks_)));

    return SingleComponentEffect::setupProcessing(setup);
}

// Cadence of the paced main-thread restart poll. ~33ms (≈30 Hz) is well below
// any audible / PDC-relevant latency, far above a busy loop, and cheap: an
// idle tick is one relaxed atomic load and an early return.
namespace { constexpr int kRestartPollIntervalMs = 33; }

// Main-thread drain of the restart publisher. process() only accumulates the
// pending restart flags into restart_publisher_ (RT-safe); the host callback
// fires here, off the audio thread. Called from the paced poll tick and from
// main-thread host entrypoints (setActive / getLatencySamples /
// getTailSamples / getState).
void PulpVst3Processor::drain_pending_restart() {
    // Nothing armed — cheap early-out (one acquire atomic load).
    if (!restart_publisher_.dispatch_armed()) return;

    // If a main-thread backend is registered, only deliver when we are
    // genuinely on the main thread. When no backend is registered, VST3's
    // threading contract says these entrypoints run on the host's UI/main
    // thread, so delivery is safe; we degrade to delivering directly.
    if (pulp::events::MainThreadDispatcher::has_backend() &&
        !pulp::events::MainThreadDispatcher::is_main_thread()) {
        // We are not on the main thread (e.g. a host that calls a query from a
        // worker). Arrange a one-shot main-thread post. call_async may allocate
        // / lock, which is fine here because this never runs on the audio
        // thread. The lambda is lifetime-safe: it captures the shared alive
        // flag and only touches `this` while the flag is true (terminate()
        // clears it before the component is destroyed).
        auto alive = drain_alive_;
        pulp::events::MainThreadDispatcher::call_async([this, alive] {
            if (alive && alive->load(std::memory_order_acquire))
                deliver_pending_restart();
        });
        return;
    }

    deliver_pending_restart();
}

void PulpVst3Processor::deliver_pending_restart() {
    auto* handler = getComponentHandler();
    restart_publisher_.poll_main_thread([handler](int32 flags) {
        // restartComponent is a host callback: may lock / allocate / re-enter.
        // Safe here because this only ever runs on the main thread.
        if (handler) handler->restartComponent(flags);
    });
}

void PulpVst3Processor::start_restart_poll() {
    if (!poll_active_) return;
    // Mark active and kick the first tick. Idempotent: re-marking active while
    // a tick chain is already running just keeps it running (the chain checks
    // poll_active_ before re-posting, so there is at most one live chain per
    // activation as long as start is only called from setActive(true)).
    poll_active_->store(true, std::memory_order_release);
    schedule_restart_poll_tick();
}

void PulpVst3Processor::schedule_restart_poll_tick() {
    // Capture the shared liveness + active flags by value so a tick that fires
    // after terminate()/deactivation is a safe no-op and never re-posts.
    auto alive = drain_alive_;
    auto active = poll_active_;
    pulp::events::MainThreadDispatcher::call_async_after(
        [this, alive, active] {
            if (!alive || !alive->load(std::memory_order_acquire)) return;
            if (!active || !active->load(std::memory_order_acquire)) return;
            deliver_pending_restart();
            // Re-post only while still active. Stopping is cooperative: when
            // setActive(false)/terminate() clears `active`, the next tick
            // returns early above and the chain ends.
            if (active->load(std::memory_order_acquire))
                schedule_restart_poll_tick();
        },
        kRestartPollIntervalMs);
}

tresult PLUGIN_API PulpVst3Processor::setActive(TBool state) {
    if (!state && processor_) {
        processor_->release();
        // Drop any per-note expression state so a re-activation does not route
        // a stale noteId to a voice that no longer exists.
        mpe_tracker_.reset();
        note_id_map_clear();
    }
    // Activation transitions run on the main thread — flush any restart the
    // audio thread accumulated, and start/stop the paced poll so a mid-stream
    // change while active is delivered without an incidental host query.
    drain_pending_restart();
    if (poll_active_) {
        if (state) {
            start_restart_poll();
        } else {
            poll_active_->store(false, std::memory_order_release);
        }
    }
    return SingleComponentEffect::setActive(state);
}

uint32 PLUGIN_API PulpVst3Processor::getLatencySamples() {
    // Host re-queries latency on the main thread after a restart; flush any
    // pending restart notification first so the report and the host's PDC
    // refresh stay in step. (Belt-and-suspenders alongside the paced poll.)
    drain_pending_restart();
    if (!processor_) return 0;
    // VST3 reports latency as unsigned, so a negative latency_samples()
    // would wrap to a huge value without the host-quirk clamp. When the
    // quirk is filtered out, the raw value passes through.
    return static_cast<uint32>(
        reported_latency_samples(processor_->latency_samples(), quirks_));
}

uint32 PLUGIN_API PulpVst3Processor::getTailSamples() {
    // Same rationale as getLatencySamples: a host re-queries the tail on the
    // main thread after a kReloadComponent restart, so flush here.
    drain_pending_restart();
    if (!processor_) return 0;
    auto tail = processor_->descriptor().tail_samples;
    if (tail < 0) return Steinberg::Vst::kInfiniteTail;
    return static_cast<uint32>(tail);
}

void PulpVst3Processor::process_decode_input_parameters(ProcessData& data) {
    param_events_.clear();

    // Reset the reused per-block MIDI buffers up front. They are cleared here
    // (not just before the event loop below) because MIDI controllers arrive
    // as parameter changes in the loop that follows and are decoded straight
    // into midi_in_; both the reserved short-event store and the SysEx sidecar
    // must be empty before either source appends. clear()/clear_sysex() recycle
    // reserved capacity, so this stays allocation-free.
    midi_in_.clear();
    midi_in_.clear_sysex();
    midi_out_.clear();
    midi_out_.clear_sysex();

    // Read parameter changes from host and sync to Pulp StateStore
    if (data.inputParameterChanges) {
        int32 count = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            auto* queue = data.inputParameterChanges->getParameterData(i);
            if (queue) {
                ParamID id = queue->getParameterId();
                int32 point_count = queue->getPointCount();
                if (point_count > 0) {
                    // Parameter changes whose ID is a REGISTERED hidden
                    // controller are MIDI controllers the host routed through
                    // IMidiMapping, not real plug-in parameters. The predicate
                    // is is_registered_controller() (the same bitmap the
                    // registration and assignment paths use), NOT a bare range
                    // test: a real plug-in parameter that happens to fall in
                    // the reserved range was never registered as a controller,
                    // so it is NOT diverted here and still reaches store_.
                    // Decode each point into a sample-accurate MIDI message on
                    // the same midi_in_ buffer the note/SysEx path uses; these
                    // never touch store_ or param_events_.
                    if (is_registered_controller(static_cast<state::ParamID>(id))) {
                        const auto decoded = detail::vst3_decode_midi_cc_param(
                            static_cast<state::ParamID>(id));
                        const auto channel =
                            static_cast<uint8_t>(decoded.channel);
                        for (int32 point = 0; point < point_count; ++point) {
                            ParamValue value;
                            int32 offset;
                            if (queue->getPoint(point, offset, value) !=
                                kResultTrue) {
                                continue;
                            }
                            // Defensive value/offset hardening. Hosts normally
                            // send normalized 0..1 at a valid in-block offset,
                            // but the bridge must not emit a malformed MIDI byte
                            // or an out-of-block sample offset.
                            const double norm = std::clamp(
                                static_cast<double>(value), 0.0, 1.0);
                            if (offset < 0 || offset >= data.numSamples) {
                                continue;  // outside [0, numSamples): drop
                            }
                            midi::MidiEvent me;
                            if (decoded.controller == detail::kVst3CtrlPitchBend) {
                                // Normalized 0..1 → 14-bit pitch bend
                                // (0..16383, center 8192).
                                const auto bend14 = static_cast<uint16_t>(
                                    std::clamp<int>(
                                        static_cast<int>(norm * 16383.0 + 0.5),
                                        0, 16383));
                                me = midi::MidiEvent::pitch_bend(channel, bend14);
                            } else if (decoded.controller ==
                                       detail::kVst3CtrlAfterTouch) {
                                // Channel pressure (status 0xD0): 7-bit value,
                                // no factory helper, so build the message
                                // directly.
                                const auto v7 = static_cast<uint8_t>(
                                    std::clamp<int>(
                                        static_cast<int>(norm * 127.0 + 0.5),
                                        0, 127));
                                me = midi::MidiEvent{
                                    choc::midi::ShortMessage(
                                        static_cast<uint8_t>(0xD0 | (channel & 0x0F)),
                                        v7, 0),
                                    0, 0.0};
                            } else {
                                // Standard CC 0..127.
                                const auto v7 = static_cast<uint8_t>(
                                    std::clamp<int>(
                                        static_cast<int>(norm * 127.0 + 0.5),
                                        0, 127));
                                me = midi::MidiEvent::cc(
                                    channel,
                                    static_cast<uint8_t>(decoded.controller),
                                    v7);
                            }
                            me.sample_offset = offset;
                            midi_in_.add(me);
                        }
                        continue;
                    }
                    for (int32 point = 0; point < point_count; ++point) {
                        ParamValue value;
                        int32 offset;
                        if (queue->getPoint(point, offset, value) != kResultTrue) {
                            continue;
                        }
                        const auto param_id = static_cast<state::ParamID>(id);
                        // VST3 hands normalized 0-1 values; the DSP cursor and
                        // the store both want the plain (denormalized) value.
                        // Denormalize once here so the shared dual-write pushes
                        // the same plain value to the sample-accurate queue and
                        // to the RT-safe store path (set_value_rt): identical to
                        // the prior push + set_normalized_rt pair, which
                        // denormalized the same value through the same range.
                        float plain_value = static_cast<float>(value);
                        if (const auto* info = store_.info(param_id)) {
                            plain_value = info->range.denormalize(plain_value);
                        }
                        boundary::apply_param_value(
                            param_events_, store_, param_id,
                            static_cast<int32_t>(offset), plain_value);
                    }
                }
            }
        }
    }
    param_events_.sort();
}

void PulpVst3Processor::process_wire_buffers(
    ProcessData& data, bool host_f64, bool native_f64, bool boundary_f64,
    int num_samples, int original_num_samples, int& in_channels,
    int& out_channels, int& sc_channels) {
    // Bus 0 routes to main input/output; bus 1 routes to
    // Processor::set_sidechain(). Additional input buses beyond index 1
    // are ignored because the Processor API exposes a single sidechain
    // slot.
    if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
        in_channels = data.inputs[0].numChannels;
        if (host_f64) {
            in_channels = (std::min)(
                in_channels, static_cast<int>(f64_input_scratch_.size()));
        }
        input_ptrs_.resize(in_channels);
        input64_ptrs_.resize(in_channels);
        for (int ch = 0; ch < in_channels; ++ch) {
            if (native_f64) {
                input64_ptrs_[ch] = data.inputs[0].channelBuffers64
                    ? data.inputs[0].channelBuffers64[ch]
                    : nullptr;
                input_ptrs_[ch] = nullptr;
            } else if (boundary_f64) {
                auto* dst = f64_input_scratch_[static_cast<std::size_t>(ch)].data();
                const double* src = data.inputs[0].channelBuffers64
                    ? data.inputs[0].channelBuffers64[ch]
                    : nullptr;
                input64_ptrs_[ch] = src;
                boundary::copy_f64_to_f32(src, dst,
                                          static_cast<std::uint32_t>(num_samples));
                input_ptrs_[ch] = src ? dst : nullptr;
            } else {
                input64_ptrs_[ch] = nullptr;
                input_ptrs_[ch] = data.inputs[0].channelBuffers32
                    ? data.inputs[0].channelBuffers32[ch]
                    : nullptr;
            }
        }
    }
    // A VST3 bus can report numChannels > 0 while inactive — in that
    // state channelBuffers32 and its entries can be null. Publishing a
    // non-null sidechain then would hand processors null pointers they
    // would happily dereference. Require an active channel-buffer array
    // with a non-null first pointer before accepting the sidechain; fall
    // back to nullptr otherwise.
    if (data.numInputs > 1 &&
        data.inputs[1].numChannels > 0 &&
        ((host_f64 && data.inputs[1].channelBuffers64 &&
          data.inputs[1].channelBuffers64[0]) ||
         (!host_f64 && data.inputs[1].channelBuffers32 &&
          data.inputs[1].channelBuffers32[0]))) {
        sc_channels = data.inputs[1].numChannels;
        if (host_f64) {
            sc_channels = (std::min)(
                sc_channels, static_cast<int>(f64_sidechain_scratch_.size()));
        }
        sidechain_ptrs_.resize(sc_channels);
        sidechain64_ptrs_.resize(sc_channels);
        for (int ch = 0; ch < sc_channels; ++ch) {
            if (native_f64) {
                sidechain64_ptrs_[ch] = data.inputs[1].channelBuffers64
                    ? data.inputs[1].channelBuffers64[ch]
                    : nullptr;
                sidechain_ptrs_[ch] = nullptr;
            } else if (boundary_f64) {
                auto* dst = f64_sidechain_scratch_[static_cast<std::size_t>(ch)].data();
                const double* src = data.inputs[1].channelBuffers64
                    ? data.inputs[1].channelBuffers64[ch]
                    : nullptr;
                sidechain64_ptrs_[ch] = src;
                boundary::copy_f64_to_f32(src, dst,
                                          static_cast<std::uint32_t>(num_samples));
                sidechain_ptrs_[ch] = src ? dst : nullptr;
            } else {
                sidechain64_ptrs_[ch] = nullptr;
                sidechain_ptrs_[ch] = data.inputs[1].channelBuffers32[ch];
            }
        }
    }

    if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
        out_channels = data.outputs[0].numChannels;
        if (host_f64) {
            out_channels = (std::min)(
                out_channels, static_cast<int>(f64_output_scratch_.size()));
        }
        output_ptrs_.resize(out_channels);
        output64_ptrs_.resize(out_channels);
        for (int ch = 0; ch < out_channels; ++ch) {
            if (native_f64) {
                output64_ptrs_[ch] = data.outputs[0].channelBuffers64 &&
                                     data.outputs[0].channelBuffers64[ch]
                    ? data.outputs[0].channelBuffers64[ch]
                    : nullptr;
                output_ptrs_[ch] = nullptr;
            } else if (boundary_f64) {
                output64_ptrs_[ch] = data.outputs[0].channelBuffers64 &&
                                     data.outputs[0].channelBuffers64[ch]
                    ? data.outputs[0].channelBuffers64[ch]
                    : nullptr;
                auto& scratch = f64_output_scratch_[static_cast<std::size_t>(ch)];
                // Zero only the frames this block renders: the boundary
                // writeback after process() copies exactly num_samples, so the
                // scratch tail past it is never read.
                std::fill_n(scratch.begin(), num_samples, 0.0f);
                output_ptrs_[ch] = data.outputs[0].channelBuffers64 &&
                                   data.outputs[0].channelBuffers64[ch]
                    ? scratch.data()
                    : nullptr;
            } else {
                output64_ptrs_[ch] = nullptr;
                output_ptrs_[ch] = data.outputs[0].channelBuffers32
                    ? data.outputs[0].channelBuffers32[ch]
                    : nullptr;
            }
        }
        // When the render was clamped above, the processor only writes the
        // first num_samples frames; zero the un-processable tail on every main
        // output channel so the host reads silence past the prepared max.
        if (original_num_samples > num_samples) {
            for (int ch = 0; ch < out_channels; ++ch) {
                if (host_f64) {
                    if (data.outputs[0].channelBuffers64 &&
                        data.outputs[0].channelBuffers64[ch]) {
                        boundary::zero_f64(
                            data.outputs[0].channelBuffers64[ch] + num_samples,
                            static_cast<std::uint32_t>(original_num_samples - num_samples));
                    }
                } else if (output_ptrs_[ch] != nullptr) {
                    std::memset(output_ptrs_[ch] + num_samples, 0,
                                sizeof(float) * (original_num_samples - num_samples));
                }
            }
        }
    }
    // Secondary (aux) output buses route to the Processor's richer
    // process(ProcessBuffers&) surface for multi-out instruments (drum
    // machines, multitimbral, stem renderers). Each routed aux bus is
    // pre-zeroed here first so a single-output Processor (the default
    // process(ProcessBuffers&) only writes the main output) leaves aux buses
    // silent rather than handing the host uninitialised memory, and a multi-out
    // Processor that writes only some aux channels still emits silence on the
    // channels it skipped. The aux views are assembled below from
    // aux_output_ptrs_ (pre-sized in setupProcessing()).
    for (int32 b = 1; b < data.numOutputs; ++b) {
        auto& bus = data.outputs[b];
        for (int32 ch = 0; ch < bus.numChannels; ++ch) {
            if (host_f64 && bus.channelBuffers64 && bus.channelBuffers64[ch]) {
                boundary::zero_f64(bus.channelBuffers64[ch],
                                   static_cast<std::uint32_t>(original_num_samples));
            } else if (!host_f64 && bus.channelBuffers32 && bus.channelBuffers32[ch]) {
                std::memset(bus.channelBuffers32[ch], 0,
                            sizeof(float) * original_num_samples);
            }
        }
    }
}

bool PulpVst3Processor::process_maybe_bypass(
    ProcessData& data, bool host_f64, int in_channels, int out_channels,
    int original_num_samples) {
    if (bypass_param_id_ != 0 &&
        store_.get_value(bypass_param_id_) >= 0.5f) {
        // All-or-nothing across the block: only delay when the boundary owns a
        // dry-delay line for every output channel. A channel count above the
        // shared boundary ceiling (kBoundaryMaxChannels) falls back to a
        // zero-delay copy for the whole block — degrading uniformly, never a
        // mix of delayed and undelayed channels.
        const bool delayed = bypass_.is_latency_compensated() &&
            out_channels <= static_cast<int>(bypass_.channel_capacity());
        for (int ch = 0; ch < out_channels; ++ch) {
            // A VST3 bus can report numChannels > 0 while individual
            // channelBuffers32[ch] entries are null; null-check before
            // writing. This guard mirrors the silence-accommodation path.
            if (host_f64) {
                auto* out64 = data.numOutputs > 0 && data.outputs[0].channelBuffers64
                    ? data.outputs[0].channelBuffers64[ch]
                    : nullptr;
                if (out64 == nullptr) continue;
                auto* in64 = data.numInputs > 0 &&
                                      ch < data.inputs[0].numChannels &&
                                      data.inputs[0].channelBuffers64
                    ? data.inputs[0].channelBuffers64[ch]
                    : nullptr;
                if (in64 != nullptr) {
                    if (delayed) {
                        for (int n = 0; n < original_num_samples; ++n) {
                            out64[n] = static_cast<double>(bypass_.process_sample(
                                static_cast<float>(in64[n]),
                                static_cast<std::size_t>(ch)));
                        }
                    } else {
                        std::memcpy(out64, in64,
                                    sizeof(double) * static_cast<std::size_t>(original_num_samples));
                    }
                } else {
                    boundary::zero_f64(out64,
                                       static_cast<std::uint32_t>(original_num_samples));
                }
                continue;
            }
            if (output_ptrs_[ch] == nullptr) continue;
            // Bypass is pure host-buffer passthrough (no processor scratch),
            // so it safely handles the full original block rather than the
            // clamped count. The dry-delay line is sized independently of the
            // block size (to the reported latency in setupProcessing()), so it
            // too processes the full `original_num_samples` — bypass stays a
            // true 1:1 host passthrough with no clamp tail to zero.
            if (ch < in_channels && input_ptrs_[ch] != nullptr) {
                if (delayed) {
                    // The plugin path is host-compensated by its reported
                    // latency, so the bypassed dry signal must be delayed by
                    // the same amount to stay sample-aligned with the host's
                    // plugin-delay-compensation. The delay line is fed only
                    // while bypassed: a one-time transient in the first
                    // reported-latency samples after engaging bypass is
                    // accepted (bypass toggling is a user action, not
                    // sample-critical) in exchange for a zero-cost
                    // non-bypassed path. Steady state emits input[n - latency].
                    const float* in = input_ptrs_[ch];
                    float* out = output_ptrs_[ch];
                    for (int n = 0; n < original_num_samples; ++n) {
                        out[n] = bypass_.process_sample(
                            in[n], static_cast<std::size_t>(ch));
                    }
                } else {
                    std::memcpy(output_ptrs_[ch], input_ptrs_[ch],
                                sizeof(float) * original_num_samples);
                }
            } else {
                std::memset(output_ptrs_[ch], 0,
                            sizeof(float) * original_num_samples);
            }
        }
        processor_->set_sidechain(nullptr);
        // Trigger reset is a single-exit invariant: a Reset/trigger param the
        // host raised this block must settle even though we short-circuited
        // before process(), or a panic/reset raised while bypassed would fire
        // late on the next non-bypassed block.
        store_.reset_triggers_rt();
        return true;
    }
    return false;
}

void PulpVst3Processor::process_decode_input_events(ProcessData& data) {
    // midi_in_ / midi_out_ were reset at the top of process() (the controller
    // decode that runs in the parameter-change loop appends to midi_in_, so
    // the buffers cannot be cleared here). Note and SysEx events append on top
    // of any controllers already decoded for this block.
    if (data.inputEvents) {
        int32 event_count = data.inputEvents->getEventCount();
        for (int32 i = 0; i < event_count; ++i) {
            Event evt;
            if (data.inputEvents->getEvent(i, evt) == kResultOk) {
                if (evt.type == Event::kNoteOnEvent) {
                    const auto channel =
                        static_cast<uint8_t>(evt.noteOn.channel);
                    const auto note =
                        static_cast<uint8_t>(evt.noteOn.pitch);
                    auto me = midi::MidiEvent::note_on(
                        channel, note,
                        static_cast<uint8_t>(evt.noteOn.velocity * 127.0f));
                    me.sample_offset = evt.sampleOffset;
                    midi_in_.add(me);
                    // Remember this note's (channel, note) under its noteId so a
                    // later kNoteExpressionValueEvent referencing the same noteId
                    // routes to the right MPE voice. No-op when MPE is off or the
                    // host supplied no noteId (-1). A full table drops the
                    // mapping (RT-safe) and bumps the observable drop counter.
                    if (mpe_enabled_) {
                        if (!note_id_map_insert(evt.noteOn.noteId, channel,
                                                note)) {
                            note_expression_drops_.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                } else if (evt.type == Event::kNoteOffEvent) {
                    const auto channel =
                        static_cast<uint8_t>(evt.noteOff.channel);
                    auto me = midi::MidiEvent::note_off(
                        channel,
                        static_cast<uint8_t>(evt.noteOff.pitch),
                        static_cast<uint8_t>(evt.noteOff.velocity * 127.0f));
                    me.sample_offset = evt.sampleOffset;
                    midi_in_.add(me);
                    if (mpe_enabled_) {
                        note_id_map_erase(evt.noteOff.noteId);
                    }
                } else if (evt.type == Event::kNoteExpressionValueEvent &&
                           mpe_enabled_) {
                    // Per-note expression. The event references the noteId of a
                    // live note-on; look up its (channel, note) and synthesize
                    // the channel-wide MIDI message the MpeVoiceTracker narrows
                    // back to the matching member-channel voice — the same
                    // sidecar contract the CLAP adapter uses
                    // (clap_adapter.cpp kNoteExpression path). Mapping:
                    //   kTuningTypeID     -> per-note pitch bend
                    //   kVolumeTypeID     -> per-note pressure (channel AT)
                    //   kBrightnessTypeID -> per-note timbre (CC74)
                    //
                    // SCOPING (by design, matches CLAP): VST3 note expression is
                    // noteId-targeted, but Pulp's per-note model IS MPE — one
                    // note per member channel — and the Processor has no
                    // noteId-targeted expression API. We therefore bridge to the
                    // MPE (channel-per-note) model via a channel-wide message.
                    // Routing is exact when each note lives on its own MPE member
                    // channel (the expressive-controller case MPE is built for);
                    // when multiple notes share one channel the expression
                    // collapses to channel-wide — identical to the CLAP path.
                    const auto& ne = evt.noteExpressionValue;
                    const NoteIdSlot* slot = note_id_map_find(ne.noteId);
                    if (slot == nullptr) {
                        // Expression for an unknown / already-released noteId
                        // (or one whose mapping was dropped on a full table).
                        // Nothing to route — count it as an observable drop.
                        note_expression_drops_.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    if (slot != nullptr) {
                        const uint8_t channel = slot->channel;
                        const double norm =
                            std::clamp(ne.value, 0.0, 1.0);
                        midi::MidiEvent me{};
                        bool emitted = false;
                        if (ne.typeId == kTuningTypeID) {
                            // VST3 tuning is normalized over ±120 st; decode
                            // it to semitones here, then let the shared mapping
                            // normalize to the MPE member-bend default so the
                            // tracker expands it back to the right per-note
                            // bend (mirrors CLAP).
                            me = mpe::pitch_bend_from_semitones(
                                channel, vst3_tuning_norm_to_semitones(norm));
                            emitted = true;
                        } else if (ne.typeId == kVolumeTypeID) {
                            // Loudness axis -> channel pressure (status 0xD0).
                            me = mpe::channel_pressure(channel, norm);
                            emitted = true;
                        } else if (ne.typeId == kBrightnessTypeID) {
                            // Timbre -> CC 74.
                            me = mpe::timbre_cc(channel, norm);
                            emitted = true;
                        }
                        // kPanTypeID (and any other declared/unknown type) has
                        // no MPE axis, so it is accepted by the host but not
                        // routed into the sidecar.
                        if (emitted) {
                            me.sample_offset = evt.sampleOffset;
                            midi_in_.add(me);
                        }
                    }
                } else if (evt.type == Event::kDataEvent
                           && evt.data.type == DataEvent::kMidiSysEx) {
                    // Route kData/kMidiSysEx payloads into MidiBuffer's
                    // variable-length sidecar. VST3 delivers the raw
                    // F0..F7 bytes in evt.data.bytes with length in
                    // evt.data.size.
                    if (evt.data.bytes && evt.data.size > 0) {
                        // Copy into the buffer's pooled payload storage rather
                        // than constructing a fresh std::vector per event — in
                        // realtime mode the copy reuses reserved capacity and
                        // does not allocate.
                        midi_in_.add_sysex_copy(
                            evt.data.bytes,
                            static_cast<std::size_t>(evt.data.size),
                            evt.sampleOffset,
                            0.0);
                    }
                }
            }
        }
    }

    // Controllers (decoded from parameter changes) and note/SysEx events are
    // appended from two independent sources, so the combined stream may not be
    // in sample order. Sort once so the Processor sees sample-accurate,
    // monotonically increasing offsets. std::sort over the reserved, bounded
    // events vector does not allocate.
    midi_in_.sort();
}

void PulpVst3Processor::process_publish_output_params(
    ProcessData& data, std::span<const state::ParamInfo> all_params) {
    if (data.outputParameterChanges) {
        // (1) Sample-accurate explicit output events. Global-sort by offset so a
        // parameter's multiple points land in ascending order in its VST3 queue
        // (VST3 requires per-queue ascending sample offsets).
        output_param_events_.sort();
        const int32 last_frame = data.numSamples > 0 ? data.numSamples - 1 : 0;
        for (const auto& ev : output_param_events_) {
            const std::size_t idx =
                boundary::find_param_index(all_params, ev.param_id);
            if (idx == boundary::kParamIndexNotFound) continue;  // unknown — drop
            int32 offset = ev.sample_offset;
            if (offset < 0) offset = 0;
            if (offset > last_frame) offset = last_frame;  // clamp into the block
            auto*& queue = output_param_queue_cache_[idx];
            if (!queue) {
                int32 qi = 0;
                queue = data.outputParameterChanges->addParameterData(
                    static_cast<ParamID>(ev.param_id), qi);
            }
            if (queue) {
                int32 pt_index = 0;
                float normalized = all_params[idx].range.normalize(ev.value);
                queue->addPoint(offset, static_cast<ParamValue>(normalized), pt_index);
            }
            output_param_has_event_[idx] = 1;
        }
        // (2) Snapshot-diff fallback, skipping params already reported in (1).
        for (std::size_t i = 0; i < all_params.size(); ++i) {
            if (output_param_has_event_[i]) {
                // Keep the VST3 parameter system synced to the final value even
                // though we reported the sample-accurate points above.
                setParamNormalized(static_cast<ParamID>(all_params[i].id),
                                   static_cast<ParamValue>(store_.get_normalized(all_params[i].id)));
                continue;
            }
            float current = store_.get_value(all_params[i].id);
            if (boundary::changed_since_snapshot(current, param_snapshot_[i])) {
                int32 index = 0;
                auto* queue = data.outputParameterChanges->addParameterData(
                    static_cast<ParamID>(all_params[i].id), index);
                if (queue) {
                    int32 pt_index = 0;
                    float normalized = store_.get_normalized(all_params[i].id);
                    queue->addPoint(0, static_cast<ParamValue>(normalized), pt_index);
                }
                // Sync to VST3 parameter system too
                setParamNormalized(static_cast<ParamID>(all_params[i].id),
                                   static_cast<ParamValue>(store_.get_normalized(all_params[i].id)));
            }
        }
    }
}

void PulpVst3Processor::process_publish_restart_flags() {
    {
        int32 flags = 0;
        if (processor_->consume_latency_changed_flag()) flags |= kLatencyChanged;
        if (processor_->consume_tail_changed_flag())    flags |= kReloadComponent;
        // note_pending() is the only thing the audio thread does here: an
        // atomic OR plus an exchange, no allocation and no lock. The matching
        // restartComponent(flags) host callback is delivered later by
        // drain_pending_restart() on the main thread (driven by the main-thread
        // host entrypoints getLatencySamples / getTailSamples / setActive,
        // which a host re-queries after a latency/tail change). Calling
        // call_async here is deliberately avoided — it allocates and locks.
        restart_publisher_.note_pending(flags);
    }
}

tresult PLUGIN_API PulpVst3Processor::process(ProcessData& data) {
    if (!processor_) return kInternalError;
    const bool host_f64 = data.symbolicSampleSize == kSample64 ||
                          (data.symbolicSampleSize != kSample32 &&
                           selected_sample_size_ == kSample64);
    const bool native_f64 = host_f64 && native_f64_enabled_;
    const bool boundary_f64 = host_f64 && !native_f64;

    // Flush denormals to zero for the whole audio-callback body so quiet tails
    // in recursive filter/reverb/feedback state can't stall the host's audio
    // thread, then restore its prior FP mode on scope exit. See
    // docs/guides/dsp-threading.md "Numeric mode".
    pulp::signal::ScopedFlushDenormals flush_denormals;

    // Phase (a): clear the reused param/MIDI buffers, decode host parameter
    // changes into the StateStore boundary, and divert IMidiMapping-registered
    // controller changes into midi_in_.
    process_decode_input_parameters(data);

    // Build buffer views
    int32 num_samples = data.numSamples;
    if (num_samples == 0) return kResultOk;

    // Max-block contract guard (defensive; well-behaved hosts never exceed the
    // advertised max). The Processor and all its scratch buffers were sized in
    // setupProcessing() to max_block_size_ (setup.maxSamplesPerBlock). A render
    // larger than that would overrun them and corrupt DSP state. Per the shared
    // max-block contract (max_block_contract.hpp) — matching CLAP/AU-v3/AAX —
    // clamp the processed region to the prepared max and zero the un-processable
    // tail [max_block_size_, original) on every main output channel so it reads
    // back as clean silence rather than garbage.
    const int32 original_num_samples = num_samples;
    num_samples = clamp_block_to_prepared_max(num_samples, max_block_size_);

    // Phase (d, f32 half): wire the host's main input, sidechain, and main
    // output channel pointers into the reused ptr vectors and pre-zero routed
    // aux output buses.
    int in_channels = 0;
    int out_channels = 0;
    int sc_channels = 0;
    process_wire_buffers(data, host_f64, native_f64, boundary_f64, num_samples,
                         original_num_samples, in_channels, out_channels,
                         sc_channels);

    // If the host accepted an arrangement the processor does not natively
    // support, the processor was prepare()'d for native_in_/native_out_
    // channels only. Hand the processor exactly those counts, and
    // zero-fill all host main-bus output channels first so channels the
    // processor does not write emit silence instead of uninitialised
    // memory.
    int proc_in = in_channels;
    int proc_out = out_channels;
    if (silence_unsupported_active_) {
        for (int ch = 0; ch < out_channels; ++ch) {
            if (host_f64 && output64_ptrs_[ch] != nullptr) {
                boundary::zero_f64(output64_ptrs_[ch],
                                   static_cast<std::uint32_t>(num_samples));
            } else if (!host_f64 && output_ptrs_[ch] != nullptr) {
                std::memset(output_ptrs_[ch], 0, sizeof(float) * num_samples);
            }
        }
        proc_in  = (in_channels  < native_in_)  ? in_channels  : native_in_;
        proc_out = (out_channels < native_out_) ? out_channels : native_out_;
    }

    audio::BufferView<const float> input_view(
        const_cast<const float* const*>(input_ptrs_.data()),
        proc_in, num_samples);
    audio::BufferView<float> output_view(
        output_ptrs_.data(), proc_out, num_samples);
    audio::BufferView<const float> sidechain_view(
        const_cast<const float* const*>(sidechain_ptrs_.data()),
        sc_channels, num_samples);
    std::array<ProcessBusBufferView<const float>, 2> input_buses{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main,
                     proc_in, false, data.numInputs > 0},
            .buffer = input_view,
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain,
                     sc_channels, true, sc_channels > 0},
            .buffer = sidechain_view,
        },
    }};
    // Build one ProcessBusBufferView per routed output bus. Index 0 is the
    // main output; each subsequent entry wires the host's aux output channel
    // pointers into aux_output_ptrs_ (pre-sized in setupProcessing()). Host
    // output buses beyond the declared/pre-sized set are zero-filled (above)
    // but not routed — the same safe fallback as before.
    static constexpr std::size_t kMaxOutputBuses = BusBufferSet::kMaxBuses;
    std::array<ProcessBusBufferView<float>, kMaxOutputBuses> output_buses{};
    output_buses[0] = {
        .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                 proc_out, false, data.numOutputs > 0},
        .buffer = output_view,
    };
    std::size_t routed_output_buses = 1;
    const std::size_t max_routed =
        (std::min)(static_cast<std::size_t>(data.numOutputs),
                   (std::min)(aux_output_ptrs_.size() + 1, kMaxOutputBuses));
    for (std::size_t b = 1; b < max_routed; ++b) {
        auto& bus = data.outputs[b];
        auto& ptrs = aux_output_ptrs_[b - 1];
        int aux_channels =
            (std::min)(static_cast<int>(bus.numChannels),
                       static_cast<int>(ptrs.size()));
        bool active = bus.channelBuffers32 != nullptr;
        if (host_f64) active = bus.channelBuffers64 != nullptr;
        if (active) {
            for (int ch = 0; ch < aux_channels; ++ch) {
                if (native_f64) {
                    auto& ptrs64 = aux_output64_ptrs_[b - 1];
                    ptrs64[static_cast<std::size_t>(ch)] =
                        bus.channelBuffers64 && bus.channelBuffers64[ch]
                            ? bus.channelBuffers64[ch]
                            : nullptr;
                    ptrs[static_cast<std::size_t>(ch)] = nullptr;
                } else if (boundary_f64) {
                    auto& ptrs64 = aux_output64_ptrs_[b - 1];
                    ptrs64[static_cast<std::size_t>(ch)] =
                        bus.channelBuffers64 && bus.channelBuffers64[ch]
                            ? bus.channelBuffers64[ch]
                            : nullptr;
                    auto& scratch =
                        f64_aux_output_scratch_[b - 1][static_cast<std::size_t>(ch)];
                    // Same bound as the main-output scratch above: the aux
                    // writeback copies exactly num_samples.
                    std::fill_n(scratch.begin(), num_samples, 0.0f);
                    ptrs[static_cast<std::size_t>(ch)] =
                        bus.channelBuffers64 && bus.channelBuffers64[ch]
                            ? scratch.data()
                            : nullptr;
                } else {
                    aux_output64_ptrs_[b - 1][static_cast<std::size_t>(ch)] = nullptr;
                    ptrs[static_cast<std::size_t>(ch)] = bus.channelBuffers32[ch];
                }
                const bool channel_valid = native_f64
                    ? aux_output64_ptrs_[b - 1][static_cast<std::size_t>(ch)] != nullptr
                    : ptrs[static_cast<std::size_t>(ch)] != nullptr;
                if (!channel_valid) {
                    // A null per-channel pointer demotes the whole bus to
                    // inactive so the Processor sees an empty bus rather than a
                    // half-valid BufferView.
                    active = false;
                    break;
                }
            }
        }
        if (!active) aux_channels = 0;
        // declared_channels reports the descriptor's declared layout (captured
        // in setupProcessing); buffer.num_channels() carries the actual routed
        // count. Keeping these distinct lets matches_declared_layout() detect a
        // host-vs-declared channel-count mismatch instead of being tautological.
        output_buses[routed_output_buses++] = {
            .info = {"Aux Out", b, BusDirection::Output, BusRole::Aux,
                     declared_aux_channels_[b - 1], true, active},
            .buffer = audio::BufferView<float>(ptrs.data(), aux_channels,
                                               num_samples),
        };
    }
    ProcessBuffers process_buffers{
        .inputs = ProcessBusBufferSet<const float>(input_buses),
        .outputs = ProcessBusBufferSet<float>(
            std::span(output_buses.data(), routed_output_buses)),
    };
    audio::BufferView<const double> input64_view(
        input64_ptrs_.data(), proc_in, num_samples);
    audio::BufferView<double> output64_view(
        output64_ptrs_.data(), proc_out, num_samples);
    audio::BufferView<const double> sidechain64_view(
        sidechain64_ptrs_.data(), sc_channels, num_samples);
    std::array<ProcessBusBufferView<const double>, 2> input64_buses{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main,
                     proc_in, false, data.numInputs > 0},
            .buffer = input64_view,
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain,
                     sc_channels, true, sc_channels > 0},
            .buffer = sidechain64_view,
        },
    }};
    std::array<ProcessBusBufferView<double>, kMaxOutputBuses> output64_buses{};
    output64_buses[0] = {
        .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                 proc_out, false, data.numOutputs > 0},
        .buffer = output64_view,
    };
    for (std::size_t b = 1; b < routed_output_buses; ++b) {
        if (b >= static_cast<std::size_t>(data.numOutputs)) break;
        auto& bus = data.outputs[b];
        auto& ptrs = aux_output64_ptrs_[b - 1];
        int aux_channels =
            (std::min)(static_cast<int>(bus.numChannels),
                       static_cast<int>(ptrs.size()));
        bool active = host_f64 && bus.channelBuffers64 != nullptr;
        if (active) {
            for (int ch = 0; ch < aux_channels; ++ch) {
                ptrs[static_cast<std::size_t>(ch)] =
                    bus.channelBuffers64[ch];
                if (!ptrs[static_cast<std::size_t>(ch)]) {
                    active = false;
                    break;
                }
            }
        }
        if (!active) aux_channels = 0;
        output64_buses[b] = {
            .info = {"Aux Out", b, BusDirection::Output, BusRole::Aux,
                     declared_aux_channels_[b - 1], true, active},
            .buffer = audio::BufferView<double>(ptrs.data(), aux_channels,
                                                num_samples),
        };
    }
    ProcessBuffers64 process_buffers64{
        .inputs = ProcessBusBufferSet<const double>(input64_buses),
        .outputs = ProcessBusBufferSet<double>(
            std::span(output64_buses.data(), routed_output_buses)),
    };

    // Phase (c): VST3 processBlockBypassed short-circuit. When the bypass
    // parameter is engaged the adapter does the pass-through itself and returns
    // before process(), leaving MIDI output and silenceFlags untouched.
    if (process_maybe_bypass(data, host_f64, in_channels, out_channels,
                             original_num_samples)) {
        return kResultOk;
    }

    // Phase (b): decode host input events (notes, per-note expression, SysEx)
    // onto midi_in_ and sort the combined controller+note stream.
    process_decode_input_events(data);

    auto ctx = build_process_context(data, processSetup, num_samples,
                                     playhead_prev_);

    // Snapshot parameter values before processing so we can detect
    // plugin-side changes and report them to the host for automation recording
    auto all_params = store_.all_params();
    boundary::snapshot_param_values(store_, all_params, param_snapshot_);
    output_param_has_event_.assign(all_params.size(), 0);
    output_param_queue_cache_.assign(all_params.size(), nullptr);
    processor_->set_param_events(&param_events_);
    output_param_events_.clear();
    processor_->set_output_param_events(&output_param_events_);

    // MPE sidecar: run the (sorted) inbound MIDI — note on/off plus the
    // channel-wide messages synthesized from kNoteExpressionValueEvent above —
    // through the voice tracker, then hand the resulting per-note expression
    // buffer to the processor for the duration of this process() call. Events
    // stay sample-ordered because midi_in_ was just sorted. Mirrors the CLAP
    // adapter's set_mpe_input contract; clears per block, allocation-free.
    if (mpe_enabled_) {
        mpe_buffer_.clear();
        for (const auto& ev : midi_in_) {
            mpe_current_sample_offset_ = ev.sample_offset;
            mpe_tracker_.process(ev);
        }
        processor_->set_mpe_input(&mpe_buffer_);
    } else {
        processor_->set_mpe_input(nullptr);
    }

    // Wrap the plugin call in a ScopedNoAlloc so debug hooks can flag a
    // plugin that allocates on the audio thread.
    {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;
        if (native_f64) {
            processor_->process_f64(process_buffers64, midi_in_, midi_out_, ctx);
        } else {
            processor_->process(process_buffers, midi_in_, midi_out_, ctx);
        }
    }

    // VST3 puts the obligation to declare output silence on the plugin, and a
    // host is free to propagate its input silenceFlags into the output buffers
    // it hands us. A Processor may synthesize output from silence — a
    // generator, an oscillator, a DC/control-voltage source, a reverb tail — so
    // any bus we just rendered into must be reported as carrying signal.
    // Leaving a stale flag set hands the host a full buffer labeled silent.
    // The bypass path returns before this point and leaves the flags alone:
    // there the plugin is a wire, so upstream silence stays silence.
    for (int32 b = 0; b < data.numOutputs; ++b) {
        data.outputs[b].silenceFlags = 0;
    }

    if (boundary_f64) {
        if (data.numOutputs > 0 && data.outputs[0].channelBuffers64) {
            for (int ch = 0; ch < out_channels; ++ch) {
                boundary::copy_f32_to_f64(
                    output_ptrs_[ch],
                    data.outputs[0].channelBuffers64[ch],
                    static_cast<std::uint32_t>(num_samples));
            }
        }
        for (std::size_t b = 1; b < routed_output_buses; ++b) {
            if (b >= static_cast<std::size_t>(data.numOutputs)) break;
            auto& bus = data.outputs[b];
            if (!bus.channelBuffers64) continue;
            auto& ptrs = aux_output_ptrs_[b - 1];
            // Bounded by the routed view, not the host channel count: only the
            // view's channels have scratch wired (and zeroed) this block. A bus
            // demoted by a null mid-bus channel pointer has a zero-channel view,
            // so its stale scratch pointers are never dereferenced and the host
            // keeps the direct channelBuffers64 pre-zero from block start.
            const int aux_channels =
                static_cast<int>(output_buses[b].buffer.num_channels());
            for (int ch = 0; ch < aux_channels; ++ch) {
                boundary::copy_f32_to_f64(ptrs[static_cast<std::size_t>(ch)],
                                          bus.channelBuffers64[ch],
                                          static_cast<std::uint32_t>(num_samples));
            }
        }
    }

    // Return trigger / momentary params (panic, reset, tap) to their default
    // now that the Processor has observed this block. Done before the
    // output-change scan below so the host records the auto-reset as automation.
    store_.reset_triggers_rt();

    // Phase (f): publish output parameter changes so the host can record
    // automation the plugin made during process().
    process_publish_output_params(data, all_params);

    // Phase (h): publish the processor's pending latency/tail restart flags.
    process_publish_restart_flags();

    write_midi_output(data, midi_out_);

    return kResultOk;
}

tresult PLUGIN_API PulpVst3Processor::getState(IBStream* stream) {
    // getState runs on the main thread — another opportunity to flush a
    // pending restart (alongside the paced poll and the latency/tail queries).
    drain_pending_restart();
    if (!processor_) return kResultFalse;
    auto data = plugin_state_io::serialize(store_, *processor_);
    int32 written;
    return stream->write(data.data(), static_cast<int32>(data.size()), &written);
}

tresult PLUGIN_API PulpVst3Processor::setState(IBStream* stream) {
    // Read all data from stream
    std::vector<uint8_t> data;
    char buf[4096];
    int32 read_count;
    while (stream->read(buf, sizeof(buf), &read_count) == kResultOk && read_count > 0) {
        data.insert(data.end(), buf, buf + read_count);
    }
    if (!processor_) return kResultFalse;
    if (!plugin_state_io::deserialize(data, store_, *processor_)) return kResultFalse;

    // Sync restored values back to VST3 parameter system
    for (const auto& param : store_.all_params()) {
        float normalized = store_.get_normalized(param.id);
        setParamNormalized(static_cast<ParamID>(param.id),
                           static_cast<ParamValue>(normalized));
    }
    return kResultOk;
}

} // namespace pulp::format::vst3
