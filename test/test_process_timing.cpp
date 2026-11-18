// Neutral per-block timing facts (format::ProcessTiming) and their projection
// onto the host event timeline: the per-format builders, the shared AU event
// placement, oversized-block chunking, and LV2 varispeed.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/host_sample_position.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/format/process_timing.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/host_event_timestamps.hpp>

#include <clap/fixedpoint.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#if __has_include(<pluginterfaces/vst/ivstprocesscontext.h>)
#include <pluginterfaces/vst/ivstprocesscontext.h>
#define PULP_TIMING_TEST_HAS_VST3 1
#endif

using namespace pulp;
using format::PlaybackRate;
using format::ProcessTiming;
using host::HostBlockBoundary;
using host::HostBlockClock;
using host::HostBlockTimeline;
using host::HostBlockWindow;
using host::HostEventIngress;
using host::HostEventRecord;
using host::HostEventResult;

namespace {

// The CLAP sectime a host derives from an integer sample position.
std::int64_t host_sectime_for(std::int64_t sample, double sample_rate) {
    const long double seconds = static_cast<long double>(sample) / sample_rate;
    return static_cast<std::int64_t>(std::llround(seconds * CLAP_SECTIME_FACTOR));
}

midi::MidiEvent note(std::uint8_t key) {
    return midi::MidiEvent::note_on(0, key, 100);
}

} // namespace

TEST_CASE("timing mirrors equal the SDK constants", "[process-timing][format]") {
    STATIC_REQUIRE(format::kClapSectimeFactor == CLAP_SECTIME_FACTOR);
#if defined(PULP_TIMING_TEST_HAS_VST3)
    STATIC_REQUIRE(format::kVst3ContextPlaying ==
                   static_cast<std::uint32_t>(Steinberg::Vst::ProcessContext::kPlaying));
    STATIC_REQUIRE(format::kVst3ContextContTimeValid ==
                   static_cast<std::uint32_t>(Steinberg::Vst::ProcessContext::kContTimeValid));
#endif
    // CoreAudioTypes: kAudioTimeStampSampleTimeValid = (1U << 0).
    STATIC_REQUIRE(format::kAuTimeStampSampleTimeValid == 1u);
    STATIC_REQUIRE(host::kAuImmediateWindow == format::detail::kAuImmediateWindow);
}

TEST_CASE("CLAP fixed-point seconds convert back to the exact host sample",
          "[process-timing][format][clap]") {
    const std::array<double, 4> rates{44100.0, 48000.0, 88200.0, 96000.0};
    // Positions across a day of audio, including ones whose seconds value is
    // not a terminating binary fraction at 44.1 kHz.
    const std::array<std::int64_t, 8> samples{0, 1, 63, 441, 44099, 13823, 158760000, 4147200000};
    for (const double rate : rates) {
        for (const auto sample : samples) {
            const auto decoded =
                format::clap_sectime_to_samples(host_sectime_for(sample, rate), rate);
            INFO("rate " << rate << " sample " << sample);
            REQUIRE(decoded.has_value());
            REQUIRE(*decoded == sample);
        }
    }
    // A run of consecutive 64-frame blocks never rounds to a non-contiguous
    // position, so a seconds-only CLAP transport does not read as a seek.
    for (std::int64_t start = 0; start < 64 * 1000; start += 64) {
        const auto a = format::clap_sectime_to_samples(host_sectime_for(start, 44100.0), 44100.0);
        const auto b =
            format::clap_sectime_to_samples(host_sectime_for(start + 64, 44100.0), 44100.0);
        REQUIRE((a && b));
        REQUIRE(*b - *a == 64);
    }
    // Refusals: no sample rate, or a position past int64.
    REQUIRE_FALSE(format::clap_sectime_to_samples(CLAP_SECTIME_FACTOR, 0.0).has_value());
    REQUIRE_FALSE(format::clap_sectime_to_samples(CLAP_SECTIME_FACTOR, -1.0).has_value());
    REQUIRE_FALSE(format::clap_sectime_to_samples(std::numeric_limits<std::int64_t>::max(), 1.0e12)
                      .has_value());
    // Negative control: truncating instead of rounding loses a sample at
    // positions whose fixed-point seconds land a hair below the integer.
    int truncation_losses = 0;
    for (std::int64_t sample = 1; sample < 2000; ++sample) {
        const double seconds = static_cast<double>(host_sectime_for(sample, 44100.0)) /
                               static_cast<double>(CLAP_SECTIME_FACTOR);
        if (static_cast<std::int64_t>(seconds * 44100.0) != sample)
            ++truncation_losses;
    }
    REQUIRE(truncation_losses > 0);
}

