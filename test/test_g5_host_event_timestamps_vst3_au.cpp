// VST3 and Audio Unit timing inputs mapped onto HostBlockTimeline.
//
// The mapping helpers take plain fields; where the VST3 SDK or AudioToolbox
// headers are available the mirrored constants and field correspondences are
// checked against the real SDK types.

#include <pulp/host/host_event_timestamps.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#if __has_include(<pluginterfaces/vst/ivstprocesscontext.h>)
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#define PULP_TEST_HAS_VST3_SDK 1
#endif

// AUEventSampleTimeImmediate is declared only for Objective-C, so a C++ test
// can check the AudioTimeStamp side against the SDK but not that constant.
#if defined(__APPLE__) && __has_include(<CoreAudioTypes/CoreAudioBaseTypes.h>)
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#define PULP_TEST_HAS_AU_SDK 1
#endif

using pulp::host::AuBlockTiming;
using pulp::host::HostBlockBoundary;
using pulp::host::HostBlockClock;
using pulp::host::HostBlockTimeline;
using pulp::host::HostBlockWindow;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;
using pulp::host::Vst3BlockTiming;

namespace {

constexpr std::uint32_t kVst3Producer = 5;
constexpr std::uint32_t kAuProducer = 6;

Vst3BlockTiming vst3_block(std::int64_t continuous, std::int64_t project, bool playing,
                           std::int32_t frames) {
    Vst3BlockTiming t;
    t.has_process_context = true;
    t.state =
        pulp::host::kVst3ContextContTimeValid | (playing ? pulp::host::kVst3ContextPlaying : 0u);
    t.continuous_time_samples = continuous;
    t.project_time_samples = project;
    t.num_samples = frames;
    return t;
}

AuBlockTiming au_block(double sample_time, std::uint32_t frames, bool moving = false,
                       double position = 0.0, bool has_transport = false) {
    AuBlockTiming t;
    t.timestamp_flags = pulp::host::kAuTimeStampSampleTimeValid;
    t.sample_time = sample_time;
    t.frames = frames;
    t.has_transport = has_transport;
    t.transport_moving = moving;
    t.transport_sample_position = position;
    return t;
}

HostBlockBoundary step(HostBlockTimeline& timeline, const HostBlockClock& clock,
                       HostBlockWindow& window) {
    REQUIRE(timeline.advance(clock, window) == HostEventResult::Accepted);
    return timeline.last_boundary();
}

std::vector<std::int64_t> drain(HostEventIngress<64>& ingress) {
    std::array<HostEventRecord, 64> out{};
    std::size_t count = 0;
    REQUIRE(ingress.project(out, count) == HostEventResult::Accepted);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
    std::vector<std::int64_t> samples;
    for (std::size_t i = 0; i < count; ++i)
        samples.push_back(out[i].absolute_sample);
    return samples;
}

} // namespace

// ---------------------------------------------------------------------------
// SDK correspondence
// ---------------------------------------------------------------------------

#if defined(PULP_TEST_HAS_VST3_SDK)
static_assert(pulp::host::kVst3ContextPlaying == Steinberg::Vst::ProcessContext::kPlaying);
static_assert(pulp::host::kVst3ContextContTimeValid ==
              Steinberg::Vst::ProcessContext::kContTimeValid);

TEST_CASE("VST3 ProcessContext fields map onto the block clock", "[graph][g5][vst3]") {
    Steinberg::Vst::ProcessContext pc{};
    pc.state = Steinberg::Vst::ProcessContext::kPlaying |
               Steinberg::Vst::ProcessContext::kContTimeValid |
               Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
    pc.projectTimeSamples = 44100;
    pc.continousTimeSamples = 900000;

    Vst3BlockTiming timing;
    timing.has_process_context = true;
    timing.state = pc.state;
    timing.project_time_samples = pc.projectTimeSamples;
    timing.continuous_time_samples = pc.continousTimeSamples;
    timing.num_samples = 128;
    const auto clock = pulp::host::clock_from_vst3(timing);
    CHECK(clock.steady_sample == 900000);
    CHECK(clock.frames == 128);
    CHECK(clock.has_transport_sample);
    CHECK(clock.transport_sample == 44100);
    CHECK(clock.transport_playing);

    // kProjectTimeMusicValid covers the beat position only; projectTimeSamples
    // is still the transport position without it.
    timing.state = Steinberg::Vst::ProcessContext::kContTimeValid;
    const auto stopped = pulp::host::clock_from_vst3(timing);
    CHECK(stopped.has_transport_sample);
    CHECK(stopped.transport_sample == 44100);
    CHECK_FALSE(stopped.transport_playing);

    // A VST3 event's sampleOffset converts through the signed-offset helper.
    Steinberg::Vst::Event ev{};
    ev.sampleOffset = 17;
    std::uint32_t block_time = 0;
    CHECK(pulp::host::host_block_time_from_offset(ev.sampleOffset, block_time) ==
          HostEventResult::Accepted);
    CHECK(block_time == 17);
}
#endif

