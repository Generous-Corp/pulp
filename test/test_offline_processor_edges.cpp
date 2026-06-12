#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/offline_processor.hpp>

#include <cstdint>
#include <utility>
#include <vector>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

AudioFileData make_stereo_fixture() {
    AudioFileData input;
    input.sample_rate = 48000;
    input.channels = {
        {0.1f, 0.2f, 0.3f, 0.4f, 0.5f},
        {-1.0f, -2.0f, -3.0f, -4.0f, -5.0f},
    };
    return input;
}

}  // namespace

TEST_CASE("offline_process rejects invalid inputs", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();

    REQUIRE_FALSE(offline_process(AudioFileData{}, [](const float*, float*, int, int, double) {}).has_value());
    REQUIRE_FALSE(offline_process(input, {}, 2).has_value());
    REQUIRE_FALSE(offline_process(input, [](const float*, float*, int, int, double) {}, 0).has_value());
    REQUIRE_FALSE(offline_process(input, [](const float*, float*, int, int, double) {}, -4).has_value());
}

TEST_CASE("offline_process rejects ragged channel data before invoking callback",
          "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 48000;
    input.channels = {
        {0.1f, 0.2f, 0.3f},
        {1.0f},
    };

    bool callback_called = false;
    auto output = offline_process(
        input,
        [&](const float*, float*, int, int, double) {
            callback_called = true;
        },
        2);

    REQUIRE_FALSE(output.has_value());
    REQUIRE_FALSE(callback_called);
}

TEST_CASE("offline_process rejects empty trailing channels",
          "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 48000;
    input.channels = {
        {0.1f, 0.2f},
        {},
    };

    REQUIRE_FALSE(offline_process(
        input,
        [](const float*, float*, int, int, double) {
            FAIL("callback must not run for malformed input");
        },
        2).has_value());
}

TEST_CASE("offline_process preserves mono output shape", "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 44100;
    input.channels = {{0.25f, 0.5f, 0.75f}};

    auto output = offline_process(
        input,
        [](const float*, float* out, int channels, int frames, double sample_rate) {
            REQUIRE(channels == 1);
            REQUIRE(sample_rate == 44100.0);
            for (int frame = 0; frame < frames; ++frame)
                out[frame] = static_cast<float>(frame + 1);
        },
        8);

    REQUIRE(output.has_value());
    REQUIRE(output->sample_rate == 44100);
    REQUIRE(output->num_channels() == 1);
    REQUIRE(output->num_frames() == 3);
    REQUIRE(output->channels[0] == std::vector<float>{1.0f, 2.0f, 3.0f});
}

TEST_CASE("offline_process preserves stereo output shape", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();

    auto output = offline_process(
        input,
        [](const float*, float* out, int channels, int frames, double) {
            REQUIRE(channels == 2);
            for (int frame = 0; frame < frames; ++frame) {
                out[frame * channels] = 10.0f + static_cast<float>(frame);
                out[frame * channels + 1] = 20.0f + static_cast<float>(frame);
            }
        },
        16);

    REQUIRE(output.has_value());
    REQUIRE(output->sample_rate == 48000);
    REQUIRE(output->num_channels() == 2);
    REQUIRE(output->num_frames() == 5);
}

TEST_CASE("offline_process passes full block metadata", "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 48000;
    input.channels = {
        {0.1f, 0.2f, 0.3f, 0.4f},
        {-1.0f, -2.0f, -3.0f, -4.0f},
    };
    std::vector<int> channels_seen;
    std::vector<double> rates_seen;

    auto output = offline_process(
        input,
        [&](const float* in, float* out, int channels, int frames, double sample_rate) {
            channels_seen.push_back(channels);
            rates_seen.push_back(sample_rate);
            REQUIRE(frames == 2);
            for (int sample = 0; sample < channels * frames; ++sample)
                out[sample] = in[sample];
        },
        2);

    REQUIRE(output.has_value());
    REQUIRE(channels_seen == std::vector<int>{2, 2});
    REQUIRE(rates_seen == std::vector<double>{48000.0, 48000.0});
}

TEST_CASE("offline_process reports the final tail block frame count", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();
    std::vector<int> frames_seen;

    auto output = offline_process(
        input,
        [&](const float*, float*, int, int frames, double) {
            frames_seen.push_back(frames);
        },
        3);

    REQUIRE(output.has_value());
    REQUIRE(frames_seen == std::vector<int>{3, 2});
}

