// AU v2 instrument (aumu) render-path RT-safety guard.
//
// Task 1.3 (planning/2026-07-08-remaining-open-items-implementation-plan.md
// §1.3): the AU-v2 instrument adapter wraps `Processor::process()` in a
// `pulp::runtime::ScopedNoAlloc` inside `PulpAUInstrument::Render`
// (au_v2_instrument.cpp). This test drives the real MusicDeviceBase render
// entry point for one steady-state block and asserts the audio callback
// neither allocates nor takes a blocking lock.
//
// Mechanism (see test/harness/scoped_rt_process_probe.hpp): built with
// PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1 and linked against the trap TU
// (test/native_components/rt_intercept_test_support.cpp), `ScopedRtProcessProbe`
// enters an always-on RtNoAllocScope whose strong operator-new / pthread-lock
// overrides ABORT the process if an allocation or blocking lock is attempted in
// scope. A regression that made the instrument render path allocate (or removed
// the production guard so a locking Processor::process slipped through) would
// SIGABRT here.
//
// The first render call is warm-up: the SDK output element lazily allocates its
// IO buffer inside PrepareBuffer and `output_ptrs_` resizes once. Those are the
// adapter's one-time setup allocations and live OUTSIDE the probe scope, exactly
// as they would on a host's first audio callback. The measured call is the
// steady-state block, where the render path must be allocation/lock-free.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#import <AudioToolbox/AudioToolbox.h>

#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;

namespace {

// Minimal RT-safe instrument: no audio input, stereo output, one param. Its
// process() writes a constant to every output sample — real per-sample work
// that touches no heap and takes no lock, so a clean render path passes and a
// regression that allocates inside the guarded scope trips the trap.
class RtInstrumentProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "AUInstrumentRtTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-instrument-rt",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Level",
            .unit = "",
            .range = {0.0f, 1.0f, 0.5f, 0.01f},
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float level = state().get_value(1);
        for (std::size_t c = 0; c < output.num_channels(); ++c) {
            float* dst = output.channel_ptr(c);
            for (std::size_t n = 0; n < output.num_samples(); ++n) {
                dst[n] = level;
            }
        }
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& context) override {
        auto* out = audio.main_output();
        audio::BufferView<const float> empty_input;
        if (out) {
            process(*out, empty_input, midi_in, midi_out, context);
        }
    }
};

std::unique_ptr<format::Processor> create_rt_instrument() {
    return std::make_unique<RtInstrumentProcessor>();
}

// Scoped swap of the global processor factory (mirrors test_au_plugin_state.mm).
struct ScopedFactoryRegistration {
    explicit ScopedFactoryRegistration(format::ProcessorFactory factory)
        : previous(format::registered_factory()) {
        format::register_plugin(factory);
    }
    ~ScopedFactoryRegistration() { format::register_plugin(previous); }
    format::ProcessorFactory previous;
};

// Canonical non-interleaved 32-bit float stream format.
AudioStreamBasicDescription make_float_format(double sample_rate,
                                              UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                       kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

}  // namespace

TEST_CASE("AU v2 instrument Render path is allocation/lock-free",
          "[au][au-v2][instrument][rt-safety]") {
    ScopedFactoryRegistration registration(create_rt_instrument);

    constexpr double kSampleRate = 48000.0;
    constexpr UInt32 kChannels = 2;
    constexpr UInt32 kFrames = 256;

    format::au::PulpAUInstrument instrument(nullptr);

    // A directly-constructed AUBase never gets the SDK dispatch's
    // PostConstructorInternal(), so create the I/O elements ourselves before
    // touching the output bus (host construction does this implicitly).
    instrument.CreateElements();

    // Configure the output element format and the render-block ceiling, then
    // initialize (prepares the Processor at the element's rate/width).
    REQUIRE(instrument.GetOutput(0)->SetStreamFormat(
                make_float_format(kSampleRate, kChannels)) == noErr);
    UInt32 max_frames = kFrames;
    REQUIRE(instrument.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                           kAudioUnitScope_Global, 0, &max_frames,
                                           sizeof(max_frames)) == noErr);
    // DoInitialize (not the bare virtual Initialize) runs the adapter's
    // Initialize() AND ReallocateBuffers() + flips the initialized flag, so the
    // output element's IO buffer is allocated at max_frames and Render is
    // permitted — this is the sequence the SDK dispatch runs for a host.
    REQUIRE(instrument.DoInitialize() == noErr);

    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp timestamp{};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    timestamp.mSampleTime = 0;

    // Warm-up: first render lazily allocates the SDK IO buffer + output_ptrs_
    // (adapter one-time setup, outside the RT contract). A second warm-up call
    // proves those allocations are not repeated.
    REQUIRE(instrument.Render(flags, timestamp, kFrames) == noErr);
    REQUIRE(instrument.Render(flags, timestamp, kFrames) == noErr);

    // Measured steady-state block: the whole Render body — MIDI drain, context
    // build, and the guarded Processor::process — must not allocate or lock.
    OSStatus render_status = noErr;
    std::size_t allocation_count = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        render_status = instrument.Render(flags, timestamp, kFrames);
        allocation_count = probe.allocation_count();
    }
    REQUIRE(render_status == noErr);
    REQUIRE(allocation_count == 0);

    instrument.DoCleanup();
}
