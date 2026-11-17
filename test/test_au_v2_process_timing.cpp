// Neutral timing facts (`ProcessContext::timing`) through the real AU v2
// adapters, projected onto `pulp::host::HostBlockTimeline`.
//
// The effect is driven through `AUBase::DoRender` with a host render callback
// on its main input, so `AUEffectBase::Render` and its scheduled-parameter
// slicing run exactly as a host would reach them. The instrument and MIDI
// processor are driven through their `Render()` overrides, as the other AU v2
// tests do. No AU host loads the units here: the SDK classes are constructed
// in process, so a real host's call order is approximated, not observed.

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/au_v2_midi_processor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/host/host_event_timestamps.hpp>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnitSDK/AUPlugInDispatch.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using pulp::format::PlaybackRate;
using pulp::format::PluginDescriptor;
using pulp::format::ProcessTiming;
using pulp::host::HostBlockBoundary;
using pulp::host::HostBlockTimeline;
using pulp::host::HostBlockWindow;
using pulp::host::HostEventResult;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr UInt32 kFrames = 64;
constexpr pulp::state::ParamID kGainId = 1;
constexpr pulp::state::ParamID kBypassId = 2;

// What one Processor call observed. Recorded into fixed storage because the
// adapters run process() under the no-allocation guard.
struct Observed {
    ProcessTiming timing;
    int frames = 0;
    std::int64_t position_samples = 0;
    bool transport_jump = false;
};

struct Recorder {
    std::array<Observed, 256> calls{};
    std::size_t count = 0;
    void record(const pulp::format::ProcessContext& ctx) noexcept {
        if (count < calls.size())
            calls[count++] = {ctx.timing, ctx.num_samples, ctx.position_samples,
                              ctx.transport_jump};
    }
    void clear() noexcept {
        count = 0;
    }
};
Recorder g_recorder;

