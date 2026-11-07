// What a hosted VST3 plug-in sees from Pulp's VST3 slot
// (core/host/src/plugin_slot_vst3.cpp): the host name, ProcessContext
// transport, offline and 64-bit processing, every audio bus (sidechain in, aux
// out), the full MIDI 1.0 map in both directions, IComponentHandler feedback,
// and latency restarts reaching SignalGraph's latency compensation.
//
// Driven through make_vst3_slot with a fake AudioEffect processor and an
// EditController that implements IMidiMapping, so no bundle is loaded.

#include <catch2/catch_test_macros.hpp>

#include "plugin_slot_vst3_internal.hpp"
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>

#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <public.sdk/source/vst/vstaudioeffect.h>
#include <public.sdk/source/vst/vsteditcontroller.h>

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Steinberg;
using pulp::host::PluginHostSettings;
using pulp::host::PluginInfo;
using pulp::host::PluginParameterEdit;
using pulp::host::PluginSlot;
namespace fmt = pulp::format;

namespace {

constexpr Vst::ParamID kGain = 1;
constexpr Vst::ParamID kBend = 2;
constexpr Vst::ParamID kProgram = 3;

struct ParamPoint {
    Vst::ParamID id;
    int32 offset;
    double value;
};

class FakeProcessor : public Vst::AudioEffect {
  public:
    // Observed.
    int32 setup_mode = -1;
    int32 setup_sample_size = -1;
    int32 data_mode = -1;
    int32 data_sample_size = -1;
    bool had_context = false;
    Vst::ProcessContext context{};
    int32 num_inputs = 0;
    int32 num_outputs = 0;
    float sidechain_sample = -1.0f;
    std::vector<Vst::Event> events;
    std::vector<std::vector<uint8>> sysex;
    std::vector<ParamPoint> params;
    uint32 latency = 0;

    // Emitted on the next process().
    bool emit_midi = false;
    bool emit_param = false;
    float aux_value = 0.0f;
    std::array<uint8, 4> sysex_out{0xF0, 0x11, 0x22, 0xF7};

    tresult PLUGIN_API initialize(FUnknown* context_in) SMTG_OVERRIDE {
        const tresult r = AudioEffect::initialize(context_in);
        if (r != kResultOk)
            return r;
        addAudioInput(STR16("Main In"), Vst::SpeakerArr::kStereo);
        addAudioInput(STR16("Sidechain"), Vst::SpeakerArr::kStereo, Vst::kAux);
        addAudioOutput(STR16("Main Out"), Vst::SpeakerArr::kStereo);
        addAudioOutput(STR16("Aux Out"), Vst::SpeakerArr::kStereo, Vst::kAux);
        addEventInput(STR16("MIDI In"));
        addEventOutput(STR16("MIDI Out"));
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 size) SMTG_OVERRIDE {
        return size == Vst::kSample32 || size == Vst::kSample64 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) SMTG_OVERRIDE {
        setup_mode = setup.processMode;
        setup_sample_size = setup.symbolicSampleSize;
        return AudioEffect::setupProcessing(setup);
    }

    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE {
        return latency;
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) SMTG_OVERRIDE {
        data_mode = data.processMode;
        data_sample_size = data.symbolicSampleSize;
        had_context = data.processContext != nullptr;
        if (had_context)
            context = *data.processContext;
        num_inputs = data.numInputs;
        num_outputs = data.numOutputs;
        if (data.numInputs > 1 && data.inputs[1].channelBuffers32)
            sidechain_sample = data.inputs[1].channelBuffers32[0][0];

        events.clear();
        sysex.clear();
        if (data.inputEvents) {
            for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
                Vst::Event e{};
                data.inputEvents->getEvent(i, e);
                events.push_back(e);
                if (e.type == Vst::Event::kDataEvent)
                    sysex.emplace_back(e.data.bytes, e.data.bytes + e.data.size);
            }
        }
        params.clear();
        if (data.inputParameterChanges) {
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
                auto* q = data.inputParameterChanges->getParameterData(i);
                for (int32 p = 0; p < q->getPointCount(); ++p) {
                    int32 offset = 0;
                    Vst::ParamValue value = 0;
                    q->getPoint(p, offset, value);
                    params.push_back({q->getParameterId(), offset, value});
                }
            }
        }

