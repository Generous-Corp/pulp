#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace pulp::signal;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

RealtimePitchTimeConfig stream_config(int max_block) {
    RealtimePitchTimeConfig config;
    config.mode = PitchTimeMode::time_stretch;
    config.quality = PitchTimeQuality::low_latency;
    config.channels = 1;
    config.max_block = max_block;
    return config;
}

std::vector<float> sine(double frequency, double amplitude, int frames) {
    std::vector<float> samples(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame)
        samples[static_cast<std::size_t>(frame)] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * frame / kSampleRate));
    return samples;
}

void advance_to_final_buffer(RealtimePitchTimeProcessor& processor) {
    std::vector<float> discarded(256);
    float* output[] = {discarded.data()};
    for (int guard = 0; guard < 64; ++guard) {
        const int before = processor.available_stretched();
        const auto status = processor.finalize();
        REQUIRE(status != PitchTimeStreamFinalizeStatus::complete);
        if (status == PitchTimeStreamFinalizeStatus::draining
            && processor.available_stretched() > 0
            && processor.available_stretched() == before)
            return;
        if (status == PitchTimeStreamFinalizeStatus::backpressure) {
            const int count = std::min(processor.available_stretched(), 256);
            REQUIRE(count > 0);
            REQUIRE(processor.read_stretched(output, count) == count);
        }
    }
    FAIL("finite stream did not reach its final buffered output");
}

} // namespace

TEST_CASE("RealtimePitchTimeProcessor rejects non-positive prepared block capacity",
          "[signal][pitch-time][streaming]") {
    for (const int invalid : {0, -1}) {
        RealtimePitchTimeProcessor processor;
        REQUIRE(processor.prepare(kSampleRate, stream_config(invalid))
                == PitchTimePrepareStatus::invalid_max_block);
    }
}

TEST_CASE("RealtimePitchTimeProcessor partial EOF read preserves the real prefix",
          "[signal][pitch-time][streaming]") {
    RealtimePitchTimeProcessor exact;
    RealtimePitchTimeProcessor partial;
    REQUIRE(exact.prepare(kSampleRate, stream_config(256)) == PitchTimePrepareStatus::prepared);
    REQUIRE(partial.prepare(kSampleRate, stream_config(256)) == PitchTimePrepareStatus::prepared);

    const auto input = sine(367.0, 0.5, 513);
    for (std::size_t offset = 0; offset < input.size();) {
        const int count = static_cast<int>(std::min<std::size_t>(256, input.size() - offset));
        const float* chunk[] = {input.data() + offset};
        REQUIRE(exact.feed(chunk, count) == PitchTimeStreamFeedStatus::accepted);
        REQUIRE(partial.feed(chunk, count) == PitchTimeStreamFeedStatus::accepted);
        offset += static_cast<std::size_t>(count);
    }

    advance_to_final_buffer(exact);
    advance_to_final_buffer(partial);
    const int available = exact.available_stretched();
    REQUIRE(available > 0);
    REQUIRE(partial.available_stretched() == available);

    std::vector<float> expected(static_cast<std::size_t>(available));
    float* expected_output[] = {expected.data()};
    REQUIRE(exact.read_stretched(expected_output, available) == available);

    constexpr int excess = 7;
    std::vector<float> actual(static_cast<std::size_t>(available + excess), 123.0f);
    float* actual_output[] = {actual.data()};
    REQUIRE(partial.read_stretched(actual_output, available + excess) == available);
    REQUIRE(std::equal(expected.begin(), expected.end(), actual.begin()));
    REQUIRE(std::all_of(actual.begin() + available, actual.end(),
                        [](float value) { return value == 0.0f; }));
    REQUIRE(partial.available_stretched() == 0);
    REQUIRE(partial.finalize() == PitchTimeStreamFinalizeStatus::complete);
}

TEST_CASE("RealtimePitchTimeProcessor finalize backpressure preserves buffered state",
          "[signal][pitch-time][streaming]") {
    auto config = stream_config(1024);
    config.max_time_ratio = 2.0f;
    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    processor.set_time_ratio(2.0f);

    std::vector<float> input(1024, 0.25f);
    const float* source[] = {input.data()};
    PitchTimeStreamFeedStatus feed_status = PitchTimeStreamFeedStatus::accepted;
    for (int guard = 0; guard < 1000; ++guard) {
        feed_status = processor.feed(source, 1024);
        if (feed_status == PitchTimeStreamFeedStatus::backpressure) break;
        REQUIRE(feed_status == PitchTimeStreamFeedStatus::accepted);
    }
    REQUIRE(feed_status == PitchTimeStreamFeedStatus::backpressure);

    const int available = processor.available_stretched();
    const int free_space = processor.output_free_space();
    REQUIRE(available > 0);
    REQUIRE(processor.finalize() == PitchTimeStreamFinalizeStatus::backpressure);
    REQUIRE(processor.available_stretched() == available);
    REQUIRE(processor.output_free_space() == free_space);
    REQUIRE(processor.feed(source, 1) == PitchTimeStreamFeedStatus::input_closed);
    REQUIRE(processor.finalize() == PitchTimeStreamFinalizeStatus::backpressure);
    REQUIRE(processor.available_stretched() == available);
    REQUIRE(processor.output_free_space() == free_space);

    std::vector<float> output_storage(1024);
    float* output[] = {output_storage.data()};
    PitchTimeStreamFinalizeStatus finalize_status = PitchTimeStreamFinalizeStatus::backpressure;
    for (int guard = 0; guard < 1000; ++guard) {
        const int count = std::min(processor.available_stretched(), 1024);
        if (count > 0) REQUIRE(processor.read_stretched(output, count) == count);
        finalize_status = processor.finalize();
        if (finalize_status == PitchTimeStreamFinalizeStatus::complete) break;
        REQUIRE((finalize_status == PitchTimeStreamFinalizeStatus::draining
                 || finalize_status == PitchTimeStreamFinalizeStatus::backpressure));
    }
    REQUIRE(finalize_status == PitchTimeStreamFinalizeStatus::complete);
    REQUIRE(processor.available_stretched() == 0);
}