void define_gain(pulp::state::StateStore& store) {
    store.add_parameter({.id = kGainId, .name = "Gain", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
}

class TimingEffect final : public pulp::format::Processor {
  public:
    // The per-slice render: AUEffectBase calls process() once per slice.
    bool wants_sample_accurate_parameters() const override {
        return true;
    }
    bool wants_au_scheduled_parameter_slices() const override {
        return true;
    }
    PluginDescriptor descriptor() const override {
        return {.name = "AUv2TimingEffect",
                .manufacturer = "PulpTest",
                .bundle_id = "com.pulp.test.au-v2-timing-effect",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"Main In", 2}},
                .output_buses = {{"Main Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore& store) override {
        define_gain(store);
        store.add_parameter({.id = kBypassId, .name = "Bypass", .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext& ctx) override {
        g_recorder.record(ctx);
        for (std::size_t c = 0; c < output.num_channels() && c < input.num_channels(); ++c)
            for (std::size_t n = 0; n < output.num_samples(); ++n)
                output.channel_ptr(c)[n] = input.channel_ptr(c)[n];
    }
};

class TimingMidiEffect final : public pulp::format::Processor {
  public:
    PluginDescriptor descriptor() const override {
        return {.name = "AUv2TimingMidi",
                .manufacturer = "PulpTest",
                .bundle_id = "com.pulp.test.au-v2-timing-midi",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::MidiEffect,
                .input_buses = {},
                .output_buses = {},
                .accepts_midi = true,
                .produces_midi = true};
    }
    void define_parameters(pulp::state::StateStore& store) override {
        define_gain(store);
        store.add_parameter({.id = kBypassId, .name = "Bypass", .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& ctx) override {
        g_recorder.record(ctx);
    }
};

class TimingInstrument final : public pulp::format::Processor {
  public:
    PluginDescriptor descriptor() const override {
        return {.name = "AUv2TimingInstrument",
                .manufacturer = "PulpTest",
                .bundle_id = "com.pulp.test.au-v2-timing-instrument",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Instrument,
                .input_buses = {},
                .output_buses = {{"Main Out", 2}},
                .accepts_midi = true};
    }
    void define_parameters(pulp::state::StateStore& store) override {
        define_gain(store);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& ctx) override {
        g_recorder.record(ctx);
    }
};

template <class P> std::unique_ptr<pulp::format::Processor> make() {
    return std::make_unique<P>();
}

struct ScopedFactory {
    explicit ScopedFactory(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
        g_recorder.clear();
    }
    ~ScopedFactory() {
        pulp::format::register_plugin(previous);
    }
    pulp::format::ProcessorFactory previous;
};

AudioStreamBasicDescription float_format(UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = kSampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

// Host transport, answered through kAudioUnitProperty_HostCallbacks.
struct HostTransport {
    bool installed = true;
    Boolean playing = true;
    Float64 sample = 0.0;
};

OSStatus host_beat_and_tempo(void*, Float64* beat, Float64* tempo) {
    if (beat)
        *beat = 0.0;
    if (tempo)
        *tempo = 120.0;
    return noErr;
}

OSStatus host_transport_state(void* user, Boolean* playing, Boolean* changed, Float64* sample,
                              Boolean* cycling, Float64* cycle_start, Float64* cycle_end) {
    const auto* transport = static_cast<const HostTransport*>(user);
    if (!transport->installed)
        return kAudioUnitErr_CannotDoInCurrentContext;
    if (playing)
        *playing = transport->playing;
    if (changed)
        *changed = false;
    if (sample)
        *sample = transport->sample;
    if (cycling)
        *cycling = false;
    if (cycle_start)
        *cycle_start = 0.0;
    if (cycle_end)
        *cycle_end = 0.0;
    return noErr;
}

void install_host_callbacks(ausdk::AUBase& unit, HostTransport& transport) {
    HostCallbackInfo callbacks{};
    callbacks.hostUserData = &transport;
    callbacks.beatAndTempoProc = host_beat_and_tempo;
    callbacks.transportStateProc = host_transport_state;
    REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0,
                                     &callbacks, sizeof(callbacks)) == noErr);
}

void set_max_frames(ausdk::AUBase& unit) {
    UInt32 max_frames = kFrames;
    REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                     kAudioUnitScope_Global, 0, &max_frames,
                                     sizeof(max_frames)) == noErr);
}

OSStatus fill_input(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                    UInt32 frames, AudioBufferList* io) {
    for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
        auto* dst = static_cast<float*>(io->mBuffers[c].mData);
        for (UInt32 n = 0; n < frames; ++n)
            dst[n] = 0.25f;
    }
    return noErr;
}

AudioTimeStamp time_stamp(Float64 sample_time, bool valid) {
    AudioTimeStamp ts{};
    ts.mFlags = valid ? kAudioTimeStampSampleTimeValid : 0u;
    ts.mSampleTime = sample_time;
    return ts;
}

// The effect brought to the state the SDK dispatch would leave it in: formats,
// block ceiling, host callbacks, a render callback on the main input, then
// Initialize().
struct LiveEffect {
    pulp::format::au::PulpAUEffect unit{nullptr};
    HostTransport transport;
    std::array<float, kFrames> left{}, right{};
    struct List {
        AudioBufferList bl;
        AudioBuffer second;
    } out{};

    LiveEffect() {
        unit.CreateElements();
        const auto format = float_format(2);
        REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                         &format, sizeof(format)) == noErr);
        REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                         &format, sizeof(format)) == noErr);
        set_max_frames(unit);
        AURenderCallbackStruct cb{fill_input, nullptr};
        REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_SetRenderCallback,
                                         kAudioUnitScope_Input, 0, &cb, sizeof(cb)) == noErr);
        install_host_callbacks(unit, transport);
        REQUIRE(unit.DoInitialize() == noErr);
    }
    ~LiveEffect() {
        unit.DoCleanup();
    }

    // Schedule immediate gain events at `offsets`, so the render is cut into
    // slices at each of them.
    void slice_at(std::initializer_list<UInt32> offsets) {
        std::vector<AudioUnitParameterEvent> events;
        float value = 0.1f;
        for (const auto offset : offsets) {
            AudioUnitParameterEvent event{};
            event.eventType = kParameterEvent_Immediate;
            event.parameter = kGainId;
            event.scope = kAudioUnitScope_Global;
            event.eventValues.immediate.bufferOffset = offset;
            event.eventValues.immediate.value = value;
            value += 0.1f;
            events.push_back(event);
        }
        if (!events.empty())
            REQUIRE(unit.ScheduleParameter(events.data(), static_cast<UInt32>(events.size())) ==
                    noErr);
    }

    OSStatus render(Float64 sample_time, bool sample_time_valid = true, UInt32 frames = kFrames) {
        out.bl.mNumberBuffers = 2;
        out.bl.mBuffers[0] = {1, frames * static_cast<UInt32>(sizeof(float)), left.data()};
        out.bl.mBuffers[1] = {1, frames * static_cast<UInt32>(sizeof(float)), right.data()};
        AudioUnitRenderActionFlags flags = 0;
        const auto ts = time_stamp(sample_time, sample_time_valid);
        return unit.DoRender(flags, ts, 0, frames, out.bl);
    }
};

