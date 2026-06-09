#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::audio::AudioStreamHandoff;
using pulp::audio::AudioStreamHandoffConfig;
using pulp::audio::AudioStreamHandoffPullResult;
using pulp::audio::AudioStreamHandoffStats;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::PlanarAudioRingBuffer;

namespace {

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

void fill_ramp(Buffer<float>& buffer) {
    for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
        buffer.channel(0)[i] = static_cast<float>(i) / 128.0f;
    }
}

void fill_sequence(Buffer<float>& buffer, float first) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
            buffer.channel(ch)[i] = first + static_cast<float>(i) +
                                    static_cast<float>(ch) * 1000.0f;
        }
    }
}

void prepare_handoff(AudioStreamHandoff& handoff) {
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 96000.0, 4096, 2048, 256}));
    REQUIRE(handoff.resampling());
}

}  // namespace

TEST_CASE("AudioStreamHandoff split pulls preserve resampled stream state",
          "[audio][sampler][handoff]") {
    Buffer<float> source(1, 1024);
    fill_ramp(source);
    std::vector<const float*> source_ptrs;
    const auto input = const_view(source, source_ptrs);

    AudioStreamHandoff one_shot;
    prepare_handoff(one_shot);
    REQUIRE(one_shot.push(input, source.num_samples()) == source.num_samples());
    Buffer<float> expected(1, 256);
    const auto one_pull = one_shot.pull(expected.view(), expected.num_samples());
    REQUIRE_FALSE(one_pull.underrun);
    REQUIRE(one_pull.rendered_frames == expected.num_samples());
    REQUIRE(one_pull.source_backed_frames == expected.num_samples());
    REQUIRE(one_pull.source_frames_consumed > 0);
    REQUIRE(one_pull.source_frames_consumed < source.num_samples());
    const auto one_stats = one_shot.stats();
    REQUIRE(one_stats.estimated_source_latency_frames > 0.0);
    REQUIRE_THAT(one_stats.estimated_host_latency_frames,
                 WithinAbs(one_stats.estimated_source_latency_frames * 2.0, 1.0e-12));
    REQUIRE(one_stats.estimated_latency_seconds > 0.0);

    AudioStreamHandoff split;
    prepare_handoff(split);
    REQUIRE(split.push(input, source.num_samples()) == source.num_samples());
    Buffer<float> first(1, 64);
    Buffer<float> second(1, 192);
    AudioStreamHandoffPullResult first_pull;
    AudioStreamHandoffPullResult second_pull;
    AudioStreamHandoffStats after_first;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        first_pull = split.pull(first.view(), first.num_samples());
        after_first = split.stats();
        second_pull = split.pull(second.view(), second.num_samples());
    }
    REQUIRE_FALSE(first_pull.underrun);
    REQUIRE(first_pull.rendered_frames == first.num_samples());
    REQUIRE(first_pull.source_backed_frames == first.num_samples());
    REQUIRE(first_pull.source_frames_consumed > 0);
    REQUIRE(after_first.source_frames_consumed > 0);
    REQUIRE(after_first.source_frames_consumed < source.num_samples());
    REQUIRE(after_first.queued_source_frames > 0);
    REQUIRE(after_first.queued_source_seconds > 0.0);
    REQUIRE_FALSE(second_pull.underrun);
    REQUIRE(second_pull.rendered_frames == second.num_samples());

    for (std::size_t i = 0; i < first.num_samples(); ++i) {
        REQUIRE_THAT(first.channel(0)[i], WithinAbs(expected.channel(0)[i], 1.0e-6f));
    }
    for (std::size_t i = 0; i < second.num_samples(); ++i) {
        REQUIRE_THAT(second.channel(0)[i],
                     WithinAbs(expected.channel(0)[first.num_samples() + i], 1.0e-6f));
    }
}