TEST_CASE("format builders publish only what the host supplied", "[process-timing][format]") {
    SECTION("CLAP") {
        const auto none = format::clap_process_timing(-1, std::nullopt, true, false);
        REQUIRE_FALSE(none.has_steady_sample);
        REQUIRE_FALSE(none.has_transport_sample);
        REQUIRE_FALSE(none.transport_moving);
        const auto full = format::clap_process_timing(4096, std::int64_t{256}, true, true);
        REQUIRE(full.has_steady_sample);
        REQUIRE(full.steady_sample == 4096);
        REQUIRE(full.transport_sample == 256);
        REQUIRE(full.transport_moving);
        REQUIRE(full.playback_rate == PlaybackRate::Unknown);
        REQUIRE(full.discontinuity);
    }
    SECTION("VST3") {
        REQUIRE(format::vst3_process_timing(false, ~0u, 5, 5, false, false) == ProcessTiming{});
        const auto no_counter =
            format::vst3_process_timing(true, format::kVst3ContextPlaying, 100, 9, false, false);
        REQUIRE_FALSE(no_counter.has_steady_sample);
        REQUIRE(no_counter.has_transport_sample);
        REQUIRE(no_counter.transport_moving);
        const auto negative_counter = format::vst3_process_timing(
            true, format::kVst3ContextContTimeValid, 100, -1, false, false);
        REQUIRE_FALSE(negative_counter.has_steady_sample);
        const auto varispeed = format::vst3_process_timing(
            true, format::kVst3ContextPlaying | format::kVst3ContextContTimeValid, 100, 9, true,
            false);
        REQUIRE(varispeed.steady_sample == 9);
        REQUIRE(varispeed.playback_rate == PlaybackRate::NonUnit);
        REQUIRE_FALSE(varispeed.transport_tracks_samples());
    }
    SECTION("AU") {
        const auto rounded = format::au_process_timing(
            format::kAuTimeStampSampleTimeValid, 13823.9999, true, true, 511.9999, false, false);
        REQUIRE(rounded.steady_sample == 13824);
        REQUIRE(rounded.transport_sample == 512);
        const auto invalid = format::au_process_timing(0, 13824.0, false, true, 0.0, false, false);
        REQUIRE_FALSE(invalid.has_steady_sample);
        REQUIRE_FALSE(invalid.has_transport_sample);
        const auto nan = format::au_process_timing(format::kAuTimeStampSampleTimeValid, NAN, true,
                                                   true, INFINITY, false, false);
        REQUIRE_FALSE(nan.has_steady_sample);
        REQUIRE_FALSE(nan.has_transport_sample);
    }
    SECTION("LV2") {
        REQUIRE(format::lv2_process_timing(false, 10, true, 1.0, false) == ProcessTiming{});
        const auto stopped = format::lv2_process_timing(true, 10, true, 0.0, false);
        REQUIRE(stopped.has_transport_sample);
        REQUIRE_FALSE(stopped.transport_moving);
        const auto unit = format::lv2_process_timing(true, 10, true, 1.0, false);
        REQUIRE(unit.playback_rate == PlaybackRate::Unit);
        REQUIRE(unit.transport_tracks_samples());
        const auto half = format::lv2_process_timing(true, 10, true, 0.5, false);
        REQUIRE(half.transport_moving);
        REQUIRE(half.playback_rate == PlaybackRate::NonUnit);
        REQUIRE_FALSE(half.transport_tracks_samples());
        const auto reverse = format::lv2_process_timing(true, 10, true, -1.0, false);
        REQUIRE(reverse.playback_rate == PlaybackRate::NonUnit);
        REQUIRE_FALSE(format::lv2_process_timing(true, 10, false, 1.0, false).transport_moving);
    }
}