// Every Processor call recorded since `from`, placed on one timeline. Returns
// the (epoch, absolute sample) of every frame delivered, and the boundaries.
struct Projection {
    std::vector<std::pair<std::uint64_t, std::int64_t>> frames;
    std::vector<HostBlockBoundary> boundaries;
};

Projection project(const Observed* calls, std::size_t count) {
    Projection result;
    HostBlockTimeline timeline;
    for (std::size_t i = 0; i < count; ++i) {
        HostBlockWindow window;
        const auto clock = pulp::host::clock_from_process_timing(
            calls[i].timing, static_cast<std::size_t>(calls[i].frames));
        REQUIRE(timeline.advance(clock, window) == HostEventResult::Accepted);
        result.boundaries.push_back(timeline.last_boundary());
        for (std::uint32_t n = 0; n < window.frames; ++n) {
            std::int64_t absolute = 0;
            REQUIRE(timeline.project(window, n, absolute) == HostEventResult::Accepted);
            result.frames.emplace_back(window.epoch, absolute);
        }
    }
    return result;
}

Projection project_recorded() {
    return project(g_recorder.calls.data(), g_recorder.count);
}

std::vector<std::pair<std::uint64_t, std::int64_t>> one_epoch(std::int64_t frames) {
    std::vector<std::pair<std::uint64_t, std::int64_t>> expected;
    for (std::int64_t n = 0; n < frames; ++n)
        expected.emplace_back(1u, n);
    return expected;
}

} // namespace

TEST_CASE("AU v2 effect publishes the render sample time and host transport",
          "[au][auv2][timing]") {
    ScopedFactory factory(make<TimingEffect>);
    LiveEffect fx;

    fx.transport.sample = 1000.0;
    REQUIRE(fx.render(5000.0) == noErr);
    fx.transport.sample = 1064.0;
    REQUIRE(fx.render(5064.0) == noErr);

    REQUIRE(g_recorder.count == 2);
    const auto& first = g_recorder.calls[0].timing;
    REQUIRE(first.discontinuity); // first observed block after Initialize()
    REQUIRE(first.has_steady_sample);
    REQUIRE(first.steady_sample == 5000);
    REQUIRE(first.has_transport_sample);
    REQUIRE(first.transport_sample == 1000);
    REQUIRE(first.transport_moving);
    REQUIRE(first.playback_rate == PlaybackRate::Unknown);
    const auto& second = g_recorder.calls[1].timing;
    REQUIRE_FALSE(second.discontinuity);
    REQUIRE(second.steady_sample == 5064);
    REQUIRE(second.transport_sample == 1064);
    REQUIRE(g_recorder.calls[1].position_samples == second.transport_sample);

    const auto projection = project_recorded();
    REQUIRE(projection.boundaries == std::vector<HostBlockBoundary>{HostBlockBoundary::First,
                                                                    HostBlockBoundary::Continuous});
    REQUIRE(projection.frames == one_epoch(2 * kFrames));

    SECTION("an invalid sample time publishes no counter, and a stopped transport no movement") {
        g_recorder.clear();
        fx.transport.playing = false;
        REQUIRE(fx.render(0.0, false) == noErr);
        REQUIRE(g_recorder.count == 1);
        const auto& timing = g_recorder.calls[0].timing;
        REQUIRE_FALSE(timing.has_steady_sample);
        REQUIRE(timing.has_transport_sample);
        REQUIRE_FALSE(timing.transport_moving);
    }

    SECTION("a host without a transport callback publishes no transport") {
        g_recorder.clear();
        fx.transport.installed = false;
        REQUIRE(fx.render(5128.0) == noErr);
        REQUIRE(g_recorder.count == 1);
        REQUIRE_FALSE(g_recorder.calls[0].timing.has_transport_sample);
        REQUIRE(g_recorder.calls[0].timing.steady_sample == 5128);
    }
}