TEST_CASE("AudioStreamHandoff reports queued source seconds for equal-rate handoff",
          "[audio][sampler][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 48000.0, 1024, 512, 256}));

    Buffer<float> source(1, 480);
    fill_ramp(source);
    std::vector<const float*> source_ptrs;
    REQUIRE(handoff.push(const_view(source, source_ptrs), source.num_samples()) ==
            source.num_samples());

    const auto stats = handoff.stats();
    REQUIRE(stats.queued_source_frames == source.num_samples());
    REQUIRE_THAT(stats.queued_source_seconds, WithinAbs(0.01, 1.0e-12));
    REQUIRE(stats.estimated_source_latency_frames == 0.0);
    REQUIRE(stats.estimated_host_latency_frames == 0.0);
    REQUIRE(stats.estimated_latency_seconds == 0.0);
}

TEST_CASE("AudioStreamHandoff prepares and pulls across common source and host rates",
          "[audio][sampler][handoff]") {
    struct Case {
        double source_rate;
        double host_rate;
    };
    const Case cases[] = {
        {44100.0, 44100.0},
        {48000.0, 48000.0},
        {88200.0, 88200.0},
        {96000.0, 96000.0},
        {176400.0, 176400.0},
        {192000.0, 192000.0},
        {44100.0, 48000.0},
        {88200.0, 48000.0},
        {96000.0, 48000.0},
        {176400.0, 48000.0},
        {192000.0, 48000.0},
        {48000.0, 44100.0},
        {48000.0, 88200.0},
        {48000.0, 96000.0},
        {48000.0, 176400.0},
        {48000.0, 192000.0},
    };

    for (const auto& c : cases) {
        AudioStreamHandoff handoff;
        REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
            1, 1, c.source_rate, c.host_rate, 16384, 8192, 512}));

        Buffer<float> source(1, 8192);
        for (std::size_t i = 0; i < source.num_samples(); ++i) {
            source.channel(0)[i] = 0.75f;
        }
        std::vector<const float*> source_ptrs;
        REQUIRE(handoff.push(const_view(source, source_ptrs), source.num_samples()) ==
                source.num_samples());

        Buffer<float> out(1, 512);
        AudioStreamHandoffPullResult pulled;
        AudioStreamHandoffStats stats;
        {
            pulp::runtime::ScopedNoAlloc no_alloc;
            pulled = handoff.pull(out.view(), out.num_samples());
            stats = handoff.stats();
        }
        INFO("source_rate=" << c.source_rate << " host_rate=" << c.host_rate);
        REQUIRE(pulled.rendered_frames == out.num_samples());
        REQUIRE_FALSE(pulled.underrun);
        REQUIRE(stats.source_frames_consumed > 0);

        double sum = 0.0;
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            REQUIRE(std::isfinite(out.channel(0)[i]));
            sum += out.channel(0)[i];
        }
        const auto avg = sum / static_cast<double>(out.num_samples());
        REQUIRE(avg > 0.25);
        REQUIRE(avg < 1.0);
    }
}

TEST_CASE("AudioStreamHandoff zero-channel pulls do not consume queued audio",
          "[audio][sampler][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 48000.0, 1024, 512, 256}));

    Buffer<float> source(1, 4);
    fill_ramp(source);
    std::vector<const float*> source_ptrs;
    REQUIRE(handoff.push(const_view(source, source_ptrs), source.num_samples()) ==
            source.num_samples());

    BufferView<float> no_channels(nullptr, 0, 2);
    const auto no_output = handoff.pull(no_channels, 2);
    REQUIRE(no_output.requested_frames == 0);
    REQUIRE(no_output.rendered_frames == 0);
    REQUIRE(no_output.source_frames_consumed == 0);
    REQUIRE(handoff.stats().queued_source_frames == source.num_samples());

    Buffer<float> out(1, 4);
    const auto output = handoff.pull(out.view(), out.num_samples());
    REQUIRE_FALSE(output.underrun);
    REQUIRE(output.rendered_frames == out.num_samples());
    REQUIRE(out.channel(0)[0] == source.channel(0)[0]);
    REQUIRE(out.channel(0)[3] == source.channel(0)[3]);
}

