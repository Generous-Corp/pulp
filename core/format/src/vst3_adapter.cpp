// VST3 Adapter implementation
// Uses SingleComponentEffect pattern: combined processor + controller
// Parameters are registered with the VST3 parameter system and synced
// with the Pulp StateStore during processing

#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/vst3_plug_view.hpp>
#include <pulp/runtime/log.hpp>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/base/ustring.h>
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

    // Create the Pulp processor
    processor_ = factory_();
    if (!processor_) return kInternalError;

    auto desc = processor_->descriptor();
    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);

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
    return kResultOk;
}

tresult PLUGIN_API PulpVst3Processor::terminate() {
    processor_.reset();
    return SingleComponentEffect::terminate();
}

IPlugView* PLUGIN_API PulpVst3Processor::createView(FIDString name) {
    // VST3 hosts call createView("editor") to get the plugin's GUI
    if (name && strcmp(name, ViewType::kEditor) == 0) {
#ifdef PULP_VST3_GUI
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
    // Accept stereo or mono
    if (numIns >= 1 && numOuts >= 1) {
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }
    return kResultFalse;
}

tresult PLUGIN_API PulpVst3Processor::setupProcessing(ProcessSetup& setup) {
    if (!processor_) return kInternalError;

    PrepareContext ctx;
    ctx.sample_rate = setup.sampleRate;
    ctx.max_buffer_size = setup.maxSamplesPerBlock;
    ctx.input_channels = 2;  // TODO: query from bus arrangement
    ctx.output_channels = 2;

    processor_->prepare(ctx);

    // Pre-allocate buffer pointer arrays for real-time safety
    input_ptrs_.resize(ctx.input_channels);
    output_ptrs_.resize(ctx.output_channels);

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
    return static_cast<uint32>(processor_->latency_samples());
}

uint32 PLUGIN_API PulpVst3Processor::getTailSamples() {
    if (!processor_) return 0;
    auto tail = processor_->descriptor().tail_samples;
    if (tail < 0) return Steinberg::Vst::kInfiniteTail;
    return static_cast<uint32>(tail);
}

tresult PLUGIN_API PulpVst3Processor::process(ProcessData& data) {
    if (!processor_) return kInternalError;

    // Read parameter changes from host and sync to Pulp StateStore
    if (data.inputParameterChanges) {
        int32 count = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            auto* queue = data.inputParameterChanges->getParameterData(i);
            if (queue) {
                ParamID id = queue->getParameterId();
                int32 point_count = queue->getPointCount();
                if (point_count > 0) {
                    ParamValue value;
                    int32 offset;
                    queue->getPoint(point_count - 1, offset, value);
                    // VST3 uses normalized 0-1 values — sync to Pulp store
                    store_.set_normalized(static_cast<state::ParamID>(id),
                                         static_cast<float>(value));
                }
            }
        }
    }

    // Build buffer views
    int32 num_samples = data.numSamples;
    if (num_samples == 0) return kResultOk;

    int in_channels = 0;
    int out_channels = 0;

    if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
        in_channels = data.inputs[0].numChannels;
        input_ptrs_.resize(in_channels);
        for (int ch = 0; ch < in_channels; ++ch) {
            input_ptrs_[ch] = data.inputs[0].channelBuffers32[ch];
        }
    }

    if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
        out_channels = data.outputs[0].numChannels;
        output_ptrs_.resize(out_channels);
        for (int ch = 0; ch < out_channels; ++ch) {
            output_ptrs_[ch] = data.outputs[0].channelBuffers32[ch];
        }
    }

    audio::BufferView<const float> input_view(
        const_cast<const float* const*>(input_ptrs_.data()),
        in_channels, num_samples);
    audio::BufferView<float> output_view(
        output_ptrs_.data(), out_channels, num_samples);

    // Build MIDI buffers from VST3 events
    midi::MidiBuffer midi_in, midi_out;
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
                    midi_in.add(me);
                } else if (evt.type == Event::kNoteOffEvent) {
                    auto me = midi::MidiEvent::note_off(
                        static_cast<uint8_t>(evt.noteOff.channel),
                        static_cast<uint8_t>(evt.noteOff.pitch),
                        static_cast<uint8_t>(evt.noteOff.velocity * 127.0f));
                    me.sample_offset = evt.sampleOffset;
                    midi_in.add(me);
                }
            }
        }
    }

    // Build process context
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = processSetup.sampleRate;
    ctx.num_samples = num_samples;
    if (data.processContext) {
        ctx.is_playing = (data.processContext->state & Steinberg::Vst::ProcessContext::kPlaying) != 0;
        ctx.tempo_bpm = data.processContext->tempo;
        ctx.position_samples = data.processContext->projectTimeSamples;
        if (data.processContext->state & Steinberg::Vst::ProcessContext::kTimeSigValid) {
            ctx.time_sig_numerator = data.processContext->timeSigNumerator;
            ctx.time_sig_denominator = data.processContext->timeSigDenominator;
        }
    }

    // Snapshot parameter values before processing so we can detect
    // plugin-side changes and report them to the host for automation recording
    auto all_params = store_.all_params();
    param_snapshot_.resize(all_params.size());
    for (std::size_t i = 0; i < all_params.size(); ++i) {
        param_snapshot_[i] = store_.get_value(all_params[i].id);
    }

    // Process!
    processor_->process(output_view, input_view, midi_in, midi_out, ctx);

    // Write parameter output changes — lets the host record automation
    // from parameter changes made by the plugin during process()
    if (data.outputParameterChanges) {
        for (std::size_t i = 0; i < all_params.size(); ++i) {
            float current = store_.get_value(all_params[i].id);
            if (current != param_snapshot_[i]) {
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

    // Write MIDI output
    if (data.outputEvents && !midi_out.empty()) {
        for (const auto& me : midi_out) {
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
    auto data = store_.serialize();
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
    if (!store_.deserialize(data)) return kResultFalse;

    // Sync restored values back to VST3 parameter system
    for (const auto& param : store_.all_params()) {
        float normalized = store_.get_normalized(param.id);
        setParamNormalized(static_cast<ParamID>(param.id),
                           static_cast<ParamValue>(normalized));
    }
    return kResultOk;
}

} // namespace pulp::format::vst3