#if defined(PULP_TEST_HAS_AU_SDK)
static_assert(pulp::host::kAuTimeStampSampleTimeValid == kAudioTimeStampSampleTimeValid);

TEST_CASE("AudioTimeStamp fields map onto the block clock", "[graph][g5][au]") {
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
    ts.mSampleTime = 13823.9999; // REAPER-style value a hair below the integer
    AuBlockTiming timing;
    timing.timestamp_flags = ts.mFlags;
    timing.sample_time = ts.mSampleTime;
    timing.frames = 64;
    timing.has_transport = true;
    timing.transport_moving = true;
    timing.transport_sample_position = 2047.9999;
    const auto clock = pulp::host::clock_from_au(timing);
    CHECK(clock.steady_sample == 13824);
    CHECK(clock.frames == 64);
    CHECK(clock.has_transport_sample);
    CHECK(clock.transport_sample == 2048);
    CHECK(clock.transport_playing);
}
#endif

// ---------------------------------------------------------------------------
// VST3
// ---------------------------------------------------------------------------

TEST_CASE("VST3 timing validity decides whether a steady clock exists", "[graph][g5][vst3]") {
    using pulp::host::clock_from_vst3;

    auto timing = vst3_block(5000, 100, true, 64);
    CHECK(clock_from_vst3(timing).steady_sample == 5000);

    timing.state &= ~pulp::host::kVst3ContextContTimeValid;
    CHECK(clock_from_vst3(timing).steady_sample == -1);

    auto negative = vst3_block(-3, 100, true, 64);
    CHECK(clock_from_vst3(negative).steady_sample == -1);

    Vst3BlockTiming no_context;
    no_context.num_samples = 64;
    no_context.continuous_time_samples = 777;
    no_context.state = pulp::host::kVst3ContextContTimeValid | pulp::host::kVst3ContextPlaying;
    const auto bare = clock_from_vst3(no_context);
    CHECK(bare.steady_sample == -1);
    CHECK_FALSE(bare.has_transport_sample);
    CHECK_FALSE(bare.transport_playing);
    CHECK(bare.frames == 64);

    // A non-positive numSamples maps to zero frames, which advance() refuses.
    HostBlockTimeline timeline;
    HostBlockWindow window;
    CHECK(clock_from_vst3(vst3_block(0, 0, false, -1)).frames == 0);
    CHECK(timeline.advance(clock_from_vst3(vst3_block(0, 0, false, -1)), window) ==
          HostEventResult::OutOfRange);
}