TEST_CASE("a context without adapter timing projects as an unclocked block",
          "[process-timing][host]") {
    format::ProcessContext ctx;
    ctx.num_samples = 64;
    // The legacy transport fields alone are not timing facts.
    ctx.position_samples = 1000;
    ctx.is_playing = true;
    const auto clock = host::clock_from_process_context(ctx);
    REQUIRE(clock.frames == 64);
    REQUIRE(clock.steady_sample < 0);
    REQUIRE_FALSE(clock.has_transport_sample);
    REQUIRE_FALSE(clock.discontinuity);

    ctx.num_samples = 0;
    HostBlockTimeline timeline;
    HostBlockWindow window;
    REQUIRE(host::advance_from_context(timeline, ctx, window) == HostEventResult::OutOfRange);
    ctx.num_samples = -5;
    REQUIRE(host::clock_from_process_context(ctx).frames == 0);
}

TEST_CASE("the AU raw offset is shared by the clamping and the refusing policy",
          "[process-timing][format][au]") {
    constexpr std::int64_t kImmediate = host::kAuEventSampleTimeImmediate;
    constexpr std::uint32_t kFrames = 64;
    host::AuBlockTiming timing;
    timing.timestamp_flags = host::kAuTimeStampSampleTimeValid;
    timing.sample_time = 1000.0;
    timing.frames = kFrames;

    struct Case {
        std::int64_t event;
        std::optional<std::int64_t> raw;
    };
    const std::array<Case, 6> cases{{
        {kImmediate + 5, 5},
        {1010, 10},
        {1000 + kFrames - 1, kFrames - 1},
        {1000 + kFrames, kFrames}, // one past the block
        {990, -10},                // before the block
        {kImmediate + 4095, 4095}, // last immediate slot
    }};
    for (const auto& c : cases) {
        INFO("event " << c.event);
        const auto raw = format::detail::au_event_raw_offset(c.event, kImmediate, 1000.0);
        REQUIRE(raw == c.raw);
        const auto legacy =
            format::detail::au_event_block_offset(c.event, kImmediate, 1000.0, kFrames);
        std::uint32_t block_time = 0;
        const auto refused = host::au_event_block_time(c.event, timing, block_time);
        if (*c.raw >= 0 && *c.raw < kFrames) {
            REQUIRE(refused == HostEventResult::Accepted);
            REQUIRE(block_time == static_cast<std::uint32_t>(*c.raw));
            REQUIRE(legacy == *c.raw);
        } else {
            // Same placement, different policy: the host refuses, the legacy
            // MIDI path moves the event to the nearest block edge.
            REQUIRE(refused == HostEventResult::OutOfRange);
            REQUIRE(legacy == (*c.raw < 0 ? 0 : static_cast<std::int32_t>(kFrames - 1)));
        }
    }
    // Without a valid block sample time an absolute event cannot be placed:
    // the host refuses it, the legacy path keeps its old fallback.
    REQUIRE_FALSE(format::detail::au_event_raw_offset(1010, kImmediate, std::nullopt));
    REQUIRE(format::detail::au_event_block_offset(30, kImmediate, std::nullopt, kFrames) == 30);
    timing.timestamp_flags = 0;
    std::uint32_t block_time = 0;
    REQUIRE(host::au_event_block_time(30, timing, block_time) == HostEventResult::OutOfRange);
    // Overflow of the difference is unplaceable.
    REQUIRE_FALSE(format::detail::au_event_raw_offset(std::numeric_limits<std::int64_t>::min() + 1,
                                                      kImmediate, 1.0e18));
}

