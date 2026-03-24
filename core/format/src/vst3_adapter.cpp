// VST3 Adapter implementation
// Implements the VST3 hosting contract by wrapping a Pulp Processor

#include "vst3_adapter.hpp"
#include <pulp/runtime/log.hpp>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <cstring>

namespace pulp::format::vst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

PulpVst3Processor::PulpVst3Processor(ProcessorFactory factory)
    : factory_(factory)
{
    setControllerClass(FUID()); // Will be set properly in the factory
}

tresult PLUGIN_API PulpVst3Processor::initialize(FUnknown* context) {
    auto result = AudioEffect::initialize(context);
    if (result != kResultOk) return result;

    // Create the Pulp processor
    processor_ = factory_();
    if (!processor_) return kInternalError;

    auto desc = processor_->descriptor();
    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);

    // Add audio buses based on processor descriptor
    if (desc.default_input_channels > 0) {
        addAudioInput(STR16("Audio In"),
            desc.default_input_channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo);
    }
    if (desc.default_output_channels > 0) {
        addAudioOutput(STR16("Audio Out"),
            desc.default_output_channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo);
    }

    // Add event buses for MIDI
    if (desc.accepts_midi) {
        addEventInput(STR16("MIDI In"), 1);
    }
    if (desc.produces_midi) {
        addEventOutput(STR16("MIDI Out"), 1);
    }

    runtime::log_info("VST3: initialized '{}'", desc.name);
    return kResultOk;
}

tresult PLUGIN_API PulpVst3Processor::terminate() {
    processor_.reset();
    return AudioEffect::terminate();
}

tresult PLUGIN_API PulpVst3Processor::setBusArrangements(
    SpeakerArrangement* inputs, int32 numIns,
    SpeakerArrangement* outputs, int32 numOuts)
{
    // Accept stereo or mono
    if (numIns >= 1 && numOuts >= 1) {
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }
    return kResultFalse;
}

tresult PLUGIN_API PulpVst3Processor::setupProcessing(ProcessSetup& setup) {
    if (!processor_) return kInternalError;

    PrepareContext ctx;
    ctx.sample_rate = setup.sampleRate;
    ctx.max_buffer_size = setup.maxSamplesPerBlock;
    // Channel count determined by bus arrangement
    ctx.input_channels = 2;  // TODO: query from bus arrangement
    ctx.output_channels = 2;

    processor_->prepare(ctx);
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API PulpVst3Processor::setActive(TBool state) {
    if (!state && processor_) {
        processor_->release();
    }
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API PulpVst3Processor::process(ProcessData& data) {
    if (!processor_) return kInternalError;

    // Read parameter changes from host
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
                    // VST3 uses normalized 0-1 values
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

    // Process!
    processor_->process(output_view, input_view, midi_in, midi_out, ctx);

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
    return store_.deserialize(data) ? kResultOk : kResultFalse;
}

} // namespace pulp::format::vst3