        if (data.numOutputs > 1 && data.outputs[1].channelBuffers32) {
            for (int32 c = 0; c < data.outputs[1].numChannels; ++c)
                for (int32 i = 0; i < data.numSamples; ++i)
                    data.outputs[1].channelBuffers32[c][i] = aux_value;
        }

        if (emit_midi && data.outputEvents) {
            Vst::Event on{};
            on.type = Vst::Event::kNoteOnEvent;
            on.sampleOffset = 2;
            on.noteOn.channel = 1;
            on.noteOn.pitch = 62;
            on.noteOn.velocity = 1.0f;
            data.outputEvents->addEvent(on);
            Vst::Event cc{};
            cc.type = Vst::Event::kLegacyMIDICCOutEvent;
            cc.sampleOffset = 3;
            cc.midiCCOut.controlNumber = 74;
            cc.midiCCOut.channel = 1;
            cc.midiCCOut.value = 33;
            data.outputEvents->addEvent(cc);
            Vst::Event bend{};
            bend.type = Vst::Event::kLegacyMIDICCOutEvent;
            bend.sampleOffset = 4;
            bend.midiCCOut.controlNumber = Vst::kPitchBend;
            bend.midiCCOut.channel = 0;
            bend.midiCCOut.value = 0x00;
            bend.midiCCOut.value2 = 0x40;
            data.outputEvents->addEvent(bend);
            Vst::Event sx{};
            sx.type = Vst::Event::kDataEvent;
            sx.sampleOffset = 5;
            sx.data.type = Vst::DataEvent::kMidiSysEx;
            sx.data.size = (uint32)sysex_out.size();
            sx.data.bytes = sysex_out.data();
            data.outputEvents->addEvent(sx);
        }
        if (emit_param && data.outputParameterChanges) {
            int32 index = 0;
            if (auto* q = data.outputParameterChanges->addParameterData(kGain, index))
                q->addPoint(0, 0.3, index);
        }
        return kResultOk;
    }
};

class FakeController : public Vst::EditController, public Vst::IMidiMapping {
  public:
    tresult PLUGIN_API initialize(FUnknown* context_in) SMTG_OVERRIDE {
        const tresult r = EditController::initialize(context_in);
        if (r != kResultOk)
            return r;
        parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5, Vst::ParameterInfo::kCanAutomate,
                                kGain);
        parameters.addParameter(STR16("Bend"), nullptr, 0, 0.5, Vst::ParameterInfo::kCanAutomate,
                                kBend);
        parameters.addParameter(STR16("Program"), nullptr, 7, 0.0,
                                Vst::ParameterInfo::kIsProgramChange, kProgram);
        return kResultOk;
    }

    tresult PLUGIN_API getMidiControllerAssignment(int32, int16, Vst::CtrlNumber ctrl,
                                                   Vst::ParamID& id) SMTG_OVERRIDE {
        if (ctrl == 7)
            id = kGain;
        else if (ctrl == Vst::kPitchBend)
            id = kBend;
        else if (ctrl == Vst::kCtrlProgramChange)
            id = kProgram;
        else
            return kResultFalse;
        return kResultTrue;
    }

    Vst::IComponentHandler* handler() const {
        return componentHandler;
    }

    OBJ_METHODS(FakeController, EditController)
    DEFINE_INTERFACES
    DEF_INTERFACE(Vst::IMidiMapping)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)
};

// Owns the fakes for a test. The slot terminates and releases what it is given,
// so an extra reference keeps the fakes inspectable until the slot is gone.
struct Rig {
    FakeProcessor* processor = new FakeProcessor;
    FakeController* controller = new FakeController;
    std::unique_ptr<PluginSlot> slot;