TEST_CASE("the host AU event timeline admits exactly what the AU adapter admits",
          "[process-timing][format][au][parity]") {
    constexpr std::int64_t kImmediate = host::kAuEventSampleTimeImmediate;
    constexpr std::int64_t kInt32Max = std::numeric_limits<std::int32_t>::max();
    constexpr std::uint32_t kMaxFrames = std::numeric_limits<std::uint32_t>::max();

    struct Case {
        std::int64_t event;
        std::uint32_t frames;
        bool sample_time_valid;
        bool accepted;
    };
    // The block starts at sample 0, so an absolute time is its own offset.
    const std::array<Case, 11> cases{{
        {0, 64, true, true},                       // first frame
        {63, 64, true, true},                      // last frame
        {64, 64, true, false},                     // at the end
        {-1, 64, true, false},                     // before the block
        {kImmediate + 5, 64, true, true},          // immediate window
        {kImmediate + 64, 64, true, false},        // immediate, past the end
        {5, 64, false, false},                     // absolute, no block time
        {0, 0, true, false},                       // empty block
        {kInt32Max, kMaxFrames, true, true},       // largest deliverable offset
        {kInt32Max + 1, kMaxFrames, true, false},  // in the block, not an int32
        {kMaxFrames - 1, kMaxFrames, true, false}, // last frame, not an int32
    }};
    for (const auto& c : cases) {
        INFO("event " << c.event << " frames " << c.frames << " valid " << c.sample_time_valid);
        host::AuBlockTiming timing;
        timing.timestamp_flags = c.sample_time_valid ? host::kAuTimeStampSampleTimeValid : 0u;
        timing.sample_time = 0.0;
        timing.frames = c.frames;

        format::EventTimeAdmission admission{static_cast<std::int64_t>(c.frames)};
        const auto adapter = format::admit_au_event_time(
            admission, c.event, kImmediate,
            c.sample_time_valid ? std::optional<double>{0.0} : std::nullopt);
        REQUIRE(adapter.has_value() == c.accepted);

        std::uint32_t block_time = 0xDEADBEEFu;
        const auto result = host::au_event_block_time(c.event, timing, block_time);
        REQUIRE((result == HostEventResult::Accepted) == c.accepted);
        if (c.accepted)
            REQUIRE(block_time == static_cast<std::uint32_t>(*adapter));
        else
            REQUIRE(block_time == 0xDEADBEEFu); // a refusal writes nothing
    }
}

