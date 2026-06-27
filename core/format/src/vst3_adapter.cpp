// VST3 Adapter implementation
// Uses SingleComponentEffect pattern: combined processor + controller
// Parameters are registered with the VST3 parameter system and synced
// with the Pulp StateStore during processing

#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/detail/vst3_frame_rate.hpp>
#include <pulp/format/plugin_state_io.hpp>
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
#include <array>
#include <cstring>

namespace pulp::format::vst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

PulpVst3Processor::PulpVst3Processor(ProcessorFactory factory)
    : factory_(factory)
{
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

    // Add event buses for MIDI
    if (desc.accepts_midi) {
        addEventInput(STR16("MIDI In"), 1);
    }
    if (desc.produces_midi) {
        addEventOutput(STR16("MIDI Out"), 1);
    }

    // Register Pulp parameters with the VST3 parameter system
    for (const auto& param : store_.all_params()) {
        int32 flags = ParameterInfo::kCanAutomate;

        // Boolean parameters (step == 1, range 0-1) get step count 1
        int32 step_count = 0;
        if (param.range.step >= 1.0f && param.range.min == 0.0f && param.range.max == 1.0f) {
            step_count = 1;
            if (param.name == "Bypass") {
                flags |= ParameterInfo::kIsBypass;
                // Remember which ParamID carries kIsBypass so process()
                // can short-circuit to pass-through audio without
                // invoking the Processor when the host sets it.
                bypass_param_id_ = param.id;
            }
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

    runtime::log_info("VST3: initialized '{}' with {} parameters",
                      desc.name, store_.param_count());

    // Opt this plugin instance into the process-wide MainThreadDispatcher
    // backend. On macOS this installs or refcounts a Cocoa backend
    // posting to `dispatch_get_main_queue`, so host callbacks and view
    // code can post work to the DAW's main thread.
    main_thread_token_ = pulp::events::register_plugin_backend();

    return kResultOk;
}

tresult PLUGIN_API PulpVst3Processor::terminate() {
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

    // Cache what the processor's buffers are prepared for; the silence
    // accommodation clamps process() views to these counts.
    native_in_ = in_ch;
    native_out_ = out_ch;

    // Pre-allocate buffer pointer arrays for real-time safety
    input_ptrs_.resize(ctx.input_channels);
    output_ptrs_.resize(ctx.output_channels);

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

    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API PulpVst3Processor::setActive(TBool state) {
    if (!state && processor_) {
        processor_->release();
    }
    return SingleComponentEffect::setActive(state);
}

uint32 PLUGIN_API PulpVst3Processor::getLatencySamples() {
    if (!processor_) return 0;
    // VST3 reports latency as unsigned, so a negative latency_samples()
    // would wrap to a huge value without the host-quirk clamp. When the
    // quirk is filtered out, the raw value passes through.
    return static_cast<uint32>(
        reported_latency_samples(processor_->latency_samples(), quirks_));
}

uint32 PLUGIN_API PulpVst3Processor::getTailSamples() {
    if (!processor_) return 0;
    auto tail = processor_->descriptor().tail_samples;
    if (tail < 0) return Steinberg::Vst::kInfiniteTail;
    return static_cast<uint32>(tail);
}

tresult PLUGIN_API PulpVst3Processor::process(ProcessData& data) {
    if (!processor_) return kInternalError;

    // Flush denormals to zero for the whole audio-callback body so quiet tails
    // in recursive filter/reverb/feedback state can't stall the host's audio
    // thread, then restore its prior FP mode on scope exit. See
    // docs/guides/dsp-threading.md "Numeric mode".
    pulp::signal::ScopedFlushDenormals flush_denormals;

    param_events_.clear();

    // Read parameter changes from host and sync to Pulp StateStore
    if (data.inputParameterChanges) {
        int32 count = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            auto* queue = data.inputParameterChanges->getParameterData(i);
            if (queue) {
                ParamID id = queue->getParameterId();
                int32 point_count = queue->getPointCount();
                if (point_count > 0) {
                    for (int32 point = 0; point < point_count; ++point) {
                        ParamValue value;
                        int32 offset;
                        if (queue->getPoint(point, offset, value) != kResultTrue) {
                            continue;
                        }
                        const auto param_id = static_cast<state::ParamID>(id);
                        float plain_value = static_cast<float>(value);
                        if (const auto* info = store_.info(param_id)) {
                            plain_value = info->range.denormalize(plain_value);
                        }
                        param_events_.push({
                            param_id,
                            static_cast<int32_t>(offset),
                            plain_value,
                        });
                        // VST3 uses normalized 0-1 values — sync to Pulp store
                        // via the RT-safe path so any Main listeners are
                        // deferred to pump_listeners() instead of allocating
                        // a dispatch lambda on the audio thread.
                        store_.set_normalized_rt(param_id, static_cast<float>(value));
                    }
                }
            }
        }
    }
    param_events_.sort();

    // Build buffer views
    int32 num_samples = data.numSamples;
    if (num_samples == 0) return kResultOk;

    // Bus 0 routes to main input/output; bus 1 routes to
    // Processor::set_sidechain(). Additional input buses beyond index 1
    // are ignored because the Processor API exposes a single sidechain
    // slot.
    int in_channels = 0;
    int out_channels = 0;
    int sc_channels = 0;

    if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
        in_channels = data.inputs[0].numChannels;
        input_ptrs_.resize(in_channels);
        for (int ch = 0; ch < in_channels; ++ch) {
            input_ptrs_[ch] = data.inputs[0].channelBuffers32[ch];
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
        data.inputs[1].channelBuffers32 &&
        data.inputs[1].channelBuffers32[0]) {
        sc_channels = data.inputs[1].numChannels;
        sidechain_ptrs_.resize(sc_channels);
        for (int ch = 0; ch < sc_channels; ++ch) {
            sidechain_ptrs_[ch] = data.inputs[1].channelBuffers32[ch];
        }
    }

    if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
        out_channels = data.outputs[0].numChannels;
        output_ptrs_.resize(out_channels);
        for (int ch = 0; ch < out_channels; ++ch) {
            output_ptrs_[ch] = data.outputs[0].channelBuffers32[ch];
        }
    }
    // Secondary output buses are zero-filled so hosts do not read
    // uninitialised memory on multi-out instruments. The Processor API
    // currently exposes only the main output bus.
    for (int32 b = 1; b < data.numOutputs; ++b) {
        auto& bus = data.outputs[b];
        for (int32 ch = 0; ch < bus.numChannels; ++ch) {
            if (bus.channelBuffers32 && bus.channelBuffers32[ch]) {
                std::memset(bus.channelBuffers32[ch], 0,
                            sizeof(float) * num_samples);
            }
        }
    }

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
            if (output_ptrs_[ch] != nullptr) {
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
    std::array<ProcessBusBufferView<float>, 1> output_buses{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                     proc_out, false, data.numOutputs > 0},
            .buffer = output_view,
        },
    }};
    ProcessBuffers process_buffers{
        .inputs = ProcessBusBufferSet<const float>(input_buses),
        .outputs = ProcessBusBufferSet<float>(output_buses),
    };

    // VST3 `processBlockBypassed` behaviour. When the current bypass
    // value is >= 0.5, the adapter does the pass-through itself instead
    // of asking the Processor. Effects copy main input to main output;
    // instruments and generators zero-fill. MIDI output stays empty so a
    // bypassed MIDI FX does not leak notes into the host bus.
    if (bypass_param_id_ != 0 &&
        store_.get_value(bypass_param_id_) >= 0.5f) {
        for (int ch = 0; ch < out_channels; ++ch) {
            // A VST3 bus can report numChannels > 0 while individual
            // channelBuffers32[ch] entries are null; null-check before
            // writing. This guard mirrors the silence-accommodation path.
            if (output_ptrs_[ch] == nullptr) continue;
            if (ch < in_channels && input_ptrs_[ch] != nullptr) {
                std::memcpy(output_ptrs_[ch], input_ptrs_[ch],
                            sizeof(float) * num_samples);
            } else {
                std::memset(output_ptrs_[ch], 0,
                            sizeof(float) * num_samples);
            }
        }
        processor_->set_sidechain(nullptr);
        return kResultOk;
    }

    // Reuse the reserved, realtime-capacity-limited per-block MIDI buffers so
    // translating VST3 events never allocates on the audio thread (see
    // setupProcessing()). Both the short-event store AND the SysEx sidecar must
    // be reset every block: clear() empties only events_, so clear_sysex() is
    // required as well — otherwise a SysEx payload from one block would leak
    // into later blocks. clear_sysex() recycles pooled payloads, so it stays
    // allocation-free.
    midi_in_.clear();
    midi_in_.clear_sysex();
    midi_out_.clear();
    midi_out_.clear_sysex();
    if (data.inputEvents) {
        int32 event_count = data.inputEvents->getEventCount();
        for (int32 i = 0; i < event_count; ++i) {
            Event evt;
            if (data.inputEvents->getEvent(i, evt) == kResultOk) {
                if (evt.type == Event::kNoteOnEvent) {
                    auto me = midi::MidiEvent::note_on(
                        static_cast<uint8_t>(evt.noteOn.channel),
                        static_cast<uint8_t>(evt.noteOn.pitch),
                        static_cast<uint8_t>(evt.noteOn.velocity * 127.0f));
                    me.sample_offset = evt.sampleOffset;
                    midi_in_.add(me);
                } else if (evt.type == Event::kNoteOffEvent) {
                    auto me = midi::MidiEvent::note_off(
                        static_cast<uint8_t>(evt.noteOff.channel),
                        static_cast<uint8_t>(evt.noteOff.pitch),
                        static_cast<uint8_t>(evt.noteOff.velocity * 127.0f));
                    me.sample_offset = evt.sampleOffset;
                    midi_in_.add(me);
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

    // Build process context
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = processSetup.sampleRate;
    ctx.num_samples = num_samples;
    ctx.process_mode = processSetup.processMode == Steinberg::Vst::kOffline
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

    // Diff against the previous block to populate the transport change
    // flags. Stateful; updates `playhead_prev_` in place.
    pulp::format::detail::compute_playhead_changes(ctx, playhead_prev_);

    // Snapshot parameter values before processing so we can detect
    // plugin-side changes and report them to the host for automation recording
    auto all_params = store_.all_params();
    param_snapshot_.resize(all_params.size());
    for (std::size_t i = 0; i < all_params.size(); ++i) {
        param_snapshot_[i] = store_.get_value(all_params[i].id);
    }
    processor_->set_param_events(&param_events_);

    // Wrap the plugin call in a ScopedNoAlloc so debug hooks can flag a
    // plugin that allocates on the audio thread.
    {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;
        processor_->process(process_buffers, midi_in_, midi_out_, ctx);
    }

    // Write parameter output changes — lets the host record automation
    // from parameter changes made by the plugin during process()
    if (data.outputParameterChanges) {
        for (std::size_t i = 0; i < all_params.size(); ++i) {
            float current = store_.get_value(all_params[i].id);
            if (std::memcmp(&current, &param_snapshot_[i], sizeof(float)) != 0) {
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

    // Publish latency / tail changes the processor flagged during
    // process(). VST3's IComponentHandler::restartComponent is documented
    // as safe to call from the host's audio callback; the handler is
    // expected to queue it for main-thread delivery. We still drain the
    // atomic flag so process() never has to take a lock or allocate.
    if (componentHandler) {
        int32 flags = 0;
        if (processor_->consume_latency_changed_flag()) flags |= kLatencyChanged;
        if (processor_->consume_tail_changed_flag())    flags |= kReloadComponent;
        if (flags != 0) {
            componentHandler->restartComponent(flags);
        }
    }

    // Write MIDI output
    if (data.outputEvents && !midi_out_.empty()) {
        for (const auto& me : midi_out_) {
            Event evt{};
            evt.sampleOffset = me.sample_offset;
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

    return kResultOk;
}

tresult PLUGIN_API PulpVst3Processor::getState(IBStream* stream) {
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