TEST_CASE("VST3 blocks classify contiguous, gap, seek, and stopped", "[graph][g5][vst3]") {
    using pulp::host::clock_from_vst3;
    HostBlockTimeline timeline;
    HostBlockWindow window;

    SECTION("contiguous playing blocks") {
        CHECK(step(timeline, clock_from_vst3(vst3_block(1000, 0, true, 64)), window) ==
              HostBlockBoundary::First);
        CHECK(step(timeline, clock_from_vst3(vst3_block(1064, 64, true, 64)), window) ==
              HostBlockBoundary::Continuous);
        CHECK(window.sample_start == 64);
    }

    SECTION("playing forward gap with matching transport delta") {
        step(timeline, clock_from_vst3(vst3_block(1000, 500, true, 64)), window);
        CHECK(step(timeline, clock_from_vst3(vst3_block(1300, 800, true, 64)), window) ==
              HostBlockBoundary::Gap);
        CHECK(window.epoch == 1);
        CHECK(window.sample_start == 300);
    }

    SECTION("cycle wrap: counter continues, project time jumps back") {
        step(timeline, clock_from_vst3(vst3_block(1000, 4000, true, 64)), window);
        CHECK(step(timeline, clock_from_vst3(vst3_block(1064, 2000, true, 64)), window) ==
              HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
        CHECK(window.sample_start == 0);
    }

    SECTION("seek: transport delta differs from counter delta") {
        step(timeline, clock_from_vst3(vst3_block(1000, 500, true, 64)), window);
        CHECK(step(timeline, clock_from_vst3(vst3_block(1300, 9000, true, 64)), window) ==
              HostBlockBoundary::NewEpoch);
    }

    SECTION("stopped transport relocating is never compared") {
        step(timeline, clock_from_vst3(vst3_block(1000, 500, false, 64)), window);
        CHECK(step(timeline, clock_from_vst3(vst3_block(1064, 9000, false, 64)), window) ==
              HostBlockBoundary::Continuous);
        CHECK(window.epoch == 1);
    }

    SECTION("kContTimeValid missing behaves like a host without a steady clock") {
        auto first = vst3_block(1000, 500, true, 64);
        first.state &= ~pulp::host::kVst3ContextContTimeValid;
        auto second = vst3_block(99999, 564, true, 64);
        second.state &= ~pulp::host::kVst3ContextContTimeValid;
        step(timeline, clock_from_vst3(first), window);
        // Without a counter the playing transport must advance by the block.
        CHECK(step(timeline, clock_from_vst3(second), window) == HostBlockBoundary::Continuous);
        auto skipped = vst3_block(0, 900, true, 64);
        skipped.state &= ~pulp::host::kVst3ContextContTimeValid;
        CHECK(step(timeline, clock_from_vst3(skipped), window) == HostBlockBoundary::NewEpoch);
        // Gaining the counter mid-stream is a clock-source change.
        CHECK(step(timeline, clock_from_vst3(vst3_block(5000, 964, true, 64)), window) ==
              HostBlockBoundary::NewEpoch);
    }

    SECTION("adapter-signalled discontinuity") {
        step(timeline, clock_from_vst3(vst3_block(1000, 500, true, 64)), window);
        auto restart = vst3_block(1064, 564, true, 64);
        restart.discontinuity = true;
        CHECK(step(timeline, clock_from_vst3(restart), window) == HostBlockBoundary::NewEpoch);
    }
}

TEST_CASE("VST3 sampleOffsets outside the block are refused", "[graph][g5][vst3]") {
    HostBlockTimeline timeline;
    HostBlockWindow window;
    step(timeline, pulp::host::clock_from_vst3(vst3_block(0, 0, true, 64)), window);

    for (const std::int32_t offset : {-1, std::numeric_limits<std::int32_t>::min()}) {
        std::uint32_t block_time = 1234;
        CHECK(pulp::host::host_block_time_from_offset(offset, block_time) ==
              HostEventResult::OutOfRange);
        CHECK(block_time == 1234);
    }
    for (const std::int32_t offset : {64, 65, std::numeric_limits<std::int32_t>::max()}) {
        std::uint32_t block_time = 0;
        REQUIRE(pulp::host::host_block_time_from_offset(offset, block_time) ==
                HostEventResult::Accepted);
        std::int64_t absolute = -9;
        CHECK(timeline.project(window, block_time, absolute) == HostEventResult::OutOfRange);
        CHECK(absolute == -9);
    }
    std::uint32_t last = 0;
    REQUIRE(pulp::host::host_block_time_from_offset(63, last) == HostEventResult::Accepted);
    std::int64_t absolute = 0;
    CHECK(timeline.project(window, last, absolute) == HostEventResult::Accepted);
    CHECK(absolute == 63);
}

// ---------------------------------------------------------------------------
// Audio Unit
// ---------------------------------------------------------------------------

TEST_CASE("AU timestamp validity decides whether a steady clock exists", "[graph][g5][au]") {
    using pulp::host::clock_from_au;

    CHECK(clock_from_au(au_block(4096.0, 64)).steady_sample == 4096);
    CHECK(clock_from_au(au_block(4095.6, 64)).steady_sample == 4096);

    auto invalid = au_block(4096.0, 64);
    invalid.timestamp_flags = 0;
    CHECK(clock_from_au(invalid).steady_sample == -1);

    CHECK(clock_from_au(au_block(std::nan(""), 64)).steady_sample == -1);
    CHECK(clock_from_au(au_block(1e300, 64)).steady_sample == -1);
    CHECK(clock_from_au(au_block(-128.0, 64)).steady_sample == -1);

    // Transport is used only when the host callback succeeded.
    const auto no_cb = clock_from_au(au_block(0.0, 64, true, 500.0, false));
    CHECK_FALSE(no_cb.has_transport_sample);
    CHECK_FALSE(no_cb.transport_playing);
    const auto with_cb = clock_from_au(au_block(0.0, 64, true, 500.0, true));
    CHECK(with_cb.has_transport_sample);
    CHECK(with_cb.transport_sample == 500);
    CHECK(with_cb.transport_playing);
    const auto bad_pos = clock_from_au(au_block(0.0, 64, true, std::nan(""), true));
    CHECK_FALSE(bad_pos.has_transport_sample);
}

TEST_CASE("AU blocks classify contiguous, gap, loop, and stopped", "[graph][g5][au]") {
    using pulp::host::clock_from_au;
    HostBlockTimeline timeline;
    HostBlockWindow window;

    SECTION("contiguous") {
        step(timeline, clock_from_au(au_block(256.0, 128, true, 0.0, true)), window);
        CHECK(step(timeline, clock_from_au(au_block(384.0, 128, true, 128.0, true)), window) ==
              HostBlockBoundary::Continuous);
        CHECK(window.sample_start == 128);
    }

    SECTION("playing forward gap with matching transport delta") {
        step(timeline, clock_from_au(au_block(256.0, 128, true, 0.0, true)), window);
        CHECK(step(timeline, clock_from_au(au_block(640.0, 128, true, 384.0, true)), window) ==
              HostBlockBoundary::Gap);
        CHECK(window.epoch == 1);
        CHECK(window.sample_start == 384);
    }

    SECTION("cycling host wraps the position while mSampleTime keeps running") {
        step(timeline, clock_from_au(au_block(256.0, 128, true, 8000.0, true)), window);
        CHECK(step(timeline, clock_from_au(au_block(384.0, 128, true, 1000.0, true)), window) ==
              HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
        CHECK(window.sample_start == 0);
    }

    SECTION("stopped transport relocating is never compared") {
        step(timeline, clock_from_au(au_block(256.0, 128, false, 0.0, true)), window);
        CHECK(step(timeline, clock_from_au(au_block(384.0, 128, false, 70000.0, true)), window) ==
              HostBlockBoundary::Continuous);
    }

    SECTION("an invalid sample time behaves like a host without a steady clock") {
        auto first = au_block(0.0, 128, true, 0.0, true);
        first.timestamp_flags = 0;
        auto second = au_block(0.0, 128, true, 128.0, true);
        second.timestamp_flags = 0;
        step(timeline, clock_from_au(first), window);
        CHECK(step(timeline, clock_from_au(second), window) == HostBlockBoundary::Continuous);
        auto skipped = au_block(0.0, 128, true, 1000.0, true);
        skipped.timestamp_flags = 0;
        CHECK(step(timeline, clock_from_au(skipped), window) == HostBlockBoundary::NewEpoch);
        CHECK(step(timeline, clock_from_au(au_block(5000.0, 128, true, 1128.0, true)), window) ==
              HostBlockBoundary::NewEpoch);
    }

    SECTION("backwards mSampleTime") {
        step(timeline, clock_from_au(au_block(1000.0, 128)), window);
        CHECK(step(timeline, clock_from_au(au_block(900.0, 128)), window) ==
              HostBlockBoundary::NewEpoch);
    }
}

TEST_CASE("AUv3 event sample times convert without clamping", "[graph][g5][au]") {
    using pulp::host::au_event_block_time;
    using pulp::host::kAuEventSampleTimeImmediate;
    const auto block = au_block(10000.0, 64);
    std::uint32_t t = 999;

    CHECK(au_event_block_time(kAuEventSampleTimeImmediate, block, t) == HostEventResult::Accepted);
    CHECK(t == 0);
    CHECK(au_event_block_time(kAuEventSampleTimeImmediate + 63, block, t) ==
          HostEventResult::Accepted);
    CHECK(t == 63);
    CHECK(au_event_block_time(10000, block, t) == HostEventResult::Accepted);
    CHECK(t == 0);
    CHECK(au_event_block_time(10063, block, t) == HostEventResult::Accepted);
    CHECK(t == 63);

    t = 999;
    for (const std::int64_t refused :
         {std::int64_t{9999}, std::int64_t{10064}, kAuEventSampleTimeImmediate + 64,
          kAuEventSampleTimeImmediate - 1, std::numeric_limits<std::int64_t>::min(),
          std::numeric_limits<std::int64_t>::max()}) {
        CHECK(au_event_block_time(refused, block, t) == HostEventResult::OutOfRange);
        CHECK(t == 999);
    }

    // An absolute time cannot be placed without a valid block sample time.
    auto untimed = block;
    untimed.timestamp_flags = 0;
    CHECK(au_event_block_time(10005, untimed, t) == HostEventResult::OutOfRange);
    CHECK(au_event_block_time(kAuEventSampleTimeImmediate + 5, untimed, t) ==
          HostEventResult::Accepted);
    CHECK(t == 5);

    // Rounded block start, as the adapters round mSampleTime.
    CHECK(au_event_block_time(10000, au_block(9999.9999, 64), t) == HostEventResult::Accepted);
    CHECK(t == 0);
}

// ---------------------------------------------------------------------------
// Partition invariance through the ingress
// ---------------------------------------------------------------------------

TEST_CASE("VST3 and AU absolute samples are invariant to block partitioning",
          "[graph][g5][vst3][au]") {
    constexpr std::int64_t kOrigin = 48000;
    constexpr std::int64_t kTransportOrigin = 2000;
    constexpr std::int64_t kTotal = 1024;
    const std::vector<std::int64_t> positions{0, 1, 31, 32, 63, 64, 126, 127, 128, 255, 500, 1023};

    enum class Format { Vst3, Au };
    auto render = [&](Format format, std::size_t block) {
        HostBlockTimeline timeline;
        HostEventIngress<64> ingress;
        ingress.reset(1);
        std::uint64_t sequence = 0;
        std::vector<std::int64_t> projected;
        for (std::int64_t at = 0; at < kTotal;) {
            const auto frames =
                std::min<std::int64_t>(static_cast<std::int64_t>(block), kTotal - at);
            HostBlockClock clock;
            AuBlockTiming au;
            if (format == Format::Vst3) {
                clock = pulp::host::clock_from_vst3(vst3_block(
                    kOrigin + at, kTransportOrigin + at, true, static_cast<std::int32_t>(frames)));
            } else {
                au = au_block(static_cast<double>(kOrigin + at), static_cast<std::uint32_t>(frames),
                              true, static_cast<double>(kTransportOrigin + at), true);
                clock = pulp::host::clock_from_au(au);
            }
            HostBlockWindow window;
            REQUIRE(timeline.advance(clock, window) == HostEventResult::Accepted);
            CHECK(window.epoch == 1);
            REQUIRE(ingress.begin_block(window.epoch, window.sample_start, window.frames) ==
                    HostEventResult::Accepted);
            for (const auto pos : positions) {
                if (pos < at || pos >= at + frames)
                    continue;
                std::uint32_t block_time = 0;
                if (format == Format::Vst3) {
                    const auto sample_offset = static_cast<std::int32_t>(pos - at);
                    REQUIRE(pulp::host::host_block_time_from_offset(sample_offset, block_time) ==
                            HostEventResult::Accepted);
                } else {
                    // AUv3 render events carry absolute mSampleTime-axis times.
                    REQUIRE(pulp::host::au_event_block_time(kOrigin + pos, au, block_time) ==
                            HostEventResult::Accepted);
                }
                HostEventRecord record;
                REQUIRE(timeline.make_record(window, block_time,
                                             format == Format::Vst3 ? kVst3Producer : kAuProducer,
                                             sequence++,
                                             pulp::midi::MidiEvent::note_on(
                                                 0, static_cast<std::uint8_t>(pos % 128), 100),
                                             record) == HostEventResult::Accepted);
                REQUIRE(ingress.publish(record) == HostEventResult::Accepted);
            }
            const auto samples = drain(ingress);
            projected.insert(projected.end(), samples.begin(), samples.end());
            at += frames;
        }
        return projected;
    };

    for (const auto format : {Format::Vst3, Format::Au}) {
        const auto at32 = render(format, 32);
        CHECK(at32 == positions);
        CHECK(render(format, 64) == at32);
        CHECK(render(format, 127) == at32);
    }
}