TEST_CASE("an oversized host block is refused whole and accepted in chunks",
          "[process-timing][host][chunk]") {
    constexpr std::size_t kHostFrames = 10000;
    HostBlockClock block;
    block.steady_sample = 50000;
    block.frames = kHostFrames;
    block.has_transport_sample = true;
    block.transport_sample = 7000;
    block.transport_playing = true;
    block.discontinuity = true;

    HostBlockTimeline whole;
    HostBlockWindow window;
    REQUIRE(whole.advance(block, window) == HostEventResult::OutOfRange);

    // Events at block-relative times across the whole host block.
    const std::array<std::uint32_t, 6> event_times{0, 4095, 4096, 8191, 8192, 9999};

    // Reference: the same samples delivered by the host as three blocks.
    std::vector<std::int64_t> reference;
    {
        HostBlockTimeline timeline;
        const std::array<std::size_t, 3> sizes{4096, 4096, 1808};
        std::size_t offset = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            HostBlockClock clock = block;
            clock.frames = sizes[i];
            clock.steady_sample = block.steady_sample + static_cast<std::int64_t>(offset);
            clock.transport_sample = block.transport_sample + static_cast<std::int64_t>(offset);
            clock.discontinuity = i == 0;
            REQUIRE(timeline.advance(clock, window) == HostEventResult::Accepted);
            for (const auto t : event_times) {
                if (t < offset || t >= offset + sizes[i])
                    continue;
                std::int64_t absolute = 0;
                REQUIRE(timeline.project(window, static_cast<std::uint32_t>(t - offset),
                                         absolute) == HostEventResult::Accepted);
                reference.push_back(absolute);
            }
            offset += sizes[i];
        }
    }

    // Chunked: one host block split by host_block_chunk.
    std::vector<std::int64_t> chunked;
    std::vector<HostBlockBoundary> boundaries;
    HostBlockTimeline timeline;
    std::size_t offset = 0;
    while (offset < kHostFrames) {
        const auto frames = std::min(host::kHostBlockMaxFrames, kHostFrames - offset);
        HostBlockClock chunk;
        REQUIRE(host::host_block_chunk(block, offset, frames, chunk) == HostEventResult::Accepted);
        REQUIRE(chunk.discontinuity == (offset == 0));
        REQUIRE(timeline.advance(chunk, window) == HostEventResult::Accepted);
        boundaries.push_back(timeline.last_boundary());
        for (const auto t : event_times) {
            if (t < offset || t >= offset + frames)
                continue;
            std::int64_t absolute = 0;
            REQUIRE(timeline.project(window, static_cast<std::uint32_t>(t - offset), absolute) ==
                    HostEventResult::Accepted);
            chunked.push_back(absolute);
        }
        offset += frames;
    }
    REQUIRE(chunked == reference);
    REQUIRE(chunked == std::vector<std::int64_t>{0, 4095, 4096, 8191, 8192, 9999});
    REQUIRE(boundaries == std::vector<HostBlockBoundary>{HostBlockBoundary::First,
                                                         HostBlockBoundary::Continuous,
                                                         HostBlockBoundary::Continuous});

    // A stopped transport stays where the host left it in every chunk.
    HostBlockClock stopped = block;
    stopped.transport_playing = false;
    HostBlockClock chunk;
    REQUIRE(host::host_block_chunk(stopped, 4096, 4096, chunk) == HostEventResult::Accepted);
    REQUIRE(chunk.transport_sample == stopped.transport_sample);
    REQUIRE(chunk.steady_sample == stopped.steady_sample + 4096);

    // Refusals leave the output untouched.
    HostBlockClock untouched;
    untouched.frames = 77;
    REQUIRE(host::host_block_chunk(block, 0, 0, untouched) == HostEventResult::OutOfRange);
    REQUIRE(host::host_block_chunk(block, 0, 4097, untouched) == HostEventResult::OutOfRange);
    REQUIRE(host::host_block_chunk(block, 8192, 1809, untouched) == HostEventResult::OutOfRange);
    REQUIRE(host::host_block_chunk(block, 10001, 1, untouched) == HostEventResult::OutOfRange);
    HostBlockClock huge = block;
    huge.steady_sample = std::numeric_limits<std::int64_t>::max() - 10;
    REQUIRE(host::host_block_chunk(huge, 4096, 16, untouched) == HostEventResult::OutOfRange);
    REQUIRE(untouched.frames == 77);
}

TEST_CASE("LV2 varispeed playback is not read as a seek on every block",
          "[process-timing][host][lv2]") {
    // The host plays at half speed: time:frame advances by half a block.
    constexpr std::uint32_t kFrames = 64;
    auto run = [&](bool honour_rate) {
        HostBlockTimeline timeline;
        HostBlockWindow window;
        std::vector<HostBlockBoundary> boundaries;
        std::int64_t frame = 1000;
        for (int block = 0; block < 6; ++block) {
            format::ProcessContext ctx;
            ctx.num_samples = kFrames;
            ctx.timing = format::lv2_process_timing(true, frame, true, 0.5, block == 0);
            if (!honour_rate)
                ctx.timing.playback_rate = PlaybackRate::Unknown; // planted: rate dropped
            REQUIRE(host::advance_from_context(timeline, ctx, window) == HostEventResult::Accepted);
            boundaries.push_back(timeline.last_boundary());
            frame += kFrames / 2;
        }
        return boundaries;
    };
    const auto honoured = run(true);
    REQUIRE(honoured.front() == HostBlockBoundary::First);
    for (std::size_t i = 1; i < honoured.size(); ++i)
        REQUIRE(honoured[i] == HostBlockBoundary::Continuous);
    // Negative control: without the rate fact every block reads as a seek.
    const auto dropped = run(false);
    for (std::size_t i = 1; i < dropped.size(); ++i)
        REQUIRE(dropped[i] == HostBlockBoundary::NewEpoch);

    // At unit speed a relocation is still caught.
    HostBlockTimeline timeline;
    HostBlockWindow window;
    format::ProcessContext ctx;
    ctx.num_samples = kFrames;
    ctx.timing = format::lv2_process_timing(true, 0, true, 1.0, false);
    REQUIRE(host::advance_from_context(timeline, ctx, window) == HostEventResult::Accepted);
    ctx.timing = format::lv2_process_timing(true, kFrames, true, 1.0, false);
    REQUIRE(host::advance_from_context(timeline, ctx, window) == HostEventResult::Accepted);
    REQUIRE(timeline.last_boundary() == HostBlockBoundary::Continuous);
    ctx.timing = format::lv2_process_timing(true, 9000, true, 1.0, false);
    REQUIRE(host::advance_from_context(timeline, ctx, window) == HostEventResult::Accepted);
    REQUIRE(timeline.last_boundary() == HostBlockBoundary::NewEpoch);
}