    Rig() {
        processor->initialize(nullptr);
        controller->initialize(nullptr);
        processor->addRef();
        controller->addRef();
        Vst::IAudioProcessor* audio = nullptr;
        processor->queryInterface(Vst::IAudioProcessor::iid, (void**)&audio);
        PluginInfo info;
        info.name = "Fake VST3 Fidelity";
        info.format = pulp::host::PluginFormat::VST3;
        slot = pulp::host::make_vst3_slot(info, processor, audio, controller);
    }
    ~Rig() {
        slot.reset();
        processor->release();
        controller->release();
    }
};

struct Buses {
    static constexpr int kFrames = 32;
    std::vector<std::vector<float>> store =
        std::vector<std::vector<float>>(8, std::vector<float>(kFrames, 0.0f));
    std::array<const float*, 2> main_in{store[0].data(), store[1].data()};
    std::array<const float*, 2> side_in{store[2].data(), store[3].data()};
    std::array<float*, 2> main_out{store[4].data(), store[5].data()};
    std::array<float*, 2> aux_out{store[6].data(), store[7].data()};
    std::array<fmt::ProcessBusBufferView<const float>, 2> ins{};
    std::array<fmt::ProcessBusBufferView<float>, 2> outs{};
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue params;

    Buses() {
        ins[0].buffer = pulp::audio::BufferView<const float>(main_in.data(), 2, kFrames);
        ins[1].info.role = fmt::BusRole::Sidechain;
        ins[1].buffer = pulp::audio::BufferView<const float>(side_in.data(), 2, kFrames);
        outs[0].buffer = pulp::audio::BufferView<float>(main_out.data(), 2, kFrames);
        outs[1].info.role = fmt::BusRole::Aux;
        outs[1].buffer = pulp::audio::BufferView<float>(aux_out.data(), 2, kFrames);
    }
    fmt::ProcessBuffers buffers() {
        return {fmt::ProcessBusBufferSet<const float>{std::span(ins)},
                fmt::ProcessBusBufferSet<float>{std::span(outs)}};
    }
    void run(PluginSlot& slot) {
        auto b = buffers();
        slot.process(b, midi_in, midi_out, params, kFrames);
    }
    void run(PluginSlot& slot, const fmt::ProcessContext& ctx) {
        auto b = buffers();
        slot.process(b, midi_in, midi_out, params, kFrames, ctx);
    }
};

std::string host_name() {
    Vst::String128 name{};
    pulp::host::vst3_host_application()->getName(name);
    std::string out;
    for (int i = 0; i < 128 && name[i]; ++i)
        out.push_back((char)name[i]);
    return out;
}

const ParamPoint* find_param(const std::vector<ParamPoint>& points, Vst::ParamID id) {
    for (const auto& p : points)
        if (p.id == id)
            return &p;
    return nullptr;
}

} // namespace

TEST_CASE("VST3 host application reports the configured host name", "[host][vst3][host-context]") {
    const auto previous = pulp::host::plugin_host_identity();
    auto identity = previous;
    identity.name = "Fidelity Host";
    pulp::host::set_plugin_host_identity(identity);
    CHECK(host_name() == "Fidelity Host");
    pulp::host::set_plugin_host_identity(previous);
    CHECK(host_name() == previous.name);
}

TEST_CASE("VST3 slot sets up realtime 32-bit processing by default", "[host][vst3][offline]") {
    Rig rig;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    CHECK(rig.processor->setup_mode == Vst::kRealtime);
    CHECK(rig.processor->setup_sample_size == Vst::kSample32);
    Buses b;
    b.run(*rig.slot);
    CHECK(rig.processor->data_mode == Vst::kRealtime);
}

TEST_CASE("VST3 slot runs offline and 64-bit when the host settings ask", "[host][vst3][offline]") {
    Rig rig;
    PluginHostSettings settings;
    settings.process_mode = fmt::ProcessMode::Offline;
    settings.prefer_double_precision = true;
    REQUIRE(rig.slot->set_host_settings(settings));
    REQUIRE(rig.slot->prepare(48000.0, 64));
    CHECK(rig.processor->setup_mode == Vst::kOffline);
    CHECK(rig.processor->setup_sample_size == Vst::kSample64);
    Buses b;
    b.run(*rig.slot);
    CHECK(rig.processor->data_mode == Vst::kOffline);
    CHECK(rig.processor->data_sample_size == Vst::kSample64);
}

