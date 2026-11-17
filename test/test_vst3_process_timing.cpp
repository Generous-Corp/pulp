// VST3 adapter -> public Processor -> host event timeline traces.
//
// The real PulpVst3Processor is driven through IComponent / IAudioProcessor
// with an SDK ProcessContext. A Processor records the context and MIDI it
// receives; a test-side host binding advances one HostBlockTimeline from that
// context alone and projects the delivered events, as a graph-owning
// Processor would.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/host_event_timestamps.hpp>

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace pulp;
using host::HostBlockBoundary;
using host::HostBlockTimeline;
using host::HostBlockWindow;
using host::HostEventIngress;
using host::HostEventRecord;
using host::HostEventResult;
using SdkContext = Steinberg::Vst::ProcessContext;

namespace {

constexpr int kMaxFrames = 256;

struct ObservedBlock {
    format::ProcessContext ctx;
    std::vector<std::int32_t> offsets;
};

std::vector<ObservedBlock>* g_sink = nullptr;

class TimingProbe final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "PulpVst3TimingProbe";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.vst3-process-timing";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        d.accepts_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i)
                oc[i] = 0.0f;
        }
        if (!g_sink)
            return;
        ObservedBlock block;
        block.ctx = ctx;
        for (const auto& event : midi_in)
            block.offsets.push_back(event.sample_offset);
        g_sink->push_back(block);
    }
};

std::unique_ptr<format::Processor> make_probe() {
    return std::make_unique<TimingProbe>();
}

class HostApp final : public Steinberg::Vst::IHostApplication {
  public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* kName = "PulpTest";
        int i = 0;
        for (; kName[i]; ++i)
            name[i] = static_cast<Steinberg::Vst::TChar>(kName[i]);
        name[i] = 0;
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj)
            *obj = nullptr;
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }
};

struct Block {
    // Absent: the host supplies no ProcessContext at all.
    bool has_context = true;
    bool playing = true;
    // Absent: kContTimeValid is clear.
    std::optional<std::int64_t> continuous;
    std::int64_t project = 0;
    int frames = 64;
    std::vector<std::int32_t> event_offsets{};
};

struct Vst3Host {
    HostApp app;
    format::vst3::PulpVst3Processor processor{&make_probe};
    std::vector<ObservedBlock> observed;

    Vst3Host() {
        g_sink = &observed;
        REQUIRE(processor.initialize(&app) == Steinberg::kResultOk);
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = kMaxFrames;
        setup.sampleRate = 48000.0;
        REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
        REQUIRE(processor.setActive(true) == Steinberg::kResultOk);
        // SingleComponentEffect answers kNotImplemented; the adapter keeps that
        // answer and only records the restart.
        (void)processor.setProcessing(true);
    }
    ~Vst3Host() {
        processor.setProcessing(false);
        processor.setActive(false);
        processor.terminate();
        g_sink = nullptr;
    }

    void run(const Block& block) {
        std::vector<float> in_l(static_cast<std::size_t>(block.frames));
        std::vector<float> in_r(in_l.size()), out_l(in_l.size()), out_r(in_l.size());
        float* inputs[2] = {in_l.data(), in_r.data()};
        float* outputs[2] = {out_l.data(), out_r.data()};
        Steinberg::Vst::AudioBusBuffers in_bus[1]{};
        in_bus[0].numChannels = 2;
        in_bus[0].channelBuffers32 = inputs;
        Steinberg::Vst::AudioBusBuffers out_bus[1]{};
        out_bus[0].numChannels = 2;
        out_bus[0].channelBuffers32 = outputs;

        Steinberg::Vst::ParameterChanges in_params;
        Steinberg::Vst::ParameterChanges out_params;
        Steinberg::Vst::EventList in_events(16);
        Steinberg::Vst::EventList out_events(16);
        for (const auto offset : block.event_offsets) {
            Steinberg::Vst::Event event{};
            event.type = Steinberg::Vst::Event::kNoteOnEvent;
            event.sampleOffset = offset;
            event.noteOn.channel = 0;
            event.noteOn.pitch = 60;
            event.noteOn.velocity = 0.8f;
            event.noteOn.noteId = -1;
            in_events.addEvent(event);
        }

        SdkContext context{};
        context.sampleRate = 48000.0;
        context.state = (block.playing ? SdkContext::kPlaying : 0u) |
                        (block.continuous ? SdkContext::kContTimeValid : 0u);
        context.projectTimeSamples = block.project;
        context.continousTimeSamples = block.continuous.value_or(0);

        Steinberg::Vst::ProcessData data{};
        data.processMode = Steinberg::Vst::kRealtime;
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = block.frames;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = in_bus;
        data.outputs = out_bus;
        data.inputParameterChanges = &in_params;
        data.outputParameterChanges = &out_params;
        data.inputEvents = &in_events;
        data.outputEvents = &out_events;
        data.processContext = block.has_context ? &context : nullptr;
        REQUIRE(processor.process(data) == Steinberg::kResultOk);
    }
};