// Split from test_sampler_looper_primitives.cpp during sampler PR hardening.

TEST_CASE("AudioStreamHandoff passes equal-rate source blocks through",
          "[audio][sampler][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        2, 2, 48000.0, 48000.0, 16, 8, 8}));
    REQUIRE_FALSE(handoff.resampling());

    Buffer<float> source(2, 4);
    fill_sequence(source, 10.0f);
    std::vector<const float*> source_ptrs;
    REQUIRE(handoff.push(const_view(source, source_ptrs), 4) == 4);

    Buffer<float> out(2, 4);
    const auto pulled = handoff.pull(out.view(), 4);
    REQUIRE(pulled.rendered_frames == 4);
    REQUIRE(pulled.source_backed_frames == 4);
    REQUIRE_FALSE(pulled.underrun);
    REQUIRE(out.channel(0)[0] == 10.0f);
    REQUIRE(out.channel(0)[3] == 13.0f);
    REQUIRE(out.channel(1)[0] == 1010.0f);
    REQUIRE(out.channel(1)[3] == 1013.0f);
    REQUIRE(handoff.stats().source_frames_consumed == 4);
}

TEST_CASE("AudioStreamHandoff rejects zero-channel source pushes",
          "[audio][sampler][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 48000.0, 16, 8, 8}));

    BufferView<const float> source(nullptr, 0, 4);
    REQUIRE(handoff.push(source, 4) == 0);
    REQUIRE(handoff.stats().source_frames_pushed == 0);
    REQUIRE(handoff.stats().queued_source_frames == 0);
}

TEST_CASE("AudioStreamHandoff reports overrun and underrun deterministically",
          "[audio][sampler][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 2, 48000.0, 48000.0, 2, 4, 4}));

    Buffer<float> source(1, 4);
    fill_sequence(source, 1.0f);
    std::vector<const float*> source_ptrs;
    REQUIRE(handoff.push(const_view(source, source_ptrs), 4) == 2);
    REQUIRE(handoff.stats().dropped_source_frames == 2);
    REQUIRE(handoff.stats().overrun_frames == 2);

    Buffer<float> out(2, 4);
    const auto pulled = handoff.pull(out.view(), 4);
    REQUIRE(pulled.rendered_frames == 4);
    REQUIRE(pulled.source_backed_frames == 2);
    REQUIRE(pulled.underrun);
    REQUIRE(out.channel(0)[0] == 1.0f);
    REQUIRE(out.channel(0)[1] == 2.0f);
    REQUIRE(out.channel(0)[2] == 0.0f);
    REQUIRE(out.channel(1)[0] == 0.0f);
    REQUIRE(handoff.stats().underrun_frames == 2);
}

TEST_CASE("AudioStreamHandoff bridges common generated-audio sample rates",
          "[audio][sampler][handoff]") {
    struct Case {
        double source_rate;
        double host_rate;
        std::uint32_t pull_frames;
    };
    const Case cases[] = {
        {48000.0, 44100.0, 1024},
        {48000.0, 88200.0, 2048},
        {48000.0, 96000.0, 2048},
        {48000.0, 176400.0, 2048},
        {48000.0, 192000.0, 2048},
    };

    for (const auto& c : cases) {
        AudioStreamHandoff handoff;
        REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
            1, 1, c.source_rate, c.host_rate, 8192, 4096, c.pull_frames}));
        REQUIRE(handoff.resampling());

        Buffer<float> source(1, 4096);
        for (std::size_t i = 0; i < source.num_samples(); ++i) {
            source.channel(0)[i] = 1.0f;
        }
        std::vector<const float*> source_ptrs;
        REQUIRE(handoff.push(const_view(source, source_ptrs), source.num_samples()) ==
                source.num_samples());

        Buffer<float> out(1, c.pull_frames);
        const auto pulled = handoff.pull(out.view(), c.pull_frames);
        INFO("source_rate=" << c.source_rate << " host_rate=" << c.host_rate);
        REQUIRE(pulled.rendered_frames == c.pull_frames);
        REQUIRE_FALSE(pulled.underrun);
        REQUIRE(handoff.stats().source_frames_consumed > 0);

        double avg = 0.0;
        std::size_t count = 0;
        const auto guard = std::min<std::size_t>(256, out.num_samples() / 4);
        for (std::size_t i = guard; i + guard < out.num_samples(); ++i) {
            avg += out.channel(0)[i];
            ++count;
        }
        avg = count == 0 ? 0.0 : avg / static_cast<double>(count);
        REQUIRE(avg > 0.5);
        REQUIRE(avg < 1.1);
    }
}