TEST_CASE("VST3 plug-in without a host transport sees a free-running clock and no tempo",
          "[host][vst3][transport]") {
    Rig rig;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    Buses b;
    b.run(*rig.slot);
    b.run(*rig.slot);
    REQUIRE(rig.processor->had_context);
    const auto& c = rig.processor->context;
    CHECK(c.projectTimeSamples == Buses::kFrames);
    CHECK((c.state & Vst::ProcessContext::kTempoValid) == 0);
    CHECK((c.state & Vst::ProcessContext::kTimeSigValid) == 0);
}

TEST_CASE("VST3 plug-in receives the host transport", "[host][vst3][transport]") {
    Rig rig;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    fmt::ProcessContext ctx;
    ctx.is_playing = true;
    ctx.is_looping = true;
    ctx.tempo_bpm = 96.0;
    ctx.position_beats = 13.0;
    ctx.position_samples = 123456;
    ctx.time_sig_numerator = 6;
    ctx.time_sig_denominator = 8;
    ctx.loop_start_beats = 12.0;
    ctx.loop_end_beats = 18.0;
    ctx.host_time_ns = 42;
    ctx.frame_rate = fmt::FrameRate::fps_29_97_drop;
    using F = fmt::TransportField;
    for (auto f : {F::Playing, F::Looping, F::Tempo, F::BeatPosition, F::SamplePosition,
                   F::TimeSignature, F::LoopRange, F::HostTime, F::FrameRate})
        ctx.transport_validity.set(f);
    Buses b;
    b.run(*rig.slot, ctx);

    REQUIRE(rig.processor->had_context);
    const auto& c = rig.processor->context;
    using PC = Vst::ProcessContext;
    CHECK((c.state & PC::kPlaying) != 0);
    CHECK((c.state & PC::kRecording) == 0);
    CHECK((c.state & PC::kCycleActive) != 0);
    CHECK(c.sampleRate == 48000.0);
    CHECK(c.projectTimeSamples == 123456);
    CHECK((c.state & PC::kTempoValid) != 0);
    CHECK(c.tempo == 96.0);
    CHECK((c.state & PC::kProjectTimeMusicValid) != 0);
    CHECK(c.projectTimeMusic == 13.0);
    CHECK((c.state & PC::kTimeSigValid) != 0);
    CHECK(c.timeSigNumerator == 6);
    CHECK(c.timeSigDenominator == 8);
    // 6/8 is three quarter notes per bar; beat 13 is in the bar starting at 12.
    CHECK((c.state & PC::kBarPositionValid) != 0);
    CHECK(c.barPositionMusic == 12.0);
    CHECK((c.state & PC::kCycleValid) != 0);
    CHECK(c.cycleStartMusic == 12.0);
    CHECK(c.cycleEndMusic == 18.0);
    CHECK((c.state & PC::kSystemTimeValid) != 0);
    CHECK(c.systemTime == 42);
    CHECK((c.state & PC::kSmpteValid) != 0);
    CHECK(c.frameRate.framesPerSecond == 30);
    CHECK(c.frameRate.flags == (Vst::FrameRate::kPullDownRate | Vst::FrameRate::kDropRate));
}

TEST_CASE("VST3 sidechain bus receives the host sidechain and aux outputs reach the host",
          "[host][vst3][buses]") {
    Rig rig;
    rig.processor->aux_value = 0.375f;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    Buses b;
    std::fill(b.store[2].begin(), b.store[2].end(), 0.5f);
    b.run(*rig.slot);
    CHECK(rig.processor->num_inputs == 2);
    CHECK(rig.processor->num_outputs == 2);
    CHECK(rig.processor->sidechain_sample == 0.5f);
    CHECK(b.store[6][0] == 0.375f);
    CHECK(b.store[7][Buses::kFrames - 1] == 0.375f);
}

