#import <AudioToolbox/AudioToolbox.h>

#include "../core/format/src/projection_capability.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/signal_graph.hpp>

#import "../core/format/src/au_audio_unit.h"

#include <array>
#include <cmath>
#include <memory>

namespace {

std::unique_ptr<pulp::format::Processor> create_baked_au_processor() {
    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(1, "Output");
    if (!graph.connect(input, 0, gain, 0) || !graph.connect(gain, 0, output, 0) ||
        !graph.set_node_gain(gain, 0.5f) || !graph.prepare(48000.0, 64))
        return nullptr;

    auto lowered = pulp::host::bake(graph);
    return std::move(lowered.processor);
}

AudioStreamBasicDescription mono_float_format() {
    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;
    return format;
}

struct ScopedFactoryRegistration {
    pulp::format::ProcessorFactory previous;
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }
    ~ScopedFactoryRegistration() {
        pulp::format::register_plugin(previous);
    }
};

} // namespace

TEST_CASE("DSPX-07 admits AU only for bounded baked processors", "[dspx][au][capability]") {
    using namespace pulp::format;
    REQUIRE(projection_capability(ProjectionSurface::au, true, true).supported());
    REQUIRE_FALSE(projection_capability(ProjectionSurface::au, false, true).supported());
    REQUIRE_FALSE(projection_capability(ProjectionSurface::au, true, false).supported());
}

TEST_CASE("DSPX-07 baked processor reaches the real AU v2 render path",
          "[dspx][au][auv2][lifecycle]") {
    ScopedFactoryRegistration registration(create_baked_au_processor);
    constexpr UInt32 frames = 64;
    const auto format = mono_float_format();

    pulp::format::au::PulpAUEffect effect(nullptr);
    effect.CreateElements();
    REQUIRE(effect.Input(0).SetStreamFormat(format) == noErr);
    REQUIRE(effect.Output(0).SetStreamFormat(format) == noErr);
    UInt32 max_frames = frames;
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global, 0, &max_frames,
                                       sizeof(max_frames)) == noErr);
    REQUIRE(effect.DoInitialize() == noErr);
    REQUIRE(effect.GetLatency() == 0.0);

    std::array<float, frames> input{};
    std::array<float, frames> output{};
    for (UInt32 i = 0; i < frames; ++i)
        input[i] = static_cast<float>(i + 1) / static_cast<float>(frames);
    AudioBufferList input_list{};
    input_list.mNumberBuffers = 1;
    input_list.mBuffers[0] = {1, sizeof(input), input.data()};
    AudioBufferList output_list{};
    output_list.mNumberBuffers = 1;
    output_list.mBuffers[0] = {1, sizeof(output), output.data()};
    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(effect.ProcessBufferLists(flags, input_list, output_list, frames) == noErr);
    for (UInt32 i = 0; i < frames; ++i)
        REQUIRE(output[i] == Catch::Approx(input[i] * 0.5f));

    // The adapter's ordinary AU reset boundary remains usable for the baked
    // processor. Reinitialize and render a second block to prove no hidden
    // graph/source ownership is required after the first lifecycle.
    effect.Reset(kAudioUnitScope_Global, 0);
    output.fill(0.0f);
    REQUIRE(effect.ProcessBufferLists(flags, input_list, output_list, frames) == noErr);
    for (UInt32 i = 0; i < frames; ++i)
        REQUIRE(std::isfinite(output[i]));
    effect.DoCleanup();
}

TEST_CASE("DSPX-07 baked processor reaches the real AU v3 render path",
          "[dspx][au][auv3][lifecycle]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_baked_au_processor);
        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Effect;
        description.componentSubType = 'DspX';
        description.componentManufacturer = 'Plup';
        NSError* error = nil;
        PulpAudioUnit* unit = [[PulpAudioUnit alloc] initWithComponentDescription:description
                                                                          options:0
                                                                            error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);
        unit.maximumFramesToRender = 64;
        NSError* allocation_error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&allocation_error]);
        REQUIRE(allocation_error == nil);
        REQUIRE(unit.latency == 0.0);

        std::array<float, 64> output{};
        AudioBufferList output_list{};
        output_list.mNumberBuffers = 1;
        output_list.mBuffers[0] = {1, sizeof(output), output.data()};
        std::array<float, 64> input{};
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<float>(i + 1) / static_cast<float>(input.size());

        AUInternalRenderBlock render = [unit internalRenderBlock];
        REQUIRE(render != nil);
        AURenderPullInputBlock pull = ^AUAudioUnitStatus(
            AudioUnitRenderActionFlags*, const AudioTimeStamp*, AUAudioFrameCount frame_count,
            NSInteger, AudioBufferList* input_data) {
          REQUIRE(frame_count == input.size());
          REQUIRE(input_data != nullptr);
          input_data->mNumberBuffers = 1;
          input_data->mBuffers[0].mNumberChannels = 1;
          input_data->mBuffers[0].mDataByteSize = sizeof(input);
          input_data->mBuffers[0].mData = const_cast<float*>(input.data());
          return noErr;
        };
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        REQUIRE(render(&flags, &timestamp, input.size(), 0, &output_list, nil, pull) == noErr);
        for (std::size_t i = 0; i < output.size(); ++i)
            REQUIRE(output[i] == Catch::Approx(input[i] * 0.5f));

        [unit deallocateRenderResources];
        [unit release];
    }
}