TEST_CASE("AudioStreamHandoff zero-fills resampled underruns",
          "[audio][sampler][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 96000.0, 64, 16, 128}));
    REQUIRE(handoff.resampling());

    Buffer<float> source(1, 8);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = 1.0f;
    }
    std::vector<const float*> source_ptrs;
    REQUIRE(handoff.push(const_view(source, source_ptrs), source.num_samples()) ==
            source.num_samples());

    Buffer<float> out(1, 128);
    AudioStreamHandoffPullResult pulled;
    std::uint64_t underrun_frames = 0;
    {
        pulp::runtime::ScopedNoAlloc guard;
        pulled = handoff.pull(out.view(), out.num_samples());
        underrun_frames = handoff.stats().underrun_frames;
    }
    REQUIRE(pulled.rendered_frames == out.num_samples());
    REQUIRE(pulled.source_backed_frames < pulled.rendered_frames);
    REQUIRE(pulled.underrun);
    REQUIRE(out.channel(0)[out.num_samples() - 1] == 0.0f);
    REQUIRE(underrun_frames == pulled.rendered_frames - pulled.source_backed_frames);
}

TEST_CASE("Sampler storage and handoff hot paths run under no-allocation guard",
          "[audio][sampler][rt]") {
    PlanarAudioRingBuffer ring;
    REQUIRE(ring.prepare(2, 16));
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        2, 2, 48000.0, 48000.0, 16, 8, 8}));

    Buffer<float> source(2, 8);
    fill_sequence(source, 1.0f);
    Buffer<float> out(2, 8);
    std::vector<const float*> source_ptrs;
    const auto input = const_view(source, source_ptrs);

    std::uint64_t ring_written = 0;
    bool ring_read = false;
    std::uint64_t handoff_written = 0;
    AudioStreamHandoffPullResult pulled;
    {
        pulp::runtime::ScopedNoAlloc guard;
        ring_written = ring.write(input, 8);
        ring_read = ring.read(out.view(), 8);
        handoff_written = handoff.push(input, 8);
        pulled = handoff.pull(out.view(), 8);
    }
    REQUIRE(ring_written == 8);
    REQUIRE(ring_read);
    REQUIRE(handoff_written == 8);
    REQUIRE(pulled.rendered_frames == 8);
    REQUIRE_FALSE(pulled.underrun);
}

TEST_CASE("Resampled AudioStreamHandoff hot path runs under no-allocation guard",
          "[audio][sampler][rt][handoff]") {
    AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare(AudioStreamHandoffConfig{
        1, 1, 48000.0, 44100.0, 4096, 2048, 512}));
    REQUIRE(handoff.resampling());

    Buffer<float> source(1, 2048);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = 1.0f;
    }
    Buffer<float> out(1, 512);
    std::vector<const float*> source_ptrs;
    const auto input = const_view(source, source_ptrs);
    REQUIRE(handoff.push(input, source.num_samples()) == source.num_samples());

    AudioStreamHandoffPullResult pulled;
    {
        pulp::runtime::ScopedNoAlloc guard;
        pulled = handoff.pull(out.view(), out.num_samples());
    }
    REQUIRE(pulled.rendered_frames == out.num_samples());
    REQUIRE_FALSE(pulled.underrun);
}