TEST_CASE("VST3 sidechain without a host sidechain gets silence through the legacy overload",
          "[host][vst3][buses]") {
    Rig rig;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    std::vector<float> l(16, 0.1f), r(16, 0.1f), ol(16), or_(16);
    std::array<const float*, 2> in{l.data(), r.data()};
    std::array<float*, 2> out{ol.data(), or_.data()};
    pulp::audio::BufferView<const float> input(in.data(), 2, 16);
    pulp::audio::BufferView<float> output(out.data(), 2, 16);
    pulp::midi::MidiBuffer mi, mo;
    pulp::host::ParameterEventQueue params;
    rig.slot->process(output, input, mi, mo, params, 16);
    CHECK(rig.processor->num_inputs == 2);
    CHECK(rig.processor->sidechain_sample == 0.0f);
}

TEST_CASE("VST3 plug-in receives notes, poly pressure, sysex, and mapped controllers",
          "[host][vst3][midi]") {
    Rig rig;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    Buses b;
    auto on = pulp::midi::MidiEvent::note_on(2, 60, 127);
    on.sample_offset = 1;
    b.midi_in.add(on);
    pulp::midi::MidiEvent pressure;
    pressure.sample_offset = 2;
    pressure.message = choc::midi::ShortMessage(0xA2, 60, 127);
    b.midi_in.add(pressure);
    pulp::midi::MidiEvent volume;
    volume.sample_offset = 3;
    volume.message = choc::midi::ShortMessage(0xB0, 7, 127);
    b.midi_in.add(volume);
    pulp::midi::MidiEvent unmapped;
    unmapped.sample_offset = 3;
    unmapped.message = choc::midi::ShortMessage(0xB0, 1, 127);
    b.midi_in.add(unmapped);
    pulp::midi::MidiEvent bend;
    bend.sample_offset = 4;
    bend.message = choc::midi::ShortMessage(0xE0, 0x7F, 0x7F);
    b.midi_in.add(bend);
    pulp::midi::MidiEvent program;
    program.sample_offset = 5;
    program.message = choc::midi::ShortMessage(0xC0, 7, 0);
    b.midi_in.add(program);
    pulp::midi::MidiEvent off;
    off.sample_offset = 9;
    off.message = choc::midi::ShortMessage(0x82, 60, 0);
    b.midi_in.add(off);
    b.midi_in.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 6);
    b.run(*rig.slot);

    const auto& events = rig.processor->events;
    REQUIRE(events.size() == 4);
    CHECK(events[0].type == Vst::Event::kNoteOnEvent);
    CHECK(events[0].noteOn.channel == 2);
    CHECK(events[0].noteOn.velocity == 1.0f);
    CHECK(events[1].type == Vst::Event::kPolyPressureEvent);
    CHECK(events[1].polyPressure.pressure == 1.0f);
    CHECK(events[2].type == Vst::Event::kDataEvent);
    CHECK(events[2].sampleOffset == 6);
    CHECK(events[3].type == Vst::Event::kNoteOffEvent);
    REQUIRE(rig.processor->sysex.size() == 1);
    CHECK(rig.processor->sysex[0] == std::vector<uint8>{0xF0, 0x7D, 0x01, 0xF7});

    const auto& params = rig.processor->params;
    const auto* gain = find_param(params, kGain);
    REQUIRE(gain);
    CHECK(gain->offset == 3);
    CHECK(gain->value == 1.0);
    const auto* bend_param = find_param(params, kBend);
    REQUIRE(bend_param);
    CHECK(bend_param->value == 1.0);
    const auto* program_param = find_param(params, kProgram);
    REQUIRE(program_param);
    CHECK(program_param->value == 1.0); // program 7 of a 7-step list
    CHECK(params.size() == 3);          // CC 1 is unmapped and dropped
}

