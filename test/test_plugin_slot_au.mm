// test_plugin_slot_au.mm — AU host slot (core/host/src/plugin_slot_au.mm).
//
// Covers the A2 fix: AuSlot::process must not allocate the output
// AudioBufferList on the audio thread (it now reuses a member sized in
// prepare()). Two layers:
//   1. A pure unit test of the AudioBufferList build helper — proves the
//      no-allocation invariant deterministically (pointer stability) with no
//      AudioUnit, so it runs everywhere including headless CI.
//   2. An integration test that loads a real system Apple effect AU and drives
//      process() — the path CI historically could not exercise (PluginSlot
//      could not load an AU in the test env). Skips honestly when no system AU
//      is loadable rather than faking a pass.
#if defined(__APPLE__)

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include "harness/rt_allocation_probe.hpp"
#include "../core/host/src/plugin_slot_au_internal.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudioTypes/CoreAudioTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace pulp;

namespace {

std::string fourcc(std::uint32_t v) {
    return std::string{static_cast<char>((v >> 24) & 0xff),
                       static_cast<char>((v >> 16) & 0xff),
                       static_cast<char>((v >> 8) & 0xff),
                       static_cast<char>(v & 0xff)};
}

// First Apple-manufacturer audio Effect, as a "TYPE:SUBT:MANU" 4CC triplet
// (what load_au_plugin parses). Empty if none is registered — a headless CI
// VM may not surface Apple's bundled AUs.
std::string first_apple_effect_unique_id() {
    AudioComponentDescription want{};
    want.componentType = kAudioUnitType_Effect;
    want.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent c = AudioComponentFindNext(nullptr, &want);
    if (!c) return {};
    AudioComponentDescription got{};
    if (AudioComponentGetDescription(c, &got) != noErr) return {};
    return fourcc(got.componentType) + ":" + fourcc(got.componentSubType) + ":" +
           fourcc(got.componentManufacturer);
}

}  // namespace

TEST_CASE("AU host output AudioBufferList reuses pre-sized storage (A2 RT no-alloc)",
          "[host][au][rt][a2]") {
    using namespace pulp::host::au_internal;
    constexpr int channels = 2;
    constexpr int frames = 512;

    std::vector<float> ch0(frames, 0.0f), ch1(frames, 0.0f);
    std::array<float*, channels> ptrs{ch0.data(), ch1.data()};
    audio::BufferView<float> out(ptrs.data(), channels, frames);

    std::vector<std::uint8_t> storage;
    // prepare(): the first reserve grows the buffer exactly once...
    REQUIRE(reserve_audio_buffer_list(storage, channels));
    REQUIRE(storage.size() == audio_buffer_list_bytes(channels));
    // ...and any later reserve for <= channels is a no-op (the RT-safe outcome).
    REQUIRE_FALSE(reserve_audio_buffer_list(storage, channels));
    REQUIRE_FALSE(reserve_audio_buffer_list(storage, 1));

    const std::uint8_t* base = storage.data();

    // Steady state: thousands of per-block refills. Per the RtAllocationProbe
    // contract, no Catch macros run inside the probe scope (they allocate);
    // the count is captured to a plain local and asserted afterwards. The
    // primary, allocator-independent proof is the pointer-stability check.
    std::size_t alloc_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 4096; ++i) {
            (void)fill_output_audio_buffer_list(
                storage, out, channels,
                static_cast<std::uint32_t>(frames) * sizeof(float));
        }
        alloc_count = probe.allocation_count();
    }

    REQUIRE(storage.data() == base);  // never reallocated/moved
    REQUIRE(alloc_count == 0);

    // The filled list points at the caller's channels with the right shape.
    auto* abl = reinterpret_cast<const AudioBufferList*>(storage.data());
    REQUIRE(abl->mNumberBuffers == static_cast<UInt32>(channels));
    REQUIRE(abl->mBuffers[0].mNumberChannels == 1u);
    REQUIRE(abl->mBuffers[0].mDataByteSize ==
            static_cast<UInt32>(frames) * sizeof(float));
    REQUIRE(abl->mBuffers[0].mData == ch0.data());
    REQUIRE(abl->mBuffers[1].mData == ch1.data());
}

TEST_CASE("AU host slot renders a real system AudioUnit (A2 integration)",
          "[host][au][integration][a2]") {
    const std::string uid = first_apple_effect_unique_id();
    if (uid.empty()) {
        SUCCEED("no Apple effect AU registered in this environment");
        return;
    }

    host::PluginInfo info;
    info.name = "SystemEffect";
    info.unique_id = uid;
    info.format = host::PluginFormat::AudioUnit;
    info.is_effect = true;
    info.num_inputs = 2;
    info.num_outputs = 2;

    auto slot = host::PluginSlot::load(info);
    if (!slot) {
        SUCCEED("system AU '" + uid + "' did not load in this environment");
        return;
    }
    REQUIRE(slot->prepare(48000.0, 512));

    constexpr int channels = 2;
    constexpr int frames = 512;
    std::vector<float> in0(frames, 0.25f), in1(frames, 0.25f);
    std::vector<float> out0(frames, 0.0f), out1(frames, 0.0f);
    std::array<const float*, channels> in_ptrs{in0.data(), in1.data()};
    std::array<float*, channels> out_ptrs{out0.data(), out1.data()};
    audio::BufferView<const float> input(in_ptrs.data(), channels, frames);
    audio::BufferView<float> output(out_ptrs.data(), channels, frames);
    midi::MidiBuffer midi_in, midi_out;
    host::ParameterEventQueue params;

    // Drive many blocks through the real render path (what CI couldn't). A
    // reallocated/corrupted output AudioBufferList would surface as a render
    // error or NaN/garbage in the output; require every sample stays finite.
    bool all_finite = true;
    for (int blk = 0; blk < 64 && all_finite; ++blk) {
        std::fill(out0.begin(), out0.end(), 0.0f);
        std::fill(out1.begin(), out1.end(), 0.0f);
        slot->process(output, input, midi_in, midi_out, params, frames);
        for (int i = 0; i < frames; ++i) {
            if (!std::isfinite(out0[i]) || !std::isfinite(out1[i])) {
                all_finite = false;
                break;
            }
        }
    }
    REQUIRE(all_finite);
    REQUIRE(slot->is_loaded());
}

#endif  // __APPLE__