TEST_CASE("AU v2 effect advances the counter and a playing transport per scheduled slice",
          "[au][auv2][timing][partition]") {
    ScopedFactory factory(make<TimingEffect>);
    LiveEffect fx;

    fx.transport.sample = 1000.0;
    REQUIRE(fx.render(5000.0) == noErr);
    fx.transport.sample = 1064.0;
    fx.slice_at({16, 40});
    REQUIRE(fx.render(5064.0) == noErr);

    REQUIRE(g_recorder.count == 4);
    const std::array<std::int64_t, 3> starts{5064, 5080, 5104};
    const std::array<int, 3> lengths{16, 24, 24};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& call = g_recorder.calls[i + 1];
        INFO("slice " << i);
        REQUIRE(call.frames == lengths[i]);
        REQUIRE(call.timing.has_steady_sample);
        REQUIRE(call.timing.steady_sample == starts[i]);
        REQUIRE(call.timing.transport_sample == starts[i] - 4000);
        REQUIRE(call.position_samples == call.timing.transport_sample);
        REQUIRE_FALSE(call.timing.discontinuity);
        // The legacy seek flag agrees: a slice is not a transport jump.
        REQUIRE_FALSE(call.transport_jump);
    }
    const auto projection = project_recorded();
    REQUIRE(projection.frames == one_epoch(2 * kFrames));
    for (std::size_t i = 1; i < projection.boundaries.size(); ++i)
        REQUIRE(projection.boundaries[i] == HostBlockBoundary::Continuous);

    SECTION("negative control: a slice that does not advance the counter is a new epoch") {
        // The same trace with every slice reporting the render's first frame,
        // which is what publishing the render time stamp per slice would do.
        std::array<Observed, 4> unadvanced{};
        std::copy_n(g_recorder.calls.begin(), 4, unadvanced.begin());
        for (std::size_t i = 2; i < 4; ++i) {
            unadvanced[i].timing.steady_sample = unadvanced[1].timing.steady_sample;
            unadvanced[i].timing.transport_sample = unadvanced[1].timing.transport_sample;
        }
        const auto broken = project(unadvanced.data(), unadvanced.size());
        REQUIRE(broken.boundaries[2] == HostBlockBoundary::NewEpoch);
        REQUIRE(broken.frames != one_epoch(2 * kFrames));
    }

    SECTION("a stopped transport stays where the host left it inside a sliced render") {
        g_recorder.clear();
        fx.transport.playing = false;
        fx.transport.sample = 777.0;
        fx.slice_at({32});
        REQUIRE(fx.render(5128.0) == noErr);
        REQUIRE(g_recorder.count == 2);
        REQUIRE(g_recorder.calls[0].timing.transport_sample == 777);
        REQUIRE(g_recorder.calls[1].timing.transport_sample == 777);
        REQUIRE(g_recorder.calls[1].timing.steady_sample == 5160);
    }
}