TEST_CASE("records built from adapter offsets refuse invalid offsets and stale windows",
          "[process-timing][host]") {
    HostBlockTimeline timeline;
    HostBlockWindow first;
    format::ProcessContext ctx;
    ctx.num_samples = 32;
    ctx.timing = format::clap_process_timing(0, std::int64_t{0}, true, true);
    REQUIRE(host::advance_from_context(timeline, ctx, first) == HostEventResult::Accepted);

    HostEventRecord record;
    record.absolute_sample = -99;
    REQUIRE(host::make_record_from_block_offset(timeline, first, 31, 1, 1, note(60), record) ==
            HostEventResult::Accepted);
    REQUIRE(record.absolute_sample == 31);
    REQUIRE(record.midi.sample_offset == 31);
    HostEventRecord untouched;
    untouched.absolute_sample = -99;
    REQUIRE(host::make_record_from_block_offset(timeline, first, 32, 1, 2, note(60), untouched) ==
            HostEventResult::OutOfRange);
    REQUIRE(host::make_record_from_block_offset(timeline, first, -1, 1, 2, note(60), untouched) ==
            HostEventResult::OutOfRange);
    REQUIRE(untouched.absolute_sample == -99);

    // Seek: the next block opens a new epoch, and the old window is stale.
    ctx.timing = format::clap_process_timing(32, std::int64_t{48000}, true, false);
    HostBlockWindow second;
    REQUIRE(host::advance_from_context(timeline, ctx, second) == HostEventResult::Accepted);
    REQUIRE(timeline.last_boundary() == HostBlockBoundary::NewEpoch);
    REQUIRE(host::make_record_from_block_offset(timeline, first, 0, 1, 3, note(60), untouched) ==
            HostEventResult::StaleEpoch);
    // A record for the retired epoch is refused by the ingress too.
    HostEventIngress<8> ingress;
    REQUIRE(ingress.begin_block(second.epoch, second.sample_start, second.frames) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(record) == HostEventResult::StaleEpoch);
}

TEST_CASE("event times are classified against the rendered frames",
          "[process-timing][format][offsets]") {
    using format::classify_event_time;
    using format::EventTimePlacement;
    constexpr std::int64_t kFrames = 64;
    STATIC_REQUIRE(classify_event_time(0, kFrames) == EventTimePlacement::InBlock);
    STATIC_REQUIRE(classify_event_time(kFrames - 1, kFrames) == EventTimePlacement::InBlock);
    STATIC_REQUIRE(classify_event_time(kFrames, kFrames) == EventTimePlacement::AtOrPastEnd);
    STATIC_REQUIRE(classify_event_time(-1, kFrames) == EventTimePlacement::BeforeBlock);
    STATIC_REQUIRE(classify_event_time(std::numeric_limits<std::int64_t>::max(), kFrames) ==
                   EventTimePlacement::AtOrPastEnd);
    STATIC_REQUIRE(classify_event_time(std::numeric_limits<std::int64_t>::min(), kFrames) ==
                   EventTimePlacement::BeforeBlock);
    // A block of zero frames holds no event.
    STATIC_REQUIRE(classify_event_time(0, 0) == EventTimePlacement::AtOrPastEnd);
    // A time in another unit is never read as frames, even when its bytes
    // would fall inside the block.
    STATIC_REQUIRE(classify_event_time(false, 5, kFrames) == EventTimePlacement::WrongUnit);
    STATIC_REQUIRE(classify_event_time(true, 5, kFrames) == EventTimePlacement::InBlock);
}