TEST_CASE("offline_process interleaves input samples frame-major", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();
    std::vector<float> first_block;

    auto output = offline_process(
        input,
        [&](const float* in, float*, int channels, int frames, double) {
            if (first_block.empty())
                first_block.assign(in, in + channels * frames);
        },
        3);

    REQUIRE(output.has_value());
    REQUIRE(first_block == std::vector<float>{0.1f, -1.0f, 0.2f, -2.0f, 0.3f, -3.0f});
}

TEST_CASE("offline_process deinterleaves callback output by channel", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();

    auto output = offline_process(
        input,
        [](const float*, float* out, int channels, int frames, double) {
            for (int frame = 0; frame < frames; ++frame) {
                out[frame * channels] = 100.0f + static_cast<float>(frame);
                out[frame * channels + 1] = 200.0f + static_cast<float>(frame);
            }
        },
        5);

    REQUIRE(output.has_value());
    REQUIRE(output->channels[0] == std::vector<float>{100.0f, 101.0f, 102.0f, 103.0f, 104.0f});
    REQUIRE(output->channels[1] == std::vector<float>{200.0f, 201.0f, 202.0f, 203.0f, 204.0f});
}

TEST_CASE("offline_process clears unwritten output samples in every block", "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 32000;
    input.channels = {{1.0f, 2.0f, 3.0f, 4.0f}};

    auto output = offline_process(
        input,
        [](const float*, float* out, int, int, double) {
            out[0] = 9.0f;
        },
        2);

    REQUIRE(output.has_value());
    REQUIRE(output->channels[0] == std::vector<float>{9.0f, 0.0f, 9.0f, 0.0f});
}

TEST_CASE("offline_process clears padded tail input samples", "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 32000;
    input.channels = {{1.0f, 2.0f, 3.0f, 4.0f, 5.0f}};

    std::vector<float> observed_padding;
    auto output = offline_process(
        input,
        [&](const float* in, float*, int, int frames, double) {
            if (frames == 1)
                observed_padding.assign(in + frames, in + 4);
        },
        4);

    REQUIRE(output.has_value());
    REQUIRE(observed_padding == std::vector<float>{0.0f, 0.0f, 0.0f});
}