TEST_CASE("AU v2 effect slice partitions place every frame at the same absolute sample",
          "[au][auv2][timing][partition]") {
    ScopedFactory factory(make<TimingEffect>);
    const std::vector<std::vector<UInt32>> partitions{
        {}, {32}, {16, 40}, {1, 2, 3, 63}, {8, 16, 24, 32, 40, 48, 56}};

    for (const bool counter : {true, false}) {
        std::vector<std::pair<std::uint64_t, std::int64_t>> reference;
        for (const auto& cuts : partitions) {
            INFO("counter=" << counter << " cuts=" << cuts.size());
            g_recorder.clear();
            LiveEffect fx;
            for (int block = 0; block < 3; ++block) {
                fx.transport.sample = 2000.0 + block * kFrames;
                if (block == 1) {
                    std::vector<AudioUnitParameterEvent> events;
                    for (const auto cut : cuts) {
                        AudioUnitParameterEvent event{};
                        event.eventType = kParameterEvent_Immediate;
                        event.parameter = kGainId;
                        event.scope = kAudioUnitScope_Global;
                        event.eventValues.immediate.bufferOffset = cut;
                        event.eventValues.immediate.value = 0.3f;
                        events.push_back(event);
                    }
                    if (!events.empty())
                        REQUIRE(fx.unit.ScheduleParameter(
                                    events.data(), static_cast<UInt32>(events.size())) == noErr);
                }
                // Without a counter each render still needs a distinct time
                // stamp so the SDK does not treat it as a repeat; the
                // projection uses only the transport then.
                REQUIRE(fx.render(9000.0 + block * kFrames, counter) == noErr);
            }
            REQUIRE(g_recorder.count == 2 + cuts.size() + 1);
            const auto projection = project_recorded();
            REQUIRE(projection.frames == one_epoch(3 * kFrames));
            if (reference.empty())
                reference = projection.frames;
            REQUIRE(projection.frames == reference);
        }
    }
}

TEST_CASE("AU v2 effect reports a discontinuity after Reset and re-initialization",
          "[au][auv2][timing][reset]") {
    ScopedFactory factory(make<TimingEffect>);
    LiveEffect fx;
    fx.transport.playing = false;

    REQUIRE(fx.render(0.0) == noErr);
    REQUIRE(fx.render(64.0) == noErr);
    REQUIRE(fx.unit.Reset(kAudioUnitScope_Global, 0) == noErr);
    REQUIRE(fx.render(128.0) == noErr);
    REQUIRE(fx.render(192.0) == noErr);
    fx.unit.DoCleanup();
    REQUIRE(fx.unit.DoInitialize() == noErr);
    REQUIRE(fx.render(256.0) == noErr);

    REQUIRE(g_recorder.count == 5);
    const std::array<bool, 5> expected{true, false, true, false, true};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("block " << i);
        REQUIRE(g_recorder.calls[i].timing.discontinuity == expected[i]);
    }
    // A reset is a new epoch even though the counter continued.
    const auto projection = project_recorded();
    REQUIRE(projection.boundaries[2] == HostBlockBoundary::NewEpoch);
    REQUIRE(projection.boundaries[3] == HostBlockBoundary::Continuous);
}