struct Binding {
    HostBlockTimeline timeline;
    HostEventIngress<64> ingress;
    std::uint64_t sequence = 0;

    HostBlockBoundary feed(const ObservedBlock& block,
                           std::vector<HostEventResult>* results = nullptr,
                           std::vector<std::int64_t>* projected = nullptr,
                           HostBlockWindow* window_out = nullptr) {
        HostBlockWindow window;
        REQUIRE(host::advance_from_context(timeline, block.ctx, window) ==
                HostEventResult::Accepted);
        REQUIRE(ingress.begin_block(window.epoch, window.sample_start, window.frames) ==
                HostEventResult::Accepted);
        for (const auto offset : block.offsets) {
            HostEventRecord record;
            auto result =
                host::make_record_from_block_offset(timeline, window, offset, 1, ++sequence,
                                                    midi::MidiEvent::note_on(0, 60, 100), record);
            if (result == HostEventResult::Accepted)
                result = ingress.publish(record);
            if (results)
                results->push_back(result);
        }
        std::array<HostEventRecord, 64> out{};
        std::size_t count = 0;
        REQUIRE(ingress.project(out, count) == HostEventResult::Accepted);
        if (projected)
            for (std::size_t i = 0; i < count; ++i)
                projected->push_back(out[i].absolute_sample);
        REQUIRE(ingress.end_block() == HostEventResult::Accepted);
        if (window_out)
            *window_out = window;
        return timeline.last_boundary();
    }
};

} // namespace

TEST_CASE("VST3 requests and publishes the continuous-time counter", "[process-timing][vst3]") {
    using PCR = Steinberg::Vst::IProcessContextRequirements;
    REQUIRE((format::vst3::kProcessContextRequirements & PCR::kNeedContinousTimeSamples) != 0);

    Vst3Host host;
    REQUIRE(host.observed.empty()); // activation warm-up is not an observed block
    host.run({.continuous = 4800, .project = 960});
    host.run({.continuous = 4864, .project = 1024});
    REQUIRE(host.observed.size() == 2);
    const auto& first = host.observed[0].ctx.timing;
    REQUIRE(first.discontinuity);
    REQUIRE(first.has_steady_sample);
    REQUIRE(first.steady_sample == 4800);
    REQUIRE(first.transport_sample == 960);
    REQUIRE(first.transport_moving);
    REQUIRE(first.playback_rate == format::PlaybackRate::Unknown);
    REQUIRE_FALSE(host.observed[1].ctx.timing.discontinuity);
    REQUIRE(host.observed[1].ctx.timing.transport_sample == host.observed[1].ctx.position_samples);

    // A processing restart is a discontinuity for exactly the next block.
    (void)host.processor.setProcessing(false);
    (void)host.processor.setProcessing(true);
    host.run({.continuous = 9000, .project = 2000});
    host.run({.continuous = 9064, .project = 2064});
    REQUIRE(host.observed[2].ctx.timing.discontinuity);
    REQUIRE_FALSE(host.observed[3].ctx.timing.discontinuity);
}