TEST_CASE("VST3 plug-in MIDI output comes back as MIDI 1.0", "[host][vst3][midi]") {
    Rig rig;
    rig.processor->emit_midi = true;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    Buses b;
    b.run(*rig.slot);
    std::vector<std::array<uint8_t, 3>> got;
    for (const auto& e : b.midi_out)
        got.push_back({e.message.data()[0], e.message.data()[1], e.message.data()[2]});
    REQUIRE(got.size() == 3);
    CHECK(got[0] == std::array<uint8_t, 3>{0x91, 62, 127});
    CHECK(got[1] == std::array<uint8_t, 3>{0xB1, 74, 33});
    CHECK(got[2] == std::array<uint8_t, 3>{0xE0, 0x00, 0x40});
    REQUIRE(b.midi_out.sysex_size() == 1);
    CHECK(b.midi_out.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x11, 0x22, 0xF7});
}

TEST_CASE("VST3 editor edits reach the host and the processor", "[host][vst3][params]") {
    Rig rig;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    auto* handler = rig.controller->handler();
    REQUIRE(handler != nullptr);
    handler->beginEdit(kGain);
    rig.controller->setParamNormalized(kGain, 0.75);
    handler->performEdit(kGain, 0.75);
    handler->endEdit(kGain);

    std::array<PluginParameterEdit, 8> edits{};
    REQUIRE(rig.slot->drain_parameter_edits(edits) == 3);
    CHECK(edits[0].kind == PluginParameterEdit::Kind::begin_gesture);
    CHECK(edits[1].kind == PluginParameterEdit::Kind::value);
    CHECK(edits[1].id == kGain);
    CHECK(edits[1].plain_value == 0.75f);
    CHECK(edits[2].kind == PluginParameterEdit::Kind::end_gesture);

    Buses b;
    b.run(*rig.slot);
    const auto* gain = find_param(rig.processor->params, kGain);
    REQUIRE(gain);
    CHECK(gain->offset == 0);
    CHECK(gain->value == 0.75);
}

TEST_CASE("VST3 processor parameter output reaches the host and the controller",
          "[host][vst3][params]") {
    Rig rig;
    rig.processor->emit_param = true;
    REQUIRE(rig.slot->prepare(48000.0, 64));
    Buses b;
    b.run(*rig.slot);
    std::array<PluginParameterEdit, 4> edits{};
    REQUIRE(rig.slot->drain_parameter_edits(edits) == 1);
    CHECK(edits[0].id == kGain);
    CHECK(std::abs(edits[0].plain_value - 0.3f) < 1e-6f);
    CHECK(std::abs(rig.controller->getParamNormalized(kGain) - 0.3) < 1e-9);
}

TEST_CASE("VST3 setDirty is counted as a state change", "[host][vst3][params]") {
    Rig rig;
    Vst::IComponentHandler2* handler2 = nullptr;
    REQUIRE(rig.controller->handler()->queryInterface(Vst::IComponentHandler2::iid,
                                                      (void**)&handler2) == kResultTrue);
    handler2->setDirty(true);
    CHECK(rig.slot->state_change_count() == 1);
}

TEST_CASE("VST3 latency restart reaches SignalGraph latency compensation",
          "[host][vst3][restart][pdc]") {
    Rig rig;
    rig.processor->latency = 32;
    auto* processor = rig.processor;
    auto* controller = rig.controller;
    pulp::host::SignalGraph graph;
    const auto in = graph.add_input_node(2);
    const auto plug = graph.add_plugin_node(std::move(rig.slot), 2, 2, "fake");
    const auto out = graph.add_output_node(2);
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(in, 1, plug, 1));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.connect(plug, 1, out, 1));
    REQUIRE(graph.prepare(48000.0, 128));
    CHECK(graph.latency_samples() == 32);
    CHECK_FALSE(graph.plugin_restart_pending());

    processor->latency = 512;
    controller->handler()->restartComponent(Vst::kLatencyChanged);
    CHECK(graph.plugin_restart_pending());
    REQUIRE(graph.apply_plugin_restarts());
    CHECK(graph.latency_samples() == 512);
    CHECK_FALSE(graph.plugin_restart_pending());
}