TEST_CASE("event-time admission delivers in-block offsets unchanged and counts the rest",
          "[process-timing][format][offsets]") {
    format::EventTimeAdmission admission{64};
    REQUIRE(admission.admit(0) == std::optional<std::int32_t>{0});
    REQUIRE(admission.admit(63) == std::optional<std::int32_t>{63});
    REQUIRE(admission.refused == 0);
    REQUIRE_FALSE(admission.admit(64));
    REQUIRE_FALSE(admission.admit(-1));
    REQUIRE_FALSE(admission.admit(std::int64_t{1} << 40));
    REQUIRE_FALSE(admission.admit(false, 3));
    REQUIRE(admission.refused == 4);

    // An offset that is in the block but would not fit the delivered int32 is
    // refused rather than truncated.
    format::EventTimeAdmission huge{std::numeric_limits<std::int64_t>::max()};
    REQUIRE(huge.admit(std::numeric_limits<std::int32_t>::max()) ==
            std::optional<std::int32_t>{std::numeric_limits<std::int32_t>::max()});
    REQUIRE_FALSE(huge.admit(std::int64_t{std::numeric_limits<std::int32_t>::max()} + 1));
    REQUIRE(huge.refused == 1);

    // The count saturates instead of wrapping to zero.
    format::EventTimeAdmission full{8, std::numeric_limits<std::uint32_t>::max()};
    REQUIRE_FALSE(full.admit(8));
    REQUIRE(full.refused == std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("AU render event times are placed before they are admitted",
          "[process-timing][au][offsets]") {
    constexpr std::int64_t kImmediate = host::kAuEventSampleTimeImmediate;
    constexpr std::uint32_t kFrames = 512;
    format::EventTimeAdmission admission{kFrames};
    // Immediate window: already an offset, with or without a block time.
    REQUIRE(format::admit_au_event_time(admission, kImmediate + 7, kImmediate, std::nullopt) ==
            std::optional<std::int32_t>{7});
    // Positive sample times are absolute on the render axis. Only the
    // negative AUEventSampleTimeImmediate window is an offset without a
    // block sample time, so the same numeric offset is refused here.
    REQUIRE_FALSE(format::admit_au_event_time(admission, 9, kImmediate, std::nullopt));
    REQUIRE(format::admit_au_event_time(admission, kImmediate + 9, kImmediate, std::nullopt) ==
            std::optional<std::int32_t>{9});
    REQUIRE(admission.refused == 1);
    // Absolute: placed against the rounded block time (REAPER-style 13823.9999).
    REQUIRE(format::admit_au_event_time(admission, 13829, kImmediate, 13823.9999) ==
            std::optional<std::int32_t>{5});
    REQUIRE(format::admit_au_event_time(admission, 13824 + kFrames - 1, kImmediate, 13824.0) ==
            std::optional<std::int32_t>{kFrames - 1});
    REQUIRE(admission.refused == 1);
    // Planted negatives: at the end, before the block, and absolute with no
    // block time to place it against. None is clamped to an edge.
    REQUIRE_FALSE(format::admit_au_event_time(admission, 13824 + kFrames, kImmediate, 13824.0));
    REQUIRE_FALSE(format::admit_au_event_time(admission, 13800, kImmediate, 13824.0));
    REQUIRE_FALSE(format::admit_au_event_time(admission, 9, kImmediate, std::nullopt));
    REQUIRE(admission.refused == 4);
}

TEST_CASE("an LV2 sequence is read as frames only in frame units",
          "[process-timing][lv2][offsets]") {
    constexpr std::uint32_t kFrameTime = 17;
    constexpr std::uint32_t kBeatTime = 18;
    STATIC_REQUIRE(format::lv2_sequence_unit_is_frames(0, kFrameTime));
    STATIC_REQUIRE(format::lv2_sequence_unit_is_frames(kFrameTime, kFrameTime));
    STATIC_REQUIRE_FALSE(format::lv2_sequence_unit_is_frames(kBeatTime, kFrameTime));
    // An unmapped frameTime URID matches only the unit 0.
    STATIC_REQUIRE(format::lv2_sequence_unit_is_frames(0, 0));
    STATIC_REQUIRE_FALSE(format::lv2_sequence_unit_is_frames(kBeatTime, 0));
}