TEST_CASE("VST3 seeks, loops, stopped transport, gaps and clock changes",
          "[process-timing][vst3][boundaries]") {
    Vst3Host host;
    host.run({.continuous = 0, .project = 100});                      // activation
    host.run({.continuous = 64, .project = 164});                     // continuous
    host.run({.continuous = 128, .project = 7000});                   // seek while playing
    host.run({.continuous = 192, .project = 2000});                   // cycle wrap
    host.run({.playing = false, .continuous = 256, .project = 2064}); // stopped
    host.run({.playing = false, .continuous = 320, .project = 50});   // relocate stopped
    host.run({.playing = false, .continuous = 1000, .project = 50});  // host skipped
    host.run({.playing = false, .project = 50});                      // kContTimeValid drops out
    host.run({.has_context = false});                                 // no context at all
    REQUIRE(host.observed.size() == 9);
    REQUIRE_FALSE(host.observed[7].ctx.timing.has_steady_sample);
    REQUIRE_FALSE(host.observed[8].ctx.timing.has_transport_sample);

    Binding binding;
    std::vector<HostBlockBoundary> boundaries;
    for (const auto& block : host.observed)
        boundaries.push_back(binding.feed(block));
    REQUIRE(boundaries == std::vector<HostBlockBoundary>{
                              HostBlockBoundary::First, HostBlockBoundary::Continuous,
                              HostBlockBoundary::NewEpoch, HostBlockBoundary::NewEpoch,
                              HostBlockBoundary::Continuous, HostBlockBoundary::Continuous,
                              HostBlockBoundary::Gap, HostBlockBoundary::NewEpoch,
                              HostBlockBoundary::Continuous});
}

TEST_CASE("VST3 marks the block after a clamped tail discontinuous",
          "[process-timing][vst3][skipped]") {
    Vst3Host host;
    host.run({.has_context = false});
    host.run({.has_context = false, .frames = 2 * kMaxFrames}); // renders only kMaxFrames
    host.run({.has_context = false});
    host.run({.has_context = false});
    REQUIRE(host.observed.size() == 4);
    REQUIRE(host.observed[1].ctx.num_samples == kMaxFrames);
    REQUIRE_FALSE(host.observed[1].ctx.timing.discontinuity);
    // The Processor never saw the tail and there is no counter to measure it.
    REQUIRE(host.observed[2].ctx.timing.discontinuity);
    REQUIRE_FALSE(host.observed[3].ctx.timing.discontinuity);
}

TEST_CASE("VST3 timing projects identically for every block partition",
          "[process-timing][vst3][partition]") {
    const std::vector<std::int64_t> event_samples{0, 5, 63, 64, 130, 255};
    const std::vector<std::vector<int>> partitions{
        {256}, {64, 64, 64, 64}, {100, 100, 56}, {1, 127, 128}};
    std::vector<std::vector<std::int64_t>> projections;
    for (const auto& partition : partitions) {
        Vst3Host host;
        std::int64_t start = 0;
        for (const auto frames : partition) {
            Block block{.continuous = 10000 + start, .project = 480 + start, .frames = frames};
            for (const auto sample : event_samples)
                if (sample >= start && sample < start + frames)
                    block.event_offsets.push_back(static_cast<std::int32_t>(sample - start));
            host.run(block);
            start += frames;
        }
        Binding binding;
        std::vector<std::int64_t> projected;
        for (std::size_t i = 0; i < host.observed.size(); ++i) {
            const auto boundary = binding.feed(host.observed[i], nullptr, &projected);
            REQUIRE(boundary ==
                    (i == 0 ? HostBlockBoundary::First : HostBlockBoundary::Continuous));
        }
        projections.push_back(projected);
    }
    REQUIRE(projections.front() == event_samples);
    for (const auto& projection : projections)
        REQUIRE(projection == projections.front());
}

TEST_CASE("VST3 event offsets outside the block are refused, never clamped",
          "[process-timing][vst3][offsets]") {
    Vst3Host host;
    host.run({.continuous = 0, .project = 0, .frames = 32, .event_offsets = {-1, 0, 31, 32}});
    REQUIRE(host.observed.size() == 1);
    Binding binding;
    std::vector<HostEventResult> results;
    std::vector<std::int64_t> projected;
    (void)binding.feed(host.observed[0], &results, &projected);
    // The adapter sorts the decoded stream by offset before the Processor
    // sees it.
    REQUIRE(results ==
            std::vector<HostEventResult>{HostEventResult::OutOfRange, HostEventResult::Accepted,
                                         HostEventResult::Accepted, HostEventResult::OutOfRange});
    REQUIRE(projected == std::vector<std::int64_t>{0, 31});
}