TEST_CASE("offline_process supports block size larger than the input", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();
    int calls = 0;
    int frames_seen = 0;

    auto output = offline_process(
        input,
        [&](const float* in, float* out, int channels, int frames, double) {
            ++calls;
            frames_seen = frames;
            for (int sample = 0; sample < channels * frames; ++sample)
                out[sample] = in[sample] * 2.0f;
        },
        64);

    REQUIRE(output.has_value());
    REQUIRE(calls == 1);
    REQUIRE(frames_seen == 5);
    REQUIRE_THAT(output->channels[0][4], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(output->channels[1][4], WithinAbs(-10.0f, 0.0001f));
}

TEST_CASE("offline_process supports block size exactly equal to the input", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();
    int calls = 0;

    auto output = offline_process(
        input,
        [&](const float* in, float* out, int channels, int frames, double) {
            ++calls;
            REQUIRE(frames == 5);
            for (int sample = 0; sample < channels * frames; ++sample)
                out[sample] = in[sample];
        },
        5);

    REQUIRE(output.has_value());
    REQUIRE(calls == 1);
    REQUIRE(output->channels == input.channels);
}

TEST_CASE("offline_render reports deterministic block schedule metadata",
          "[audio][offline][advanced][phase3]") {
    auto input = make_stereo_fixture();
    OfflineRenderOptions options;
    options.block_size_schedule = {2, 1};
    options.start_sample_position = 100;
    options.deterministic_seed = 0x1234;

    std::vector<int> frames_seen;
    std::vector<int> scheduled_seen;
    std::vector<uint64_t> positions_seen;
    std::vector<uint64_t> block_indices_seen;
    std::vector<uint64_t> seeds_seen;

    auto output = offline_render(
        input,
        [&](const float* in, float* out, int channels,
            const OfflineRenderBlockContext& context) {
            frames_seen.push_back(context.frames);
            scheduled_seen.push_back(context.scheduled_block_size);
            positions_seen.push_back(context.sample_position);
            block_indices_seen.push_back(context.block_index);
            seeds_seen.push_back(context.deterministic_seed);
            for (int sample = 0; sample < channels * context.frames; ++sample)
                out[sample] = in[sample];
        },
        options);

    REQUIRE(output.has_value());
    REQUIRE(output->channels == input.channels);
    REQUIRE(frames_seen == std::vector<int>{2, 1, 1, 1});
    REQUIRE(scheduled_seen == std::vector<int>{2, 1, 1, 1});
    REQUIRE(positions_seen == std::vector<uint64_t>{100, 102, 103, 104});
    REQUIRE(block_indices_seen == std::vector<uint64_t>{0, 1, 2, 3});
    REQUIRE(seeds_seen == std::vector<uint64_t>{0x1234, 0x1234, 0x1234, 0x1234});
}

TEST_CASE("offline_render output can be deterministic across block schedules",
          "[audio][offline][advanced][phase3]") {
    auto input = make_stereo_fixture();

    auto render_absolute_sample_index =
        [](const AudioFileData& source, std::vector<int> schedule) {
            OfflineRenderOptions options;
            options.block_size_schedule = std::move(schedule);
            options.start_sample_position = 7;
            return offline_render(
                source,
                [](const float*, float* out, int channels,
                   const OfflineRenderBlockContext& context) {
                    for (int frame = 0; frame < context.frames; ++frame) {
                        const float value = static_cast<float>(
                            context.sample_position + static_cast<uint64_t>(frame));
                        for (int channel = 0; channel < channels; ++channel) {
                            out[frame * channels + channel] = value;
                        }
                    }
                },
                options);
        };

    auto one_block = render_absolute_sample_index(input, {5});
    auto varied_blocks = render_absolute_sample_index(input, {2, 2, 1});

    REQUIRE(one_block.has_value());
    REQUIRE(varied_blocks.has_value());
    REQUIRE(one_block->channels == varied_blocks->channels);
    REQUIRE(one_block->channels[0] == std::vector<float>{7.0f, 8.0f, 9.0f, 10.0f, 11.0f});
    REQUIRE(one_block->channels[1] == one_block->channels[0]);
}

TEST_CASE("offline_render rejects invalid block schedules",
          "[audio][offline][advanced][phase3]") {
    auto input = make_stereo_fixture();
    OfflineRenderOptions options;
    options.fallback_block_size = 0;
    REQUIRE_FALSE(offline_render(
        input,
        [](const float*, float*, int, const OfflineRenderBlockContext&) {},
        options).has_value());

    options.fallback_block_size = 4;
    options.block_size_schedule = {2, 0, 1};
    REQUIRE_FALSE(offline_render(
        input,
        [](const float*, float*, int, const OfflineRenderBlockContext&) {},
        options).has_value());
}

TEST_CASE("apply_gain preserves an empty audio container", "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 96000;

    auto output = apply_gain(input, 4.0f);

    REQUIRE(output.sample_rate == 96000);
    REQUIRE(output.channels.empty());
    REQUIRE(output.empty());
}

TEST_CASE("apply_gain scales multiple channels independently", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();

    auto output = apply_gain(input, 0.25f);

    REQUIRE(output.sample_rate == 48000);
    REQUIRE(output.num_channels() == 2);
    REQUIRE_THAT(output.channels[0][1], WithinAbs(0.05f, 0.0001f));
    REQUIRE_THAT(output.channels[1][1], WithinAbs(-0.5f, 0.0001f));
}

TEST_CASE("apply_gain supports zero gain", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();

    auto output = apply_gain(input, 0.0f);

    REQUIRE(output.num_frames() == input.num_frames());
    REQUIRE(output.channels[0] == std::vector<float>{0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    REQUIRE(output.channels[1] == std::vector<float>{-0.0f, -0.0f, -0.0f, -0.0f, -0.0f});
}

TEST_CASE("apply_gain supports negative gain", "[audio][offline][processor-edges]") {
    auto input = make_stereo_fixture();

    auto output = apply_gain(input, -2.0f);

    REQUIRE_THAT(output.channels[0][2], WithinAbs(-0.6f, 0.0001f));
    REQUIRE_THAT(output.channels[1][2], WithinAbs(6.0f, 0.0001f));
}

TEST_CASE("apply_gain preserves ragged channel lengths",
          "[audio][offline][processor-edges]") {
    AudioFileData input;
    input.sample_rate = 48000;
    input.channels = {
        {1.0f, -0.5f, 0.25f},
        {0.5f},
    };

    auto output = apply_gain(input, 2.0f);

    REQUIRE(output.sample_rate == 48000);
    REQUIRE(output.channels[0] == std::vector<float>{2.0f, -1.0f, 0.5f});
    REQUIRE(output.channels[1] == std::vector<float>{1.0f});
}