TEST_CASE("AU v2 effect marks every skipped render", "[au][auv2][timing][skip]") {
    ScopedFactory factory(make<TimingEffect>);
    LiveEffect fx;
    fx.transport.playing = false;

    SECTION("the stock bypass with a counter: the next block measures the gap") {
        REQUIRE(fx.render(0.0) == noErr);
        fx.unit.SetBypassEffect(true);
        REQUIRE(fx.render(64.0) == noErr);
        fx.unit.SetBypassEffect(false);
        REQUIRE(fx.render(128.0) == noErr);
        REQUIRE(g_recorder.count == 2);
        REQUIRE_FALSE(g_recorder.calls[1].timing.discontinuity);
        const auto projection = project_recorded();
        REQUIRE(projection.boundaries[1] == HostBlockBoundary::Gap);
        REQUIRE(projection.frames.back() == std::pair<std::uint64_t, std::int64_t>{1u, 191});
    }

    SECTION("the stock bypass without a counter: the next block is discontinuous") {
        REQUIRE(fx.render(0.0, false) == noErr);
        fx.unit.SetBypassEffect(true);
        REQUIRE(fx.render(64.0, false) == noErr);
        fx.unit.SetBypassEffect(false);
        REQUIRE(fx.render(128.0, false) == noErr);
        REQUIRE(g_recorder.count == 2);
        REQUIRE(g_recorder.calls[1].timing.discontinuity);
    }

    SECTION("negative control: the same counterless renders without a skip stay continuous") {
        REQUIRE(fx.render(0.0, false) == noErr);
        REQUIRE(fx.render(64.0, false) == noErr);
        REQUIRE(g_recorder.count == 2);
        REQUIRE_FALSE(g_recorder.calls[1].timing.discontinuity);
        // Without the mark the projection cannot see a skip at all.
        REQUIRE(project_recorded().boundaries[1] == HostBlockBoundary::Continuous);
    }

    SECTION("the Bypass parameter skips inside the render; marked the same way") {
        constexpr auto bypass = kBypassId;
        REQUIRE(fx.render(0.0, false) == noErr);
        REQUIRE(fx.unit.SetParameter(bypass, kAudioUnitScope_Global, 0, 1.0f, 0) == noErr);
        REQUIRE(fx.render(64.0, false) == noErr);
        REQUIRE(fx.unit.SetParameter(bypass, kAudioUnitScope_Global, 0, 0.0f, 0) == noErr);
        REQUIRE(fx.render(128.0, false) == noErr);
        REQUIRE(g_recorder.count == 2);
        REQUIRE(g_recorder.calls[1].timing.discontinuity);
    }

    SECTION("a skip does not consume a pending reset") {
        REQUIRE(fx.render(0.0) == noErr);
        REQUIRE(fx.unit.Reset(kAudioUnitScope_Global, 0) == noErr);
        fx.unit.SetBypassEffect(true);
        REQUIRE(fx.render(64.0) == noErr);
        fx.unit.SetBypassEffect(false);
        REQUIRE(fx.render(128.0) == noErr);
        REQUIRE(g_recorder.count == 2);
        REQUIRE(g_recorder.calls[1].timing.discontinuity);
    }
}

namespace {

struct LiveMidiProcessor {
    pulp::format::au::PulpAUMidiProcessor unit{nullptr};
    HostTransport transport;
    LiveMidiProcessor() {
        unit.CreateElements();
        REQUIRE(unit.GetOutput(0)->SetStreamFormat(float_format(2)) == noErr);
        set_max_frames(unit);
        install_host_callbacks(unit, transport);
        REQUIRE(unit.DoInitialize() == noErr);
    }
    ~LiveMidiProcessor() {
        unit.DoCleanup();
    }
    OSStatus render(Float64 sample_time, bool valid = true) {
        AudioUnitRenderActionFlags flags = 0;
        return unit.Render(flags, time_stamp(sample_time, valid), kFrames);
    }
};

struct LiveInstrument {
    pulp::format::au::PulpAUInstrument unit{nullptr};
    HostTransport transport;
    LiveInstrument() {
        unit.CreateElements();
        REQUIRE(unit.GetOutput(0)->SetStreamFormat(float_format(2)) == noErr);
        set_max_frames(unit);
        install_host_callbacks(unit, transport);
        REQUIRE(unit.DoInitialize() == noErr);
    }
    ~LiveInstrument() {
        unit.DoCleanup();
    }
    OSStatus render(Float64 sample_time, bool valid = true) {
        AudioUnitRenderActionFlags flags = 0;
        return unit.Render(flags, time_stamp(sample_time, valid), kFrames);
    }
};

} // namespace

TEST_CASE("AU v2 MIDI processor publishes timing facts and marks bypassed renders",
          "[au][auv2][timing][midi-processor]") {
    ScopedFactory factory(make<TimingMidiEffect>);
    LiveMidiProcessor mp;
    mp.transport.sample = 300.0;

    REQUIRE(mp.render(700.0) == noErr);
    mp.transport.sample = 364.0;
    REQUIRE(mp.render(764.0) == noErr);
    REQUIRE(g_recorder.count == 2);
    REQUIRE(g_recorder.calls[0].timing.discontinuity);
    REQUIRE(g_recorder.calls[0].timing.steady_sample == 700);
    REQUIRE(g_recorder.calls[0].timing.transport_sample == 300);
    REQUIRE(g_recorder.calls[0].timing.transport_moving);
    REQUIRE_FALSE(g_recorder.calls[1].timing.discontinuity);
    REQUIRE(project_recorded().boundaries[1] == HostBlockBoundary::Continuous);

    SECTION("Reset") {
        REQUIRE(mp.unit.Reset(kAudioUnitScope_Global, 0) == noErr);
        mp.transport.sample = 428.0;
        REQUIRE(mp.render(828.0) == noErr);
        REQUIRE(g_recorder.calls[2].timing.discontinuity);
    }

    SECTION("a bypassed render without a counter makes the next block discontinuous") {
        mp.transport.playing = false;
        REQUIRE(mp.unit.SetParameter(kBypassId, kAudioUnitScope_Global, 0, 1.0f, 0) == noErr);
        REQUIRE(mp.render(0.0, false) == noErr);
        REQUIRE(mp.unit.SetParameter(kBypassId, kAudioUnitScope_Global, 0, 0.0f, 0) == noErr);
        REQUIRE(mp.render(0.0, false) == noErr);
        REQUIRE(g_recorder.count == 3);
        REQUIRE(g_recorder.calls[2].timing.discontinuity);
    }

    SECTION("a bypassed render with a counter is a gap, not a discontinuity") {
        REQUIRE(mp.unit.SetParameter(kBypassId, kAudioUnitScope_Global, 0, 1.0f, 0) == noErr);
        REQUIRE(mp.render(828.0) == noErr);
        REQUIRE(mp.unit.SetParameter(kBypassId, kAudioUnitScope_Global, 0, 0.0f, 0) == noErr);
        mp.transport.sample = 492.0;
        REQUIRE(mp.render(892.0) == noErr);
        REQUIRE(g_recorder.count == 3);
        REQUIRE_FALSE(g_recorder.calls[2].timing.discontinuity);
        REQUIRE(project_recorded().boundaries[2] == HostBlockBoundary::Gap);
    }

    SECTION("an oversized render is a skip") {
        mp.transport.playing = false;
        AudioUnitRenderActionFlags flags = 0;
        REQUIRE(mp.unit.Render(flags, time_stamp(0.0, false), kFrames * 2) ==
                kAudioUnitErr_TooManyFramesToProcess);
        REQUIRE(mp.render(0.0, false) == noErr);
        REQUIRE(g_recorder.count == 3);
        REQUIRE(g_recorder.calls[2].timing.discontinuity);
    }
}

TEST_CASE("AU v2 instrument publishes timing facts and a discontinuity after Reset",
          "[au][auv2][timing][instrument]") {
    ScopedFactory factory(make<TimingInstrument>);
    LiveInstrument inst;
    inst.transport.sample = 48000.0;

    REQUIRE(inst.render(96000.0) == noErr);
    inst.transport.sample = 48064.0;
    REQUIRE(inst.render(96064.0) == noErr);
    REQUIRE(inst.unit.Reset(kAudioUnitScope_Global, 0) == noErr);
    inst.transport.sample = 48128.0;
    REQUIRE(inst.render(96128.0) == noErr);

    REQUIRE(g_recorder.count == 3);
    REQUIRE(g_recorder.calls[0].timing.discontinuity);
    REQUIRE(g_recorder.calls[0].timing.steady_sample == 96000);
    REQUIRE(g_recorder.calls[0].timing.transport_sample == 48000);
    REQUIRE_FALSE(g_recorder.calls[1].timing.discontinuity);
    REQUIRE(g_recorder.calls[2].timing.discontinuity);
    const auto projection = project_recorded();
    REQUIRE(projection.boundaries == std::vector<HostBlockBoundary>{HostBlockBoundary::First,
                                                                    HostBlockBoundary::Continuous,
                                                                    HostBlockBoundary::NewEpoch});
}
